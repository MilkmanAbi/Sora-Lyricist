#include "lyrics.h"

#include <algorithm>
#include <regex>
#include <sstream>

namespace mt {

bool Lyrics::synced() const {
    return std::any_of(times.begin(), times.end(), [](auto& t) { return t.has_value(); });
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

Lyrics load_lyrics(const fs::path& p) {
    Lyrics L;
    std::string text = read_text(p);
    const bool lrc = p.extension() == ".lrc";
    static const std::regex tag(R"(\[(\d+):(\d+(?:\.\d+)?)\])"), any_tag(R"(\[[^\]]*\])");
    struct Row {
        double t;
        std::string text;
    };
    std::vector<Row> rows;
    std::stringstream ss(text);
    std::string raw;
    std::string plain;
    bool untimed = false;
    while (std::getline(ss, raw)) {
        std::string line = trim(raw);
        plain += std::regex_replace(raw, any_tag, "") + "\n";
        if (lrc) {
            // [mm:ss.xx][mm:ss.xx]text: a line sung several times may carry several stamps
            std::vector<double> ts;
            auto it = std::sregex_iterator(line.begin(), line.end(), tag);
            for (; it != std::sregex_iterator(); ++it) ts.push_back(std::stod((*it)[1]) * 60 + std::stod((*it)[2]));
            std::string body = trim(std::regex_replace(line, any_tag, ""));
            if (body.empty()) continue;  // blank / metadata ([ti:...]) / timing-only lines
            if (ts.empty()) {
                // a line without a stamp (mtn's merged Genius + LRCLIB lyrics): aligned freely, between its neighbours
                rows.push_back({-1, body});
                untimed = true;
                continue;
            }
            for (double t : ts) rows.push_back({t, body});
        } else if (!line.empty()) {
            rows.push_back({-1, line});
        }
    }
    // lines with several stamps go to their place in time; with untimed lines present the file order is the order
    if (lrc && !untimed) std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t < b.t; });
    for (auto& r : rows) {
        int li = (int)L.lines.size();
        L.lines.push_back(r.text);
        L.times.push_back(r.t >= 0 ? std::optional<double>(r.t) : std::nullopt);
        std::string t = r.text;
        for (char& c : t)
            if (c == '(' || c == ')' || c == '[' || c == ']') c = ' ';
        std::stringstream ws(t);
        std::string w;
        while (ws >> w) {
            L.words.push_back(w);
            L.line_of.push_back(li);
        }
    }
    L.raw = plain;
    if (L.words.empty()) die("no lyrics in %s", utf8(p).c_str());
    return L;
}

}  // namespace mt
