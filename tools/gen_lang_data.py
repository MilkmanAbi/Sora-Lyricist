"""Romanisation data for the aligner (build-time only, like export_models.py).

Usage: <venv with unidecode + pykakasi>/python tools/gen_lang_data.py <models dir>

  roman.tsv     codepoint (hex) -> ASCII spelling, from Unidecode's tables (GPL-2.0+): accented Latin, Greek,
                Cyrillic, Indic, Thai, Arabic, Hebrew, Han (Mandarin pinyin) ... Kana, Hangul and the Indic inherent
                vowel are handled in src/roman.cpp, which does better than character-by-character tables there.
  ja_kanwa.tsv  Japanese kanji words -> hiragana reading, from pykakasi's kanwa dictionary (GPL-3.0+): the
                engine reads kanji by longest match, as kakasi does (春の風 -> はる の かぜ -> haru no kaze).
"""
import os
import pickle
import sys

import pykakasi
from unidecode import unidecode

OUT = sys.argv[1] if len(sys.argv) > 1 else "models"
os.makedirs(OUT, exist_ok=True)

# every block the tables know, minus ASCII and the scripts roman.cpp spells itself (kana 3040-30FF, Hangul)
RANGES = [(0x00A0, 0x2FFF), (0x3000, 0x303F), (0x3100, 0x33FF), (0x3400, 0x4DBF), (0x4E00, 0x9FFF),
          (0xF900, 0xFAFF), (0xFE30, 0xFE4F), (0xFF00, 0xFFEF)]
n = 0
with open(os.path.join(OUT, "roman.tsv"), "w", encoding="ascii", newline="\n") as f:
    f.write("# codepoint\tASCII spelling (Unidecode, GPL-2.0-or-later)\n")
    for lo, hi in RANGES:
        for cp in range(lo, hi + 1):
            if 0xAC00 <= cp <= 0xD7A3:
                continue
            s = unidecode(chr(cp))
            s = "".join(c for c in s if c.isalnum() or c in "' ").strip().lower()
            if s:
                f.write(f"{cp:x}\t{s}\n")
                n += 1
print(f"roman.tsv: {n} characters")

kanwa = pickle.load(open(os.path.join(os.path.dirname(pykakasi.__file__), "data", "kanwadict4.db"), "rb"))
rows = []
for table in kanwa.values():
    for word, readings in table.items():
        yomi = next((y for y, con in readings if con is None), None)  # the first plain reading
        if yomi and "\t" not in word and "\n" not in word:
            rows.append((word, yomi))
rows.sort()
with open(os.path.join(OUT, "ja_kanwa.tsv"), "w", encoding="utf-8", newline="\n") as f:
    f.write("# kanji word\threading (pykakasi kanwadict, GPL-3.0-or-later)\n")
    for w, y in rows:
        f.write(f"{w}\t{y}\n")
print(f"ja_kanwa.tsv: {len(rows)} words")
