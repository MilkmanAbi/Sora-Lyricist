"""Time each exported network per chunk: PyTorch vs OpenVINO CPU vs OpenVINO iGPU (fp16), and check the outputs agree.

Usage: export-venv/Scripts/python tools/spike_bench.py <models dir> [model names...]
"""
import json
import os
import sys
import time

import numpy as np
import openvino as ov
import torch

D = sys.argv[1]
names = sys.argv[2:] or ["beat_this", "w2v_base", "w2v_large", "htdemucs_core"]
core = ov.Core()
core.set_property({"CACHE_DIR": os.path.join(D, "..", "cache", "ov_kernels")})
torch.set_num_threads(4)
rng = np.random.default_rng(0)


def inputs_for(name):
    meta = json.load(open(os.path.join(D, name + ".json")))
    if name == "beat_this":
        return [rng.standard_normal((1, 1500, 128)).astype(np.float32)]
    if name.startswith("w2v"):
        return [rng.standard_normal((1, meta["samples"])).astype(np.float32) * 0.1]
    return [rng.standard_normal((1, 2, meta["segment_samples"])).astype(np.float32) * 0.1,
            rng.standard_normal((1, 4, 2048, meta["frames"])).astype(np.float32)]


def timeit(fn, n=3):
    fn()  # warm-up
    ts = []
    for _ in range(n):
        t = time.perf_counter()
        fn()
        ts.append(time.perf_counter() - t)
    return min(ts)


for name in names:
    x = inputs_for(name)
    model = core.read_model(os.path.join(D, name + ".xml"))
    row = {}
    outs = {}
    for dev, cfg in [("CPU", {}), ("GPU", {"INFERENCE_PRECISION_HINT": "f16"})]:
        t = time.perf_counter()
        try:
            c = core.compile_model(model, "GPU" if dev.startswith("GPU") else dev, cfg)
        except Exception as e:
            row[dev] = f"fail: {str(e)[:80]}"
            continue
        comp = time.perf_counter() - t
        req = c.create_infer_request()
        row[dev] = round(timeit(lambda: req.infer(x)), 3)
        row[dev + "_compile"] = round(comp, 1)
        outs[dev] = [np.array(req.get_output_tensor(i).data) for i in range(len(c.outputs))]
    if "CPU" in outs:
        for dev in ("GPU", "GPU32"):
            if dev in outs:
                a, b = outs["CPU"][0], outs[dev][0]
                row[dev + "_maxdiff"] = float(np.abs(a - b).max())
                row[dev + "_reldiff"] = float(np.abs(a - b).max() / (np.abs(a).max() + 1e-9))
    print(name, json.dumps(row), flush=True)
