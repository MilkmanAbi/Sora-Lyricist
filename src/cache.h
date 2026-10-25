// SSD chunk cache. Every expensive per-chunk result (a separated 7.8 s segment, 24 s of wav2vec2 log-probs, a beat
// window) is stored under a key that hashes its exact input samples + the model + its settings. A re-run, a crash
// half-way through, or the same song analysed again with new lyrics only recomputes chunks it has never seen.
#pragma once
#include <atomic>
#include <vector>

#include "common.h"

namespace mt {

class Cache {
public:
    explicit Cache(fs::path dir = {}) : dir_(std::move(dir)) {}
    bool enabled() const { return !dir_.empty(); }
    bool get(const char* stage, uint64_t key, std::vector<float>& out) const;
    void put(const char* stage, uint64_t key, const float* data, size_t n) const;
    mutable std::atomic<size_t> hits{0}, misses{0};
    // keep the cache under max_bytes: delete the least recently written chunks down to 80% of it
    // (~120 MB of separated chunks per 4-minute song; compiled GPU kernels are kept)
    void trim(uint64_t max_bytes) const;

private:
    fs::path file(const char* stage, uint64_t key) const;
    fs::path dir_;
};

}  // namespace mt
