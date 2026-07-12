// Standalone CLI exercising the telemetry client against a running dashboard.
// Usage: telemetry-driver <baseUrl> [apiKey] [fullVersion] [eventCount] [spoolDir] [--no-end]
// --no-end skips the explicit EndSession, simulating a game that quits without
// a script-side shutdown hook — the plugin must auto-close the session.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>

#include "telemetry_client.h"

static std::string Timestamp(int offsetSeconds) {
    std::time_t t = std::time(nullptr) + offsetSeconds;
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <baseUrl> [apiKey] [fullVersion] [eventCount] [spoolDir]\n",
                     argv[0]);
        return 2;
    }
    agstel::Config cfg;
    cfg.baseUrl = argv[1];
    cfg.apiKey = argc > 2 ? argv[2] : "";
    cfg.fullVersion = argc > 3 ? argv[3] : "0.0.1-dev-driver";
    cfg.platform = "driver";
    cfg.runtimeInfo = "telemetry-driver test harness";
    cfg.spoolDir = argc > 5 ? argv[5] : "";
    int count = argc > 4 ? std::atoi(argv[4]) : 30;
    bool noEnd = argc > 6 && std::string(argv[6]) == "--no-end";

    // Mirror what the AGS script module emits at session start.
    agstel::Send(Timestamp(0) + "|session_start|date=" + Timestamp(0).substr(0, 10));
    agstel::Send(Timestamp(0) + "|build|version=" + cfg.fullVersion);
    agstel::Send(Timestamp(0) + "|platform|tag=" + cfg.platform);
    agstel::Send(Timestamp(0) + "|runtime|info=" + cfg.runtimeInfo);

    agstel::Init(cfg);

    for (int i = 0; i < count; ++i) {
        int room = 1 + (i % 5);
        agstel::Send(Timestamp(i) + "|room_enter|room_id=" + std::to_string(room) +
                     "|phase=before_fadein");
        agstel::Send(Timestamp(i) + "|custom|event=driver_tick|data=i" + std::to_string(i));
        agstel::UpdateStats(i + 1, i, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (i % 10 == 0) {
            std::printf("sent %d/%d (status=%d)\n", i, count, agstel::GetStatus());
        }
    }

    agstel::Send(Timestamp(count) + "|milestone|name=driver_done");
    if (!noEnd) {
        agstel::Send(Timestamp(count) + "|session_end|session_seconds=" + std::to_string(count) +
                     "|active_seconds=" + std::to_string(count - 2) + "|idle_seconds=2");
        agstel::EndSession(count, count - 2, 2);
    }

    // Give the worker a moment to flush + PATCH, then shut down like the engine would.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    agstel::Shutdown(3000);
    std::printf("done (final status=%d)\n", agstel::GetStatus());
    return 0;
}
