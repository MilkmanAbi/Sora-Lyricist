"""Lyric timing outside English: JamendoLyrics' French, German and Spanish songs (hand-timed words).

Usage (music-tool venv, as tools/bench.py):
  python tools/bench_lang.py <jamendolyrics dir> <work dir> --system NAME=MTN_EXE[|extra flags] [...] [--per-lang N]

First 90 s of each song at normal speed, two kinds of lyrics: plain text (no timing at all) and "noisy" line
timestamps (as tools/bench.py). Results are cached per system in <work>/results_lang.json; stems and wav2vec2
windows come from mtn's SSD cache, so a second system costs only its Viterbi pass.
"""
import argparse
import csv
import json
import os
import re
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402


def norm(w):  # letters of any script, lower case: "Für," -> "für"
    return re.sub(r"[^\w']", "", w.lower())


def errors(words, truth):
    import difflib
    a, b = [norm(w["word"]) for w in words], [norm(w["word"]) for w in truth]
    e = []
    for blk in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
        e += [words[blk.a + k]["start"] - truth[blk.b + k]["start"] for k in range(blk.size)]
    return np.array(e), len(truth) - len(e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("work")
    ap.add_argument("--system", action="append", required=True, help="NAME=PATH_TO_MTN[|extra flags]")
    ap.add_argument("--per-lang", type=int, default=3)
    ap.add_argument("--langs", default="French,German,Spanish")
    args = ap.parse_args()
    os.makedirs(os.path.join(args.work, "songs"), exist_ok=True)
    rows = list(csv.DictReader(open(os.path.join(args.root, "JamendoLyrics.csv"), encoding="utf-8")))
    songs = []
    for lang in args.langs.split(","):
        # one singer, words only (no "la la" lines): what lyric timing is judged on
        pick = [r for r in rows if r["Language"] == lang and r["Polyphonic"] == "false" and r["NonLexical"] == "false"]
        songs += [(lang, os.path.splitext(r["Filepath"])[0]) for r in pick[:args.per_lang]]
    path = os.path.join(args.work, "results_lang.json")
    res = json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {}
    for lang, name in songs:
        tag, wav, lrcs, truth = bench.prepare(args.root, args.work, name, 1.0)
        txt = os.path.join(args.work, tag + ".txt")
        with open(lrcs["exact"], encoding="utf-8") as f:
            open(txt, "w", encoding="utf-8").write("".join(re.sub(r"\[[^\]]*\]", "", ln) for ln in f))
        for cond, lyr, flag in (("plain", txt, "--lyrics"), ("noisy", lrcs["noisy"], "--lrc")):
            r = res.setdefault(f"{tag}|{cond}", {"lang": lang, "truth": truth})
            for spec in args.system:
                label, rest = spec.split("=", 1)
                exe, _, extra = rest.partition("|")
                if label in r:
                    continue
                out = os.path.join(args.work, "songs", f"{tag}.{cond}.{label}.song.json")
                p = subprocess.run([exe, "analyze", wav, flag, lyr, "--out", out] + extra.split(),
                                   capture_output=True, text=True, encoding="utf-8", errors="replace")
                if p.returncode:
                    print(p.stderr[-1500:])
                    raise SystemExit(f"{label} failed on {tag}")
                song = json.load(open(out, encoding="utf-8"))
                r[label] = {"words": song["words"], "fit": song["engine"].get("lyrics_fit")}
                json.dump(res, open(path, "w", encoding="utf-8"))
        print(lang, tag, "done", flush=True)
    labels = [s.split("=", 1)[0] for s in args.system]
    print(f"\n{'lang':8} {'lyrics':6} {'system':10} {'words':>6} {'mean':>7} {'median':>7} {'<100ms':>7} {'>300ms':>7} {'fit':>5}")
    for lang in args.langs.split(","):
        for cond in ("plain", "noisy"):
            for s in labels:
                es, miss, n, fits = [], 0, 0, []
                for key, r in res.items():
                    if r["lang"] != lang or not key.endswith("|" + cond) or s not in r:
                        continue
                    e, m = errors(r[s]["words"], r["truth"])
                    es.append(e)
                    miss += m
                    n += len(r["truth"])
                    fits.append(r[s]["fit"] or 0)
                if not es:
                    continue
                ae = np.concatenate([np.abs(np.concatenate(es)), np.full(miss, 5.0)])
                print(f"{lang:8} {cond:6} {s:10} {n:6} {ae.mean() * 1000:5.0f}ms {np.median(ae) * 1000:5.0f}ms "
                      f"{(ae < .1).mean():7.0%} {(ae > .3).mean():7.0%} {np.mean(fits):5.2f}")


if __name__ == "__main__":
    main()
