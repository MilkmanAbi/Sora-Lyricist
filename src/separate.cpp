#include "separate.h"

#include <cmath>
#include <numeric>

#include "dsp.h"
#include "infer.h"

namespace mt {

static const int kSeg = 343980;   // 7.8 s at 44.1 kHz (the model's training segment)
static const int kNfft = 4096, kHop = 1024;
static const int kFrames = 336;   // ceil(kSeg / kHop)
static const int kFreqs = 2048;
static const int kVocals = 3;     // sources: drums, bass, other, vocals

// HTDemucs._spec for one channel: reflect-pad by 3/2 hop (+ round up), STFT, drop the Nyquist bin, keep frames 2..2+le
static void demucs_spec(const float* x, float* re, float* im) {
    const int le = kFrames, pad = kHop / 2 * 3;
    std::vector<float> xp;
    reflect_pad(x, kSeg, pad, pad + (size_t)le * kHop - kSeg, xp);
    size_t frames;
    std::vector<cfloat> z = stft(xp.data(), xp.size(), kNfft, kHop, true, frames);  // frames == le + 4
    for (int f = 0; f < kFreqs; f++)
        for (int t = 0; t < le; t++) {
            cfloat v = z[(size_t)f * frames + t + 2];
            re[(size_t)f * le + t] = v.real();
            im[(size_t)f * le + t] = v.imag();
        }
}

// HTDemucs._ispec: zero Nyquist bin, two empty frames either side, iSTFT, trim the 3/2-hop padding
static std::vector<float> demucs_ispec(const float* re, const float* im) {
    const int le = kFrames, T = le + 4, pad = kHop / 2 * 3;
    std::vector<cfloat> z((size_t)(kFreqs + 1) * T, cfloat(0, 0));
    for (int f = 0; f < kFreqs; f++)
        for (int t = 0; t < le; t++) z[(size_t)f * T + t + 2] = cfloat(re[(size_t)f * le + t], im[(size_t)f * le + t]);
    size_t full = (size_t)kHop * (size_t)std::ceil((double)kSeg / kHop) + 2 * pad;
    std::vector<float> x = istft(z.data(), T, kNfft, kHop, true, full);
    return std::vector<float>(x.begin() + pad, x.begin() + pad + kSeg);
}

Audio separate_vocals(const Audio& mix, const fs::path& models, const std::string& model, const std::string& device,
                     const Cache& cache, const Progress& on_progress) {
    if (mix.channels != 2 || mix.sr != 44100) die("separate_vocals wants stereo 44.1 kHz");
    const size_t n = mix.frames();
    // demucs.separate: normalise by the mono mix's mean / std (unbiased), undo on the output
    std::vector<float> ref = mix.mono();
    double mean = std::accumulate(ref.begin(), ref.end(), 0.0) / n, var = 0;
    for (float v : ref) var += (v - mean) * (v - mean);
    double sd = std::sqrt(var / std::max<size_t>(1, n - 1));
    std::vector<float> L(n), R(n);
    for (size_t i = 0; i < n; i++) {
        L[i] = (float)((mix.data[2 * i] - mean) / sd);
        R[i] = (float)((mix.data[2 * i + 1] - mean) / sd);
    }

    const size_t stride = (size_t)(0.75 * kSeg);  // overlap 0.25
    std::vector<size_t> offsets;
    for (size_t o = 0; o < n; o += stride) offsets.push_back(o);
    std::vector<float> weight(kSeg);  // triangle cross-fade, peak 1
    for (int i = 0; i < kSeg / 2; i++) weight[i] = (float)(i + 1) / (kSeg / 2);
    for (int i = kSeg / 2; i < kSeg; i++) weight[i] = (float)(kSeg - i) / (kSeg / 2);

    std::vector<double> outL(n, 0), outR(n, 0), wsum(n, 0);
    std::unique_ptr<Net> net;
    const uint64_t model_key = fnv(model + "/v1");
    struct Job {
        size_t offset, len;
        uint64_t key;
        std::vector<float> chunk;  // [2][kSeg] model input (normalised, centred padding)
        bool cached = false;
        std::vector<float> vocals;  // [2][kSeg]
    };
    std::vector<Job> jobs(offsets.size());
    for (size_t k = 0; k < offsets.size(); k++) {
        Job& j = jobs[k];
        j.offset = offsets[k];
        j.len = std::min<size_t>(kSeg, n - j.offset);
        // TensorChunk.padded(kSeg): centred on the chunk, real audio as context where there is some, zeros beyond
        long long delta = kSeg - (long long)j.len, start = (long long)j.offset - delta / 2;
        j.chunk.assign(2 * (size_t)kSeg, 0.f);
        for (int i = 0; i < kSeg; i++) {
            long long s = start + i;
            if (s < 0 || s >= (long long)n) continue;
            j.chunk[i] = L[s];
            j.chunk[kSeg + i] = R[s];
        }
        j.key = fnv(j.chunk.data(), j.chunk.size() * sizeof(float), model_key);
        j.cached = cache.get("demucs", j.key, j.vocals) && j.vocals.size() == 2 * (size_t)kSeg;
    }
    size_t todo = 0;
    for (auto& j : jobs) todo += !j.cached;
    log("separating vocals: %zu chunks (%zu from the SSD cache)", jobs.size(), jobs.size() - todo);

    auto vocals_of = [&](Net& m, int r) {  // model outputs of request r -> vocal waveform of the chunk
        const float* spec = m.output(0, r);   // [1][4 src][4][2048][336]
        const float* wave = m.output(1, r);   // [1][4 src][2][kSeg]
        const size_t plane = (size_t)kFreqs * kFrames;
        const float* v = spec + (size_t)kVocals * 4 * plane;
        std::vector<float> out(2 * (size_t)kSeg);
        for (int c = 0; c < 2; c++) {
            std::vector<float> x = demucs_ispec(v + (2 * c) * plane, v + (2 * c + 1) * plane);
            const float* xt = wave + ((size_t)kVocals * 2 + c) * kSeg;
            for (int i = 0; i < kSeg; i++) out[(size_t)c * kSeg + i] = x[i] + xt[i];
        }
        return out;
    };
    auto finish = [&](Job& j, int r) {
        j.vocals = vocals_of(*net, r);
        cache.put("demucs", j.key, j.vocals.data(), j.vocals.size());
    };
    auto prepare_on = [&](Net& m, Job& j, int r) {
        float* mixin = m.input(0, r);
        float* magin = m.input(1, r);
        memcpy(mixin, j.chunk.data(), j.chunk.size() * sizeof(float));
        const size_t plane = (size_t)kFreqs * kFrames;
        for (int c = 0; c < 2; c++) demucs_spec(j.chunk.data() + (size_t)c * kSeg, magin + (2 * c) * plane, magin + (2 * c + 1) * plane);
    };
    auto prepare = [&](Job& j, int r) { prepare_on(*net, j, r); };

    // overlap-add in chunk order; once chunk k is in, everything before chunk k+1's start is final and is handed to
    // on_progress (the aligner starts on it while the device separates the rest)
    Audio voc;
    voc.sr = 44100;
    voc.channels = 2;
    voc.data.assign(2 * n, 0.f);
    size_t next = 0, final_frames = 0;
    auto accumulate = [&] {
        while (next < jobs.size() && jobs[next].vocals.size() == 2 * (size_t)kSeg) {
            Job& j = jobs[next];
            long long delta = kSeg - (long long)j.len, lo = delta / 2;  // center_trim to the chunk's own samples
            for (size_t i = 0; i < j.len; i++) {
                double w = weight[i];
                outL[j.offset + i] += w * j.vocals[lo + i];
                outR[j.offset + i] += w * j.vocals[(size_t)kSeg + lo + i];
                wsum[j.offset + i] += w;
            }
            std::vector<float>().swap(j.chunk);
            std::vector<float>().swap(j.vocals);
            next++;
            size_t upto = next < jobs.size() ? jobs[next].offset : n;
            for (size_t i = final_frames; i < upto; i++) {
                voc.data[2 * i] = (float)(outL[i] / wsum[i] * sd + mean);
                voc.data[2 * i + 1] = (float)(outR[i] / wsum[i] * sd + mean);
            }
            final_frames = upto;
            progress("separate", (double)next, (double)jobs.size());
            if (on_progress) on_progress(voc, final_frames);
        }
    };
    accumulate();  // leading chunks that came from the cache
    if (todo) {
        // fp32 on a GPU: Demucs overflows in fp16 on real music (vocal stem at -18 dB SDR, tools/gpu_demucs_check.py)
        net = std::make_unique<Net>(model_path(models, model, device), device, 2, /*gpu_fp16=*/false);
        if (is_dml(device)) {
            // Trust, but verify: the first chunk also runs on the CPU. A GPU whose driver mis-computes Demucs, or one
            // too slow to finish it inside Windows' GPU watchdog (an Intel iGPU through DirectML: "GPU will not respond"
            // and garbage), would otherwise hand the aligner noise. Costs one CPU chunk.
            Job* first = nullptr;
            for (auto& j : jobs)
                if (!j.cached) {
                    first = &j;
                    break;
                }
            std::vector<float> g;
            std::string why;
            try {
                prepare_on(*net, *first, 0);
                net->infer(0);
                g = vocals_of(*net, 0);
            } catch (const std::exception& e) {
                why = e.what();
            }
            Net cpu(models / (model + ".xml"), "CPU", 1, false);
            prepare_on(cpu, *first, 0);
            cpu.infer(0);
            first->vocals = vocals_of(cpu, 0);
            double sig = 0, err = 0;
            for (size_t i = 0; i < g.size(); i++) {
                sig += (double)first->vocals[i] * first->vocals[i];
                err += ((double)first->vocals[i] - g[i]) * ((double)first->vocals[i] - g[i]);
            }
            double db = g.empty() ? -99 : 10 * std::log10(sig / (err + 1e-20));
            if (db >= 30) {
                log("  GPU check: DirectML matches the CPU on the first chunk (%.0f dB)", db);
            } else {
                std::string what = why.empty() ? "differs from the CPU (" + std::to_string((int)db) + " dB)" : why;
                log("  GPU check: DirectML %s - separating on the Intel GPU / CPU instead (and from now on)", what.c_str());
                mark_gpu_bad(device, model, what);
                net = std::make_unique<Net>(models / (model + ".xml"), "AUTO", 2, /*gpu_fp16=*/false);
            }
            cache.put("demucs", first->key, first->vocals.data(), first->vocals.size());
            first->cached = true;
            todo--;
            accumulate();
        }
        Timer t;
        int r = 0;
        Job* running = nullptr;
        int running_r = 0;
        size_t done = 0;
        for (auto& j : jobs) {
            if (j.cached) continue;
            prepare(j, r);  // CPU: STFT of this chunk while the device still works on the previous one
            net->start(r);
            if (running) {
                net->wait(running_r);
                finish(*running, running_r);
                log("  chunk %zu/%zu (%.1fs per chunk)", ++done, todo, t.s() / done);
                accumulate();
            }
            running = &j;
            running_r = r;
            r ^= 1;
        }
        if (running) {
            net->wait(running_r);
            finish(*running, running_r);
            log("  chunk %zu/%zu (%.1fs per chunk)", ++done, todo, t.s() / done);
        }
    }
    accumulate();
    return voc;
}

}  // namespace mt
