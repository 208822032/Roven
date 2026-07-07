#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"

if [[ ! -x "$KV_BIN" ]]; then
  printf '[INFO][build] building kvstore\n'
  make -j2 kvstore
fi

N="${N:-10000}"
DIM="${DIM:-64}"
TOPK="${TOPK:-10}"
QUERIES="${QUERIES:-100}"
BACKEND_ARGS=()

if [[ -n "${BACKEND:-}" ]]; then
  BACKEND_ARGS+=(--backend "$BACKEND")
fi

python3 scripts/bench_vector_core_compare.py \
  --corrected \
  --kv-bin "$KV_BIN" \
  --n "$N" \
  --dim "$DIM" \
  --topk "$TOPK" \
  --queries "$QUERIES" \
  "${BACKEND_ARGS[@]}"

if [[ "${RUN_100K:-0}" == "1" ]]; then
  timeout "${TIMEOUT_100K_SECONDS:-1800}" \
    python3 scripts/bench_vector_core_compare.py \
      --corrected \
      --kv-bin "$KV_BIN" \
      --n 100000 \
      --dim "$DIM" \
      --topk "$TOPK" \
      --queries "${QUERIES_100K:-100}" \
      "${BACKEND_ARGS[@]}" || {
        status=$?
        if [[ "$status" == "124" ]]; then
          printf '[WARN] 100K benchmark timed out after %s seconds\n' "${TIMEOUT_100K_SECONDS:-1800}" >&2
        else
          exit "$status"
        fi
      }
fi
