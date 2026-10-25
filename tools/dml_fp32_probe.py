"""Why does wav2vec2 come out all-blank on DirectML? Hypothesis: its first op, a layer norm over the whole 384,000-sample
waveform, is a reduction too large for DirectML. Export it with and without that normalisation (normalising in numpy
instead) and compare letters with the CPU. Temporary files, deleted afterwards.

Usage: export-venv/Scripts/python tools/dml_fp32_probe.py <models dir> <vocals.wav>
"""
import os
import subprocess
import sys

import numpy as np
import onnxruntime as ort
import torch
import torchaudio

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_models import W2V_SAMPLES  # noqa: E402

D, voc = sys.argv[1], sys.argv[2]
bundle = torchaudio.pipelines.WAV2VEC2_ASR_LARGE_LV60K_960H
outer = bundle.get_model().eval()   # normalises the waveform, then the network, then log-softmax


class Inner(torch.nn.Module):       # the network only: input must already be normalised
    def __init__(self, m):
        super().__init__()
        self.m = m

    def forward(self, wave):
        em, _ = self.m(wave)
        return torch.log_softmax(em, dim=-1)


raw = subprocess.run(["ffmpeg", "-v", "error", "-i", voc, "-f", "f32le", "-ac", "1", "-ar", "16000", "-"], capture_output=True, check=True).stdout
v = np.frombuffer(raw, np.float32)
x = np.zeros((1, W2V_SAMPLES), np.float32)
seg = v[18 * 16000:][:W2V_SAMPLES]
x[0, :len(seg)] = seg
xn = ((x - x.mean()) / np.sqrt(x.var() + 1e-5)).astype(np.float32)  # F.layer_norm over the whole waveform
so = ort.SessionOptions()
so.enable_mem_pattern = False
so.log_severity_level = 3
inner = Inner(outer.model).eval()
p = os.path.join(D, "_probe_inner.onnx")
with torch.no_grad():
    torch.onnx.export(inner, (torch.from_numpy(xn),), p, input_names=["wave"], output_names=["logp"], opset_version=17, dynamo=False)
c = ort.InferenceSession(p, so, providers=["CPUExecutionProvider"]).run(None, {"wave": xn})[0][0].argmax(-1)
d = ort.InferenceSession(p, so, providers=["DmlExecutionProvider"]).run(None, {"wave": xn})[0][0].argmax(-1)
letters = c != 0
print(f"normalised outside the network: letters agree {(d == c)[letters].mean():.1%}, "
      f"DML non-blank {(d != 0).mean():.1%} (CPU {(c != 0).mean():.1%})", flush=True)
for f in os.listdir(D):
    if f.startswith("_probe_"):
        os.remove(os.path.join(D, f))
