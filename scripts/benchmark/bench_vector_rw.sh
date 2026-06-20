#!/usr/bin/env bash
# bench_vector_rw.sh — KVLite V-family benchmark wrapper
#
# Wraps tests/vector_benchmark for latency or throughput mode.
# Write-once-read-many: write COUNT records, then reuse for VGET/VSEARCH/VDEL.
#
# Usage:
#   BENCH_MODE=latency ./scripts/benchmark/bench_vector_rw.sh
#   BENCH_MODE=throughput ./scripts/benchmark/bench_vector_rw.sh
#   BENCH_MODE=latency COUNT=1000 SEARCH_COUNT=200 ./scripts/benchmark/bench_vector_rw.sh
#
# Output:
#   results/vector_rw_raw.csv
#   results/vector_rw_summary.md
#   logs/vector_rw/

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

# ---- configurable env vars ----
KV_BIN="${KV_BIN:-./kvstore}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-9300}"
CONFIG="${CONFIG:-config/master.conf}"

BENCH_MODE="${BENCH_MODE:-latency}"
COUNT="${COUNT:-100000}"
SEARCH_COUNT="${SEARCH_COUNT:-10000}"
PREPARE_COUNT="${PREPARE_COUNT:-100000}"
SCOPE="${SCOPE:-bench_scope}"
PREFIX="${PREFIX:-rw_data}"
TOPK="${TOPK:-10}"
THRESHOLD="${THRESHOLD:-0.0}"
TEXT_MODE="${TEXT_MODE:-synthetic}"

# Fixed server configuration
INDEX_TYPE="${INDEX_TYPE:-hnsw}"
METRIC="${METRIC:-ip}"
DISTANCE_BACKEND="${DISTANCE_BACKEND:-auto}"
DIM="${DIM:-64}"

# latency defaults
LATENCY_CLIENTS="${LATENCY_CLIENTS:-1}"
LATENCY_PIPELINE="${LATENCY_PIPELINE:-1}"

# throughput defaults
VSET_CLIENTS="${VSET_CLIENTS:-50}"
VSET_PIPELINE="${VSET_PIPELINE:-1}"
VGET_CLIENTS="${VGET_CLIENTS:-100}"
VGET_PIPELINE="${VGET_PIPELINE:-16}"
VSEARCH_CLIENTS="${VSEARCH_CLIENTS:-50}"
VSEARCH_PIPELINE="${VSEARCH_PIPELINE:-4}"
VDEL_CLIENTS="${VDEL_CLIENTS:-100}"
VDEL_PIPELINE="${VDEL_PIPELINE:-16}"

RESULT_DIR="${RESULT_DIR:-results}"
VECTOR_RW_RAW_CSV="$RESULT_DIR/vector_rw_raw.csv"
VECTOR_RW_SUMMARY_MD="$RESULT_DIR/vector_rw_summary.md"
LOG_DIR="${LOG_DIR:-logs/vector_rw}"

BENCH_BIN="${BENCH_BIN:-tests/vector_benchmark}"

RUN_ID="$(date +%Y%m%d_%H%M%S)"

# ---- helpers ----
log()  { echo "[$(date +%H:%M:%S)] $*"; }
warn() { echo "[$(date +%H:%M:%S)] WARN: $*" >&2; }

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "[ERROR] Missing dependency: $1" >&2
    return 1
  fi
}

mkdir -p "$LOG_DIR" "$RESULT_DIR" data

# ---- CSV management ----
CSV_HEADER="timestamp,run_id,mode,op,requests,clients,pipeline,count,topk,qps,avg_ms,p50_ms,p95_ms,p99_ms,min_ms,max_ms,elapsed_ms,success,failed,errors,notes"

init_csv() {
  if [ ! -f "$VECTOR_RW_RAW_CSV" ]; then
    echo "$CSV_HEADER" > "$VECTOR_RW_RAW_CSV"
  fi
}

