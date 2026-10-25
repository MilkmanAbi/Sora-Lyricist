"""Run mtn on songs from a music folder with two lyric versions (LRCLIB only vs Genius words on LRCLIB timing) and
compare what can be measured without hand-timed truth:
  conf      mean aligner confidence per word (wav2vec2 probability of the word's letters): text that matches the
            singing scores higher
  low       share of words with confidence < 0.2 (a word the aligner couldn't find in the audio)
  squashed  share of words lit < 120 ms (squeezed between neighbours: usually words that weren't sung there)
  line dev  median |line start - LRCLIB timestamp| (how far the audio moved the lines from their stamps)

Usage: python tools/library_test.py <work dir> <music dir> song_stem [song_stem ...]
Lyrics come from `mtn lyrics --lyrics-source local|both` into <work>/lyrics/<stem>.<source>.lrc.
"""
import json
import os
import subprocess
import sys
import time

import numpy as np

MTN = os.path.expandvars(r"%USERPROFILE%\music-native\build\mtn.exe")


def stats(song, lrc):
    w = song["words"]
    conf = np.array([x["score"] for x in w])
    dur = np.array([x["end"] - x["start"] for x in w])
    stamps = []
    for ln in open(lrc, encoding="utf-8"):
        if ln.startswith("[") and ln[1:3].isdigit():
            stamps.append(int(ln[1:3]) * 60 + float(ln[4:9]))
    starts = [x["start"] for x in w if x.get("line_start")]
    dev = [min(abs(s - t) for t in stamps) for s in starts] if stamps else [0]
    return {"words": len(w), "conf": conf.mean(), "low": (conf < 0.2).mean(), "squashed": (dur < 0.12).mean(),
            "line_dev": float(np.median(dev))}


def main():
    work, music, stems = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(os.path.join(work, "lyrics"), exist_ok=True)
    rows = []
    for stem in stems:
        audio = os.path.join(music, stem + ".mp3")
        for src in ("local", "both"):
            lrc = os.path.join(work, "lyrics", f"{stem}.{src}.lrc")
            if not os.path.exists(lrc):
                subprocess.run([MTN, "lyrics", audio, "--yes", "--lyrics-source", src, "--save-lyrics", lrc],
                               capture_output=True, check=True)
            out = os.path.join(work, "songs", f"{stem}.{src}.song.json")
            t = time.time()
            p = subprocess.run([MTN, "analyze", audio, "--lrc", lrc, "--out", out], capture_output=True, text=True)
            if p.returncode:
                print(stem, src, "FAILED:", p.stderr[-600:], flush=True)
                continue
            s = stats(json.load(open(out, encoding="utf-8")), lrc)
            s.update(song=stem, src=src, secs=time.time() - t)
            rows.append(s)
            print(f"{stem[:28]:28} {src:5} {s['words']:4} words  conf {s['conf']:.2f}  low {s['low']:4.0%}  "
                  f"squashed {s['squashed']:4.0%}  line dev {s['line_dev'] * 1000:4.0f} ms  {s['secs']:5.0f} s", flush=True)
    json.dump(rows, open(os.path.join(work, "library_test.json"), "w"), indent=1)
    for src in ("local", "both"):
        r = [x for x in rows if x["src"] == src]
        if r:
            print(f"ALL {src:5}: conf {np.mean([x['conf'] for x in r]):.3f}  low {np.mean([x['low'] for x in r]):.1%}  "
                  f"squashed {np.mean([x['squashed'] for x in r]):.1%}  line dev {np.median([x['line_dev'] for x in r]) * 1000:.0f} ms")


if __name__ == "__main__":
    main()
