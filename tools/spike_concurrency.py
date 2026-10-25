"""Spike 2: can CPU and iGPU work at the same time (shared ~15 W budget), and does fp16 overflow have a fix?

  a) w2v_large on the GPU in fp16 with ACTIVATIONS_SCALE_FACTOR (avoids inf) - accuracy vs CPU
  b) CPU with 8 threads (hyper-threads) vs the default 4
  c) htdemucs_core on the GPU and w2v_large on the CPU at the same time vs each alone
"""
import json
import os
import sys
import threading
import time

import numpy as np
import openvino as ov

D = sys.argv[1]
core = ov.Core()
core.set_property({"CACHE_DIR": os.path.join(D, "..", "cache", "ov_kernels")})
rng = np.random.default_rng(0)
w2v_in = [rng.standard_normal((1, 24 * 16000)).astype(np.float32) * 0.1]
dm_in = [rng.standard_normal((1, 2, 343980)).astype(np.float32) * 0.1, rng.standard_normal((1, 4, 2048, 336)).astype(np.float32)]


def bench(req, x, n=3):
    req.infer(x)
    ts = []
    for _ in range(n):
        t = time.perf_counter()
        req.infer(x)
        ts.append(time.perf_counter() - t)
    return min(ts)


w2v = core.read_model(os.path.join(D, "w2v_large.xml"))
dm = core.read_model(os.path.join(D, "htdemucs_core.xml"))
cpu_w2v = core.compile_model(w2v, "CPU")
r_cpu = cpu_w2v.create_infer_request()
r_cpu.infer(w2v_in)
ref = np.array(r_cpu.get_output_tensor(0).data)

if "a" in sys.argv[2:] or len(sys.argv) == 2:
    for sf in (8.0, 32.0):
        c = core.compile_model(w2v, "GPU", {"INFERENCE_PRECISION_HINT": "f16", "ACTIVATIONS_SCALE_FACTOR": sf})
        r = c.create_infer_request()
        t = bench(r, w2v_in, 2)
        out = np.array(r.get_output_tensor(0).data)
        same = (out.argmax(-1) == ref.argmax(-1)).mean()
        print(f"a) w2v_large GPU f16 scale {sf}: {t:.2f}s, maxdiff {np.abs(out - ref).max():.3f}, argmax agree {same:.1%}", flush=True)

if "b" in sys.argv[2:] or len(sys.argv) == 2:
    for th in (4, 8):
        c = core.compile_model(w2v, "CPU", {"INFERENCE_NUM_THREADS": th})
        print(f"b) w2v_large CPU {th} threads: {bench(c.create_infer_request(), w2v_in, 2):.2f}s", flush=True)

if "c" in sys.argv[2:] or len(sys.argv) == 2:
    gpu_dm = core.compile_model(dm, "GPU", {"INFERENCE_PRECISION_HINT": "f16"})
    r_gpu = gpu_dm.create_infer_request()
    solo_gpu = bench(r_gpu, dm_in, 2)
    solo_cpu = bench(r_cpu, w2v_in, 2)
    res = {}

    def loop(name, req, x, n):
        req.infer(x)
        t = time.perf_counter()
        for _ in range(n):
            req.infer(x)
        res[name] = (time.perf_counter() - t) / n

    a = threading.Thread(target=loop, args=("gpu", r_gpu, dm_in, 4))
    b = threading.Thread(target=loop, args=("cpu", r_cpu, w2v_in, 2))
    t = time.perf_counter()
    a.start(); b.start(); a.join(); b.join()
    wall = time.perf_counter() - t
    print(f"c) alone: demucs GPU {solo_gpu:.2f}s/chunk, w2v CPU {solo_cpu:.2f}s/window", flush=True)
    print(f"   together: demucs GPU {res['gpu']:.2f}s/chunk, w2v CPU {res['cpu']:.2f}s/window (wall {wall:.1f}s; "
          f"sequential would be {4 * solo_gpu + 2 * solo_cpu:.1f}s)", flush=True)
