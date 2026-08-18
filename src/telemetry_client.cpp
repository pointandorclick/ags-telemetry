#include "telemetry_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "http_transport.h"
#include "json_util.h"

namespace fs = std::filesystem;

namespace agstel {
namespace {

// ---- tunables ---------------------------------------------------------------
constexpr size_t kQueueCap = 5000;   // max in-memory lines before oldest spill to disk
constexpr size_t kFlushAt = 20;      // flush as soon as this many lines are queued
constexpr size_t kBatchMax = 200;    // max lines per POST
constexpr int kFlushIntervalMs = 3000;
constexpr int kConnectTimeoutMs = 3000;
constexpr int kTotalTimeoutMs = 6000;
constexpr int kShutdownConnectMs = 1000;
constexpr int kShutdownTotalMs = 2000;
constexpr int kBackoffStartMs = 1000;
constexpr int kBackoffMaxMs = 60000;
constexpr int kFatal4xxLimit = 3;
constexpr size_t kReplayMaxBytes = 4 * 1024 * 1024;  // per pending file

// ---- shared state (guarded by g_m unless atomic) -----------------------------
std::mutex g_m;
std::condition_variable g_cv;
std::deque<std::string> g_queue;
Config g_cfg;
bool g_running = false;
bool g_workerFinished = false;
bool g_endRequested = false;
bool g_shutdownRequested = false;
bool g_statsSet = false;
int g_endSessionSeconds = 0;
int g_endActiveSeconds = 0;
int g_endIdleSeconds = 0;
std::chrono::steady_clock::time_point g_connectedAt;
std::atomic<int> g_status{kOff};
std::atomic<long> g_sessionId{-1};
std::thread g_worker;

// ---- helpers (worker thread only, unless noted) -------------------------------

std::vector<std::pair<std::string, std::string>> JsonHeaders() {
    std::vector<std::pair<std::string, std::string>> h;
    h.emplace_back("Content-Type", "application/json");
    if (!g_cfg.apiKey.empty()) h.emplace_back("Authorization", "Bearer " + g_cfg.apiKey);
    return h;
}

bool Stopping() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_shutdownRequested;
}

// Interruptible sleep; returns early if end/shutdown requested or queue overflows.
void BackoffWait(int ms) {
    std::unique_lock<std::mutex> lk(g_m);
    g_cv.wait_for(lk, std::chrono::milliseconds(ms), [] {
        return g_shutdownRequested || g_endRequested || g_queue.size() > kQueueCap;
    });
}

std::string SpoolFileName(long sessionId) {
    std::ostringstream name;
    static int counter = 0;
    if (sessionId >= 0)
        name << "pending-events-" << sessionId << "-" << std::time(nullptr) << "-" << ++counter << ".log";
    else
        name << "pending-import-" << std::time(nullptr) << "-" << ++counter << ".log";
    return name.str();
}

// Appends lines to a spool file so they can be delivered on the next game run.
void SpoolLines(const std::vector<std::string>& lines, long sessionId) {
    if (g_cfg.spoolDir.empty() || lines.empty()) return;
    std::error_code ec;
    fs::create_directories(g_cfg.spoolDir, ec);
    std::ofstream f(fs::path(g_cfg.spoolDir) / SpoolFileName(sessionId), std::ios::app);
    if (!f) return;
    for (const auto& line : lines) f << line << "\n";
}

// Drains everything currently queued into a spool file (used on shutdown/error).
void SpoolQueue() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(g_m);
        lines.assign(g_queue.begin(), g_queue.end());
        g_queue.clear();
    }
    SpoolLines(lines, g_sessionId.load());
}

// Posts events and returns true on success. On success, resp is populated with
// the server response (used to extract bugIds for screenshot uploads).
bool PostEvents(long sessionId, const std::vector<std::string>& lines,
                int connectMs, int totalMs, HttpResponse* respOut = nullptr) {
    std::ostringstream body;
    body << "{\"sessionId\":" << sessionId << ",\"rawLines\":[";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) body << ",";
        body << "\"" << JsonEscape(lines[i]) << "\"";
    }
    body << "]}";
    HttpResponse resp;
    bool ok = HttpRequest("POST", g_cfg.baseUrl + "/api/collect/events",
                          JsonHeaders(), body.str(), &resp, connectMs, totalMs);
    bool success = ok && resp.status >= 200 && resp.status < 300;
    if (success && respOut) *respOut = std::move(resp);
    return success;
}

