#include "http_transport.h"

#include <curl/curl.h>

#include <mutex>

// NOTE: HttpRequest is only ever called from the telemetry worker thread, so
// the cached easy handle below needs no locking. Reusing the handle keeps the
// TCP connection alive between flushes instead of reconnecting every time.

namespace {

std::once_flag g_curlInit;
CURL* g_curl = nullptr;

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

bool HttpRequest(const std::string& method,
                 const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body,
                 HttpResponse* resp,
                 int connectTimeoutMs,
                 int totalTimeoutMs) {
    std::call_once(g_curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    if (!g_curl) g_curl = curl_easy_init();
    CURL* curl = g_curl;
    if (!curl) return false;

    resp->status = 0;
    resp->body.clear();

    // Reset options but keep the connection cache alive.
    curl_easy_reset(curl);

    struct curl_slist* hdrs = nullptr;
    for (const auto& h : headers) {
        hdrs = curl_slist_append(hdrs, (h.first + ": " + h.second).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (!body.empty() || method == "POST" || method == "PATCH") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp->body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(connectTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(totalTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ags-telemetry/1.0");

    CURLcode rc = curl_easy_perform(curl);
    bool ok = (rc == CURLE_OK);
    if (ok) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp->status = code;
    } else {
        // Connection may be in a bad state — rebuild the handle next time.
        curl_easy_cleanup(curl);
        g_curl = nullptr;
    }

    curl_slist_free_all(hdrs);
    return ok;
}
