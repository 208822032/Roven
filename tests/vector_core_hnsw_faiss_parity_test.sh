#!/usr/bin/env bash
set -euo pipefail

BIN="./build/tests/bin/vector_core_hnsw_faiss_parity_test"

if [[ ! -x "$BIN" ]]; then
	printf '[INFO][build] building HNSW-P1b FAISS parity test\n'
	make ENABLE_HNSWLIB="${ENABLE_HNSWLIB:-1}" ENABLE_FAISS=1 "$BIN" -j"$(nproc)"
fi

: "${HNSW_P1B_MODE:=quick}"

HNSW_P1B_MODE="$HNSW_P1B_MODE" "$BIN"
