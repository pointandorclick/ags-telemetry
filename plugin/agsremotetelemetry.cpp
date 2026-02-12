// AGS Remote Telemetry Plugin
// Sends telemetry events to a dashboard server in real-time.
// Uses a background thread for non-blocking HTTP.
//
// HTTP backend:
//   Windows: WinHTTP (built-in, no external dependencies)
//   macOS/Linux: libcurl
//
// Usage: define TELEMETRY_SERVER_URL in your AGS project

#if defined(_WIN32)
#undef WINDOWS_VERSION
#define WINDOWS_VERSION
// Include Windows headers BEFORE agsplugin.h so its type guards
// (_WINDOWS_, _WINGDI_) detect the real types and skip the stubs.
#include <windows.h>
#include <winhttp.h>
#endif

#define THIS_IS_THE_PLUGIN
#include "agsplugin.h"

#ifndef WINDOWS_VERSION
#include <curl/curl.h>
#endif

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static IAGSEngine *engine = nullptr;
static IAGSEditor *editor = nullptr;

// Configuration
static std::string g_serverUrl;
static std::string g_apiKey;

// Thread-safe event queue
static std::mutex g_queueMutex;
static std::condition_variable g_queueCV;
static std::queue<std::string> g_eventQueue;

// Background thread
static std::thread g_senderThread;
static std::atomic<bool> g_threadRunning{false};
static std::atomic<bool> g_shutdownRequested{false};

// Session state (accessed only on background thread)
static int g_sessionId = -1;
static bool g_sessionPending = false;
static std::string g_pendingVersion;
static std::string g_pendingPlatform;
static std::string g_pendingRuntime;

// Cache file for offline events
static std::string g_cachePath;

// Parsed URL components (used by WinHTTP backend)
static std::string g_urlHost;
static int g_urlPort = 80;
static bool g_urlSecure = false;

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
static std::string jsonEscape(const std::string &s)
{
    std::string r;
    r.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    r += buf;
                } else {
                    r += c;
                }
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------
static void parseBaseUrl(const std::string &url)
{
    // Extract scheme, host, port from the server URL
    std::string remainder = url;

    if (remainder.substr(0, 8) == "https://") {
        g_urlSecure = true;
        g_urlPort = 443;
        remainder = remainder.substr(8);
    } else if (remainder.substr(0, 7) == "http://") {
        g_urlSecure = false;
        g_urlPort = 80;
        remainder = remainder.substr(7);
    }

    // Remove trailing path
    size_t slash = remainder.find('/');
    if (slash != std::string::npos) {
        remainder = remainder.substr(0, slash);
    }

    // Check for port
    size_t colon = remainder.find(':');
    if (colon != std::string::npos) {
        g_urlHost = remainder.substr(0, colon);
        g_urlPort = atoi(remainder.substr(colon + 1).c_str());
    } else {
        g_urlHost = remainder;
    }
}

// Extract path from a full URL (everything after host:port)
static std::string extractPath(const std::string &url)
{
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return url;
    size_t pathStart = url.find('/', schemeEnd + 3);
    if (pathStart == std::string::npos) return "/";
    return url.substr(pathStart);
}

// ---------------------------------------------------------------------------
// Event parsing
// ---------------------------------------------------------------------------
struct EventField {
    std::string key;
    std::string value;
};

struct ParsedEvent {
    std::string timestamp;
    std::string eventType;
    std::vector<EventField> fields;

    std::string getField(const std::string &k) const {
        for (auto &f : fields) {
            if (f.key == k) return f.value;
        }
        return "";
    }
};

static ParsedEvent parseEventLine(const std::string &line)
{
    ParsedEvent ev;
    size_t pos = 0;
    int part = 0;

    while (pos < line.size()) {
        size_t next = line.find('|', pos);
        if (next == std::string::npos) next = line.size();
        std::string token = line.substr(pos, next - pos);

        if (part == 0) {
            ev.timestamp = token;
        } else if (part == 1) {
            ev.eventType = token;
        } else {
            size_t eq = token.find('=');
            if (eq != std::string::npos) {
                ev.fields.push_back({token.substr(0, eq), token.substr(eq + 1)});
            }
        }
        part++;
        pos = next + 1;
    }
    return ev;
}

