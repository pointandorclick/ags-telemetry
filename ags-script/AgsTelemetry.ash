// AGS Telemetry plugin — script API reference
//
// NOTE: When the plugin is enabled in the AGS editor, these imports are
// injected automatically (the plugin registers its own script header).
// Do NOT also add this file as a module header in that case — you'd get
// duplicate-import errors. This file documents the API and can serve as a
// starting point for a no-op stub module on platforms without the plugin.
//
// All functions return immediately; network I/O happens on a background
// thread inside the plugin. Events are batched (~3s / 20 events) and
// anything undeliverable is spooled to disk and re-sent on the next launch.

// Starts a telemetry session against an ags-telemetry-dashboard server.
//   serverUrl:   e.g. "https://telemetry.example.com" or "http://localhost:3000"
//   apiKey:      TELEMETRY_API_KEY of the dashboard (null/empty if none)
//   fullVersion: version string the dashboard parses, e.g. "0.2.1-beta-maya"
//   platform:    e.g. "windows", "mac"
//   runtimeInfo: System.RuntimeInfo
//   spoolDir:    where undelivered events are kept, e.g. "$SAVEGAMEDIR$/telemetry"
import void AgsTelemetry_Init(String serverUrl, String apiKey, String fullVersion, String platform, String runtimeInfo, String spoolDir);

// Queues one pipe-delimited event line: "YYYY-MM-DD HH:MM:SS|event_type|k=v|k=v"
import void AgsTelemetry_Send(String rawLine);

// Feeds the plugin the running timing stats (call ~once a second from your
// tick). AGS never calls a script function on quit, so the plugin closes the
// session itself at engine shutdown using the latest values supplied here.
import void AgsTelemetry_UpdateStats(int sessionSeconds, int activeSeconds, int idleSeconds);

// Flushes remaining events and closes the session with timing stats.
import void AgsTelemetry_EndSession(int sessionSeconds, int activeSeconds, int idleSeconds);

// 0 = off, 1 = connecting, 2 = connected/streaming, 3 = rejected by server
import int AgsTelemetry_GetStatus();
