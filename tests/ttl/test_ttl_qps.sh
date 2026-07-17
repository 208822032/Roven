#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"
CLIENT_BIN="${CLIENT_BIN:-./tests/kvstore_client}"
TEST_BIN="tests/write_ttl_qps"
HOST="${KV_HOST:-127.0.0.1}"

if [[ -n "${TTL_QPS_BACKENDS:-}" ]]; then
  read -r -a BACKENDS <<<"$(printf '%s' "$TTL_QPS_BACKENDS" | tr ',' ' ')"
else
  BACKENDS=(array rbtree hash skiptable)
fi

COUNT="${TTL_QPS_COUNT:-100000}"
SAMPLE_COUNT="${TTL_QPS_SAMPLE_COUNT:-256}"
TTL_SECS="${TTL_QPS_TTL_SECS:-8}"
WAIT_BUFFER_SECS="${TTL_QPS_WAIT_BUFFER_SECS:-4}"

TMP_DIR="$(mktemp -d)"
SERVER_PID=""
SERVER_LOG=""
CONFIG_FILE=""

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  if [[ -z "${KEEP_TTL_QPS_TMP:-}" ]]; then
    rm -rf "$TMP_DIR"
  else
    printf '[INFO][cleanup] keeping tmp dir: %s\n' "$TMP_DIR" >&2
  fi
}
trap cleanup EXIT

ensure_kvstore_bins() {
  if [[ ! -x "$KV_BIN" || ! -x "$CLIENT_BIN" ]]; then
    printf '[INFO][build] building kvstore and kvstore_client via make\n'
    make -j2 kvstore tests/kvstore_client
  fi
}

normalize_output() {
  python3 -c 'import re,sys; data=sys.stdin.read(); data=re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", data); data=data.replace("\r", " ").replace("\n", " "); data=re.sub(r"[ \t]+", " ", data).strip(); sys.stdout.write(data)'
}

capture_output() {
  "$CLIENT_BIN" "$HOST" "$PORT" -c "$1" | normalize_output
}

wait_for_server() {
  for _ in $(seq 1 120); do
    if capture_output "EXIST __ttl_qps_probe__" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done

  printf '[FAIL][server] startup timeout\n' >&2
  [[ -f "$SERVER_LOG" ]] && tail -n 120 "$SERVER_LOG" >&2 || true
  exit 1
}

start_server() {
  local backend="$1"
  local port="$2"

  SERVER_LOG="$TMP_DIR/${backend}.log"
  CONFIG_FILE="$TMP_DIR/${backend}.conf"

  cat > "$CONFIG_FILE" <<EOF
role = master
port = $port
sync_port = $((port + 1000))
enable_aof = no
aof_fsync_mode = no
aof_filename = $TMP_DIR/${backend}.aof
rdb_filename = $TMP_DIR/${backend}.rdb
dump_filename = $TMP_DIR/${backend}.dat
network_mode = reactor
mem_backend = pool
sync_mode = disabled
max_value_size = 1048576
EOF

  "$KV_BIN" --config "$CONFIG_FILE" > "$SERVER_LOG" 2>&1 &
  SERVER_PID=$!
  wait_for_server
}

stop_server() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
  fi
}

pick_port() {
  python3 - <<'PY'
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
}

run_backend() {
  local backend="$1"
  local mode="$2"
  local port

  port="$(pick_port)"
  PORT="$port"
  printf '[INFO][%s] port=%s count=%s sample=%s ttl=%s wait_buffer=%s\n' \
    "$backend" "$port" "$COUNT" "$SAMPLE_COUNT" "$TTL_SECS" "$WAIT_BUFFER_SECS"

  start_server "$backend" "$port"
  if ! "$TEST_BIN" "$HOST" "$port" "$mode" "$COUNT" "$SAMPLE_COUNT" "$TTL_SECS" "$WAIT_BUFFER_SECS"; then
    printf '[FAIL][%s] ttl qps test failed\n' "$backend" >&2
    [[ -f "$SERVER_LOG" ]] && tail -n 120 "$SERVER_LOG" >&2 || true
    stop_server
    exit 1
  fi

  stop_server
  printf '[PASS][%s] ttl qps test passed\n' "$backend"
}

main() {
  ensure_kvstore_bins

  local i backend mode
  for i in "${!BACKENDS[@]}"; do
    backend="${BACKENDS[$i]}"
    case "$backend" in
      array) mode=1 ;;
      rbtree) mode=2 ;;
      hash) mode=3 ;;
      skiptable) mode=4 ;;
      all) mode=5 ;;
      *)
        printf '[FAIL] unknown backend=%s\n' "$backend" >&2
        exit 1
        ;;
    esac
    run_backend "$backend" "$mode"
  done

  printf '[PASS][ttl][qps] all backend runs completed successfully\n'
}

main "$@"
