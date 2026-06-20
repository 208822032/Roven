#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

KV_HOST="${KV_HOST:-127.0.0.1}"
KV_PORT="${KV_PORT:-9300}"
C="${C:-50}"
N="${N:-100000}"
P="${P:-1}"
VALUE_SIZE="${VALUE_SIZE:-32}"
KEYSPACE="${KEYSPACE:-100000}"
POP_N="${POP_N:-$KEYSPACE}"
PERF_FREQ="${PERF_FREQ:-99}"
PERF_TIMEOUT_SEC="${PERF_TIMEOUT_SEC:-10}"
PERF_EVENT="${PERF_EVENT:-cpu-clock}"
SUDO_PASSWORD="${SUDO_PASSWORD:-}"
PID="${PID:-}"
DS="${DS:-}"
OP="${OP:-}"
RUN_ID="${RUN_ID:-$(date +%Y%m%d_%H%M%S)}"

PERF_DIR="${PERF_DIR:-$ROOT_DIR/results/profile}"
LOG_DIR="${LOG_DIR:-$ROOT_DIR/logs/perf}"
mkdir -p "$PERF_DIR" "$LOG_DIR"

usage() {
  cat <<EOF
Usage: $(basename "$0") --ds rbtree|skiptable [--op set|get] [options]

Options:
  --ds NAME          Data structure to profile (required)
  --op NAME          Benchmark op to profile: set or get (required)
  --pid PID          kvstore PID to profile (default: auto-detect)
  --kv-host HOST     KVStore host (default: $KV_HOST)
  --kv-port PORT     KVStore port (default: $KV_PORT)
  -c, --clients N    Concurrency (default: $C)
  -n, --requests N   Requests for the profiled phase (default: $N)
  -P, --pipeline N   Pipeline depth (default: $P)
  --value-size N     Value size in bytes (default: $VALUE_SIZE)
  --keyspace N       Keyspace for random keys (default: $KEYSPACE)
  --populate N       Populate requests before profiling (default: $POP_N)
  --perf-freq N      perf sampling frequency (default: $PERF_FREQ)
  --perf-timeout S   perf sampling duration in seconds (default: $PERF_TIMEOUT_SEC)
  --perf-event EVT   perf event to sample (default: $PERF_EVENT)
  --perf-dir DIR     perf output directory (default: $PERF_DIR)
  --log-dir DIR      benchmark log directory (default: $LOG_DIR)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ds) DS="$2"; shift 2 ;;
    --op) OP="$2"; shift 2 ;;
    --pid) PID="$2"; shift 2 ;;
    --kv-host) KV_HOST="$2"; shift 2 ;;
    --kv-port) KV_PORT="$2"; shift 2 ;;
    -c|--clients) C="$2"; shift 2 ;;
    -n|--requests) N="$2"; shift 2 ;;
    -P|--pipeline) P="$2"; shift 2 ;;
    --value-size) VALUE_SIZE="$2"; shift 2 ;;
    --keyspace) KEYSPACE="$2"; shift 2 ;;
    --populate) POP_N="$2"; shift 2 ;;
    --perf-freq) PERF_FREQ="$2"; shift 2 ;;
    --perf-timeout) PERF_TIMEOUT_SEC="$2"; shift 2 ;;
    --perf-event) PERF_EVENT="$2"; shift 2 ;;
    --perf-dir) PERF_DIR="$2"; shift 2; mkdir -p "$PERF_DIR" ;;
    --log-dir) LOG_DIR="$2"; shift 2; mkdir -p "$LOG_DIR" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ -z "$DS" || -z "$OP" ]]; then
  echo "[ERROR] --ds and --op are required" >&2
  usage
  exit 1
fi

case "$DS" in
  rbtree|skiptable|array|hash) ;;
  *) echo "[ERROR] Unsupported ds: $DS" >&2; exit 1 ;;
esac

case "$OP" in
  set|get) ;;
  *) echo "[ERROR] Unsupported op: $OP" >&2; exit 1 ;;
esac

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "[ERROR] Missing dependency: $1" >&2
    exit 1
  fi
}

need_cmd redis-benchmark
need_cmd python3

if [[ -z "$PID" ]]; then
  PID="$(pgrep -n -x kvstore || true)"
fi

if [[ -z "$PID" ]]; then
  echo "[ERROR] Could not auto-detect kvstore PID; pass --pid explicitly" >&2
  exit 1
fi

render_value() {
  python3 - "$VALUE_SIZE" <<'PY'
import sys
n = int(sys.argv[1])
print("v" * max(0, n))
PY
}

