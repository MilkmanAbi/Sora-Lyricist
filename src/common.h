// Shared bits: logging, timing, paths, hashing.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace mt {

void log(const char* fmt, ...);
[[noreturn]] void die(const char* fmt, ...);
// --progress-json: one {"stage","done","total"} line on stdout per step, for the app's progress bars
void enable_progress_json(bool on);
void progress(const char* stage, double done, double total);

struct Timer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    double s() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
};

// 64-bit FNV-1a, chained: hash(data, n, hash(other...)). Keys the SSD chunk cache.
uint64_t fnv(const void* data, size_t n, uint64_t h = 1469598103934665603ull);
inline uint64_t fnv(const std::string& s, uint64_t h = 1469598103934665603ull) { return fnv(s.data(), s.size(), h); }
std::string hex(uint64_t h);

std::string utf8(const fs::path& p);
fs::path exe_dir();
std::string read_text(const fs::path& p);
void write_text(const fs::path& p, const std::string& s);  // via a temp file + rename: never half-written

}  // namespace mt
