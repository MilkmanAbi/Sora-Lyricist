"""Line timing for songs without hand-timed words (Japanese, Russian, Hindi ...): align from the plain text (no
timestamps given) and compare each line's first word with the line's timestamp in a synced .lrc made by people
(LRCLIB). Community stamps run ~0.2-0.5 s early and vary, so this measures "is the line in the right place", not
millisecond accuracy.

Usage: python tools/bench_lines.py <work dir> --song AUDIO=LRC [...] --system NAME=MTN_EXE [...]
"""
import argparse
import json
import os
import re
import subprocess

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--song", action="append", required=True)
    ap.add_argument("--system", action="append", required=True)
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    path = os.path.join(args.work, "results_lines.json")
    res = json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {}
    for spec in args.song:
        audio, lrc = spec.split("=", 1)
        name = os.path.splitext(os.path.basename(audio))[0]
        stamps, lines = [], []
        for ln in open(lrc, encoding="utf-8", errors="replace"):
            m = re.match(r"\[(\d+):(\d+(?:\.\d+)?)\](.*)", ln.strip())
            if m and m.group(3).strip():
                stamps.append(int(m.group(1)) * 60 + float(m.group(2)))
                lines.append(m.group(3).strip())
        txt = os.path.join(args.work, name + ".txt")
        open(txt, "w", encoding="utf-8").write("\n".join(lines) + "\n")
        r = res.setdefault(name, {"stamps": stamps})
        for s in args.system:
            label, exe = s.split("=", 1)
            if label in r:
                continue
            out = os.path.join(args.work, f"{name}.{label}.song.json")
            p = subprocess.run([exe, "analyze", audio, "--lyrics", txt, "--out", out], capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            if p.returncode:
                raise SystemExit(f"{label} failed on {name}:\n{p.stderr[-1500:]}")
            song = json.load(open(out, encoding="utf-8"))
            starts = {}
            for w in song["words"]:
                if w.get("line_start") and "line" in w:
                    starts.setdefault(w["line"], w["start"])
            r[label] = {"starts": [starts.get(i) for i in range(len(stamps))], "fit": song["engine"]["lyrics_fit"]}
            json.dump(res, open(path, "w", encoding="utf-8"))
        print(name, "done", flush=True)
    labels = [s.split("=", 1)[0] for s in args.system]
    print(f"\n{'song':24} {'system':8} {'lines':>5} {'median':>7} {'<0.5s':>6} {'<1s':>6} {'>3s':>5} {'fit':>5}")
    for name, r in res.items():
        for s in labels:
            if s not in r:
                continue
            e = np.array([abs(a - b) if a is not None else 30.0 for a, b in zip(r[s]["starts"], r["stamps"])])
            print(f"{name[:24]:24} {s:8} {len(e):5} {np.median(e):6.2f}s {(e < .5).mean():6.0%} {(e < 1).mean():6.0%} "
                  f"{(e > 3).mean():5.0%} {r[s]['fit']:5.2f}")


if __name__ == "__main__":
    main()
