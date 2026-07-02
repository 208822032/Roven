#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/tests/bin/vector_core_hnsw_v2_search_recall_test"

if [[ ! -x "$BIN" ]]; then
  printf '[INFO][build] building hnsw_v2 search recall test\n'
  make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 "$BIN"
fi

"$BIN"
