#include "lyrics_web.h"

#include <io.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <regex>
#include <sstream>

#include "audio.h"
#include "http.h"
#include "json.hpp"

namespace mt {

using json = nlohmann::json;

// ------------------------------------------------------------------ text helpers

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// words for comparing lyrics: lower case, letters/digits/apostrophes only (curly quotes folded)
static std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 &&
            ((unsigned char)s[i + 2] == 0x98 || (unsigned char)s[i + 2] == 0x99)) {
            i += 2;
            continue;  // apostrophes are dropped: "don't" == "dont"
        }
        if (std::isalnum(c) || c >= 0x80) cur += (char)std::tolower(c);
        else if (c == '\'') continue;
        else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// 2 * LCS / (|a| + |b|): 1 = same words in the same order
static double similarity(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() || b.empty()) return 0;
    std::vector<int> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
    for (size_t i = 1; i <= a.size(); i++) {
        for (size_t j = 1; j <= b.size(); j++)
            cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 : std::max(prev[j], cur[j - 1]);
        std::swap(prev, cur);
    }
    return 2.0 * prev[b.size()] / (a.size() + b.size());
}

static std::string decode_entities(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            o += s[i];
            continue;
        }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) {
            o += s[i];
            continue;
        }
        std::string ent = s.substr(i + 1, e - i - 1);
        unsigned long cp = 0;
        if (ent == "amp") cp = '&';
        else if (ent == "lt") cp = '<';
        else if (ent == "gt") cp = '>';
        else if (ent == "quot") cp = '"';
        else if (ent == "apos") cp = '\'';
        else if (ent == "nbsp") cp = ' ';
        else if (!ent.empty() && ent[0] == '#')
            cp = std::strtoul(ent.c_str() + 1 + (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')),
                              nullptr, ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? 16 : 10);
        if (!cp) {
            o += s[i];
            continue;
        }
        if (cp < 0x80) o += (char)cp;  // to UTF-8
        else if (cp < 0x800) o += {(char)(0xC0 | (cp >> 6)), (char)(0x80 | (cp & 0x3F))};
        else if (cp < 0x10000)
            o += {(char)(0xE0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F))};
        else
            o += {(char)(0xF0 | (cp >> 18)), (char)(0x80 | ((cp >> 12) & 0x3F)), (char)(0x80 | ((cp >> 6) & 0x3F)),
                  (char)(0x80 | (cp & 0x3F))};
        i = e;
    }
    return o;
}

// end of the <div> element that opens at `open` (nested divs counted)
static size_t div_end(const std::string& h, size_t open) {
    int depth = 0;
    size_t p = open;
    while (p < h.size()) {
        size_t a = h.find("<div", p), b = h.find("</div>", p);
        if (b == std::string::npos) return h.size();
        if (a != std::string::npos && a < b) {
            depth++;
            p = a + 4;
        } else {
            if (--depth == 0) return b + 6;
            p = b + 6;
        }
    }
    return h.size();
}

static std::string mmss(double s) {
    char b[16];
    snprintf(b, sizeof b, "%d:%02d", (int)s / 60, (int)s % 60);
    return b;
}

// ------------------------------------------------------------------ song identity

SongQuery guess_query(const fs::path& audio, double duration) {
    SongQuery q;
    q.duration = duration;
    auto tags = read_tags(audio);
    q.title = tags.count("title") ? trim(tags["title"]) : "";
    q.artist = tags.count("artist") ? trim(tags["artist"]) : (tags.count("album_artist") ? trim(tags["album_artist"]) : "");
    if (q.title.empty()) {  // "Die_With_A_Smile.mp3" -> "Die With A Smile"
        std::string t = utf8(audio.stem());
        for (char& c : t)
            if (c == '_') c = ' ';
        q.title = trim(std::regex_replace(t, std::regex(R"(\s*[\(\[](official|lyrics?|lyric video|audio|hd|hq)[^\)\]]*[\)\]])",
                                                        std::regex::icase), ""));
    }
    return q;
}

