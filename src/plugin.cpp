// AGS plugin entry points and script-function glue for the telemetry client.
#define THIS_IS_THE_PLUGIN
#include "agsplugin.h"

#include <cstring>
#include <string>

#include "telemetry_client.h"

namespace {

IAGSEngine* g_engine = nullptr;

// Keep in sync with ags-script/AgsTelemetry.ash (that file is reference/docs;
// this header is what the editor injects at compile time).
const char* kScriptHeader =
    "// AGS Telemetry plugin — realtime telemetry transport (auto-registered)\r\n"
    "import void AgsTelemetry_Init(String serverUrl, String apiKey, String fullVersion, String platform, String runtimeInfo, String spoolDir);\r\n"
    "import void AgsTelemetry_Send(String rawLine);\r\n"
    "import void AgsTelemetry_UpdateStats(int sessionSeconds, int activeSeconds, int idleSeconds);\r\n"
    "import void AgsTelemetry_EndSession(int sessionSeconds, int activeSeconds, int idleSeconds);\r\n"
    "import int AgsTelemetry_GetStatus();\r\n";

std::string SafeStr(const char* s) {
    return s ? std::string(s) : std::string();
}

// Resolves AGS script paths like "$SAVEGAMEDIR$/telemetry" to a real filesystem
// path via the engine (interface v27+). Pre-resolved paths pass through as-is.
std::string ResolveSpoolDir(const std::string& scriptPath) {
    if (scriptPath.empty()) return scriptPath;
    if (scriptPath.find('$') == std::string::npos) return scriptPath;
    if (!g_engine || g_engine->version < 27) return std::string();
    size_t len = g_engine->ResolveFilePath(scriptPath.c_str(), nullptr, 0);
    if (len == 0) return std::string();
    std::string buf(len, '\0');
    g_engine->ResolveFilePath(scriptPath.c_str(), &buf[0], buf.size());
    buf.resize(std::strlen(buf.c_str()));
    return buf;
}

// ---- script-visible functions -------------------------------------------------

void AgsTelemetry_Init(const char* serverUrl, const char* apiKey,
                       const char* fullVersion, const char* platform,
                       const char* runtimeInfo, const char* spoolDir) {
    agstel::Config cfg;
    cfg.baseUrl = SafeStr(serverUrl);
    cfg.apiKey = SafeStr(apiKey);
    cfg.fullVersion = SafeStr(fullVersion);
    cfg.platform = SafeStr(platform);
    cfg.runtimeInfo = SafeStr(runtimeInfo);
    cfg.spoolDir = ResolveSpoolDir(SafeStr(spoolDir));
    cfg.savegameDir = ResolveSpoolDir("$SAVEGAMEDIR$");
    agstel::Init(cfg);
}

void AgsTelemetry_Send(const char* rawLine) {
    if (rawLine) agstel::Send(rawLine);
}

void AgsTelemetry_UpdateStats(int sessionSeconds, int activeSeconds, int idleSeconds) {
    agstel::UpdateStats(sessionSeconds, activeSeconds, idleSeconds);
}

void AgsTelemetry_EndSession(int sessionSeconds, int activeSeconds, int idleSeconds) {
    agstel::EndSession(sessionSeconds, activeSeconds, idleSeconds);
}

int AgsTelemetry_GetStatus() {
    return agstel::GetStatus();
}

}  // namespace

// ---- engine interface -----------------------------------------------------------

DLLEXPORT const char* AGS_GetPluginName(void) {
    return "AGS Telemetry";
}

DLLEXPORT int AGS_PluginV2() {
    return 1;
}

DLLEXPORT void AGS_EngineStartup(IAGSEngine* engine) {
    g_engine = engine;
    engine->RegisterScriptFunction("AgsTelemetry_Init", (void*)AgsTelemetry_Init);
    engine->RegisterScriptFunction("AgsTelemetry_Send", (void*)AgsTelemetry_Send);
    engine->RegisterScriptFunction("AgsTelemetry_UpdateStats", (void*)AgsTelemetry_UpdateStats);
    engine->RegisterScriptFunction("AgsTelemetry_EndSession", (void*)AgsTelemetry_EndSession);
    engine->RegisterScriptFunction("AgsTelemetry_GetStatus", (void*)AgsTelemetry_GetStatus);
}

DLLEXPORT void AGS_EngineShutdown(void) {
    agstel::Shutdown(2500);
    g_engine = nullptr;
}

DLLEXPORT intptr_t AGS_EngineOnEvent(int event, intptr_t data) {
    (void)event;
    (void)data;
    return 0;
}

DLLEXPORT int AGS_EngineDebugHook(const char* scriptName, int lineNum, int reserved) {
    (void)scriptName;
    (void)lineNum;
    (void)reserved;
    return 0;
}

DLLEXPORT void AGS_EngineInitGfx(const char* driverID, void* data) {
    (void)driverID;
    (void)data;
}

// ---- editor interface -------------------------------------------------------------

DLLEXPORT int AGS_EditorStartup(IAGSEditor* editor) {
    editor->RegisterScriptHeader(kScriptHeader);
    return 0;
}

DLLEXPORT void AGS_EditorShutdown(void) {}

DLLEXPORT void AGS_EditorProperties(HWND parent) {
    (void)parent;
}

DLLEXPORT int AGS_EditorSaveGame(char* buffer, int bufsize) {
    (void)buffer;
    (void)bufsize;
    return 0;
}

DLLEXPORT void AGS_EditorLoadGame(char* buffer, int bufsize) {
    (void)buffer;
    (void)bufsize;
}