// Build JSON for a single event (for the events array)
static std::string eventToJson(const ParsedEvent &ev)
{
    std::string json = "{\"timestamp\":\"" + jsonEscape(ev.timestamp) + "\","
                       "\"eventType\":\"" + jsonEscape(ev.eventType) + "\","
                       "\"fields\":{";
    bool first = true;
    for (auto &f : ev.fields) {
        if (!first) json += ",";
        json += "\"" + jsonEscape(f.key) + "\":\"" + jsonEscape(f.value) + "\"";
        first = false;
    }
    json += "}}";
    return json;
}

// ---------------------------------------------------------------------------
// HTTP backend
// ---------------------------------------------------------------------------

#ifdef WINDOWS_VERSION
// ---- WinHTTP backend (Windows) ----

static std::string httpRequest(const std::string &method, const std::string &url,
                               const std::string &body)
{
    std::string path = extractPath(url);

    // Convert strings to wide chars for WinHTTP
    int hostLen = MultiByteToWideChar(CP_UTF8, 0, g_urlHost.c_str(), -1, NULL, 0);
    int pathLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    int methodLen = MultiByteToWideChar(CP_UTF8, 0, method.c_str(), -1, NULL, 0);

    std::vector<wchar_t> wHost(hostLen), wPath(pathLen), wMethod(methodLen);
    MultiByteToWideChar(CP_UTF8, 0, g_urlHost.c_str(), -1, wHost.data(), hostLen);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wPath.data(), pathLen);
    MultiByteToWideChar(CP_UTF8, 0, method.c_str(), -1, wMethod.data(), methodLen);

    HINTERNET hSession = WinHttpOpen(L"AGSRemoteTelemetry/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    // Set timeouts: resolve=5s, connect=5s, send=10s, receive=10s
    WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, wHost.data(),
                                        static_cast<INTERNET_PORT>(g_urlPort), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD flags = g_urlSecure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, wMethod.data(), wPath.data(),
                                            NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Add headers
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!g_apiKey.empty()) {
        std::string authStr = "Authorization: Bearer " + g_apiKey + "\r\n";
        int authLen = MultiByteToWideChar(CP_UTF8, 0, authStr.c_str(), -1, NULL, 0);
        std::vector<wchar_t> wAuth(authLen);
        MultiByteToWideChar(CP_UTF8, 0, authStr.c_str(), -1, wAuth.data(), authLen);
        headers += wAuth.data();
    }

    WinHttpAddRequestHeaders(hRequest, headers.c_str(),
                             static_cast<DWORD>(headers.size()),
                             WINHTTP_ADDREQ_FLAG_ADD);

    BOOL result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     (LPVOID)body.c_str(),
                                     static_cast<DWORD>(body.size()),
                                     static_cast<DWORD>(body.size()), 0);
    if (!result) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    result = WinHttpReceiveResponse(hRequest, NULL);
    if (!result) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Check status code
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    // Read response body
    std::string response;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        std::vector<char> buf(bytesAvailable);
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead);
        response.append(buf.data(), bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (statusCode < 200 || statusCode >= 300) {
        return "";
    }
    return response;
}

static void httpGlobalInit() {}
static void httpGlobalCleanup() {}

#else
// ---- libcurl backend (macOS / Linux) ----

static size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t totalSize = size * nmemb;
    auto *response = static_cast<std::string *>(userp);
    response->append(static_cast<char *>(contents), totalSize);
    return totalSize;
}

static std::string httpRequest(const std::string &method, const std::string &url,
                               const std::string &body)
{
    CURL *curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (!g_apiKey.empty()) {
        std::string authHeader = "Authorization: Bearer " + g_apiKey;
        headers = curl_slist_append(headers, authHeader.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    } else if (method == "PATCH") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode < 200 || httpCode >= 300) {
        return "";
    }
    return response;
}

static void httpGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
static void httpGlobalCleanup() { curl_global_cleanup(); }

#endif

// Extract a numeric JSON field value: "fieldName": 123
static int extractJsonInt(const std::string &json, const std::string &field)
{
    std::string needle = "\"" + field + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return -1;
    pos += needle.size();
    // skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return atoi(json.c_str() + pos);
}

