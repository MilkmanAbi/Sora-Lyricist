// Audio in/out. Decoding goes through ffmpeg (any format, resampled by it); stems are written as 16-bit WAV like
// Demucs writes them, so the existing Python/Remotion steps read them unchanged.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "common.h"

namespace mt {

struct Audio {
    int sr = 0, channels = 0;
    std::vector<float> data;  // interleaved
    size_t frames() const { return channels ? data.size() / channels : 0; }
    std::vector<float> channel(int c) const;
    std::vector<float> mono() const;
};

fs::path ffmpeg_path();
Audio decode(const fs::path& src, int sr, int channels);
// container tags (title, artist, ...), lower-case keys
std::map<std::string, std::string> read_tags(const fs::path& src);
bool extract_cover(const fs::path& src, const fs::path& dst);  // embedded album art -> dst (.jpg); false if none
void write_wav16(const fs::path& dst, const Audio& a);

}  // namespace mt
