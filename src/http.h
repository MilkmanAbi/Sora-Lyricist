// Minimal HTTPS GET over WinHTTP (built into Windows: no extra DLLs). Follows redirects, accepts gzip.
#pragma once
#include <string>

namespace mt {

struct HttpResult {
    int status = 0;  // 0 = no connection
    std::string body;
};

HttpResult http_get(const std::string& url);
std::string url_encode(const std::string& s);

}  // namespace mt
