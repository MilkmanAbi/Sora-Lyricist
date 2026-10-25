# Sora

Sora makes lyric videos: drop songs in, and it checks their tags, finds and times their lyrics, and renders the videos.
It's a native (C++20) rewrite of the Python Music Tool's heavy half. The neural networks run on whatever the PC has:
- a **discrete GPU** (AMD Radeon, NVIDIA, Intel Arc) through ONNX Runtime + DirectML
- otherwise an **Intel iGPU** alongside the CPU, through OpenVINO
- otherwise the **CPU**

Finished chunks are cached on the SSD. It installs as a normal Windows program (Inno Setup), with everything it needs
inside its own folder.

## Download

Get `Sora-Setup-<version>.exe` from [Releases](../../releases). It needs Windows 10 (1809+) or 11, 64-bit, and
installs per user (no admin). Python, Node, FFmpeg, the models and the renderer are inside it, so nothing has to be
installed by hand; WebView2 is added if Windows lacks it. The first song downloads nothing, but the lyric lookup needs
the internet. Allow ~3 GB of disk, plus the SSD cache (5 GB cap).

## Devices

`mtn devices` (or the status line in the window) shows what was found and the plan:

| PC has | separation (Demucs) | beat tracking | lyric timing (wav2vec2) |
|---|---|---|---|
| a discrete GPU (D3D12 says it is not UMA) | DirectML fp32 | DirectML fp32 | DirectML fp16 |
| an Intel iGPU | OpenVINO GPU fp32 | CPU | CPU INT8, alongside the GPU |
| nothing else | CPU | CPU | CPU INT8 |

Discrete means the GPU has its own memory. That's asked of D3D12 (`D3D12_FEATURE_ARCHITECTURE.UMA`) rather than read
from the "dedicated memory" figure, which AMD APUs also report for RAM they borrow. With several discrete GPUs, the
one with the most memory is used. Override with `--device cpu|gpu|dml|dml:N`, per network with
`--sep-device/--beat-device/--w2v-device`, or `--no-dgpu`. Demucs never runs in fp16 on a GPU: it overflows on real
music.

**Every GPU is checked before it's trusted.** The first chunk of each network on DirectML also runs on the CPU:
- Demucs must reach 30 dB SDR against the CPU.
- The aligner must hear the same letters, within ±2 frames, on at least half of the sung frames.
- Beat This! must produce the same logits, within 0.05.

