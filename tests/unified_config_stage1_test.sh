#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d /tmp/kvlite-unified-config-stage1-build-XXXXXX)"
trap 'rm -rf "$BUILD_DIR"' EXIT

cc \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -I"$ROOT_DIR/include" \
  -I"$ROOT_DIR/src" \
  "$ROOT_DIR/tests/unified_config_stage1_test.c" \
  "$ROOT_DIR/src/core/kvs_config.c" \
  -lm \
  -o "$BUILD_DIR/unified_config_stage1_test"

cd "$ROOT_DIR"
"$BUILD_DIR/unified_config_stage1_test"
