#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

KV_HOST="${KV_HOST:-127.0.0.1}"
KV_PORT="${KV_PORT:-9300}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"

C="${C:-50}"
N="${N:-100000}"
P="${P:-1}"
VALUE_SIZE="${VALUE_SIZE:-32}"
KEYSPACE="${KEYSPACE:-100000}"
POP_N="${POP_N:-$KEYSPACE}"
RANGE_LIMIT="${RANGE_LIMIT:-100}"
VALIDATE_RATIO="${VALIDATE_RATIO:-0.01}"
TIMEOUT_SEC="${TIMEOUT_SEC:-10}"
ALL_RANGE_SORT_CLOSE="${ALL_RANGE_SORT_CLOSE:-0}"

LOG_DIR="${LOG_DIR:-$ROOT_DIR/logs}"
RESULT_DIR="${RESULT_DIR:-$ROOT_DIR/results}"
RAW_CSV="$RESULT_DIR/raw.csv"
SUMMARY_MD="$RESULT_DIR/summary.md"

RUN_ID="$(date +%Y%m%d_%H%M%S)"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --kv-host HOST          KVStore host (default: $KV_HOST)
  --kv-port PORT          KVStore port (default: $KV_PORT)
  --redis-host HOST       Redis host (default: $REDIS_HOST)
  --redis-port PORT       Redis port (default: $REDIS_PORT)
  -c, --clients N         Concurrency (default: $C)
  -n, --requests N        Requests for each benchmark phase (default: $N)
  -P, --pipeline N        Pipeline depth (default: $P)
  --value-size N          Value size in bytes (default: $VALUE_SIZE)
  --keyspace N            Keyspace for random keys (default: $KEYSPACE)
  --populate N            Populate requests before read/range/sort (default: $POP_N)
  --range-limit N         LIMIT for RRANGE/SRANGE and *SORT (default: $RANGE_LIMIT)
  --validate-ratio R      Range/sort response type sample validation ratio [0,1] (default: $VALIDATE_RATIO)
  --timeout SEC           Socket timeout for custom RESP benchmark (default: $TIMEOUT_SEC)
  --log-dir DIR           Log output dir (default: $LOG_DIR)
  --result-dir DIR        Result output dir (default: $RESULT_DIR)
  --all-range/sort-close   Disable range/sort benchmark for all data structures
  -h, --help              Show this help

Environment variables can override defaults: KV_HOST KV_PORT REDIS_HOST REDIS_PORT C N P VALUE_SIZE KEYSPACE POP_N RANGE_LIMIT VALIDATE_RATIO TIMEOUT_SEC LOG_DIR RESULT_DIR ALL_RANGE_SORT_CLOSE
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --kv-host) KV_HOST="$2"; shift 2 ;;
    --kv-port) KV_PORT="$2"; shift 2 ;;
    --redis-host) REDIS_HOST="$2"; shift 2 ;;
    --redis-port) REDIS_PORT="$2"; shift 2 ;;
    -c|--clients) C="$2"; shift 2 ;;
    -n|--requests) N="$2"; shift 2 ;;
    -P|--pipeline) P="$2"; shift 2 ;;
    --value-size) VALUE_SIZE="$2"; shift 2 ;;
    --keyspace) KEYSPACE="$2"; shift 2 ;;
    --populate) POP_N="$2"; shift 2 ;;
    --range-limit) RANGE_LIMIT="$2"; shift 2 ;;
    --validate-ratio) VALIDATE_RATIO="$2"; shift 2 ;;
    --timeout) TIMEOUT_SEC="$2"; shift 2 ;;
    --log-dir) LOG_DIR="$2"; shift 2 ;;
    --result-dir) RESULT_DIR="$2"; RAW_CSV="$RESULT_DIR/raw.csv"; SUMMARY_MD="$RESULT_DIR/summary.md"; shift 2 ;;
    --all-range/sort-close) ALL_RANGE_SORT_CLOSE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ "$ALL_RANGE_SORT_CLOSE" != "0" && "$ALL_RANGE_SORT_CLOSE" != "1" ]]; then
  echo "[ERROR] Invalid ALL_RANGE_SORT_CLOSE: $ALL_RANGE_SORT_CLOSE (allowed: 0|1)" >&2
  exit 1
fi

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "[ERROR] Missing dependency: $1" >&2
    return 1
  fi
}

mkdir -p "$LOG_DIR" "$RESULT_DIR"

need_cmd redis-benchmark
need_cmd python3

cat > "$RAW_CSV" <<EOF
system,ds,op,c,P,n,value_size,keyspace,qps,avg_ms,p50_ms,p95_ms,p99_ms,notes
EOF

