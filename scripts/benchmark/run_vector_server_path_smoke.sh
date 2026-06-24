#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

DATASET_ID="${DATASET_ID:-smoke_N1000_nq100_d64_seed42_ip_norm_top10_scope-empty}"
MANIFEST="${MANIFEST:-results/vector_native_faiss/datasets/${DATASET_ID}/manifest.json}"
OUT_DIR="${OUT_DIR:-results/vector_native_faiss/server_path_smoke}"
RAW_CSV="$OUT_DIR/raw.csv"
SUMMARY_MD="$OUT_DIR/summary.md"
LOG_DIR="$OUT_DIR/logs"
CONF_DIR="$OUT_DIR/configs"
DATA_DIR="$OUT_DIR/data"

PORT="${PORT:-19310}"
SYNC_PORT="${SYNC_PORT:-29310}"
HOST="${HOST:-127.0.0.1}"
SCOPE="${SCOPE:-smoke_scope}"
HNSW_M="${HNSW_M:-32}"
HNSW_EF_CONSTRUCTION="${HNSW_EF_CONSTRUCTION:-256}"
HNSW_EF_SEARCH="${HNSW_EF_SEARCH:-128}"

mkdir -p "$OUT_DIR" "$LOG_DIR" "$CONF_DIR" "$DATA_DIR"

if [ ! -f "$MANIFEST" ]; then
  echo "manifest not found: $MANIFEST" >&2
  exit 1
fi

read_manifest_field() {
  python3 - "$MANIFEST" "$1" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)
value = data.get(sys.argv[2], "")
if isinstance(value, bool):
    print("true" if value else "false")
else:
    print(value)
PY
}

parse_metric() {
  python3 - "$1" "$2" <<'PY'
import sys
path, key = sys.argv[1], sys.argv[2]
value = ""
try:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if "=" not in line:
                continue
            k, v = line.rstrip("\n").split("=", 1)
            if k == key:
                value = v
except FileNotFoundError:
    pass
print(value)
PY
}

csv_escape() {
  python3 - "$1" <<'PY'
import sys
s = sys.argv[1]
print('"' + s.replace('"', '""') + '"')
PY
}

N="$(read_manifest_field N)"
NQ="$(read_manifest_field nq)"
DIM="$(read_manifest_field dim)"
METRIC="$(read_manifest_field metric)"
NORMALIZE="$(read_manifest_field normalize)"
TOPK="$(read_manifest_field topK)"
RUN_ID="server_path_smoke_$(date -u +%Y%m%dT%H%M%SZ)"

hnswlib_available=0
hnswlib_reason="third_party/hnswlib is empty or missing hnswlib/hnswlib.h"
if [ -f "third_party/hnswlib/hnswlib/hnswlib.h" ]; then
  hnswlib_available=1
  hnswlib_reason="third_party/hnswlib/hnswlib/hnswlib.h exists"
fi

faiss_available=0
faiss_reason="third_party/faiss is empty or missing faiss headers/build/faiss/libfaiss.a"
if [ -f "third_party/faiss/faiss/IndexFlat.h" ] && [ -f "third_party/faiss/build/faiss/libfaiss.a" ]; then
  faiss_available=1
  faiss_reason="faiss headers and static library exist"
elif [ -f "third_party/faiss/faiss/IndexFlat.h" ]; then
  faiss_reason="faiss headers exist but third_party/faiss/build/faiss/libfaiss.a is missing"
fi

echo "run_id,dataset_id,layer,backend,operation,N,nq,dim,metric,normalize,topK,M,efConstruction,efSearch,requests,clients,pipeline,elapsed_ms,qps,avg_ms,p50_ms,p95_ms,p99_ms,max_ms,success,failed,errors,timeout,index_add_avg_us,index_search_avg_us,non_index_avg_ms,status,notes" > "$RAW_CSV"

