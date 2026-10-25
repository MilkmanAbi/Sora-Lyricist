"""Per-window check of the DirectML aligner (w2v_large_fp16.onnx) against the CPU INT8 one on real vocals, the way the
engine windows them (24 s windows, 20 s apart, 2 s context, zeros past the ends).

Usage: export-venv/Scripts/python tools/dml_w2v_windows.py <models dir> <vocals.wav>
"""
import os
import subprocess
import sys

import numpy as np
import onnxruntime as ort
import openvino as ov

D, voc = sys.argv[1], sys.argv[2]
raw = subprocess.run(["ffmpeg", "-v", "error", "-i", voc, "-f", "f32le", "-ac", "1", "-ar", "16000", "-"], capture_output=True, check=True).stdout
v = np.frombuffer(raw, np.float32)
so = ort.SessionOptions()
so.enable_mem_pattern = False
so.log_severity_level = 3
dml = ort.InferenceSession(os.path.join(D, "w2v_large_fp16.onnx"), so, providers=["DmlExecutionProvider"])
cpu16 = ort.InferenceSession(os.path.join(D, "w2v_large_fp16.onnx"), so, providers=["CPUExecutionProvider"])
i8 = ov.Core().compile_model(os.path.join(D, "w2v_large_int8.xml"), "CPU").create_infer_request()
W, STEP, CTX = 24 * 16000, 20 * 16000, 2 * 16000
for k, a in enumerate(range(0, len(v), STEP)):
    w = np.zeros(W, np.float32)
    lo = a - CTX
    s0, s1 = max(0, lo), min(len(v), lo + W)
    w[s0 - lo:s1 - lo] = v[s0:s1]
    x = w[None]
    d = dml.run(None, {"wave": x})[0][0]
    c = cpu16.run(None, {"wave": x})[0][0]
    i8.infer([x])
    q = np.array(i8.get_output_tensor(0).data)[0]
    nonblank = q.argmax(-1) != 0
    print(f"window {k}: DML vs CPU fp16 agree {(d.argmax(-1) == c.argmax(-1)).mean():.1%}, "
          f"DML vs INT8 {(d.argmax(-1) == q.argmax(-1)).mean():.1%} (letters only {(d.argmax(-1) == q.argmax(-1))[nonblank].mean():.1%}), "
          f"CPU fp16 vs INT8 letters {(c.argmax(-1) == q.argmax(-1))[nonblank].mean():.1%}", flush=True)
