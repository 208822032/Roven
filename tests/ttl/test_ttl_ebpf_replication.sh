#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"
CLIENT_BIN="${CLIENT_BIN:-./tests/kvstore_client}"
EBPF_SYNCD_BIN="${EBPF_SYNCD_BIN:-./src/eBPF/ebpf_syncd}"
HOST="${KV_HOST:-127.0.0.1}"

if [[ -n "${EBPF_TTL_BACKENDS:-}" ]]; then
  read -r -a BACKENDS <<<"$(printf '%s' "$EBPF_TTL_BACKENDS" | tr ',' ' ')"
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

declare -A MOD_CMD=(
  [array]="MOD"
  [rbtree]="RMOD"
  [hash]="HMOD"
  [skiptable]="SMOD"
)

TMP_DIR="$(mktemp -d)"
MASTER_PID=""
SLAVE_PID=""
SYNCD_PID=""
MASTER_LOG=""
SLAVE_LOG=""
SYNCD_LOG=""

cleanup() {
  for pid_var in SYNCD_PID SLAVE_PID MASTER_PID; do
    local pid="${!pid_var:-}"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

normalize_output() {
  python3 -c 'import re,sys; data=sys.stdin.read(); data=re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", data); data=data.replace("\r", " ").replace("\n", " "); data=re.sub(r"[ \t]+", " ", data).strip(); sys.stdout.write(data)'
}

capture_output_on_port() {
  local port="$1"
  local cmd="$2"
  "$CLIENT_BIN" "$HOST" "$port" -c "$cmd" | normalize_output
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
  if [[ -f "$SYNCD_LOG" ]]; then
    printf '[FAIL][%s][syncd-log]\n' "$backend" >&2
    tail -n 120 "$SYNCD_LOG" >&2 || true
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

  MASTER_LOG="$TMP_DIR/${backend}_master.log"
  local config_file="$TMP_DIR/${backend}_master.conf"

  cat > "$config_file" <<EOF
role = master
port = $port
sync_port = $((port + 1000))
enable_aof = no
aof_fsync_mode = no
aof_filename = $TMP_DIR/${backend}_master.aof
rdb_filename = $TMP_DIR/${backend}_master.rdb
dump_filename = $TMP_DIR/${backend}_master.dat
network_mode = reactor
mem_backend = pool
sync_mode = ebpf
max_value_size = 1048576
EOF

  "$KV_BIN" --config "$config_file" > "$MASTER_LOG" 2>&1 &
  MASTER_PID=$!
}

start_slave() {
  local backend="$1"
  local port="$2"
  local master_port="$3"

  SLAVE_LOG="$TMP_DIR/${backend}_slave.log"
  local config_file="$TMP_DIR/${backend}_slave.conf"

  cat > "$config_file" <<EOF
role = slave
port = $port
master_ip = 127.0.0.1
master_port = $master_port
sync_port = $((master_port + 1000))
enable_aof = no
aof_fsync_mode = no
aof_filename = $TMP_DIR/${backend}_slave.aof
rdb_filename = $TMP_DIR/${backend}_slave.rdb
dump_filename = $TMP_DIR/${backend}_slave.dat
network_mode = reactor
mem_backend = pool
sync_mode = ebpf
max_value_size = 1048576
EOF

  "$KV_BIN" --config "$config_file" > "$SLAVE_LOG" 2>&1 &
  SLAVE_PID=$!
}

start_syncd() {
  local backend="$1"
  local master_port="$2"
  local slave_port="$3"

  SYNCD_LOG="$TMP_DIR/${backend}_syncd.log"
  local syncd_config="$TMP_DIR/${backend}_ebpf_sync.conf"

  cat > "$syncd_config" <<EOF
master_ip=127.0.0.1
master_port=$master_port
slave_count=1
slave_ip=127.0.0.1
slave_port=$slave_port
max_payload=1048576
EOF

  "$EBPF_SYNCD_BIN" "$syncd_config" > "$SYNCD_LOG" 2>&1 &
  SYNCD_PID=$!
  sleep 1
}

stop_syncd() {
  if [[ -n "${SYNCD_PID:-}" ]] && kill -0 "$SYNCD_PID" 2>/dev/null; then
    kill -TERM "$SYNCD_PID" 2>/dev/null || true
    wait "$SYNCD_PID" 2>/dev/null || true
    SYNCD_PID=""
  fi
}

stop_slave() {
  if [[ -n "${SLAVE_PID:-}" ]] && kill -0 "$SLAVE_PID" 2>/dev/null; then
    kill -TERM "$SLAVE_PID" 2>/dev/null || true
    wait "$SLAVE_PID" 2>/dev/null || true
    SLAVE_PID=""
  fi
}

stop_master() {
  if [[ -n "${MASTER_PID:-}" ]] && kill -0 "$MASTER_PID" 2>/dev/null; then
    kill -TERM "$MASTER_PID" 2>/dev/null || true
    wait "$MASTER_PID" 2>/dev/null || true
    MASTER_PID=""
  fi
}

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

run_backend_suite() {
  local backend="$1"
  local write_cmd="${WRITE_CMD[$backend]}"
  local read_cmd="${READ_CMD[$backend]}"
  local del_cmd="${DEL_CMD[$backend]}"
  local mod_cmd="${MOD_CMD[$backend]}"

  read -r MASTER_PORT SLAVE_PORT _ < <(pick_ports)

  printf '[INFO][%s] master_port=%s slave_port=%s\n' "$backend" "$MASTER_PORT" "$SLAVE_PORT"

  start_master "$backend" "$MASTER_PORT"
  if ! wait_for_port_ready "$MASTER_PORT" "EXIST __ebpf_master_probe__"; then
    fail_case "$backend" "master" "startup" "master did not become ready" "ready" "not-ready" "EXIST __ebpf_master_probe__"
  fi

  start_slave "$backend" "$SLAVE_PORT" "$MASTER_PORT"
  if ! wait_for_port_ready "$SLAVE_PORT" "EXIST __ebpf_slave_probe__"; then
    fail_case "$backend" "slave" "startup" "slave did not become ready" "ready" "not-ready" "EXIST __ebpf_slave_probe__"
  fi

  start_syncd "$backend" "$MASTER_PORT" "$SLAVE_PORT"

  local seed_key="ebpf_${backend}_seed"
  local expire_key="ebpf_${backend}_expire"
  local persist_key="ebpf_${backend}_persist"
  local delete_key="ebpf_${backend}_delete"
  local overwrite_key="ebpf_${backend}_overwrite"
  local full_keep_key="ebpf_${backend}_full_keep"
  local full_ttl_key="ebpf_${backend}_full_ttl"
  local full_expired_key="ebpf_${backend}_full_expired"
  local postsync_key="ebpf_${backend}_postsync"

  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "seed-write" "$write_cmd $seed_key value0" "OK"
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "seed-replicated" "$read_cmd $seed_key" "value0" 15 0.2

  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "expire-write" "$write_cmd $expire_key value1" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "expire-apply" "EXPIRE $expire_key 2" "OK"
  expect_eventually_ttl_range_on_port "$backend" "slave" "$SLAVE_PORT" "expire-ttl-replicated" "$expire_key" 0 2 15 0.2
  sleep 3
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "expire-slave-deleted" "$read_cmd $expire_key" "NO EXIST" 10 0.2
  expect_eventually_exact_on_port "$backend" "master" "$MASTER_PORT" "expire-master-deleted" "$read_cmd $expire_key" "NO EXIST" 10 0.2

  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "persist-write" "$write_cmd $persist_key value2" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "persist-expire" "EXPIRE $persist_key 60" "OK"
  expect_eventually_ttl_range_on_port "$backend" "slave" "$SLAVE_PORT" "persist-ttl-replicated" "$persist_key" 55 60 15 0.2
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "persist-action" "PERSIST $persist_key" "OK"
  expect_eventually_ttl_exact_on_port "$backend" "slave" "$SLAVE_PORT" "persist-ttl-cleared" "$persist_key" -1 15 0.2

  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "delete-write" "$write_cmd $delete_key value3" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "delete-action" "$del_cmd $delete_key" "OK"
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "delete-replayed" "$read_cmd $delete_key" "NO EXIST" 15 0.2

  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "overwrite-write-1" "$write_cmd $overwrite_key value4" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "overwrite-expire" "EXPIRE $overwrite_key 60" "OK"
  expect_eventually_ttl_range_on_port "$backend" "slave" "$SLAVE_PORT" "overwrite-ttl-replicated" "$overwrite_key" 55 60 15 0.2
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "overwrite-write-2" "$mod_cmd $overwrite_key value5" "OK"
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "overwrite-value-replayed" "$read_cmd $overwrite_key" "value5" 15 0.2
  expect_eventually_ttl_exact_on_port "$backend" "slave" "$SLAVE_PORT" "overwrite-ttl-cleared" "$overwrite_key" -1 15 0.2

  stop_syncd
  stop_slave

  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "fullsync-keep-write" "$write_cmd $full_keep_key keepme" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "fullsync-ttl-write" "$write_cmd $full_ttl_key ttlme" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "fullsync-ttl-apply" "EXPIRE $full_ttl_key 60" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "fullsync-expired-write" "$write_cmd $full_expired_key doomed" "OK"
  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "fullsync-expired-apply" "EXPIRE $full_expired_key 1" "OK"
  sleep 2

  start_slave "$backend" "$SLAVE_PORT" "$MASTER_PORT"
  if ! wait_for_port_ready "$SLAVE_PORT" "EXIST __ebpf_slave_rejoin_probe__"; then
    fail_case "$backend" "slave" "rejoin-startup" "restarted slave did not become ready" "ready" "not-ready" "EXIST __ebpf_slave_rejoin_probe__"
  fi

  start_syncd "$backend" "$MASTER_PORT" "$SLAVE_PORT"

  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "fullsync-keep-visible" "$read_cmd $full_keep_key" "keepme" 20 0.2
  expect_eventually_ttl_exact_on_port "$backend" "slave" "$SLAVE_PORT" "fullsync-keep-ttl" "$full_keep_key" -1 20 0.2
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "fullsync-ttl-visible" "$read_cmd $full_ttl_key" "ttlme" 20 0.2
  expect_eventually_ttl_range_on_port "$backend" "slave" "$SLAVE_PORT" "fullsync-ttl-preserved" "$full_ttl_key" 45 60 20 0.2
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "fullsync-expired-missing" "$read_cmd $full_expired_key" "NO EXIST" 20 0.2

  expect_exact_on_port "$backend" "master" "$MASTER_PORT" "postsync-write" "$write_cmd $postsync_key alive" "OK"
  expect_eventually_exact_on_port "$backend" "slave" "$SLAVE_PORT" "postsync-replicated" "$read_cmd $postsync_key" "alive" 20 0.2

  stop_syncd
  stop_slave
  stop_master

  pass_case "$backend" "suite" "all cases passed" "eBPF TTL replication"
}

for backend in "${BACKENDS[@]}"; do
  run_backend_suite "$backend"
done
