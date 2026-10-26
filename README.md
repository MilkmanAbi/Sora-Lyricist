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
| 4 | The Sora app: window, tagger, lyric picker, self-contained Inno Setup installer (`app\`, `installer\`) | **done** (v0.3.0; themes + own backgrounds v0.4.0; lyrics in any language v0.5.0) |

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
3. **Lyrics** are looked up as soon as a song is added, on five sites at once (below). Pick the recommended version
   (the best words on the best timing), any one site's own, or *My own* (paste plain lines or a synced .lrc). The
   chosen version is previewed with its timestamps, along with the lines where the words site corrected the timed
   one. Lyrics in Japanese, Chinese, Korean, Cyrillic, Indic scripts and more are timed through their romanisation
   and can be shown as written or romanised.
4. **Look**: a video theme. *Original* is Music Tool's own look in a colour you pick. The **Classic** themes are Darker,
   Polarized (Solarized), Monokai, Gruvbox, Dracula, Nord, Catppuccin and Tokyo Night. The **Moods** are Ocean,
   Sakura, Sky, Forest, Sunset, Lavender and Melon. A theme recolours the sleeve, title strip and record, and puts a
   soft highlight behind the word being sung. *Behind the lyrics* is the spinning record, or **your own video, GIF or
   picture**: it fills the frame, loops if it's shorter than the song, is tinted by the theme, and its sound is
   dropped. *Use for all songs* copies the look to the whole list.
5. **Make**: Video 16:9 (1920×1080), iPod short (~30 s, 4:3 + the .m4v a 5th-gen iPod plays), iPod full, the lyrics
   file, and a tagged copy of the audio. *Save to* is asked when you press Make (and remembered as the default).
   Progress shows per song (separating, beat, timing, laying out, rendering). *Stop* ends every running step at once.

Under the hood the window (C++ + WebView2, static runtime) runs `engine\mtn.exe` for tags, lyrics and analysis, then
`python\python.exe pipeline\mt_pipeline.py`, which reuses the Python tool's own `make.py` steps (layout, Remotion
render, iPod encode) and adds tagging and delivery. Themes live in `ui\themes.json`. The pipeline writes the chosen
theme and the converted clip (silent H.264 at the video's size, in `renderer\public\bg`) into the edit. Sora's
renderer additions in `app\renderer\src\sora` draw them; without either, Music Tool's own composition renders
unchanged. Every child process sits in a kill-on-close job object.

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
`ISCC installer\sora.iss` (setup). For development, `MT_DEV=1` enables the page's dev tools, and
`MT_TEST_FILES=a.mp3;b.mp3` with `MT_TEST_START=folder` adds songs and makes them without any clicks
(`MT_TEST_OUTPUTS`, `MT_TEST_THEME=sakura`, `MT_TEST_BACKGROUND=clip.mp4` narrow it down).

## Lyrics from the web

Without `--lrc` / `--lyrics`, `mtn analyze` finds the lyrics itself; `mtn lyrics SONG.mp3` does just that step. Five
sites are asked at once:

| site | gives | best for |
|---|---|---|
| [LRCLIB](https://lrclib.net) | synced lines | everything (community-made) |
| [NetEase Cloud Music](https://music.163.com) | synced lines | Chinese, Japanese, Korean, and plenty of Western songs |
| [Bugs](https://music.bugs.co.kr) | synced lines | Korean |
| [Genius](https://genius.com) | carefully edited words and line breaks, no timing | most languages |
| [lyrics.ovh](https://lyrics.ovh) | plain words | English (needs the artist) |

```
Lyrics for Albireo.mp3 (3:27)
  LRCLIB:    "Albireo" by Rokudenashi (3:27), 52 lines, timed
  NetEase:   empty lyrics on NetEase
  Bugs:      "アルビレオ / Albireo" by ロクデナシ (Rokudenashi), 49 lines, timed
  Genius:    "アルビレオ (Albireo)" by ロクデナシ (Rokudenashi), 49 lines
  lyrics.ovh: not on lyrics.ovh
  lyrics in Japanese: timed letter by letter through their romanisation

  [1] Genius words on LRCLIB timing  49 lines matched (0 with different words), 0 only on Genius, 0 only on LRCLIB
  [2] LRCLIB                          "Albireo" by Rokudenashi (3:27), 52 lines, timed
  [3] Bugs                            "アルビレオ / Albireo" by ロクデナシ, 49 lines, timed
  [4] Genius                          "アルビレオ (Albireo)" by ロクデナシ, 49 lines, words only
Which lyrics? [1]:
```

- **Search.** The title and artist come from the file's tags, or failing that its name. Titles are compared across
  scripts: a file called "Haru no Kaze" finds 春の風, because both sides are compared through their romanisation.
  Genius's search is literal ("Lady Gaga **x** Bruno Mars" finds nothing), so the query is simplified step by step
  until a hit fits. The timed sites list several versions of most songs, and the one whose **length matches the file**
  is used. Without an artist the title has to match closely, and the prompt asks you to check it's the right song.
- **Same song?** Every site's answer is compared line by line with every other's. The answer most others agree with
  is the reference, and answers that share less than half their lines with it are namesakes and are left out
  (*봄날*: NetEase offered a different song of that name, LRCLIB and Bugs had BTS's). Japanese and Chinese lines are
  compared character by character.
- **Best version** (the default): the best words site (Genius, else lyrics.ovh) on the best timed site (the one whose
  length fits the file; LRCLIB, then NetEase, then Bugs). The two are aligned line by line (1:1, 2:1 and 1:2 matches,
  because Genius breaks lines differently). Matched lines take the edited words and the timed site's timestamp.
  Lines only the words site has are aligned untimed: the .lrc keeps them without a stamp, which mtn reads but the
  Python tool skips. Repeats the words site wrote only once come from the timed site. Bracketed ad-libs it adds and
  the timed site doesn't have are dropped, e.g. "Make me your radio (Yeah, ha)". They are backing vocals, and
  placing them dragged the lead line's timing.
- **Clean-up.** The credits NetEase puts on top as timed lines ("作词 : Ayase") and its "pure music" placeholder for
  instrumentals are dropped.
- **Lyrics-fit check.** After aligning, mtn checks that the lyrics match the singing. Mean letter confidence was
  0.24-0.66 for correct lyrics (0.20-0.33 for romanised Japanese) and 0.06 for a namesake's. Below 0.15 it warns,
  and `song.json` keeps `lyrics_fit`.
- `--yes` takes the defaults without asking (the best version, saved next to the output), as do piped or scheduled
  runs. Other flags: `--lyrics-source best|lrclib|netease|bugs|genius|ovh` (the old `local` / `web` / `both` still
  work), `--save-lyrics PATH`, `--title`, `--artist`. Sites that answer 429/5xx are retried after 1/3/7/15 s.
- Some of these sites' terms don't allow automated access (Genius forbids scraping). Uta-Net was tried and left out:
  it blocks automated clients. This is for personal lyric videos; keep it out of anything public.

What the words site changes (8 songs from the music folder, LRCLIB alone vs Genius words on LRCLIB timing, same
audio): the timing quality is the same (letter confidence 0.42 vs 0.42, squashed words 27% vs 27%). The gain is
**correct words on screen**: Genius fixed "keep **it** stuck" → "keep **me** stuck", "yikky yeah" → "ye-yeah" and
"hearts" → "heart's" (Stereo Hearts), and "the only **one** worth fighting for" → "the only **war**" (Die With A
Smile).

## Lyrics in any language

The aligner (wav2vec2, trained on English) hears Latin letters. Until v0.4 every other letter was simply dropped, so
Japanese, Russian or Hindi lyrics had nothing to align and their words were spread evenly between neighbours. Now
every word is **romanised for the aligner while the video shows it as written** (`src/roman.cpp`):

| script | romanised by |
|---|---|
| accented Latin (é ñ ü ß), Greek, Cyrillic, Arabic, Hebrew, Thai, Chinese (pinyin) | Unidecode's tables (`models\roman.tsv`) |
| kana | Hepburn, with small kana (きゃ kya), っ (doubled consonant) and ー (long vowel) |
| kanji in Japanese lyrics | pykakasi's dictionary, longest match (`models\ja_kanwa.tsv`): 春の風 → haru no kaze |
| Hangul | Revised Romanisation from the syllable's parts, with liaison (한국어 → hangugeo) |
| Devanagari, Bengali, Gurmukhi, Gujarati, Tamil, Telugu, Kannada, Malayalam | table letters + the inherent "a" (नमस्ते → namaste) |

A letter said twice in the romanisation is listened for once (maataram → mataram): in CTC two equal letters need a
blank frame between them, which the English model hardly gives a long vowel. Plain ASCII words are untouched, so
English timing is bit for bit what it was.

**Spaceless scripts** (Japanese, Chinese, Thai) are cut into word-sized units so the karaoke lights up a few characters
at a time instead of a whole line. Japanese: a kanji/katakana run with its trailing hiragana, with long kana stretches
broken after particles (ひとつに|なれない, 溶けて|ゆくように). Chinese: two characters. The video sets these units flush,
in Windows' Japanese/Chinese/Korean/Indic UI fonts (the look's own fonts are Latin-only).

**Show romanised.** For lyrics that aren't in Latin letters the app offers *Show as written* or *Show romanised*
(`mtn analyze --show roman`); the timing is the same either way. `mtn romanize TEXT|FILE` prints the units, the
aligner's letters and the readable Latin for each line.

Measured (`tools/bench_lang.py`, `tools/bench_lines.py`):

| songs | lyrics given | before | now |
|---|---|---|---|
| 3 Japanese songs from the library (Albireo, Haru no Kaze, Toki no Uta) | plain text, no stamps | lines 65-147 s off (median) | median 0.08 / 0.28 / 0.30 s; 96 / 68 / 70% of lines within 0.5 s of LRCLIB's stamps |
| JamendoLyrics German (3 songs) | plain text | 52 ms mean word error | 50 ms |
| JamendoLyrics Spanish (3 songs) | plain text | 63 ms | 63 ms |
| JamendoLyrics French (3 songs) | plain text | 421 ms (one song's outliers; median 67) | 422 ms |

Latin-script languages already timed well (the old code dropped only the accented letters); the gain is everywhere
else. The Japanese errors are against community line stamps, which run a little early and vary, so they measure "the
line is in the right place" rather than milliseconds. Nirvana Shatakam (Sanskrit, Devanagari) aligns with confidence
0.30. A multilingual aligner (MMS) would add ~300 MB to the installer; with these numbers it isn't needed yet.

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

The romanisation tables too (a venv with `unidecode` and `pykakasi`):

```
roman-venv\Scripts\python tools\gen_lang_data.py %MTN_HOME%\models     # roman.tsv, ja_kanwa.tsv
```

Lyrics outside English (music-tool venv):
```
python tools\bench_lang.py <jamendolyrics> <work> --system old=mtn.exe --system new=mtn_dev.exe   # FR / DE / ES words
python tools\bench_lines.py <work> --song song.mp3=song.lrc --system new=mtn_dev.exe            # line starts vs an .lrc
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
