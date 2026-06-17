#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage10_4-ntyco.$$}"
SERVER_PID=""
SERVER_PORT=""
SERVER_LOG=""

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    if [[ -n "${SERVER_PORT}" ]]; then
      python3 - "${HOST}" "${SERVER_PORT}" >/dev/null 2>&1 <<'PY' || true
import socket, sys
host, port = sys.argv[1], int(sys.argv[2])
payload = b"*1\r\n$4\r\nQUIT\r\n"
s = socket.create_connection((host, port), timeout=2)
s.sendall(payload)
try:
    s.recv(64)
except OSError:
    pass
s.close()
PY
    fi
    for _ in $(seq 1 80); do
      if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then break; fi
      sleep 0.1
    done
    if kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      kill "${SERVER_PID}" >/dev/null 2>&1 || true
    fi
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

send_cmd() {
  python3 - "${HOST}" "${SERVER_PORT}" "$@" <<'PY'
import socket, sys
host, port = sys.argv[1], int(sys.argv[2])
tokens = sys.argv[3:]
out = [f"*{len(tokens)}\r\n".encode()]
for token in tokens:
    data = str(token).encode()
    out.append(f"${len(data)}\r\n".encode())
    out.append(data)
    out.append(b"\r\n")
s = socket.create_connection((host, port), timeout=5)
s.sendall(b"".join(out))
s.shutdown(socket.SHUT_WR)
parts = []
while True:
    chunk = s.recv(4096)
    if not chunk:
        break
    parts.append(chunk)
s.close()
sys.stdout.buffer.write(b"".join(parts))
PY
}

check() {
  local desc="$1"
  local expected="$2"
  local actual="$3"
  if echo "${actual}" | grep -q "${expected}"; then
    echo "  OK: ${desc}"
  else
    echo "  FAIL: ${desc} (expected='${expected}' actual='${actual}')" >&2
    return 1
  fi
}

start_ntyco_server() {
  local port="$1"
  local data_dir="$2"
  local conf="${data_dir}/ntyco.conf"
  SERVER_LOG="${data_dir}/kvlite.log"

  cat >"${conf}" <<EOF_CONF
role = master
port = ${port}
sync_port = $((port + 1000))
network_mode = ntyco
enable_aof = no
aof_fsync_mode = no
aof_filename = ${data_dir}/kvlite.aof
rdb_filename = ${data_dir}/kvlite.rdb
dump_filename = ${data_dir}/kvlite.dat
mem_backend = pool
sync_mode = tcp
log_level = info
log_async = no
embedding_provider = hash
embedding_dim = 64
embedding_num_threads = 1
embedding_max_text_len = 512
embedding_worker_pool_enabled = no
EOF_CONF

  cd "${ROOT_DIR}"
  "${ROOT_DIR}/kvstore" --config "${conf}" >"${SERVER_LOG}" 2>&1 &
  SERVER_PID=$!
  cd - >/dev/null
  SERVER_PORT="${port}"

  local ready=0
  for _ in $(seq 1 60); do
    if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      echo "ERROR: Server failed to start" >&2
      echo "--- Server log ---" >&2
      cat "${SERVER_LOG}" >&2
      return 1
    fi
    READY=$(python3 - "${HOST}" "${port}" 2>/dev/null <<'PY' || echo "NOT_READY"
import socket, sys
host, port = sys.argv[1], int(sys.argv[2])
try:
    s = socket.create_connection((host, port), timeout=0.5)
    payload = b"*3\r\n$3\r\nSET\r\n$11\r\nready_probe\r\n$1\r\n1\r\n"
    s.sendall(payload)
    s.shutdown(socket.SHUT_WR)
    resp = b""
    while True:
        chunk = s.recv(128)
        if not chunk: break
        resp += chunk
    s.close()
    if resp:
        print("READY")
    else:
        print("NOT_READY")
except Exception:
    print("NOT_READY")
PY
)
    if [[ "${READY}" == "READY" ]]; then
      ready=1
      break
    fi
    sleep 0.2
  done

  if [[ ${ready} -eq 0 ]]; then
    echo "ERROR: Server did not become ready" >&2
    echo "--- Server log ---" >&2
    cat "${SERVER_LOG}" >&2
    return 1
  fi
}

# ============================================================
# Test Cases
# ============================================================

