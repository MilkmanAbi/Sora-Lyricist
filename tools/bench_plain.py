"""Plain-text lyrics (no line timestamps) on the benchmark tracks: mtn --lyrics vs the Python tool's --lyrics path.
Needs a finished tools/bench.py work dir (stems are in mtn's cache, so mtn only re-runs the Viterbi).

Usage (music-tool venv): python tools/bench_plain.py <work dir> [--mtn PATH] [--no-python]
"""
import argparse
import json
import os
import re
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

sys.path.insert(0, bench.TOOLS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--mtn", default=os.path.expandvars(r"%USERPROFILE%\music-native\build\mtn.exe"))
    ap.add_argument("--no-python", action="store_true")
    args = ap.parse_args()
    res = json.load(open(os.path.join(args.work, "results.json")))
    path = os.path.join(args.work, "results_plain.json")
    out = json.load(open(path)) if os.path.exists(path) else {}
    for key in sorted(k for k in res if k.endswith("|exact")):
        tag = key.split("|")[0]
        r = out.setdefault(tag, {"truth": res[key]["truth"]})
        txt = os.path.join(args.work, tag + ".txt")
        with open(os.path.join(args.work, tag + ".exact.lrc"), encoding="utf-8") as f:
            open(txt, "w", encoding="utf-8").write("".join(re.sub(r"\[[^\]]*\]", "", ln) for ln in f))
        song = os.path.join(args.work, "songs", f"{tag}.plain.native.song.json")
        if "native" not in r:
            p = subprocess.run([args.mtn, "analyze", os.path.join(args.work, tag + ".wav"), "--lyrics", txt, "--out", song],
                               capture_output=True, text=True)
            if p.returncode:
                raise SystemExit(p.stderr[-1500:])
            r["native"] = {"words": json.load(open(song, encoding="utf-8"))["words"]}
        if not args.no_python and "python" not in r:
            import analyze_song as A
            vocals = json.load(open(song, encoding="utf-8"))["stems"]["vocals"]
            text = re.sub(r"\s+", " ", re.sub(r"[()\[\]]", " ", open(txt, encoding="utf-8").read())).strip()
            try:
                r["python"] = {"words": A.align_words(vocals, text, "en")}
            except Exception as ex:
                print("python failed", tag, ex)
                r["python"] = {"words": []}
        json.dump(out, open(path, "w"))
        print(tag, "done", flush=True)
    for sp in ("normal", "slowed"):
        for s in ("python", "native"):
            es, miss = [], 0
            for tag, r in out.items():
                if ("slowed" in tag) != (sp == "slowed") or s not in r:
                    continue
                e, m = bench.errors(r[s]["words"], r["truth"])
                es.append(e)
                miss += m
            if not es:
                continue
            e = np.concatenate(es)
            ae = np.concatenate([np.abs(e), np.full(miss, 5.0)])
            print(f"plain {sp:7} {s:7} mean {ae.mean() * 1000:5.0f}ms  median {np.median(ae) * 1000:4.0f}ms  "
                  f"<100 {(ae < .1).mean():4.0%}  >300 {(ae > .3).mean():4.0%}  bias {np.median(e) * 1000:+4.0f}ms")


if __name__ == "__main__":
    main()
