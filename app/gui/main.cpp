// Sora.exe - the window: an HTML interface (app\ui, shown with WebView2) over the engine (engine\mtn.exe) and the
// video pipeline (python\python.exe pipeline\mt_pipeline.py).
//
// The page sends {"cmd": ...} messages; the host answers with {"type": ...} messages. Everything slow runs on worker
// threads as child processes, which print JSON progress lines; those are relayed to the page through the UI thread.
// Every child process sits in one job object that kills it when the window closes, so nothing keeps running.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"
#include "json.hpp"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;
namespace fs = std::filesystem;

static const UINT WM_TO_PAGE = WM_APP + 1;  // lParam: std::string* (JSON) to post to the page
static HWND g_hwnd;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_view;
static HANDLE g_job;                        // kill-on-close job object for every child process
static std::atomic<bool> g_busy{false}, g_cancel{false};
static fs::path g_app, g_work;

// ------------------------------------------------------------------ strings / paths

static std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string U(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string U(const fs::path& p) { return U(p.wstring()); }

static fs::path known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// to the page, from any thread
static void send(const json& j) { PostMessageW(g_hwnd, WM_TO_PAGE, 0, (LPARAM) new std::string(j.dump())); }

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string data_url(const fs::path& img) {  // small images (covers) for the page's <img>
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s = read_file(img), o;
    if (s.empty()) return "";
    for (size_t i = 0; i < s.size(); i += 3) {
        unsigned v = (unsigned char)s[i] << 16 | (i + 1 < s.size() ? (unsigned char)s[i + 1] << 8 : 0) |
                     (i + 2 < s.size() ? (unsigned char)s[i + 2] : 0);
        o += b64[v >> 18 & 63];
        o += b64[v >> 12 & 63];
        o += i + 1 < s.size() ? b64[v >> 6 & 63] : '=';
        o += i + 2 < s.size() ? b64[v & 63] : '=';
    }
    auto ext = img.extension().wstring();
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return std::string("data:image/") + (ext == L".png" ? "png" : "jpeg") + ";base64," + o;
}

// ------------------------------------------------------------------ child processes

// Run a command line; call on_line for each stdout line; stderr goes to `log`. Returns the exit code (-1: not started).
static int run(const std::wstring& cmd, const std::function<void(const std::string&)>& on_line, const fs::path& log) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    fs::create_directories(log.parent_path());
    HANDLE err = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = err;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             g_app.c_str(), &si, &pi);
    CloseHandle(wr);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    AssignProcessToJobObject(g_job, pi.hProcess);  // before it runs, so its own children join the job too
    ResumeThread(pi.hThread);
    std::string buf;
    char chunk[4096];
    DWORD got;
    while (ReadFile(rd, chunk, sizeof chunk, &got, nullptr) && got) {
        buf.append(chunk, got);
        size_t p;
        while ((p = buf.find('\n')) != std::string::npos) {
            std::string l = buf.substr(0, p);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) on_line(l);
            buf.erase(0, p + 1);
        }
    }
    if (!buf.empty()) on_line(buf);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static std::wstring mtn() { return quote((g_app / "engine" / "mtn.exe").wstring()); }

static std::string last_error_line(const fs::path& log) {
    std::ifstream f(log);
    std::string l, last;
    while (std::getline(f, l))
        if (l.rfind("error:", 0) == 0 || l.find("Error") != std::string::npos) last = l;
    return last;
}

// ------------------------------------------------------------------ commands

static void cmd_tags(const json& m) {
    std::string id = m["id"];
    fs::path cover = g_work / "covers" / (id + ".jpg");
    fs::path log = g_work / "logs" / (id + ".log");
    json out;
    run(mtn() + L" tags " + quote(W(m["path"])) + L" --cover " + quote(cover.wstring()),
        [&](const std::string& l) { out = json::parse(l, nullptr, false); }, log);
    if (out.is_discarded() || out.is_null()) out = json::object();
    if (out.value("cover", false)) {
        out["coverPath"] = U(cover);
        out["coverUrl"] = data_url(cover);
    }
    send({{"type", "tags"}, {"id", id}, {"tags", out}});
}