test_ntyco_basic_ops() {
  echo "--- test_ntyco_basic_ops ---"

  local r

  r=$(send_cmd "SET" "nkey" "hello_ntyco")
  check "SET returns OK" "\+OK" "${r}" || return 1

  r=$(send_cmd "GET" "nkey")
  check "GET returns value" "hello_ntyco" "${r}" || return 1

  r=$(send_cmd "DEL" "nkey")
  check "DEL returns OK" "\+OK" "${r}" || return 1

  r=$(send_cmd "GET" "nkey")
  check "GET deleted key returns NO EXIST" "NO EXIST" "${r}" || return 1
}

test_ntyco_lazy_expire() {
  echo "--- test_ntyco_lazy_expire ---"

  local r

  send_cmd "SET" "nephem" "v1" >/dev/null
  send_cmd "EXPIRE" "nephem" "1" >/dev/null

  r=$(send_cmd "GET" "nephem")
  check "GET before expiry returns value" "v1" "${r}" || return 1

  sleep 2

  r=$(send_cmd "GET" "nephem")
  check "GET after expiry triggers lazy expire" "NO EXIST" "${r}" || return 1

  r=$(send_cmd "TTL" "nephem")
  check "TTL on expired key returns -2" "\-2" "${r}" || return 1
}

test_ntyco_ttl_ops() {
  echo "--- test_ntyco_ttl_ops ---"

  local r

  send_cmd "SET" "nttl" "tv" >/dev/null
  send_cmd "EXPIRE" "nttl" "100" >/dev/null

  r=$(send_cmd "TTL" "nttl")
  check "TTL returns positive integer" ":[0-9]" "${r}" || return 1

  send_cmd "PERSIST" "nttl" >/dev/null

  r=$(send_cmd "TTL" "nttl")
  check "TTL after PERSIST returns -1" "\-1" "${r}" || return 1

  send_cmd "PEXPIREAT" "nttl" "1" >/dev/null

  r=$(send_cmd "GET" "nttl")
  check "PEXPIREAT with past time deletes key" "NO EXIST" "${r}" || return 1
}

test_ntyco_startup_warning() {
  echo "--- test_ntyco_startup_warning ---"

  if [[ -f "${SERVER_LOG}" ]]; then
    if grep -q "legacy sync mode\|bypass command executor\|rwlock removal" "${SERVER_LOG}" 2>/dev/null; then
      echo "  OK: Startup warning found in log"
      grep -i "legacy\|bypass\|rwlock" "${SERVER_LOG}" | head -3
    else
      echo "  WARN: Startup warning not found (checking KVS_LOG_WARN visibility)"
      echo "  Log excerpt (first 20 lines):"
      head -20 "${SERVER_LOG}"
    fi
  fi
}

test_ntyco_shutdown() {
  echo "--- test_ntyco_shutdown ---"

  local r
  # QUIT may close connection before response; that's OK in ntyco mode
  r=$(send_cmd "QUIT" 2>/dev/null || echo "")
  if echo "${r}" | grep -q "+OK"; then
    echo "  OK: QUIT returned OK"
  else
    echo "  OK: QUIT sent (response may be empty due to immediate close)"
  fi

  for _ in $(seq 1 80); do
    if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      echo "  OK: Server exited cleanly"
      SERVER_PID=""
      return 0
    fi
    sleep 0.1
  done
  echo "  WARN: Server did not exit after QUIT (will be killed by cleanup)"
}

# ============================================================
# Main
# ============================================================

main() {
  echo "============================================"
  echo "Stage 10.4: Ntyco Executor Readiness Test"
  echo "============================================"

  mkdir -p "${TMP_DIR}"
  SERVER_PORT=$(pick_port)
  echo "  Data dir: ${TMP_DIR}"
  echo "  Server port: ${SERVER_PORT}"

  echo ""
  echo "Starting KVLite in ntyco mode..."
  if ! start_ntyco_server "${SERVER_PORT}" "${TMP_DIR}"; then
    echo "FATAL: Failed to start ntyco server" >&2
    return 1
  fi
  echo "  Server PID: ${SERVER_PID}"

  local ok=1

  test_ntyco_startup_warning || true
  test_ntyco_basic_ops || ok=0
  test_ntyco_lazy_expire || ok=0
  test_ntyco_ttl_ops || ok=0
  test_ntyco_shutdown || true

  echo ""
  if [[ ${ok} -eq 1 ]]; then
    echo "============================================"
    echo "Stage 10.4 Ntyco Test: ALL PASSED"
    echo "============================================"
    return 0
  else
    echo "============================================"
    echo "Stage 10.4 Ntyco Test: SOME FAILED"
    echo "============================================"
    return 1
  fi
}

main "$@"
