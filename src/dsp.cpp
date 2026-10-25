#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "pocketfft_hdronly.hpp"

namespace mt {

static const double kPi = 3.14159265358979323846;

static std::vector<float> hann(int n) {  // periodic (torch.hann_window / scipy fftbins=True)
    std::vector<float> w(n);
    for (int i = 0; i < n; i++) w[i] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * i / n));
    return w;
}

void reflect_pad(const float* x, size_t n, size_t left, size_t right, std::vector<float>& out) {
    out.resize(n + left + right);
    auto at = [&](long long i) {  // numpy 'reflect' (edge sample not repeated), folded for very short inputs
        long long m = (long long)n;
        if (m == 1) return x[0];
        long long p = 2 * (m - 1);
        i %= p;
        if (i < 0) i += p;
        return x[i < m ? i : p - i];
    };
    for (size_t i = 0; i < out.size(); i++) out[i] = at((long long)i - (long long)left);
}

static std::vector<cfloat> stft_padded(const std::vector<float>& xp, size_t frames, int n_fft, int hop, float scale) {
    const int nf = n_fft / 2 + 1;
    std::vector<float> win = hann(n_fft);
    std::vector<cfloat> out((size_t)nf * frames);
#pragma omp parallel
    {
        std::vector<float> buf(n_fft);
        std::vector<cfloat> spec(nf);
#pragma omp for schedule(static)
        for (long long t = 0; t < (long long)frames; t++) {
            const float* src = xp.data() + (size_t)t * hop;
            for (int i = 0; i < n_fft; i++) buf[i] = src[i] * win[i];
            pocketfft::r2c<float>({(size_t)n_fft}, {sizeof(float)}, {sizeof(cfloat)}, 0, true, buf.data(), spec.data(), scale);
            for (int f = 0; f < nf; f++) out[(size_t)f * frames + t] = spec[f];
        }
    }
    return out;
}

std::vector<cfloat> stft(const float* x, size_t n, int n_fft, int hop, bool normalized, size_t& frames) {
    std::vector<float> xp;
    reflect_pad(x, n, n_fft / 2, n_fft / 2, xp);
    frames = 1 + n / hop;
    return stft_padded(xp, frames, n_fft, hop, normalized ? 1.0f / std::sqrt((float)n_fft) : 1.0f);
}

std::vector<float> istft(const cfloat* z, size_t frames, int n_fft, int hop, bool normalized, size_t length) {
    const int nf = n_fft / 2 + 1;
    std::vector<float> win = hann(n_fft);
    size_t total = (size_t)n_fft + (size_t)hop * (frames - 1);
    std::vector<float> frames_out((size_t)frames * n_fft);
    float scale = (normalized ? std::sqrt((float)n_fft) : 1.0f) / n_fft;
#pragma omp parallel
    {
        std::vector<cfloat> spec(nf);
#pragma omp for schedule(static)
        for (long long t = 0; t < (long long)frames; t++) {
            for (int f = 0; f < nf; f++) spec[f] = z[(size_t)f * frames + t];
            spec[0].imag(0);  // irfft ignores the imaginary part of DC / Nyquist, as torch does
            spec[nf - 1].imag(0);
            float* dst = frames_out.data() + (size_t)t * n_fft;
            pocketfft::c2r<float>({(size_t)n_fft}, {sizeof(cfloat)}, {sizeof(float)}, 0, false, spec.data(), dst, scale);
            for (int i = 0; i < n_fft; i++) dst[i] *= win[i];
        }
    }
    std::vector<float> y(total, 0.f), env(total, 0.f);
    for (size_t t = 0; t < frames; t++) {
        const float* src = frames_out.data() + t * n_fft;
        size_t o = t * hop;
        for (int i = 0; i < n_fft; i++) {
            y[o + i] += src[i];
            env[o + i] += win[i] * win[i];
        }
    }
    std::vector<float> out(length, 0.f);
    size_t start = n_fft / 2;
    for (size_t i = 0; i < length && start + i < total; i++) {
        float e = env[start + i];
        out[i] = e > 1e-11f ? y[start + i] / e : 0.f;
    }
    return out;
}

static double hz_to_mel_slaney(double f) {
    const double f_sp = 200.0 / 3, min_log_hz = 1000.0, min_log_mel = min_log_hz / f_sp, logstep = std::log(6.4) / 27.0;
    return f >= min_log_hz ? min_log_mel + std::log(f / min_log_hz) / logstep : f / f_sp;
}

