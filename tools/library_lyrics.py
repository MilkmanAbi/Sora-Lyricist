"""Look up lyrics for every song in a folder with `mtn lyrics --yes` and tabulate what each source found.

Usage: python tools/library_lyrics.py <music dir> <out dir>    -> <out dir>/lyrics/*.lrc|txt, <out dir>/coverage.json
"""
import glob
import json
import os
import re
import subprocess
import sys
import time
from collections import Counter

MTN = os.path.expandvars(r"%USERPROFILE%\music-native\build\mtn.exe")


def main():
    music, out = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(out, "lyrics"), exist_ok=True)
    rows = []
    for f in sorted(glob.glob(os.path.join(music, "*.mp3"))):
        stem = os.path.splitext(os.path.basename(f))[0]
        p = subprocess.run([MTN, "lyrics", f, "--yes", "--save-lyrics", os.path.join(out, "lyrics", stem)],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        log = p.stderr

        def grab(pat):
            m = re.search(pat, log)
            return m.group(1).strip() if m else ""

        row = {"song": stem, "query": grab(r"looking up lyrics: (.*)"), "lrclib": grab(r"LRCLIB: (.*)"),
               "genius": re.sub(r" \(https.*", "", grab(r"Genius: (.*)")), "used": grab(r"lyrics \((\w+)\)") or "none"}
        rows.append(row)
        print(f"{stem[:32]:32} {row['used']:5} | {row['lrclib'][:50]:50} | {row['genius'][:50]}", flush=True)
        time.sleep(1)
    json.dump(rows, open(os.path.join(out, "coverage.json"), "w", encoding="utf-8"), indent=1)
    print(Counter(r["used"] for r in rows))


if __name__ == "__main__":
    main()
