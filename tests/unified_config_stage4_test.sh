#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/kvlite-unified-config-stage4-build"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

for capability in 0 1; do
  cc -std=c11 -Wall -Wextra -Werror -DKVS_ENABLE_EBPF="$capability" \
    -I"$ROOT_DIR/include" -I"$ROOT_DIR/src" \
    "$ROOT_DIR/tests/unified_config_stage4_test.c" \
    "$ROOT_DIR/src/core/kvs_config.c" \
    -o "$BUILD_DIR/unified_config_stage4_test_$capability"
  "$BUILD_DIR/unified_config_stage4_test_$capability"
done

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -DKVS_ENABLE_EBPF=1 \
  -I"$ROOT_DIR/include" -I"$ROOT_DIR/src" \
  "$ROOT_DIR/tests/unified_config_stage4_test.c" \
  "$ROOT_DIR/src/core/kvs_config.c" \
  -o "$BUILD_DIR/unified_config_stage4_test_sanitize"

ASAN_OPTIONS=detect_leaks=1 "$BUILD_DIR/unified_config_stage4_test_sanitize"
