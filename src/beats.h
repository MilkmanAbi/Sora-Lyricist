// Beats and downbeats with Beat This! (final0, minimal post-processing), as analyze_song.py's File2Beats(dbn=False).
#pragma once
#include "audio.h"
#include "cache.h"

namespace mt {

struct Beats {
    std::vector<double> beats, downbeats;
    double bpm = 0;
};

// mono22k: the mix at 22.05 kHz, channels averaged
Beats track_beats(const std::vector<float>& mono22k, const fs::path& models, const std::string& device, const Cache& cache);

}  // namespace mt
