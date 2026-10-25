#include "beats.h"

#include <algorithm>
#include <cmath>

#include "dsp.h"
#include "infer.h"

namespace mt {

static const int kChunk = 1500, kBorder = 6, kMels = 128;
static const double kFps = 50.0;

// framewise logits -> peak frames: local maxima within +/-3 frames (70 ms) with p > 0.5, adjacent peaks merged
static std::vector<double> peaks(const std::vector<float>& logit) {
    const int n = (int)logit.size();
    std::vector<int> hits;
    for (int i = 0; i < n; i++) {
        if (logit[i] <= 0) continue;
        float m = logit[i];
        for (int k = std::max(0, i - 3); k <= std::min(n - 1, i + 3); k++) m = std::max(m, logit[k]);
        if (logit[i] == m) hits.push_back(i);
    }
    std::vector<double> out;  // deduplicate_peaks(width=1): runs of neighbouring frames -> their running mean
    double p = 0;
    int c = 0;
    for (int h : hits) {
        if (c && h - p <= 1) {
            c++;
            p += (h - p) / c;
        } else {
            if (c) out.push_back(p);
            p = h;
            c = 1;
        }
    }
    if (c) out.push_back(p);
    for (double& f : out) f /= kFps;
    return out;
}

Beats track_beats(const std::vector<float>& mono22k, const fs::path& models, const std::string& device, const Cache& cache) {
    size_t frames;
    std::vector<float> spec = beat_this_logmel(mono22k, frames);
    const long long T = (long long)frames;
    // split_piece(chunk 1500, border 6, avoid_short_end)
    std::vector<long long> starts;
    for (long long s = -kBorder; s < T - kBorder; s += kChunk - 2 * kBorder) starts.push_back(s);
    if (T > kChunk - 2 * kBorder) starts.back() = T - (kChunk - kBorder);
    std::vector<float> beat(frames, -1000.f), down(frames, -1000.f);
    std::unique_ptr<Net> net;
    const uint64_t model_key = fnv("beat_this_final0/v1");
    // keep_first: earlier windows win where they overlap, so fill in reverse order
    for (long long k = (long long)starts.size() - 1; k >= 0; k--) {
        long long s = starts[k];
        std::vector<float> x((size_t)kChunk * kMels, 0.f);  // zero = the padding split_piece adds
        for (long long t = std::max(0LL, s); t < std::min(s + kChunk, T); t++)
            std::copy_n(spec.data() + t * kMels, kMels, x.data() + (t - s) * kMels);
        uint64_t key = fnv(x.data(), x.size() * sizeof(float), model_key);
        std::vector<float> out;
        if (!cache.get("beats", key, out) || out.size() != 2 * (size_t)kChunk) {
            bool verify = false;
            if (!net) {  // fp16 on an OpenVINO GPU moved logits by 0.35: fp32 there; the DirectML model is fp32 too
                net = std::make_unique<Net>(model_path(models, "beat_this", device), device, 1, /*gpu_fp16=*/false);
                verify = is_dml(device);
            }
            std::copy(x.begin(), x.end(), net->input(0));
            std::string why;
            try {
                net->infer();
            } catch (const std::exception& e) {  // a GPU that fails (driver reset, ...): the CPU takes over
                if (net->device() == "CPU") throw;
                why = e.what();
            }
            if (verify && why.empty()) {  // first DirectML window: the same logits as the CPU (fp32 both: ~1e-5 apart)
                Net cpu(model_path(models, "beat_this", "CPU"), "CPU", 1, false);
                std::copy(x.begin(), x.end(), cpu.input(0));
                cpu.infer();
                double diff = 0;
                for (int o = 0; o < 2; o++)
                    for (int i = 0; i < kChunk; i++) diff = std::max(diff, (double)std::fabs(cpu.output(o)[i] - net->output(o)[i]));
                if (diff > 0.05) why = "differs from the CPU (logits " + std::to_string(diff).substr(0, 5) + " apart)";
                else log("  GPU check: DirectML beat tracker matches the CPU");
            }
            if (!why.empty()) {
                log("  beat tracker: %s %s - using the CPU (and from now on)", net->device().c_str(), why.c_str());
                if (is_dml(net->device())) mark_gpu_bad(net->device(), "beat_this", why);
                net = std::make_unique<Net>(model_path(models, "beat_this", "CPU"), "CPU", 1, false);
                std::copy(x.begin(), x.end(), net->input(0));
                net->infer();
            }
            out.assign(net->output(0), net->output(0) + kChunk);
            out.insert(out.end(), net->output(1), net->output(1) + kChunk);
            cache.put("beats", key, out.data(), out.size());
        }
        for (long long t = s + kBorder; t < s + kChunk - kBorder; t++) {
            if (t < 0 || t >= T) continue;
            beat[t] = out[t - s];
            down[t] = out[kChunk + t - s];
        }
    }
    Beats b;
    b.beats = peaks(beat);
    std::vector<double> d = peaks(down);
    for (double& t : d) {  // each downbeat moves onto the nearest beat
        if (b.beats.empty()) break;
        auto it = std::min_element(b.beats.begin(), b.beats.end(), [&](double a, double c) { return std::fabs(a - t) < std::fabs(c - t); });
        t = *it;
    }
    std::sort(d.begin(), d.end());
    d.erase(std::unique(d.begin(), d.end()), d.end());
    b.downbeats = d;
    if (b.beats.size() > 1) {
        std::vector<double> diff;
        for (size_t i = 1; i < b.beats.size(); i++) diff.push_back(b.beats[i] - b.beats[i - 1]);
        std::nth_element(diff.begin(), diff.begin() + diff.size() / 2, diff.end());
        double med = diff[diff.size() / 2];
        if (diff.size() % 2 == 0) {  // numpy median of an even count: mean of the two middle values
            double lo = *std::max_element(diff.begin(), diff.begin() + diff.size() / 2);
            med = (med + lo) / 2;
        }
        b.bpm = 60.0 / med;
    }
    return b;
}

}  // namespace mt
