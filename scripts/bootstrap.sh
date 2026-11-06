#!/usr/bin/env bash
set -euo pipefail

# All dependencies are vendored in third_party/ — no network access needed.
echo "[afteraction] Configuring CMake..."
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo "[afteraction] Building..."
cmake --build build --parallel "$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"

echo "[afteraction] Done. Binary: build/afteraction"
