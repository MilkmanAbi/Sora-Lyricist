// mtn - Music Tool native analysis engine.
//
//   mtn analyze AUDIO [--lrc FILE | --lyrics FILE] --out analysis/songs/NAME.song.json [options]
//   mtn lyrics AUDIO [--lyrics-source local|web|both] [--save-lyrics PATH]
//   mtn devices
//
// Without a lyric file, analyze looks the song up like `mtn lyrics` does (LRCLIB and Genius) and asks which to use
// and where to save it. --yes takes the defaults (both; next to the output) without asking.
//
// Writes the same song.json as tools/analyze_song.py (stems in <out dir>/stems/), so make.py / build_edit.py /
// the Remotion renderer use it unchanged. Options:
//   --device cpu|gpu|auto     every network on this device (default: separator on the iGPU if any, the rest on the CPU)
//   --sequential              no overlapping of stages
//   --sep-device / --w2v-device / --beat-device   override one network
//   --w2v large|base          alignment model (default large)
//   --precision int8|fp32     CPU networks in INT8 when the _int8 IR exists (default) or fp32
//   --lrc-stretch             the .lrc belongs to another speed/version: fit its timeline to this audio first
//   --models DIR  --cache DIR --no-cache  --cache-gb N (chunk cache size limit, default 5)
//   --title T --artist A      what to search for (default: the file's tags, else its name)
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>


#include "align.h"
#include "audio.h"
#include "beats.h"
#include "cache.h"
#include "dsp.h"
#include "gpu.h"
#include "infer.h"
#include "json.hpp"
#include "lyrics.h"
#include "lyrics_web.h"
#include "separate.h"

using namespace mt;
using json = nlohmann::ordered_json;

static double r3(double v) { return std::round(v * 1000) / 1000; }
static double r4(double v) { return std::round(v * 10000) / 10000; }

// analyze_song.grid_position: bar, beat-in-bar (1-based, fractional) and ms from the nearest beat
static void grid_position(json& w, double t, const std::vector<double>& beats, const std::vector<double>& down) {
    if (beats.size() < 2) return;
    long long k = (long long)(std::upper_bound(beats.begin(), beats.end(), t) - beats.begin()) - 1;
    k = std::clamp<long long>(k, 0, (long long)beats.size() - 2);
    double phase = (t - beats[k]) / (beats[k + 1] - beats[k]);
    long long bar = std::upper_bound(down.begin(), down.end(), beats[k]) - down.begin();
    double bar_start = bar > 0 ? down[bar - 1] : beats[0];
    int in_bar = 0;
    for (double b : beats) in_bar += b >= bar_start - 1e-3 && b <= beats[k] + 1e-3;
    double nearest = beats[0];
    for (double b : beats)
        if (std::fabs(b - t) < std::fabs(nearest - t)) nearest = b;
    w["bar"] = bar;
    w["beat_in_bar"] = r3(in_bar + phase);
    w["beat_phase"] = r3(phase);
    w["ms_from_nearest_beat"] = std::round((t - nearest) * 1000 * 10) / 10;
}

// Theil-Sen fit of song_time = scale * lrc_time + offset from line starts of an unanchored alignment
static std::pair<double, double> fit_time_map(const std::vector<Word>& words, const Lyrics& L) {
    std::vector<std::pair<double, double>> pts;
    int last = -1;
    for (auto& w : words)
        if (w.line != last) {
            last = w.line;
            if (w.line >= 0 && L.times[w.line]) pts.push_back({*L.times[w.line], w.start});
        }
    if (pts.size() < 4) return {1.0, 0.0};
    std::vector<double> slopes;
    for (size_t i = 0; i < pts.size(); i++)
        for (size_t j = i + 1; j < pts.size(); j++)
            if (std::fabs(pts[j].first - pts[i].first) > 1.0)
                slopes.push_back((pts[j].second - pts[i].second) / (pts[j].first - pts[i].first));
    if (slopes.empty()) return {1.0, 0.0};
    std::nth_element(slopes.begin(), slopes.begin() + slopes.size() / 2, slopes.end());
    double scale = slopes[slopes.size() / 2];
    std::vector<double> offs;
    for (auto& p : pts) offs.push_back(p.second - scale * p.first);
    std::nth_element(offs.begin(), offs.begin() + offs.size() / 2, offs.end());
    return {scale, offs[offs.size() / 2]};
}

