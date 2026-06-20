#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"
CLIENT_BIN="${CLIENT_BIN:-./tests/kvstore_client}"
HOST="${KV_HOST:-127.0.0.1}"
if [[ -n "${TCP_TTL_BACKENDS:-}" ]]; then
  # Accept comma or space separated backend lists, e.g. "array" or "array,hash".
  read -r -a BACKENDS <<<"$(printf '%s' "$TCP_TTL_BACKENDS" | tr ',' ' ')"
else
  BACKENDS=(array rbtree hash skiptable)
fi

declare -A WRITE_CMD=(
  [array]="SET"
  [rbtree]="RSET"
  [hash]="HSET"
  [skiptable]="SSET"
)

declare -A READ_CMD=(
  [array]="GET"
  [rbtree]="RGET"
  [hash]="HGET"
  [skiptable]="SGET"
)

declare -A DEL_CMD=(
  [array]="DEL"
  [rbtree]="RDEL"
  [hash]="HDEL"
  [skiptable]="SDEL"
)

TMP_DIR="$(mktemp -d)"
cleanup_master_pid=""
cleanup_slave_pid=""
MASTER_LOG=""
SLAVE_LOG=""

cleanup() {
  if [[ -n "${cleanup_slave_pid:-}" ]] && kill -0 "$cleanup_slave_pid" 2>/dev/null; then
    kill -TERM "$cleanup_slave_pid" 2>/dev/null || true
    wait "$cleanup_slave_pid" 2>/dev/null || true
  fi
  if [[ -n "${cleanup_master_pid:-}" ]] && kill -0 "$cleanup_master_pid" 2>/dev/null; then
    kill -TERM "$cleanup_master_pid" 2>/dev/null || true
    wait "$cleanup_master_pid" 2>/dev/null || true
  fi
  if [[ "${KEEP_TTL_TCP_TMP:-0}" != "1" ]]; then
    rm -rf "$TMP_DIR"
  else
    printf '[INFO][cleanup] keeping tmp dir: %s\n' "$TMP_DIR" >&2
  fi
}
trap cleanup EXIT

pick_ports() {
  python3 - <<'PY'
import socket

ports = []
while len(ports) < 3:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    if port not in ports:
        ports.append(port)
print(" ".join(str(port) for port in ports))
PY
}

normalize_output() {
  python3 -c 'import re,sys; data=sys.stdin.read(); data=re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", data); data=data.replace("\r", " ").replace("\n", " "); data=re.sub(r"[ \t]+", " ", data).strip(); sys.stdout.write(data)'
}

capture_output_on_port() {
  local port="$1"
  local cmd="$2"
  "$CLIENT_BIN" "$HOST" "$port" -c "$cmd" | normalize_output
}

capture_master() {
  capture_output_on_port "$MASTER_PORT" "$1"
}

capture_slave() {
  capture_output_on_port "$SLAVE_PORT" "$1"
}

pass_case() {
  local backend="$1"
  local role="$2"
  local case_name="$3"
  local cmd="$4"
  printf '[PASS][%s][%s][%s] cmd=%s\n' "$backend" "$role" "$case_name" "$cmd"
}

fail_case() {
  local backend="$1"
  local role="$2"
  local case_name="$3"
  local reason="$4"
  local expected="${5:-}"
  local actual="${6:-}"
  local cmd="${7:-}"

  printf '[FAIL][%s][%s][%s] reason=%s' "$backend" "$role" "$case_name" "$reason" >&2
  if [[ -n "$expected" ]]; then printf ' expected=%s' "$expected" >&2; fi
  if [[ -n "$actual" ]]; then printf ' actual=%s' "$actual" >&2; fi
  if [[ -n "$cmd" ]]; then printf ' cmd=%s' "$cmd" >&2; fi
  printf '\n' >&2

  if [[ -f "$MASTER_LOG" ]]; then
    printf '[FAIL][%s][master-log]\n' "$backend" >&2
    tail -n 120 "$MASTER_LOG" >&2 || true
  fi
  if [[ -f "$SLAVE_LOG" ]]; then
    printf '[FAIL][%s][slave-log]\n' "$backend" >&2
    tail -n 120 "$SLAVE_LOG" >&2 || true
  fi
  exit 1
}

