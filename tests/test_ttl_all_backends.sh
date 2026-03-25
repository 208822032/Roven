#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"
CLIENT_BIN="${CLIENT_BIN:-./tests/kvstore_client}"
HOST="${KV_HOST:-127.0.0.1}"
PORT="${TTL_TEST_PORT:-19331}"

TMP_DIR="$(mktemp -d)"
SERVER_LOG="$TMP_DIR/kvstore.log"
CONFIG_FILE="$TMP_DIR/kvstore_ttl.conf"

declare -A SET_OVERWRITE_SUPPORT=(
  [array]=1
  [rbtree]=1
  [hash]=1
  [skiptable]=1
)

declare -A SET_OVERWRITE_REASON=(
  [array]="supported and verified on SET overwrite path"
  [rbtree]="supported and directly verified on RSET overwrite path"
  [hash]="supported and directly verified on HSET overwrite path"
  [skiptable]="supported and verified on SSET overwrite path"
)

declare -A RANGE_SORT_TTL_SUPPORT=(
  [array]=0
  [rbtree]=0
  [hash]=0
  [skiptable]=0
)

declare -A RANGE_SORT_TTL_REASON=(
  [array]="TTL filtering is not wired into RANGE/SORT yet"
  [rbtree]="TTL filtering is not wired into RANGE/SORT yet"
  [hash]="TTL filtering is not wired into RANGE/SORT yet"
  [skiptable]="TTL filtering is not wired into RANGE/SORT yet"
)

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

normalize_output() {
  python3 -c 'import re,sys; data=sys.stdin.read(); data=re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", data); data=data.replace("\r", " ").replace("\n", " "); data=re.sub(r"[ \t]+", " ", data).strip(); sys.stdout.write(data)'
}

run_client() {
  "$CLIENT_BIN" "$HOST" "$PORT" -c "$1"
}

capture_output() {
  run_client "$1" | normalize_output
}

pass_case() {
  printf '[PASS][%s][%s] cmd=%s\n' "$1" "$2" "$3"
}

skip_case() {
  printf '[SKIP][%s][%s] reason=%s\n' "$1" "$2" "$3"
}

fail_case() {
  printf '[FAIL][%s][%s] reason=%s' "$1" "$2" "$3" >&2
  if [[ -n "${4:-}" ]]; then
    printf ' expected=%s' "$4" >&2
  fi
  if [[ -n "${5:-}" ]]; then
    printf ' actual=%s' "$5" >&2
  fi
  if [[ -n "${6:-}" ]]; then
    printf ' cmd=%s' "$6" >&2
  fi
  printf '\n' >&2
  exit 1
}

backend_supports_set_overwrite() {
  [[ "${SET_OVERWRITE_SUPPORT[$1]:-0}" == "1" ]]
}

backend_supports_range_sort_ttl() {
  [[ "${RANGE_SORT_TTL_SUPPORT[$1]:-0}" == "1" ]]
}

print_backend_capabilities() {
  local backend="$1"
  local set_overwrite_state="unsupported"
  local range_sort_ttl_state="unsupported"

  if backend_supports_set_overwrite "$backend"; then
    set_overwrite_state="supported"
  fi

  if backend_supports_range_sort_ttl "$backend"; then
    range_sort_ttl_state="supported"
  else
    range_sort_ttl_state="smoke-only:${RANGE_SORT_TTL_REASON[$backend]}"
  fi

  printf '[INFO][%s] capability set-overwrite=%s; range-sort-ttl=%s\n' \
    "$backend" "$set_overwrite_state" "$range_sort_ttl_state"
}

expect_exact() {
  local backend="$1"
  local case_name="$2"
  local cmd="$3"
  local expected="$4"
  local out

  out="$(capture_output "$cmd")"
  if [[ "$out" != "$expected" ]]; then
    fail_case "$backend" "$case_name" "response mismatch" "$cmd" "$expected" "$out"
  fi
  pass_case "$backend" "$case_name" "$cmd"
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
    fail_case "$backend" "$case_name" "TTL output not integer" "TTL $key" "(integer) <n>" "$out"
  fi

  if (( val < min || val > max )); then
    fail_case "$backend" "$case_name" "TTL value out of range" "TTL $key" "[$min, $max]" "$out"
  fi
  pass_case "$backend" "$case_name" "TTL $key"
}

expect_ttl_exact() {
  local backend="$1"
  local case_name="$2"
  local key="$3"
  local expected="$4"
  local out

  out="$(capture_output "TTL $key")"
  if [[ "$out" != "(integer) $expected" ]]; then
    fail_case "$backend" "$case_name" "TTL mismatch" "TTL $key" "(integer) $expected" "$out"
  fi
  pass_case "$backend" "$case_name" "TTL $key"
}

