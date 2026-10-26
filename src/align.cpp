#include "align.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "infer.h"
#include "json.hpp"

namespace mt {

static const int kSR = 16000, kHop = 320;          // wav2vec2: one frame per 20 ms
static const int kWin = 24 * kSR;                  // model input (fixed shape)
static const int kCtx = 2 * kSR, kStep = 20 * kSR; // keep the middle 20 s of every window

EmissionStream::EmissionStream(const fs::path& models, const std::string& model, const std::string& device,
                               const Cache& cache, size_t total_samples)
    : models_(models), model_(model), device_(device), cache_(cache), total_(total_samples) {
    auto meta = nlohmann::json::parse(read_text(models / (model + ".json")));  // labels
    for (auto& l : meta["labels"]) em_.labels.push_back(l.get<std::string>());
    em_.classes = (int)em_.labels.size();
    em_.frame_s = (double)kHop / kSR;
    em_.logp.assign(((total_ + kHop - 1) / kHop) * em_.classes, 0.f);
}

EmissionStream::~EmissionStream() = default;

void EmissionStream::update(const std::vector<float>& v, size_t avail) {
    const size_t n = total_, frames = (n + kHop - 1) / kHop;
    uint64_t model_key = fnv(model_ + "/v1");
    std::vector<float> win(kWin);
    auto run = [&](Net& m) {
        std::copy(win.begin(), win.end(), m.input(0));
        m.infer();
        return std::vector<float>(m.output(0), m.output(0) + m.output_size(0));
    };
    while (next_ < n && avail >= std::min(n, next_ + kStep + kCtx)) {  // window [a - ctx, a + step + ctx) is final
        Timer t;
        const size_t a = next_;
        long long lo = (long long)a - kCtx;
        for (int i = 0; i < kWin; i++) {
            long long s = lo + i;
            win[i] = (s >= 0 && s < (long long)n) ? v[s] : 0.f;
        }
        uint64_t key = fnv(win.data(), win.size() * sizeof(float), model_key);
        std::vector<float> out;
        if (!cache_.get("w2v", key, out)) {
            bool verify = false;
            if (!net_) {
                net_ = std::make_unique<Net>(model_path(models_, model_, device_), device_);
                verify = is_dml(device_);
            }
            std::string why;
            try {
                out = run(*net_);
            } catch (const std::exception& e) {
                why = e.what();
            }
            if (verify || !why.empty()) {
                // First DirectML window: on the frames where the CPU's INT8 model hears a letter, the GPU must hear the
                // same letter within +-2 frames (40 ms). Most frames are blank in any window, so comparing all frames
                // would pass a GPU that outputs nothing at all - which is what one reset by Windows' watchdog returns.
                Net cpu(models_ / "w2v_large_int8.xml", "CPU");
                std::vector<float> ref = run(cpu);
                const size_t C = em_.classes, nf = std::min(ref.size(), out.size()) / C;
                auto best = [&](const std::vector<float>& x, size_t f) {
                    return std::max_element(x.begin() + f * C, x.begin() + (f + 1) * C) - (x.begin() + f * C);
                };
                size_t letters = 0, same = 0;
                for (size_t f = 0; f < nf; f++) {
                    auto r = best(ref, f);
                    if (r == 0) continue;  // blank
                    letters++;
                    for (size_t g = f >= 2 ? f - 2 : 0; g <= std::min(nf - 1, f + 2); g++)
                        if (best(out, g) == r) {
                            same++;
                            break;
                        }
                }
                double agree = letters ? (double)same / letters : 1.0;
                if (why.empty() && agree >= 0.5) {
                    log("  GPU check: DirectML aligner hears the same letters as the CPU (%.0f%%)", agree * 100);
                } else {
                    std::string what = why.empty() ? "hears different letters (" + std::to_string((int)(agree * 100)) + "% agree)" : why;
                    log("  GPU check: DirectML aligner %s - timing on the CPU instead (and from now on)", what.c_str());
                    mark_gpu_bad(device_, model_, what);
                    model_ = "w2v_large_int8";
                    device_ = "CPU";
                    model_key = fnv(model_ + "/v1");
                    key = fnv(win.data(), win.size() * sizeof(float), model_key);
                    net_ = std::make_unique<Net>(model_path(models_, model_, device_), device_);
                    out = ref;
                }
            }
            cache_.put("w2v", key, out.data(), out.size());
            computed_++;
        }
        const size_t wf = out.size() / em_.classes;  // ~1199 frames per 24 s window
        const size_t k0 = kCtx / kHop, keep = kStep / kHop;
        for (size_t k = 0; k < keep; k++) {
            size_t g = a / kHop + k, src = k0 + k;
            if (g >= frames || src >= wf) break;
            const float* s = out.data() + src * em_.classes;
            float* d = em_.logp.data() + g * em_.classes;
            for (int c = 0; c < em_.classes; c++) d[c] = std::max(s[c], -1e4f);  // fp16 GPU output underflows to -inf
        }
        next_ += kStep;
        windows_++;
        progress("align", (double)windows_, (double)((n + kStep - 1) / kStep));
        seconds_ += t.s();
    }
}

Emissions EmissionStream::take() {
    log("character probabilities: %zu windows (%zu computed, %.1fs)", windows_, computed_, seconds_);
    return std::move(em_);
}

static bool upper_labels(const std::vector<std::string>& labels) {
    for (auto& l : labels)
        if (l.size() == 1 && std::isupper((unsigned char)l[0])) return true;
    return false;
}

// CTC Viterbi over blank,c1,blank,c2,...; returns the state of every frame (see tools/ctc_align.py:_viterbi)
static std::vector<int> viterbi(const Emissions& em, const std::vector<int>& cls, const std::vector<uint8_t>& can_skip,
                                const std::vector<uint8_t>& filler, const std::vector<double>& anchor_s,
                                const AlignOptions& opt) {
    const size_t T = em.frames(), S = cls.size();
    const double NEG = -1e30;
    std::vector<double> prev(S, NEG), cur(S, NEG);
    std::vector<uint8_t> back(T * S, 0);
    const float* E = em.logp.data();
    for (size_t t = 0; t < T; t++) {
        const double ts = t * em.frame_s;
        const float* e_t = E + t * em.classes;
        uint8_t* bk = back.data() + t * S;
        // states beyond what can be reached by frame t (2 per character) stay at NEG; skip them
        size_t smax = std::min(S, 2 * t + 2);
        for (size_t s = 0; s < smax; s++) {
            double best;
            uint8_t arg = 0;
            if (t == 0) {
                best = s < 2 ? 0.0 : NEG;
            } else {
                best = prev[s];
                if (s >= 1 && prev[s - 1] > best) best = prev[s - 1], arg = 1;
                if (s >= 2 && can_skip[s] && prev[s - 2] > best) best = prev[s - 2], arg = 2;
            }
            if (best <= NEG) {
                cur[s] = NEG;
                continue;
            }
            double e = e_t[cls[s]];
            if (filler[s] && opt.filler_logp > e) e = opt.filler_logp;
            if (anchor_s[s] >= 0) {
                double d = std::fabs(ts - anchor_s[s]) - opt.slack;
                if (d > 0) e -= opt.weight * d;
            }
            cur[s] = best + e;
            bk[s] = arg;
        }
        std::swap(prev, cur);
        std::fill(cur.begin(), cur.end(), NEG);
    }
    std::vector<int> path(T);
    long long s = (S >= 2 && prev[S - 2] > prev[S - 1]) ? (long long)S - 2 : (long long)S - 1;
    for (long long t = (long long)T - 1; t >= 0; t--) {
        path[t] = (int)s;
        s -= back[(size_t)t * S + s];
        if (s < 0) s = 0;
    }
    return path;
}

std::vector<Word> align(const Emissions& em, const std::vector<std::string>& words, const std::vector<std::string>& letters,
                        const std::vector<int>& line_of, const std::vector<std::optional<double>>& anchors,
                        const AlignOptions& opt) {
    const bool upper = upper_labels(em.labels);
    int blank = 0, sep = -1;
    std::vector<int> index(256, -1);
    for (int i = 0; i < (int)em.labels.size(); i++) {
        const std::string& l = em.labels[i];
        if (l == "|") sep = i;
        if (l.size() == 1 && l != "-" && l != "|" && l != "*") index[(unsigned char)l[0]] = i;
    }
    std::vector<int> cls{blank};
    std::vector<uint8_t> filler{1};  // intro singing that isn't in the lyrics
    std::vector<uint8_t> wild{0};    // a '*' state: must be passed through, never skipped
    std::vector<int> first(words.size(), -1), last(words.size(), -1);
    for (size_t i = 0; i < words.size(); i++) {
        std::vector<int> tk;  // -1: a sound the romaniser couldn't spell (a blank state that may take any singing)
        const std::string& w = letters[i];
        for (size_t k = 0; k < w.size(); k++) {
            if (w[k] == '*') {
                tk.push_back(-1);
                continue;
            }
            unsigned char c = (unsigned char)w[k];
            if (c == 0xE2 && k + 2 < w.size() && (unsigned char)w[k + 1] == 0x80 &&
                ((unsigned char)w[k + 2] == 0x99 || (unsigned char)w[k + 2] == 0x98)) {  // curly apostrophes
                c = '\'';
                k += 2;
            }
            c = upper ? (unsigned char)std::toupper(c) : (unsigned char)std::tolower(c);
            if (index[c] >= 0) tk.push_back(index[c]);
        }
        if (tk.empty()) continue;  // numbers / symbols: placed between neighbours below
        bool new_line = i > 0 && !line_of.empty() && line_of[i] != line_of[i - 1];
        if (new_line) filler.back() = 1;  // the blank before a line may also hold ad-libs
        if (opt.word_sep && sep >= 0 && cls.size() > 1) {
            cls.push_back(sep);
            filler.push_back(0);
            wild.push_back(0);
            cls.push_back(blank);
            filler.push_back(new_line);
            wild.push_back(0);
        }
        first[i] = (int)cls.size();
        for (int c : tk) {
            if (c < 0) {
                cls.push_back(blank);
                filler.push_back(1);
                wild.push_back(1);
                continue;
            }
            cls.push_back(c);
            filler.push_back(0);
            wild.push_back(0);
            cls.push_back(blank);
            filler.push_back(0);
            wild.push_back(0);
        }
        last[i] = std::max(first[i], (int)cls.size() - 2);
    }
    filler.back() = 1;  // outro
    const size_t S = cls.size();
    std::vector<uint8_t> can_skip(S, 0);
    for (size_t s = 2; s < S; s++) can_skip[s] = cls[s] != blank && cls[s] != cls[s - 2] && !wild[s - 1];
    std::vector<double> anchor_s(S, -1.0);
    std::vector<char> seen(anchors.size(), 0);
    for (size_t i = 0; i < words.size(); i++) {
        if (first[i] < 0 || line_of.empty()) continue;
        int ln = line_of[i];
        if (ln < 0 || ln >= (int)anchors.size() || seen[ln] || !anchors[ln]) continue;
        seen[ln] = 1;
        anchor_s[first[i]] = *anchors[ln];
    }
    if (em.frames() < S / 2 + 1) die("audio too short for these lyrics");
    // one backpointer byte per frame and state: ~60 MB for a 4-minute song, but hours of audio would need GBs
    if ((double)em.frames() * S > 3e9)
        die("too long to align in one pass (%.0f min of audio, %zu letters): split the song", em.frames() * em.frame_s / 60, S / 2);
    std::vector<int> path = viterbi(em, cls, can_skip, filler, anchor_s, opt);

    std::vector<long long> f_first(S, -1), f_last(S, -1);
    for (size_t t = 0; t < path.size(); t++) {
        if (f_first[path[t]] < 0) f_first[path[t]] = (long long)t;
        f_last[path[t]] = (long long)t;
    }
    std::vector<Word> out(words.size());
    std::vector<char> ok(words.size(), 0);
    for (size_t i = 0; i < words.size(); i++) {
        out[i].word = words[i];
        out[i].line = line_of.empty() ? -1 : line_of[i];
        if (first[i] < 0 || f_first[first[i]] < 0) continue;
        long long f0 = f_first[first[i]], f1 = std::max(f_last[last[i]], f0);
        double sum = 0;
        int cnt = 0;
        for (long long t = f0; t <= f1; t++)
            if (cls[path[t]] != blank) sum += em.logp[(size_t)t * em.classes + cls[path[t]]], cnt++;
        out[i].start = f0 * em.frame_s;
        out[i].end = (f1 + 1) * em.frame_s;
        out[i].score = cnt ? std::exp(sum / cnt) : 0.0;
        ok[i] = 1;
    }
    for (size_t i = 0; i < words.size(); i++) {  // unalignable words: between their neighbours
        if (ok[i]) continue;
        double lo = 0, hi = -1;
        for (long long j = (long long)i - 1; j >= 0; j--)
            if (ok[j]) { lo = out[j].end; break; }
        for (size_t j = i + 1; j < words.size(); j++)
            if (ok[j]) { hi = out[j].start; break; }
        if (hi < 0) hi = lo + 0.3;
        out[i].start = lo;
        out[i].end = std::max(lo + 0.05, std::min(hi, lo + 0.3));
    }
    return out;
}

}  // namespace mt
