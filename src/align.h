// Lyric alignment: wav2vec2 character log-probs over the whole vocal stem, then one CTC Viterbi pass that places
// every word. Line timestamps (.lrc) are soft anchors, not crop windows (see tools/ctc_align.py in the Python tool).
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cache.h"

namespace mt {

struct Emissions {
    int classes = 0;
    double frame_s = 0.02;
    std::vector<float> logp;  // [frames][classes]
    std::vector<std::string> labels;
    size_t frames() const { return classes ? logp.size() / classes : 0; }
};

// wav2vec2 log-probs, computed window by window as the 16 kHz vocals become available (see main.cpp: the CPU aligns
// the start of the song while the iGPU is still separating the end). model: "w2v_large[_int8]" | "w2v_base"
class EmissionStream {
public:
    EmissionStream(const fs::path& models, const std::string& model, const std::string& device, const Cache& cache,
                   size_t total_samples);
    ~EmissionStream();
    // v16: the vocals at 16 kHz, of which samples [0, avail) are final; computes every window that is now complete
    void update(const std::vector<float>& v16, size_t avail);
    bool done() const { return next_ >= total_; }
    Emissions take();

private:
    fs::path models_;
    std::string model_, device_;
    const Cache& cache_;
    Emissions em_;
    size_t total_, next_ = 0, windows_ = 0, computed_ = 0;
    std::unique_ptr<class Net> net_;
    double seconds_ = 0;
};

struct Word {
    std::string word;
    double start = 0, end = 0, score = 0;
    int line = -1;
    bool line_start = false;
};

// defaults tuned with tools/sweep.py on JamendoLyrics songs 1-4 (normal + slowed, exact + noisy timestamps) and checked
// on held-out songs 5-8 (README)
struct AlignOptions {
    double slack = 0.5;       // a line may start this far from its timestamp for free
    double weight = 40.0;     // cost per second beyond that (log-prob units)
    double filler_logp = -0.75; // per-frame cost of unlyric'd singing (ad-libs, backing vocals) between lines
    bool word_sep = false;   // put the model's '|' between words, as WhisperX does
};

// words: as shown (song.json); letters: what to listen for per word (roman.h align_letters: a-z, apostrophes, and
// '*' for a sound the tables can't spell, which may take any singing)
std::vector<Word> align(const Emissions& em, const std::vector<std::string>& words, const std::vector<std::string>& letters,
                        const std::vector<int>& line_of, const std::vector<std::optional<double>>& anchors,
                        const AlignOptions& opt);

}  // namespace mt
