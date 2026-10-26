#include "lyrics_web.h"

#include <io.h>

#include <algorithm>
#include <cmath>
#include <future>
#include <iostream>
#include <regex>
#include <sstream>

#include "audio.h"
#include "http.h"
#include "json.hpp"
#include "roman.h"

namespace mt {

using json = nlohmann::json;

// ------------------------------------------------------------------ text helpers

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// words for comparing lyrics: lower case, accents folded, letters/digits only (apostrophes dropped: "don't" ==
// "dont"); scripts without spaces give one token per character, so two sites' Japanese lines compare letter by letter
static std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    std::u32string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        std::string w = to_u8(cur);
        bool ascii = std::all_of(w.begin(), w.end(), [](char c) { return (unsigned char)c < 0x80; });
        if (!ascii) w = align_letters(w, false);
        w.erase(std::remove_if(w.begin(), w.end(), [](char c) { return c == '\'' || c == '*'; }), w.end());
        for (char& c : w) c = (char)std::tolower((unsigned char)c);
        if (!w.empty()) out.push_back(w);
        cur.clear();
    };
    for (char32_t c : to_u32(s)) {
        if (c == '\'' || c == 0x2018 || c == 0x2019) continue;
        std::string one = to_u8(std::u32string(1, c));
        if (has_spaceless(one)) {
            flush();
            out.push_back(one);
        } else if (c < 0x80 ? std::isalnum((int)c) != 0 : (c >= 0xC0 && !(c >= 0x2000 && c <= 0x2BFF) && !(c >= 0x3000 && c <= 0x303F) &&
                                                             !(c >= 0xFF00 && c <= 0xFF20))) {
            cur += c;
        } else {
            flush();
        }
    }
    flush();
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

static std::vector<std::string> letters_of(const std::string& s) {  // "harunokaze" -> h,a,r,u,...
    std::vector<std::string> v;
    for (char c : s)
        if (std::isalnum((unsigned char)c)) v.push_back(std::string(1, c));
    return v;
}

