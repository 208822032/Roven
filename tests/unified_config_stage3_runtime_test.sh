#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

make -s clean
make -s -j"$(nproc)"

cc -std=c11 -Wall -Wextra -Werror -I./include -I./src \
  tests/unified_config_stage3_runtime_test.c \
  -Lbuild/lib -lkvstore -Lthird_party/NtyCo -lntyco \
  -lpthread -luring -ldl -lm third_party/jemalloc/lib/libjemalloc.a \
  -o /tmp/kvlite-unified-config-stage3-runtime-test

/tmp/kvlite-unified-config-stage3-runtime-test