A GPU that fails the check, or throws, is logged ("GPU check: …"). Sora finishes the song on the safe path and
records the failure in `%LOCALAPPDATA%\Sora\cache\gpu\`, so that GPU isn't tried for that network again. Delete that
folder to retry, e.g. after a driver update.

What was verified here, with no discrete GPU available:
- **Beat This! on DirectML** passes on this laptop's Iris Plus: the same 162 beats as the CPU.
- **Demucs and wav2vec2** can't run on DirectML on a 15 W Iris Plus. Each GPU submission takes longer than Windows'
  2-second GPU watchdog, so Windows resets the GPU (`887A0006`) and the outputs come back as zeros. A discrete Radeon
  or GeForce is 10-20× faster and should finish well within the watchdog, but that couldn't be tested here.
- **The safety net works.** With simulated reset output (`MT_TEST_GPU_ZEROS=1`), all three checks caught it, fell
  back, and remembered it. The words still came out at 65 ms mean error, the same as the normal path.

Windows crashes if 5 GPU resets happen within 60 seconds. The checks plus the failure record mean a bad GPU costs at
most one reset per network, ever. Don't force `--device dml` on a weak iGPU.

## Phases

| Phase | What | Status |
|---|---|---|
| 1 | **`mtn analyze`**: decode → vocal separation (htdemucs) → beats (Beat This!) → word alignment (wav2vec2 + CTC) → vocal onsets. Writes the same `song.json` as `tools/analyze_song.py`, so `make.py`, `build_edit.py` and the Remotion renderer use it unchanged (`make.py --engine native`). On a 4-minute song it takes **151 s, against 614 s for the Python pipeline (~4×)**. Re-analysing with a cached separation takes **6 s**, and the lyric timing is more accurate (below). | **done** |
| 2 | Port `build_edit.py` + `pick_short.py` (layout, short-window picking: plain logic) | next |
| 3 | Native renderer (Direct2D/D3D11 compositing + Quick Sync H.264 encode) to replace Remotion/Node | planned |
| 4 | The Sora app: window, tagger, lyric picker, self-contained Inno Setup installer (`app\`, `installer\`) | **done** (v0.3.0) |

Where the time goes on a 252 s song (i7-1065G7 + Iris Plus): the iGPU separates 44 chunks at ~3.1 s each, which takes
~140 s. Meanwhile the CPU tracks beats (11 s) and aligns each finished 24 s window of vocals (13 windows, INT8), so
alignment ends ~3 s after separation does. Separation is the critical path. CPU-only, the same song takes 188-270 s
(the CPU's Demucs slows from 3.0 to 4.4 s/chunk as the laptop heats; the iGPU stays at ~3.1).

Out of scope: cover-art generation (Stable Diffusion), the iPod depth composite, OCR of lyric videos and the web UI.
Those stay in the Python tool.

## The app (Sora.exe)

A window to drop songs into: it checks their tags, finds and times their lyrics, and makes the files you pick.

1. **Drop songs** (MP3, WAV, FLAC, M4A, AAC, OGG, OPUS, WMA, AIFF, also whole folders) or choose them with *Add…*.
2. **Tags** for each song: *Keep the file's tags* or *Use my own* (title, artist, album, cover image). The
   tags are used for the lyrics search, the video's title strip and the finished files' metadata, with the cover as
   embedded album art.
3. **Lyrics** are looked up as soon as a song is added. Pick *Local* (LRCLIB, timed), *Web* (Genius, edited words),
   *Both* (Genius words on LRCLIB timing, the default) or *My own* (paste plain lines or a synced .lrc). The
   chosen version is previewed with its timestamps, along with the lines where Genius corrected LRCLIB.
4. **Make**: Video 16:9 (1920×1080), iPod short (~30 s, 4:3 + the .m4v a 5th-gen iPod plays), iPod full, the lyrics
   file, and a tagged copy of the audio. *Save to* is asked when you press Make (and remembered as the default).
   Progress shows per song (separating, beat, timing, laying out, rendering). *Stop* ends every running step at once.

Under the hood the window (C++ + WebView2, static runtime) runs `engine\mtn.exe` for tags, lyrics and analysis, then
`python\python.exe pipeline\mt_pipeline.py`, which reuses the Python tool's own `make.py` steps (layout, Remotion
render, iPod encode) and adds tagging and delivery. Every child process sits in a kill-on-close job object.

**Self-contained.** The installed folder carries everything, so nothing has to be installed by hand:

| folder | what | size |
|---|---|---|
| `Sora.exe`, `ui\` | the window | 0.6 MB |
| `engine\` | mtn, OpenVINO, models, ffmpeg, the VC++ runtime (app-local, incl. OpenMP) | 863 MB |
| `python\` | Python 3.11 embeddable + numpy/scipy/librosa/soundfile/pyyaml/requests | 342 MB |
| `node\` | Node 24 for the renderer | 106 MB |
| `tool\` | the Python tool's helpers + the Remotion renderer, `node_modules` with headless Chrome | ~0.7 GB |

The setup installs per user (no admin) and adds WebView2 if Windows lacks it, which only some Windows 10 machines do.
It was tested end to end with `PATH` cut down to `System32`: song → lyrics → analysis → iPod video + tagged files.

Not in the app yet: the CD look (it needs cover-art layers made by `tools/art/cd_cover.py`) and the dark-room iPod
photo look (it needs the depth model). Both stay in the Python tool.

Build the app: `build.bat` (engine), `build.bat gui` (window), `installer\assemble_app.ps1` (app folder),
`ISCC installer\musictool.iss` (setup). For development, `MT_DEV=1` enables the page's dev tools, and
`MT_TEST_FILES=a.mp3;b.mp3` with `MT_TEST_START=folder` adds songs and makes them without any clicks.

## Lyrics from the web

Without `--lrc` / `--lyrics`, `mtn analyze` finds the lyrics itself; `mtn lyrics SONG.mp3` does just that step:

```
Lyrics for Die_With_A_Smile.mp3 (4:12)
  song title [Die With A Smile]:
  artist (helps find the right song) [Lady Gaga x Bruno Mars]:

  [1] local  LRCLIB: "Die With A Smile" by Lady Gaga X Bruno Mars (4:12), 37 lines, synced
  [2] web    Genius: "Die With A Smile" by Lady Gaga, 50 lines (edited words, no timing)
  [3] both   Genius words on LRCLIB timing: 37 lines matched (3 with different words), 0 only on Genius, ...
         Genius "And our love's the only war worth fighting for"
         LRCLIB "And our love's the only one worth fighting for"

