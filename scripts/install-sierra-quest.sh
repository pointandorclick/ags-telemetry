#!/bin/bash
# Installs the built plugin where the Sierra Quest toolchain finds it:
#  - agstelemetry.dll  -> AGS editor dir in the CrossOver bottle (editor loads
#    ags*.dll from its own directory and bundles it into compiled games)
#  - agstelemetry.dll  -> the game's Compiled/Windows/ (for already-built output)
#  - libagstelemetry.dylib -> the mac app bundle, next to the engine binary
set -e
cd "$(dirname "$0")/.."

AGS_EDITOR_DIR="/Users/craig/Library/Application Support/CrossOver/Bottles/AGS/drive_c/Program Files/Adventure Game Studio"
GAME_DIR="/Users/craig/Documents/Sierra Quest"
MAC_APP_MACOS="/Users/craig/Code/3rd-party/ags-mac-build/build/Sierra Quest.app/Contents/MacOS"

if [ -f build-win32/agstelemetry.dll ]; then
    cp build-win32/agstelemetry.dll "${AGS_EDITOR_DIR}/"
    echo "Installed agstelemetry.dll -> AGS editor (CrossOver bottle)"
    if [ -d "${GAME_DIR}/Compiled/Windows" ]; then
        cp build-win32/agstelemetry.dll "${GAME_DIR}/Compiled/Windows/"
        echo "Installed agstelemetry.dll -> ${GAME_DIR}/Compiled/Windows/"
    fi
else
    echo "SKIP: build-win32/agstelemetry.dll not found (run scripts/build-windows.sh)"
fi

if [ -f build-mac/libagstelemetry.dylib ]; then
    cp build-mac/libagstelemetry.dylib "${MAC_APP_MACOS}/"
    echo "Installed libagstelemetry.dylib -> ${MAC_APP_MACOS}/"
else
    echo "SKIP: build-mac/libagstelemetry.dylib not found (run scripts/build-mac.sh)"
fi

echo
echo "Next steps (one-time, in the AGS editor under CrossOver):"
echo "  1. Open Sierra Quest, expand 'Plugins' in the project tree,"
echo "     right-click 'AGS Telemetry' and choose 'Use this plugin'."
echo "  2. In Config.ash uncomment: #define TELEMETRY_REMOTE"
echo "  3. In Telemetry.asc TelemetryConfig_Init(), set Telemetry_ServerUrl"
echo "     (and Telemetry_ApiKey if the dashboard sets TELEMETRY_API_KEY)."
echo "  4. Rebuild the game."
