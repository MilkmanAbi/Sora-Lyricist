#include "common.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mt {

static Timer g_start;

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%6.1fs] %s\n", g_start.s(), buf);
    fflush(stderr);
}

static bool g_progress_json = false;

void enable_progress_json(bool on) { g_progress_json = on; }

void progress(const char* stage, double done, double total) {
    if (!g_progress_json) return;
    printf("{\"stage\":\"%s\",\"done\":%.0f,\"total\":%.0f}\n", stage, done, total);
    fflush(stdout);
}

void die(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "error: %s\n", buf);
    if (g_progress_json) {  // the app shows this message
        std::string m;
        for (const char* p = buf; *p; p++) {
            if (*p == '"' || *p == '\\') m += '\\';
            if ((unsigned char)*p >= 0x20) m += *p;
        }
        printf("{\"error\":\"%s\"}\n", m.c_str());
        fflush(stdout);
    }
    exit(1);
}

uint64_t fnv(const void* data, size_t n, uint64_t h) {
    auto p = static_cast<const uint8_t*>(data);
    // 8 bytes at a time: chunk keys hash tens of MB of audio, byte-wise FNV would be the slowest step
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::string hex(uint64_t h) {
    char b[17];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

std::string utf8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return fs::path(buf).parent_path();
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) die("can't read %s", utf8(p).c_str());
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

void write_text(const fs::path& p, const std::string& s) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    fs::path tmp = p;
    tmp += ".partial";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) die("can't write %s", utf8(tmp).c_str());
        f.write(s.data(), (std::streamsize)s.size());
    }
    fs::rename(tmp, p);
}

}  // namespace mt
