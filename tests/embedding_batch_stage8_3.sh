#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage8-3-batch.$$}"
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
    if [[ -n "${SERVER_PORT}" ]]; then
      python3 - "${HOST}" "${SERVER_PORT}" >/dev/null 2>&1 <<'PY' || true
import socket
import sys
host = sys.argv[1]
port = int(sys.argv[2])
s = socket.create_connection((host, port), timeout=2)
s.sendall(b"*1\r\n$4\r\nQUIT\r\n")
try:
    s.recv(64)
except OSError:
    pass
s.close()
PY
    fi
    for _ in $(seq 1 50); do
      if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      kill "${SERVER_PID}" >/dev/null 2>&1 || true
    fi
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

write_conf() {
  local mode="$1"
  local batch_enabled="$2"
  local port="$3"
  local conf="$4"
  local data_dir="$5"
  cat >"${conf}" <<EOF_CONF
role = master
port = ${port}
sync_port = $((port + 1000))
network_mode = ${mode}
enable_aof = no
aof_fsync_mode = no
aof_filename = ${data_dir}/kvlite.aof
rdb_filename = ${data_dir}/kvlite.rdb
dump_filename = ${data_dir}/kvlite.dat
mem_backend = pool
sync_mode = tcp
log_level = info
log_async = no
embedding_provider = local_c
embedding_model_path = models/bge-small-zh-v1.5/model.onnx
embedding_dim = 512
embedding_num_threads = 1
embedding_max_text_len = 512
embedding_ort_graph_opt_level = basic
embedding_inter_threads = 0
embedding_ort_execution_mode = sequential
embedding_worker_pool_enabled = 1
embedding_worker_threads = 4
embedding_worker_queue_capacity = 1024
embedding_per_thread_runtime_enabled = 1
embedding_per_thread_runtime_fallback_mutex = 1
embedding_shared_ort_env_enabled = 1
embedding_batch_enabled = ${batch_enabled}
embedding_batch_max_size = 4
embedding_batch_max_wait_us = 500
vector_index_type = hnsw
vector_metric = ip
vector_score_threshold = 0.0
vector_hnsw_m = 32
vector_hnsw_ef_construction = 128
vector_hnsw_ef_search = 128
vector_scope_overfetch_factor = 4
EOF_CONF
}