// ---------------------------------------------------------------------------
// Cache file (offline fallback)
// ---------------------------------------------------------------------------
static void appendToCache(const std::string &line)
{
    if (g_cachePath.empty()) return;
    FILE *f = fopen(g_cachePath.c_str(), "a");
    if (!f) return;
    fprintf(f, "%s\n", line.c_str());
    fclose(f);
}

static std::vector<std::string> readCache()
{
    std::vector<std::string> lines;
    if (g_cachePath.empty()) return lines;
    FILE *f = fopen(g_cachePath.c_str(), "r");
    if (!f) return lines;

    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
        if (len > 0) lines.emplace_back(buf, len);
    }
    fclose(f);
    return lines;
}

static void clearCache()
{
    if (g_cachePath.empty()) return;
    FILE *f = fopen(g_cachePath.c_str(), "w");
    if (f) fclose(f);
}

// ---------------------------------------------------------------------------
// Session management (called on background thread)
// ---------------------------------------------------------------------------
static bool createSession()
{
    std::string body = "{\"fullVersion\":\"" + jsonEscape(g_pendingVersion) + "\"";
    if (!g_pendingPlatform.empty())
        body += ",\"platform\":\"" + jsonEscape(g_pendingPlatform) + "\"";
    if (!g_pendingRuntime.empty())
        body += ",\"runtimeInfo\":\"" + jsonEscape(g_pendingRuntime) + "\"";
    body += "}";

    std::string resp = httpRequest("POST", g_serverUrl + "/api/collect/session", body);
    if (resp.empty()) return false;

    int sid = extractJsonInt(resp, "sessionId");
    if (sid < 0) return false;

    g_sessionId = sid;
    g_sessionPending = false;
    return true;
}

static bool sendEventBatch(const std::vector<ParsedEvent> &events)
{
    if (events.empty() || g_sessionId < 0) return true; // nothing to do

    char sidStr[32];
    snprintf(sidStr, sizeof(sidStr), "%d", g_sessionId);

    std::string body = "{\"sessionId\":" + std::string(sidStr) + ",\"events\":[";
    bool first = true;
    for (auto &ev : events) {
        if (!first) body += ",";
        body += eventToJson(ev);
        first = false;
    }
    body += "]}";

    std::string resp = httpRequest("POST", g_serverUrl + "/api/collect/events", body);
    return !resp.empty();
}

static bool endSession(const ParsedEvent &ev)
{
    if (g_sessionId < 0) return true;

    std::string body = "{";
    bool first = true;
    std::string ss = ev.getField("session_seconds");
    std::string as = ev.getField("active_seconds");
    std::string is = ev.getField("idle_seconds");

    if (!ss.empty()) {
        body += "\"sessionSeconds\":" + ss;
        first = false;
    }
    if (!as.empty()) {
        if (!first) body += ",";
        body += "\"activeSeconds\":" + as;
        first = false;
    }
    if (!is.empty()) {
        if (!first) body += ",";
        body += "\"idleSeconds\":" + is;
    }
    body += "}";

    char url[512];
    snprintf(url, sizeof(url), "%s/api/collect/session/%d", g_serverUrl.c_str(), g_sessionId);

    std::string resp = httpRequest("PATCH", url, body);
    if (!resp.empty()) {
        g_sessionId = -1;
        return true;
    }
    return false;
}

// Event types that are part of the session preamble (not sent as regular events)
static bool isPreambleEvent(const std::string &type)
{
    return type == "session_start" || type == "build" ||
           type == "platform" || type == "runtime";
}

// ---------------------------------------------------------------------------
// Rebuild a raw pipe-delimited line from a ParsedEvent
// ---------------------------------------------------------------------------
static std::string rebuildLine(const ParsedEvent &ev)
{
    std::string line = ev.timestamp + "|" + ev.eventType;
    for (auto &f : ev.fields) {
        line += "|" + f.key + "=" + f.value;
    }
    return line;
}