expect_exact_on_port() {
  local backend="$1"
  local role="$2"
  local port="$3"
  local case_name="$4"
  local cmd="$5"
  local expected="$6"
  local out

  out="$(capture_output_on_port "$port" "$cmd")"
  if [[ "$out" != "$expected" ]]; then
    fail_case "$backend" "$role" "$case_name" "response mismatch" "$expected" "$out" "$cmd"
  fi
  pass_case "$backend" "$role" "$case_name" "$cmd"
}

expect_exact_one_of_on_port() {
  local backend="$1"
  local role="$2"
  local port="$3"
  local case_name="$4"
  local cmd="$5"
  local expected_a="$6"
  local expected_b="$7"
  local out

  out="$(capture_output_on_port "$port" "$cmd")"
  if [[ "$out" != "$expected_a" && "$out" != "$expected_b" ]]; then
    fail_case "$backend" "$role" "$case_name" "response mismatch" "$expected_a | $expected_b" "$out" "$cmd"
  fi
  pass_case "$backend" "$role" "$case_name" "$cmd"
}

expect_ttl_exact_on_port() {
  local backend="$1"
  local role="$2"
  local port="$3"
  local case_name="$4"
  local key="$5"
  local expected="$6"
  local out

  out="$(capture_output_on_port "$port" "TTL $key")"
  if [[ "$out" != "(integer) $expected" ]]; then
    fail_case "$backend" "$role" "$case_name" "TTL mismatch" "(integer) $expected" "$out" "TTL $key"
  fi
  pass_case "$backend" "$role" "$case_name" "TTL $key"
}

expect_ttl_range_on_port() {
  local backend="$1"
  local role="$2"
  local port="$3"
  local case_name="$4"
  local key="$5"
  local min="$6"
  local max="$7"
  local out val

  out="$(capture_output_on_port "$port" "TTL $key")"
  if [[ "$out" =~ ^\(integer\)\ (-?[0-9]+)$ ]]; then
    val="${BASH_REMATCH[1]}"
  else
    fail_case "$backend" "$role" "$case_name" "TTL output not integer" "(integer) <n>" "$out" "TTL $key"
  fi

  if (( val < min || val > max )); then
    fail_case "$backend" "$role" "$case_name" "TTL value out of range" "[$min, $max]" "$out" "TTL $key"
  fi
  pass_case "$backend" "$role" "$case_name" "TTL $key"
}

expect_eventually_exact_on_port() {
  local backend="$1"
  local role="$2"
  local port="$3"
  local case_name="$4"
  local cmd="$5"
  local expected="$6"
  local timeout_secs="${7:-10}"
  local interval_secs="${8:-0.2}"
  local start_ts now_ts out

  start_ts="$(date +%s)"
  while true; do
    out="$(capture_output_on_port "$port" "$cmd")"
    now_ts="$(date +%s)"
    if [[ "$out" == "$expected" ]]; then
      pass_case "$backend" "$role" "$case_name" "$cmd"
      return 0
    fi
    if (( now_ts - start_ts >= timeout_secs )); then
      fail_case "$backend" "$role" "$case_name" "eventually exact timeout" "$expected" "$out" "$cmd"
    fi
    sleep "$interval_secs"
  done
}

expect_eventually_ttl_exact_on_port() {
  local backend="$1"
  local role="$2"
  local port="$3"
  local case_name="$4"
  local key="$5"
  local expected="$6"
  local timeout_secs="${7:-10}"
  local interval_secs="${8:-0.2}"
  local start_ts now_ts out

  start_ts="$(date +%s)"
  while true; do
    out="$(capture_output_on_port "$port" "TTL $key")"
    now_ts="$(date +%s)"
    if [[ "$out" == "(integer) $expected" ]]; then
      pass_case "$backend" "$role" "$case_name" "TTL $key"
      return 0
    fi
    if (( now_ts - start_ts >= timeout_secs )); then
      fail_case "$backend" "$role" "$case_name" "TTL mismatch" "(integer) $expected" "$out" "TTL $key"
    fi
    sleep "$interval_secs"
  done
}

expect_eventually_ttl_range_on_port() {
  local backend="$1"
  local role="$2"
  local port="$3"
  local case_name="$4"
  local key="$5"
  local min="$6"
  local max="$7"
  local timeout_secs="${8:-10}"
  local interval_secs="${9:-0.2}"
  local start_ts now_ts out val

  start_ts="$(date +%s)"
  while true; do
    out="$(capture_output_on_port "$port" "TTL $key")"
    now_ts="$(date +%s)"
    if [[ "$out" =~ ^\(integer\)\ (-?[0-9]+)$ ]]; then
      val="${BASH_REMATCH[1]}"
      if (( val >= min && val <= max )); then
        pass_case "$backend" "$role" "$case_name" "TTL $key"
        return 0
      fi
    fi
    if (( now_ts - start_ts >= timeout_secs )); then
      fail_case "$backend" "$role" "$case_name" "TTL value out of range" "[$min, $max]" "$out" "TTL $key"
    fi
    sleep "$interval_secs"
  done
}

