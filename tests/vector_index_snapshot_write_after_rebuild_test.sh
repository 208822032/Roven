#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/tests/bin/vector_index_snapshot_write_after_rebuild_test"
if [[ ! -x "$BIN" ]]; then
  printf '[INFO][build] building vector_index_snapshot_write_after_rebuild_test\n'
  make "$BIN" -j"$(nproc)"
fi

"$BIN"
