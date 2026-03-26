#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"
CLIENT_BIN="${CLIENT_BIN:-./tests/kvstore_client}"
HOST="${KV_HOST:-127.0.0.1}"
if [[ -n "${RDB_TEST_PORT:-}" ]]; then
  PORT="$RDB_TEST_PORT"
else
  PORT="$(python3 -c 'import socket; sock = socket.socket(); sock.bind(("127.0.0.1", 0)); print(sock.getsockname()[1]); sock.close()')"
fi

TMP_DIR="$(mktemp -d)"
CONFIG_FILE="$TMP_DIR/kvstore_rdb_ttl.conf"
SERVER_LOG="$TMP_DIR/kvstore.log"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  if [[ -z "${KEEP_TTL_RDB_TMP:-}" ]]; then
    rm -rf "$TMP_DIR"
  else
    printf '[INFO][debug] keeping tmp dir: %s\n' "$TMP_DIR" >&2
  fi
}
trap cleanup EXIT

normalize_output() {
  python3 -c 'import re,sys; data=sys.stdin.read(); data=re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", data); data=data.replace("\r", " ").replace("\n", " "); data=re.sub(r"[ \t]+", " ", data).strip(); sys.stdout.write(data)'
}

capture_output() {
  "$CLIENT_BIN" "$HOST" "$PORT" -c "$1" | normalize_output
}

pass_case() {
  printf '[PASS][%s][rdb][ttl] cmd=%s\n' "$1" "$2"
}

fail_case() {
  printf '[FAIL][%s][rdb][ttl] cmd=%s expected=%s actual=%s\n' "$1" "$2" "$3" "$4" >&2
  exit 1
}

expect_exact() {
  local backend="$1"
  local case_name="$2"
  local cmd="$3"
  local expected="$4"
  local out

  out="$(capture_output "$cmd")"
  if [[ "$out" != "$expected" ]]; then
    fail_case "$backend" "$cmd" "$expected" "$out"
  fi
  pass_case "$backend" "$cmd"
}

expect_eventually_exact() {
  local backend="$1"
  local case_name="$2"
  local cmd="$3"
  local expected="$4"
  local timeout_secs="${5:-8}"
  local interval_secs="${6:-0.2}"
  local min_elapsed_secs="${7:-0}"
  local start_ts now_ts out

  start_ts="$(date +%s)"
  while true; do
    out="$(capture_output "$cmd")"
    now_ts="$(date +%s)"
    if [[ "$out" == "$expected" ]] && (( now_ts - start_ts >= min_elapsed_secs )); then
      pass_case "$backend" "$cmd"
      return 0
    fi
    if (( now_ts - start_ts >= timeout_secs )); then
      fail_case "$backend" "$cmd" "$expected" "$out"
    fi
    sleep "$interval_secs"
  done
}

expect_ttl_exact() {
  local backend="$1"
  local case_name="$2"
  local key="$3"
  local expected="$4"
  local out

  out="$(capture_output "TTL $key")"
  if [[ "$out" != "(integer) $expected" ]]; then
    fail_case "$backend" "TTL $key" "(integer) $expected" "$out"
  fi
  pass_case "$backend" "TTL $key"
}

expect_ttl_range() {
  local backend="$1"
  local case_name="$2"
  local key="$3"
  local min="$4"
  local max="$5"
  local out val

  out="$(capture_output "TTL $key")"
  if [[ "$out" =~ ^\(integer\)\ (-?[0-9]+)$ ]]; then
    val="${BASH_REMATCH[1]}"
  else
    fail_case "$backend" "TTL $key" "(integer) <n>" "$out"
  fi

  if (( val < min || val > max )); then
    fail_case "$backend" "TTL $key" "[$min, $max]" "$out"
  fi
  pass_case "$backend" "TTL $key"
}

expect_output_contains_and_not_contains() {
  local backend="$1"
  local case_name="$2"
  local cmd="$3"
  local must_contain_a="$4"
  local must_contain_b="$5"
  local forbidden="$6"
  local out

  out="$(capture_output "$cmd")"
  if [[ "$out" != *"$must_contain_a"* || "$out" != *"$must_contain_b"* || "$out" == *"$forbidden"* ]]; then
    fail_case "$backend" "$cmd" "contains $must_contain_a and $must_contain_b, excludes $forbidden" "$out"
  fi
  pass_case "$backend" "$cmd"
}

wait_for_server() {
  for _ in $(seq 1 100); do
    if capture_output "EXIST __ttl_rdb_probe__" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done

  printf '[FAIL][server] startup timeout\n' >&2
  cat "$SERVER_LOG" >&2 || true
  exit 1
}

start_server() {
  local backend="$1"
  local rdb_file="$TMP_DIR/${backend}.rdb"

  cat > "$CONFIG_FILE" <<EOF
role = master
port = $PORT
sync_port = $((PORT + 1000))
enable_aof = no
aof_fsync_mode = no
aof_filename = $TMP_DIR/kvstore.aof
rdb_filename = $rdb_file
dump_filename = $TMP_DIR/kvstore.dat
network_mode = reactor
mem_backend = pool
sync_mode = ebpf
max_value_size = 1048576
EOF

  "$KV_BIN" --config "$CONFIG_FILE" > "$SERVER_LOG" 2>&1 &
  SERVER_PID=$!
  wait_for_server
  sleep 1
}

stop_server() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
  fi
}

