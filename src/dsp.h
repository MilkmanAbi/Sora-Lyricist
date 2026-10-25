// Spectral front-ends, written to match the PyTorch/torchaudio/librosa calls the models were trained with
// (same window, padding, normalisation), so the exported networks see the inputs they expect.
#pragma once
#include <complex>
#include <vector>

namespace mt {

using cfloat = std::complex<float>;

// torch.stft(x, n_fft, hop, window=hann(n_fft) periodic, center=True, pad_mode='reflect', normalized=normalized)
// -> [n_fft/2+1][frames] row-major (freq-major), frames = 1 + len/hop
std::vector<cfloat> stft(const float* x, size_t n, int n_fft, int hop, bool normalized, size_t& frames);
// torch.istft(z, n_fft, hop, window=hann(n_fft), center=True, normalized=normalized, length=length)
std::vector<float> istft(const cfloat* z, size_t frames, int n_fft, int hop, bool normalized, size_t length);

void reflect_pad(const float* x, size_t n, size_t left, size_t right, std::vector<float>& out);

// torchaudio.functional.melscale_fbanks(..., norm=None, mel_scale='slaney') -> [n_freqs][n_mels]
std::vector<float> mel_fbanks_torchaudio(int n_freqs, float f_min, float f_max, int n_mels, int sr);

// Beat This! input: log1p(1000 * melspec) at 22.05 kHz, n_fft 1024, hop 441, 128 slaney mels 30..11000 Hz
// -> [frames][128]
std::vector<float> beat_this_logmel(const std::vector<float>& mono22k, size_t& frames);

// librosa.onset.onset_strength(y, sr, hop_length) on a 22.05 kHz signal (n_fft 2048, 128 mels, power dB, lag 1,
// max_size 1, mean aggregate, centered) -> envelope per hop
std::vector<float> librosa_onset_strength(const std::vector<float>& y, int sr, int hop);
// librosa.onset.onset_detect(onset_envelope=env, backtrack=True) -> frame indices
std::vector<int> librosa_onset_detect(const std::vector<float>& env, int sr, int hop);

// windowed-sinc (Kaiser) resampling of x (n samples at in_sr) to out_sr, for output samples [j0, j1). Output j only
// reads input samples < j * in_sr / out_sr + resample_reach(in_sr, out_sr), so a growing input can be resampled as it
// arrives (the aligner starts on vocals while the separator is still running).
void resample(const float* x, size_t n, int in_sr, int out_sr, size_t j0, size_t j1, float* out);
size_t resample_reach(int in_sr, int out_sr);

}  // namespace mt
