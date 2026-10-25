"""ONNX versions of the networks for discrete GPUs (AMD / NVIDIA / Intel Arc) through ONNX Runtime + DirectML.
OpenVINO only drives Intel GPUs, so a Radeon would otherwise sit idle.

Usage: export-venv/Scripts/python tools/export_onnx.py <models dir> [demucs] [beat] [w2v]

  htdemucs_core.onnx   fp32 (fp16 overflows inside Demucs on real music - see tools/gpu_demucs_check.py)
  beat_this.onnx       fp32 (small; fp16 moved its logits by 0.35)
  w2v_large_fp16.onnx  fp16 weights and maths, fp32 in/out (per-frame best letter agreed 100% with fp32 in fp16)

Each export is checked on DirectML against the CPU result on the same input.
"""
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_models import DemucsCore, W2V, DEMUCS_SAMPLES, DEMUCS_FRAMES, W2V_SAMPLES, BEAT_FRAMES  # noqa: E402

OUT = sys.argv[1]
which = sys.argv[2:] or ["demucs", "beat", "w2v"]
# nn.MultiheadAttention's fused inference path (aten::_native_multi_head_attention) has no ONNX form; the plain path
# computes the same thing with exportable ops
torch.backends.mha.set_fastpath_enabled(False)


def check(path, feeds):
    import onnxruntime as ort
    cpu = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    dml = ort.InferenceSession(path, providers=["DmlExecutionProvider"])
    a, b = cpu.run(None, feeds), dml.run(None, feeds)
    for i, (x, y) in enumerate(zip(a, b)):
        if np.issubdtype(x.dtype, np.floating):
            fin = np.isfinite(x) & np.isfinite(y)
            rel = np.abs(x - y)[fin].max() / (np.abs(x[fin]).max() + 1e-9)
            msg = f"  output {i}: max rel diff DML vs CPU {rel:.2e}"
            if x.ndim == 3 and x.shape[-1] == 29:
                msg += f", best letter agrees {(x.argmax(-1) == y.argmax(-1)).mean():.1%}"
            print(msg, flush=True)


rng = np.random.default_rng(0)
if "demucs" in which:
    from demucs.pretrained import get_model
    m = DemucsCore(get_model("htdemucs").models[0].eval())
    mix, mag = torch.randn(1, 2, DEMUCS_SAMPLES) * 0.1, torch.randn(1, 4, 2048, DEMUCS_FRAMES)
    p = os.path.join(OUT, "htdemucs_core.onnx")
    with torch.no_grad():
        torch.onnx.export(m, (mix, mag), p, input_names=["mix", "spec"], output_names=["spec_out", "wave_out"],
                          opset_version=17, dynamo=False)
    print("saved", p, flush=True)
    check(p, {"mix": mix.numpy(), "spec": mag.numpy()})

if "beat" in which:
    from beat_this.inference import load_model
    bt = load_model("final0", device="cpu").eval()

    class B(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, x):
            o = self.m(x)
            return o["beat"], o["downbeat"]

    x = torch.randn(1, BEAT_FRAMES, 128)
    p = os.path.join(OUT, "beat_this.onnx")
    with torch.no_grad():
        traced = torch.jit.trace(B(bt), x, check_trace=False)
        torch.onnx.export(traced, (x,), p, input_names=["spec"], output_names=["beat", "downbeat"], opset_version=17,
                          dynamo=False)
    print("saved", p, flush=True)
    check(p, {"spec": x.numpy()})

if "w2v" in which:
    import torchaudio
    # ONNX Runtime's own converter: onnxconverter_common's left the attention's Cast nodes with mismatched types
    from onnxruntime.transformers import float16
    import onnx
    m = W2V(torchaudio.pipelines.WAV2VEC2_ASR_LARGE_LV60K_960H.get_model().eval())
    wave = torch.randn(1, W2V_SAMPLES) * 0.1
    p32 = os.path.join(OUT, "w2v_large_fp32.onnx")
    with torch.no_grad():
        torch.onnx.export(m, (wave,), p32, input_names=["wave"], output_names=["logp"], opset_version=17, dynamo=False)
    p = os.path.join(OUT, "w2v_large_fp16.onnx")
    # fp16 inside, fp32 at the edges; layer norms and softmax stay fp32 (where fp16 overflows first)
    m16 = float16.convert_float_to_float16(onnx.load(p32), keep_io_types=True,
                                           op_block_list=["LayerNormalization", "Softmax", "LogSoftmax", "ReduceMean", "Pow", "Sqrt"],
                                           force_fp16_initializers=False)
    onnx.save(m16, p)
    os.remove(p32)
    for f in os.listdir(OUT):  # torch may have written external weight files next to the fp32 model
        if f.startswith("w2v_large_fp32"):
            os.remove(os.path.join(OUT, f))
    print("saved", p, flush=True)
    v = (rng.standard_normal((1, W2V_SAMPLES)) * 0.1).astype(np.float32)
    check(p, {"wave": v})
