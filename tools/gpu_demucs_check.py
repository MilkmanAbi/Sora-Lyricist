"""Is Demucs usable on the iGPU? fp16 overflowed on real audio (vocal stem at -18 dB SDR). Compare GPU settings with
the CPU fp32 output on real 7.8 s chunks: plain fp16, fp16 with ACTIVATIONS_SCALE_FACTOR, fp32.

Usage: export-venv/Scripts/python tools/gpu_demucs_check.py <models dir> <song> [<song>...]
"""
import os
import sys
import time

import numpy as np
import openvino as ov

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from quantize import demucs_inputs, load, SEG  # noqa: E402

D = sys.argv[1]
core = ov.Core()
core.set_property({"CACHE_DIR": os.path.join(D, "..", "cache", "ov_kernels")})
model = core.read_model(os.path.join(D, "htdemucs_core.xml"))
chunks = []
for song in sys.argv[2:]:
    a = load(song, 44100, 2)
    a = (a - a.mean()) / (a.mean(0).std() + 1e-8)
    for o in (a.shape[1] // 3, a.shape[1] // 2):
        chunks.append(demucs_inputs(a[:, o:o + SEG]))
cpu = core.compile_model(model, "CPU").create_infer_request()
refs = []
for x in chunks:
    cpu.infer(x)
    refs.append([np.array(cpu.get_output_tensor(i).data)[0, 3] for i in (0, 1)])


def sdr(r, e):
    return 10 * np.log10((r ** 2).sum() / (((r - e) ** 2).sum() + 1e-20))


for name, cfg in [("f16", {"INFERENCE_PRECISION_HINT": "f16"}),
                  ("f16 scale 8", {"INFERENCE_PRECISION_HINT": "f16", "ACTIVATIONS_SCALE_FACTOR": 8.0}),
                  ("f16 scale 64", {"INFERENCE_PRECISION_HINT": "f16", "ACTIVATIONS_SCALE_FACTOR": 64.0}),
                  ("f32", {"INFERENCE_PRECISION_HINT": "f32"})]:
    t = time.perf_counter()
    req = core.compile_model(model, "GPU", cfg).create_infer_request()
    comp = time.perf_counter() - t
    s, ts = [], []
    for x, (rs, rw) in zip(chunks, refs):
        t = time.perf_counter()
        req.infer(x)
        ts.append(time.perf_counter() - t)
        s.append((sdr(rs, np.array(req.get_output_tensor(0).data)[0, 3]), sdr(rw, np.array(req.get_output_tensor(1).data)[0, 3])))
    print(f"GPU {name:13} compile {comp:5.1f}s  {min(ts):.2f}s/chunk  SDR spec/wave vs CPU: "
          + " ".join(f"{a:.0f}/{b:.0f}" for a, b in s), flush=True)
