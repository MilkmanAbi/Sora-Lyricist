// Lyrics from the web, for songs that come without a lyric file:
//   local  LRCLIB synced lyrics (what the Python tool uses): line timestamps, community-typed words
//   web    Genius (the lyric-breakdown site): carefully edited words and line breaks, no timing
//   both   Genius words on LRCLIB timing: lines are matched between the two, matched lines keep LRCLIB's timestamp,
//          lines only Genius has are aligned untimed, and repeats Genius left out come from LRCLIB
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "common.h"

namespace mt {

struct SongQuery {
    std::string title, artist;
    double duration = 0;  // seconds, to pick the matching version on LRCLIB
};

struct WebLyrics {
    bool found = false;
    std::string source, title, artist, url, error;
    double duration = 0;
    std::vector<std::string> lines;
    std::vector<std::optional<double>> times;
};

struct LyricsChoice {
    std::string source;  // "local" | "web" | "both" | "" (asked interactively)
    fs::path save;       // where to write the lyrics (asked interactively, default shown)
    bool interactive = true;
    std::string title, artist;  // override what the file's tags / name say
    fs::path versions_dir;      // write every version found here + a JSON summary on stdout (the app's lyric picker)
};

SongQuery guess_query(const fs::path& audio, double duration);
WebLyrics fetch_lrclib(const SongQuery& q);
WebLyrics fetch_genius(const SongQuery& q);
std::vector<std::string> parse_genius_html(const std::string& html);  // exposed for tests

// Asks (or follows `choice`), downloads, merges, saves; returns the saved file (.lrc with timestamps, else .txt).
fs::path get_lyrics(const fs::path& audio, double duration, const fs::path& default_save, LyricsChoice choice);

}  // namespace mt
