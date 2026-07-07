#!/usr/bin/env bash
set -euo pipefail

BIN="./build/tests/bin/vector_core_hnsw_v2_parity_baseline_test"

if [[ ! -x "$BIN" ]]; then
	printf '[INFO][build] building hnsw_v2 parity baseline test\n'
	make "$BIN" -j"$(nproc)"
fi

: "${HNSW_P1_MODE:=quick}"

HNSW_P1_MODE="$HNSW_P1_MODE" "$BIN"