csv_escape() {
  local s="${1:-}"
  s="${s//\"/\"\"}"
  printf '"%s"' "$s"
}

append_row() {
  local ts="$1" rid="$2" mode="$3" op="$4" req="$5" cli="$6" pl="$7"
  local cnt="$8" tk="$9" qps="${10}" avg="${11}" p50="${12}" p95="${13}" p99="${14}"
  local min_ms="${15}" max_ms="${16}" ems="${17}" suc="${18}" fail="${19}" errs="${20}" notes="${21}"

  printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
    "$ts" "$rid" "$mode" "$op" "$req" "$cli" "$pl" \
    "$cnt" "$tk" \
    "$qps" "$avg" "$p50" "$p95" "$p99" \
    "$min_ms" "$max_ms" \
    "$ems" "$suc" "$fail" "$errs" "$(csv_escape "$notes")" >> "$VECTOR_RW_RAW_CSV"
}

# ---- kvstore lifecycle ----
kvstore_pid=""

start_kvstore() {
  log "Starting kvstore: idx=$INDEX_TYPE metric=$METRIC dist=$DISTANCE_BACKEND"

  $KV_BIN --config "$CONFIG" \
    --vector_index_type "$INDEX_TYPE" \
    --vector_metric "$METRIC" \
    --vector_distance_backend "$DISTANCE_BACKEND" \
    > "$LOG_DIR/${RUN_ID}_kvstore.log" 2>&1 &

  kvstore_pid=$!
  wait_for_port "$HOST" "$PORT" 30
}

stop_kvstore() {
  if [ -n "${kvstore_pid:-}" ] && kill -0 "$kvstore_pid" 2>/dev/null; then
    log "Stopping kvstore pid=$kvstore_pid"
    kill "$kvstore_pid" 2>/dev/null || true
    wait "$kvstore_pid" 2>/dev/null || true
  fi
  kvstore_pid=""
  sleep 0.5
}

cleanup() {
  stop_kvstore
}
trap cleanup EXIT

wait_for_port() {
  local host="$1" port="$2" timeout="${3:-30}"
  local deadline=$(($(date +%s) + timeout))
  while ! nc -z "$host" "$port" 2>/dev/null; do
    if [ "$(date +%s)" -gt "$deadline" ]; then
      log "ERROR: timeout waiting for $host:$port"
      return 1
    fi
    sleep 0.5
  done
}

# ---- run benchmark and capture output ----
run_bench() {
  local mode="$1" op="$2" requests="$3" clients="$4" pipeline="$5"
  local prepare="${6:-0}"
  local logname="${7:-${op}}"

  local logf="$LOG_DIR/${RUN_ID}_${logname}.log"

  local cmd=(
    "$BENCH_BIN"
    --mode "$mode"
    -h "$HOST"
    -p "$PORT"
    -t "$op"
    -n "$requests"
    -c "$clients"
    -P "$pipeline"
    --scope "$SCOPE"
    --prefix "$PREFIX"
    --topk "$TOPK"
    --threshold "$THRESHOLD"
    --text-mode "$TEXT_MODE"
  )

  if [ "$prepare" -gt 0 ]; then
    cmd+=(--prepare "$prepare")
  fi

  "${cmd[@]}" > "$logf" 2>&1
  cat "$logf"
}

# ---- parse field from key=value output ----
parse_field() {
  local key="$1" output="$2"
  local val
  val="$(echo "$output" | grep "^${key}=" | head -1 | cut -d= -f2-)" || true
  echo "$val"
}