// titles across scripts: the file says "Haru no Kaze", the site "春の風". Same-script titles compare word by word;
// when either side isn't plain ASCII, their romanisations are compared letter by letter too.
static double title_sim(const std::string& a, const std::string& b) {
    double s = similarity(tokens(a), tokens(b));
    auto ascii = [](const std::string& x) { return std::all_of(x.begin(), x.end(), [](char c) { return (unsigned char)c < 0x80; }); };
    if (!ascii(a) || !ascii(b))
        for (bool ja : {true, false}) s = std::max(s, similarity(letters_of(romanize(a, ja)), letters_of(romanize(b, ja))));
    return s;
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
        o += to_u8(std::u32string(1, (char32_t)cp));
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

// "Lady Gaga x Bruno Mars", "A & B", "A feat. B", "A, B" -> "Lady Gaga"
static std::string first_artist(const std::string& a) {
    static const std::regex sep(R"(\s+(x|&|and|feat\.?|ft\.?|with|vs\.?)\s+|,|;|/|、)", std::regex::icase);
    std::smatch m;
    return std::regex_search(a, m, sep) ? trim(a.substr(0, m.position(0))) : a;
}

// how well a site's hit fits the query: title (x2), artist, and length when both are known
static double hit_score(const SongQuery& q, const std::string& title, const std::string& artist, double duration) {
    double s = 2.0 * title_sim(q.title, title);
    if (!q.artist.empty()) s += std::max(title_sim(first_artist(q.artist), artist), title_sim(q.artist, artist));
    if (q.duration > 0 && duration > 0) s += std::fabs(duration - q.duration) <= 5 ? 0.5 : std::fabs(duration - q.duration) <= 15 ? 0.2 : -0.3;
    return s;
}

// Timed lyric text ([mm:ss.xx]line) -> lines and times. A line with several stamps is sung several times. Credits that
// the Chinese sites put at the top as timed lines ("作词 : Ayase", "Composer: ...") are dropped, as are their
// "pure music, please enjoy" placeholders (an instrumental).
static void parse_lrc(const std::string& text, WebLyrics& w) {
    static const std::regex tag(R"(\[(\d+):(\d+(?:\.\d+)?)\])"), any_tag(R"(\[[^\]]*\])");
    // "作词 : Ayase", "Composer：X": a short label, a colon (either width), then a name
    auto credit = [](const std::string& body) {
        std::u32string u = to_u32(body);
        size_t c = u.find_first_of(U":：");
        if (c == std::u32string::npos || c == 0 || c > 14) return false;
        size_t head = 0;
        for (size_t i = 0; i < c; i++) head += u[i] != ' ';
        return head >= 1 && head <= 10 && u.find_first_not_of(U" ", c + 1) != std::u32string::npos;
    };
    struct Row {
        double t;
        std::string text;
    };
    std::vector<Row> rows;
    bool lyric_seen = false;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        std::vector<double> ts;
        for (auto it = std::sregex_iterator(line.begin(), line.end(), tag); it != std::sregex_iterator(); ++it)
            ts.push_back(std::stod((*it)[1]) * 60 + std::stod((*it)[2]));
        std::string body = trim(std::regex_replace(line, any_tag, ""));
        if (body.empty()) continue;
        if (!lyric_seen && credit(body)) continue;
        lyric_seen = true;
        if (ts.empty()) rows.push_back({-1, body});
        for (double t : ts) rows.push_back({t, body});
    }
    if (std::all_of(rows.begin(), rows.end(), [](const Row& r) { return r.t >= 0; }))
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t < b.t; });
    for (auto& r : rows) {
        w.lines.push_back(r.text);
        w.times.push_back(r.t >= 0 ? std::optional<double>(r.t) : std::nullopt);
    }
    w.timed = std::any_of(w.times.begin(), w.times.end(), [](auto& t) { return t.has_value(); });
    if (w.lines.size() <= 2 && !w.lines.empty() &&
        (w.lines[0].find("纯音乐") != std::string::npos || w.lines[0].find("純音樂") != std::string::npos)) {
        w.lines.clear(), w.times.clear();
        w.error = "an instrumental on " + w.source;
    }
}

static void plain_lines(const std::string& text, WebLyrics& w) {
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (!line.empty()) w.lines.push_back(line), w.times.push_back(std::nullopt);
    }
}

static bool http_ok(const HttpResult& r, WebLyrics& w) {
    if (r.status == 200) return true;
    w.error = r.status ? w.source + " answered " + std::to_string(r.status) : "can't reach " + w.source;
    return false;
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

// ------------------------------------------------------------------ timed sites

WebLyrics fetch_lrclib(const SongQuery& q) {
    WebLyrics w;
    w.id = "lrclib", w.source = "LRCLIB";
    std::string url = "https://lrclib.net/api/search?track_name=" + url_encode(q.title);
    if (!q.artist.empty()) url += "&artist_name=" + url_encode(q.artist);
    HttpResult r = http_get(url);
    if (!http_ok(r, w)) return w;
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
        if (title_sim(q.title, h.value("trackName", "")) < 0.5) continue;
        double d = h.contains("duration") && h["duration"].is_number() ? h["duration"].get<double>() : 0;
        double cost = (synced ? 0 : 1000) + (q.duration > 0 && d > 0 ? std::fabs(d - q.duration) : 30);
        if (cost < best_cost) best_cost = cost, best = &h;
    }
    if (!best) {
        w.error = "only instrumental or other songs on LRCLIB";
        return w;
    }
    w.title = best->value("trackName", "");
    w.artist = best->value("artistName", "");
    w.duration = (*best)["duration"].is_number() ? (*best)["duration"].get<double>() : 0;
    w.url = "https://lrclib.net/api/get/" + std::to_string(best->value("id", 0));
    std::string text = (*best)["syncedLyrics"].is_string() ? (*best)["syncedLyrics"].get<std::string>()
                                                            : best->value("plainLyrics", "");
    parse_lrc(text, w);
    w.found = !w.lines.empty();
    if (!w.found && w.error.empty()) w.error = "empty lyrics on LRCLIB";
    return w;
}

