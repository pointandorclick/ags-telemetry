#!/bin/bash
# Cross-compiles the 32-bit Windows DLL (matches the 32-bit AGS 3.6.x
# editor and engine). Requires: brew install mingw-w64
set -e
cd "$(dirname "$0")/.."

cmake -B build-win32 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-win32

echo
echo "Built: build-win32/agstelemetry.dll"
file build-win32/agstelemetry.dll | grep -q "PE32 executable" || {
    echo "ERROR: expected a PE32 (32-bit) DLL"; exit 1;
}
file build-win32/agstelemetry.dll
