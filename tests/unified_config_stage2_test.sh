#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d /tmp/kvlite-unified-config-stage2-build-XXXXXX)"
trap 'rm -rf "$BUILD_DIR"' EXIT

cc \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -fsanitize=address,undefined \
  -fno-omit-frame-pointer \
  -I"$ROOT_DIR/include" \
  -I"$ROOT_DIR/src" \
  "$ROOT_DIR/tests/unified_config_stage2_test.c" \
  "$ROOT_DIR/src/core/kvs_config.c" \
  -lm \
  -o "$BUILD_DIR/unified_config_stage2_test"

ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  "$BUILD_DIR/unified_config_stage2_test"

python3 "$ROOT_DIR/tests/unified_config_stage2_integration_test.py" "$ROOT_DIR"
