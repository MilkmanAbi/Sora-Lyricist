// Vocal separation with Demucs v4 (htdemucs), the same model and chunking as `demucs --two-stems=vocals`.
#pragma once
#include <functional>

#include "audio.h"
#include "cache.h"

namespace mt {

// mix: stereo 44.1 kHz. Returns the vocal stem (stereo 44.1 kHz).
// model: IR base name in `models` (htdemucs_core or htdemucs_core_int8)
// on_progress(stem, frames): stem frames [0, frames) are final (called after every chunk, in order)
using Progress = std::function<void(const Audio& stem, size_t frames)>;
Audio separate_vocals(const Audio& mix, const fs::path& models, const std::string& model, const std::string& device,
                     const Cache& cache, const Progress& on_progress = {});

}  // namespace mt