static double mel_to_hz_slaney(double m) {
    const double f_sp = 200.0 / 3, min_log_hz = 1000.0, min_log_mel = min_log_hz / f_sp, logstep = std::log(6.4) / 27.0;
    return m >= min_log_mel ? min_log_hz * std::exp(logstep * (m - min_log_mel)) : f_sp * m;
}

// triangular slaney filters over the given FFT bin frequencies; area-normalised when `slaney_norm` (librosa default)
static std::vector<float> tri_filters(const std::vector<double>& bin_hz, double f_min, double f_max, int n_mels,
                                      bool slaney_norm) {
    double m0 = hz_to_mel_slaney(f_min), m1 = hz_to_mel_slaney(f_max);
    std::vector<double> f_pts(n_mels + 2);
    for (int i = 0; i < n_mels + 2; i++) f_pts[i] = mel_to_hz_slaney(m0 + (m1 - m0) * i / (n_mels + 1));
    const int nb = (int)bin_hz.size();
    std::vector<float> fb((size_t)nb * n_mels, 0.f);
    for (int m = 0; m < n_mels; m++) {
        double d0 = f_pts[m + 1] - f_pts[m], d1 = f_pts[m + 2] - f_pts[m + 1];
        double norm = slaney_norm ? 2.0 / (f_pts[m + 2] - f_pts[m]) : 1.0;
        for (int b = 0; b < nb; b++) {
            double down = (bin_hz[b] - f_pts[m]) / d0, up = (f_pts[m + 2] - bin_hz[b]) / d1;
            fb[(size_t)b * n_mels + m] = (float)(std::max(0.0, std::min(down, up)) * norm);
        }
    }
    return fb;
}

std::vector<float> mel_fbanks_torchaudio(int n_freqs, float f_min, float f_max, int n_mels, int sr) {
    std::vector<double> hz(n_freqs);
    for (int i = 0; i < n_freqs; i++) hz[i] = (double)(sr / 2) * i / (n_freqs - 1);  // linspace(0, sr // 2, n_freqs)
    return tri_filters(hz, f_min, f_max, n_mels, false);
}

std::vector<float> beat_this_logmel(const std::vector<float>& y, size_t& frames) {
    const int n_fft = 1024, hop = 441, nf = n_fft / 2 + 1, nm = 128;
    static const std::vector<float> fb = mel_fbanks_torchaudio(nf, 30.f, 11000.f, nm, 22050);
    std::vector<cfloat> z = stft(y.data(), y.size(), n_fft, hop, true, frames);
    std::vector<float> out(frames * nm);
#pragma omp parallel for schedule(static)
    for (long long t = 0; t < (long long)frames; t++) {
        float acc[nm] = {};
        for (int f = 0; f < nf; f++) {
            float mag = std::abs(z[(size_t)f * frames + t]);
            if (mag == 0.f) continue;
            const float* row = fb.data() + (size_t)f * nm;
            for (int m = 0; m < nm; m++) acc[m] += mag * row[m];
        }
        for (int m = 0; m < nm; m++) out[(size_t)t * nm + m] = std::log1p(1000.f * acc[m]);
    }
    return out;
}

std::vector<float> librosa_onset_strength(const std::vector<float>& y, int sr, int hop) {
    const int n_fft = 2048, nf = n_fft / 2 + 1, nm = 128;
    std::vector<double> hz(nf);
    for (int i = 0; i < nf; i++) hz[i] = (double)sr / 2 * i / (nf - 1);
    const std::vector<float> fb = tri_filters(hz, 0.0, sr / 2.0, nm, true);
    // librosa >= 0.10 centres frames with zero padding (pad_mode='constant')
    std::vector<float> xp(y.size() + n_fft, 0.f);
    std::copy(y.begin(), y.end(), xp.begin() + n_fft / 2);
    size_t frames = 1 + y.size() / hop;
    std::vector<cfloat> z = stft_padded(xp, frames, n_fft, hop, 1.0f);
    std::vector<float> db(frames * nm);
    float top = -1e30f;
#pragma omp parallel for schedule(static)
    for (long long t = 0; t < (long long)frames; t++) {
        double acc[nm] = {};
        for (int f = 0; f < nf; f++) {
            float p = std::norm(z[(size_t)f * frames + t]);
            if (p == 0.f) continue;
            const float* row = fb.data() + (size_t)f * nm;
            for (int m = 0; m < nm; m++) acc[m] += (double)p * row[m];
        }
        for (int m = 0; m < nm; m++) db[(size_t)t * nm + m] = (float)(10.0 * std::log10(std::max(1e-10, acc[m])));
    }
    for (float v : db) top = std::max(top, v);
    for (float& v : db) v = std::max(v, top - 80.f);  // power_to_db(top_db=80)
    // spectral flux (lag 1), half-wave rectified, mean over bands; padded by lag + n_fft // (2 hop) at the front
    size_t pad = 1 + n_fft / (2 * hop);
    std::vector<float> env(frames, 0.f);
    for (size_t t = 1; t < frames; t++) {
        double s = 0;
        for (int m = 0; m < nm; m++) s += std::max(0.f, db[t * nm + m] - db[(t - 1) * nm + m]);
        size_t o = t - 1 + pad;
        if (o < frames) env[o] = (float)(s / nm);
    }
    return env;
}

