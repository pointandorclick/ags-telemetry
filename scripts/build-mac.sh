#!/bin/bash
# Builds the universal macOS dylib (and the test driver).
set -e
cd "$(dirname "$0")/.."

cmake -B build-mac -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
      -DBUILD_TEST_DRIVER=ON
cmake --build build-mac

echo
echo "Built: build-mac/libagstelemetry.dylib"
lipo -info build-mac/libagstelemetry.dylib