# ==================== Main Benchmark ====================
main() {
  need_cmd nc

  if [ ! -x "$BENCH_BIN" ]; then
    log "ERROR: $BENCH_BIN not found or not executable. Run 'make' first."
    return 1
  fi

  init_csv

  log "========== KVLite Vector RW Benchmark =========="
  log "Run ID: $RUN_ID  Mode: $BENCH_MODE"
  log "Target: $HOST:$PORT"
  log "Config: idx=$INDEX_TYPE metric=$METRIC dist=$DISTANCE_BACKEND dim=$DIM"
  log "Params: COUNT=$COUNT SEARCH_COUNT=$SEARCH_COUNT TOPK=$TOPK"
  log "Scope: $SCOPE  Prefix: $PREFIX  TEXT_MODE=$TEXT_MODE THRESHOLD=$THRESHOLD"
  log "Logs: $LOG_DIR"

  start_kvstore

  if [ "$BENCH_MODE" = "latency" ]; then
    local VSET_C=$LATENCY_CLIENTS VSET_P=$LATENCY_PIPELINE
    local VGET_C=$LATENCY_CLIENTS VGET_P=$LATENCY_PIPELINE
    local VSEARCH_C=$LATENCY_CLIENTS VSEARCH_P=$LATENCY_PIPELINE
    local VDEL_C=$LATENCY_CLIENTS VDEL_P=$LATENCY_PIPELINE
  else
    local VSET_C=$VSET_CLIENTS VSET_P=$VSET_PIPELINE
    local VGET_C=$VGET_CLIENTS VGET_P=$VGET_PIPELINE
    local VSEARCH_C=$VSEARCH_CLIENTS VSEARCH_P=$VSEARCH_PIPELINE
    local VDEL_C=$VDEL_CLIENTS VDEL_P=$VDEL_PIPELINE
  fi

  # ---- 1) VSET: write once (with prepare) ----
  log "=== VSET: write $COUNT records ==="
  local ts; ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  local out; out=$(run_bench "$BENCH_MODE" "vset" "$COUNT" "$VSET_C" "$VSET_P" "$PREPARE_COUNT" "vset")
  local su fai err ems qps avg p50 p95 p99 min_ms max_ms
  su="$(parse_field "success" "$out")"
  fai="$(parse_field "failed" "$out")"
  err="$(parse_field "errors" "$out")"
  ems="$(parse_field "elapsed_ms" "$out")"
  qps="$(parse_field "qps" "$out")"
  avg="$(parse_field "avg_ms" "$out")"
  p50="$(parse_field "p50_ms" "$out")"
  p95="$(parse_field "p95_ms" "$out")"
  p99="$(parse_field "p99_ms" "$out")"
  min_ms="$(parse_field "min_ms" "$out")"
  max_ms="$(parse_field "max_ms" "$out")"
  append_row "$ts" "$RUN_ID" "$BENCH_MODE" "VSET" "$COUNT" "$VSET_C" "$VSET_P" \
    "$COUNT" "$TOPK" \
    "$qps" "$avg" "$p50" "$p95" "$p99" "$min_ms" "$max_ms" \
    "$ems" "$su" "$fai" "$err" ""

  # ---- 2) VGET: read same data (no prepare) ----
  log "=== VGET: read $COUNT records ==="
  ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  out=$(run_bench "$BENCH_MODE" "vget" "$COUNT" "$VGET_C" "$VGET_P" 0 "vget")
  su="$(parse_field "success" "$out")"
  fai="$(parse_field "failed" "$out")"
  err="$(parse_field "errors" "$out")"
  ems="$(parse_field "elapsed_ms" "$out")"
  qps="$(parse_field "qps" "$out")"
  avg="$(parse_field "avg_ms" "$out")"
  p50="$(parse_field "p50_ms" "$out")"
  p95="$(parse_field "p95_ms" "$out")"
  p99="$(parse_field "p99_ms" "$out")"
  min_ms="$(parse_field "min_ms" "$out")"
  max_ms="$(parse_field "max_ms" "$out")"
  append_row "$ts" "$RUN_ID" "$BENCH_MODE" "VGET" "$COUNT" "$VGET_C" "$VGET_P" \
    "$COUNT" "$TOPK" \
    "$qps" "$avg" "$p50" "$p95" "$p99" "$min_ms" "$max_ms" \
    "$ems" "$su" "$fai" "$err" ""

  # ---- 3) VSEARCH: search same data (no prepare) ----
  log "=== VSEARCH: search $SEARCH_COUNT queries ==="
  ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  out=$(run_bench "$BENCH_MODE" "vsearch" "$SEARCH_COUNT" "$VSEARCH_C" "$VSEARCH_P" 0 "vsearch")
  su="$(parse_field "success" "$out")"
  fai="$(parse_field "failed" "$out")"
  err="$(parse_field "errors" "$out")"
  ems="$(parse_field "elapsed_ms" "$out")"
  qps="$(parse_field "qps" "$out")"
  avg="$(parse_field "avg_ms" "$out")"
  p50="$(parse_field "p50_ms" "$out")"
  p95="$(parse_field "p95_ms" "$out")"
  p99="$(parse_field "p99_ms" "$out")"
  min_ms="$(parse_field "min_ms" "$out")"
  max_ms="$(parse_field "max_ms" "$out")"
  append_row "$ts" "$RUN_ID" "$BENCH_MODE" "VSEARCH" "$SEARCH_COUNT" "$VSEARCH_C" "$VSEARCH_P" \
    "$SEARCH_COUNT" "$TOPK" \
    "$qps" "$avg" "$p50" "$p95" "$p99" "$min_ms" "$max_ms" \
    "$ems" "$su" "$fai" "$err" ""

  # ---- 4) VDEL: delete same data (no prepare) ----
  log "=== VDEL: delete $COUNT records ==="
  ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  out=$(run_bench "$BENCH_MODE" "vdel" "$COUNT" "$VDEL_C" "$VDEL_P" 0 "vdel")
  su="$(parse_field "success" "$out")"
  fai="$(parse_field "failed" "$out")"
  err="$(parse_field "errors" "$out")"
  ems="$(parse_field "elapsed_ms" "$out")"
  qps="$(parse_field "qps" "$out")"
  avg="$(parse_field "avg_ms" "$out")"
  p50="$(parse_field "p50_ms" "$out")"
  p95="$(parse_field "p95_ms" "$out")"
  p99="$(parse_field "p99_ms" "$out")"
  min_ms="$(parse_field "min_ms" "$out")"
  max_ms="$(parse_field "max_ms" "$out")"
  append_row "$ts" "$RUN_ID" "$BENCH_MODE" "VDEL" "$COUNT" "$VGET_C" "$VGET_P" \
    "$COUNT" "$TOPK" \
    "$qps" "$avg" "$p50" "$p95" "$p99" "$min_ms" "$max_ms" \
    "$ems" "$su" "$fai" "$err" ""

  stop_kvstore

  # ---- Generate Summary ----
  generate_summary

  log "========== DONE =========="
  log "Raw CSV: $VECTOR_RW_RAW_CSV"
  log "Summary: $VECTOR_RW_SUMMARY_MD"
  log "Logs:    $LOG_DIR"
}

