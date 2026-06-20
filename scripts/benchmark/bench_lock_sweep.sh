#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

RESULT_ROOT="${RESULT_ROOT:-$ROOT_DIR/results/lock_sweep}"
LOG_ROOT="${LOG_ROOT:-$ROOT_DIR/logs/lock_sweep}"
KV_HOST="${KV_HOST:-127.0.0.1}"
KV_PORT="${KV_PORT:-9300}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
N="${N:-100000}"
P="${P:-1}"
VALUE_SIZE="${VALUE_SIZE:-32}"
KEYSPACE="${KEYSPACE:-100000}"
POP_N="${POP_N:-$KEYSPACE}"
C_VALUES="${C_VALUES:-1 5 10 50}"

mkdir -p "$RESULT_ROOT" "$LOG_ROOT"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --result-root DIR   Root directory for per-run results (default: $RESULT_ROOT)
  --log-root DIR      Root directory for per-run logs (default: $LOG_ROOT)
  --c-values LIST     Space-separated concurrency values (default: "$C_VALUES")
  --kv-host HOST      KVStore host (default: $KV_HOST)
  --kv-port PORT      KVStore port (default: $KV_PORT)
  --redis-host HOST   Redis host (default: $REDIS_HOST)
  --redis-port PORT   Redis port (default: $REDIS_PORT)
  --n N               Requests per benchmark phase (default: $N)
  --p P               Pipeline depth (default: $P)
  --value-size N      Value size in bytes (default: $VALUE_SIZE)
  --keyspace N        Keyspace for random keys (default: $KEYSPACE)
  --populate N        Populate requests before reads (default: $POP_N)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --result-root) RESULT_ROOT="$2"; shift 2 ;;
    --log-root) LOG_ROOT="$2"; shift 2 ;;
    --c-values) C_VALUES="$2"; shift 2 ;;
    --kv-host) KV_HOST="$2"; shift 2 ;;
    --kv-port) KV_PORT="$2"; shift 2 ;;
    --redis-host) REDIS_HOST="$2"; shift 2 ;;
    --redis-port) REDIS_PORT="$2"; shift 2 ;;
    --n) N="$2"; shift 2 ;;
    --p) P="$2"; shift 2 ;;
    --value-size) VALUE_SIZE="$2"; shift 2 ;;
    --keyspace) KEYSPACE="$2"; shift 2 ;;
    --populate) POP_N="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "[ERROR] Missing dependency: $1" >&2
    exit 1
  fi
}

need_cmd python3
need_cmd bash

run_id="$(date +%Y%m%d_%H%M%S)"
summary_dir="$RESULT_ROOT/$run_id"
mkdir -p "$summary_dir"

declare -a c_list=()
read -r -a c_list <<< "$C_VALUES"

echo "[INFO] Run ID: $run_id"
echo "[INFO] Concurrency sweep: ${c_list[*]}"
echo "[INFO] Benchmark params: n=$N P=$P value_size=$VALUE_SIZE keyspace=$KEYSPACE populate=$POP_N"

for c in "${c_list[@]}"; do
  run_dir="$summary_dir/c${c}"
  log_dir="$LOG_ROOT/$run_id/c${c}"
  mkdir -p "$run_dir" "$log_dir"

  echo "[INFO] Running bench_all.sh with c=$c"
  C="$c" N="$N" P="$P" VALUE_SIZE="$VALUE_SIZE" KEYSPACE="$KEYSPACE" POP_N="$POP_N" \
    RESULT_DIR="$run_dir" LOG_DIR="$log_dir" \
    bash scripts/benchmark/bench_all.sh --kv-host "$KV_HOST" --kv-port "$KV_PORT" --redis-host "$REDIS_HOST" --redis-port "$REDIS_PORT"
done

python3 - "$summary_dir" <<'PY'
import os
import sys

root = sys.argv[1]
targets = []
for name in sorted(os.listdir(root), key=lambda item: int(item[1:]) if item.startswith('c') and item[1:].isdigit() else 10**9):
    if not name.startswith('c'):
        continue
    c_val = name[1:]
    summary_path = os.path.join(root, name, 'summary.md')
    if os.path.exists(summary_path):
        targets.append((c_val, summary_path))

def parse_summary(path):
    rows = []
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line.startswith('|'):
                continue
            if line.startswith('| ds |') or line.startswith('|---'):
                continue
            parts = [p.strip() for p in line.strip('|').split('|')]
            if len(parts) < 11:
                continue
            ds, op, kv_qps, rd_qps, kv_p50, kv_p95, kv_p99, rd_p50, rd_p95, rd_p99, notes = parts[:11]
            if ds not in {'array', 'hash', 'rbtree', 'skiptable'}:
                continue
            rows.append({
                'ds': ds,
                'op': op,
                'kv_qps': kv_qps,
                'kv_p50': kv_p50,
                'kv_p95': kv_p95,
                'kv_p99': kv_p99,
                'notes': notes,
            })
    return rows

all_rows = []
for c_val, path in targets:
    for row in parse_summary(path):
        row['c'] = c_val
        all_rows.append(row)

out_path = os.path.join(root, 'summary.md')
with open(out_path, 'w', encoding='utf-8') as f:
    f.write('# Lock Sweep Summary\n\n')
    f.write('| c | ds | op | kvstore_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | notes |\n')
    f.write('|---:|---|---|---:|---:|---:|---:|---|\n')
    order = {'array': 0, 'hash': 1, 'rbtree': 2, 'skiptable': 3, 'set': 0, 'get': 1}
    for row in sorted(all_rows, key=lambda r: (int(r['c']), order.get(r['ds'], 9), order.get(r['op'], 9))):
        f.write('| {c} | {ds} | {op} | {kv_qps} | {kv_p50} | {kv_p95} | {kv_p99} | {notes} |\n'.format(**row))

    f.write('\n## Aggregated View\n\n')
    f.write('| c | array qps | hash qps | rbtree qps | skiptable qps |\n')
    f.write('|---:|---:|---:|---:|---:|\n')
    for c_val, _ in sorted(targets, key=lambda item: int(item[0])):
        subset = [r for r in all_rows if r['c'] == c_val and r['op'] == 'get']
        lookup = {(r['ds'], r['op']): r for r in subset}
        # Use GET as the primary lock-sensitivity indicator for the aggregate table.
        vals = []
        for ds in ['array', 'hash', 'rbtree', 'skiptable']:
            row = lookup.get((ds, 'get'))
            vals.append(row['kv_qps'] if row else '')
      f.write('| {c} | {0} | {1} | {2} | {3} |\n'.format(*vals, c=c_val))

print(out_path)
PY

echo "[DONE] Combined summary: $summary_dir/summary.md"