// Resolves an AGS script path (e.g. "$SAVEGAMEDIR$/telemetry/bug_1.bmp") to a
// real filesystem path using the resolved savegameDir from config.
std::string ResolveScreenshotPath(const std::string& scriptPath) {
    const char* prefix = "$SAVEGAMEDIR$";
    size_t prefixLen = std::strlen(prefix);
    if (scriptPath.compare(0, prefixLen, prefix) == 0) {
        return g_cfg.savegameDir + scriptPath.substr(prefixLen);
    }
    return scriptPath;  // already a real path
}

// Extracts the screenshot= value from a bug_report raw line.
std::string ExtractScreenshotField(const std::string& line) {
    const char* key = "screenshot=";
    size_t pos = line.find(key);
    if (pos == std::string::npos) return {};
    pos += std::strlen(key);
    size_t end = line.find('|', pos);
    if (end == std::string::npos) end = line.size();
    return line.substr(pos, end - pos);
}

// Uploads a BMP screenshot file to the dashboard for a specific bug report.
bool PostScreenshot(long bugId, const std::string& filePath,
                    int connectMs, int totalMs) {
    std::ifstream f(filePath, std::ios::binary);
    if (!f) return false;
    std::string fileData((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    if (fileData.empty()) return false;

    const std::string boundary = "----AgsTelemetryBoundary";
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.bmp\"\r\n";
    body += "Content-Type: image/bmp\r\n\r\n";
    body += fileData;
    body += "\r\n--" + boundary + "--\r\n";

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "multipart/form-data; boundary=" + boundary);
    if (!g_cfg.apiKey.empty()) headers.emplace_back("Authorization", "Bearer " + g_cfg.apiKey);

    std::ostringstream url;
    url << g_cfg.baseUrl << "/api/bugs/" << bugId << "/screenshot";

    HttpResponse resp;
    bool ok = HttpRequest("POST", url.str(), headers, body, &resp, connectMs, totalMs);
    return ok && resp.status >= 200 && resp.status < 300;
}

// After a successful PostEvents, uploads screenshots for any bug_report lines
// in the batch. bugIds from the server response are matched to bug_report lines
// in order.
void UploadBatchScreenshots(const std::vector<std::string>& batch,
                            const HttpResponse& resp,
                            int connectMs, int totalMs) {
    if (g_cfg.savegameDir.empty()) return;

    std::vector<long> bugIds = ExtractLongArray(resp.body, "bugIds");
    if (bugIds.empty()) return;

    // Collect screenshot paths from bug_report lines in batch order.
    std::vector<std::string> screenshotPaths;
    for (const auto& line : batch) {
        if (line.find("|bug_report|") == std::string::npos) continue;
        std::string path = ExtractScreenshotField(line);
        if (!path.empty()) screenshotPaths.push_back(path);
    }

    // Match bugIds to screenshot paths (same order from the server).
    size_t count = std::min(bugIds.size(), screenshotPaths.size());
    for (size_t i = 0; i < count; ++i) {
        std::string resolved = ResolveScreenshotPath(screenshotPaths[i]);
        PostScreenshot(bugIds[i], resolved, connectMs, totalMs);
    }
}

// Negative activeSeconds/idleSeconds are omitted (server keeps them null).
bool PatchSessionEnd(long sessionId, int sessionSeconds, int activeSeconds,
                     int idleSeconds, int connectMs, int totalMs) {
    std::ostringstream body;
    body << "{\"sessionSeconds\":" << sessionSeconds;
    if (activeSeconds >= 0) body << ",\"activeSeconds\":" << activeSeconds;
    if (idleSeconds >= 0) body << ",\"idleSeconds\":" << idleSeconds;
    body << "}";
    HttpResponse resp;
    std::ostringstream url;
    url << g_cfg.baseUrl << "/api/collect/session/" << sessionId;
    bool ok = HttpRequest("PATCH", url.str(), JsonHeaders(), body.str(), &resp,
                          connectMs, totalMs);
    return ok && resp.status >= 200 && resp.status < 300;
}

// Parses "...|session_end|session_seconds=N|active_seconds=N|idle_seconds=N".
bool ParseSessionEnd(const std::string& line, int* s, int* a, int* i) {
    if (line.find("|session_end|") == std::string::npos) return false;
    auto grab = [&](const char* key, int* out) {
        size_t p = line.find(key);
        if (p == std::string::npos) return;
        *out = std::atoi(line.c_str() + p + std::strlen(key));
    };
    *s = *a = *i = 0;
    grab("session_seconds=", s);
    grab("active_seconds=", a);
    grab("idle_seconds=", i);
    return true;
}

// ---- spool replay (previous runs' leftovers) ----------------------------------

void ReplayImportFile(const fs::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return;
    std::ostringstream content;
    content << f.rdbuf();
    f.close();
    if (content.str().empty() || content.str().size() > kReplayMaxBytes) {
        std::error_code ec;
        fs::remove(file, ec);  // empty or oversized: drop rather than retry forever
        return;
    }
    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "text/plain");
    if (!g_cfg.apiKey.empty()) headers.emplace_back("Authorization", "Bearer " + g_cfg.apiKey);
    HttpResponse resp;
    bool ok = HttpRequest("POST", g_cfg.baseUrl + "/api/collect/import", headers,
                          content.str(), &resp, kConnectTimeoutMs, kTotalTimeoutMs);
    if (ok && ((resp.status >= 200 && resp.status < 300) || resp.status == 400)) {
        // 400 = server rejected the content itself; retrying won't help
        std::error_code ec;
        fs::remove(file, ec);
    }
}

