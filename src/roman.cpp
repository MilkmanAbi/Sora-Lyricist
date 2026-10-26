#include "roman.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace mt {

// ------------------------------------------------------------------ tables

static std::unordered_map<char32_t, std::string> g_table;  // models/roman.tsv
// models/ja_kanwa.tsv by first character: (word, reading), longest words first
static std::unordered_map<char32_t, std::vector<std::pair<std::u32string, std::u32string>>> g_kanwa;
static fs::path g_models;
static std::once_flag g_table_once, g_kanwa_once;

void init_romanizer(const fs::path& models) {
    g_models = models;
    std::call_once(g_table_once, [&] {
        std::error_code ec;
        fs::path p = models / "roman.tsv";
        if (!fs::exists(p, ec)) {
            log("  note: %s missing - lyrics outside plain Latin and kana won't be timed letter by letter", utf8(p).c_str());
            return;
        }
        std::stringstream ss(read_text(p));
        std::string line;
        while (std::getline(ss, line)) {
            size_t tab = line.find('\t');
            if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
            g_table[(char32_t)std::stoul(line.substr(0, tab), nullptr, 16)] = line.substr(tab + 1);
        }
    });
}

static void load_kanwa() {  // 270k words: only read when a song's lyrics have kana
    std::call_once(g_kanwa_once, [] {
        std::error_code ec;
        fs::path p = g_models / "ja_kanwa.tsv";
        if (g_models.empty() || !fs::exists(p, ec)) return;
        std::stringstream ss(read_text(p));
        std::string line;
        while (std::getline(ss, line)) {
            size_t tab = line.find('\t');
            if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
            std::u32string w = to_u32(line.substr(0, tab)), r = to_u32(line.substr(tab + 1));
            if (!w.empty()) g_kanwa[w[0]].push_back({std::move(w), std::move(r)});
        }
        for (auto& [c, v] : g_kanwa)
            std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
    });
}

// ------------------------------------------------------------------ UTF-8

static bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n ? 0 : 1)) return false;
        for (int k = 1; k <= n; k++)
            if (i + k >= s.size() || ((unsigned char)s[i + k] >> 6) != 2) return false;
        i += n + 1;
    }
    return true;
}

std::string utf8_text(const std::string& bytes) {
    std::string s = bytes.rfind("\xEF\xBB\xBF", 0) == 0 ? bytes.substr(3) : bytes;
    if (valid_utf8(s)) return s;
    // Windows-1252: 0x80-0x9F are typographic marks, 0xA0-0xFF are Latin-1
    static const char16_t w1252[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                       0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013,
                                       0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    std::u32string u;
    for (unsigned char c : s) u += c >= 0x80 && c < 0xA0 ? (char32_t)(w1252[c - 0x80] ? w1252[c - 0x80] : '?') : (char32_t)c;
    return to_u8(u);
}

std::u32string to_u32(const std::string& s) {
    std::u32string u;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || (n > 0 && i + n >= s.size())) {
            u += 0xFFFD;
            i++;
            continue;
        }
        char32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k <= n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        u += cp;
        i += n + 1;
    }
    return u;
}

std::string to_u8(const std::u32string& u) {
    std::string o;
    for (char32_t cp : u) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) o += {(char)(0xC0 | (cp >> 6)), (char)(0x80 | (cp & 0x3F))};
        else if (cp < 0x10000)
            o += {(char)(0xE0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F))};
        else
            o += {(char)(0xF0 | (cp >> 18)), (char)(0x80 | ((cp >> 12) & 0x3F)), (char)(0x80 | ((cp >> 6) & 0x3F)),
                  (char)(0x80 | (cp & 0x3F))};
    }
    return o;
}

// ------------------------------------------------------------------ scripts