std::vector<int> librosa_onset_detect(const std::vector<float>& env_in, int sr, int hop) {
    std::vector<float> x = env_in;
    if (x.empty()) return {};
    float mn = *std::min_element(x.begin(), x.end());
    for (float& v : x) v -= mn;
    float mx = *std::max_element(x.begin(), x.end());
    if (mx > 0)
        for (float& v : x) v /= mx;
    const int pre_max = (int)std::floor(0.03 * sr / hop), post_max = 1;
    const int pre_avg = (int)std::floor(0.10 * sr / hop), post_avg = pre_avg + 1;
    const int wait = pre_max;
    const float delta = 0.07f;
    const int n = (int)x.size();
    std::vector<int> peaks;
    for (int i = 0; i < n; i++) {  // librosa.util.peak_pick
        int a = std::max(0, i - pre_max), b = std::min(n, i + post_max);
        float m = *std::max_element(x.begin() + a, x.begin() + b);
        if (x[i] != m) continue;
        a = std::max(0, i - pre_avg);
        b = std::min(n, i + post_avg);
        double avg = std::accumulate(x.begin() + a, x.begin() + b, 0.0) / (b - a);
        if (x[i] < avg + delta) continue;
        peaks.push_back(i);
        i += wait;
    }
    // onset_backtrack: each onset moves back to the preceding local minimum of the envelope
    std::vector<int> minima{0};
    for (int i = 1; i + 1 < n; i++)
        if (x[i] <= x[i - 1] && x[i] < x[i + 1]) minima.push_back(i);
    std::vector<int> out;
    for (int p : peaks) {
        auto it = std::upper_bound(minima.begin(), minima.end(), p);
        out.push_back(*(it - 1));
    }
    return out;
}

static const int kHalfTaps = 32;  // filter half-length, in output samples

size_t resample_reach(int in_sr, int out_sr) {
    return (size_t)std::ceil((double)kHalfTaps * in_sr / out_sr) + 2;
}

void resample(const float* x, size_t n, int in_sr, int out_sr, size_t j0, size_t j1, float* out) {
    // polyphase: output j sits at input position j*M/L; its fractional part cycles through L phases, so the Kaiser-sinc
    // taps are computed once per phase (44.1 -> 16 kHz: L = 160, M = 441)
    const long long g = std::gcd(in_sr, out_sr), L = out_sr / g, M = in_sr / g;
    const double step = (double)in_sr / out_sr;
    const double rho = 0.9 * std::min(1.0, (double)out_sr / in_sr);  // cutoff, as a fraction of the input Nyquist
    const double W = kHalfTaps * std::max(1.0, step);                 // half-width in input samples
    const int Wi = (int)std::ceil(W), T = 2 * Wi + 2;
    const double beta = 8.6, i0b = std::cyl_bessel_i(0.0, beta);
    std::vector<float> table((size_t)L * T);
    for (long long ph = 0; ph < L; ph++)
        for (int k = 0; k < T; k++) {
            double t = (k - Wi) - (double)ph / L, u = t / W;
            double s = std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * rho * t) / (kPi * rho * t);
            table[(size_t)ph * T + k] = std::fabs(u) >= 1 ? 0.f : (float)(s * std::cyl_bessel_i(0.0, beta * std::sqrt(1 - u * u)) / i0b);
        }
#pragma omp parallel for schedule(static)
    for (long long j = (long long)j0; j < (long long)j1; j++) {
        long long num = j * M, base = num / L, ph = num % L;
        const float* h = table.data() + (size_t)ph * T;
        double acc = 0, norm = 0;
        for (int k = 0; k < T; k++) {
            long long i = base + k - Wi;
            if (i < 0 || i >= (long long)n) continue;
            acc += (double)h[k] * x[i];
            norm += h[k];
        }
        out[j - j0] = norm != 0 ? (float)(acc / norm) : 0.f;  // unit DC gain, also at the edges
    }
}

}  // namespace mt
