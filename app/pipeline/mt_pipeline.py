"""Sora: from an analysed song (mtn's song.json) to finished, tagged files in the folder the user picked.

Run by Sora.exe with the bundled Python. It reuses the Python tool's own steps (tools/make.py: build, render,
iPod encode), so layout fixes in the Python tool reach the app unchanged; only delivery and tagging are new here.

  mt_pipeline.py --song S.json --audio SRC --id ID --title T --artist A [--album AL] [--accent #hex] [--cover IMG]
                 [--lyrics FILE] [--theme ID] [--background CLIP]
                 --outputs youtube_full,ipod_short,ipod_full,lyrics,audio --out-dir DIR

--theme picks a video theme from ui/themes.json ('original' = Music Tool's own look in the song's colour);
--background puts the lyrics over the user's own video, GIF or picture (its sound is dropped) instead of the record.

Progress goes to stdout as JSON lines: {"stage": ..., "done": n, "total": m}, {"file": path}, {"error": msg}.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

APP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(APP, "tool")
FFMPEG = os.path.join(APP, "engine", "ffmpeg.exe")

# bundled runtimes first on PATH (Windows looks up npx.cmd / ffmpeg in *our* PATH, not the child's env)
os.environ["PATH"] = os.pathsep.join([os.path.join(APP, "node"), os.path.join(APP, "engine"),
                                      os.path.dirname(sys.executable), os.environ.get("PATH", "")])
os.environ["FFMPEG"] = FFMPEG
sys.path.insert(0, os.path.join(TOOL, "tools"))
import make  # noqa: E402  (the Python tool's tools/make.py)

make.PY = sys.executable
make.FFMPEG = FFMPEG
make.ENV = dict(os.environ)

# what the app makes, by the Python tool's output names (vinyl look; the CD and dark-room looks need cover art
# layers / a depth model and stay in the Python tool for now)
LABELS = {"youtube_full": "YouTube 16x9", "ipod_short": "iPod short", "ipod_full": "iPod full"}


def load_theme(theme_id):
    with open(os.path.join(APP, "ui", "themes.json"), encoding="utf-8") as f:
        themes = {t["id"]: t for t in json.load(f)["themes"]}
    if theme_id not in themes:
        raise SystemExit(f"unknown theme '{theme_id}' (have: {', '.join(themes)})")
    th = {k: v for k, v in themes[theme_id].items() if k != "group"}
    return th if "accent" in th else None  # 'original' has no colours of its own


def make_background(src, edit_id, w, h, seconds):
    """The user's clip as a silent, looping H.264 at the video's exact size (cropped to fill, 30 fps), in
    renderer/public/bg. Pictures become a one-second still. Returns (public path, frame count)."""
    still = os.path.splitext(src)[1].lower() in (".png", ".jpg", ".jpeg", ".bmp", ".webp")
    name = f"{edit_id}_{make.sha(os.path.abspath(src), os.path.getsize(src), os.path.getmtime(src), w, h)[:8]}.mp4"
    dst = os.path.join(make.ROOT, "renderer", "public", "bg", name)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    cmd = [FFMPEG, "-v", "error", "-nostats", "-progress", "pipe:1", "-y"]
    cmd += ["-loop", "1", "-t", "1", "-i", src] if still else ["-i", src, "-t", f"{seconds:.2f}"]
    cmd += ["-an", "-sn", "-dn", "-map", "0:v:0",
            "-vf", f"fps=30,scale={w}:{h}:force_original_aspect_ratio=increase:flags=lanczos,crop={w}:{h},setsar=1,format=yuv420p",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-g", "30", "-movflags", "+faststart", dst]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    frames = [int(m) for m in re.findall(r"^frame=(\d+)", r.stdout, re.M)]
    if r.returncode != 0 or not frames or frames[-1] < 1:
        raise SystemExit(f"can't read the background clip {os.path.basename(src)}:\n{r.stderr.strip()[-600:]}")
    return "bg/" + name, frames[-1]


def emit(**kw):
    print(json.dumps(kw), flush=True)


_current = {"stage": "render", "of": ""}


def streaming_run(cmd, cwd=make.ROOT, check=True, quiet=True):
    """make.run, but the child's output is read as it comes so Remotion's 'Rendered 120/787' becomes progress."""
    p = subprocess.Popen(cmd, cwd=cwd, env=make.ENV, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         encoding="utf-8", errors="replace", bufsize=1,
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    tail = []
    for line in p.stdout:  # text mode turns Remotion's \r progress updates into lines
        line = line.rstrip()
        m = re.search(r"Rendered (\d+)/(\d+)", line)
        if m:
            emit(stage=_current["stage"], of=_current["of"], done=int(m.group(1)), total=int(m.group(2)))
        elif line:
            tail = (tail + [line])[-25:]
    p.wait()
    out = "\n".join(tail)
    if check and p.returncode != 0:
        raise SystemExit(f"step failed: {' '.join(map(str, cmd[:4]))} ...\n{out}")
    return subprocess.CompletedProcess(cmd, p.returncode, out, "")


make.run = streaming_run
make.say = lambda msg: print(msg, file=sys.stderr, flush=True)


def safe(name):
    return re.sub(r'[<>:"/\\|?*]', "", name).strip() or "song"


def tag(src, dst, a, cover=None, audio_only=False):
    """Copy src to dst with title/artist/album tags (+ cover art where the format takes it), no re-encode."""
    cmd = [FFMPEG, "-v", "error", "-y", "-i", src]
    attach = cover and os.path.exists(cover) and os.path.splitext(dst)[1].lower() in (".mp3", ".m4a", ".mp4", ".m4v", ".flac")
    if attach:
        cmd += ["-i", cover, "-map", "0:a" if audio_only else "0", "-map", "1", "-c", "copy",
                "-disposition:v:" + ("0" if audio_only else "1"), "attached_pic"]
    else:
        cmd += ["-map", "0:a" if audio_only else "0", "-c", "copy"]
    for k in ("title", "artist", "album"):
        if a.get(k):
            cmd += ["-metadata", f"{k}={a[k]}"]
    if dst.lower().endswith(".m4v"):
        cmd += ["-f", "ipod"]
    r = subprocess.run(cmd + [dst], capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if r.returncode != 0 and attach:  # some players' formats refuse the picture: tags without it
        return tag(src, dst, a, None, audio_only)
    if r.returncode != 0:
        shutil.copyfile(src, dst)
    return dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--song", required=True)
    ap.add_argument("--audio", required=True)
    ap.add_argument("--id", required=True)
    ap.add_argument("--title", default="")
    ap.add_argument("--artist", default="")
    ap.add_argument("--album", default="")
    ap.add_argument("--accent", default="#ff6b4a")
    ap.add_argument("--cover")
    ap.add_argument("--lyrics")
    ap.add_argument("--theme", default="original")
    ap.add_argument("--background")
    ap.add_argument("--outputs", required=True)
    ap.add_argument("--out-dir", required=True)
    a = ap.parse_args()
    outs = [o for o in a.outputs.split(",") if o]
    os.makedirs(a.out_dir, exist_ok=True)
    base = safe(f"{a.title} - {a.artist}" if a.artist else a.title)
    meta = {"title": a.title, "artist": a.artist, "album": a.album}
    sid = re.sub(r"\W", "_", a.id)
    theme = load_theme(a.theme)
    if a.background and not os.path.exists(a.background):
        raise SystemExit(f"background clip not found: {a.background}")
    cfg = {"title": a.title or a.id, "artist": a.artist, "accent": theme["accent"] if theme else a.accent}

    if "lyrics" in outs and a.lyrics and os.path.exists(a.lyrics):
        dst = os.path.join(a.out_dir, base + os.path.splitext(a.lyrics)[1])
        shutil.copyfile(a.lyrics, dst)
        emit(file=dst)
    if "audio" in outs:
        dst = os.path.join(a.out_dir, base + os.path.splitext(a.audio)[1].lower())
        if os.path.abspath(dst) != os.path.abspath(a.audio):
            emit(file=tag(a.audio, dst, meta, a.cover, audio_only=True))

    videos = [o for o in outs if o in LABELS]
    if any(make.OUTPUTS[o]["version"] == "short" for o in videos):
        emit(stage="short", done=0, total=1)
        make.run([make.PY, "tools/pick_short.py", a.song])
    for k, out in enumerate(videos):
        spec = make.OUTPUTS[out]
        _current.update(stage="render", of=LABELS[out])
        emit(stage="layout", of=LABELS[out], done=k, total=len(videos))
        edit_id = make.build(sid, cfg, out, spec, a.song, "vinyl")
        edit_json = os.path.join(make.ROOT, "renderer", "public", "edits", f"{edit_id}.json")
        bg = None
        if theme or a.background:  # read by Sora's renderer additions (renderer/src/sora)
            with open(edit_json, encoding="utf-8") as f:
                data = json.load(f)
            if theme:
                data["theme"] = theme
            if a.background:
                emit(stage="background", of=LABELS[out], done=0, total=1)
                src, frames = make_background(a.background, edit_id, data["width"], data["height"],
                                              data["durationInFrames"] / data["fps"])
                bg = os.path.join(make.ROOT, "renderer", "public", src)
                data["background"] = {"src": src, "frames": frames}
            with open(edit_json, "w", encoding="utf-8") as f:
                json.dump(data, f)
        qa = make.run([make.PY, "tools/qa_edit.py", edit_json], check=False)
        for line in qa.stdout.splitlines():
            if "ERROR" in line:
                print("QA: " + line, file=sys.stderr, flush=True)
        mp4 = make.render(edit_id, spec)
        final = make.ipod_file(mp4) if spec["format"] == "ipod" else mp4
        ext = os.path.splitext(final)[1]
        dst = os.path.join(a.out_dir, f"{base} ({LABELS[out]}){ext}")
        emit(file=tag(final, dst, meta, a.cover))
        if final != mp4:  # the iPod gets the .m4v; keep the full-quality 4:3 mp4 too
            emit(file=tag(mp4, os.path.join(a.out_dir, f"{base} ({LABELS[out]}, HQ).mp4"), meta, a.cover))
        # working files are big (a 4-minute song's audio is ~40 MB): drop them once delivered
        for p in (mp4, final, bg, os.path.join(make.ROOT, "renderer", "public", "audio", f"{edit_id}.wav")):
            if p and os.path.exists(p):
                os.remove(p)
    emit(stage="done", done=1, total=1)


if __name__ == "__main__":
    try:
        main()
    except SystemExit as e:
        if e.code not in (0, None):
            emit(error=str(e.code))
            sys.exit(1)
    except Exception as e:  # noqa: BLE001 - the app shows the message
        emit(error=f"{type(e).__name__}: {e}")
        sys.exit(1)
