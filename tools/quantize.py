"""INT8 versions of the big networks for the CPU (AVX-512 VNNI / AVX-VNNI run int8 matmuls ~2-3x faster than fp32).

Usage: export-venv/Scripts/python tools/quantize.py <models dir> --mixes DIR --vocals WAV [WAV...] [--only w2v_large,htdemucs_core]

Post-training quantization with NNCF, calibrated on real audio: random 7.8 s mix chunks for Demucs (songs from
--mixes), random 24 s windows of separated vocals for wav2vec2. Each int8 model is checked on held-out chunks
against its fp32 original (Demucs: SDR of the vocal output; wav2vec2: agreement of the per-frame best character).
"""
import argparse
import glob
import math
import os
import random
import subprocess

import numpy as np
import nncf
import openvino as ov
import torch

SEG, FRAMES = 343980, 336


def load(path, sr, ch):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", str(ch), "-ar", str(sr), "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).reshape(-1, ch).T.copy()


def demucs_inputs(mix):
    """(mix chunk, spectrogram) exactly as HTDemucs._spec + _magnitude make them."""
    x = torch.from_numpy(mix)[None]
    hl, nfft = 1024, 4096
    le = int(math.ceil(x.shape[-1] / hl))
    pad = hl // 2 * 3
    xp = torch.nn.functional.pad(x, (pad, pad + le * hl - x.shape[-1]), mode="reflect")
    z = torch.stft(xp.reshape(-1, xp.shape[-1]), nfft, hl, window=torch.hann_window(nfft), win_length=nfft,
                   normalized=True, center=True, return_complex=True, pad_mode="reflect")
    z = z.view(1, 2, *z.shape[-2:])[..., :-1, :][..., 2:2 + le]
    m = torch.view_as_real(z).permute(0, 1, 4, 2, 3).reshape(1, 4, 2048, le)
    return [x.numpy().astype(np.float32), m.numpy().astype(np.float32)]


def demucs_samples(files, n, rng):
    out = []
    for f in files:
        a = load(f, 44100, 2)
        if a.shape[1] < SEG * 2:
            continue
        a = (a - a.mean()) / (a.mean(0).std() + 1e-8)  # the normalisation demucs.separate applies
        for _ in range(max(1, n // len(files))):
            o = rng.randrange(0, a.shape[1] - SEG)
            out.append(demucs_inputs(a[:, o:o + SEG]))
    return out[:n]


def w2v_samples(wavs, n, rng):
    out = []
    for w in wavs:
        a = load(w, 16000, 1)[0]
        for _ in range(max(1, n // len(wavs))):
            o = rng.randrange(0, max(1, len(a) - 24 * 16000))
            x = np.zeros(24 * 16000, np.float32)
            seg = a[o:o + 24 * 16000]
            x[:len(seg)] = seg
            out.append([x[None]])
    return out[:n]


def sdr(ref, est):
    return 10 * np.log10((ref ** 2).sum() / (((ref - est) ** 2).sum() + 1e-12))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("models")
    ap.add_argument("--mixes", required=True)
    ap.add_argument("--vocals", nargs="+", required=True)
    ap.add_argument("--only", default="htdemucs_core,w2v_large")
    ap.add_argument("--calib", type=int, default=48)
    args = ap.parse_args()
    rng = random.Random(0)
    core = ov.Core()
    mixes = sorted(glob.glob(os.path.join(args.mixes, "*.mp3")) + glob.glob(os.path.join(args.mixes, "*.wav")))
    rng.shuffle(mixes)
    for name in args.only.split(","):
        src = os.path.join(args.models, name + ".xml")
        model = core.read_model(src)
        if name.startswith("htdemucs"):
            calib = demucs_samples(mixes[:16], args.calib, rng)
            held = demucs_samples(mixes[16:20], 4, rng)
        else:
            calib = w2v_samples(args.vocals, args.calib, rng)
            held = w2v_samples(args.vocals, 4, random.Random(1))
        q = nncf.quantize(model, nncf.Dataset(calib), model_type=nncf.ModelType.TRANSFORMER,
                          preset=nncf.QuantizationPreset.MIXED, subset_size=len(calib))
        dst = os.path.join(args.models, name + "_int8.xml")
        ov.save_model(q, dst, compress_to_fp16=False)
        import json, shutil, time
        shutil.copyfile(os.path.join(args.models, name + ".json"), os.path.join(args.models, name + "_int8.json"))
        f32 = core.compile_model(model, "CPU").create_infer_request()
        i8 = core.compile_model(dst, "CPU").create_infer_request()
        scores, times = [], []
        for x in held:
            f32.infer(x)
            t = time.perf_counter()
            i8.infer(x)
            times.append(time.perf_counter() - t)
            a, b = np.array(f32.get_output_tensor(0).data), np.array(i8.get_output_tensor(0).data)
            if name.startswith("htdemucs"):
                wa, wb = np.array(f32.get_output_tensor(1).data)[0, 3], np.array(i8.get_output_tensor(1).data)[0, 3]
                scores.append((sdr(a[0, 3], b[0, 3]), sdr(wa, wb)))
            else:
                scores.append(((a.argmax(-1) == b.argmax(-1)).mean(), np.abs(a - b).mean()))
        print(name, "int8:", dst, "| held-out:", [tuple(round(float(v), 3) for v in s) for s in scores],
              f"| {min(times[1:] or times):.2f}s per chunk", flush=True)


if __name__ == "__main__":
    main()
