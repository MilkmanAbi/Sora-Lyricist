#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mt {

std::vector<float> Audio::channel(int c) const {
    std::vector<float> out(frames());
    for (size_t i = 0; i < out.size(); i++) out[i] = data[i * channels + c];
    return out;
}

std::vector<float> Audio::mono() const {
    std::vector<float> out(frames());
    for (size_t i = 0; i < out.size(); i++) {
        float s = 0;
        for (int c = 0; c < channels; c++) s += data[i * channels + c];
        out[i] = s / channels;
    }
    return out;
}

fs::path ffmpeg_path() {
    if (const wchar_t* e = _wgetenv(L"FFMPEG")) return e;
    fs::path local = exe_dir() / "ffmpeg.exe";  // bundled by the installer
    if (fs::exists(local)) return local;
    wchar_t buf[MAX_PATH * 4];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, (DWORD)std::size(buf), buf, nullptr)) return buf;
    die("ffmpeg not found (put ffmpeg.exe next to the program, on PATH, or set FFMPEG)");
}

static std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// run a command line, return its stdout (stderr goes to ours)
static std::string run_capture(const std::wstring& cmd, DWORD& code) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) die("CreatePipe failed");
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        die("could not start ffmpeg");
    CloseHandle(wr);
    std::string bytes;
    std::vector<char> buf(1 << 20);
    DWORD got;
    while (ReadFile(rd, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) bytes.append(buf.data(), got);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return bytes;
}

Audio decode(const fs::path& src, int sr, int channels) {
    if (!fs::exists(src)) die("no such file: %s", utf8(src).c_str());
    std::wstring cmd = quote(ffmpeg_path().wstring()) + L" -nostdin -v error -i " + quote(src.wstring()) +
                       L" -vn -f f32le -acodec pcm_f32le -ac " + std::to_wstring(channels) + L" -ar " +
                       std::to_wstring(sr) + L" -";
    DWORD code;
    std::string bytes = run_capture(cmd, code);
    if (code != 0) die("ffmpeg could not decode %s", utf8(src).c_str());
    Audio a;
    a.sr = sr;
    a.channels = channels;
    a.data.resize(bytes.size() / sizeof(float));
    memcpy(a.data.data(), bytes.data(), a.data.size() * sizeof(float));
    return a;
}

bool extract_cover(const fs::path& src, const fs::path& dst) {
    // the first attached picture (ID3 APIC / MP4 covr / FLAC picture), re-encoded as JPEG
    if (dst.has_parent_path()) fs::create_directories(dst.parent_path());
    std::wstring cmd = quote(ffmpeg_path().wstring()) + L" -nostdin -v error -y -i " + quote(src.wstring()) +
                       L" -an -map 0:v:0? -frames:v 1 -q:v 3 " + quote(dst.wstring());
    DWORD code;
    run_capture(cmd, code);
    return code == 0 && fs::exists(dst) && fs::file_size(dst) > 0;
}

std::map<std::string, std::string> read_tags(const fs::path& src) {
    std::wstring cmd = quote(ffmpeg_path().wstring()) + L" -nostdin -v error -i " + quote(src.wstring()) +
                       L" -f ffmetadata -";
    DWORD code;
    std::string text = run_capture(cmd, code);
    std::map<std::string, std::string> tags;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        std::string line = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? text.size() : e + 1;
        size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || line[0] == '[' || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        for (char& c : k) c = (char)std::tolower((unsigned char)c);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        tags[k] = v;
    }
    return tags;
}

void write_wav16(const fs::path& dst, const Audio& a) {
    // like Demucs' default --clip-mode rescale: only scale down when the stem would clip
    float peak = 0;
    for (float v : a.data) peak = std::max(peak, std::fabs(v));
    float k = 1.0f / std::max(1.01f * peak, 1.0f);
    std::vector<int16_t> pcm(a.data.size());
    for (size_t i = 0; i < pcm.size(); i++)
        pcm[i] = (int16_t)std::clamp(a.data[i] * k * 32768.0f, -32768.0f, 32767.0f);
    if (dst.has_parent_path()) fs::create_directories(dst.parent_path());
    fs::path tmp = dst;
    tmp += ".partial";
    {
        std::ofstream f(tmp, std::ios::binary);
        auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
        auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
        uint32_t bytes = (uint32_t)(pcm.size() * 2);
        f.write("RIFF", 4);
        u32(36 + bytes);
        f.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16((uint16_t)a.channels);
        u32((uint32_t)a.sr);
        u32((uint32_t)(a.sr * a.channels * 2));
        u16((uint16_t)(a.channels * 2));
        u16(16);
        f.write("data", 4);
        u32(bytes);
        f.write(reinterpret_cast<const char*>(pcm.data()), bytes);
    }
    fs::rename(tmp, dst);
}

}  // namespace mt