build_command() {
  local ds="$1"
  local op="$2"
  case "$ds:$op" in
    rbtree:set) echo "RSET key:__rand_int__ __value__" ;;
    rbtree:get) echo "RGET key:__rand_int__" ;;
    skiptable:set) echo "SSET key:__rand_int__ __value__" ;;
    skiptable:get) echo "SGET key:__rand_int__" ;;
    array:set) echo "SET key:__rand_int__ __value__" ;;
    array:get) echo "GET key:__rand_int__" ;;
    hash:set) echo "HSET key:__rand_int__ __value__" ;;
    hash:get) echo "HGET key:__rand_int__" ;;
    *) return 1 ;;
  esac
}

run_redis_benchmark() {
  local nreq="$1"
  local command_tpl="$2"
  local value_blob="$3"
  local log_file="$4"

  local cmd="$command_tpl"
  cmd="${cmd//__value__/$value_blob}"
  read -r -a cmd_parts <<< "$cmd"

  set +e
  redis-benchmark \
    -h "$KV_HOST" \
    -p "$KV_PORT" \
    -c "$C" \
    -n "$nreq" \
    -P "$P" \
    -d "$VALUE_SIZE" \
    -r "$KEYSPACE" \
    "${cmd_parts[@]}" >"$log_file" 2>&1
  local rc=$?
  set -e
  return "$rc"
}

VALUE_BLOB="$(render_value)"
RUN_PREFIX="${RUN_ID}_${DS}_${OP}"
PERF_DATA="$PERF_DIR/${RUN_PREFIX}.perf.data"
PERF_REPORT="$PERF_DIR/${RUN_PREFIX}.perf.report.txt"
BENCH_LOG="$LOG_DIR/${RUN_PREFIX}.bench.log"
POP_LOG="$LOG_DIR/${RUN_PREFIX}.populate.log"
PERF_RECORD_LOG="$PERF_DIR/${RUN_PREFIX}.perf.record.log"

echo "[INFO] Run ID: $RUN_ID"
echo "[INFO] Target PID: $PID"
echo "[INFO] Case: ds=$DS op=$OP c=$C n=$N P=$P value_size=$VALUE_SIZE keyspace=$KEYSPACE populate=$POP_N"
echo "[INFO] Perf event: $PERF_EVENT"

populate_tpl="$(build_command "$DS" set)"
profile_tpl="$(build_command "$DS" "$OP")"

echo "[INFO] Warming up with populate phase: $DS"
run_redis_benchmark "$POP_N" "$populate_tpl" "$VALUE_BLOB" "$POP_LOG" || true

echo "[INFO] Profiling benchmark phase: $DS $OP"
run_redis_benchmark "$N" "$profile_tpl" "$VALUE_BLOB" "$BENCH_LOG" &
BENCH_PID=$!

if [[ -n "$SUDO_PASSWORD" ]]; then
  set +e
  sudo -S -p '' timeout -s INT "$PERF_TIMEOUT_SEC" perf record -e "$PERF_EVENT" -F "$PERF_FREQ" -p "$PID" -o "$PERF_DATA" <<<"$SUDO_PASSWORD" > /dev/null 2>"$PERF_RECORD_LOG"
  PERF_RC=$?
  set -e
else
  set +e
  sudo -p '' timeout -s INT "$PERF_TIMEOUT_SEC" perf record -e "$PERF_EVENT" -F "$PERF_FREQ" -p "$PID" -o "$PERF_DATA" > /dev/null 2>"$PERF_RECORD_LOG"
  PERF_RC=$?
  set -e
fi

wait "$BENCH_PID" || true

if [[ "$PERF_RC" -ne 0 && "$PERF_RC" -ne 124 && "$PERF_RC" -ne 130 ]]; then
  echo "[WARN] perf record exited with rc=$PERF_RC" >&2
fi

if [[ ! -s "$PERF_DATA" ]]; then
  echo "[ERROR] perf data was not produced: $PERF_DATA" >&2
  exit 1
fi

if [[ -n "$SUDO_PASSWORD" ]]; then
  sudo -S -p '' perf report --stdio -i "$PERF_DATA" --sort symbol,dso --percent-limit 1 --no-children <<<"$SUDO_PASSWORD" > "$PERF_REPORT"
else
  sudo -p '' perf report --stdio -i "$PERF_DATA" --sort symbol,dso --percent-limit 1 --no-children > "$PERF_REPORT"
fi

echo "[DONE] perf data: $PERF_DATA"
echo "[DONE] perf report: $PERF_REPORT"
echo "[DONE] benchmark log: $BENCH_LOG"
echo "[DONE] populate log: $POP_LOG"