void ReplayEventsFile(const fs::path& file, long sessionId) {
    std::ifstream f(file);
    if (!f) return;
    std::vector<std::string> lines;
    std::string line;
    size_t bytes = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        bytes += line.size();
        if (bytes > kReplayMaxBytes) break;
        lines.push_back(line);
    }
    f.close();
    if (lines.empty()) {
        std::error_code ec;
        fs::remove(file, ec);
        return;
    }
    // Post in kBatchMax chunks; bail (keep file) on first failure.
    for (size_t i = 0; i < lines.size(); i += kBatchMax) {
        std::vector<std::string> batch(
            lines.begin() + i,
            lines.begin() + std::min(lines.size(), i + kBatchMax));
        if (!PostEvents(sessionId, batch, kConnectTimeoutMs, kTotalTimeoutMs)) return;
    }
    int s, a, idle;
    for (const auto& l : lines) {
        if (ParseSessionEnd(l, &s, &a, &idle)) {
            PatchSessionEnd(sessionId, s, a, idle, kConnectTimeoutMs, kTotalTimeoutMs);
            break;
        }
    }
    std::error_code ec;
    fs::remove(file, ec);
}

void ReplayPending() {
    if (g_cfg.spoolDir.empty()) return;
    std::error_code ec;
    if (!fs::is_directory(g_cfg.spoolDir, ec)) return;
    for (const auto& entry : fs::directory_iterator(g_cfg.spoolDir, ec)) {
        if (Stopping()) return;
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("pending-import-", 0) == 0) {
            ReplayImportFile(entry.path());
        } else if (name.rfind("pending-events-", 0) == 0) {
            long sid = std::atol(name.c_str() + std::strlen("pending-events-"));
            if (sid > 0) ReplayEventsFile(entry.path(), sid);
        }
    }
}

// ---- session establishment -----------------------------------------------------

