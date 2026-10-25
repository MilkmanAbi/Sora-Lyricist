"""Lyric-timing benchmark: native engine vs the Python tool's aligner, on JamendoLyrics (hand-timed words).

Usage (music-tool venv, which has whisperx for the old aligner):
  python tools/bench.py <jamendolyrics dir> <work dir> [--mtn PATH] [--songs N] [--no-python] [--extra "--w2v base"]

Per song: the first 90 s, at normal speed and as a "super slowed + reverb" copy (0.8x, like the Python tool's
tools/bench/prepare_jamendo.py). Two kinds of line timestamps:
  exact   the dataset's own line starts (what the old benchmark used; optimistic)
  noisy   +0.3 s global offset and +-0.4 s jitter per line - like LRCLIB files made for another upload of the song
mtn separates the vocals itself (numerically the same as Demucs), so the old aligner runs on those stems too.
Re-runs are cheap: mtn's SSD cache keeps the separated chunks and wav2vec2 windows, so only the Viterbi re-runs.
"""
import argparse
import csv
import difflib
import json
import os
import re
import subprocess
import sys
import time
import zlib

import numpy as np

TOOLS = r"C:\Users\abina\music-tool\tools"
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, "bench"))

SONGS = ["HILA_-_Give_Me_the_Same", "Quentin_Hannappe_-_Keep_On", "Avercage_-_Embers", "Color_Out_-_Falling_Star",
         "The.madpix.project_-_One_Way_Street", "JASON_MILLER_-_CROWD_PLEASER", "Lower_Loveday_-_Is_It_Right_",
         "Moon_I_Mean_-_Wrong_Concept", "Slingshot_Miracle_-_Whistler", "Rxbyn_-_Bad_Side",
         "Tom_Orlando_-_The_One__feat._Tina_G_", "LUNABLIND_-_Vision__Radio_Edit_",
         "Explosive_Ear_Candy_-_Like_The_Sun", "Cortez_-_Feel__Stripped_"]
CLIP_S, SLOW = 90, 0.8


def fmt(t):
    return f"[{int(t // 60):02d}:{t % 60:05.2f}]"


def norm(w):
    return re.sub(r"[^a-z0-9']", "", w.lower())


def errors(words, truth):
    a, b = [norm(w["word"]) for w in words], [norm(w["word"]) for w in truth]
    e = []
    for blk in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
        e += [words[blk.a + k]["start"] - truth[blk.b + k]["start"] for k in range(blk.size)]
    return np.array(e), len(truth) - len(e)


def prepare(root, work, name, speed):
    from prepare_jamendo import clip_wav, slowed_reverb
    lines = list(csv.DictReader(open(os.path.join(root, "annotations", "lines", name + ".csv"), encoding="utf-8")))
    words = list(csv.DictReader(open(os.path.join(root, "annotations", "words", name + ".csv"), encoding="utf-8")))
    texts = [w.strip() for w in open(os.path.join(root, "lyrics", name + ".words.txt"), encoding="utf-8") if w.strip()]
    lines = [l for l in lines if float(l["end_time"]) <= CLIP_S - 0.5]
    truth = [{"word": t, "start": float(w["word_start"])} for t, w in zip(texts, words)]
    truth = truth[:sum(len(l["lyrics_line"].split()) for l in lines)]
    k = 1 / speed
    tag = name + "__90s" + ("" if speed == 1.0 else "__slowed")
    wav = os.path.join(work, tag + ".wav")
    base = os.path.join(work, name + "__90s.wav")
    if not os.path.exists(base):
        clip_wav(os.path.join(root, "mp3", name + ".mp3"), base)
    if speed != 1.0 and not os.path.exists(wav):
        slowed_reverb(base, wav)
    rng = np.random.default_rng(zlib.crc32(tag.encode()))  # same noise every run (hash() is salted)
    lrcs = {}
    for cond in ("exact", "noisy"):
        p = os.path.join(work, f"{tag}.{cond}.lrc")
        with open(p, "w", encoding="utf-8") as f:
            for l in lines:
                t = float(l["start_time"]) * k
                if cond == "noisy":
                    t = max(0.0, t + 0.3 + rng.uniform(-0.4, 0.4))
                f.write(f"{fmt(t)}{l['lyrics_line']}\n")
        lrcs[cond] = p
    return tag, wav, lrcs, [dict(w, start=w["start"] * k) for w in truth]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("work")
    ap.add_argument("--mtn", default=os.path.expandvars(r"%USERPROFILE%\music-native\build\mtn.exe"))
    ap.add_argument("--songs", type=int, default=len(SONGS))
    ap.add_argument("--no-python", action="store_true")
    ap.add_argument("--extra", default="", help="extra mtn flags, e.g. '--w2v base' (results cached per flag set)")
    ap.add_argument("--label", default="native")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    res_path = os.path.join(args.work, "results.json")
    res = json.load(open(res_path)) if os.path.exists(res_path) else {}
    for name in SONGS[:args.songs]:
        for speed in (1.0, SLOW):
            tag, wav, lrcs, truth = prepare(args.root, args.work, name, speed)
            for cond, lrc in lrcs.items():
                key = f"{tag}|{cond}"
                r = res.setdefault(key, {"truth": truth})
                out = os.path.join(args.work, "songs", f"{tag}.{cond}.{args.label}.song.json")
                if args.label not in r:
                    t = time.time()
                    cmd = [args.mtn, "analyze", wav, "--lrc", lrc, "--out", out] + args.extra.split()
                    p = subprocess.run(cmd, capture_output=True, text=True)
                    if p.returncode:
                        print(p.stderr[-2000:])
                        raise SystemExit(f"mtn failed on {tag}")
                    r[args.label] = {"words": json.load(open(out, encoding="utf-8"))["words"], "s": time.time() - t}
                if not args.no_python and "python" not in r:
                    import analyze_song as A
                    song = json.load(open(out, encoding="utf-8"))
                    t = time.time()
                    try:
                        w = A.align_words(song["stems"]["vocals"], "", "en", lrc=lrc)
                    except Exception as ex:  # the old aligner raises on some lines
                        print(f"  python aligner failed on {key}: {ex}")
                        w = []
                    r["python"] = {"words": w, "s": time.time() - t}
                json.dump(res, open(res_path, "w"))
            print(tag, "done", flush=True)
    report(res)


def report(res):
    systems = sorted({k for r in res.values() for k in r if k != "truth"})
    print(f"\n{'condition':16} {'system':14} {'words':>6} {'mean err':>9} {'median':>7} {'<100ms':>7} {'>300ms':>7} {'bias':>6}")
    for cond in ("exact", "noisy"):
        for sp in ("normal", "slowed"):
            for s in systems:
                es, miss, n = [], 0, 0
                for key, r in res.items():
                    tag, c = key.split("|")
                    if c != cond or ("slowed" in tag) != (sp == "slowed") or s not in r:
                        continue
                    e, m = errors(r[s]["words"], r["truth"])
                    es.append(e)
                    miss += m
                    n += len(r["truth"])
                if not es:
                    continue
                e = np.concatenate(es)
                ae = np.concatenate([np.abs(e), np.full(miss, 5.0)])  # a word never shown counts as badly wrong
                print(f"{cond + ' ' + sp:16} {s:14} {n:6} {ae.mean() * 1000:7.0f}ms {np.median(ae) * 1000:5.0f}ms "
                      f"{(ae < .1).mean():7.0%} {(ae > .3).mean():7.0%} {np.median(e) * 1000:+5.0f}ms")


if __name__ == "__main__":
    main()