static fs::path default_cache() {
    if (const wchar_t* e = _wgetenv(L"LOCALAPPDATA")) return fs::path(e) / "Sora" / "cache";
    return exe_dir() / "cache";
}

static fs::path default_models() {
    if (const wchar_t* e = _wgetenv(L"MT_MODELS")) return e;
    return exe_dir() / "models";
}

static int usage() {
    fprintf(stderr,
            "usage: mtn analyze AUDIO [--lrc FILE | --lyrics FILE] --out SONG.json [--device auto|cpu|gpu]\n"
            "           [--sep-device D] [--w2v-device D] [--beat-device D] [--w2v large|base]\n"
            "           [--precision int8|fp32] [--lrc-stretch]\n"
            "           [--models DIR] [--cache DIR] [--no-cache] [--cache-gb N]\n"
            "           [--lyrics-source local|web|both] [--save-lyrics PATH] [--title T] [--artist A] [--yes]\n"
            "       mtn lyrics AUDIO [--lyrics-source local|web|both] [--save-lyrics PATH] [--title T] [--artist A]\n"
            "                        [--yes]\n"
            "       mtn lyrics AUDIO --versions DIR [--title T] [--artist A]   (every version + JSON summary)\n"
            "       mtn tags AUDIO [--cover OUT.jpg]                        (JSON: title, artist, album, ...)\n"
            "       mtn devices\n"
            "       --progress-json: stage progress as JSON lines on stdout\n");
    return 2;
}

static std::string dev_name(std::string d) {
    for (char& c : d) c = (char)std::toupper((unsigned char)c);
    return d;
}