expect_eventually_exact() {
  local backend="$1"
  local case_name="$2"
  local cmd="$3"
  local expected="$4"
  local timeout_secs="${5:-5}"
  local interval_secs="${6:-0.2}"
  local min_elapsed_secs="${7:-0}"
  local start_ts now_ts out

  start_ts="$(date +%s)"
  while true; do
    out="$(capture_output "$cmd")"
    now_ts="$(date +%s)"
    if [[ "$out" == "$expected" ]] && (( now_ts - start_ts >= min_elapsed_secs )); then
      pass_case "$backend" "$case_name" "$cmd"
      return 0
    fi
    if (( now_ts - start_ts >= timeout_secs )); then
      fail_case "$backend" "$case_name" "eventually exact timeout" "$expected" "$out" "$cmd"
    fi
    sleep "$interval_secs"
  done
}

expect_eventually_ttl_exact() {
  local backend="$1"
  local case_name="$2"
  local key="$3"
  local expected="$4"
  local timeout_secs="${5:-5}"
  local interval_secs="${6:-0.2}"
  local min_elapsed_secs="${7:-0}"
  local cmd="TTL $key"
  local out start_ts now_ts

  start_ts="$(date +%s)"
  while true; do
    out="$(capture_output "$cmd")"
    now_ts="$(date +%s)"
    if [[ "$out" == "(integer) $expected" ]] && (( now_ts - start_ts >= min_elapsed_secs )); then
      pass_case "$backend" "$case_name" "$cmd"
      return 0
    fi
    if (( now_ts - start_ts >= timeout_secs )); then
      fail_case "$backend" "$case_name" "TTL mismatch" "(integer) $expected" "$out" "$cmd"
    fi
    sleep "$interval_secs"
  done
}

expect_not_error() {
  local backend="$1"
  local case_name="$2"
  local cmd="$3"
  local out

  out="$(capture_output "$cmd")"
  if [[ -z "$out" || "$out" == "(error)"* ]]; then
    fail_case "$backend" "$case_name" "unexpected error or empty response" "$cmd" "non-error response" "$out"
  fi
  pass_case "$backend" "$case_name" "$cmd"
}

wait_for_server() {
  local i
  for i in $(seq 1 100); do
    if run_client "EXIST __ttl_ready_probe__" >/dev/null 2>&1; then
      printf '[INFO][server][startup] service is ready\n'
      return 0
    fi
    sleep 0.1
  done

  printf '[FAIL][server][startup] kvstore did not become ready\n' >&2
  cat "$SERVER_LOG" >&2 || true
  exit 1
}

start_server() {
  cat > "$CONFIG_FILE" <<EOF
role = master
port = $PORT
sync_port = $((PORT + 1000))
enable_aof = no
aof_fsync_mode = no
aof_filename = $TMP_DIR/kvstore.aof
rdb_filename = $TMP_DIR/kvstore.rdb
dump_filename = $TMP_DIR/kvstore.dat
network_mode = reactor
mem_backend = pool
sync_mode = ebpf
max_value_size = 1048576
EOF

  "$KV_BIN" --config "$CONFIG_FILE" > "$SERVER_LOG" 2>&1 &
  SERVER_PID=$!
  wait_for_server
}

