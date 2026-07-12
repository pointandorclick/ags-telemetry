// Minimal blocking HTTP transport, implemented per-platform:
//   http_winhttp.cpp (Windows, WinHTTP)  /  http_curl.cpp (macOS/Linux, libcurl)
// Only ever called from the telemetry worker thread — never from the game thread.
#pragma once

#include <string>
#include <utility>
#include <vector>

struct HttpResponse {
    long status = 0;  // 0 = transport failure (no HTTP exchange happened)
    std::string body;
};

// method: "GET", "POST" or "PATCH". Returns true if an HTTP exchange completed
// (any status code); false on connect/DNS/TLS/timeout failure.
bool HttpRequest(const std::string& method,
                 const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body,
                 HttpResponse* resp,
                 int connectTimeoutMs,
                 int totalTimeoutMs);