Which lyrics? 1 = local, 2 = web, 3 = both [3]:
Save lyrics to [...\lyrics\Die_With_A_Smile.lrc]:
```

- **Search.** The title and artist come from the file's tags, or failing that its name. Genius's search is literal
  ("Lady Gaga **x** Bruno Mars" finds nothing), so the query is simplified step by step until a hit fits. LRCLIB
  lists several versions of most songs, and the one whose **length matches the file** is used. Without an artist the
  title has to match closely, and the prompt asks you to check it's the right song.
- **both** aligns the two line by line (1:1, 2:1 and 1:2 matches, because Genius breaks lines differently). Matched
  lines take Genius's words and LRCLIB's timestamp. Genius-only lines are aligned untimed: the .lrc keeps them without
  a stamp, which mtn reads but the Python tool skips. Repeats that Genius wrote only once come from LRCLIB.
  Bracketed ad-libs that Genius adds and LRCLIB doesn't have are dropped, e.g. "Make me your radio (Yeah, ha)". They
  are backing vocals, and placing them dragged the lead line's timing.
- **Namesakes.** If fewer than half the lines match, the two are different songs (*Fireworks*: Drake on Genius,
  DAOKO on LRCLIB). Then the one whose length fits the file wins and "both" is not offered.
- **Lyrics-fit check.** After aligning, mtn checks that the lyrics match the singing. Mean letter confidence was
  0.24-0.66 for correct lyrics and 0.06 for a namesake's. Below 0.15 it warns, and `song.json` keeps `lyrics_fit`.
- `--yes` takes the defaults without asking (both, saved next to the output), as do piped or scheduled runs.
  Other flags: `--lyrics-source local|web|both`, `--save-lyrics PATH`, `--title`, `--artist`. LRCLIB answers 503
  under load, so requests retry after 1/3/7/15 s.
- Genius's terms don't allow scraping. This is for personal lyric videos; keep it out of anything public.

What it changes (8 songs from the music folder, local vs both, same audio): the timing quality is the same (letter
confidence 0.42 vs 0.42, squashed words 27% vs 27%). The gain is **correct words on screen**: Genius fixed "keep **it**
stuck" → "keep **me** stuck", "yikky yeah" → "ye-yeah" and "hearts" → "heart's" (Stereo Hearts), and "the only **one**
worth fighting for" → "the only **war**" (Die With A Smile).

## Layout

The source is here, on the Desktop. Anything big lives in `%MTN_HOME%` (default `%USERPROFILE%\music-native`), which
isn't synced by OneDrive:

```
%MTN_HOME%\third_party   OpenVINO SDK (headers, import libs, DLLs), pocketfft, nlohmann/json
%MTN_HOME%\models        OpenVINO IR (.xml/.bin) made by tools/export_models.py
%MTN_HOME%\build         mtn.exe + runtime DLLs (models\ is a junction)
%LOCALAPPDATA%\MusicToolNative\cache   SSD chunk cache + compiled GPU kernels
```

## Build

```
build.bat                     # MSVC (VS 2026), /O2 /arch:AVX2 /openmp, links OpenVINO
%MTN_HOME%\build\mtn.exe devices
%MTN_HOME%\build\mtn.exe analyze song.wav --lrc song.lrc --out analysis\songs\song.song.json
build.bat dev                 # same, as mtn_dev.exe (when a benchmark holds mtn.exe)
installer\stage.bat           # then: "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\mtn.iss
```

From the Python tool: `.venv\Scripts\python tools\make.py medusa --engine native` (finds `mtn` on PATH, or `MTN=`).

Benchmark and tuning (music-tool venv, which has the old aligner):
```
python tools\bench.py <jamendolyrics> <work> --songs 8      # native vs Python, exact/noisy, normal/slowed
python tools\sweep.py <work> --snapshot keys.json --grid "weight=15,40 filler=-3,-0.75"
python tools\parity.py native.song.json python.song.json --truth words.json
```

Models are a one-time export, done at build time (`export-venv` = torch, demucs, beat_this, openvino):

```
export-venv\Scripts\python tools\export_models.py %MTN_HOME%\models
```

## Design

- **Fixed shapes everywhere.** The GPU plugin compiles kernels per input shape, so every network gets one fixed chunk
  size: Demucs 7.8 s segments, wav2vec2 24 s windows (20 s kept + 2 s context either side), Beat This! 30 s windows.
  The compiled kernels are cached (`ov::cache_dir`), so only the very first run pays the compile.
- **SSD chunk cache** (`src/cache.*`). Each chunk's result is keyed by a hash of its exact input samples plus the model
  id. Re-running a song, resuming after a crash, or re-aligning with corrected lyrics only computes chunks the cache
  has never seen. Writes go to a temp file and are then renamed, so a killed run never leaves a half-written chunk.
- **CPU + iGPU pipeline** (`src/main.cpp`). Demucs runs on the iGPU in **fp32**: fp16 overflows inside Demucs on real
  music, giving a vocal stem at −18 dB SDR (`tools/gpu_demucs_check.py`). Demucs keeps two infer requests, so the CPU
  does chunk k+1's STFT while the GPU runs chunk k. After every chunk the separator hands over the newly final
  stretch of the stem. While the GPU works, the CPU resamples it to 16 kHz in-process, tracks beats, and runs INT8
  wav2vec2 on each 24 s window as soon as it is complete. With no GPU, or with `--sequential`, the stages run back to
  back on the CPU (overlapping them on the CPU alone would only oversubscribe it). The Viterbi, peak picking and
  onsets stay on the CPU because they are sequential or small.
- **Same maths as the Python tool.** The STFT/iSTFT, mel banks, Demucs' `apply_model` chunking, Beat This!'s
  `split_piece` and post-processing, and librosa's onset detector are ported with the same windows, padding and
  normalisation. `tools/parity.py` checks the native output against the Python output stage by stage.
- **Alignment** is one whole-song CTC Viterbi with the `.lrc` line times as soft anchors (`src/align.cpp`), instead of
  WhisperX cropping the song into two-line windows. That way a wrong or early timestamp can't crop words off.

## Measurements (i7-1065G7 + Iris Plus G7, 4 cores, 15 W, on AC)

**Real audio**, per chunk/window, model load excluded (`mtn` logs, `tools/gpu_demucs_check.py`, `tools/spike_overlap_real.py`):

| network | CPU | iGPU |
|---|---|---|
| htdemucs (7.8 s chunk) | 3.0-4.4 s fp32 (INT8 rejected: stem at 2-18 dB SDR) | **3.0-3.5 s fp32** (exact, 95-100 dB vs CPU) · fp16 2.2 s but **broken** |
| wav2vec2-large (24 s window) | 7.7 s fp32 · **~3 s INT8** (≈2.5×) | 7.9 s fp16 (accurate, not faster) |
| Beat This! (30 s window) | ~4 s | fp16 moves logits by 0.35, so it stays on the CPU |

Concurrency on real audio: Demucs on the iGPU together with INT8 wav2vec2 on the CPU takes 20.4 s, against 27.9 s
back to back. With the CPU busy, the iGPU only slows from 2.9 to 3.4 s per chunk.

An earlier spike (`tools/spike_bench.py`, `spike_concurrency.py`) fed **random noise** into the networks. That hit
slow CPU paths (Demucs at 9.1 s per chunk instead of 3-4 s), made concurrency look like a loss, and made INT8 look
9× faster. Its numbers are kept in the scripts' history, but they're wrong for music, so use the table above.
First-ever compile per network on the iGPU takes 30-110 s. The kernels are cached after that (~3 s).

Parity on a 90 s clip (`tools/parity.py`, same stems into both pipelines): the vocal stem matches Demucs at 77 dB SDR,
beats 162/162 (99% within 10 ms), vocal onsets 227/227 (99% within 10 ms).

## Lyric timing accuracy

`tools/bench.py` runs JamendoLyrics (hand-timed words), 8 English songs, first 90 s, each at normal speed and as a
"super slowed + reverb" copy (0.8×). Each version runs with two kinds of line timestamps: **exact** (the dataset's
own) and **noisy** (+0.3 s offset ±0.4 s jitter, like an LRCLIB file made for a different upload). That's 3,468 words.
The error is |predicted − true| word start, and a word that never shows counts as 5 s.

| condition | Python tool (WhisperX, 2-line windows) | **mtn** (whole-song CTC, soft anchors, INT8 wav2vec2) |
|---|---|---|
| exact, normal | 91 ms · 78% < 100 ms · 4% > 300 ms | **83 ms · 83% · 4%** |
| exact, slowed | 116 ms · 70% · 6% | 117 ms · 73% · 5% |
| noisy, normal | 119 ms · 69% · 9% | **84 ms · 83% · 4%** |
| noisy, slowed | 153 ms · 61% · 12% | **119 ms · 73% · 5%** |
| **all** | 120 ms · 69% · 8% | **99 ms · 77% · 4%** (bias 0 ms) |

The aligner's three knobs (anchor weight 40, filler −0.75, start shift −20 ms) were tuned with `tools/sweep.py` on
songs 1-4 only. On held-out songs 5-8 they give **66 ms · 81% · 1%**, against 90 ms · 74% · 5% for the Python tool, so
the tuning carries over. (Numbers with the current build, in-process 16 kHz resampling: all 32 tracks 100 ms · 77% · 5%.) Because the anchors are soft, exact and noisy timestamps give the same result. The old windows
crop audio at the timestamp, so they lose accuracy whenever it is wrong.

**Plain-text lyrics** (no timestamps: a dropped `.txt` or an unsynced LRCLIB result) are aligned unanchored over the
whole song (`tools/bench_plain.py`, same 16 tracks): mtn gets 123 ms · 81% · 5% (normal) and 182 ms · 72% · 7%
(slowed). The Python tool gets 484 ms · 73% · 8% and 567 ms · 67% · 10%, because it sometimes misplaces whole sections.

Things tried and dropped (all measured): the `|` word-separator token (worse, 187 vs 153 ms), wav2vec2 fp32 instead
of INT8 (no consistent gain: 309 vs 219 ms and 309 vs 375 ms on the two hardest tracks), Demucs INT8 (vocal stem too
damaged).

## Licences

Sora's own code is GPL-3.0 (`LICENSE`). The bundled software and models keep their own terms, listed in
`THIRD-PARTY-NOTICES.txt`, which installs alongside the app. Notably, the htdemucs weights are non-commercial
(CC BY-NC 4.0), the bundled FFmpeg is GPL-3.0, and Remotion needs a company licence for companies of more than 3
people. The lyric-video layout and renderer come from Music Tool, by its authors. Genius lyric lookup is for personal
use only.
