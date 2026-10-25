#include "http.h"

#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace mt {

static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string url_encode(const std::string& s) {
    static const char* hexd = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else if (c == ' ') o += "%20";
        else o += {'%', hexd[c >> 4], hexd[c & 15]};
    }
    return o;
}

static HttpResult get_once(const std::string& url);

// retries what's worth retrying (no connection, 429 rate limit, 5xx overload: LRCLIB answers 503 under load) after
// 1, 3, 7 and 15 seconds (LRCLIB flips between 200 and 503 for the same query within seconds)
HttpResult http_get(const std::string& url) {
    HttpResult r;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt) Sleep((DWORD)((1u << attempt) - 1) * 1000);
        r = get_once(url);
        if (r.status != 0 && r.status != 429 && r.status < 500) break;
    }
    return r;
}

static HttpResult get_once(const std::string& url) {
    HttpResult res;
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path);
    wchar_t extra[4096];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = (DWORD)std::size(extra);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return res;
    std::wstring object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);

    // a browser user agent: lyric sites answer plain clients with a block page
    HINTERNET ses = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                                L"Chrome/141.0 Safari/537.36",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return res;
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(ses, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof decomp);
    WinHttpSetTimeouts(ses, 10000, 10000, 20000, 30000);
    HINTERNET con = WinHttpConnect(ses, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req && WinHttpSendRequest(req, L"Accept-Language: en\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &len, WINHTTP_NO_HEADER_INDEX);
        res.status = (int)code;
        std::vector<char> buf(1 << 16);
        DWORD got = 0;
        while (WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got) && got) res.body.append(buf.data(), got);
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return res;
}

}  // namespace mt