csv_escape() {
  local s="${1:-}"
  s="${s//\"/\"\"}"
  printf '"%s"' "$s"
}

append_row() {
  local system="$1" ds="$2" op="$3" c="$4" p="$5" n="$6" vsize="$7" keyspace="$8" qps="$9" avg="${10}" p50="${11}" p95="${12}" p99="${13}" notes="${14}"
  printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
    "$system" "$ds" "$op" "$c" "$p" "$n" "$vsize" "$keyspace" "$qps" "$avg" "$p50" "$p95" "$p99" "$(csv_escape "$notes")" >> "$RAW_CSV"
}

parse_json_fields() {
  local json_file="$1"
  python3 - "$json_file" <<'PY'
import json, sys
p = sys.argv[1]
with open(p, 'r', encoding='utf-8') as f:
    d = json.load(f)
vals = [d.get('qps'), d.get('avg_ms'), d.get('p50_ms'), d.get('p95_ms'), d.get('p99_ms')]
out = []
for v in vals:
    if v is None:
        out.append("")
    else:
        out.append(f"{float(v):.6f}")
print("\t".join(out))
PY
}

run_redis_custom() {
  local system="$1" host="$2" port="$3" nreq="$4" ds="$5" op="$6" notes="$7"; shift 7
  local cmd_parts=("$@")

  local log="$LOG_DIR/${RUN_ID}_${host}_${port}_${ds}_${op}_redis_benchmark.log"
  local json="$log.json"

  set +e
  {
    echo "[INFO] Running redis-benchmark custom command for $host:$port ds=$ds op=$op"
    redis-benchmark -h "$host" -p "$port" -c "$C" -n "$nreq" -P "$P" -d "$VALUE_SIZE" -r "$KEYSPACE" "${cmd_parts[@]}"
  } >"$log" 2>&1
  local rb_rc=$?
  set -e

  if ! python3 scripts/parse_redis_benchmark.py --file "$log" > "$json"; then
    append_row "$system" "$ds" "$op" "$C" "$P" "$nreq" "$VALUE_SIZE" "$KEYSPACE" "" "" "" "" "" "${notes}; redis-benchmark parse failed"
    return 1
  fi

  local fields
  fields="$(parse_json_fields "$json")"
  local qps avg p50 p95 p99
  IFS=$'\t' read -r qps avg p50 p95 p99 <<< "$fields"

  if [[ $rb_rc -ne 0 ]]; then
    append_row "$system" "$ds" "$op" "$C" "$P" "$nreq" "$VALUE_SIZE" "$KEYSPACE" "$qps" "$avg" "$p50" "$p95" "$p99" "${notes}; redis-benchmark exit=${rb_rc}"
    return 1
  fi

  append_row "$system" "$ds" "$op" "$C" "$P" "$nreq" "$VALUE_SIZE" "$KEYSPACE" "$qps" "$avg" "$p50" "$p95" "$p99" "$notes"
  return 0
}

run_resp_bench() {
  local ds="$1" op="$2" cmd_tpl="$3" expect_type="$4" notes="$5" nreq="${6:-$N}"
  local log="$LOG_DIR/${RUN_ID}_kvstore_${ds}_${op}_respbench.log"
  local json="$log.json"

  {
    echo "[INFO] Running custom RESP benchmark on KVStore ds=$ds op=$op"
    python3 scripts/kvstore_resp_bench.py \
      --host "$KV_HOST" \
      --port "$KV_PORT" \
      --clients "$C" \
      --requests "$nreq" \
      --pipeline "$P" \
      --command "$cmd_tpl" \
      --value-size "$VALUE_SIZE" \
      --keyspace "$KEYSPACE" \
      --start-key "key:00000000" \
      --end-key "key:$(printf '%08d' $((KEYSPACE-1)))" \
      --limit "$RANGE_LIMIT" \
      --timeout "$TIMEOUT_SEC" \
      --validate-ratio "$VALIDATE_RATIO" \
      --expect-type "$expect_type" \
      --system kvstore \
      --ds "$ds" \
      --op "$op"
  } >"$log" 2>&1

  tail -n 1 "$log" > "$json"

  local fields
  fields="$(parse_json_fields "$json")"
  local qps avg p50 p95 p99
  IFS=$'\t' read -r qps avg p50 p95 p99 <<< "$fields"

  append_row "kvstore" "$ds" "$op" "$C" "$P" "$nreq" "$VALUE_SIZE" "$KEYSPACE" "$qps" "$avg" "$p50" "$p95" "$p99" "$notes"
}

