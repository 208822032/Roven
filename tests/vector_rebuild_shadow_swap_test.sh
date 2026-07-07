#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/tests/bin/vector_rebuild_shadow_swap_test"

if [[ ! -x "$BIN" ]]; then
  printf '[INFO][build] building rebuild shadow swap test\n'
  make "$BIN"
fi

"$BIN"
