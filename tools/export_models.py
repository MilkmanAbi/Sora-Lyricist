"""Export the analysis networks to OpenVINO IR (build-time only; the app itself has no Python).

Usage: export-venv/Scripts/python tools/export_models.py <models dir>

Every network gets a fixed input shape: the iGPU plugin compiles kernels per shape, so the engine pads its chunks
to these sizes instead of feeding variable lengths.

  htdemucs_core   Demucs v4 without its STFT/iSTFT (done natively): (mix, spectrogram) -> (spec out, wave out)
  w2v_large       wav2vec2 large LV-60k 960h CTC (characters), 24 s windows at 16 kHz -> log-probs
  w2v_base        wav2vec2 base 960h, same contract (faster, less accurate)
  beat_this       Beat This! final0 on 1500-frame log-mel windows (30 s at 50 fps) -> beat / downbeat logits
"""
import json
import os
import sys

import torch
import openvino as ov

OUT = sys.argv[1] if len(sys.argv) > 1 else "models"
os.makedirs(OUT, exist_ok=True)

DEMUCS_SAMPLES = 343980          # htdemucs training segment: 7.8 s at 44.1 kHz
DEMUCS_FRAMES = 336              # ceil(343980 / 1024)
W2V_SAMPLES = 24 * 16000         # 20 s kept + 2 s context each side
BEAT_FRAMES = 1500


class DemucsCore(torch.nn.Module):
    """HTDemucs.forward between _spec and _mask/_ispec (see demucs/htdemucs.py)."""

    def __init__(self, m):
        super().__init__()
        self.m = m

    def forward(self, mix, mag):
        m = self.m
        x = mag
        B, C, Fq, T = x.shape
        mean = x.mean(dim=(1, 2, 3), keepdim=True)
        std = x.std(dim=(1, 2, 3), keepdim=True)
        x = (x - mean) / (1e-5 + std)
        xt = mix
        meant = xt.mean(dim=(1, 2), keepdim=True)
        stdt = xt.std(dim=(1, 2), keepdim=True)
        xt = (xt - meant) / (1e-5 + stdt)
        saved, saved_t, lengths, lengths_t = [], [], [], []
        for idx, encode in enumerate(m.encoder):
            lengths.append(x.shape[-1])
            inject = None
            if idx < len(m.tencoder):
                lengths_t.append(xt.shape[-1])
                tenc = m.tencoder[idx]
                xt = tenc(xt)
                if not tenc.empty:
                    saved_t.append(xt)
                else:
                    inject = xt
            x = encode(x, inject)
            if idx == 0 and m.freq_emb is not None:
                frs = torch.arange(x.shape[-2], device=x.device)
                emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                x = x + m.freq_emb_scale * emb
            saved.append(x)
        if m.crosstransformer:
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = x.reshape(b, c, f * t)
                x = m.channel_upsampler(x)
                x = x.reshape(b, -1, f, t)
                xt = m.channel_upsampler_t(xt)
            x, xt = m.crosstransformer(x, xt)
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = x.reshape(b, c, f * t)
                x = m.channel_downsampler(x)
                x = x.reshape(b, -1, f, t)
                xt = m.channel_downsampler_t(xt)
        for idx, decode in enumerate(m.decoder):
            skip = saved.pop(-1)
            x, pre = decode(x, skip, lengths.pop(-1))
            offset = m.depth - len(m.tdecoder)
            if idx >= offset:
                tdec = m.tdecoder[idx - offset]
                length_t = lengths_t.pop(-1)
                if tdec.empty:
                    pre = pre[:, :, 0]
                    xt, _ = tdec(pre, None, length_t)
                else:
                    skip = saved_t.pop(-1)
                    xt, _ = tdec(xt, skip, length_t)
        S = len(m.sources)
        x = x.view(B, S, -1, Fq, T) * std[:, None] + mean[:, None]
        xt = xt.view(B, S, -1, mix.shape[-1]) * stdt[:, None] + meant[:, None]
        return x, xt


class W2V(torch.nn.Module):
    def __init__(self, m):
        super().__init__()
        self.m = m

    def forward(self, wave):
        em, _ = self.m(wave)
        return torch.log_softmax(em, dim=-1)


def save(model, name, meta, shapes):
    # static shapes: the engine always feeds full chunks, and the GPU plugin compiles per shape
    model.reshape({model.inputs[i]: ov.PartialShape(sh) for i, sh in enumerate(shapes)})
    ov.save_model(model, os.path.join(OUT, name + ".xml"), compress_to_fp16=False)
    with open(os.path.join(OUT, name + ".json"), "w") as f:
        json.dump(meta, f, indent=1)
    print("saved", name, flush=True)


def export_demucs():
    from demucs.pretrained import get_model
    bag = get_model("htdemucs")
    m = bag.models[0].eval()
    mix = torch.randn(1, 2, DEMUCS_SAMPLES)
    mag = torch.randn(1, 4, 2048, DEMUCS_FRAMES)
    with torch.no_grad():
        ovm = ov.convert_model(DemucsCore(m), example_input=(mix, mag))
    save(ovm, "htdemucs_core", {"sources": list(m.sources), "samplerate": m.samplerate, "segment_samples": DEMUCS_SAMPLES,
                                "frames": DEMUCS_FRAMES, "nfft": m.nfft, "hop": m.hop_length if hasattr(m, "hop_length") else m.nfft // 4},
         [list(mix.shape), list(mag.shape)])


def export_w2v(bundle_name, name):
    import torchaudio
    bundle = getattr(torchaudio.pipelines, bundle_name)
    m = W2V(bundle.get_model().eval())
    wave = torch.randn(1, W2V_SAMPLES)
    with torch.no_grad():
        ovm = ov.convert_model(m, example_input=(wave,))
    save(ovm, name, {"labels": list(bundle.get_labels()), "samplerate": 16000, "samples": W2V_SAMPLES}, [[1, W2V_SAMPLES]])


def export_beat_this():
    from beat_this.inference import load_model
    m = load_model("final0", device="cpu").eval()
    spec = torch.randn(1, BEAT_FRAMES, 128)

    class B(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, x):
            out = self.m(x)
            return out["beat"], out["downbeat"]

    with torch.no_grad():  # the tracer's self-check trips on the rotary cache; the outputs match PyTorch (< 1e-5)
        ovm = ov.convert_model(torch.jit.trace(B(m), spec, check_trace=False), example_input=(spec,))
    save(ovm, "beat_this", {"frames": BEAT_FRAMES, "mels": 128, "samplerate": 22050, "hop": 441, "nfft": 1024},
         [[1, BEAT_FRAMES, 128]])


if __name__ == "__main__":
    which = sys.argv[2:] or ["demucs", "w2v_large", "w2v_base", "beat"]
    if "demucs" in which:
        export_demucs()
    if "w2v_large" in which:
        export_w2v("WAV2VEC2_ASR_LARGE_LV60K_960H", "w2v_large")
    if "w2v_base" in which:
        export_w2v("WAV2VEC2_ASR_BASE_960H", "w2v_base")
    if "beat" in which:
        export_beat_this()