static int run(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(utf8(fs::path(wargv[i])));
    if (args.empty()) return usage();
    std::map<std::string, std::string> opt;
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--lrc-stretch" || a == "--no-cache" || a == "--word-sep" || a == "--yes" || a == "--sequential" ||
            a == "--long" || a == "--progress-json" || a == "--no-dgpu" || a == "--json")
            opt[a] = "1";
        else if (a.rfind("--", 0) == 0 && i + 1 < args.size()) opt[a] = args[++i];
        else pos.push_back(a);
    }
    fs::path models = opt.count("--models") ? fs::u8path(opt["--models"]) : default_models();
    fs::path cache_dir = opt.count("--cache") ? fs::u8path(opt["--cache"]) : default_cache();
    init_openvino(cache_dir / "ov_kernels");
    set_gpu_marks(cache_dir / "gpu");
    if (pos[0] == "devices") {
        // what this PC has, and where analyze would run each network by default
        auto gpus = list_gpus();
        auto ovd = openvino_devices();
        auto dg = discrete_gpu();
        bool intel_gpu = std::find(ovd.begin(), ovd.end(), "GPU") != ovd.end();
        std::string plan = dg ? "discrete GPU (DirectML): " + dg->name
                         : intel_gpu ? "Intel GPU (OpenVINO) for separation + CPU for timing"
                                     : "CPU";
        if (opt.count("--json")) {
            json j = {{"plan", plan}, {"openvino", ovd}, {"gpus", json::array()}};
            for (auto& g : gpus)
                j["gpus"].push_back({{"index", g.index}, {"name", g.name}, {"vram_gb", r3(g.vram / 1e9)}, {"discrete", g.discrete}});
            printf("%s\n", j.dump().c_str());
        } else {
            for (auto& g : gpus)
                printf("GPU %d  %-40s %6.1f GB  %s\n", g.index, g.name.c_str(), g.vram / 1e9, g.discrete ? "discrete" : "integrated");
            printf("OpenVINO devices:");
            for (auto& d : ovd) printf(" %s", d.c_str());
            printf("\nanalyze will use: %s\n", plan.c_str());
        }
        return 0;
    }
    LyricsChoice lc;
    lc.source = opt.count("--lyrics-source") ? opt["--lyrics-source"] : "";
    lc.save = opt.count("--save-lyrics") ? fs::u8path(opt["--save-lyrics"]) : fs::path();
    lc.interactive = !opt.count("--yes");
    lc.title = opt.count("--title") ? opt["--title"] : "";
    lc.artist = opt.count("--artist") ? opt["--artist"] : "";
    enable_progress_json(opt.count("--progress-json") > 0);
    if (pos[0] == "lyrics" && pos.size() >= 2) {
        fs::path src = fs::u8path(pos[1]);
        Audio a = decode(src, 8000, 1);  // only for the length (LRCLIB lists several versions of a song)
        if (opt.count("--versions")) {
            lc.versions_dir = fs::u8path(opt["--versions"]);
            lc.interactive = false;
        }
        get_lyrics(src, a.frames() / 8000.0, fs::current_path() / src.stem(), lc);
        return 0;
    }
    if (pos[0] == "tags" && pos.size() >= 2) {
        // for the app's tagger: what the file says about itself (+ its embedded cover with --cover OUT.jpg)
        fs::path src = fs::u8path(pos[1]);
        auto tags = read_tags(src);
        SongQuery q = guess_query(src, 0);
        Audio a = decode(src, 8000, 1);
        json j = {{"file", utf8(src)}, {"title", tags.count("title") ? tags["title"] : ""},
                  {"artist", tags.count("artist") ? tags["artist"] : ""}, {"album", tags.count("album") ? tags["album"] : ""},
                  {"year", tags.count("date") ? tags["date"] : ""}, {"guess_title", q.title},
                  {"duration", r3(a.frames() / 8000.0)}};
        j["cover"] = opt.count("--cover") ? extract_cover(src, fs::u8path(opt["--cover"])) : false;
        printf("%s\n", j.dump().c_str());
        return 0;
    }
    if (pos[0] != "analyze" || pos.size() < 2 || !opt.count("--out")) return usage();

    Timer total;
    const std::string all = opt.count("--device") ? dev_name(opt["--device"]) : "";
    auto device_for = [&](const char* key, const char* fallback) {
        if (opt.count(key)) return dev_name(opt[key]);
        return all.empty() ? std::string(fallback) : all;
    };
    // Default split (measured on real audio, README):
    //  - a discrete GPU (AMD / NVIDIA / Intel Arc, through DirectML) runs all three networks;
    //  - else an Intel iGPU (OpenVINO) runs the separator in fp32 (fp16 breaks Demucs) while the CPU tracks beats and
    //    aligns each finished stretch of vocals with INT8 wav2vec2;
    //  - else the CPU runs everything, one stage at a time.
    // --device / --sep-device / --w2v-device / --beat-device override (cpu, gpu = OpenVINO GPU, dml, dml:N).
    std::string dgpu;
    if (!opt.count("--no-dgpu") && all.empty())
        if (auto g = discrete_gpu()) {
            dgpu = "DML:" + std::to_string(g->index);
            log("discrete GPU: %s (%.0f GB) - running the networks on it", g->name.c_str(), g->vram / 1e9);
        }
    std::string sep_dev = device_for("--sep-device", dgpu.empty() ? "AUTO" : dgpu.c_str());
    std::string w2v_dev = device_for("--w2v-device", dgpu.empty() ? "CPU" : dgpu.c_str());
    std::string beat_dev = device_for("--beat-device", dgpu.empty() ? "CPU" : dgpu.c_str());
    for (std::string* d : {&sep_dev, &w2v_dev, &beat_dev})
        if (*d == "DML")  // no adapter given: the discrete one, else adapter 0
            if (auto g = discrete_gpu()) *d = "DML:" + std::to_string(g->index);
    const bool fp32 = opt.count("--precision") && opt["--precision"] == "fp32";
    // INT8 (VNNI) variants run on the CPU only; an OpenVINO GPU gets the fp32 IR and runs it in fp16
    // (the GPU plugin has no INT8 on these iGPUs). DirectML runs the .onnx exports: Demucs and Beat This! in fp32,
    // wav2vec2 in fp16 (w2v_large_fp16). A GPU without its model file installed falls back to the CPU.
    auto variant = [&](const std::string& base, std::string& dev) {
        if (is_dml(dev)) {
            std::string name = base == "w2v_large" ? "w2v_large_fp16" : base;
            if (gpu_marked_bad(dev, name)) {
                // failed its self-check on this GPU before (delete cache\gpu to try again, e.g. after a driver update)
                log("  %s: this GPU failed its check before - not using it for this", name.c_str());
                dev = base == "htdemucs_core" ? "AUTO" : "CPU";
            } else if (fs::exists(models / (name + ".onnx"))) {
                return name;
            } else {
                log("  %s: no DirectML model installed - using the CPU", base.c_str());
                dev = "CPU";
            }
        }
        bool have32 = fs::exists(models / (base + ".xml")), have8 = fs::exists(models / (base + "_int8.xml"));
        if (dev != "CPU" && !have32 && have8) {
            log("  %s: no GPU model installed - using the CPU", base.c_str());
            dev = "CPU";
        }
        if (dev == "CPU" && have8 && (!fp32 || !have32)) return base + "_int8";
        return base;
    };
    if (sep_dev == "AUTO") {
        auto devs = openvino_devices();
        sep_dev = std::find(devs.begin(), devs.end(), "GPU") != devs.end() ? "GPU" : "CPU";
    }
    const std::string sep_model = variant("htdemucs_core", sep_dev);
    if (sep_dev == "AUTO") {  // a DirectML fallback hands the separator back to the Intel GPU if there is one
        auto devs = openvino_devices();
        sep_dev = std::find(devs.begin(), devs.end(), "GPU") != devs.end() ? "GPU" : "CPU";
    }
    const std::string w2v_model = variant(std::string("w2v_") + (opt.count("--w2v") ? opt["--w2v"] : "large"), w2v_dev);
    variant("beat_this", beat_dev);  // only checks the DirectML file is there (else the CPU)
    Cache cache(opt.count("--no-cache") ? fs::path() : cache_dir);

    fs::path src = fs::u8path(pos[1]);
    fs::path out = fs::u8path(opt["--out"]);

    // same file layout as analyze_song.py
    out = fs::absolute(out);  // song.json records the stem paths; later steps run from other folders
    fs::path work = out.parent_path() / "stems";
    std::string name = utf8(out.stem());
    if (name.size() > 5 && name.substr(name.size() - 5) == ".song") name.resize(name.size() - 5);
    fs::path mix_wav = work / (name + ".wav");
    fs::path voc_wav = work / "htdemucs" / fs::u8path(name) / "vocals.wav";

    log("[1/4] decoding %s", utf8(src.filename()).c_str());
    Audio mix = decode(src, 44100, 2);
    write_wav16(mix_wav, mix);
    log("  %.1f s of audio", mix.frames() / 44100.0);
    // a 7-hour sleep loop in a music folder would separate for hours and need GBs to align: say so up front
    if (mix.frames() > 30 * 60 * 44100 && !opt.count("--long"))
        die("%s is %.0f minutes long - mtn is made for songs (up to 30 min; --long to go ahead anyway)",
            utf8(src.filename()).c_str(), mix.frames() / 44100.0 / 60);

    // lyrics: the file given, else looked up on the web (asks which version, and where to keep it)
    fs::path lyr = opt.count("--lrc")      ? fs::u8path(opt["--lrc"])
                 : opt.count("--lyrics")   ? fs::u8path(opt["--lyrics"])
                                           : get_lyrics(src, mix.frames() / 44100.0,
                                                        out.parent_path() / "lyrics" / fs::u8path(name), lc);
    Lyrics L = load_lyrics(lyr);
    if (opt.count("--lrc") && !L.synced()) die("%s has no [mm:ss] line timestamps", utf8(lyr).c_str());
    const bool anchored = L.synced();

    // The aligner reads the vocals at 16 kHz, resampled here as the separator finalises them; with the separator on
    // the iGPU, the CPU meanwhile tracks beats and aligns each finished 24 s window (measured: 20 s instead of 28 s
    // for the same work back to back). On the CPU alone, overlapping would only oversubscribe it: one stage at a time.
    const bool overlap = sep_dev != "CPU" && !opt.count("--sequential");
    const size_t n44 = mix.frames(), n16 = (size_t)((unsigned long long)n44 * 16000 / 44100);
    std::vector<float> mono44(n44), v16(n16);
    size_t final44 = 0, ready16 = 0;
    bool sep_done = false;
    std::mutex mu;
    std::condition_variable cv;
    auto progress = [&](const Audio& stem, size_t frames) {  // separator thread: copy out the newly final stretch
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = final44; i < frames; i++) mono44[i] = 0.5f * (stem.data[2 * i] + stem.data[2 * i + 1]);
        final44 = frames;
        cv.notify_all();
    };
    auto resample_ready = [&](size_t avail44, bool all) {  // extend v16 as far as the final 44.1 kHz audio allows
        size_t upto = all ? n16 : (avail44 > resample_reach(44100, 16000)
                                       ? std::min(n16, (size_t)((double)(avail44 - resample_reach(44100, 16000)) * 16000 / 44100))
                                       : 0);
        if (upto > ready16) {
            resample(mono44.data(), all ? n44 : avail44, 44100, 16000, ready16, upto, v16.data() + ready16);
            ready16 = upto;
        }
    };

    log("[1/4] separating vocals (%s on %s)%s", sep_model.c_str(), sep_dev.c_str(), overlap ? ", aligning alongside" : "");
    Audio voc;
    Beats beats;
    EmissionStream es(models, w2v_model, w2v_dev, cache, n16);
    auto track = [&] {
        Audio m22 = decode(mix_wav, 22050, 2);
        beats = track_beats(m22.mono(), models, beat_dev, cache);
        mt::progress("beats", 1, 1);
        log("[2/4] beats: %zu, bpm %.1f", beats.beats.size(), beats.bpm);
    };
    if (overlap) {
        std::exception_ptr sep_error;
        std::thread sep([&] {
            Audio v;
            try {
                v = separate_vocals(mix, models, sep_model, sep_dev, cache, progress);
            } catch (...) {  // an exception escaping a thread would end the program without a word
                sep_error = std::current_exception();
            }
            std::lock_guard<std::mutex> lk(mu);
            voc = std::move(v);
            sep_done = true;
            cv.notify_all();
        });
        struct Join {  // join on every way out of this block, so an error below can't abandon a running thread
            std::thread& t;
            ~Join() {
                if (t.joinable()) t.join();
            }
        } join{sep};
        track();  // CPU, while the GPU separates
        log("[3/4] aligning %zu words to %zu lines (%s on %s)", L.words.size(), L.lines.size(), w2v_model.c_str(), w2v_dev.c_str());
        while (!es.done()) {
            size_t avail;
            bool all;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return sep_done || final44 > ready16 * 44100 / 16000 + 44100 * 4; });
                avail = final44;
                all = sep_done;
                if (sep_error) break;
            }
            resample_ready(avail, all);
            es.update(v16, ready16);
        }
        sep.join();
        if (sep_error) std::rethrow_exception(sep_error);
    } else {
        voc = separate_vocals(mix, models, sep_model, sep_dev, cache, progress);
        track();
        log("[3/4] aligning %zu words to %zu lines (%s on %s)", L.words.size(), L.lines.size(), w2v_model.c_str(), w2v_dev.c_str());
        resample_ready(n44, true);
        es.update(v16, n16);
    }
    write_wav16(voc_wav, voc);
    Emissions em = es.take();
    AlignOptions ao;
    ao.word_sep = opt.count("--word-sep") > 0;
    if (opt.count("--slack")) ao.slack = std::stod(opt["--slack"]);  // tuning knobs (tools/bench.py --extra)
    if (opt.count("--weight")) ao.weight = std::stod(opt["--weight"]);
    if (opt.count("--filler")) ao.filler_logp = std::stod(opt["--filler"]);
    std::vector<std::optional<double>> anchors = L.times;
    json time_map = nullptr;
    if (opt.count("--lrc-stretch")) {
        auto free = align(em, L.words, L.line_of, std::vector<std::optional<double>>(L.lines.size()), ao);
        auto [scale, offset] = fit_time_map(free, L);
        for (auto& a : anchors)
            if (a) a = *a * scale + offset;
        time_map = {{"scale", r4(scale)}, {"offset", r3(offset)}};
        log("  lrc timeline fitted: song = %.4f * lrc + %.2fs", scale, offset);
    }
    std::vector<Word> words = align(em, L.words, L.line_of, anchored ? anchors : std::vector<std::optional<double>>{}, ao);
    // CTC character spikes trail the sung onset by a roughly constant amount (measured on tools/bench.py; the
    // contrastive-alignment literature reports the same and shifts its predictions too)
    const double shift = opt.count("--shift") ? std::stod(opt["--shift"]) : -0.02;
    for (auto& w : words) {
        w.start = std::max(0.0, w.start + shift);
        w.end = std::max(w.start + 0.02, w.end + shift);
    }
    // do these lyrics belong to this recording? Mean letter confidence was 0.24-0.66 on correct lyrics across a test
    // of 8 songs and 0.06 for a namesake's lyrics (Genius "Eureka" by Lil Wayne on another song called Eureka)
    double fit = 0;
    for (auto& w : words) fit += w.score;
    fit /= std::max<size_t>(1, words.size());
    if (fit < 0.15)
        log("WARNING: these lyrics barely match the singing (confidence %.2f; correct lyrics score 0.24+). Probably a "
            "different song or version - check %s, or rerun with --title/--artist or --lyrics FILE",
            fit, utf8(lyr).c_str());

    std::vector<double> onsets;
    Audio v22 = decode(voc_wav, 22050, 1);
    auto env = librosa_onset_strength(v22.data, 22050, 128);
    for (int f : librosa_onset_detect(env, 22050, 128)) onsets.push_back(f * 128.0 / 22050);
    log("[4/4] vocal onsets: %zu", onsets.size());

    json jw = json::array();
    for (size_t i = 0; i < words.size(); i++) {
        const Word& w = words[i];
        json o = {{"word", w.word}, {"start", r3(w.start)}, {"end", r3(w.end)}, {"score", r3(w.score)}};
        bool ls = i == 0 || words[i - 1].line != w.line;
        o["line_start"] = ls;
        if (ls) o["line"] = w.line;
        grid_position(o, w.start, beats.beats, beats.downbeats);
        if (!onsets.empty()) {
            double best = onsets[0];
            for (double t : onsets)
                if (std::fabs(t - w.start) < std::fabs(best - w.start)) best = t;
            o["ms_from_vocal_onset"] = std::round((w.start - best) * 1000 * 10) / 10;
        }
        jw.push_back(o);
    }
    auto arr = [](const std::vector<double>& v) {
        json a = json::array();
        for (double x : v) a.push_back(r4(x));
        return a;
    };
    json song = {{"source", utf8(src.filename())},
                 {"bpm", std::round(beats.bpm * 100) / 100},
                 {"beat_source", "beat_this"},
                 {"beats", arr(beats.beats)},
                 {"downbeats", arr(beats.downbeats)},
                 {"vocal_onsets", arr(onsets)},
                 {"lyrics_used", L.raw},
                 {"lrc_time_map", time_map},
                 {"words", jw},
                 {"stems", {{"mix", utf8(mix_wav)}, {"vocals", utf8(voc_wav)}}},
                 {"engine", {{"name", "mtn"}, {"separator", sep_model + "@" + sep_dev}, {"aligner", w2v_model + "@" + w2v_dev + " global-ctc"},
                             {"lyrics", utf8(lyr)}, {"lyrics_fit", r3(fit)}, {"seconds", r3(total.s())}}}};
    write_text(out, song.dump(1));
    cache.trim((uint64_t)(std::stod(opt.count("--cache-gb") ? opt["--cache-gb"] : "5") * 1e9));
    log("bpm %.1f, %zu beats, %zu bars, %zu words -> %s  (cache: %zu hits, %zu misses; %.1fs total)", beats.bpm,
        beats.beats.size(), beats.downbeats.size(), words.size(), utf8(out).c_str(), cache.hits.load(), cache.misses.load(), total.s());
    return 0;
}

int wmain(int argc, wchar_t** wargv) {
    try {
        return run(argc, wargv);
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