static void cmd_lyrics(const json& m) {
    std::string id = m["id"];
    fs::path dir = g_work / "lyrics" / id;
    fs::path log = g_work / "logs" / (id + ".log");
    std::wstring c = mtn() + L" lyrics " + quote(W(m["path"])) + L" --versions " + quote(dir.wstring());
    if (!m.value("title", std::string()).empty()) c += L" --title " + quote(W(m["title"]));
    if (!m.value("artist", std::string()).empty()) c += L" --artist " + quote(W(m["artist"]));
    json summary;
    int code = run(c, [&](const std::string& l) {
        json j = json::parse(l, nullptr, false);
        if (j.is_object()) summary = j;
    }, log);
    json texts = json::object();  // each version's text, by its id ("best", "lrclib", "genius", ...)
    if (summary.is_object() && summary.contains("versions"))
        for (auto& v : summary["versions"])
            if (v.contains("file")) texts[v.value("id", "")] = read_file(W(v["file"].get<std::string>()));
    send({{"type", "lyrics"}, {"id", id}, {"ok", code == 0 && summary.is_object()}, {"summary", summary}, {"texts", texts},
          {"error", code == 0 ? "" : last_error_line(log)}});
}

static void cmd_save_lyrics(const json& m) {  // the user's own lyrics (pasted or edited) -> a file the engine reads
    std::string id = m["id"];
    fs::path p = g_work / "lyrics" / id / (std::string("mine") + (m.value("synced", false) ? ".lrc" : ".txt"));
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << m["text"].get<std::string>();
    send({{"type", "savedLyrics"}, {"id", id}, {"file", U(p)}});
}

// one song: analyse (mtn) then make the chosen files (pipeline); progress relayed as {"type":"progress"}
static bool run_song(const json& s) {
    std::string id = s["id"];
    fs::path work = g_work / "songs" / id, log = g_work / "logs" / (id + ".log");
    fs::path song = work / (id + ".song.json");
    std::string lyr = s["lyricsFile"];
    bool synced = fs::path(W(lyr)).extension() == L".lrc";
    std::string error;
    auto relay = [&](const char* phase) {
        return [&, phase](const std::string& l) {
            json j = json::parse(l, nullptr, false);
            if (!j.is_object()) return;
            if (j.contains("error")) error = j["error"];
            else if (j.contains("file")) send({{"type", "file"}, {"id", id}, {"path", j["file"]}});
            else send({{"type", "progress"}, {"id", id}, {"phase", phase}, {"stage", j.value("stage", "")},
                       {"of", j.value("of", "")}, {"done", j.value("done", 0.0)}, {"total", j.value("total", 1.0)}});
        };
    };
    send({{"type", "progress"}, {"id", id}, {"phase", "analyze"}, {"stage", "start"}, {"done", 0}, {"total", 1}});
    std::wstring a = mtn() + L" analyze " + quote(W(s["path"])) + (synced ? L" --lrc " : L" --lyrics ") + quote(W(lyr)) +
                     L" --out " + quote(song.wstring()) + L" --progress-json --yes";
    if (!s.value("title", std::string()).empty()) a += L" --title " + quote(W(s["title"]));
    if (!s.value("artist", std::string()).empty()) a += L" --artist " + quote(W(s["artist"]));
    if (s.value("show", std::string()) == "roman") a += L" --show roman";  // lyrics not in Latin letters, romanised
    if (run(a, relay("analyze"), log) != 0 || g_cancel) {
        if (error.empty()) error = g_cancel ? "cancelled" : last_error_line(log);
        send({{"type", "songDone"}, {"id", id}, {"ok", false}, {"error", error}});
        return false;
    }
    std::string outs;
    for (auto& o : s["outputs"]) outs += (outs.empty() ? "" : ",") + o.get<std::string>();
    std::wstring p = quote((g_app / "python" / "python.exe").wstring()) + L" -X utf8 " +
                     quote((g_app / "pipeline" / "mt_pipeline.py").wstring()) + L" --song " + quote(song.wstring()) +
                     L" --audio " + quote(W(s["path"])) + L" --id " + quote(W(id)) + L" --title " + quote(W(s.value("title", ""))) +
                     L" --artist " + quote(W(s.value("artist", ""))) + L" --album " + quote(W(s.value("album", ""))) +
                     L" --accent " + quote(W(s.value("accent", "#ff6b4a"))) + L" --lyrics " + quote(W(lyr)) +
                     L" --outputs " + quote(W(outs)) + L" --out-dir " + quote(W(s["outDir"]));
    if (!s.value("cover", std::string()).empty()) p += L" --cover " + quote(W(s["cover"]));
    if (!s.value("theme", std::string()).empty()) p += L" --theme " + quote(W(s["theme"]));
    if (!s.value("background", std::string()).empty()) p += L" --background " + quote(W(s["background"]));
    int code = run(p, relay("make"), log);
    bool ok = code == 0 && !g_cancel;
    if (!ok && error.empty()) error = g_cancel ? "cancelled" : last_error_line(log);
    send({{"type", "songDone"}, {"id", id}, {"ok", ok}, {"error", error}, {"log", U(log)}});
    return ok;
}