// ------------------------------------------------------------------ LRCLIB

WebLyrics fetch_lrclib(const SongQuery& q) {
    WebLyrics w;
    w.source = "LRCLIB";
    std::string url = "https://lrclib.net/api/search?track_name=" + url_encode(q.title);
    if (!q.artist.empty()) url += "&artist_name=" + url_encode(q.artist);
    HttpResult r = http_get(url);
    if (r.status != 200) {
        w.error = r.status ? "LRCLIB answered " + std::to_string(r.status) : "can't reach LRCLIB";
        return w;
    }
    json hits = json::parse(r.body, nullptr, false);
    if (!hits.is_array() || hits.empty()) {
        w.error = "not on LRCLIB";
        return w;
    }
    // synced first, then the version whose length matches this file (slowed / sped-up / extended uploads differ)
    const json* best = nullptr;
    double best_cost = 1e9;
    for (auto& h : hits) {
        bool synced = h.contains("syncedLyrics") && h["syncedLyrics"].is_string();
        if (h.value("instrumental", false)) continue;
        // LRCLIB's search also matches words anywhere ("Adventure Time" -> "Island Song (Adventure Time ...)")
        if (similarity(tokens(q.title), tokens(h.value("trackName", ""))) < 0.5) continue;
        double d = h.contains("duration") && h["duration"].is_number() ? h["duration"].get<double>() : 0;
        double cost = (synced ? 0 : 1000) + (q.duration > 0 && d > 0 ? std::fabs(d - q.duration) : 30);
        if (cost < best_cost) best_cost = cost, best = &h;
    }
    if (!best) {
        w.error = "only instrumental entries on LRCLIB";
        return w;
    }
    w.title = best->value("trackName", "");
    w.artist = best->value("artistName", "");
    w.duration = (*best)["duration"].is_number() ? (*best)["duration"].get<double>() : 0;
    std::string text = (*best)["syncedLyrics"].is_string() ? (*best)["syncedLyrics"].get<std::string>()
                                                            : best->value("plainLyrics", "");
    static const std::regex tag(R"(\[(\d+):(\d+(?:\.\d+)?)\])"), any_tag(R"(\[[^\]]*\])");
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        std::smatch m;
        std::optional<double> t;
        if (std::regex_search(line, m, tag)) t = std::stod(m[1]) * 60 + std::stod(m[2]);
        std::string body = trim(std::regex_replace(line, any_tag, ""));
        if (body.empty()) continue;
        w.lines.push_back(body);
        w.times.push_back(t);
    }
    w.found = !w.lines.empty();
    if (!w.found) w.error = "empty lyrics on LRCLIB";
    return w;
}

// ------------------------------------------------------------------ Genius

std::vector<std::string> parse_genius_html(const std::string& h) {
    std::string text;
    size_t p = 0;
    while ((p = h.find("data-lyrics-container=\"true\"", p)) != std::string::npos) {
        size_t open = h.rfind("<div", p), end = div_end(h, open);
        std::string inner = h.substr(open, end - open);
        // drop the "N Contributors / Translations / Song title Lyrics" header and other non-lyric blocks
        size_t x;
        while ((x = inner.find("data-exclude-from-selection=\"true\"")) != std::string::npos) {
            size_t o = inner.rfind("<div", x);
            inner.erase(o, div_end(inner, o) - o);
        }
        inner = std::regex_replace(inner, std::regex(R"(<br\s*/?>)", std::regex::icase), "\n");
        inner = std::regex_replace(inner, std::regex(R"(<[^>]*>)"), "");
        text += decode_entities(inner) + "\n";
        p = end;
    }
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string line;
    static const std::regex section(R"(^\[.*\]$)"), times(R"(\s*[\(\[]?\s*[x×]\s*(\d)\s*[\)\]]?\s*$)", std::regex::icase);
    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.empty() || std::regex_match(line, section)) continue;  // [Chorus: Bruno Mars] etc.
        std::smatch m;
        int rep = 1;  // "line (x2)" -> the line twice
        if (std::regex_search(line, m, times) && m.position(0) > 0) {
            rep = std::clamp(m[1].str()[0] - '0', 1, 4);
            line = trim(line.substr(0, m.position(0)));
        }
        for (int k = 0; k < rep; k++) lines.push_back(line);
    }
    return lines;
}

