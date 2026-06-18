#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

KV_BIN="${KV_BIN:-./kvstore}"
HOST="${KV_HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage12-ntyco.$$}"
SERVER_PID=""
SERVER_PORT=""

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
    kill -KILL "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -z "${KEEP_STAGE12_TMP:-}" ]]; then
    rm -rf "${TMP_DIR}"
  fi
}
trap cleanup EXIT

write_conf() {
  local port="$1"
  local conf="$2"
  local data_dir="$3"
  cat >"${conf}" <<EOF_CONF
role = master
port = ${port}
sync_port = $((port + 1000))
network_mode = ntyco
enable_aof = yes
aof_fsync_mode = no
aof_filename = ${data_dir}/stage12.aof
rdb_filename = ${data_dir}/stage12.rdb
dump_filename = ${data_dir}/stage12.dat
mem_backend = pool
sync_mode = tcp
log_level = info
log_async = no
embedding_provider = hash
embedding_dim = 64
embedding_num_threads = 1
embedding_max_text_len = 512
embedding_worker_pool_enabled = no
vector_index_type = hnsw
vector_metric = ip
vector_score_threshold = 0.0
vector_hnsw_m = 16
vector_hnsw_ef_construction = 64
vector_hnsw_ef_search = 64
EOF_CONF
}

wait_for_ready() {
  local port="$1"
  python3 - "${HOST}" "${port}" <<'PY'
import socket
import sys
import time
host = sys.argv[1]
port = int(sys.argv[2])

def enc(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        out += [f"${len(data)}\r\n".encode(), data, b"\r\n"]
    return b"".join(out)

payload = enc(["SET", "stage12_ntyco_probe", "1"])
deadline = time.time() + 20
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=2)
        s.sendall(payload)
        data = s.recv(64)
        s.close()
        if data.startswith(b"+OK"):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.1)
raise SystemExit("server did not become ready")
PY
}

mkdir -p "${TMP_DIR}"
SERVER_PORT="$(pick_port)"
CONF="${TMP_DIR}/kvstore.conf"
LOG="${TMP_DIR}/kvstore.log"
write_conf "${SERVER_PORT}" "${CONF}" "${TMP_DIR}"
"${KV_BIN}" --config "${CONF}" >"${LOG}" 2>&1 &
SERVER_PID=$!
wait_for_ready "${SERVER_PORT}"

python3 - "${HOST}" "${SERVER_PORT}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])

def enc(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        out += [f"${len(data)}\r\n".encode(), data, b"\r\n"]
    return b"".join(out)

idle = socket.create_connection((host, port), timeout=5)
idle.settimeout(5)
quit_sock = socket.create_connection((host, port), timeout=5)
quit_sock.settimeout(5)
quit_sock.sendall(enc(["QUIT"]))
try:
    quit_sock.recv(128)
except OSError:
    pass
quit_sock.close()
time.sleep(0.5)
idle.close()
PY

for _ in $(seq 1 80); do
  if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

if kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
  echo "stage12 ntyco shutdown did not exit cleanly" >&2
  exit 1
fi
wait "${SERVER_PID}" >/dev/null 2>&1 || true
SERVER_PID=""

if ! grep -q "NtyCo network exited" "${LOG}"; then
  echo "stage12 ntyco shutdown missing clean exit log" >&2
  exit 1
fi

echo "stage12 ntyco mixed shutdown regression: ok"
