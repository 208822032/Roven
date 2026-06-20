#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"
CLIENT_BIN="${CLIENT_BIN:-./tests/kvstore_client}"
HOST="${KV_HOST:-127.0.0.1}"
if [[ -n "${AOF_TEST_PORT:-}" ]]; then
  PORT="$AOF_TEST_PORT"
else
  PORT="$(python3 -c 'import socket; sock = socket.socket(); sock.bind(("127.0.0.1", 0)); print(sock.getsockname()[1]); sock.close()')"
fi

TMP_DIR="$(mktemp -d)"
CONFIG_FILE="$TMP_DIR/kvstore_aof_ttl.conf"
SERVER_LOG="$TMP_DIR/kvstore.log"
AOF_FILE="$TMP_DIR/kvstore.aof"
AOF_REWRITE_PAYLOAD_FILE="$TMP_DIR/aof_rewrite_payload.bin"
AOF_REWRITE_PAYLOAD_MB="${AOF_REWRITE_PAYLOAD_MB:-1}"
AOF_REWRITE_ITERATIONS="${AOF_REWRITE_ITERATIONS:-70}"
AOF_REWRITE_MAX_FILE_SIZE="${AOF_REWRITE_MAX_FILE_SIZE:-5242880}"

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

capture_output_with_file() {
  python3 - "$HOST" "$PORT" "$1" "$2" "$3" <<'PY'
import pathlib
import shlex
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
cmd = sys.argv[3]
key = sys.argv[4]
value_file = pathlib.Path(sys.argv[5])

parts = shlex.split(cmd) + [key, value_file.read_text()]
request = f"*{len(parts)}\r\n" + "".join(f"${len(part)}\r\n{part}\r\n" for part in parts)

sock = socket.create_connection((host, port), timeout=3)
try:
  sock.sendall(request.encode())
  sock.settimeout(0.2)
  deadline = time.time() + 10
  chunks = []
  seen_data = False
  while time.time() < deadline:
    try:
      data = sock.recv(8192)
      if not data:
        break
      seen_data = True
      chunks.append(data)
    except socket.timeout:
      if seen_data:
        break
      continue
  sys.stdout.write(b"".join(chunks).decode("utf-8", "replace"))
finally:
  sock.close()
PY
}

capture_raw_output() {
  python3 - "$HOST" "$PORT" "$1" <<'PY'
import shlex
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
cmd = sys.argv[3]
parts = shlex.split(cmd)
request = f"*{len(parts)}\r\n" + "".join(f"${len(part)}\r\n{part}\r\n" for part in parts)

sock = socket.create_connection((host, port), timeout=3)
try:
  sock.sendall(request.encode())
  sock.settimeout(0.2)
  deadline = time.time() + 5
  chunks = []
  seen_data = False
  while time.time() < deadline:
    try:
      data = sock.recv(8192)
      if not data:
        break
      seen_data = True
      chunks.append(data)
    except socket.timeout:
      if seen_data:
        break
      continue
  sys.stdout.write(b"".join(chunks).decode("utf-8", "replace"))
finally:
  sock.close()
PY
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
  if [[ -f "$SERVER_LOG" ]]; then
    printf '[FAIL][server-log]\n' >&2
    tail -n 120 "$SERVER_LOG" >&2 || true
  fi
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
    fail_case "$backend" "$case_name" "response mismatch" "$cmd" "$expected" "$out"
  fi
  pass_case "$backend" "$case_name" "$cmd"
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
  local timeout_secs="${5:-8}"
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
    fail_case "$backend" "$case_name" "contains $must_contain_a and $must_contain_b, excludes $forbidden" "$cmd" "$out"
  fi
  pass_case "$backend" "$case_name" "$cmd"
}