run_backend_suite() {
  local backend="$1"
  local write_cmd="$2"
  local read_cmd="$3"
  local del_cmd="$4"
  local mod_cmd="$5"
  local range_cmd="$6"
  local sort_cmd="$7"
  local prefix="$8"

  local persist_key="${prefix}persist"
  local survive_key="${prefix}survive"
  local overwrite_key="${prefix}overwrite"
  local delete_key="${prefix}delete"
  local pre_save_expired_key="${prefix}pre_save_expired"
  local load_expired_key="${prefix}load_expired"
  local range_key_a="${prefix}range_a"
  local range_key_b="${prefix}range_b"
  local range_key_c="${prefix}range_c"

  printf '[INFO][%s][rdb][ttl] running backend suite\n' "$backend"

  start_server "$backend"

  expect_eventually_exact "$backend" "persist-seed" "$write_cmd $persist_key keep" "OK"
  expect_eventually_exact "$backend" "survive-seed" "$write_cmd $survive_key live" "OK"
  expect_eventually_exact "$backend" "survive-expire" "EXPIRE $survive_key 60" "OK"
  expect_ttl_range "$backend" "survive-before-save" "$survive_key" 50 60

  expect_eventually_exact "$backend" "overwrite-seed" "$write_cmd $overwrite_key first" "OK"
  expect_eventually_exact "$backend" "overwrite-expire" "EXPIRE $overwrite_key 60" "OK"
  expect_eventually_exact "$backend" "overwrite-clear-ttl" "$mod_cmd $overwrite_key second" "OK"
  expect_ttl_exact "$backend" "overwrite-clear-ttl-check" "$overwrite_key" -1

  expect_eventually_exact "$backend" "delete-seed" "$write_cmd $delete_key gone" "OK"
  expect_eventually_exact "$backend" "delete-call" "$del_cmd $delete_key" "OK"
  expect_exact "$backend" "delete-missing-before-save" "$read_cmd $delete_key" "NO EXIST"

  expect_eventually_exact "$backend" "pre-save-expired-seed" "$write_cmd $pre_save_expired_key soon" "OK"
  expect_eventually_exact "$backend" "pre-save-expired-ttl" "EXPIRE $pre_save_expired_key 1" "OK"

  expect_eventually_exact "$backend" "load-expired-seed" "$write_cmd $load_expired_key late" "OK"
  expect_eventually_exact "$backend" "load-expired-ttl" "EXPIRE $load_expired_key 5" "OK"
  expect_ttl_range "$backend" "load-expired-before-save" "$load_expired_key" 3 5

  expect_eventually_exact "$backend" "range-seed-a" "$write_cmd $range_key_a 11" "OK"
  expect_eventually_exact "$backend" "range-seed-b" "$write_cmd $range_key_b 22" "OK"
  expect_eventually_exact "$backend" "range-seed-c" "$write_cmd $range_key_c 33" "OK"
  expect_eventually_exact "$backend" "range-expire-b" "EXPIRE $range_key_b 5" "OK"
  expect_ttl_range "$backend" "range-b-before-save" "$range_key_b" 3 5

  sleep 2

  expect_exact "$backend" "pre-save-expired-before-save" "$read_cmd $pre_save_expired_key" "NO EXIST"
  expect_exact "$backend" "delete-still-missing-before-save" "$read_cmd $delete_key" "NO EXIST"

  stop_server

  sleep 5
  start_server "$backend"

  expect_eventually_exact "$backend" "persist-after-reload" "$read_cmd $persist_key" "keep"
  expect_exact "$backend" "survive-after-reload" "$read_cmd $survive_key" "live"
  expect_ttl_range "$backend" "survive-after-reload-ttl" "$survive_key" 35 60

  expect_exact "$backend" "overwrite-after-reload" "$read_cmd $overwrite_key" "second"
  expect_ttl_exact "$backend" "overwrite-after-reload-ttl" "$overwrite_key" -1

  expect_exact "$backend" "delete-after-reload" "$read_cmd $delete_key" "NO EXIST"
  expect_exact "$backend" "pre-save-expired-after-reload" "$read_cmd $pre_save_expired_key" "NO EXIST"
  expect_exact "$backend" "load-expired-after-reload" "$read_cmd $load_expired_key" "NO EXIST"

  expect_exact "$backend" "range-a-after-reload" "$read_cmd $range_key_a" "11"
  expect_exact "$backend" "range-b-after-reload" "$read_cmd $range_key_b" "NO EXIST"
  expect_exact "$backend" "range-c-after-reload" "$read_cmd $range_key_c" "33"
  expect_output_contains_and_not_contains "$backend" "range-after-reload" "$range_cmd $range_key_a $range_key_c" "$range_key_a" "$range_key_c" "$range_key_b"
  expect_output_contains_and_not_contains "$backend" "sort-after-reload" "$sort_cmd low 10" "$range_key_a" "$range_key_c" "$range_key_b"

  stop_server

  printf '[PASS][%s][rdb][ttl] backend suite completed successfully\n' "$backend"
}

run_backend_suite "array" "SET" "GET" "DEL" "MOD" "RANGE" "SORT" "arr_ttl_"
run_backend_suite "rbtree" "RSET" "RGET" "RDEL" "RMOD" "RRANGE" "RSORT" "rbt_ttl_"
run_backend_suite "hash" "HSET" "HGET" "HDEL" "HMOD" "HRANGE" "HSORT" "hash_ttl_"
run_backend_suite "skiptable" "SSET" "SGET" "SDEL" "SMOD" "SRANGE" "SSORT" "skip_ttl_"

printf '[PASS][rdb][ttl] RDB save/load TTL suite completed successfully\n'