run_backend_suite() {
  local backend="$1"
  local write_cmd="$2"
  local read_cmd="$3"
  local exist_cmd="$4"
  local del_cmd="$5"
  local mod_cmd="$6"
  local range_cmd="$7"
  local sort_cmd="$8"
  local prefix="$9"

  local basic_key="${prefix}basic"
  local lazy_get_key="${prefix}lazy_get"
  local lazy_exist_key="${prefix}lazy_exist"
  local persist_key="${prefix}persist"
  local overwrite_set_key="${prefix}overwrite_set"
  local overwrite_mod_key="${prefix}overwrite_mod"
  local delete_key="${prefix}delete"
  local range_key_a="${prefix}range_a"
  local range_key_b="${prefix}range_b"

  printf '[INFO][%s] running TTL suite\n' "$backend"
  print_backend_capabilities "$backend"

  expect_exact "$backend" "basic-set" "$write_cmd $basic_key 1" "OK"
  expect_exact "$backend" "basic-expire" "EXPIRE $basic_key 1" "OK"
  expect_ttl_range "$backend" "basic-ttl" "$basic_key" 0 1
  expect_exact "$backend" "basic-get-before-expire" "$read_cmd $basic_key" "1"
  expect_eventually_exact "$backend" "basic-get-after-expire" "$read_cmd $basic_key" "NO EXIST" 5 0.2 1
  expect_eventually_exact "$backend" "basic-exist-after-expire" "$exist_cmd $basic_key" "NO EXIST" 5 0.2 1
  expect_eventually_ttl_exact "$backend" "basic-ttl-after-expire" "$basic_key" -2 5 0.2 1

  expect_exact "$backend" "lazy-get-set" "$write_cmd $lazy_get_key 7" "OK"
  expect_exact "$backend" "lazy-get-expire" "EXPIRE $lazy_get_key 1" "OK"
  expect_eventually_exact "$backend" "lazy-get-trigger" "$read_cmd $lazy_get_key" "NO EXIST" 5 0.2 1
  expect_eventually_ttl_exact "$backend" "lazy-get-ttl" "$lazy_get_key" -2 5 0.2 1

  expect_exact "$backend" "lazy-exist-set" "$write_cmd $lazy_exist_key 8" "OK"
  expect_exact "$backend" "lazy-exist-expire" "EXPIRE $lazy_exist_key 1" "OK"
  expect_eventually_exact "$backend" "lazy-exist-trigger" "$exist_cmd $lazy_exist_key" "NO EXIST" 5 0.2 1
  expect_eventually_ttl_exact "$backend" "lazy-exist-ttl" "$lazy_exist_key" -2 5 0.2 1

  expect_exact "$backend" "persist-set" "$write_cmd $persist_key 2" "OK"
  expect_exact "$backend" "persist-expire" "EXPIRE $persist_key 1" "OK"
  expect_exact "$backend" "persist-call" "PERSIST $persist_key" "OK"
  expect_ttl_exact "$backend" "persist-ttl" "$persist_key" -1
  expect_eventually_exact "$backend" "persist-read-after-window" "$read_cmd $persist_key" "2" 5 0.2 2

  expect_exact "$backend" "mod-clear-seed" "$write_cmd $overwrite_mod_key 4" "OK"
  expect_exact "$backend" "mod-clear-expire" "EXPIRE $overwrite_mod_key 1" "OK"
  expect_exact "$backend" "mod-clear-call" "$mod_cmd $overwrite_mod_key 44" "OK"
  expect_ttl_exact "$backend" "mod-clear-ttl" "$overwrite_mod_key" -1
  expect_eventually_exact "$backend" "mod-clear-survive" "$read_cmd $overwrite_mod_key" "44" 5 0.2 2

  if backend_supports_set_overwrite "$backend"; then
    expect_exact "$backend" "set-overwrite-seed" "$write_cmd $overwrite_set_key 3" "OK"
    expect_exact "$backend" "set-overwrite-expire" "EXPIRE $overwrite_set_key 1" "OK"
    expect_exact "$backend" "set-overwrite-call" "$write_cmd $overwrite_set_key 33" "EXIST"
    expect_ttl_exact "$backend" "set-overwrite-ttl" "$overwrite_set_key" -1
    expect_eventually_exact "$backend" "set-overwrite-survive" "$read_cmd $overwrite_set_key" "33" 5 0.2 2
  else
    skip_case "$backend" "set-overwrite" "${SET_OVERWRITE_REASON[$backend]}"
  fi

  expect_exact "$backend" "delete-seed" "$write_cmd $delete_key 5" "OK"
  expect_exact "$backend" "delete-expire" "EXPIRE $delete_key 1" "OK"
  expect_exact "$backend" "delete-call" "$del_cmd $delete_key" "OK"
  expect_exact "$backend" "delete-read" "$read_cmd $delete_key" "NO EXIST"
  expect_ttl_exact "$backend" "delete-ttl" "$delete_key" -2

  expect_exact "$backend" "range-seed-a" "$write_cmd $range_key_a 11" "OK"
  expect_exact "$backend" "range-seed-b" "$write_cmd $range_key_b 22" "OK"
  expect_not_error "$backend" "range-smoke" "$range_cmd $range_key_a $range_key_b"
  expect_not_error "$backend" "sort-smoke" "$sort_cmd low 10"
  skip_case "$backend" "range-ttl" "${RANGE_SORT_TTL_REASON[$backend]}"
  expect_exact "$backend" "range-cleanup-a" "$del_cmd $range_key_a" "OK"
  expect_exact "$backend" "range-cleanup-b" "$del_cmd $range_key_b" "OK"

  pass_case "$backend" "suite" "all cases passed"
}

start_server

expect_exact "global" "missing-ttl" "TTL no_such_key" "(integer) -2"
expect_exact "global" "missing-expire" "EXPIRE no_such_key 10" "NO EXIST"
expect_exact "global" "missing-persist" "PERSIST no_such_key" "NO EXIST"

run_backend_suite "array" "SET" "GET" "EXIST" "DEL" "MOD" "RANGE" "SORT" "arr_ttl_"
run_backend_suite "rbtree" "RSET" "RGET" "REXIST" "RDEL" "RMOD" "RRANGE" "RSORT" "rbt_ttl_"
run_backend_suite "hash" "HSET" "HGET" "HEXIST" "HDEL" "HMOD" "HRANGE" "HSORT" "hash_ttl_"
run_backend_suite "skiptable" "SSET" "SGET" "SEXIST" "SDEL" "SMOD" "SRANGE" "SSORT" "skip_ttl_"

printf '[PASS][ttl][all-backends] TTL matrix suite completed successfully\n'
