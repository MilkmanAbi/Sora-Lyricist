"""Make the fp32 wav2vec2 network (normalisation outside) + one real vocal window as .onnx/.npy for dml_bisect.py.
Usage: export-venv/Scripts/python tools/dml_make_probe.py <out dir> <vocals.wav>"""
import os
import subprocess
import sys

import numpy as np
import torch
import torchaudio

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_models import W2V_SAMPLES  # noqa: E402

out, voc = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)


class Inner(torch.nn.Module):
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
x = ((x - x.mean()) / np.sqrt(x.var() + 1e-5)).astype(np.float32)
np.save(os.path.join(out, "w2v_in.npy"), x)
m = Inner(torchaudio.pipelines.WAV2VEC2_ASR_LARGE_LV60K_960H.get_model().eval().model).eval()
with torch.no_grad():
    torch.onnx.export(m, (torch.from_numpy(x),), os.path.join(out, "w2v32.onnx"), input_names=["wave"], output_names=["logp"],
                      opset_version=17, dynamo=False)
print("ok")
