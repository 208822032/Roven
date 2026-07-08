#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/tests/bin/vector_index_snapshot_cold_start_bench"
MODE="${HNSW_S_BENCH_MODE:-quick}"

if [[ ! -x "$BIN" ]]; then
	printf '[INFO][build] building vector_index_snapshot_cold_start_bench\n'
	make -j"$(nproc)" "$BIN"
fi

if [[ "$MODE" == "quick" ]]; then
	tmp_dir="$(mktemp -d /tmp/kvlite_hnsw_s_bench_XXXXXX)"
	cleanup() {
		rm -rf "$tmp_dir"
	}
	trap cleanup EXIT
	(
		cd "$tmp_dir"
		HNSW_S_BENCH_MODE=quick "$ROOT_DIR/$BIN"
	)
else
	HNSW_S_BENCH_MODE="$MODE" "$BIN"
fi