wait_for_port_ready() {
  local port="$1"
  local probe_cmd="$2"
  local i
  for i in $(seq 1 120); do
    if capture_output_on_port "$port" "$probe_cmd" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

start_master() {
  local backend="$1"
  local port="$2"
  local sync_port="$3"

  MASTER_LOG="$TMP_DIR/${backend}_master.log"
  local config_file="$TMP_DIR/${backend}_master.conf"

  cat > "$config_file" <<EOF
role = master
port = $port
sync_port = $sync_port
enable_aof = no
aof_fsync_mode = no
aof_filename = $TMP_DIR/${backend}_master.aof
rdb_filename = $TMP_DIR/${backend}_master.rdb
dump_filename = $TMP_DIR/${backend}_master.dat
network_mode = reactor
mem_backend = pool
sync_mode = tcp
max_value_size = 1048576
EOF

  "$KV_BIN" --config "$config_file" > "$MASTER_LOG" 2>&1 &
  cleanup_master_pid=$!
}

start_slave() {
  local backend="$1"
  local port="$2"
  local master_port="$3"
  local sync_port="$4"

  SLAVE_LOG="$TMP_DIR/${backend}_slave.log"
  local config_file="$TMP_DIR/${backend}_slave.conf"

  cat > "$config_file" <<EOF
role = slave
port = $port
master_ip = 127.0.0.1
master_port = $master_port
sync_port = $sync_port
enable_aof = no
aof_fsync_mode = no
aof_filename = $TMP_DIR/${backend}_slave.aof
rdb_filename = $TMP_DIR/${backend}_slave.rdb
dump_filename = $TMP_DIR/${backend}_slave.dat
network_mode = reactor
mem_backend = pool
sync_mode = tcp
max_value_size = 1048576
EOF

  "$KV_BIN" --config "$config_file" > "$SLAVE_LOG" 2>&1 &
  cleanup_slave_pid=$!
}

stop_slave() {
  if [[ -n "${cleanup_slave_pid:-}" ]] && kill -0 "$cleanup_slave_pid" 2>/dev/null; then
    kill -TERM "$cleanup_slave_pid" 2>/dev/null || true
    wait "$cleanup_slave_pid" 2>/dev/null || true
    cleanup_slave_pid=""
  fi
}

stop_master() {
  if [[ -n "${cleanup_master_pid:-}" ]] && kill -0 "$cleanup_master_pid" 2>/dev/null; then
    kill -TERM "$cleanup_master_pid" 2>/dev/null || true
    wait "$cleanup_master_pid" 2>/dev/null || true
    cleanup_master_pid=""
  fi
}

run_backend_suite() {
  local backend="$1"
  local write_cmd="${WRITE_CMD[$backend]}"
  local read_cmd="${READ_CMD[$backend]}"
  local del_cmd="${DEL_CMD[$backend]}"
  local prefix="tcp_${backend}_"

  read -r MASTER_PORT SLAVE_PORT SYNC_PORT < <(pick_ports)

  printf '[INFO][%s] master_port=%s slave_port=%s sync_port=%s\n' "$backend" "$MASTER_PORT" "$SLAVE_PORT" "$SYNC_PORT"

  start_master "$backend" "$MASTER_PORT" "$SYNC_PORT"
  if ! wait_for_port_ready "$MASTER_PORT" "EXIST __tcp_master_probe__"; then
    fail_case "$backend" "master" "startup" "master did not become ready" "ready" "not-ready" "EXIST __tcp_master_probe__"
  fi

  start_slave "$backend" "$SLAVE_PORT" "$MASTER_PORT" "$SYNC_PORT"
  if ! wait_for_port_ready "$SLAVE_PORT" "EXIST __tcp_slave_probe__"; then
    fail_case "$backend" "slave" "startup" "slave did not become ready" "ready" "not-ready" "EXIST __tcp_slave_probe__"
  fi

  local sync_key="${prefix}sync_probe"
  local expire_key="${prefix}expire_key"
  local persist_key="${prefix}persist_key"
  local delete_key="${prefix}delete_key"
  local overwrite_key="${prefix}overwrite_key"
  local full_keep_key="${prefix}full_keep"
  local full_ttl_key="${prefix}full_ttl"

  # Warm up the slave connection and confirm the TCP sync path is active.
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "sync-seed" "$write_cmd $sync_key ready" "OK"
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "sync-seed-replay" "$read_cmd $sync_key" "ready" 10 0.2

  # 1) SET + EXPIRE: slave should carry TTL and receive the master's eventual delete.
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "expire-set" "$write_cmd $expire_key value1" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "expire-apply" "EXPIRE $expire_key 2" "OK"
  expect_eventually_ttl_exact_on_port "$backend" "slave" "$SLAVE_PORT" "expire-ttl-replicated" "$expire_key" 1 10 0.2
  sleep 3
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "expire-slave-visible-after-time" "$read_cmd $expire_key" "NO EXIST" 5 0.2
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "expire-trigger-delete" "$read_cmd $expire_key" "NO EXIST"
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "expire-final-slave-delete" "$read_cmd $expire_key" "NO EXIST" 10 0.2

  # 2) PERSIST: slave should clear TTL after master persists the key.
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "persist-set" "$write_cmd $persist_key value2" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "persist-ttl" "EXPIRE $persist_key 60" "OK"
  expect_eventually_ttl_range_on_port "$backend" "slave" "$SLAVE_PORT" "persist-ttl-replicated" "$persist_key" 55 60
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "persist-action" "PERSIST $persist_key" "OK"
  expect_eventually_ttl_exact_on_port "$backend" "slave" "$SLAVE_PORT" "persist-ttl-cleared" "$persist_key" -1 10 0.2

  # 3) DEL: slave should remove the key after master deletion.
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "delete-set" "$write_cmd $delete_key value3" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "delete-action" "$del_cmd $delete_key" "OK"
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "delete-replayed" "$read_cmd $delete_key" "NO EXIST" 10 0.2

  # 4) Overwrite write clears TTL on both master and slave.
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "overwrite-set-1" "$write_cmd $overwrite_key value4" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "overwrite-expire" "EXPIRE $overwrite_key 60" "OK"
  expect_eventually_ttl_range_on_port "$backend" "slave" "$SLAVE_PORT" "overwrite-ttl-replicated" "$overwrite_key" 55 60
  expect_exact_one_of_on_port "$backend" "master" "$MASTER_PORT" "overwrite-write-2" "$write_cmd $overwrite_key value5" "OK" "EXIST"
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "overwrite-value-replayed" "$read_cmd $overwrite_key" "value5" 10 0.2
  expect_eventually_ttl_exact_on_port "$backend" "slave" "$SLAVE_PORT" "overwrite-ttl-cleared" "$overwrite_key" -1 10 0.2

  # 5) New slave full synchronization should restore permanent key and TTL key.
  stop_slave
  sleep 1

  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "fullsync-keep-set" "$write_cmd $full_keep_key keepme" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "fullsync-ttl-set" "$write_cmd $full_ttl_key ttlme" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "fullsync-ttl-apply" "EXPIRE $full_ttl_key 60" "OK"

  start_slave "$backend" "$SLAVE_PORT" "$MASTER_PORT" "$SYNC_PORT"
  if ! wait_for_port_ready "$SLAVE_PORT" "EXIST __tcp_slave_rejoin_probe__"; then
    fail_case "$backend" "slave" "rejoin-startup" "restarted slave did not become ready" "ready" "not-ready" "EXIST __tcp_slave_rejoin_probe__"
  fi

  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "fullsync-keep-visible" "$read_cmd $full_keep_key" "keepme" 15 0.2
  expect_eventually_ttl_exact_on_port "$backend" "slave" "$SLAVE_PORT" "fullsync-keep-ttl" "$full_keep_key" -1 15 0.2
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "fullsync-ttl-visible" "$read_cmd $full_ttl_key" "ttlme" 15 0.2
  expect_eventually_ttl_range_on_port "$backend" "slave" "$SLAVE_PORT" "fullsync-ttl-preserved" "$full_ttl_key" 55 60

  stop_slave
  stop_master

  printf '[PASS][%s][suite] TCP TTL replication suite completed successfully\n' "$backend"
}

for backend in "${BACKENDS[@]}"; do
  run_backend_suite "$backend"
done