static void cmd_start(const json& m) {
    if (g_busy.exchange(true)) return;
    g_cancel = false;
    std::thread([songs = m["songs"]] {
        for (auto& s : songs) {
            if (g_cancel) break;
            run_song(s);
        }
        g_busy = false;
        send({{"type", "allDone"}, {"cancelled", g_cancel.load()}});
    }).detach();
}

static void cmd_cancel() {
    g_cancel = true;
    TerminateJobObject(g_job, 1);  // every running child (mtn, python, node, chrome); the job stays usable
}

// ------------------------------------------------------------------ dialogs

static std::vector<std::wstring> pick(bool folder, bool multi, const std::vector<COMDLG_FILTERSPEC>& filters,
                                      const wchar_t* title, const std::wstring& start = L"") {
    std::vector<std::wstring> out;
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d)))) return out;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0) | (multi ? FOS_ALLOWMULTISELECT : 0));
    if (!filters.empty()) d->SetFileTypes((UINT)filters.size(), filters.data());
    d->SetTitle(title);
    if (!start.empty()) {
        ComPtr<IShellItem> si;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) d->SetFolder(si.Get());
    }
    if (FAILED(d->Show(g_hwnd))) return out;
    ComPtr<IShellItemArray> items;
    if (FAILED(d->GetResults(&items))) return out;
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; i++) {
        ComPtr<IShellItem> it;
        PWSTR p = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            out.push_back(p);
            CoTaskMemFree(p);
        }
    }
    return out;
}

static const wchar_t* kAudio = L"*.mp3;*.wav;*.flac;*.m4a;*.aac;*.ogg;*.opus;*.wma;*.aiff;*.aif;*.alac;*.webm;*.mp4;*.mkv;*.mka";

static const wchar_t* kClips = L"*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv;*.gif;*.png;*.jpg;*.jpeg;*.webp";

// the user's background clip: a still from it for the page (the pipeline does the real conversion)
static void cmd_background(const std::string& id, const std::wstring& clip) {
    fs::path thumb = g_work / "thumbs" / (std::to_string(GetTickCount64()) + ".jpg");
    fs::create_directories(thumb.parent_path());
    std::wstring ff = quote((g_app / "engine" / "ffmpeg.exe").wstring());
    auto grab = [&](const wchar_t* seek) {
        run(ff + L" -v error -y" + seek + L" -i " + quote(clip) + L" -frames:v 1 -vf \"scale=320:-2\" " + quote(thumb.wstring()),
            [](const std::string&) {}, g_work / "logs" / "thumbs.log");
        std::error_code ec;
        return fs::file_size(thumb, ec) > 0 && !ec;
    };
    bool ok = grab(L" -ss 0.5") || grab(L"");  // a GIF or clip shorter than half a second: its first frame
    send({{"type", "background"}, {"id", id}, {"path", U(fs::path(clip))}, {"name", U(fs::path(clip).filename())},
          {"url", ok ? data_url(thumb) : ""}, {"ok", ok}});
    std::error_code ec;
    fs::remove(thumb, ec);
}

static bool is_audio(const fs::path& p) {
    static const std::vector<std::wstring> ext{L".mp3", L".wav", L".flac", L".m4a", L".aac", L".ogg", L".opus", L".wma",
                                               L".aiff", L".aif", L".alac", L".webm", L".mp4", L".mkv", L".mka"};
    auto e = p.extension().wstring();
    for (auto& c : e) c = (wchar_t)towlower(c);
    return std::find(ext.begin(), ext.end(), e) != ext.end();
}

static void send_files(const std::vector<std::wstring>& paths) {  // folders are opened up: their audio files count
    json files = json::array();
    for (auto& p : paths) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (auto& e : fs::recursive_directory_iterator(p, ec))
                if (e.is_regular_file() && is_audio(e.path())) files.push_back(U(e.path()));
        } else if (is_audio(p)) {
            files.push_back(U(fs::path(p)));
        }
    }
    send({{"type", "files"}, {"paths", files}});
}