append_row() {
  local backend="$1" operation="$2" requests="$3" clients="$4" pipeline="$5" output_file="$6" status="$7" notes="$8"
  local elapsed qps avg p50 p95 p99 max success failed errors derived_status
  elapsed="$(parse_metric "$output_file" elapsed_ms)"
  qps="$(parse_metric "$output_file" qps)"
  avg="$(parse_metric "$output_file" avg_ms)"
  p50="$(parse_metric "$output_file" p50_ms)"
  p95="$(parse_metric "$output_file" p95_ms)"
  p99="$(parse_metric "$output_file" p99_ms)"
  max="$(parse_metric "$output_file" max_ms)"
  success="$(parse_metric "$output_file" success)"
  failed="$(parse_metric "$output_file" failed)"
  errors="$(parse_metric "$output_file" errors)"
  if [ "${success:-0}" = "$requests" ] && [ "${errors:-0}" = "0" ]; then
    derived_status="$status"
  else
    derived_status="fail"
  fi
  printf '%s,%s,server_path,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,false,,,,%s,%s\n' \
    "$RUN_ID" "$DATASET_ID" "$backend" "$operation" "$N" "$NQ" "$DIM" "$METRIC" "$NORMALIZE" "$TOPK" \
    "$HNSW_M" "$HNSW_EF_CONSTRUCTION" "$HNSW_EF_SEARCH" "$requests" "$clients" "$pipeline" \
    "$elapsed" "$qps" "$avg" "$p50" "$p95" "$p99" "$max" "${success:-0}" "${failed:-0}" "${errors:-0}" \
    "$derived_status" "$(csv_escape "$notes")" >> "$RAW_CSV"
}

append_unavailable() {
  local backend="$1" notes="$2"
  printf '%s,%s,server_path,%s,VSET_VSEARCH,%s,%s,%s,%s,%s,%s,%s,%s,%s,0,1,1,,,,,,,,0,0,0,false,,,,unavailable,%s\n' \
    "$RUN_ID" "$DATASET_ID" "$backend" "$N" "$NQ" "$DIM" "$METRIC" "$NORMALIZE" "$TOPK" \
    "$HNSW_M" "$HNSW_EF_CONSTRUCTION" "$HNSW_EF_SEARCH" "$(csv_escape "$notes")" >> "$RAW_CSV"
}

ensure_binaries() {
  if [ ! -x "./kvstore" ]; then
    make main ENABLE_EBPF=0 ENABLE_ONNX_EMBEDDING=0 ENABLE_HNSWLIB=0 ENABLE_FAISS=0
  fi
  if [ ! -x "build/tests/bin/vector_benchmark" ]; then
    make build/tests/bin/vector_benchmark ENABLE_EBPF=0 ENABLE_ONNX_EMBEDDING=0 ENABLE_HNSWLIB=0 ENABLE_FAISS=0
  fi
}

