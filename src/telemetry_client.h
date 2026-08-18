// Core telemetry client: thread-safe queue + single background worker that
// talks to the ags-telemetry-dashboard collect API. AGS-free — usable from the
// plugin glue and from the standalone test driver.
//
// Threading contract: Init/Send/EndSession/GetStatus/Shutdown are called from
// the game (main) thread and never block on network I/O. All HTTP happens on
// the worker thread.
#pragma once

#include <string>

namespace agstel {

struct Config {
    std::string baseUrl;      // e.g. "http://localhost:3000" or "https://telemetry.example.com"
    std::string apiKey;       // optional; sent as "Authorization: Bearer <key>"
    std::string fullVersion;  // e.g. "0.2.1-beta-maya" — identifies version/channel/tester
    std::string platform;     // e.g. "windows", "mac"
    std::string runtimeInfo;  // free text (System.RuntimeInfo)
    std::string spoolDir;     // resolved filesystem dir for offline spool; empty disables spooling
    std::string savegameDir;  // resolved $SAVEGAMEDIR$ for screenshot path resolution
};

enum Status {
    kOff = 0,         // Init not called, or permanently disabled after fatal server rejection
    kConnecting = 1,  // session not yet established (retrying)
    kConnected = 2,   // streaming events
    kError = 3,       // server rejected us (bad key / bad version); events spool locally
};

// Starts the worker thread and asynchronously opens a session. Safe to call once
// per process run; subsequent calls are ignored while a session is active.
void Init(const Config& cfg);

// Enqueues one pipe-delimited event line ("YYYY-MM-DD HH:MM:SS|type|k=v|..").
void Send(const std::string& rawLine);

// Updates the running timing stats (cheap: mutex + three ints). Call ~once a
// second from the game's tick so the plugin can close the session with real
// numbers even when the game quits without calling EndSession — AGS has no
// reliable script-side shutdown hook, but the engine does tell the plugin.
void UpdateStats(int sessionSeconds, int activeSeconds, int idleSeconds);

// Requests the final flush and session PATCH with the given timing stats.
void EndSession(int sessionSeconds, int activeSeconds, int idleSeconds);

int GetStatus();

// Blocks up to graceMs for the worker to flush; spools anything unsent, then
// joins (or abandons) the worker. Called from AGS_EngineShutdown.
void Shutdown(int graceMs);

}  // namespace agstel
