#include "cache.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace mt {

static const char kMagic[8] = {'M', 'T', 'C', 'H', 'U', 'N', 'K', '1'};

fs::path Cache::file(const char* stage, uint64_t key) const { return dir_ / stage / (hex(key) + ".f32"); }

bool Cache::get(const char* stage, uint64_t key, std::vector<float>& out) const {
    if (!enabled()) return false;
    std::ifstream f(file(stage, key), std::ios::binary);
    if (!f) {
        misses++;
        return false;
    }
    char magic[8];
    uint64_t n = 0;
    f.read(magic, 8);
    f.read(reinterpret_cast<char*>(&n), 8);
    if (!f || memcmp(magic, kMagic, 8) != 0 || n > (1ull << 32)) return false;
    out.resize(n);
    f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)(n * sizeof(float)));
    if (!f) return false;  // truncated (should not happen: writes are atomic)
    hits++;
    return true;
}

void Cache::put(const char* stage, uint64_t key, const float* data, size_t n) const {
    if (!enabled()) return;
    fs::path p = file(stage, key);
    fs::create_directories(p.parent_path());
    fs::path tmp = p;
    tmp += ".partial";
    {
        std::ofstream f(tmp, std::ios::binary);
        uint64_t n64 = n;
        f.write(kMagic, 8);
        f.write(reinterpret_cast<const char*>(&n64), 8);
        f.write(reinterpret_cast<const char*>(data), (std::streamsize)(n * sizeof(float)));
        if (!f) {
            f.close();
            fs::remove(tmp);
            return;  // disk full etc.: caching is best-effort
        }
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
}

void Cache::trim(uint64_t max_bytes) const {
    if (!enabled() || !fs::exists(dir_)) return;
    struct F {
        fs::path p;
        fs::file_time_type t;
        uint64_t n;
    };
    std::vector<F> files;
    uint64_t total = 0;
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(dir_, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".f32") continue;  // chunk files only
        files.push_back({e.path(), e.last_write_time(), e.file_size()});
        total += files.back().n;
    }
    if (total <= max_bytes) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t < b.t; });
    uint64_t freed = 0;
    for (auto& f : files) {
        if (total - freed <= max_bytes * 8 / 10) break;
        if (fs::remove(f.p, ec)) freed += f.n;
    }
    log("cache: removed %.0f MB of old chunks (limit %.1f GB)", freed / 1e6, max_bytes / 1e9);
}

}  // namespace mt
