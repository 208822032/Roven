#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/kvlite-unified-config-stage3-build"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

cc -std=c11 -Wall -Wextra -Werror -I"$ROOT_DIR/include" -I"$ROOT_DIR/src" \
  "$ROOT_DIR/tests/unified_config_stage3_test.c" \
  "$ROOT_DIR/src/core/kvs_config.c" \
  -o "$BUILD_DIR/unified_config_stage3_test"

"$BUILD_DIR/unified_config_stage3_test"

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I"$ROOT_DIR/include" -I"$ROOT_DIR/src" \
  "$ROOT_DIR/tests/unified_config_stage3_test.c" \
  "$ROOT_DIR/src/core/kvs_config.c" \
  -o "$BUILD_DIR/unified_config_stage3_test_sanitize"

ASAN_OPTIONS=detect_leaks=1 "$BUILD_DIR/unified_config_stage3_test_sanitize"
