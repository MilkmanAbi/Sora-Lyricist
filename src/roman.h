// Lyrics in any script: display units and the Latin letters the aligner listens for.
//
// The wav2vec2 aligners hear Latin letters only, so every word gets a romanised spelling for the CTC graph while
// song.json keeps the word as written. Spaceless scripts (Japanese, Chinese, Thai) are cut into word-sized units so
// the karaoke lights up a few characters at a time instead of a whole line.
//   accented Latin, Greek, Cyrillic, Han, Thai, Arabic, ...   models/roman.tsv (Unidecode tables)
//   kana            Hepburn, with small kana (きゃ kya), っ (doubled consonant) and ー (long vowel)
//   kanji in Japanese lyrics   models/ja_kanwa.tsv readings, longest match (as kakasi does)
//   Hangul          Revised Romanisation from the syllable's jamo
//   Indic scripts   table letters + the inherent "a" after a consonant without a vowel sign or virama
#pragma once
#include <string>
#include <vector>

#include "common.h"

namespace mt {

void init_romanizer(const fs::path& models);  // loads the tables once; without them only ASCII and kana are spelled

std::string utf8_text(const std::string& bytes);  // valid UTF-8 as is, else read as Windows-1252 (old .lrc files)
std::u32string to_u32(const std::string& s);
std::string to_u8(const std::u32string& s);

bool has_kana(const std::string& text);       // Japanese: its kanji are read with the Japanese dictionary
bool has_spaceless(const std::string& text);  // Han / kana / Thai: words aren't separated by spaces

// A line's display units: its space-separated words, with runs of a spaceless script cut into short units (Japanese:
// a kanji/katakana run with its trailing hiragana; Chinese: two characters). Punctuation stays with its unit.
std::vector<std::string> split_units(const std::string& line, bool japanese);

// The aligner's letters for one unit: lower-case a-z and apostrophes; '*' marks a sound the tables can't spell.
// Plain ASCII words come back unchanged (so English alignment is exactly what it was).
std::string align_letters(const std::string& word, bool japanese);

// The main script of a text, for people: "Japanese", "Chinese", "Korean", "Cyrillic", "Devanagari", "Latin", ...
std::string script_name(const std::string& text);

// Readable Latin for a whole text, units separated by spaces ("夜に駆ける" -> "yoru ni kakeru"): for comparing titles
// and lines across lyric sites, and for showing lyrics romanised.
std::string romanize(const std::string& text, bool japanese);

}  // namespace mt
