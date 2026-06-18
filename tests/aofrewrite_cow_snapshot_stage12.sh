#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

KV_BIN="${KV_BIN:-./kvstore}"
HOST="${KV_HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage12-aofrewrite.$$}"
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

stop_server() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    if [[ -n "${SERVER_PORT}" ]]; then
      python3 - "${HOST}" "${SERVER_PORT}" >/dev/null 2>&1 <<'PY' || true
import socket
import sys
host = sys.argv[1]
port = int(sys.argv[2])
payload = b"*1\r\n$4\r\nQUIT\r\n"
try:
    s = socket.create_connection((host, port), timeout=2)
    s.sendall(payload)
    try:
        s.recv(64)
    except OSError:
        pass
    s.close()
except OSError:
    pass
PY
    fi
    for _ in $(seq 1 80); do
      if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      kill -TERM "${SERVER_PID}" >/dev/null 2>&1 || true
      for _ in $(seq 1 40); do
        if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
          break
        fi
        sleep 0.1
      done
    fi
    if kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      kill -KILL "${SERVER_PID}" >/dev/null 2>&1 || true
      wait "${SERVER_PID}" >/dev/null 2>&1 || true
      echo "stage12 aofrewrite server did not stop cleanly" >&2
      exit 1
    fi
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
  SERVER_PID=""
  SERVER_PORT=""
}

cleanup() {
  stop_server
  if [[ -z "${KEEP_STAGE12_TMP:-}" ]]; then
    rm -rf "${TMP_DIR}"
  fi
}
trap cleanup EXIT

write_conf() {
  local mode="$1"
  local port="$2"
  local conf="$3"
  local data_dir="$4"
  cat >"${conf}" <<EOF_CONF
role = master
port = ${port}
sync_port = $((port + 1000))
network_mode = ${mode}
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
        data = str(token).encode()
        out += [f"${len(data)}\r\n".encode(), data, b"\r\n"]
    return b"".join(out)
deadline = time.time() + 20
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=2)
        s.sendall(enc(["SET", "stage12_aof_probe", "1"]))
        data = s.recv(64)
        s.close()
        if data.startswith(b"+OK"):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.1)
raise SystemExit("server did not become ready")
PY
}

run_case() {
  local mode="$1"
  local dir="${TMP_DIR}/${mode}"
  local conf="${dir}/kvstore.conf"
  local log="${dir}/kvstore.log"
  local port

  mkdir -p "${dir}"
  port="$(pick_port)"
  SERVER_PORT="${port}"
  write_conf "${mode}" "${port}" "${conf}" "${dir}"
  "${KV_BIN}" --config "${conf}" >"${log}" 2>&1 &
  SERVER_PID=$!
  wait_for_ready "${port}"

  python3 - "${HOST}" "${port}" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])

def enc(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        out += [f"${len(data)}\r\n".encode(), data, b"\r\n"]
    return b"".join(out)

class Client:
    def __init__(self):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(10)
        self.buf = b""
    def close(self):
        self.sock.close()
    def fill(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk
    def line(self):
        while b"\r\n" not in self.buf:
            self.fill(len(self.buf) + 1)
        idx = self.buf.index(b"\r\n")
        line = self.buf[:idx]
        self.buf = self.buf[idx + 2:]
        return line
    def read(self):
        self.fill(1)
        typ = self.buf[:1]
        self.buf = self.buf[1:]
        if typ == b"+":
            return ("simple", self.line().decode())
        if typ == b"$":
            n = int(self.line())
            if n < 0:
                return ("null", None)
            self.fill(n + 2)
            data = self.buf[:n]
            self.buf = self.buf[n + 2:]
            return ("bulk", data.decode("utf-8", "replace"))
        if typ == b"*":
            n = int(self.line())
            return ("array", [self.read() for _ in range(n)])
        if typ == b":":
            return ("integer", int(self.line()))
        if typ == b"-":
            return ("error", self.line().decode())
        raise RuntimeError(f"bad RESP type {typ!r}")
    def call(self, tokens):
        self.sock.sendall(enc(tokens))
        return self.read()

c = Client()
try:
    for i in range(64):
        assert c.call(["SET", f"stage12_aof_{i}", "x" * 32]) == ("simple", "OK")
    for i in range(8):
        assert c.call(["VSET", f"stage12 question {i}", f"stage12 answer {i}", "stage12_scope"]) == ("simple", "OK")
    assert c.call(["AOFREWRITE"]) == ("simple", "OK")
    assert c.call(["AOFREWRITE"]) == ("simple", "OK")
    assert c.call(["GET", "stage12_aof_10"]) == ("bulk", "x" * 32)
finally:
    c.close()
PY

  stop_server
  if grep -E "failed to acquire COW snapshot|AOF rewrite failed" "${log}" >/dev/null 2>&1; then
    echo "stage12 aofrewrite failure signature found in ${log}" >&2
    exit 1
  fi
  echo "stage12 aofrewrite mode=${mode}: ok"
}

mkdir -p "${TMP_DIR}"
run_case reactor
run_case proactor

echo "stage12 aofrewrite cow snapshot regression: ok"