static bool is_han(char32_t c) {
    return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0x20000 && c <= 0x2FFFF) || c == 0x3005 || c == 0x3006;  // 々 〆
}
static bool is_hira(char32_t c) { return c >= 0x3041 && c <= 0x309F; }
static bool is_kata(char32_t c) { return (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC || (c >= 0x31F0 && c <= 0x31FF); }
static bool is_kana(char32_t c) { return is_hira(c) || is_kata(c); }
static bool is_thai(char32_t c) { return c >= 0x0E00 && c <= 0x0E7F; }
static bool is_space(char32_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0 || c == 0x3000; }
static bool is_apos(char32_t c) { return c == '\'' || c == 0x2018 || c == 0x2019 || c == 0x02BC; }
static bool is_punct(char32_t c) {
    if (c < 0x80) return !std::isalnum((int)c) && !is_apos(c) && !is_space(c);
    return (c >= 0x2010 && c <= 0x205E && !is_apos(c)) || (c >= 0x3001 && c <= 0x303F && c != 0x3005 && c != 0x3006) ||
           c == 0x30FB || (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) ||
           (c >= 0xFF5B && c <= 0xFF65) || c == 0x00A1 || c == 0x00BF || c == 0x00AB || c == 0x00BB || c == 0x266A ||
           c == 0x266B;  // ♪
}
static bool spaceless_char(char32_t c) { return is_han(c) || is_kana(c) || is_thai(c); }
static bool opening(char32_t c) {
    return c == 0x300C || c == 0x300E || c == 0xFF08 || c == 0x3010 || c == 0x3008 || c == 0x300A || c == 0x201C ||
           c == 0x2018 || c == '(' || c == '[' || c == '"';
}
// small kana, ー and iteration marks belong to the character before them
static bool clings(char32_t c) {
    char32_t h = is_kata(c) && c <= 0x30F6 ? c - 0x60 : c;
    return h == 0x3041 || h == 0x3043 || h == 0x3045 || h == 0x3047 || h == 0x3049 || h == 0x3063 || h == 0x3083 ||
           h == 0x3085 || h == 0x3087 || h == 0x308E || c == 0x30FC || c == 0x309D || c == 0x309E || c == 0x30FD ||
           c == 0x30FE || c == 0x3005;
}

bool has_kana(const std::string& text) {
    for (char32_t c : to_u32(text))
        if (is_kana(c) && c != 0x30FC) return true;
    return false;
}

bool has_spaceless(const std::string& text) {
    for (char32_t c : to_u32(text))
        if (spaceless_char(c)) return true;
    return false;
}

std::string script_name(const std::string& text) {
    std::unordered_map<std::string, int> n;
    bool kana = false;
    for (char32_t c : to_u32(text)) {
        const char* s = nullptr;
        if (c < 0x80) s = std::isalpha((int)c) ? "Latin" : nullptr;
        else if (is_kana(c)) s = "Japanese", kana = true;
        else if (is_han(c)) s = "Han";
        else if (c >= 0xAC00 && c <= 0xD7A3) s = "Korean";
        else if (c >= 0x00C0 && c <= 0x024F) s = "Latin";
        else if (c >= 0x0370 && c <= 0x03FF) s = "Greek";
        else if (c >= 0x0400 && c <= 0x04FF) s = "Cyrillic";
        else if (c >= 0x0590 && c <= 0x05FF) s = "Hebrew";
        else if (c >= 0x0600 && c <= 0x06FF) s = "Arabic";
        else if (c >= 0x0900 && c <= 0x097F) s = "Devanagari";
        else if (c >= 0x0980 && c <= 0x09FF) s = "Bengali";
        else if (c >= 0x0A00 && c <= 0x0AFF) s = "Gurmukhi/Gujarati";
        else if (c >= 0x0B80 && c <= 0x0BFF) s = "Tamil";
        else if (c >= 0x0C00 && c <= 0x0C7F) s = "Telugu";
        else if (c >= 0x0C80 && c <= 0x0CFF) s = "Kannada";
        else if (c >= 0x0D00 && c <= 0x0D7F) s = "Malayalam";
        else if (is_thai(c)) s = "Thai";
        if (s) n[s]++;
    }
    if (kana) n["Japanese"] += n["Han"], n.erase("Han");  // kanji in Japanese lyrics
    else if (n.count("Han")) n["Chinese"] = n["Han"], n.erase("Han");
    std::string best = "Latin";
    int most = 0;
    for (auto& [k, v] : n)
        if (v > most) most = v, best = k;
    return best;
}

// ------------------------------------------------------------------ display units

static bool closing(char32_t c) {
    return c == 0x300D || c == 0x300F || c == 0xFF09 || c == 0x3011 || c == 0x3009 || c == 0x300B || c == 0x3001 ||
           c == 0x3002 || c == 0xFF01 || c == 0xFF1F || c == 0xFF0C || c == ',' || c == '.' || c == '!' || c == '?' ||
           c == ')' || c == ']' || c == 0x2026;
}
// Japanese particles and the conjunctive て: a long kana stretch breaks after them (ように|溶けて|ゆくように)
static bool particle(char32_t c) {
    char32_t h = is_kata(c) && c <= 0x30F6 ? c - 0x60 : c;
    return h == 0x306F || h == 0x304C || h == 0x3092 || h == 0x306B || h == 0x3067 || h == 0x3068 || h == 0x3082 ||
           h == 0x306E || h == 0x3078 || h == 0x3084 || h == 0x304B || h == 0x306D || h == 0x3088 || h == 0x3066;
}

// a long unit -> pieces of about `most` letters, never starting on punctuation or a clinging kana. Japanese picks
// the cuts by cost (after a particle is free, fewer pieces are better: ひとつに|なれない, 溶けて|ゆくように); other
// scripts get even pieces.
static std::vector<std::u32string> cut_unit(const std::u32string& u, size_t most, bool japanese) {
    std::vector<size_t> pos;  // index in u of each letter
    for (size_t i = 0; i < u.size(); i++)
        if (!is_punct(u[i])) pos.push_back(i);
    const size_t L = pos.size();
    if (L <= most + 1) return {u};
    // may a piece start at letter j? (not on a clinging kana)
    auto can_start = [&](size_t j) { return j == 0 || !clings(u[pos[j]]); };
    std::vector<size_t> cuts;  // letter indices where a new piece starts
    if (japanese) {
        std::vector<double> best(L + 1, 1e9);
        std::vector<size_t> from(L + 1, 0);
        best[0] = 0;
        for (size_t j = 1; j <= L; j++) {
            if (j < L && !can_start(j)) continue;
            for (size_t i = j >= most + 3 ? j - most - 3 : 0; i < j; i++) {
                if (best[i] >= 1e9) continue;
                size_t n = j - i;
                double c = 0.5 + (n > most + 1 ? 3.0 * (n - most - 1) : 0) + (n == 1 ? 1.5 : 0);
                if (j < L && !particle(u[pos[j - 1]])) c += 1.0;  // a break that isn't after a particle
                if (best[i] + c < best[j]) best[j] = best[i] + c, from[j] = i;
            }
        }
        for (size_t j = L; j > 0; j = from[j])
            if (from[j] > 0) cuts.push_back(from[j]);
        std::reverse(cuts.begin(), cuts.end());
    } else {
        size_t pieces = (L + most - 1) / most, per = (L + pieces - 1) / pieces;
        for (size_t j = per; j < L; j += per) {
            while (j < L && !can_start(j)) j++;
            if (j < L) cuts.push_back(j);
        }
    }
    std::vector<std::u32string> out;
    size_t start = 0;
    for (size_t j : cuts) {
        out.push_back(u.substr(start, pos[j] - start));
        start = pos[j];
    }
    out.push_back(u.substr(start));
    return out;
}

std::vector<std::string> split_units(const std::string& line, bool japanese) {
    std::vector<std::string> units;
    std::u32string u = to_u32(line), tok;
    auto token = [&](const std::u32string& t) {
        if (t.empty()) return;
        bool sl = std::any_of(t.begin(), t.end(), spaceless_char);
        if (!sl) {
            units.push_back(to_u8(t));
            return;
        }
        // kinds: 1 kanji/katakana (a unit's head), 2 hiragana (its tail), 3 Latin/digits/other, 4 Thai
        std::vector<std::u32string> out;
        std::u32string cur, lead;
        int phase = 0, han_in_unit = 0;
        auto flush = [&] {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            phase = 0;
            han_in_unit = 0;
        };
        for (char32_t c : t) {
            if (is_punct(c)) {
                if (cur.empty() || opening(c)) {
                    if (opening(c)) flush();
                    lead += c;
                } else {
                    cur += c;
                    if (closing(c)) flush();  // 「さよなら」|だけ, ラーメン、|ちゃんと
                }
                continue;
            }
            if (clings(c) && phase != 0) {  // ー, small kana: part of the character before
                cur += c;
                continue;
            }
            int kind = is_han(c) || (is_kata(c) && !clings(c)) ? 1 : is_hira(c) || clings(c) ? 2 : is_thai(c) ? 4 : 3;
            if (kind == 1) {
                bool chinese_pair = !japanese && is_han(c) && han_in_unit >= 2;
                if (phase == 2 || phase == 3 || phase == 4 || chinese_pair) flush();
                phase = 1;
                han_in_unit += is_han(c);
            } else if (kind == 2) {
                if (phase == 3 || phase == 4) flush();
                if (phase == 0 || phase == 1) phase = 2;
            } else if (kind != phase) {
                flush();
                phase = kind;
            }
            cur += lead;
            lead.clear();
            cur += c;
        }
        if (!lead.empty()) {
            if (!cur.empty()) cur += lead;
            else if (!out.empty()) out.back() += lead;
            else cur = lead;
        }
        flush();
        for (auto& o : out)
            for (auto& piece : cut_unit(o, 4, japanese)) units.push_back(to_u8(piece));
    };
    for (char32_t c : u) {
        if (is_space(c)) {
            token(tok);
            tok.clear();
        } else {
            tok += c;
        }
    }
    token(tok);
    return units;
}

// ------------------------------------------------------------------ spelling

static const char* kHira[] = {  // U+3041 .. U+3096
    "a",  "a",  "i",  "i",  "u",  "u",  "e",  "e",  "o",  "o",  "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge",
    "ko", "go", "sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo", "ta", "da", "chi", "ji", "",  "tsu",
    "zu", "te", "de", "to", "do", "na", "ni", "nu", "ne", "no", "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu",
    "pu", "he", "be", "pe", "ho", "bo", "po", "ma", "mi", "mu", "me", "mo", "ya", "ya", "yu", "yu", "yo", "yo",
    "ra", "ri", "ru", "re", "ro", "wa", "wa", "i",  "e",  "o",  "n",  "vu", "ka", "ke"};
static_assert(sizeof(kHira) / sizeof(kHira[0]) == 0x3096 - 0x3041 + 1);

static bool vowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

// Hepburn for a run of kana: きゃ kya, しゃ sha, ふぁ fa, っ doubles the next consonant, ー repeats the vowel
static void kana_roman(const std::u32string& run, std::string& out) {
    bool sokuon = false;
    for (char32_t c : run) {
        if (c == U' ') {
            if (!sokuon && !out.empty() && out.back() != ' ') out += ' ';  // (奪っ た: "ubatta", one word)
            continue;
        }
        char32_t h = is_kata(c) && c <= 0x30F6 ? c - 0x60 : c;
        if (c == 0x30FC) {  // ー
            for (size_t k = out.size(); k-- > 0;)
                if (vowel(out[k])) {
                    out += out[k];
                    break;
                }
            continue;
        }
        if (h == 0x3063) {  // っ
            sokuon = true;
            continue;
        }
        if (h < 0x3041 || h > 0x3096) continue;  // iteration marks, ・
        if (h == 0x3083 || h == 0x3085 || h == 0x3087) {  // small ゃゅょ
            char v = h == 0x3083 ? 'a' : h == 0x3085 ? 'u' : 'o';
            size_t n = out.size();
            if (n >= 3 && (out.compare(n - 3, 3, "shi") == 0 || out.compare(n - 3, 3, "chi") == 0)) out.pop_back(), out += v;
            else if (n >= 2 && out.compare(n - 2, 2, "ji") == 0) out.pop_back(), out += v;
            else if (n >= 2 && out[n - 1] == 'i') out.pop_back(), out += 'y', out += v;
            else out += 'y', out += v;
            continue;
        }
        if (h == 0x3041 || h == 0x3043 || h == 0x3045 || h == 0x3047 || h == 0x3049) {  // small vowels: ファ fa, ティ ti
            if (out.size() >= 2 && vowel(out.back()) && !vowel(out[out.size() - 2])) out.pop_back();
            out += kHira[h - 0x3041];
            continue;
        }
        std::string syl = kHira[h - 0x3041];
        if (sokuon && !syl.empty() && !vowel(syl[0])) out += syl[0] == 'c' ? 't' : syl[0];
        sokuon = false;
        out += syl;
    }
}

// Hangul syllable -> Revised Romanisation; a final consonant before a silent ㅇ is carried over (한국어 hangugeo)
static void hangul_roman(char32_t c, char32_t next, std::string& out) {
    static const char* L[] = {"g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s", "ss", "", "j", "jj", "ch", "k", "t", "p", "h"};
    static const char* V[] = {"a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae",
                              "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i"};
    static const char* T[] = {"", "k", "k", "k", "n", "n", "n", "t", "l", "k", "l", "l", "l", "p",
                              "l", "l", "m", "p", "p", "t", "t", "ng", "t", "t", "k", "t", "p", "t"};
    static const char* TL[] = {"", "g", "kk", "gs", "n", "nj", "n", "d", "r", "lg", "lm", "lb", "lr", "lt",
                               "lp", "r", "m", "b", "bs", "s", "ss", "ng", "j", "ch", "k", "t", "p", ""};
    int i = (int)(c - 0xAC00);
    int l = i / 588, v = (i % 588) / 28, t = i % 28;
    bool liaison = next >= 0xAC00 && next <= 0xD7A3 && (int)(next - 0xAC00) / 588 == 11;  // next starts with ㅇ
    out += L[l];
    out += V[v];
    out += liaison ? TL[t] : T[t];
}

// Indic scripts (Devanagari .. Malayalam share one layout): a consonant without a vowel sign or virama after it
// carries an "a" (न म स ् त े -> namaste)
static bool indic(char32_t c) { return c >= 0x0900 && c < 0x0D80; }
static int indic_off(char32_t c) { return (int)(c & 0x7F); }
static bool indic_consonant(char32_t c) {
    int o = indic_off(c);
    return indic(c) && ((o >= 0x15 && o <= 0x39) || (o >= 0x58 && o <= 0x5F));
}
static bool indic_mark(char32_t c) {  // vowel sign, virama or nukta
    int o = indic_off(c);
    return indic(c) && ((o >= 0x3C && o <= 0x4D) || (o >= 0x55 && o <= 0x57) || o == 0x62 || o == 0x63);
}

// a stretch of kanji and kana -> one kana run (kanji by their dictionary reading) spelled in one go, so a reading
// ending in っ doubles the consonant after it (奪っ|た -> ubatta); a kanji the dictionary lacks is '*'
static void japanese_run(const std::u32string& w, size_t& i, std::string& out) {
    load_kanwa();
    std::u32string kana;
    while (i < w.size() && (is_kana(w[i]) || is_han(w[i]))) {
        if (is_kana(w[i])) {
            kana += w[i++];
            continue;
        }
        bool found = false;
        auto it = g_kanwa.find(w[i]);
        if (it != g_kanwa.end())
            for (auto& [word, reading] : it->second)
                if (w.compare(i, word.size(), word) == 0) {
                    kana += reading;
                    kana += U' ';  // (a space between readings: kept in readable romaji, dropped for the aligner)
                    i += word.size();
                    found = true;
                    break;
                }
        if (!found) {
            kana_roman(kana, out);
            kana.clear();
            out += " * ";
            i++;
        }
    }
    kana_roman(kana, out);
    out += ' ';
}

// any text -> Latin, units / syllables separated by spaces
static std::string spell(const std::u32string& w, bool japanese) {
    std::string out;
    for (size_t i = 0; i < w.size();) {
        char32_t c = w[i];
        if (c < 0x80) {
            out += std::isalnum((int)c) || c == '\'' ? (char)std::tolower((int)c) : ' ';
            i++;
        } else if (is_apos(c)) {
            out += '\'';
            i++;
        } else if (japanese && (is_kana(c) || is_han(c))) {
            japanese_run(w, i, out);
        } else if (is_kana(c)) {
            size_t j = i;
            while (j < w.size() && is_kana(w[j])) j++;
            kana_roman(w.substr(i, j - i), out);
            out += ' ';
            i = j;
        } else if (c >= 0xAC00 && c <= 0xD7A3) {
            hangul_roman(c, i + 1 < w.size() ? w[i + 1] : 0, out);
            i++;
        } else {
            auto t = g_table.find(c);
            if (t != g_table.end()) {
                out += t->second;
                if (indic_consonant(c)) {
                    size_t k = i + 1;
                    if (k < w.size() && indic_off(w[k]) == 0x3C && indic(w[k])) k++;  // nukta
                    if (!(k < w.size() && indic_mark(w[k]))) out += 'a';
                }
                if (is_han(c)) out += ' ';  // one syllable per character
            } else if (is_han(c) || (c >= 0xAC00 && c <= 0xD7FF)) {
                out += "* ";
            } else if (!indic(c)) {  // (a virama or other mark the tables leave out adds nothing)
                out += ' ';
            }
            i++;
        }
    }
    return out;
}

static std::string squeeze(const std::string& s) {  // single spaces, no ends
    std::string o;
    for (char c : s)
        if (c != ' ' || (!o.empty() && o.back() != ' ')) o += c;
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

std::string align_letters(const std::string& word, bool japanese) {
    bool ascii = std::all_of(word.begin(), word.end(), [](char c) { return (unsigned char)c < 0x80; });
    if (ascii) return word;
    // a repeated letter is said once (मातरम् maataram, ラーメン raamen, 待って matte): in CTC two equal letters need a
    // blank frame between them, which an English-trained model hardly ever gives a long vowel
    std::string o;
    for (char c : spell(to_u32(word), japanese))
        if (((c >= 'a' && c <= 'z') || c == '\'' || c == '*') && (o.empty() || c != o.back() || c == '*')) o += c;
    return o;
}

std::string romanize(const std::string& text, bool japanese) {
    std::string out;
    std::u32string u = to_u32(text), tok;
    auto token = [&](const std::u32string& t) {
        if (t.empty()) return;
        std::string s = to_u8(t);
        if (has_spaceless(s))
            for (auto& unit : split_units(s, japanese)) out += spell(to_u32(unit), japanese) + " ";
        else
            out += spell(t, japanese) + " ";
    };
    for (char32_t c : u) {
        if (is_space(c)) token(tok), tok.clear();
        else tok += c;
    }
    token(tok);
    std::string o = squeeze(out);
    o.erase(std::remove(o.begin(), o.end(), '*'), o.end());
    return squeeze(o);
}

}  // namespace mt
