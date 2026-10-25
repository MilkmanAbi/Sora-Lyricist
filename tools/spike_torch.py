"""PyTorch baseline for spike_bench.py: same networks, same chunk sizes, CPU, 4 threads."""
import sys
import time

import torch

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from export_models import DemucsCore, W2V, DEMUCS_SAMPLES, DEMUCS_FRAMES, W2V_SAMPLES  # noqa: E402

torch.set_num_threads(4)


def timeit(fn, n=3):
    with torch.inference_mode():
        fn()
        ts = []
        for _ in range(n):
            t = time.perf_counter()
            fn()
            ts.append(time.perf_counter() - t)
    return round(min(ts), 3)


from beat_this.inference import load_model  # noqa: E402
bt = load_model("final0", device="cpu").eval()
x = torch.randn(1, 1500, 128)
print("beat_this torch", timeit(lambda: bt(x)), flush=True)

import torchaudio  # noqa: E402
for name, b in [("w2v_base", "WAV2VEC2_ASR_BASE_960H"), ("w2v_large", "WAV2VEC2_ASR_LARGE_LV60K_960H")]:
    m = W2V(getattr(torchaudio.pipelines, b).get_model().eval())
    w = torch.randn(1, W2V_SAMPLES) * 0.1
    print(name, "torch", timeit(lambda: m(w)), flush=True)

from demucs.pretrained import get_model  # noqa: E402
dm = DemucsCore(get_model("htdemucs").models[0].eval())
mix, mag = torch.randn(1, 2, DEMUCS_SAMPLES) * 0.1, torch.randn(1, 4, 2048, DEMUCS_FRAMES)
print("htdemucs_core torch", timeit(lambda: dm(mix, mag)), flush=True)
