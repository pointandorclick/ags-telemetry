#include "http_transport.h"

#include <windows.h>
#include <winhttp.h>

// NOTE: HttpRequest is only ever called from the telemetry worker thread, so
// the cached session/connection handles below need no locking. Reusing them
// keeps the TCP connection alive between flushes — a fresh WinHttpOpen +
// connect per request is expensive (especially under Wine, where every handle
// operation is a wineserver round-trip) and caused visible frame hitches.

namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

HINTERNET g_session = nullptr;
HINTERNET g_conn = nullptr;
std::wstring g_connHost;
INTERNET_PORT g_connPort = 0;

void CloseConnection() {
    if (g_conn) {
        WinHttpCloseHandle(g_conn);
        g_conn = nullptr;
    }
    g_connHost.clear();
    g_connPort = 0;
}

HINTERNET GetConnection(const std::wstring& host, INTERNET_PORT port) {
    if (g_conn && g_connHost == host && g_connPort == port) return g_conn;
    CloseConnection();
    if (!g_session) {
        // No proxy: skips per-request proxy-config resolution; telemetry
        // targets a direct HTTP(S) endpoint.
        g_session = WinHttpOpen(L"ags-telemetry/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_session) return nullptr;
    }
    g_conn = WinHttpConnect(g_session, host.c_str(), port, 0);
    if (g_conn) {
        g_connHost = host;
        g_connPort = port;
    }
    return g_conn;
}

}  // namespace

bool HttpRequest(const std::string& method,
                 const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body,
                 HttpResponse* resp,
                 int connectTimeoutMs,
                 int totalTimeoutMs) {
    resp->status = 0;
    resp->body.clear();

    std::wstring wurl = Widen(url);

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return false;

    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET conn = GetConnection(host, uc.nPort);
    if (!conn) return false;

    HINTERNET req = WinHttpOpenRequest(conn, Widen(method).c_str(), path,
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        CloseConnection();
        return false;
    }

    WinHttpSetTimeouts(req, connectTimeoutMs, connectTimeoutMs,
                       totalTimeoutMs, totalTimeoutMs);

    std::wstring hdrBlock;
    for (const auto& h : headers) {
        hdrBlock += Widen(h.first) + L": " + Widen(h.second) + L"\r\n";
    }

    bool ok = WinHttpSendRequest(req,
                                 hdrBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrBlock.c_str(),
                                 hdrBlock.empty() ? 0 : (DWORD)-1,
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                 (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(req, nullptr);

    if (ok) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        ok = WinHttpQueryHeaders(req,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX);
        if (ok) {
            resp->status = (long)status;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(req, &chunk[0], avail, &read) || read == 0) break;
                chunk.resize(read);
                resp->body += chunk;
            }
        }
    }

    WinHttpCloseHandle(req);
    if (!ok) {
        // Connection may be broken — drop it so the next request reconnects.
        CloseConnection();
    }
    return ok;
}