// Returns true when connected; false when permanently rejected or shutting down.
bool EstablishSession() {
    int backoff = kBackoffStartMs;
    int rejections = 0;
    while (!Stopping()) {
        std::ostringstream body;
        body << "{\"fullVersion\":\"" << JsonEscape(g_cfg.fullVersion) << "\""
             << ",\"platform\":\"" << JsonEscape(g_cfg.platform) << "\""
             << ",\"runtimeInfo\":\"" << JsonEscape(g_cfg.runtimeInfo) << "\"}";
        HttpResponse resp;
        bool ok = HttpRequest("POST", g_cfg.baseUrl + "/api/collect/session",
                              JsonHeaders(), body.str(), &resp,
                              kConnectTimeoutMs, kTotalTimeoutMs);
        if (ok && resp.status >= 200 && resp.status < 300) {
            long sid = ExtractLong(resp.body, "sessionId", -1);
            if (sid >= 0) {
                g_sessionId.store(sid);
                g_status.store(kConnected);
                {
                    std::lock_guard<std::mutex> lk(g_m);
                    g_connectedAt = std::chrono::steady_clock::now();
                }
                return true;
            }
            ok = false;  // 2xx without a sessionId — treat as server error, retry
        }
        if (ok && resp.status >= 400 && resp.status < 500) {
            if (++rejections >= kFatal4xxLimit) {
                g_status.store(kError);  // bad key / unregistered version — stop asking
                return false;
            }
        }
        // Also drain overflow while we wait so long offline sessions stay bounded.
        {
            std::unique_lock<std::mutex> lk(g_m);
            if (g_queue.size() > kQueueCap) {
                std::vector<std::string> spill(g_queue.begin(),
                                               g_queue.begin() + kQueueCap / 2);
                g_queue.erase(g_queue.begin(), g_queue.begin() + kQueueCap / 2);
                lk.unlock();
                SpoolLines(spill, -1);
            }
        }
        if (g_endRequested || g_shutdownRequested) return false;
        BackoffWait(backoff);
        backoff = std::min(backoff * 2, kBackoffMaxMs);
    }
    return false;
}

// ---- streaming loop -------------------------------------------------------------

void StreamEvents() {
    int backoff = kBackoffStartMs;
    for (;;) {
        std::vector<std::string> batch;
        bool endNow = false;
        bool shutdownNow = false;
        {
            std::unique_lock<std::mutex> lk(g_m);
            g_cv.wait_for(lk, std::chrono::milliseconds(kFlushIntervalMs), [] {
                return g_queue.size() >= kFlushAt || g_endRequested || g_shutdownRequested;
            });
            shutdownNow = g_shutdownRequested;
            size_t take = std::min(g_queue.size(), kBatchMax);
            batch.assign(g_queue.begin(), g_queue.begin() + take);
            g_queue.erase(g_queue.begin(), g_queue.begin() + take);
            endNow = g_endRequested && g_queue.empty();
        }

        if (!batch.empty()) {
            int connectMs = shutdownNow ? kShutdownConnectMs : kConnectTimeoutMs;
            int totalMs = shutdownNow ? kShutdownTotalMs : kTotalTimeoutMs;
            HttpResponse eventsResp;
            if (PostEvents(g_sessionId.load(), batch, connectMs, totalMs, &eventsResp)) {
                backoff = kBackoffStartMs;
                UploadBatchScreenshots(batch, eventsResp, connectMs, totalMs);
            } else if (shutdownNow) {
                SpoolLines(batch, g_sessionId.load());
                SpoolQueue();
                return;
            } else {
                // Put the batch back and retry after backoff; overflow spills to
                // disk. The spool write happens outside the lock so the game
                // thread's Send() never waits on disk I/O.
                std::vector<std::string> spill;
                {
                    std::lock_guard<std::mutex> lk(g_m);
                    g_queue.insert(g_queue.begin(), batch.begin(), batch.end());
                    if (g_queue.size() > kQueueCap) {
                        spill.assign(g_queue.begin(), g_queue.begin() + kQueueCap / 2);
                        g_queue.erase(g_queue.begin(), g_queue.begin() + kQueueCap / 2);
                    }
                }
                if (!spill.empty()) SpoolLines(spill, g_sessionId.load());
                BackoffWait(backoff);
                backoff = std::min(backoff * 2, kBackoffMaxMs);
                continue;
            }
        }

        if (endNow) {
            int s, a, i;
            {
                std::lock_guard<std::mutex> lk(g_m);
                if (g_statsSet) {
                    s = g_endSessionSeconds;
                    a = g_endActiveSeconds;
                    i = g_endIdleSeconds;
                } else {
                    // No stats ever supplied (game quit without EndSession and
                    // never ticked UpdateStats) — session wall time is still
                    // better than leaving the session dangling open.
                    s = (int)std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - g_connectedAt)
                            .count();
                    a = -1;
                    i = -1;
                }
            }
            int connectMs = shutdownNow ? kShutdownConnectMs : kConnectTimeoutMs;
            int totalMs = shutdownNow ? kShutdownTotalMs : kTotalTimeoutMs;
            if (!PatchSessionEnd(g_sessionId.load(), s, a, i, connectMs, totalMs) &&
                !shutdownNow) {
                PatchSessionEnd(g_sessionId.load(), s, a, i,
                                kShutdownConnectMs, kShutdownTotalMs);
            }
            return;
        }
        if (shutdownNow) {
            SpoolQueue();
            return;
        }
    }
}