// "Lady Gaga x Bruno Mars", "A & B", "A feat. B", "A, B" -> "Lady Gaga"
static std::string first_artist(const std::string& a) {
    static const std::regex sep(R"(\s+(x|&|and|feat\.?|ft\.?|with|vs\.?)\s+|,|;|/)", std::regex::icase);
    std::smatch m;
    return std::regex_search(a, m, sep) ? trim(a.substr(0, m.position(0))) : a;
}

WebLyrics fetch_genius(const SongQuery& q) {
    WebLyrics w;
    w.source = "Genius";
    // best song hit: title words match, artist matches if known; skip translations / instrumentals / remixes
    // unless the file name asks for them. Genius' search is literal ("Lady Gaga x Bruno Mars" finds nothing), so
    // the query gets simpler until a hit fits.
    static const std::regex variant(R"(instrumental|remix|translation|romanized|cover|live|sped|slowed|karaoke)",
                                    std::regex::icase);
    const bool want_variant = std::regex_search(q.title, variant);
    auto qt = tokens(q.title), qa = tokens(q.artist);
    std::vector<std::string> queries{q.title + " " + q.artist};
    if (first_artist(q.artist) != q.artist) queries.push_back(q.title + " " + first_artist(q.artist));
    if (!q.artist.empty()) queries.push_back(q.title);
    json best;
    double best_score = -1e9;
    for (auto& query : queries) {
        HttpResult r = http_get("https://genius.com/api/search/multi?q=" + url_encode(trim(query)));
        if (r.status != 200) {
            w.error = r.status ? "Genius answered " + std::to_string(r.status) : "can't reach Genius";
            return w;
        }
        json d = json::parse(r.body, nullptr, false);
        if (d.is_discarded() || !d.contains("response")) continue;
        int rank = 0;
        for (auto& sec : d["response"]["sections"]) {
            std::string type = sec.value("type", "");
            if (type != "song" && type != "top_hit") continue;
            for (auto& h : sec["hits"]) {
                if (h.value("type", "") != "song") continue;
                const json& s = h["result"];
                std::string title = s.value("title", ""), full = s.value("full_title", "");
                std::string artist = s.contains("primary_artist") ? s["primary_artist"].value("name", "") : "";
                double ts = similarity(qt, tokens(title));
                // a different song; with no artist to check against, the title has to match more closely
                if (ts < (qa.empty() ? 0.8 : 0.6)) continue;
                double score = 2.0 * ts - 0.05 * rank++;
                if (!qa.empty()) score += similarity(tokens(first_artist(q.artist)), tokens(artist + " " + full));
                if (!want_variant && std::regex_search(full, variant)) score -= 1.5;
                if (score > best_score) best_score = score, best = s;
            }
        }
        if (best_score >= (qa.empty() ? 1.9 : 2.5)) break;  // title and artist both fit: no need to look further
    }
    if (best.is_null() || best_score < 1.2) {
        w.error = "no matching song on Genius";
        return w;
    }
    const json* bp = &best;
    w.url = bp->value("url", "");
    w.title = bp->value("title", "");
    w.artist = bp->contains("primary_artist") ? (*bp)["primary_artist"].value("name", "") : "";
    HttpResult page = http_get(w.url);
    if (page.status != 200) {
        w.error = "Genius lyrics page answered " + std::to_string(page.status);
        return w;
    }
    w.lines = parse_genius_html(page.body);
    w.times.assign(w.lines.size(), std::nullopt);
    w.found = !w.lines.empty();
    if (!w.found) w.error = "no lyrics on the Genius page (instrumental, or the page layout changed)";
    return w;
}