cmd_write() {
  local ds="$1"
  case "$ds" in
    array) echo "SET key:__rand_int__ __value__" ;;
    rbtree) echo "RSET key:__rand_int__ __value__" ;;
    hash) echo "HSET key:__rand_int__ __value__" ;;
    skiptable) echo "SSET key:__rand_int__ __value__" ;;
    *) return 1 ;;
  esac
}

cmd_read() {
  local ds="$1"
  case "$ds" in
    array) echo "GET key:__rand_int__" ;;
    rbtree) echo "RGET key:__rand_int__" ;;
    hash) echo "HGET key:__rand_int__" ;;
    skiptable) echo "SGET key:__rand_int__" ;;
    *) return 1 ;;
  esac
}

cmd_range() {
  local ds="$1"
  case "$ds" in
    array) echo "RANGE __start_key__ __end_key__" ;;
    rbtree) echo "RRANGE __start_key__ __end_key__ LIMIT __limit__" ;;
    hash) echo "HRANGE __start_key__ __end_key__" ;;
    skiptable) echo "SRANGE __start_key__ __end_key__ LIMIT __limit__" ;;
    *) return 1 ;;
  esac
}

cmd_sort() {
  local ds="$1"
  case "$ds" in
    array) echo "SORT low __limit__" ;;
    rbtree) echo "RSORT low __limit__" ;;
    hash) echo "HSORT low __limit__" ;;
    skiptable) echo "SSORT low __limit__" ;;
    *) return 1 ;;
  esac
}

render_value() {
  python3 - "$VALUE_SIZE" <<'PY'
import sys
n = int(sys.argv[1])
print("v" * max(0, n))
PY
}

VALUE_BLOB="$(render_value)"

echo "[INFO] Run ID: $RUN_ID"
echo "[INFO] KVStore target: $KV_HOST:$KV_PORT, Redis target: $REDIS_HOST:$REDIS_PORT"
echo "[INFO] Params: c=$C n=$N P=$P value_size=$VALUE_SIZE keyspace=$KEYSPACE populate=$POP_N range_limit=$RANGE_LIMIT"
echo "[INFO] all range/sort close: $ALL_RANGE_SORT_CLOSE"

declare -a DS_LIST=(array rbtree hash skiptable)
RUN_RANGE_SORT=1
if [[ "$ALL_RANGE_SORT_CLOSE" -eq 1 ]]; then
  RUN_RANGE_SORT=0
fi

for ds in "${DS_LIST[@]}"; do
  write_tpl="$(cmd_write "$ds")"
  read_tpl="$(cmd_read "$ds")"
  if [[ "$RUN_RANGE_SORT" -eq 1 ]]; then
    range_tpl="$(cmd_range "$ds")"
    sort_tpl="$(cmd_sort "$ds")"
  fi

  write_cmd="${write_tpl//__value__/$VALUE_BLOB}"
  read -r -a write_parts <<< "$write_cmd"
  read -r -a read_parts <<< "$read_tpl"

  if ! run_redis_custom "kvstore" "$KV_HOST" "$KV_PORT" "$POP_N" "$ds" "populate" "kvstore populate phase via redis-benchmark custom command" "${write_parts[@]}"; then
    run_resp_bench "$ds" "populate" "$write_tpl" "simple" "fallback: kvstore populate via custom RESP benchmark" "$POP_N"
  fi

  if ! run_redis_custom "kvstore" "$KV_HOST" "$KV_PORT" "$N" "$ds" "set" "kvstore set via redis-benchmark custom command" "${write_parts[@]}"; then
    run_resp_bench "$ds" "set" "$write_tpl" "simple" "fallback: kvstore set via custom RESP benchmark" "$N"
  fi

  if ! run_redis_custom "kvstore" "$KV_HOST" "$KV_PORT" "$N" "$ds" "get" "kvstore get via redis-benchmark custom command" "${read_parts[@]}"; then
    run_resp_bench "$ds" "get" "$read_tpl" "any" "fallback: kvstore get via custom RESP benchmark" "$N"
  fi

  if [[ "$RUN_RANGE_SORT" -eq 1 ]]; then
    run_resp_bench "$ds" "range" "$range_tpl" "array" "kvstore range via custom RESP benchmark"
    run_resp_bench "$ds" "sort" "$sort_tpl" "array" "kvstore sort via custom RESP benchmark"
  fi

done