void WorkerMain() {
    g_status.store(kConnecting);
    ReplayPending();

    if (EstablishSession()) {
        StreamEvents();
    } else {
        // Never connected: preserve whatever the game produced for next run.
        SpoolQueue();
    }
    if (g_status.load() != kError) g_status.store(kOff);

    std::lock_guard<std::mutex> lk(g_m);
    g_workerFinished = true;
    g_cv.notify_all();
}

}  // namespace

// ---- public API (game thread) ----------------------------------------------------

void Init(const Config& cfg) {
    std::lock_guard<std::mutex> lk(g_m);
    if (g_running) return;
    g_cfg = cfg;
    // Trim trailing slash so path concatenation stays predictable.
    while (!g_cfg.baseUrl.empty() && g_cfg.baseUrl.back() == '/') g_cfg.baseUrl.pop_back();
    if (g_cfg.baseUrl.empty()) return;
    g_running = true;
    g_workerFinished = false;
    g_endRequested = false;
    g_shutdownRequested = false;
    g_statsSet = false;
    g_worker = std::thread(WorkerMain);
}

void Send(const std::string& rawLine) {
    if (rawLine.empty()) return;
    std::lock_guard<std::mutex> lk(g_m);
    // Accept lines even before Init: the worker flushes them once started.
    // Hard cap at 2x the soft cap protects memory if Init never comes.
    if (g_queue.size() >= kQueueCap * 2) g_queue.pop_front();
    g_queue.push_back(rawLine);
    if (g_queue.size() >= kFlushAt) g_cv.notify_all();
}

void UpdateStats(int sessionSeconds, int activeSeconds, int idleSeconds) {
    std::lock_guard<std::mutex> lk(g_m);
    g_endSessionSeconds = sessionSeconds;
    g_endActiveSeconds = activeSeconds;
    g_endIdleSeconds = idleSeconds;
    g_statsSet = true;
}

void EndSession(int sessionSeconds, int activeSeconds, int idleSeconds) {
    std::lock_guard<std::mutex> lk(g_m);
    g_endSessionSeconds = sessionSeconds;
    g_endActiveSeconds = activeSeconds;
    g_endIdleSeconds = idleSeconds;
    g_statsSet = true;
    g_endRequested = true;
    g_cv.notify_all();
}

int GetStatus() {
    return g_status.load();
}

void Shutdown(int graceMs) {
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (!g_running) return;
        // AGS has no reliable script-side quit hook, so engine shutdown doubles
        // as EndSession: the worker flushes and PATCHes the session closed with
        // the latest UpdateStats values (or wall time as a fallback).
        g_endRequested = true;
        g_shutdownRequested = true;
        g_cv.notify_all();
    }
    std::unique_lock<std::mutex> lk(g_m);
    bool finished = g_cv.wait_for(lk, std::chrono::milliseconds(graceMs),
                                  [] { return g_workerFinished; });
    lk.unlock();
    if (finished) {
        g_worker.join();
    } else {
        // Process is exiting; the OS reclaims the thread. Data is on disk (spool +
        // the script module's own local log), so nothing is lost, only late.
        g_worker.detach();
    }
    std::lock_guard<std::mutex> lk2(g_m);
    g_running = false;
}

}  // namespace agstel
