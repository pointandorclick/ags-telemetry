# ags-telemetry

Realtime gameplay telemetry for [Adventure Game Studio](https://www.adventuregamestudio.co.uk/)
(AGS 3.6.x) games. A native engine plugin streams events — room navigation,
interactions, idle time, bug reports, milestones — to an
[ags-telemetry-dashboard](https://github.com/pointandorclick/ags-telementry-dashboard)
server while the player plays, with automatic offline spooling so no session
is ever lost. Sessions appear on the dashboard's Live page within seconds of
the game starting.

Two parts, both in this repo:

- **`src/` — the `agstelemetry` engine plugin** (C++17): a thin async HTTP
  transport. Script calls return instantly; a background thread batches,
  retries and delivers.
- **`ags-script/Telemetry.ash` / `Telemetry.asc` — the capture module** (AGS
  script): decides *what* gets logged and writes a local `telemetry.log`
  alongside the realtime stream.

## Installing into an AGS game

1. **Get the plugin binaries** — build them (see [Building](#building)) or use
   prebuilt `agstelemetry.dll` / `libagstelemetry.dylib`.

2. **Install the plugin in the editor** — copy `agstelemetry.dll` into the AGS
   editor's directory (next to `AGSEditor.exe`), restart the editor, then in
   the project tree right-click **Plugins → AGS Telemetry → Use this plugin**.

3. **Add the capture module** — import `ags-script/Telemetry.ash` and
   `Telemetry.asc` as a script module. It expects two defines earlier in your
   header chain (e.g. a `Config.ash` module that comes first):

   ```agsscript
   #define VERSION "0.1.0"       // becomes the dashboard's version/tester id
   #define TELEMETRY_ENABLED     // master switch for the module
   #define TELEMETRY_REMOTE      // realtime streaming via the plugin
   ```

4. **Configure the server** — in `TelemetryConfig_Init()` at the bottom of
   `Telemetry.asc`:

   ```agsscript
   Telemetry_ServerUrl = "https://your-dashboard.example.com";
   Telemetry_ApiKey    = "";   // dashboard's TELEMETRY_API_KEY, if set
   ```

5. **Wire the module into your global script**:

   ```agsscript
   function game_start() {
     TelemetryConfig_Init();
     Telemetry_StartSession();
   }

   function repeatedly_execute_always() {
     Telemetry_Tick();                      // idle/active tracking + stats feed
   }

   // in on_key_press() and on_mouse_click():
   Telemetry_UserInput();

   function on_event(EventType event, int data) {
     if (event == eEventEnterRoomBeforeFadein) Telemetry_LogRoomEnter(data, true);
     else if (event == eEventEnterRoomAfterFadein) Telemetry_LogRoomEnter(data, false);
     else if (event == eEventLeaveRoom) Telemetry_LogRoomLeave(data, false);
     else if (event == eEventLeaveRoomAfterFadeout) Telemetry_LogRoomLeave(data, true);
     else if (event == eEventGameSaved) Telemetry_LogGameSaved(data);
     else if (event == eEventRestoreGame) Telemetry_LogGameRestored(data);
   }
   ```

   Optional extras (see `Telemetry.ash` for the full API): call
   `Telemetry_LogRoomInteraction` / `Telemetry_LogInventoryUseAt` /
   `Telemetry_LogInventoryUseInv` from your click handlers,
   `Telemetry_LogUnhandled` from `unhandled_event`, and
   `Telemetry_LogMilestone` / `Telemetry_LogEvent` wherever you like.

6. **Run a dashboard** — an
   [ags-telemetry-dashboard](https://github.com/pointandorclick/ags-telementry-dashboard)
   instance with `TELEMETRY_AUTO_REGISTER_VERSIONS=true` set (without it, the
   server rejects sessions from versions it doesn't know: the game runs fine
   but nothing shows on the Live page — events spool locally until the server
   accepts them).

7. **Ship the plugin with the game** — the editor bundles enabled plugins into
   Windows builds; make sure `agstelemetry.dll` sits next to the game exe in
   `Compiled/Windows/`. For macOS ports, place `libagstelemetry.dylib` next to
   the engine binary inside the app bundle. (`scripts/install-sierra-quest.sh`
   is an example installer that does all of this for one specific project.)

Everything degrades safely: with `TELEMETRY_REMOTE` off the game compiles and
runs with local-file logging only, and with the server unreachable the game is
unaffected while events spool to disk.

## How it works

```
AGS script (Telemetry.asc module)          agstelemetry plugin (native)
  _Telemetry_WriteLine(line) ───────────►  AgsTelemetry_Send(line)   [returns instantly]
      │ also appends to the local               │ in-memory queue
      ▼ telemetry.log                           ▼ background worker thread
                                           batches → HTTP POST /api/collect/events
                                           offline → spool to disk, re-send next launch
```

- The AGS **script module is the capture layer** — the plugin is a thin async
  transport. All script calls are non-blocking; the game never waits on
  network or disk I/O from the plugin.
- **Session lifecycle**: `AgsTelemetry_Init` POSTs `/api/collect/session`
  (retrying with backoff until connected), events flush every ~3 s or 20
  events over a persistent keep-alive connection, and the session is PATCHed
  closed when the game ends.
- **Offline/failure**: events queue in memory (cap 5000, overflow to disk).
  Anything undelivered at exit is spooled to `<spoolDir>/pending-*.log` and
  re-sent on the next launch — complete offline sessions via
  `POST /api/collect/import`, partial remainders via `/api/collect/events`.
  The local `telemetry.log` remains the ground-truth backup.
- **Auth**: optional `Authorization: Bearer <TELEMETRY_API_KEY>`. After 3
  hard rejections (bad key / unregistered version) the plugin stops calling
  the server for the rest of the session and spools locally instead.

## Script API (plugin)

The capture module calls these for you; you only need them directly if you're
integrating without the module. The editor injects the imports automatically
when the plugin is enabled (see `ags-script/AgsTelemetry.ash` for reference):

```agsscript
import void AgsTelemetry_Init(String serverUrl, String apiKey, String fullVersion,
                              String platform, String runtimeInfo, String spoolDir);
import void AgsTelemetry_Send(String rawLine);   // "YYYY-MM-DD HH:MM:SS|type|k=v|k=v"
import void AgsTelemetry_UpdateStats(int sessionSeconds, int activeSeconds, int idleSeconds);
import void AgsTelemetry_EndSession(int sessionSeconds, int activeSeconds, int idleSeconds);
import int  AgsTelemetry_GetStatus();            // 0 off, 1 connecting, 2 connected, 3 rejected
```

**Session closing:** AGS never calls a script function when the game quits, so
don't rely on `AgsTelemetry_EndSession` alone. Call `AgsTelemetry_UpdateStats`
about once a second (the module's `Telemetry_Tick` does this) — the plugin
then closes the session itself, with a final flush and the latest stats, when
the engine shuts down. Force-kills and crashes are covered server-side: the
dashboard auto-closes sessions that receive no events for 10 minutes.

`spoolDir` accepts AGS script paths (e.g. `"$SAVEGAMEDIR$/telemetry"`); the
plugin resolves them through the engine (interface v27+).

## Building

| Target | Command | Output | HTTP stack |
|---|---|---|---|
| macOS (universal) | `scripts/build-mac.sh` | `build-mac/libagstelemetry.dylib` | system libcurl |
| Windows 32-bit | `scripts/build-windows.sh` | `build-win32/agstelemetry.dll` | WinHTTP |
| Linux x86-64 | `cmake -B build-linux && cmake --build build-linux` (on Linux/Docker) | `libagstelemetry.so` | libcurl (`libcurl4-openssl-dev`) |

Windows cross-build needs `brew install mingw-w64` (macOS) or the mingw-w64
packages on Linux. The DLL is 32-bit (PE32) because the AGS 3.6.x editor and
default engine are 32-bit; it depends only on `kernel32`, `winhttp` and the
UCRT (present on Windows 10+).

`-DBUILD_TEST_DRIVER=ON` also builds `telemetry-driver`, a CLI that simulates
a full game session against a dashboard:

```
./build-mac/telemetry-driver http://localhost:3000 <apiKey> 0.9.0-dev-me 25 /tmp/spool
```

## Dashboard requirements

Requires [ags-telemetry-dashboard](https://github.com/pointandorclick/ags-telementry-dashboard)
with:

- `POST /api/collect/events` accepting `{"sessionId": N, "rawLines": [...]}`.
- `POST /api/collect/import` (text/plain log content) for spooled offline
  sessions. Returns 422 when the version is unregistered — the plugin keeps
  the file and retries next launch; 400 means "bad content, discard".
- `TELEMETRY_AUTO_REGISTER_VERSIONS=true` to auto-create unknown
  `(version, channel)` pairs.

The dashboard's `/live` page polls every 5 s, so sessions appear within
seconds of launch with no push infrastructure.

## Platform notes & limitations

- **Threading**: the worker thread never touches `IAGSEngine`; script-facing
  calls only mutate the queue under a mutex. Engine shutdown gives the worker
  ~2.5 s to flush, then spools what's left — worst case data arrives on the
  next launch, never lost (and the local log always has everything).
- **macOS**: the engine must load `libagstelemetry.dylib` (place next to the
  engine binary in the app bundle). If your mac engine build doesn't dlopen
  plugins, compile the plugin in as a builtin
  (`pl_register_builtin_plugin`) — sources are engine-agnostic C++17.
- **Web (Emscripten)**: AGS's web port doesn't support plugins; keep
  `TELEMETRY_REMOTE` off for web builds (local-file logging still works).
- **Android**: possible later — build per-ABI `.so`s; not wired up yet.
