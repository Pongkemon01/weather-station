#!/usr/bin/env bash
# Compile and run OTA host-side unit tests via WSL2 gcc.
# Invoke from WSL2: bash scripts/run_native_tests.sh

set -e

cd "$(dirname "$0")/.."
BASE=$(pwd)

UNITY_SRC="$BASE/.pio/libdeps/native_test/Unity/src"
UNITY_CFG="$BASE/.pio/build/native_test/unity_config"

gcc \
    -I"$BASE/Src" \
    -I"$BASE/shared" \
    -I"$UNITY_SRC" \
    -I"$UNITY_CFG" \
    "$BASE/Src/ota_version_parser.c" \
    "$UNITY_SRC/unity.c" \
    "$BASE/test/test_ota_size_guard/test_main.c" \
    -o /tmp/test_ota_size_guard

/tmp/test_ota_size_guard
