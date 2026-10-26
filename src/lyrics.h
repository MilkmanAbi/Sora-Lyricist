// Lyric sheets: synced .lrc (line timestamps become alignment anchors) or plain text (one line per line).
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "common.h"

namespace mt {

struct Lyrics {
    std::vector<std::string> lines;               // text as written (brackets kept)
    std::vector<std::optional<double>> times;     // .lrc timestamp per line, or none
    std::vector<std::string> words;               // display units (roman.h): brackets removed, split on whitespace,
                                                  // spaceless scripts cut into short units
    std::vector<std::string> letters;             // what the aligner listens for, per word (roman.h align_letters)
    std::vector<int> line_of;                     // line index of each word
    bool japanese = false;                        // kana present: kanji are read the Japanese way
    std::string raw;                              // file text without time tags (song.json "lyrics_used")
    bool synced() const;
};

Lyrics load_lyrics(const fs::path& p);

}  // namespace mt
