"""Real-audio concurrency check: Demucs fp32 on the iGPU while wav2vec2 INT8 runs on the CPU. Is the pair faster
than running them one after the other? (spike_concurrency.py used random inputs, which hit slow paths.)

Usage: export-venv/Scripts/python tools/spike_overlap_real.py <models dir> <song>
"""
import os
import sys
import threading
import time

import numpy as np
import openvino as ov

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from quantize import demucs_inputs, load, SEG  # noqa: E402

D, song = sys.argv[1], sys.argv[2]
core = ov.Core()
core.set_property({"CACHE_DIR": os.path.join(D, "..", "cache", "ov_kernels")})
a = load(song, 44100, 2)
a = (a - a.mean()) / (a.mean(0).std() + 1e-8)
dm_x = [demucs_inputs(a[:, o:o + SEG]) for o in range(SEG, 7 * SEG, SEG)]
v = load(song, 16000, 1)[0]
w_x = [[v[o:o + 384000][None].copy()] for o in range(0, 4 * 320000, 320000)]
gpu = core.compile_model(os.path.join(D, "htdemucs_core.xml"), "GPU", {"INFERENCE_PRECISION_HINT": "f32"}).create_infer_request()
cpu = core.compile_model(os.path.join(D, "w2v_large_int8.xml"), "CPU").create_infer_request()
gpu.infer(dm_x[0]); cpu.infer(w_x[0])  # warm-up


def run(req, xs, out, key):
    t = time.perf_counter()
    for x in xs:
        req.infer(x)
    out[key] = time.perf_counter() - t


solo = {}
run(gpu, dm_x, solo, "gpu")
run(cpu, w_x, solo, "cpu")
both = {}
t = time.perf_counter()
th = [threading.Thread(target=run, args=(gpu, dm_x, both, "gpu")), threading.Thread(target=run, args=(cpu, w_x, both, "cpu"))]
[x.start() for x in th]
[x.join() for x in th]
wall = time.perf_counter() - t
print(f"alone:    Demucs GPU fp32 {solo['gpu'] / len(dm_x):.2f}s/chunk, w2v INT8 CPU {solo['cpu'] / len(w_x):.2f}s/window,"
      f" back to back {solo['gpu'] + solo['cpu']:.1f}s")
print(f"together: Demucs GPU fp32 {both['gpu'] / len(dm_x):.2f}s/chunk, w2v INT8 CPU {both['cpu'] / len(w_x):.2f}s/window,"
      f" wall {wall:.1f}s")