expect_rewrite_aof_file_state() {
  local backend="$1"
  local case_name="$2"
  local file_path="$3"
  local forbidden_key_one="$4"
  local forbidden_key_two="$5"
  local forbidden_key_three="$6"
  local forbidden_key_four="$7"

  python3 - "$backend" "$case_name" "$file_path" "$forbidden_key_one" "$forbidden_key_two" "$forbidden_key_three" "$forbidden_key_four" <<'PY'
import pathlib
import sys

backend = sys.argv[1]
case_name = sys.argv[2]
file_path = pathlib.Path(sys.argv[3])
forbidden_keys = [arg.encode() for arg in sys.argv[4:] if arg]

data = file_path.read_bytes()
if b"$9\r\nPEXPIREAT\r\n" not in data:
    print(f"[FAIL][{backend}][{case_name}] reason=missing PEXPIREAT expected=RESP canonical TTL actual={file_path}", file=sys.stderr)
    sys.exit(1)

if b"$7\r\nEXPIRE\r\n" in data:
    print(f"[FAIL][{backend}][{case_name}] reason=raw EXPIRE present expected_absent=RESP EXPIRE actual={file_path}", file=sys.stderr)
    sys.exit(1)

for marker in forbidden_keys:
    if marker and marker in data:
        print(f"[FAIL][{backend}][{case_name}] reason=forbidden key present expected_absent={marker!r} actual={file_path}", file=sys.stderr)
        sys.exit(1)
PY

  pass_case "$backend" "$case_name" "$file_path"
}

wait_for_aof_rewrite() {
  local backend="$1"
  local file_path="$2"
  local old_inode="$3"
  local timeout_secs="${4:-60}"
  local interval_secs="${5:-0.5}"
  local start_ts now_ts inode size

  start_ts="$(date +%s)"
  while true; do
    if [[ -f "$file_path" ]]; then
      inode="$(stat -c '%i' "$file_path")"
      size="$(stat -c '%s' "$file_path")"
      if [[ "$inode" != "$old_inode" ]] && (( size <= AOF_REWRITE_MAX_FILE_SIZE )); then
        pass_case "$backend" "aof-rewrite" "$file_path"
        return 0
      fi
    fi

    now_ts="$(date +%s)"
    if (( now_ts - start_ts >= timeout_secs )); then
      fail_case "$backend" "aof-rewrite" "rewrite timeout" "$old_inode" "new inode and compacted file" "${file_path}"
    fi
    sleep "$interval_secs"
  done
}

wait_for_server() {
  for _ in $(seq 1 100); do
    if capture_output "EXIST __ttl_aof_probe__" >/dev/null 2>&1; then
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
  local aof_file="$TMP_DIR/${backend}.aof"

  cat > "$CONFIG_FILE" <<EOF
role = master
port = $PORT
sync_port = $((PORT + 1000))
enable_aof = yes
aof_fsync_mode = always
aof_filename = $aof_file
rdb_filename = $TMP_DIR/${backend}.rdb
dump_filename = $TMP_DIR/${backend}.dat
network_mode = reactor
mem_backend = pool
sync_mode = ebpf
max_value_size = 134217728
EOF

  "$KV_BIN" --config "$CONFIG_FILE" > "$SERVER_LOG" 2>&1 &
  SERVER_PID=$!
  wait_for_server
  sleep 0.5
}

stop_server() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
  fi
}