static void on_message(ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR raw = nullptr;
    if (FAILED(args->get_WebMessageAsJson(&raw))) return;
    json m = json::parse(U(std::wstring(raw)), nullptr, false);
    CoTaskMemFree(raw);
    if (!m.is_object()) return;
    std::string c = m.value("cmd", "");
    if (c == "init") {
        json init = {{"type", "init"}, {"outDir", U(known(FOLDERID_Music) / "Sora")}, {"busy", g_busy.load()}};
        if (const wchar_t* t = _wgetenv(L"MT_TEST_START")) init["testStart"] = U(std::wstring(t));
        if (const wchar_t* t = _wgetenv(L"MT_TEST_OUTPUTS")) init["testOutputs"] = U(std::wstring(t));
        if (const wchar_t* t = _wgetenv(L"MT_TEST_THEME")) init["testTheme"] = U(std::wstring(t));
        if (const wchar_t* t = _wgetenv(L"MT_TEST_BACKGROUND")) init["testBackground"] = U(std::wstring(t));
        send(init);
        std::thread([] {  // which GPU / CPU the engine will use, for the status line
            json d;
            run(mtn() + L" devices --json", [&](const std::string& l) { d = json::parse(l, nullptr, false); },
                g_work / "logs" / "devices.log");
            if (d.is_object()) send({{"type", "devices"}, {"plan", d.value("plan", "")}});
        }).detach();
        if (const wchar_t* t = _wgetenv(L"MT_TEST_FILES")) {  // testing: files as if dropped (';'-separated)
            std::vector<std::wstring> paths;
            std::wstringstream ss(t);
            std::wstring p;
            while (std::getline(ss, p, L';'))
                if (!p.empty()) paths.push_back(p);
            send_files(paths);
        }
    } else if (c == "drop") {  // files dragged onto the page arrive as WebView2 File objects carrying their paths
        std::vector<std::wstring> paths;
        ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> a2;
        ComPtr<ICoreWebView2ObjectCollectionView> objs;
        if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&a2))) && SUCCEEDED(a2->get_AdditionalObjects(&objs))) {
            UINT32 n = 0;
            objs->get_Count(&n);
            for (UINT32 i = 0; i < n; i++) {
                ComPtr<IUnknown> o;
                ComPtr<ICoreWebView2File> f;
                LPWSTR p = nullptr;
                if (SUCCEEDED(objs->GetValueAtIndex(i, &o)) && SUCCEEDED(o.As(&f)) && SUCCEEDED(f->get_Path(&p))) {
                    paths.push_back(p);
                    CoTaskMemFree(p);
                }
            }
        }
        send_files(paths);
    } else if (c == "browse") {
        send_files(pick(false, true, {{L"Audio", kAudio}, {L"All files", L"*.*"}}, L"Add songs"));
    } else if (c == "pickFolder") {
        auto r = pick(true, false, {}, L"Where should the finished files go?", W(m.value("current", "")));
        if (!r.empty()) {
            fs::create_directories(r[0]);
            send({{"type", "folder"}, {"path", U(r[0])}, {"thenStart", m.value("thenStart", false)}});
        } else {
            send({{"type", "folder"}, {"path", ""}, {"thenStart", false}});
        }
    } else if (c == "pickCover") {
        auto r = pick(false, false, {{L"Images", L"*.jpg;*.jpeg;*.png"}}, L"Cover image");
        if (!r.empty()) send({{"type", "cover"}, {"id", m["id"]}, {"path", U(r[0])}, {"url", data_url(r[0])}});
    } else if (c == "pickBackground") {
        std::vector<std::wstring> r;
        if (_wgetenv(L"MT_TEST_BACKGROUND") && !m.value("path", std::string()).empty()) r.push_back(W(m["path"]));  // testing
        else r = pick(false, false, {{L"Videos, GIFs and pictures", kClips}, {L"All files", L"*.*"}}, L"Background video");
        if (!r.empty()) std::thread(cmd_background, m.value("id", std::string()), r[0]).detach();
    } else if (c == "tags") {
        std::thread(cmd_tags, m).detach();
    } else if (c == "lyrics") {
        std::thread(cmd_lyrics, m).detach();
    } else if (c == "saveLyrics") {
        cmd_save_lyrics(m);
    } else if (c == "start") {
        cmd_start(m);
    } else if (c == "cancel") {
        cmd_cancel();
    } else if (c == "open") {
        ShellExecuteW(g_hwnd, L"open", W(m["path"]).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (c == "reveal") {
        std::wstring a = L"/select," + quote(W(m["path"]));
        ShellExecuteW(g_hwnd, L"open", L"explorer.exe", a.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

// ------------------------------------------------------------------ window

static bool dark_theme() {
    DWORD v = 1, n = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n);
    return v == 0;
}

static void title_bar_theme(HWND h) {
    BOOL d = dark_theme();
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof d);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TO_PAGE: {
            std::string* s = (std::string*)lp;
            if (g_view) g_view->PostWebMessageAsJson(W(*s).c_str());
            delete s;
            return 0;
        }
        case WM_SIZE:
            if (g_controller) {
                RECT r;
                GetClientRect(h, &r);
                g_controller->put_Bounds(r);
            }
            return 0;
        case WM_SETTINGCHANGE:
            title_bar_theme(h);
            break;
        case WM_GETMINMAXINFO: {
            UINT dpi = GetDpiForWindow(h);
            ((MINMAXINFO*)lp)->ptMinTrackSize = {MulDiv(900, dpi, 96), MulDiv(600, dpi, 96)};
            return 0;
        }
        case WM_CLOSE:
            if (g_busy && MessageBoxW(h, L"Songs are still being made. Stop and close Sora?", L"Sora",
                                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                return 0;
            break;
        case WM_DESTROY:
            cmd_cancel();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    g_app = fs::path(exe).parent_path();
    if (const wchar_t* e = _wgetenv(L"MT_APP_DIR")) g_app = e;  // dev: run the window against an assembled app folder
    g_work = known(FOLDERID_LocalAppData) / "Sora" / "work";
    fs::create_directories(g_work);

    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);

    if (!fs::exists(g_app / "ui" / "index.html") || !fs::exists(g_app / "engine" / "mtn.exe")) {
        MessageBoxW(nullptr, (L"Sora's files are missing from " + g_app.wstring() + L". Reinstall Sora.").c_str(),
                    L"Sora", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = CreateSolidBrush(dark_theme() ? RGB(28, 28, 30) : RGB(246, 246, 247));
    wc.lpszClassName = L"SoraWindow";
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    int w = MulDiv(1180, dpi, 96), h = MulDiv(780, dpi, 96);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Sora", WS_OVERLAPPEDWINDOW, wa.left + (wa.right - wa.left - w) / 2,
                             wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    title_bar_theme(g_hwnd);
    ShowWindow(g_hwnd, show);

    fs::path data = known(FOLDERID_LocalAppData) / "Sora" / "WebView2";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT r, ICoreWebView2Environment* env) {
            if (FAILED(r)) {
                MessageBoxW(g_hwnd, L"Microsoft Edge WebView2 is missing. Reinstall Sora (it installs WebView2).",
                            L"Sora", MB_ICONERROR);
                return r;
            }
            return env->CreateCoreWebView2Controller(
                g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT r, ICoreWebView2Controller* c) {
                                if (FAILED(r)) return r;
                                g_controller = c;
                                c->get_CoreWebView2(&g_view);
                                RECT b;
                                GetClientRect(g_hwnd, &b);
                                c->put_Bounds(b);
                                ComPtr<ICoreWebView2Settings> s;
                                g_view->get_Settings(&s);
                                bool dev = _wgetenv(L"MT_DEV") != nullptr;
                                s->put_AreDevToolsEnabled(dev);
                                s->put_AreDefaultContextMenusEnabled(dev);
                                s->put_IsStatusBarEnabled(FALSE);
                                s->put_IsZoomControlEnabled(FALSE);
                                ComPtr<ICoreWebView2_3> v3;
                                if (SUCCEEDED(g_view.As(&v3)))
                                    v3->SetVirtualHostNameToFolderMapping(L"app.sora", (g_app / "ui").c_str(),
                                                                          COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                EventRegistrationToken t;
                                g_view->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) {
                                            on_message(a);
                                            return S_OK;
                                        }).Get(), &t);
                                g_view->Navigate(L"https://app.sora/index.html");
                                return S_OK;
                            }).Get());
        }).Get());
    if (FAILED(hr)) {
        MessageBoxW(g_hwnd, L"Microsoft Edge WebView2 is missing. Reinstall Sora (it installs WebView2).", L"Sora",
                    MB_ICONERROR);
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(g_job);  // kills anything still running
    return 0;
}