// NetEase Cloud Music: the biggest catalogue of synced Chinese, Japanese and Korean lyrics (plenty of Western songs too)
WebLyrics fetch_netease(const SongQuery& q) {
    WebLyrics w;
    w.id = "netease", w.source = "NetEase";
    std::vector<std::string> queries{q.title + " " + q.artist};
    if (!q.artist.empty()) queries.push_back(q.title);
    json best;
    double best_score = -1e9;
    for (auto& query : queries) {
        HttpResult r = http_get("https://music.163.com/api/search/get?type=1&limit=12&s=" + url_encode(trim(query)));
        if (!http_ok(r, w)) return w;
        json d = json::parse(r.body, nullptr, false);
        if (d.is_discarded() || !d.contains("result") || !d["result"].contains("songs")) continue;
        for (auto& s : d["result"]["songs"]) {
            std::string title = s.value("name", ""), artist;
            for (auto& a : s.value("artists", json::array())) artist += (artist.empty() ? "" : ", ") + a.value("name", "");
            if (title_sim(q.title, title) < (q.artist.empty() ? 0.8 : 0.6)) continue;
            double score = hit_score(q, title, artist, s.value("duration", 0.0) / 1000.0);
            if (score > best_score) best_score = score, best = s;
        }
        if (best_score >= (q.artist.empty() ? 2.3 : 3.0)) break;
    }
    if (best.is_null()) {
        w.error = "not on NetEase";
        return w;
    }
    w.title = best.value("name", "");
    for (auto& a : best.value("artists", json::array())) w.artist += (w.artist.empty() ? "" : ", ") + a.value("name", "");
    w.duration = best.value("duration", 0.0) / 1000.0;
    long long id = best.value("id", 0LL);
    w.url = "https://music.163.com/song?id=" + std::to_string(id);
    HttpResult r = http_get("https://music.163.com/api/song/lyric?lv=1&tv=-1&rv=-1&id=" + std::to_string(id));
    if (!http_ok(r, w)) return w;
    json d = json::parse(r.body, nullptr, false);
    if (d.is_discarded() || d.value("nolyric", false) || d.value("uncollected", false) || !d.contains("lrc")) {
        w.error = "no lyrics on NetEase (instrumental, or none collected)";
        return w;
    }
    parse_lrc(d["lrc"].value("lyric", ""), w);
    w.found = !w.lines.empty();
    if (!w.found && w.error.empty()) w.error = "empty lyrics on NetEase";
    return w;
}

