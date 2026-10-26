// Lyrics from the web, for songs that come without a lyric file. Every site is asked at once:
//   timed  LRCLIB (community synced lyrics), NetEase Cloud Music (huge Chinese / Japanese / Korean catalogue,
//          synced), Bugs (Korean, synced)
//   words  Genius (carefully edited words and line breaks, many languages), lyrics.ovh (plain words, mostly English)
// The answers are checked against each other (same song, not a namesake), and the best words are put on the best
// timing: lines are matched between the two, matched lines keep the timed site's timestamp, lines only the words site
// has are aligned untimed, and repeats it left out come from the timed site.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "common.h"

namespace mt {

struct SongQuery {
    std::string title, artist;
    double duration = 0;  // seconds, to pick the matching version on the timed sites
};

struct WebLyrics {
    bool found = false;
    std::string id, source;  // "lrclib", "LRCLIB"
    bool timed = false;      // line timestamps
    std::string title, artist, url, error;
    double duration = 0;
    std::vector<std::string> lines;
    std::vector<std::optional<double>> times;
};

struct LyricsChoice {
    std::string source;  // a site's id, "best", or "" (asked interactively); "local"/"web"/"both" still work
    fs::path save;       // where to write the lyrics (asked interactively, default shown)
    bool interactive = true;
    std::string title, artist;  // override what the file's tags / name say
    fs::path versions_dir;      // write every version found here + a JSON summary on stdout (the app's lyric picker)
};

SongQuery guess_query(const fs::path& audio, double duration);
WebLyrics fetch_lrclib(const SongQuery& q);
WebLyrics fetch_netease(const SongQuery& q);
WebLyrics fetch_bugs(const SongQuery& q);
WebLyrics fetch_genius(const SongQuery& q);
WebLyrics fetch_ovh(const SongQuery& q);
std::vector<WebLyrics> fetch_all(const SongQuery& q);  // every site, in parallel
std::vector<std::string> parse_genius_html(const std::string& html);  // exposed for tests

// Asks (or follows `choice`), downloads, merges, saves; returns the saved file (.lrc with timestamps, else .txt).
fs::path get_lyrics(const fs::path& audio, double duration, const fs::path& default_save, LyricsChoice choice);

}  // namespace mt
