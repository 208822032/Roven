#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/tests/bin/vector_rebuild_shadow_replay_test"

if [[ ! -x "$BIN" ]]; then
  printf '[INFO][build] building rebuild shadow replay test\n'
  make ENABLE_HNSWLIB="${ENABLE_HNSWLIB:-1}" ENABLE_FAISS="${ENABLE_FAISS:-1}" "$BIN"
fi

"$BIN"

