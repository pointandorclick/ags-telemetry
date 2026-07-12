# ags-telemetry

Native Adventure Game Studio (AGS 3.6.x) engine plugin that streams gameplay
telemetry in realtime to an [ags-telemetry-dashboard](https://github.com/pointandorclick/ags-telementry-dashboard)
server — replacing the old "write a log file and hope the tester emails it"
workflow.

## How it works

```
AGS script (Telemetry.asc module)          agstelemetry plugin (native)
  _Telemetry_WriteLine(line) ───────────►  AgsTelemetry_Send(line)   [returns instantly]
      │ still appends to the local              │ in-memory queue
      ▼ telemetry.log as before                 ▼ background worker thread
                                           batches → HTTP POST /api/collect/events
                                           offline → spool to disk, re-send next launch
```

- The existing AGS **script module stays the capture layer** — the plugin is a
  thin async transport. All four script calls are non-blocking; the game never
  stalls on network I/O.
- **Session lifecycle**: `AgsTelemetry_Init` POSTs `/api/collect/session`
  (retrying with backoff until connected), events flush every ~3 s or 20
  events, `AgsTelemetry_EndSession` flushes and PATCHes the session closed.
- **Offline/failure**: events queue in memory (cap 5000, overflow to disk).
  Anything undelivered at exit is spooled to `<spoolDir>/pending-*.log` and
  re-sent on the next launch — complete offline sessions via
  `POST /api/collect/import`, partial remainders via `/api/collect/events`.
  The script module's local `telemetry.log` is untouched and remains the
  ground-truth backup.
- **Auth**: optional `Authorization: Bearer <TELEMETRY_API_KEY>`. After 3
  hard rejections (bad key / unregistered version) the plugin stops calling
  the server for the rest of the session and spools locally instead.

## Script API

Registered by the plugin (the editor injects these imports automatically when
the plugin is enabled — see `ags-script/AgsTelemetry.ash` for reference).
`ags-script/` also carries the full `Telemetry.ash`/`Telemetry.asc` capture
module (the successor to the original script-only version of this repo, now
with the plugin hooks behind `#define TELEMETRY_REMOTE`):

```agsscript
import void AgsTelemetry_Init(String serverUrl, String apiKey, String fullVersion,
                              String platform, String runtimeInfo, String spoolDir);
import void AgsTelemetry_Send(String rawLine);   // "YYYY-MM-DD HH:MM:SS|type|k=v|k=v"
import void AgsTelemetry_UpdateStats(int sessionSeconds, int activeSeconds, int idleSeconds);
import void AgsTelemetry_EndSession(int sessionSeconds, int activeSeconds, int idleSeconds);
import int  AgsTelemetry_GetStatus();            // 0 off, 1 connecting, 2 connected, 3 rejected
```

**Session closing:** AGS never calls a script function when the game quits
(`game_shutdown()` is not a real AGS entry point), so don't rely on
`AgsTelemetry_EndSession` alone. Call `AgsTelemetry_UpdateStats` about once a
second from your tick — the plugin then closes the session itself (final flush
+ PATCH with the latest stats) when the engine shuts down. Force-kills and
crashes are covered server-side: the dashboard auto-closes sessions that
receive no events for 10 minutes.

`spoolDir` accepts AGS script paths (e.g. `"$SAVEGAMEDIR$/telemetry"`); the
plugin resolves them through the engine (interface v27+).

## Building

| Target | Command | Output | HTTP stack |
|---|---|---|---|
| macOS (universal) | `scripts/build-mac.sh` | `build-mac/libagstelemetry.dylib` | system libcurl |
| Windows 32-bit | `scripts/build-windows.sh` | `build-win32/agstelemetry.dll` | WinHTTP |
| Linux x86-64 | `cmake -B build-linux && cmake --build build-linux` (on Linux/Docker) | `libagstelemetry.so` | libcurl (`libcurl4-openssl-dev`) |

Windows cross-build needs `brew install mingw-w64`. The DLL is 32-bit (PE32)
because the AGS 3.6.x editor and default engine are 32-bit; it depends only on
`kernel32`, `winhttp` and the UCRT (present on Windows 10+).

`-DBUILD_TEST_DRIVER=ON` also builds `telemetry-driver`, a CLI that simulates
a full game session against a dashboard:

```
./build-mac/telemetry-driver http://localhost:3000 <apiKey> 0.9.0-dev-me 25 /tmp/spool
```

## Installing into a game (Sierra Quest specifics)

0. **Dashboard first**: make sure the server has
   `TELEMETRY_AUTO_REGISTER_VERSIONS=true` (`.env.local` for `npm run dev`,
   compose environment for Docker — restart after changing). Without it the
   server 400-rejects sessions from any version not registered via the GitLab
   webhook: the game runs fine but nothing appears on the Live page, and the
   plugin spools the session to `<save dir>/telemetry/pending-*.log` for
   delivery on a later launch.
1. `scripts/build-mac.sh && scripts/build-windows.sh`
2. `scripts/install-sierra-quest.sh` — copies the DLL into the AGS editor dir
   (CrossOver bottle) + `Compiled/Windows/`, and the dylib into the mac app
   bundle next to the engine binary.
3. In the AGS editor: project tree → **Plugins** → right-click
   **AGS Telemetry** → *Use this plugin*.
4. In the game project: uncomment `#define TELEMETRY_REMOTE` in `Config.ash`,
   set `Telemetry_ServerUrl` / `Telemetry_ApiKey` in `TelemetryConfig_Init()`
   (Telemetry.asc), rebuild.

The Telemetry module guards every plugin call behind `TELEMETRY_REMOTE`, so
builds compile unchanged while the define is off.

## Dashboard requirements

Needs these (already implemented in ags-telemetry-dashboard):

- `POST /api/collect/events` accepts `{"sessionId": N, "rawLines": [...]}`
  in addition to structured `events`.
- `POST /api/collect/import` (text/plain log content) for spooled offline
  sessions. Returns 422 when the version is unregistered — the plugin keeps
  the file and retries next launch; 400 means "bad content, discard".
- `TELEMETRY_AUTO_REGISTER_VERSIONS=true` env var to auto-create unknown
  `(version, channel)` pairs instead of rejecting sessions that weren't
  tagged via the GitLab webhook.

The dashboard's `/live` page polls every 5 s, so sessions appear within
seconds of launch with no push infrastructure.

## Platform notes & limitations

- **Threading**: the worker thread never touches `IAGSEngine`; script-facing
  calls only mutate the queue under a mutex. Engine shutdown gives the worker
  ~2.5 s to flush, then spools what's left — worst case data arrives on the
  next launch, never lost (and the local log always has everything).
- **macOS**: the engine must load `libagstelemetry.dylib` (place next to the
  engine binary in the app bundle). If the self-built mac engine doesn't
  dlopen plugins, compile the plugin in as a builtin
  (`pl_register_builtin_plugin`) — sources are engine-agnostic C++17.
- **Web (Emscripten)**: AGS's web port doesn't support plugins; telemetry is
  a no-op there (keep `TELEMETRY_REMOTE` off for web builds).
- **Android**: possible later — build per-ABI `.so`s; not wired up yet.