# ==================== Summary Generator ====================
generate_summary() {
  log "Generating vector_rw_summary.md ..."

  python3 - "$VECTOR_RW_RAW_CSV" "$VECTOR_RW_SUMMARY_MD" "$INDEX_TYPE" "$METRIC" "$DISTANCE_BACKEND" "$DIM" <<'PY'
import csv
import sys
import os
from datetime import datetime
from collections import OrderedDict

raw_csv, summary_md, idx, metric, dist, dim = (
    sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4],
    sys.argv[5], sys.argv[6]
)

if not os.path.exists(raw_csv):
    print(f"ERROR: {raw_csv} not found", file=sys.stderr)
    sys.exit(1)

all_rows = []
with open(raw_csv, newline='', encoding='utf-8') as f:
    for r in csv.DictReader(f):
        all_rows.append(r)

if not all_rows:
    print("WARN: CSV is empty, skipping summary", file=sys.stderr)
    sys.exit(0)

# Collect all unique run_ids and modes present in CSV
run_ids = list(OrderedDict.fromkeys(r.get('run_id', '') for r in all_rows))
modes_seen = list(OrderedDict.fromkeys(r.get('mode', '') for r in all_rows))

md = []
md.append('# KVLite V 族读写压测报告')
md.append('')
md.append(f'生成时间: {datetime.now().strftime("%Y-%m-%d %H:%M:%S")}')
md.append(f'Run IDs: {", ".join(run_ids)}')
md.append(f'Modes: {", ".join(modes_seen)}')
md.append('')
md.append(f'> 固定配置: index_type={idx}  metric={metric}  distance_backend={dist}  dim={dim}')
md.append('')

