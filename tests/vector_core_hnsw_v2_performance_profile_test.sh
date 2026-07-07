#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/tests/bin/vector_core_hnsw_v2_performance_profile_test"

if [[ ! -x "$BIN" ]]; then
  printf '[INFO][build] building hnsw_v2 performance profile test\n'
  make "$BIN"
fi

"$BIN"