for ds in "${DS_LIST[@]}"; do
  redis_set_cmd="SET key:__rand_int__ $VALUE_BLOB"
  redis_get_cmd="GET key:__rand_int__"

  read -r -a redis_set_parts <<< "$redis_set_cmd"
  read -r -a redis_get_parts <<< "$redis_get_cmd"

  run_redis_custom "redis" "$REDIS_HOST" "$REDIS_PORT" "$POP_N" "$ds" "populate" "redis populate for aligned set/get" "${redis_set_parts[@]}"
  run_redis_custom "redis" "$REDIS_HOST" "$REDIS_PORT" "$N" "$ds" "set" "redis set aligned with kvstore set/get" "${redis_set_parts[@]}"
  run_redis_custom "redis" "$REDIS_HOST" "$REDIS_PORT" "$N" "$ds" "get" "redis get aligned with kvstore set/get" "${redis_get_parts[@]}"

  if [[ "$RUN_RANGE_SORT" -eq 1 ]]; then
    append_row "redis" "$ds" "range" "$C" "$P" "$N" "$VALUE_SIZE" "$KEYSPACE" "" "" "" "" "" "not aligned: kvstore RANGE family has different semantics than redis-benchmark default commands"
    append_row "redis" "$ds" "sort" "$C" "$P" "$N" "$VALUE_SIZE" "$KEYSPACE" "" "" "" "" "" "not aligned: kvstore SORT family differs from Redis structure-specific operations"
  fi
done

python3 - "$RAW_CSV" "$SUMMARY_MD" <<'PY'
import csv
import sys

raw_csv, summary_md = sys.argv[1], sys.argv[2]

rows = []
with open(raw_csv, newline='', encoding='utf-8') as f:
    for r in csv.DictReader(f):
        rows.append(r)

# Keep latest per (system,ds,op) excluding populate
latest = {}
for r in rows:
    if r['op'] == 'populate':
        continue
    latest[(r['system'], r['ds'], r['op'])] = r

observed_ds = sorted({r['ds'] for r in rows if r.get('ds')})
preferred_ops = ['set', 'get', 'range', 'sort']
observed_ops = [op for op in preferred_ops if any(r.get('op') == op for r in rows)]
if not observed_ops:
  observed_ops = ['set', 'get']

dss = observed_ds if observed_ds else ['array', 'rbtree', 'hash', 'skiptable']
ops = observed_ops

md = []
md.append('# Benchmark Summary')
md.append('')
md.append('| ds | op | kvstore_qps | redis_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | redis_p50(ms) | redis_p95(ms) | redis_p99(ms) | notes |')
md.append('|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|')

for ds in dss:
    for op in ops:
        kv = latest.get(('kvstore', ds, op), {})
        rd = latest.get(('redis', ds, op), {})
        note = ''
        if op in ('range', 'sort'):
            note = 'range/sort are not strictly aligned between KVStore and Redis; set/get are primary aligned baseline'
        elif kv.get('qps') and rd.get('qps'):
            try:
                kq = float(kv['qps'])
                rq = float(rd['qps'])
                ratio = (kq / rq) if rq > 0 else 0
                note = f'set/get aligned; kv/redis qps={ratio:.3f}'
            except Exception:
                note = 'set/get aligned'
        md.append('| {ds} | {op} | {kq} | {rq} | {kp50} | {kp95} | {kp99} | {rp50} | {rp95} | {rp99} | {note} |'.format(
            ds=ds,
            op=op,
            kq=kv.get('qps',''), rq=rd.get('qps',''),
            kp50=kv.get('p50_ms',''), kp95=kv.get('p95_ms',''), kp99=kv.get('p99_ms',''),
            rp50=rd.get('p50_ms',''), rp95=rd.get('p95_ms',''), rp99=rd.get('p99_ms',''),
            note=note,
        ))

# Simple bottleneck hint from kvstore non-empty qps
kv_ops = [r for r in latest.values() if r.get('system') == 'kvstore' and r.get('qps')]
worst = None
for r in kv_ops:
    try:
        q = float(r['qps'])
    except Exception:
        continue
    if worst is None or q < worst[0]:
        worst = (q, r)

md.append('')
md.append('## Conclusion')
if worst is None:
    md.append('- No valid kvstore qps records were parsed; check logs under logs/.')
else:
    r = worst[1]
    md.append(f"- KVStore lowest observed throughput is ds={r['ds']} op={r['op']} qps={r['qps']}, likely first bottleneck candidate.")
md.append('- SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace.')
if 'range' in ops or 'sort' in ops:
  md.append('- RANGE/SORT use custom RESP benchmark on KVStore; Redis side is marked not aligned due to semantic mismatch.')
else:
  md.append('- RANGE/SORT were skipped in this run scope.')

with open(summary_md, 'w', encoding='utf-8') as f:
    f.write('\n'.join(md) + '\n')
PY

echo "[DONE] Raw results: $RAW_CSV"
echo "[DONE] Summary: $SUMMARY_MD"
echo "[DONE] Logs dir: $LOG_DIR"