// ---------------------------------------------------------------------------
// Process a batch of event lines (on background thread)
// Returns lines that failed to send (for caching).
// On first HTTP failure, all remaining lines are returned as failed.
// ---------------------------------------------------------------------------
static std::vector<std::string> processLines(const std::vector<std::string> &lines)
{
    std::vector<std::string> failed;
    std::vector<ParsedEvent> eventBatch;
    std::vector<std::string> preambleLines; // raw preamble lines for the current session

    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &line = lines[i];
        ParsedEvent ev = parseEventLine(line);
        if (ev.eventType.empty()) continue;

        if (ev.eventType == "session_start") {
            g_sessionPending = true;
            g_pendingVersion.clear();
            g_pendingPlatform.clear();
            g_pendingRuntime.clear();
            preambleLines.clear();
            preambleLines.push_back(line);
            continue;
        }

        if (g_sessionPending) {
            if (ev.eventType == "build") {
                g_pendingVersion = ev.getField("version");
                preambleLines.push_back(line);
                continue;
            }
            if (ev.eventType == "platform") {
                g_pendingPlatform = ev.getField("tag");
                preambleLines.push_back(line);
                continue;
            }
            if (ev.eventType == "runtime") {
                g_pendingRuntime = ev.getField("info");
                preambleLines.push_back(line);
                if (!createSession()) {
                    // Cache preamble + all remaining lines
                    for (auto &pl : preambleLines) failed.push_back(pl);
                    for (size_t j = i + 1; j < lines.size(); j++) failed.push_back(lines[j]);
                    return failed;
                }
                preambleLines.clear();
                continue;
            }
            // Non-preamble event while session pending: create with what we have
            if (!createSession()) {
                for (auto &pl : preambleLines) failed.push_back(pl);
                for (size_t j = i; j < lines.size(); j++) failed.push_back(lines[j]);
                return failed;
            }
            preambleLines.clear();
            // Fall through to process this event normally
        }

        if (ev.eventType == "session_end") {
            // Flush pending event batch first
            if (!eventBatch.empty()) {
                if (!sendEventBatch(eventBatch)) {
                    for (auto &e : eventBatch) failed.push_back(rebuildLine(e));
                    for (size_t j = i; j < lines.size(); j++) failed.push_back(lines[j]);
                    return failed;
                }
                eventBatch.clear();
            }
            if (!endSession(ev)) {
                for (size_t j = i; j < lines.size(); j++) failed.push_back(lines[j]);
                return failed;
            }
            continue;
        }

        // Skip stray preamble events
        if (isPreambleEvent(ev.eventType)) continue;

        // Regular event - add to batch
        eventBatch.push_back(ev);

        // Flush when batch reaches 20 events
        if (eventBatch.size() >= 20) {
            if (!sendEventBatch(eventBatch)) {
                for (auto &e : eventBatch) failed.push_back(rebuildLine(e));
                for (size_t j = i + 1; j < lines.size(); j++) failed.push_back(lines[j]);
                return failed;
            }
            eventBatch.clear();
        }
    }

    // Flush remaining events
    if (!eventBatch.empty()) {
        if (!sendEventBatch(eventBatch)) {
            for (auto &e : eventBatch) failed.push_back(rebuildLine(e));
        }
    }

    return failed;
}

// ---------------------------------------------------------------------------
// Background sender thread
// ---------------------------------------------------------------------------
static void senderThreadFunc()
{
    // First, process any cached events from a previous session
    auto cached = readCache();
    if (!cached.empty()) {
        auto failed = processLines(cached);
        if (failed.empty()) {
            clearCache();
        } else {
            // Rewrite cache with only failed lines
            clearCache();
            for (auto &line : failed) appendToCache(line);
        }
    }

    while (!g_shutdownRequested.load()) {
        std::vector<std::string> batch;

        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCV.wait_for(lock, std::chrono::seconds(2), [] {
                return !g_eventQueue.empty() || g_shutdownRequested.load();
            });

            // Drain the queue
            while (!g_eventQueue.empty()) {
                batch.push_back(std::move(g_eventQueue.front()));
                g_eventQueue.pop();
            }
        }

        if (batch.empty()) continue;

        auto failed = processLines(batch);
        for (auto &line : failed) {
            appendToCache(line);
        }
    }

    // Final drain on shutdown
    std::vector<std::string> remaining;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        while (!g_eventQueue.empty()) {
            remaining.push_back(std::move(g_eventQueue.front()));
            g_eventQueue.pop();
        }
    }

    if (!remaining.empty()) {
        auto failed = processLines(remaining);
        for (auto &line : failed) {
            appendToCache(line);
        }
    }
}

