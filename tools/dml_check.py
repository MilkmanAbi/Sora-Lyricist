"""Why does Demucs give wrong numbers on DirectML? Compare DML with the CPU on a real music chunk:
  1. with ONNX Runtime's graph optimisations on / off (a bad fusion vs a bad operator)
  2. if still wrong: every intermediate tensor, to find the first operator where DML diverges

Usage: export-venv/Scripts/python tools/dml_check.py <models dir> <song>
"""
import os
import sys

import numpy as np
import onnx
import onnxruntime as ort

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from quantize import demucs_inputs, load, SEG  # noqa: E402

D, song = sys.argv[1], sys.argv[2]
path = os.path.join(D, "htdemucs_core.onnx")
a = load(song, 44100, 2)
a = (a - a.mean()) / (a.mean(0).std() + 1e-8)
x = demucs_inputs(a[:, a.shape[1] // 2:a.shape[1] // 2 + SEG])
feeds = {"mix": x[0], "spec": x[1]}


def rel(p, q):
    fin = np.isfinite(p) & np.isfinite(q)
    return float(np.abs(p - q)[fin].max() / (np.abs(p[fin]).max() + 1e-9)), int((~np.isfinite(q)).sum())


cpu = ort.InferenceSession(path, providers=["CPUExecutionProvider"]).run(None, feeds)
for level, name in [(ort.GraphOptimizationLevel.ORT_ENABLE_ALL, "all"), (ort.GraphOptimizationLevel.ORT_DISABLE_ALL, "none")]:
    so = ort.SessionOptions()
    so.graph_optimization_level = level
    so.enable_mem_pattern = False
    dml = ort.InferenceSession(path, so, providers=["DmlExecutionProvider"]).run(None, feeds)
    print(f"optimisations {name}: " + ", ".join(f"out{i} rel {rel(c, d)[0]:.2e} nonfinite {rel(c, d)[1]}" for i, (c, d) in enumerate(zip(cpu, dml))), flush=True)

if "--bisect" in sys.argv:
    m = onnx.load(path)
    nodes = list(m.graph.node)
    # computed tensors only: making a Constant's output a graph output makes DirectML reject the graph (and ORT
    # silently falls back to the CPU, which then "matches")
    names = [o for n in nodes if n.op_type not in ("Constant", "Shape", "Gather", "Unsqueeze", "Concat", "Cast")
             for o in n.output]
    # expose every intermediate as an output (shape-less value_info is fine for ORT)
    have = {o.name for o in m.graph.output}
    for nm in names:
        if nm not in have:
            m.graph.output.append(onnx.helper.make_empty_tensor_value_info(nm))
    p2 = os.path.join(D, "_dbg.onnx")
    onnx.save(m, p2)
    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    so.enable_mem_pattern = False
    outs = [o.name for o in onnx.load(p2).graph.output]
    c = ort.InferenceSession(p2, so, providers=["CPUExecutionProvider"]).run(outs, feeds)
    sd = ort.InferenceSession(p2, so, providers=["DmlExecutionProvider"])
    assert "DmlExecutionProvider" in sd.get_providers(), "DirectML refused the debug graph"
    d = sd.run(outs, feeds)
    byname = dict(zip(outs, zip(c, d)))
    for n in nodes:
        for o in n.output:
            if o not in byname:
                continue
            cc, dd = byname[o]
            if not np.issubdtype(np.asarray(cc).dtype, np.floating) or np.asarray(cc).size == 0:
                continue
            r, nf = rel(np.asarray(cc), np.asarray(dd))
            if r > 1e-2 or nf:
                print(f"first divergence: {n.op_type} '{n.name}' -> {o}: rel {r:.2e}, nonfinite {nf}, shape {np.asarray(cc).shape}")
                print("  inputs:", list(n.input), " attrs:", [(at.name, onnx.helper.get_attribute_value(at)) for at in n.attribute])
                os.remove(p2)
                sys.exit(0)
    os.remove(p2)
    print("no divergence found per node")