op_order = ['VSET', 'VGET', 'VSEARCH', 'VDEL']

for mode in ['latency', 'throughput']:
    mode_rows = [r for r in all_rows if r.get('mode') == mode]
    if not mode_rows:
        continue

    md.append(f'## {mode.capitalize()} 模式')
    md.append('')

    if mode == 'latency':
        md.append('> 单连接串行请求，pipeline=1，核心看 **avg/p95/p99**，QPS 仅供参考。')
    else:
        md.append('> 多连接 + pipeline 并发压测，核心看 **QPS**，同时观察 p95/p99/errors。')
    md.append('')

    md.append('| op | requests | clients | pipeline | qps | avg_ms | p50_ms | p95_ms | p99_ms | min_ms | max_ms | elapsed_ms | success | failed | errors |')
    md.append('|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|')

    for op_name in op_order:
        r = next((r for r in mode_rows if r.get('op') == op_name), None)
        if r is None:
            continue
        md.append(
            f"| {op_name} | {r.get('requests','')} | {r.get('clients','')} | "
            f"{r.get('pipeline','')} | {r.get('qps','')} | "
            f"{r.get('avg_ms','')} | {r.get('p50_ms','')} | {r.get('p95_ms','')} | "
            f"{r.get('p99_ms','')} | {r.get('min_ms','')} | {r.get('max_ms','')} | "
            f"{r.get('elapsed_ms','')} | {r.get('success','')} | {r.get('failed','')} | "
            f"{r.get('errors','')} |"
        )
    md.append('')

# Cross-mode comparison for VSEARCH
lat_vs = next((r for r in all_rows if r.get('mode') == 'latency' and r.get('op') == 'VSEARCH'), None)
thr_vs = next((r for r in all_rows if r.get('mode') == 'throughput' and r.get('op') == 'VSEARCH'), None)

if lat_vs and thr_vs:
    md.append('## VSEARCH 双模式对比')
    md.append('')
    md.append('| metric | latency | throughput |')
    md.append('|---|---:|:---:|')
    md.append(f"| clients | {lat_vs.get('clients','')} | {thr_vs.get('clients','')} |")
    md.append(f"| pipeline | {lat_vs.get('pipeline','')} | {thr_vs.get('pipeline','')} |")
    md.append(f"| qps | {lat_vs.get('qps','')} | {thr_vs.get('qps','')} |")
    md.append(f"| avg_ms | {lat_vs.get('avg_ms','')} | {thr_vs.get('avg_ms','')} |")
    md.append(f"| p50_ms | {lat_vs.get('p50_ms','')} | {thr_vs.get('p50_ms','')} |")
    md.append(f"| p95_ms | {lat_vs.get('p95_ms','')} | {thr_vs.get('p95_ms','')} |")
    md.append(f"| p99_ms | {lat_vs.get('p99_ms','')} | {thr_vs.get('p99_ms','')} |")
    md.append('')

with open(summary_md, 'w', encoding='utf-8') as f:
    f.write('\n'.join(md) + '\n')

print(f"Summary written to {summary_md} (rows={len(all_rows)} modes={modes_seen})")
PY

  log "Summary generated: $VECTOR_RW_SUMMARY_MD"
}

main