// ------------------------------------------------------------------ merge

struct Merged {
    std::vector<std::string> lines;
    std::vector<std::optional<double>> times;
    int matched = 0, changed = 0, genius_only = 0, lrclib_only = 0;
    std::vector<std::pair<std::string, std::string>> examples;  // (Genius, LRCLIB) for lines whose words differ
};

// Line-level alignment (Needleman-Wunsch on word similarity). Moves: one line each (1:1), two Genius lines for one
// LRCLIB line (2:1: Genius breaks "If the world was ending / I'd wanna be next to you" where LRCLIB has one line), one
// Genius line for two LRCLIB lines (1:2), or a line on one side only. Matched lines take Genius' words; the first
// line of each match takes LRCLIB's timestamp, a second Genius line of a 2:1 match is aligned freely.
static Merged merge(const WebLyrics& g, const WebLyrics& l) {
    const size_t G = g.lines.size(), Ln = l.lines.size();
    std::vector<std::vector<std::string>> gt(G), lt(Ln);
    for (size_t i = 0; i < G; i++) gt[i] = tokens(g.lines[i]);
    for (size_t j = 0; j < Ln; j++) lt[j] = tokens(l.lines[j]);
    auto cat = [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
        std::vector<std::string> c = a;
        c.insert(c.end(), b.begin(), b.end());
        return c;
    };
    std::vector<std::vector<double>> S(G + 1, std::vector<double>(Ln + 1, 0)), sim(G, std::vector<double>(Ln, 0));
    std::vector<std::vector<char>> how(G + 1, std::vector<char>(Ln + 1, 0));
    for (size_t i = 1; i <= G; i++) how[i][0] = 'g';
    for (size_t j = 1; j <= Ln; j++) how[0][j] = 'l';
    for (size_t i = 1; i <= G; i++)
        for (size_t j = 1; j <= Ln; j++) {
            sim[i - 1][j - 1] = similarity(gt[i - 1], lt[j - 1]);
            auto take = [&](double v, char c) {
                if (v > S[i][j]) S[i][j] = v, how[i][j] = c;
            };
            S[i][j] = S[i - 1][j], how[i][j] = 'g';
            take(S[i][j - 1], 'l');
            if (sim[i - 1][j - 1] >= 0.5) take(S[i - 1][j - 1] + sim[i - 1][j - 1], 'm');
            // a match covering three lines counts for 1.5: it beats a half-line 1:1 match plus a gap
            if (i >= 2) {
                double v = similarity(cat(gt[i - 2], gt[i - 1]), lt[j - 1]);
                if (v >= 0.7) take(S[i - 2][j - 1] + 1.5 * v, '2');
            }
            if (j >= 2) {
                double v = similarity(gt[i - 1], cat(lt[j - 2], lt[j - 1]));
                if (v >= 0.7) take(S[i - 1][j - 2] + 1.5 * v, '3');
            }
        }
    // Genius writes background ad-libs in brackets ("Make me your radio (Yeah, ha)"). Where LRCLIB, synced to the
    // recording, left them out they are quiet backing vocals: forcing the aligner to place them pulls the lead line's
    // timing (Stereo Hearts: line error 300 -> 480 ms), so a bracket whose words LRCLIB doesn't have is dropped.
    auto keep_sung = [](const std::string& gl, const std::vector<std::string>& ltok) {
        static const std::regex paren(R"(\s*\(([^)]*)\))");
        std::string out;
        size_t last = 0;
        for (auto it = std::sregex_iterator(gl.begin(), gl.end(), paren); it != std::sregex_iterator(); ++it) {
            auto pt = tokens((*it)[1]);
            size_t have = 0;
            for (auto& t : pt) have += std::find(ltok.begin(), ltok.end(), t) != ltok.end();
            out += gl.substr(last, it->position(0) - last);
            if (!pt.empty() && have * 2 >= pt.size()) out += it->str(0);
            last = it->position(0) + it->length(0);
        }
        out += gl.substr(last);
        return out.find_first_not_of(" \t") == std::string::npos ? gl : out;  // a line that is all ad-lib stays
    };
    Merged m;
    std::vector<std::pair<std::string, std::optional<double>>> rev;
    auto note = [&](double v, const std::string& gs, const std::string& ls) {
        m.matched++;
        if (v < 0.999) {
            m.changed++;
            if (m.examples.size() < 64) m.examples.push_back({gs, ls});
        }
    };
    for (size_t i = G, j = Ln; i || j;) {
        char c = how[i][j];
        if (c == 'm') {
            rev.push_back({keep_sung(g.lines[i - 1], lt[j - 1]), l.times[j - 1]});
            note(sim[i - 1][j - 1], g.lines[i - 1], l.lines[j - 1]);
            i--, j--;
        } else if (c == '2') {  // pushed in reverse: second Genius line (untimed), then the first (LRCLIB's time)
            rev.push_back({keep_sung(g.lines[i - 1], lt[j - 1]), std::nullopt});
            rev.push_back({keep_sung(g.lines[i - 2], lt[j - 1]), l.times[j - 1]});
            note(similarity(cat(gt[i - 2], gt[i - 1]), lt[j - 1]), g.lines[i - 2] + " / " + g.lines[i - 1], l.lines[j - 1]);
            i -= 2, j--;
        } else if (c == '3') {
            rev.push_back({keep_sung(g.lines[i - 1], cat(lt[j - 2], lt[j - 1])), l.times[j - 2]});
            note(similarity(gt[i - 1], cat(lt[j - 2], lt[j - 1])), g.lines[i - 1], l.lines[j - 2] + " / " + l.lines[j - 1]);
            i--, j -= 2;
        } else if (c == 'g') {
            rev.push_back({g.lines[i - 1], std::nullopt});
            m.genius_only++;
            i--;
        } else {
            // LRCLIB has a line Genius doesn't (often a repeat Genius wrote once): keep it, in Genius' spelling if
            // Genius has the same line elsewhere
            std::string text = l.lines[j - 1];
            for (size_t k = 0; k < G; k++)
                if (sim[k][j - 1] >= 0.8) {
                    text = g.lines[k];
                    break;
                }
            rev.push_back({text, l.times[j - 1]});
            m.lrclib_only++;
            j--;
        }
    }
    std::reverse(rev.begin(), rev.end());
    std::reverse(m.examples.begin(), m.examples.end());
    for (auto& [t, s] : rev) m.lines.push_back(t), m.times.push_back(s);
    return m;
}

