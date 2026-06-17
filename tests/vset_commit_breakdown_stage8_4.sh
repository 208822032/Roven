#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage8-4-vset.$$}"
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
    for _ in $(seq 1 80); do
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
  local aof_enabled="$3"
  local port="$4"
  local conf="$5"
  local data_dir="$6"
  cat >"${conf}" <<EOF_CONF
role = master
port = ${port}
sync_port = $((port + 1000))
network_mode = ${mode}
enable_aof = ${aof_enabled}
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
  python3 - "${HOST}" "${port}" <<'PY'
import socket
import sys
import threading
import time

host = sys.argv[1]
port = int(sys.argv[2])

def enc(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode()
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)

class Client:
    def __init__(self, timeout=60):
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
    def pipeline(self, commands):
        for tokens in commands:
            self.s.sendall(enc(tokens))
        return [self.read() for _ in commands]

def require(cond, msg):
    if not cond:
        raise AssertionError(msg)

deadline = time.time() + 90
while time.time() < deadline:
    try:
        c = Client(timeout=2)
        try:
            if c.call(["SET", "stage8_4_ready", "1"]) == ("simple", "OK"):
                break
        finally:
            c.close()
    except OSError:
        time.sleep(0.2)
else:
    raise RuntimeError("server not ready")

c = Client()
try:
    for i in range(12):
        require(c.call(["VSET", f"stage8_4 insert {i}", f"answer {i}", "stage8_4_insert"]) == ("simple", "OK"), "insert VSET")
    for i in range(12):
        require(c.call(["VSET", f"stage8_4 insert {i}", f"updated answer {i}", "stage8_4_insert"]) == ("simple", "OK"), "repeat VSET update")
    require(c.call(["VGET", "stage8_4 insert 3", "stage8_4_insert"]) == ("bulk", "updated answer 3"), "repeat VSET visible")
    for i in range(8):
        require(c.call(["VSEARCH", f"stage8_4 insert {i}", "stage8_4_insert", "5", "0.0"])[0] == "array", "VSEARCH")
    mixed = []
    for i in range(12):
        mixed.append(["VSET", f"stage8_4 mixed {i}", f"mixed answer {i}", "stage8_4_mixed"])
        mixed.append(["VSEARCH", f"stage8_4 mixed {i}", "stage8_4_mixed", "5", "0.0"])
    responses = c.pipeline(mixed)
    require(all(r[0] in ("simple", "array") for r in responses), "mixed VSET/VSEARCH pipeline")
    for width in (10, 50):
        cmds = [["VSET", f"stage8_4 pipe {width} {i}", f"pipe answer {i}", f"stage8_4_pipe_{width}"] for i in range(width)]
        responses = c.pipeline(cmds)
        require(all(r == ("simple", "OK") for r in responses), f"pipeline {width}")
finally:
    c.close()

errors = []
barrier = threading.Barrier(9)

def worker(i):
    try:
        c = Client(timeout=90)
        try:
            barrier.wait()
            for j in range(8):
                q = f"stage8_4 concurrent {i} {j}"
                require(c.call(["VSET", q, f"concurrent answer {i}-{j}", "stage8_4_concurrent"]) == ("simple", "OK"), "concurrent VSET")
        finally:
            c.close()
    except Exception as exc:
        errors.append(str(exc))

threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
for t in threads:
    t.start()
barrier.wait()
for t in threads:
    t.join()
require(not errors, "; ".join(errors))
print("stage8.4 client suite: ok")
PY
}

extract_stat() {
  local prefix="$1"
  local key="$2"
  local file="$3"
  sed -n "s/.*${prefix}: //p" "${file}" | tail -n 1 | tr ' ' '\n' | sed -n "s/^${key}=//p"
}

require_stat_gt_zero() {
  local prefix="$1"
  local key="$2"
  local file="$3"
  local value
  value="$(extract_stat "${prefix}" "${key}" "${file}")"
  if [[ -z "${value}" || "${value}" -le 0 ]]; then
    echo "stats check failed prefix=${prefix} key=${key} value=${value:-} file=${file}" >&2
    tail -n 120 "${file}" >&2
    exit 1
  fi
}

mkdir -p "${TMP_DIR}"

for mode in reactor proactor; do
  for batch_enabled in 0 1; do
    for aof_enabled in no yes; do
      cleanup
      SERVER_PID=""
      SERVER_PORT=""
      port="$(pick_port)"
      data_dir="${TMP_DIR}/${mode}_batch_${batch_enabled}_aof_${aof_enabled}_data"
      rm -rf "${data_dir}"
      mkdir -p "${data_dir}"
      conf="${TMP_DIR}/${mode}_batch_${batch_enabled}_aof_${aof_enabled}.conf"
      log="${TMP_DIR}/${mode}_batch_${batch_enabled}_aof_${aof_enabled}.log"
      write_conf "${mode}" "${batch_enabled}" "${aof_enabled}" "${port}" "${conf}" "${data_dir}"

      "${ROOT_DIR}/kvstore" -c "${conf}" >"${log}" 2>&1 &
      SERVER_PID="$!"
      SERVER_PORT="${port}"
      run_client_suite "${port}"
      cleanup
      SERVER_PID=""
      SERVER_PORT=""

      require_stat_gt_zero "vset executor breakdown stats" "vset_prepared_tasks" "${log}"
      require_stat_gt_zero "vset executor breakdown stats" "vset_execute_sync_count" "${log}"
      require_stat_gt_zero "vset executor breakdown stats" "vset_post_commit_count" "${log}"
      require_stat_gt_zero "vset executor breakdown stats" "vset_finalize_response_count" "${log}"
      require_stat_gt_zero "vset executor breakdown stats" "vset_push_response_count" "${log}"
      require_stat_gt_zero "vset commit breakdown stats" "vset_commit_count" "${log}"
      require_stat_gt_zero "vset index breakdown stats" "vset_flat_index_update_count" "${log}"
      require_stat_gt_zero "vset index breakdown stats" "vset_hnsw_add_count" "${log}"
      require_stat_gt_zero "vset side-effect breakdown stats" "vset_lock_wait_count" "${log}"
      require_stat_gt_zero "vset side-effect breakdown stats" "vset_lock_hold_count" "${log}"
      require_stat_gt_zero "vset side-effect breakdown stats" "vset_response_build_count" "${log}"
      require_stat_gt_zero "vset side-effect breakdown stats" "vset_response_push_count" "${log}"
      require_stat_gt_zero "vset side-effect breakdown stats" "vsearch_search_count" "${log}"

      if [[ "${batch_enabled}" == "1" ]]; then
        require_stat_gt_zero "embedding batch stats" "batch_count" "${log}"
      fi
      if [[ "${aof_enabled}" == "yes" ]]; then
        require_stat_gt_zero "vset side-effect breakdown stats" "vset_aof_append_count" "${log}"
      fi

      echo "${mode} batch=${batch_enabled} aof=${aof_enabled}: vset stats ok"
    done
  done
done

echo "stage8.4 VSET commit breakdown correctness: ok tmp=${TMP_DIR}"