expect_aof_pexpireat() {
  local backend="$1"
  local aof_file="$2"

  if [[ ! -f "$aof_file" ]]; then
    fail_case "$backend" "aof-file" "missing AOF file" "$aof_file" "existing file" ""
  fi

  python3 - "$backend" "$aof_file" <<'PY'
import pathlib
import sys

backend = sys.argv[1]
aof_file = pathlib.Path(sys.argv[2])
data = aof_file.read_bytes()

if b"$9\r\nPEXPIREAT\r\n" not in data:
    print(f"[FAIL][{backend}][aof-pexpireat] reason=PEXPIREAT not found in AOF expected=contains PEXPIREAT actual={aof_file}", file=sys.stderr)
    sys.exit(1)

if b"$7\r\nEXPIRE\r\n" in data:
    print(f"[FAIL][{backend}][aof-canonicalization] reason=raw EXPIRE found in AOF expected=no raw EXPIRE actual={aof_file}", file=sys.stderr)
    sys.exit(1)
PY

  pass_case "$backend" "aof-canonicalization" "$aof_file"
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
  local aof_file="$TMP_DIR/${backend}.aof"

  local keep_key="${prefix}keep"
  local live_key="${prefix}live"
  local expired_key="${prefix}expired"
  local persist_key="${prefix}persist"
  local overwrite_key="${prefix}overwrite"
  local delete_key="${prefix}delete"
  local range_key_a="${prefix}range_a"
  local range_key_b="${prefix}range_b"
  local range_key_c="${prefix}range_c"
  local rewrite_probe_key="${prefix}rewrite_probe"
  local rewrite_expired_a_key="${prefix}rewrite_expired_a"
  local rewrite_expired_b_key="${prefix}rewrite_expired_b"

  printf '[INFO][%s][aof][ttl] running backend suite\n' "$backend"
  start_server "$backend"

  if [[ "$backend" == "array" ]]; then
    expect_exact "global" "missing-ttl" "TTL no_such_key" "(integer) -2"
    expect_exact "global" "missing-expire" "EXPIRE no_such_key 10" "NO EXIST"
    expect_exact "global" "missing-persist" "PERSIST no_such_key" "NO EXIST"
  fi

  expect_exact "$backend" "keep-set" "$write_cmd $keep_key keep" "OK"
  expect_exact "$backend" "keep-read" "$read_cmd $keep_key" "keep"

  expect_exact "$backend" "live-set" "$write_cmd $live_key live" "OK"
  expect_exact "$backend" "live-expire" "EXPIRE $live_key 120" "OK"
  expect_ttl_range "$backend" "live-ttl-before-reload" "$live_key" 100 120

  expect_exact "$backend" "persist-set" "$write_cmd $persist_key persist" "OK"
  expect_exact "$backend" "persist-expire" "EXPIRE $persist_key 120" "OK"
  expect_exact "$backend" "persist-call" "PERSIST $persist_key" "OK"
  expect_ttl_exact "$backend" "persist-ttl-before-reload" "$persist_key" -1

  expect_exact "$backend" "overwrite-set" "$write_cmd $overwrite_key first" "OK"
  expect_exact "$backend" "overwrite-expire" "EXPIRE $overwrite_key 120" "OK"
  expect_exact "$backend" "overwrite-clear" "$mod_cmd $overwrite_key second" "OK"
  expect_ttl_exact "$backend" "overwrite-ttl-before-reload" "$overwrite_key" -1

  expect_exact "$backend" "delete-set" "$write_cmd $delete_key doomed" "OK"
  expect_exact "$backend" "delete-expire" "EXPIRE $delete_key 120" "OK"
  expect_exact "$backend" "delete-call" "$del_cmd $delete_key" "OK"
  expect_exact "$backend" "delete-missing-live" "$read_cmd $delete_key" "NO EXIST"

  expect_exact "$backend" "expired-set" "$write_cmd $expired_key soon" "OK"
  expect_exact "$backend" "expired-expire" "EXPIRE $expired_key 1" "OK"
  if [[ "$backend" != "rbtree" ]]; then
    expect_eventually_exact "$backend" "expired-live-check" "$read_cmd $expired_key" "NO EXIST" 5 0.2 1
  fi

  expect_exact "$backend" "range-seed-a" "$write_cmd $range_key_a 11" "OK"
  expect_exact "$backend" "range-seed-b" "$write_cmd $range_key_b 22" "OK"
  expect_exact "$backend" "range-seed-c" "$write_cmd $range_key_c 33" "OK"
  expect_exact "$backend" "range-expire-b" "EXPIRE $range_key_b 1" "OK"

  sleep 2

  if [[ "$backend" != "rbtree" ]]; then
    expect_output_contains_and_not_contains "$backend" "range-live-check" "$range_cmd $range_key_a $range_key_c" "$range_key_a" "$range_key_c" "$range_key_b"
    expect_output_contains_and_not_contains "$backend" "sort-live-check" "$sort_cmd low 10" "$range_key_a" "$range_key_c" "$range_key_b"
  fi

  expect_exact "$backend" "expired-before-restart" "$read_cmd $expired_key" "NO EXIST"

  local rewrite_inode_before
  rewrite_inode_before="$(stat -c '%i' "$aof_file")"

  sleep 2

  expect_exact "$backend" "rewrite-probe" "$write_cmd $rewrite_probe_key 1" "OK"
  expect_exact "$backend" "rewrite-trigger" "AOFREWRITE" "OK"
  wait_for_aof_rewrite "$backend" "$aof_file" "$rewrite_inode_before" 180 0.5
  expect_rewrite_aof_file_state "$backend" "rewrite-file-check" "$aof_file" "$expired_key" "$range_key_b" "$delete_key" ""
  expect_exact "$backend" "rewrite-probe-after-rewrite" "$read_cmd $rewrite_probe_key" "1"

  stop_server
  expect_aof_pexpireat "$backend" "$aof_file"

  sleep 1
  start_server "$backend"

  expect_exact "$backend" "keep-after-reload" "$read_cmd $keep_key" "keep"
  expect_ttl_exact "$backend" "keep-after-reload-ttl" "$keep_key" -1

  expect_exact "$backend" "live-after-reload" "$read_cmd $live_key" "live"
  expect_ttl_range "$backend" "live-after-reload-ttl" "$live_key" 1 120

  expect_exact "$backend" "persist-after-reload" "$read_cmd $persist_key" "persist"
  expect_ttl_exact "$backend" "persist-after-reload-ttl" "$persist_key" -1

  expect_exact "$backend" "overwrite-after-reload" "$read_cmd $overwrite_key" "second"
  expect_ttl_exact "$backend" "overwrite-after-reload-ttl" "$overwrite_key" -1

  expect_exact "$backend" "delete-after-reload" "$read_cmd $delete_key" "NO EXIST"
  expect_exact "$backend" "expired-after-reload" "$read_cmd $expired_key" "NO EXIST"
  expect_exact "$backend" "range-a-after-reload" "$read_cmd $range_key_a" "11"
  expect_exact "$backend" "range-b-after-reload" "$read_cmd $range_key_b" "NO EXIST"
  expect_exact "$backend" "rewrite-probe-after-reload" "$read_cmd $rewrite_probe_key" "1"
  expect_output_contains_and_not_contains "$backend" "range-after-reload" "$range_cmd $keep_key $rewrite_probe_key" "$keep_key" "$rewrite_probe_key" "$range_key_b"
  expect_output_contains_and_not_contains "$backend" "sort-after-reload" "$sort_cmd low 10" "$keep_key" "$rewrite_probe_key" "$range_key_b"

  stop_server
  pass_case "$backend" "suite" "all cases passed"
}

run_backend_suite "array" "SET" "GET" "EXIST" "DEL" "MOD" "RANGE" "SORT" "aof_arr_"
run_backend_suite "rbtree" "RSET" "RGET" "REXIST" "RDEL" "RMOD" "RRANGE" "RSORT" "aof_rbt_"
run_backend_suite "hash" "HSET" "HGET" "HEXIST" "HDEL" "HMOD" "HRANGE" "HSORT" "aof_hash_"
run_backend_suite "skiptable" "SSET" "SGET" "SEXIST" "SDEL" "SMOD" "SRANGE" "SSORT" "aof_skip_"

printf '[PASS][global][aof][ttl] all backend suites completed successfully\n'