// Bugs: Korean music service with synced lyrics ("16.9|line＃23.6|line")
WebLyrics fetch_bugs(const SongQuery& q) {
    WebLyrics w;
    w.id = "bugs", w.source = "Bugs";
    HttpResult r = http_get("https://music.bugs.co.kr/search/track?q=" + url_encode(trim(q.title + " " + q.artist)));
    if (!http_ok(r, w)) return w;
    static const std::regex row(R"re(trackId="(\d+)"[^>]*rowType="track")re"), title(R"re(class="title"[^>]*>\s*<a[^>]*title="([^"]*)")re"),
        artist(R"re(class="artist"[^>]*>\s*<a[^>]*title="([^"]*)")re");
    std::string bid;
    double best_score = -1e9;
    const std::string& h = r.body;
    for (auto it = std::sregex_iterator(h.begin(), h.end(), row); it != std::sregex_iterator(); ++it) {
        size_t p = (size_t)it->position(0);
        std::string chunk = h.substr(p, 5000);
        std::smatch t, a;
        if (!std::regex_search(chunk, t, title)) continue;
        std::regex_search(chunk, a, artist);
        std::string tt = decode_entities(t[1]), aa = a.size() > 1 ? decode_entities(a[1]) : "";
        if (title_sim(q.title, tt) < (q.artist.empty() ? 0.8 : 0.6)) continue;
        double score = hit_score(q, tt, aa, 0);
        if (score > best_score) best_score = score, bid = (*it)[1], w.title = tt, w.artist = aa;
    }
    if (bid.empty()) {
        w.error = "not on Bugs";
        return w;
    }
    w.url = "https://music.bugs.co.kr/track/" + bid;
    HttpResult l = http_get("https://music.bugs.co.kr/player/lyrics/T/" + bid);
    json d = l.status == 200 ? json::parse(l.body, nullptr, false) : json();
    std::string text = d.is_object() ? d.value("lyrics", "") : "";
    if (!text.empty()) {
        // "16.9|line＃23.6|line" -> lines with stamps
        std::string lrc;
        size_t p = 0;
        const std::string sep = "\xEF\xBC\x83";  // ＃
        while (p < text.size()) {
            size_t e = text.find(sep, p);
            std::string item = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
            size_t bar = item.find('|');
            if (bar != std::string::npos) {
                double t = std::atof(item.substr(0, bar).c_str());
                char st[32];
                snprintf(st, sizeof st, "[%02d:%05.2f]", (int)(t / 60), std::fmod(t, 60.0));
                lrc += st + item.substr(bar + 1) + "\n";
            }
            if (e == std::string::npos) break;
            p = e + sep.size();
        }
        parse_lrc(lrc, w);
    } else {
        HttpResult n = http_get("https://music.bugs.co.kr/player/lyrics/N/" + bid);
        json dn = n.status == 200 ? json::parse(n.body, nullptr, false) : json();
        plain_lines(dn.is_object() ? dn.value("lyrics", "") : "", w);
    }
    w.found = !w.lines.empty();
    if (!w.found && w.error.empty()) w.error = "no lyrics on Bugs";
    return w;
}

// ------------------------------------------------------------------ word sites

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

WebLyrics fetch_genius(const SongQuery& q) {
    WebLyrics w;
    w.id = "genius", w.source = "Genius";
    // best song hit: title words match, artist matches if known; skip translations / instrumentals / remixes
    // unless the file name asks for them. Genius' search is literal ("Lady Gaga x Bruno Mars" finds nothing), so
    // the query gets simpler until a hit fits.
    static const std::regex variant(R"(instrumental|remix|translation|romanized|cover|live|sped|slowed|karaoke)",
                                    std::regex::icase);
    const bool want_variant = std::regex_search(q.title, variant);
    std::vector<std::string> queries{q.title + " " + q.artist};
    if (first_artist(q.artist) != q.artist) queries.push_back(q.title + " " + first_artist(q.artist));
    if (!q.artist.empty()) queries.push_back(q.title);
    json best;
    double best_score = -1e9;
    for (auto& query : queries) {
        HttpResult r = http_get("https://genius.com/api/search/multi?q=" + url_encode(trim(query)));
        if (!http_ok(r, w)) return w;
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
                double ts = title_sim(q.title, title);
                // a different song; with no artist to check against, the title has to match more closely
                if (ts < (q.artist.empty() ? 0.8 : 0.6)) continue;
                double score = 2.0 * ts - 0.05 * rank++;
                if (!q.artist.empty()) score += title_sim(first_artist(q.artist), artist + " " + full);
                if (!want_variant && std::regex_search(full, variant)) score -= 1.5;
                if (score > best_score) best_score = score, best = s;
            }
        }
        if (best_score >= (q.artist.empty() ? 1.9 : 2.5)) break;  // title and artist both fit: no need to look further
    }
    if (best.is_null() || best_score < 1.2) {
        w.error = "no matching song on Genius";
        return w;
    }
    w.url = best.value("url", "");
    w.title = best.value("title", "");
    w.artist = best.contains("primary_artist") ? best["primary_artist"].value("name", "") : "";
    HttpResult page = http_get(w.url);
    if (!http_ok(page, w)) return w;
    w.lines = parse_genius_html(page.body);
    w.times.assign(w.lines.size(), std::nullopt);
    w.found = !w.lines.empty();
    if (!w.found) w.error = "no lyrics on the Genius page (instrumental, or the page layout changed)";
    return w;
}

// lyrics.ovh: plain words (a mirror of several lyric sites), needs the artist
WebLyrics fetch_ovh(const SongQuery& q) {
    WebLyrics w;
    w.id = "ovh", w.source = "lyrics.ovh";
    if (q.artist.empty()) {
        w.error = "needs the artist";
        return w;
    }
    HttpResult r = http_get("https://api.lyrics.ovh/v1/" + url_encode(first_artist(q.artist)) + "/" + url_encode(q.title));
    if (r.status == 404) {
        w.error = "not on lyrics.ovh";
        return w;
    }
    if (!http_ok(r, w)) return w;
    json d = json::parse(r.body, nullptr, false);
    std::string text = d.is_object() ? d.value("lyrics", "") : "";
    // a French mirror heads the text with "Paroles de la chanson X par Y"
    text = std::regex_replace(text, std::regex(R"(^Paroles de la chanson[^\r\n]*[\r\n]+)"), "");
    plain_lines(text, w);
    w.title = q.title, w.artist = first_artist(q.artist);
    w.url = "https://lyrics.ovh";
    w.found = !w.lines.empty();
    if (!w.found) w.error = "not on lyrics.ovh";
    return w;
}

std::vector<WebLyrics> fetch_all(const SongQuery& q) {
    std::vector<std::future<WebLyrics>> f;
    for (auto fn : {fetch_lrclib, fetch_netease, fetch_bugs, fetch_genius, fetch_ovh})
        f.push_back(std::async(std::launch::async, [fn, &q] {
            try {
                return fn(q);
            } catch (const std::exception& e) {  // one broken site must not take the others down
                WebLyrics w;
                w.error = e.what();
                return w;
            }
        }));
    std::vector<WebLyrics> out;
    for (auto& x : f) out.push_back(x.get());
    static const char* ids[] = {"lrclib", "netease", "bugs", "genius", "ovh"};
    static const char* names[] = {"LRCLIB", "NetEase", "Bugs", "Genius", "lyrics.ovh"};
    for (size_t i = 0; i < out.size(); i++)
        if (out[i].id.empty()) out[i].id = ids[i], out[i].source = names[i];
    return out;
}

// ------------------------------------------------------------------ merge

struct Merged {
    std::vector<std::string> lines;
    std::vector<std::optional<double>> times;
    int matched = 0, changed = 0, genius_only = 0, lrclib_only = 0;
    std::vector<std::pair<std::string, std::string>> examples;  // (words site, timed site) for lines that differ
};

// Line-level alignment (Needleman-Wunsch on word similarity) of a words site `g` and a timed site `l`. Moves: one
// line each (1:1), two `g` lines for one `l` line (2:1: Genius breaks "If the world was ending / I'd wanna be next to
// you" where LRCLIB has one line), one `g` line for two `l` lines (1:2), or a line on one side only. Matched lines take
// g's words; the first line of each match takes l's timestamp, a second g line of a 2:1 match is aligned freely.
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
        static const std::regex paren(R"(\s*[\(（]([^)）]*)[\)）])");
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
        } else if (c == '2') {  // pushed in reverse: second g line (untimed), then the first (l's time)
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
            // l has a line g doesn't (often a repeat g wrote once): keep it, in g's spelling if g has the same line
            // elsewhere
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

// share of lines two sites have in common: >= 0.5 means the same song (below: a namesake, "Fireworks" by Drake on
// one site and by DAOKO on another)
static double agreement(const WebLyrics& a, const WebLyrics& b) {
    if (a.lines.empty() || b.lines.empty()) return 0;
    return (double)merge(a, b).matched / std::min(a.lines.size(), b.lines.size());
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

// one choosable version of the lyrics: a site's own, or the best words on the best timing
struct Version {
    std::string id, name, detail;
    bool timed = false;
    std::vector<std::string> lines;
    std::vector<std::optional<double>> times;
    json info;
};

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
    std::vector<WebLyrics> sites = fetch_all(q);

    // Same song everywhere? Each site's answer is compared with every other's; the one most others agree with is the
    // reference (ties: a timed site whose length matches the file), and answers that don't match it are namesakes.
    auto fits = [&](const WebLyrics& w) { return w.duration > 0 && std::fabs(w.duration - duration) <= 10; };
    std::vector<size_t> found;
    for (size_t i = 0; i < sites.size(); i++)
        if (sites[i].found) found.push_back(i);
    if (found.size() >= 2) {
        std::vector<std::vector<double>> agree(sites.size(), std::vector<double>(sites.size(), 1));
        for (size_t a = 0; a < found.size(); a++)
            for (size_t b = a + 1; b < found.size(); b++)
                agree[found[a]][found[b]] = agree[found[b]][found[a]] = agreement(sites[found[a]], sites[found[b]]);
        size_t ref = found[0];
        double ref_key = -1;
        for (size_t i : found) {
            double votes = 0;
            for (size_t j : found) votes += j != i && agree[i][j] >= 0.5;
            double key = votes * 10 + (sites[i].timed && fits(sites[i]) ? 2 : 0) + (sites[i].timed ? 1 : 0);
            if (key > ref_key) ref_key = key, ref = i;
        }
        for (size_t i : found)
            if (i != ref && agree[i][ref] < 0.5) {
                log("  %s: a different song (%.0f%% of lines match %s) - left out", sites[i].source.c_str(), agree[i][ref] * 100,
                    sites[ref].source.c_str());
                sites[i].found = false;
                sites[i].error = "a different song than " + sites[ref].source + "'s (" + sites[i].title + " by " + sites[i].artist + ")";
            }
    }
    for (auto& w : sites) {
        if (w.found)
            log("  %-10s \"%s\" by %s%s, %zu lines%s", (w.source + ":").c_str(), w.title.c_str(), w.artist.c_str(),
                w.duration > 0 ? (" (" + mmss(w.duration) + ")").c_str() : "", w.lines.size(), w.timed ? ", timed" : "");
        else
            log("  %-10s %s", (w.source + ":").c_str(), w.error.c_str());
    }

    // best timing: a timed site, the one whose length fits the file first (site order breaks ties); best words: the
    // edited words sites
    const WebLyrics* timing = nullptr;
    for (auto& w : sites)
        if (w.found && w.timed && (!timing || (fits(w) && !fits(*timing)))) timing = &w;
    const std::vector<std::string> order{"genius", "ovh"};
    const WebLyrics* words = nullptr;
    for (auto& id : order)
        for (auto& w : sites)
            if (!words && w.id == id && w.found && !w.timed) words = &w;
    if (!timing && !words) {
        std::string why;
        for (auto& w : sites) why += (why.empty() ? "" : "; ") + w.source + ": " + w.error;
        die("no lyrics found (%s). Pass --lyrics FILE or --lrc FILE, or try --title/--artist.", why.c_str());
    }

    std::vector<Version> versions;
    if (timing && words) {
        Merged m = merge(*words, *timing);
        Version v{"best", words->source + " words on " + timing->source + " timing"};
        char d[160];
        snprintf(d, sizeof d, "%d lines matched (%d with different words), %d only on %s, %d only on %s", m.matched, m.changed,
                 m.genius_only, words->source.c_str(), m.lrclib_only, timing->source.c_str());
        v.detail = d, v.timed = true, v.lines = m.lines, v.times = m.times;
        json ex = json::array();
        for (auto& [gs, ls] : m.examples) ex.push_back({gs, ls});
        v.info = {{"matched", m.matched}, {"changed", m.changed}, {"words_only", m.genius_only}, {"timing_only", m.lrclib_only},
                  {"examples", ex}, {"words", words->id}, {"timing", timing->id}};
        versions.push_back(v);
    }
    for (auto& w : sites)
        if (w.found) {
            Version v{w.id, w.source};
            v.detail = "\"" + w.title + "\" by " + w.artist + (w.duration > 0 ? " (" + mmss(w.duration) + ")" : "") + ", " +
                       std::to_string(w.lines.size()) + " lines, " + (w.timed ? "timed" : "words only");
            v.timed = w.timed, v.lines = w.lines, v.times = w.times;
            v.info = {{"title", w.title}, {"artist", w.artist}, {"duration", w.duration}, {"url", w.url}};
            versions.push_back(v);
        }
    std::string script;
    for (auto& l : versions[0].lines) script += l + "\n";
    script = script_name(script);
    if (script != "Latin") log("  lyrics in %s: timed letter by letter through their romanisation", script.c_str());

    // which version: asked, given (a site id, "best", or the old local / web / both), or the first (best) one
    std::string src = choice.source == "local" ? "lrclib" : choice.source == "web" ? "genius" : choice.source == "both" ? "best" : choice.source;
    if (interactive) {
        fprintf(stderr, "\n");
        for (size_t k = 0; k < versions.size(); k++)
            fprintf(stderr, "  [%zu] %-30s %s\n", k + 1, versions[k].name.c_str(), versions[k].detail.c_str());
        for (auto& w : sites)
            if (!w.found) fprintf(stderr, "   -  %-30s %s\n", w.source.c_str(), w.error.c_str());
        if (q.artist.empty())
            fprintf(stderr, "  note: no artist known - check these are the right song (or rerun with --artist)\n");
        if (timing && timing->duration > 0 && std::fabs(timing->duration - duration) > 5)
            fprintf(stderr, "  note: the %s version is %s long, this file %s - its timestamps are treated as hints\n",
                    timing->source.c_str(), mmss(timing->duration).c_str(), mmss(duration).c_str());
        std::string a = ask("\nWhich lyrics?", "1");
        size_t k = (size_t)std::max(1, std::atoi(a.c_str()));
        src = versions[std::min(k, versions.size()) - 1].id;
    }
    if (src.empty()) src = versions[0].id;
    auto chosen = std::find_if(versions.begin(), versions.end(), [&](const Version& v) { return v.id == src; });
    if (chosen == versions.end()) {
        auto w = std::find_if(sites.begin(), sites.end(), [&](const WebLyrics& x) { return x.id == src; });
        die("no %s lyrics (%s)", src.c_str(), w != sites.end() ? w->error.c_str() : "unknown source");
    }

    std::string header = "[ti:" + q.title + "]\n" + (q.artist.empty() ? "" : "[ar:" + q.artist + "]\n");
    if (!choice.versions_dir.empty()) {
        // for the app: every version, written side by side, and a summary on stdout to choose from
        fs::create_directories(choice.versions_dir);
        json s = {{"query", {{"title", q.title}, {"artist", q.artist}}}, {"duration", duration}, {"script", script},
                  {"versions", json::array()}, {"missing", json::array()}, {"recommended", src}};
        for (auto& v : versions) {
            json j = v.info;
            j["id"] = v.id, j["name"] = v.name, j["detail"] = v.detail, j["timed"] = v.timed, j["lines"] = v.lines.size();
            j["file"] = utf8(save_lyrics(choice.versions_dir / v.id, v.lines, v.times, header + "[re:" + v.name + " (mtn)]\n"));
            s["versions"].push_back(j);
        }
        for (auto& w : sites)
            if (!w.found) s["missing"].push_back({{"id", w.id}, {"name", w.source}, {"error", w.error}});
        printf("%s\n", s.dump().c_str());
        fflush(stdout);
        for (auto& v : s["versions"])
            if (v["id"] == src) return fs::u8path(v["file"].get<std::string>());
    }
    fs::path save = choice.save.empty() ? default_save : choice.save;
    if (interactive && choice.save.empty()) {
        fs::path shown = save;
        shown.replace_extension(chosen->timed ? ".lrc" : ".txt");
        save = fs::u8path(ask("Save lyrics to", utf8(shown)));
    }
    if (fs::is_directory(save)) save /= audio.stem();
    fs::path out = save_lyrics(save, chosen->lines, chosen->times, header + "[re:" + chosen->name + " (mtn)]\n");
    log("lyrics (%s): %s", chosen->name.c_str(), utf8(out).c_str());
    return out;
}

}  // namespace mt
