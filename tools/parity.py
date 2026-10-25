"""Compare a native song.json (mtn) with the Python pipeline's, stage by stage, and both with hand-timed truth.

Usage: python tools/parity.py NATIVE.song.json PYTHON.song.json [--truth words.json]

  vocals   SDR of the native vocal stem against the Python (Demucs) one; > 30 dB = the same separation
  beats    share of beats within 10 ms / median offset
  words    word-start difference native vs Python; with --truth, mean abs error of each against the truth
"""
import argparse
import json
import re

import numpy as np
import soundfile as sf


def sdr(ref, est):
    n = min(len(ref), len(est))
    ref, est = ref[:n], est[:n]
    return 10 * np.log10((ref ** 2).sum() / (((ref - est) ** 2).sum() + 1e-12))


def norm(w):
    return re.sub(r"[^a-z0-9']", "", w.lower())


def match(a, b):
    import difflib
    sa, sb = [norm(w["word"]) for w in a], [norm(w["word"]) for w in b]
    pairs = []
    for blk in difflib.SequenceMatcher(None, sa, sb, autojunk=False).get_matching_blocks():
        pairs += [(blk.a + k, blk.b + k) for k in range(blk.size)]
    return pairs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("native")
    ap.add_argument("python")
    ap.add_argument("--truth")
    args = ap.parse_args()
    N, P = (json.load(open(p, encoding="utf-8")) for p in (args.native, args.python))

    vn, _ = sf.read(N["stems"]["vocals"], dtype="float32")
    vp, _ = sf.read(P["stems"]["vocals"], dtype="float32")
    print(f"vocals   SDR native vs python: {sdr(vp, vn):5.1f} dB")

    bn, bp = np.array(N["beats"]), np.array(P["beats"])
    if len(bn) and len(bp):
        d = np.array([np.min(np.abs(bn - t)) for t in bp])
        print(f"beats    {len(bn)} vs {len(bp)}; within 10 ms: {(d <= 0.0101).mean():.0%}; median {np.median(d) * 1000:.1f} ms;"
              f" bpm {N['bpm']} vs {P['bpm']}; downbeats {len(N['downbeats'])} vs {len(P['downbeats'])}")
    on, op = np.array(N["vocal_onsets"]), np.array(P["vocal_onsets"])
    if len(on) and len(op):
        d = np.array([np.min(np.abs(on - t)) for t in op])
        print(f"onsets   {len(on)} vs {len(op)}; within 10 ms: {(d <= 0.0101).mean():.0%}")

    pairs = match(N["words"], P["words"])
    d = np.array([N["words"][i]["start"] - P["words"][j]["start"] for i, j in pairs])
    print(f"words    {len(N['words'])} vs {len(P['words'])}; native - python start: median {np.median(d) * 1000:+.0f} ms,"
          f" |d| mean {np.abs(d).mean() * 1000:.0f} ms, within 20 ms {(np.abs(d) <= 0.02).mean():.0%}")
    if args.truth:
        truth = json.load(open(args.truth, encoding="utf-8"))
        for name, S in (("native", N), ("python", P)):
            pr = match(S["words"], truth)
            e = np.array([S["words"][i]["start"] - truth[j]["start"] for i, j in pr])
            miss = len(truth) - len(pr)
            ae = np.concatenate([np.abs(e), np.full(miss, 5.0)])
            print(f"truth    {name:6}: mean abs err {ae.mean() * 1000:4.0f} ms, median {np.median(ae) * 1000:3.0f} ms,"
                  f" <100 ms {(ae < 0.1).mean():.0%}, >300 ms {(ae > 0.3).mean():.0%}, bias {np.median(e) * 1000:+.0f} ms")


if __name__ == "__main__":
    main()