run_client_suite() {
  local port="$1"
  local batch_enabled="$2"
  python3 - "${HOST}" "${port}" "${batch_enabled}" <<'PY'
import socket
import sys
import threading
import time

host = sys.argv[1]
port = int(sys.argv[2])
batch_enabled = int(sys.argv[3])

def enc(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode()
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)

class Client:
    def __init__(self, timeout=30):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.settimeout(timeout)
        self.buf = b""
    def close(self):
        self.s.close()
    def fill(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("closed")
            self.buf += chunk
    def line(self):
        while b"\r\n" not in self.buf:
            self.fill(len(self.buf) + 1)
        i = self.buf.index(b"\r\n")
        line = self.buf[:i]
        self.buf = self.buf[i + 2:]
        return line
    def read(self):
        self.fill(1)
        typ = self.buf[:1]
        self.buf = self.buf[1:]
        if typ in (b"+", b"-"):
            return ("simple" if typ == b"+" else "error", self.line().decode())
        if typ == b":":
            return ("integer", int(self.line()))
        if typ == b"$":
            n = int(self.line())
            if n < 0:
                return ("null", None)
            self.fill(n + 2)
            data = self.buf[:n]
            self.buf = self.buf[n + 2:]
            return ("bulk", data.decode())
        if typ == b"*":
            n = int(self.line())
            return ("array", [self.read() for _ in range(n)])
        raise RuntimeError(f"bad type {typ!r}")
    def call(self, tokens):
        self.s.sendall(enc(tokens))
        return self.read()

def require(cond, msg):
    if not cond:
        raise AssertionError(msg)

deadline = time.time() + 60
while time.time() < deadline:
    try:
        c = Client(timeout=2)
        try:
            if c.call(["SET", "stage8_3_ready", "1"]) == ("simple", "OK"):
                break
        finally:
            c.close()
    except OSError:
        time.sleep(0.2)
else:
    raise RuntimeError("server not ready")

c = Client()
try:
    require(c.call(["VSET", "怎么重置密码", "可以在登录页点击忘记密码", "stage8_3"]) == ("simple", "OK"), "VSET")
    require(c.call(["VGET", "怎么重置密码", "stage8_3"]) == ("bulk", "可以在登录页点击忘记密码"), "VGET")
    require(c.call(["VSEARCH", "忘记密码怎么办", "stage8_3", "5", "0.0"])[0] == "array", "VSEARCH")
    require(c.call(["VSET", "怎么重置密码", "新的密码答案", "stage8_3"]) == ("simple", "OK"), "repeat VSET")
    require(c.call(["VDEL", "怎么重置密码", "stage8_3"]) in [("simple", "OK"), ("simple", "NO EXIST")], "VDEL")
    require(c.call(["VSET"]) [0] == "error", "invalid VSET")
    require(c.call(["VSEARCH"]) [0] == "error", "invalid VSEARCH")
finally:
    c.close()

errors = []
barrier = threading.Barrier(17)

def worker(i):
    try:
        c = Client(timeout=60)
        try:
            barrier.wait()
            q = f"stage8_3 batch question {batch_enabled} {i}"
            a = f"stage8_3 batch answer {i}"
            require(c.call(["VSET", q, a, "stage8_3_batch"]) == ("simple", "OK"), f"concurrent VSET {i}")
            require(c.call(["VSEARCH", q, "stage8_3_batch", "3", "0.0"])[0] == "array", f"concurrent VSEARCH {i}")
        finally:
            c.close()
    except Exception as exc:
        errors.append(str(exc))

threads = [threading.Thread(target=worker, args=(i,)) for i in range(16)]
for t in threads:
    t.start()
barrier.wait()
for t in threads:
    t.join()
require(not errors, "; ".join(errors))
print("stage8.3 client suite: ok")
PY
}

extract_stat() {
  local key="$1"
  local file="$2"
  sed -n 's/.*embedding batch stats: //p' "${file}" | tail -n 1 | tr ' ' '\n' | sed -n "s/^${key}=//p"
}

mkdir -p "${TMP_DIR}"

for mode in reactor proactor; do
  for batch_enabled in 0 1; do
    cleanup
    SERVER_PID=""
    SERVER_PORT=""
    port="$(pick_port)"
    data_dir="${TMP_DIR}/${mode}_batch_${batch_enabled}_data"
    rm -rf "${data_dir}"
    mkdir -p "${data_dir}"
    conf="${TMP_DIR}/${mode}_batch_${batch_enabled}.conf"
    log="${TMP_DIR}/${mode}_batch_${batch_enabled}.log"
    write_conf "${mode}" "${batch_enabled}" "${port}" "${conf}" "${data_dir}"

    "${ROOT_DIR}/kvstore" -c "${conf}" >"${log}" 2>&1 &
    SERVER_PID="$!"
    SERVER_PORT="${port}"
    run_client_suite "${port}" "${batch_enabled}"
    bash "${ROOT_DIR}/tests/semantic_onnx_test.sh" "${HOST}" "${port}" >/dev/null
    cleanup
    SERVER_PID=""
    SERVER_PORT=""

    if [[ "${batch_enabled}" == "1" ]]; then
      count="$(extract_stat batch_count "${log}")"
      avg_size="$(extract_stat batch_avg_size "${log}")"
      max_size="$(extract_stat batch_max_observed_size "${log}")"
      if [[ -z "${count}" || -z "${avg_size}" || "${count}" -le 0 || "${avg_size}" -le 1 || "${max_size}" -le 1 ]]; then
        echo "batch stats check failed mode=${mode} count=${count:-} avg_size=${avg_size:-} max_size=${max_size:-}" >&2
        tail -n 80 "${log}" >&2
        exit 1
      fi
      echo "${mode} batch enabled: batch_count=${count} avg_size=${avg_size} max_size=${max_size}"
    else
      echo "${mode} batch disabled: ok"
    fi
  done
done

echo "stage8.3 embedding batch correctness: ok tmp=${TMP_DIR}"