// ---------------------------------------------------------------------------
// Plugin script functions
// ---------------------------------------------------------------------------
static void RT_Init(const char *serverUrl, const char *apiKey)
{
    if (g_threadRunning.load()) return; // already initialized

    g_serverUrl = serverUrl ? serverUrl : "";
    g_apiKey = apiKey ? apiKey : "";

    if (g_serverUrl.empty()) return;

    // Remove trailing slash
    while (!g_serverUrl.empty() && g_serverUrl.back() == '/')
        g_serverUrl.pop_back();

    // Parse URL components (needed by WinHTTP backend)
    parseBaseUrl(g_serverUrl);

    // Resolve cache file path
    if (engine->version >= 27) {
        char resolved[1024] = {0};
        engine->ResolveFilePath("$SAVEGAMEDIR$/telemetry/remote_cache.log",
                                resolved, sizeof(resolved));
        g_cachePath = resolved;
    } else {
        // Fallback: use current directory
        g_cachePath = "remote_cache.log";
    }

    httpGlobalInit();

    g_shutdownRequested.store(false);
    g_threadRunning.store(true);
    g_senderThread = std::thread(senderThreadFunc);
}

static void RT_Send(const char *eventLine)
{
    if (!g_threadRunning.load() || !eventLine) return;

    std::string line(eventLine);
    if (line.empty()) return;

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_eventQueue.push(std::move(line));
    }
    g_queueCV.notify_one();
}

static void RT_Update()
{
    // Signal the background thread to flush if there are pending events.
    // The thread also wakes on its own every 2 seconds.
    if (!g_threadRunning.load()) return;
    g_queueCV.notify_one();
}

static void RT_Shutdown()
{
    if (!g_threadRunning.load()) return;

    g_shutdownRequested.store(true);
    g_queueCV.notify_one();

    if (g_senderThread.joinable()) {
        g_senderThread.join();
    }

    g_threadRunning.store(false);
    httpGlobalCleanup();
}

// ---------------------------------------------------------------------------
// AGS Editor interface
// ---------------------------------------------------------------------------
static const char *ourScriptHeader =
    "import void RemoteTelemetry_Init(const string serverUrl, const string apiKey);\r\n"
    "import void RemoteTelemetry_Send(const string eventLine);\r\n"
    "import void RemoteTelemetry_Update();\r\n"
    "import void RemoteTelemetry_Shutdown();\r\n";

#if defined(WINDOWS_VERSION)
#define DLLEXPORT extern "C" __declspec(dllexport)
#else
#define DLLEXPORT extern "C" __attribute__((visibility("default")))
#endif

DLLEXPORT const char *AGS_GetPluginName()
{
    return "AGS Remote Telemetry";
}

DLLEXPORT int AGS_EditorStartup(IAGSEditor *lpEditor)
{
    editor = lpEditor;
    editor->RegisterScriptHeader(ourScriptHeader);
    return 0;
}

DLLEXPORT void AGS_EditorShutdown()
{
    if (editor) editor->UnregisterScriptHeader(ourScriptHeader);
}

// ---------------------------------------------------------------------------
// AGS Engine interface
// ---------------------------------------------------------------------------
DLLEXPORT void AGS_EngineStartup(IAGSEngine *lpEngine)
{
    engine = lpEngine;

    if (engine->version < 17) {
        engine->AbortGame("Remote Telemetry plugin requires AGS 3.4.0 or later.");
        return;
    }

    engine->RegisterScriptFunction("RemoteTelemetry_Init",     (void *)RT_Init);
    engine->RegisterScriptFunction("RemoteTelemetry_Send",     (void *)RT_Send);
    engine->RegisterScriptFunction("RemoteTelemetry_Update",   (void *)RT_Update);
    engine->RegisterScriptFunction("RemoteTelemetry_Shutdown", (void *)RT_Shutdown);
}

DLLEXPORT void AGS_EngineShutdown()
{
    RT_Shutdown();
}

DLLEXPORT intptr_t AGS_EngineOnEvent(int event, intptr_t data)
{
    return 0;
}

DLLEXPORT int AGS_EngineDebugHook(const char *scriptName, int lineNum, int reserved)
{
    return 0;
}

DLLEXPORT void AGS_EngineInitGfx(const char *driverID, void *data)
{
}

DLLEXPORT int AGS_PluginV2()
{
    return 1;
}