// ------------------------------------------------------------------ files + prompt

static std::string lrc_stamp(double t) {
    char b[32];
    snprintf(b, sizeof b, "[%02d:%05.2f]", (int)(t / 60), std::fmod(t, 60.0));
    return b;
}

// .lrc when any line is timed (untimed lines are written without a stamp; mtn aligns them freely), else .txt
static fs::path save_lyrics(fs::path dst, const std::vector<std::string>& lines,
                            const std::vector<std::optional<double>>& times, const std::string& header) {
    bool timed = std::any_of(times.begin(), times.end(), [](auto& t) { return t.has_value(); });
    dst.replace_extension(timed ? ".lrc" : ".txt");
    std::string out = timed ? header : "";
    for (size_t i = 0; i < lines.size(); i++) out += (times[i] ? lrc_stamp(*times[i]) : "") + lines[i] + "\n";
    write_text(dst, out);
    return dst;
}

static std::string ask(const std::string& question, const std::string& def) {
    fprintf(stderr, "%s [%s]: ", question.c_str(), def.c_str());
    fflush(stderr);
    std::string a;
    if (!std::getline(std::cin, a)) return def;
    a = trim(a);
    return a.empty() ? def : a;
}


fs::path get_lyrics(const fs::path& audio, double duration, const fs::path& default_save, LyricsChoice choice) {
    SongQuery q = guess_query(audio, duration);
    if (!choice.title.empty()) q.title = choice.title;
    if (!choice.artist.empty()) q.artist = choice.artist;
    // asks only on a console (or with MTN_ASK=1, to script the prompt); piped / scheduled runs take the defaults
    const bool interactive = choice.interactive && (_isatty(_fileno(stdin)) || getenv("MTN_ASK"));
    if (interactive && choice.title.empty()) {
        fprintf(stderr, "\nLyrics for %s (%s)\n", utf8(audio.filename()).c_str(), mmss(duration).c_str());
        q.title = ask("  song title", q.title);
        q.artist = ask("  artist (helps find the right song)", q.artist.empty() ? "-" : q.artist);
        if (q.artist == "-") q.artist.clear();
    }
    log("looking up lyrics: \"%s\"%s%s", q.title.c_str(), q.artist.empty() ? "" : " by ", q.artist.c_str());
    WebLyrics local = fetch_lrclib(q), web = fetch_genius(q);
    if (local.found)
        log("  LRCLIB: \"%s\" by %s (%s), %zu lines", local.title.c_str(), local.artist.c_str(), mmss(local.duration).c_str(),
            local.lines.size());
    else
        log("  LRCLIB: %s", local.error.c_str());
    if (web.found)
        log("  Genius: \"%s\" by %s, %zu lines (%s)", web.title.c_str(), web.artist.c_str(), web.lines.size(), web.url.c_str());
    else
        log("  Genius: %s", web.error.c_str());
    if (!local.found && !web.found)
        die("no lyrics found (LRCLIB: %s; Genius: %s). Pass --lyrics FILE or --lrc FILE, or try --title/--artist.",
            local.error.c_str(), web.error.c_str());
    Merged both;
    bool can_merge = false;
    if (local.found && web.found) {
        both = merge(web, local);
        // the two must be the same song: title searches without an artist can land on a namesake ("Fireworks" ->
        // Drake on Genius, DAOKO on LRCLIB). Then LRCLIB wins if its length matches the file, else Genius.
        double frac = (double)both.matched / std::min(web.lines.size(), local.lines.size());
        can_merge = frac >= 0.5;
        if (!can_merge) {
            bool lrclib_fits = local.duration > 0 && std::fabs(local.duration - duration) <= 10;
            log("  LRCLIB and Genius look like different songs (%.0f%% of lines match): using %s", frac * 100,
                lrclib_fits ? "LRCLIB (its length matches this file)" : "Genius");
            if (lrclib_fits) web.found = false, web.error = "a different song than LRCLIB's (" + web.title + " by " + web.artist + ")";
            else local.found = false, local.error = "a different song than Genius' (" + local.title + " by " + local.artist + ")";
        }
    }

    if (interactive) {
        fprintf(stderr, "\n");
        if (local.found)
            fprintf(stderr, "  [1] local  LRCLIB: \"%s\" by %s (%s), %zu lines, %s\n", local.title.c_str(),
                    local.artist.c_str(), mmss(local.duration).c_str(), local.lines.size(),
                    local.times.empty() || !local.times[0] ? "no timestamps" : "synced");
        else
            fprintf(stderr, "  [1] local  - (%s)\n", local.error.c_str());
        if (web.found)
            fprintf(stderr, "  [2] web    Genius: \"%s\" by %s, %zu lines (edited words, no timing)\n         %s\n",
                    web.title.c_str(), web.artist.c_str(), web.lines.size(), web.url.c_str());
        else
            fprintf(stderr, "  [2] web    - (%s)\n", web.error.c_str());
        if (local.found && web.found) {
            fprintf(stderr, "  [3] both   Genius words on LRCLIB timing: %d lines matched (%d with different words), "
                            "%d only on Genius, %d only on LRCLIB\n",
                    both.matched, both.changed, both.genius_only, both.lrclib_only);
            for (size_t k = 0; k < std::min<size_t>(3, both.examples.size()); k++)
                fprintf(stderr, "         Genius \"%s\"\n         LRCLIB \"%s\"\n", both.examples[k].first.c_str(),
                        both.examples[k].second.c_str());
        }
        if (q.artist.empty())
            fprintf(stderr, "  note: no artist known - check these are the right song (or rerun with --artist)\n");
        if (local.found && local.duration > 0 && std::fabs(local.duration - duration) > 5)
            fprintf(stderr, "  note: the LRCLIB version is %s long, this file %s - its timestamps are treated as hints\n",
                    mmss(local.duration).c_str(), mmss(duration).c_str());
        std::string def = (local.found && web.found) ? "3" : local.found ? "1" : "2";
        std::string a = ask("\nWhich lyrics? 1 = local, 2 = web, 3 = both", def);
        choice.source = a == "1" || a == "local" ? "local" : a == "2" || a == "web" ? "web" : "both";
    }
    std::string src = choice.source.empty() ? "both" : choice.source;
    if (src == "both" && !(local.found && web.found)) src = local.found ? "local" : "web";
    if (src == "local" && !local.found) die("no LRCLIB lyrics (%s)", local.error.c_str());
    if (src == "web" && !web.found) die("no Genius lyrics (%s)", web.error.c_str());

    if (!choice.versions_dir.empty()) {
        // for the app: every version that exists, written side by side, and a summary on stdout to choose from
        fs::create_directories(choice.versions_dir);
        std::string header = "[ti:" + q.title + "]\n" + (q.artist.empty() ? "" : "[ar:" + q.artist + "]\n");
        json s = {{"query", {{"title", q.title}, {"artist", q.artist}}}, {"duration", duration}};
        auto side = [&](const WebLyrics& w, const char* name) {
            json j = {{"found", w.found}, {"title", w.title}, {"artist", w.artist}, {"duration", w.duration},
                      {"lines", w.lines.size()}, {"url", w.url}, {"error", w.error}};
            if (w.found) j["file"] = utf8(save_lyrics(choice.versions_dir / name, w.lines, w.times, header));
            return j;
        };
        s["local"] = side(local, "local");
        s["web"] = side(web, "web");
        s["both"] = {{"found", local.found && web.found}};
        if (local.found && web.found) {
            json ex = json::array();
            for (auto& [gs, ls] : both.examples) ex.push_back({gs, ls});
            s["both"] = {{"found", true}, {"matched", both.matched}, {"changed", both.changed},
                         {"genius_only", both.genius_only}, {"lrclib_only", both.lrclib_only}, {"examples", ex},
                         {"file", utf8(save_lyrics(choice.versions_dir / "both", both.lines, both.times,
                                                   header + "[re:Genius words + LRCLIB timing (mtn)]\n"))}};
        }
        s["recommended"] = src;
        printf("%s\n", s.dump().c_str());
        fflush(stdout);
        return fs::u8path(s[src]["file"].get<std::string>());
    }
    fs::path save = choice.save.empty() ? default_save : choice.save;
    if (interactive && choice.save.empty()) {
        fs::path shown = save;
        shown.replace_extension(src == "web" ? ".txt" : ".lrc");
        save = fs::u8path(ask("Save lyrics to", utf8(shown)));
    }
    if (fs::is_directory(save)) save /= audio.stem();
    std::string header = "[ti:" + q.title + "]\n" + (q.artist.empty() ? "" : "[ar:" + q.artist + "]\n");
    fs::path out;
    if (src == "local") {
        header += "[re:LRCLIB]\n";
        out = save_lyrics(save, local.lines, local.times, header);
    } else if (src == "web") {
        out = save_lyrics(save, web.lines, web.times, header);
    } else {
        header += "[re:Genius words + LRCLIB timing (mtn)]\n";
        out = save_lyrics(save, both.lines, both.times, header);
    }
    log("lyrics (%s): %s", src.c_str(), utf8(out).c_str());
    return out;
}

}  // namespace mt
