"""Find the first operator where DirectML disagrees with the CPU in an ONNX model.

Usage: export-venv/Scripts/python tools/dml_bisect.py MODEL.onnx INPUT.npy [--first N]
Exposes the computed tensors of the first N nodes (default: all) as outputs, runs CPU and DirectML (no graph fusions,
so tensors keep their names) and reports the first one that differs by more than 1% of its range.
"""
import os
import sys

import numpy as np
import onnx
import onnxruntime as ort

path, inp = sys.argv[1], sys.argv[2]
first = int(sys.argv[sys.argv.index("--first") + 1]) if "--first" in sys.argv else None
m = onnx.load(path)
x = np.load(inp)
feeds = {m.graph.input[0].name: x}
nodes = list(m.graph.node)[:first]
skip = {"Constant", "Shape", "Gather", "Unsqueeze", "Squeeze", "Concat", "Cast", "Reshape", "ConstantOfShape", "Range",
        "Equal", "Where", "Expand", "Slice"}
expose = [o for n in nodes if n.op_type not in skip for o in n.output]
have = {o.name for o in m.graph.output}
for nm in expose:
    if nm not in have:
        m.graph.output.append(onnx.helper.make_empty_tensor_value_info(nm))
dbg = path + ".dbg.onnx"
onnx.save(m, dbg)
so = ort.SessionOptions()
so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
so.enable_mem_pattern = False
so.log_severity_level = 3
names = [o.name for o in m.graph.output]
c = dict(zip(names, ort.InferenceSession(dbg, so, providers=["CPUExecutionProvider"]).run(names, feeds)))
sd = ort.InferenceSession(dbg, so, providers=["DmlExecutionProvider"])
assert sd.get_providers()[0] == "DmlExecutionProvider"
d = dict(zip(names, sd.run(names, feeds)))
os.remove(dbg)
for n in nodes:
    for o in n.output:
        if o not in c:
            continue
        a, b = np.asarray(c[o]), np.asarray(d[o])
        if a.size == 0 or not np.issubdtype(a.dtype, np.floating):
            continue
        rng = np.abs(a).max() + 1e-9
        err = np.abs(a - b).max() / rng if np.isfinite(b).all() else np.inf
        if err > 1e-2:
            print(f"first divergence at node {nodes.index(n)}: {n.op_type} '{n.name}' -> {o} rel err {err:.2e} shape {a.shape}")
            print("  inputs:", list(n.input))
            print("  attrs:", [(at.name, onnx.helper.get_attribute_value(at)) for at in n.attribute])
            for i in n.input:
                if i in c:
                    ia, ib = np.asarray(c[i]), np.asarray(d[i])
                    print(f"  input {i}: shape {ia.shape}, cpu range {ia.min():.3g}..{ia.max():.3g}, "
                          f"dml max diff {np.abs(ia - ib).max():.3g}")
            sys.exit(0)
print("no divergence among", len(expose), "tensors")