wait_for_server() {
  local i
  for i in $(seq 1 50); do
    if python3 - "$HOST" "$PORT" <<'PY'
import socket
import sys
host, port = sys.argv[1], int(sys.argv[2])
s = socket.socket()
s.settimeout(0.1)
try:
    s.connect((host, port))
except OSError:
    sys.exit(1)
finally:
    s.close()
PY
    then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

run_native_flat() {
  local backend="native_flat"
  local run_data_dir="$DATA_DIR/${RUN_ID}_${backend}"
  local conf="$CONF_DIR/${backend}.conf"
  local server_log="$LOG_DIR/server_${backend}.log"
  local vset_out="$LOG_DIR/${backend}_vset.out"
  local vsearch_out="$LOG_DIR/${backend}_vsearch.out"
  local pid=""

  mkdir -p "$run_data_dir"
  cat > "$conf" <<EOF
port=$PORT
sync_port=$SYNC_PORT
role=master
enable_aof=no
aof_fsync_mode=no
aof_filename=$run_data_dir/kvlite.aof
rdb_filename=$run_data_dir/kvlite.rdb
dump_filename=$run_data_dir/kvlite.dat
network_mode=reactor
mem_backend=pool
max_value_size=10485760
sync_mode=tcp
log_level=error
log_async=no
embedding_provider=hash
embedding_dim=$DIM
vector_index_type=flat
vector_metric=$METRIC
vector_distance_backend=auto
vector_hnsw_m=$HNSW_M
vector_hnsw_ef_construction=$HNSW_EF_CONSTRUCTION
vector_hnsw_ef_search=$HNSW_EF_SEARCH
vector_score_threshold=0.0
vector_scope_overfetch_factor=1
EOF

  ./kvstore -c "$conf" > "$server_log" 2>&1 &
  pid="$!"
  trap 'if [ -n "${pid:-}" ]; then kill "$pid" >/dev/null 2>&1 || true; wait "$pid" >/dev/null 2>&1 || true; fi' EXIT

  if ! wait_for_server; then
    append_unavailable "$backend" "server failed to listen on $HOST:$PORT; see $server_log"
    return 1
  fi

  build/tests/bin/vector_benchmark \
    --mode latency -h "$HOST" -p "$PORT" -t vset -n 1 -c 1 -P 1 \
    --scope "$SCOPE" --prefix "$DATASET_ID" --prepare "$N" --topk "$TOPK" \
    > "$vset_out" 2>&1
  append_row "$backend" "VSET" "1" "1" "1" "$vset_out" "pass" "prepare=$N plus one VSET through RESP server path"

  build/tests/bin/vector_benchmark \
    --mode latency -h "$HOST" -p "$PORT" -t vsearch -n "$NQ" -c 1 -P 1 \
    --scope "$SCOPE" --prefix "$DATASET_ID" --topk "$TOPK" --threshold "0.0" \
    > "$vsearch_out" 2>&1
  append_row "$backend" "VSEARCH" "$NQ" "1" "1" "$vsearch_out" "pass" "$NQ VSEARCH requests through RESP server path"

  kill "$pid" >/dev/null 2>&1 || true
  wait "$pid" >/dev/null 2>&1 || true
  pid=""
  trap - EXIT
}

ensure_binaries
run_native_flat

if [ "$hnswlib_available" -eq 1 ]; then
  append_unavailable "hnswlib_hnsw" "dependency appears present, but this smoke script was built with ENABLE_HNSWLIB=0 to avoid changing current build state; run with an hnswlib-enabled build after dependency initialization"
else
  append_unavailable "hnswlib_hnsw" "$hnswlib_reason"
fi

if [ "$faiss_available" -eq 1 ]; then
  append_unavailable "faiss_flat" "dependency appears present, but current smoke build uses ENABLE_FAISS=0; run with ENABLE_FAISS=1 after validating Faiss build"
  append_unavailable "faiss_hnsw" "dependency appears present, but current smoke build uses ENABLE_FAISS=0; run with ENABLE_FAISS=1 after validating Faiss build"
else
  append_unavailable "faiss_flat" "$faiss_reason"
  append_unavailable "faiss_hnsw" "$faiss_reason"
fi

cat > "$SUMMARY_MD" <<EOF
# KVLite Native vs Faiss Server-path Smoke

This is a 1K/64d server-path smoke only. It uses the real \`kvstore\` server and \`VSET\`/\`VSEARCH\` commands, but it is not a formal 10K/100K benchmark.

## Dataset

| Field | Value |
|---|---|
| dataset_id | \`$DATASET_ID\` |
| manifest | \`$MANIFEST\` |
| N | $N |
| nq | $NQ |
| dim | $DIM |
| metric | $METRIC |
| normalize | $NORMALIZE |
| topK | $TOPK |
| smoke scope | \`$SCOPE\` |

## Dependency Readiness

| Dependency | Status | Reason |
|---|---|---|
| hnswlib | $([ "$hnswlib_available" -eq 1 ] && echo available || echo unavailable) | $hnswlib_reason |
| Faiss | $([ "$faiss_available" -eq 1 ] && echo available || echo unavailable) | $faiss_reason |

## Backend Results

| Backend | Status | Notes |
|---|---|---|
| native_flat | pass | VSET prepare + VSET and VSEARCH ran through RESP server path |
| hnswlib_hnsw | unavailable | $hnswlib_reason |
| faiss_flat | unavailable | $faiss_reason |
| faiss_hnsw | unavailable | $faiss_reason |

## Output

| File | Path |
|---|---|
| raw.csv | \`$RAW_CSV\` |
| summary.md | \`$SUMMARY_MD\` |
| server logs | \`$LOG_DIR/server_*.log\` |

## Notes

Backend switching uses temporary config files rather than the CLI \`--vector_index_type\` parser. The native_flat smoke uses deterministic text generated by \`tests/vector_benchmark.c\`; the fixed binary dataset manifest is recorded as the run identity, but formal server-path runners still need one-to-one dataset.bin/query.bin command replay before formal performance comparison.
EOF

echo "server-path smoke written:"
echo "  $RAW_CSV"
echo "  $SUMMARY_MD"
echo "  $LOG_DIR/server_*.log"
