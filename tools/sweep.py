"""Tune the aligner's knobs on benchmark tracks that are already analysed (their separation and wav2vec2 windows are in
mtn's SSD cache, so each run is only decode + Viterbi).

Usage: python tools/sweep.py <bench work dir> [--mtn PATH] [--tracks K] [--grid "slack=0.3,0.5 weight=4,15,40"]
"""
import argparse
import itertools
import json
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--mtn", default=os.path.expandvars(r"%USERPROFILE%\music-native\build\mtn_dev.exe"))
    ap.add_argument("--grid", default="slack=0.5 weight=4,15,40,100")
    ap.add_argument("--extra", default="")
    ap.add_argument("--only", default="", help="substring filter on track keys")
    ap.add_argument("--snapshot", help="file with the track keys to use (written from the current results if missing),"
                                       " so runs stay comparable while a benchmark is still adding tracks")
    args = ap.parse_args()
    res = json.load(open(os.path.join(args.work, "results.json")))
    keys = sorted(k for k, r in res.items() if "native" in r and args.only in k)
    if args.snapshot:
        if os.path.exists(args.snapshot):
            keys = json.load(open(args.snapshot))
        else:
            json.dump(keys, open(args.snapshot, "w"))
    grid = [(kv.split("=")[0], kv.split("=")[1].split(",")) for kv in args.grid.split()]
    out_dir = os.path.join(args.work, "sweep")
    os.makedirs(out_dir, exist_ok=True)
    for combo in itertools.product(*[v for _, v in grid]):
        flags = sum([[f"--{n}", v] for (n, _), v in zip(grid, combo)], []) + args.extra.split()
        errs, miss, big = [], 0, []
        for k in keys:
            tag, cond = k.split("|")
            wav = os.path.join(args.work, tag + ".wav")
            lrc = os.path.join(args.work, f"{tag}.{cond}.lrc")
            out = os.path.join(out_dir, f"{tag}.{cond}.song.json")
            p = subprocess.run([args.mtn, "analyze", wav, "--lrc", lrc, "--out", out] + flags, capture_output=True, text=True)
            if p.returncode:
                raise SystemExit(p.stderr[-1500:])
            e, m = bench.errors(json.load(open(out, encoding="utf-8"))["words"], res[k]["truth"])
            errs.append(e)
            miss += m
            ae = np.abs(e)
            big.append(f"{tag[:12]}{'/s' if 'slowed' in tag else ''}/{cond[0]}:{ae.mean() * 1000:.0f}")
        e = np.concatenate(errs)
        ae = np.concatenate([np.abs(e), np.full(miss, 5.0)])
        print(f"{' '.join(flags):40} mean {ae.mean() * 1000:5.0f}ms  <100 {(ae < .1).mean():4.0%}  >300 {(ae > .3).mean():4.0%}"
              f"  bias {np.median(e) * 1000:+4.0f}  | {' '.join(big)}", flush=True)


if __name__ == "__main__":
    main()
