#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage8-5-policy.$$}"
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
  local vset_enabled="$3"
  local vsearch_enabled="$4"
  local vset_size="$5"
  local vset_wait="$6"
  local vsearch_size="$7"
  local vsearch_wait="$8"
  local port="$9"
  local conf="${10}"
  local data_dir="${11}"
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
embedding_batch_vset_enabled = ${vset_enabled}
embedding_batch_vsearch_enabled = ${vsearch_enabled}
embedding_batch_vset_max_size = ${vset_size}
embedding_batch_vset_max_wait_us = ${vset_wait}
embedding_batch_vsearch_max_size = ${vsearch_size}
embedding_batch_vsearch_max_wait_us = ${vsearch_wait}
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
  local label="$2"
  python3 - "${HOST}" "${port}" "${label}" <<'PY'
import socket
import sys
import threading
import time

host = sys.argv[1]
port = int(sys.argv[2])
label = sys.argv[3]

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
            if c.call(["SET", "stage8_5_ready", "1"]) == ("simple", "OK"):
                break
        finally:
            c.close()
    except OSError:
        time.sleep(0.2)
else:
    raise RuntimeError("server not ready")

scope = f"stage8_5_{label}"
c = Client()
try:
    require(c.call(["VSET", "stage8_5 base question", "base answer", scope]) == ("simple", "OK"), "VSET")
    require(c.call(["VGET", "stage8_5 base question", scope]) == ("bulk", "base answer"), "VGET")
    require(c.call(["VSET", "stage8_5 base question", "updated answer", scope]) == ("simple", "OK"), "repeat VSET")
    require(c.call(["VGET", "stage8_5 base question", scope]) == ("bulk", "updated answer"), "repeat VSET visible")
    require(c.call(["VSEARCH", "stage8_5 base question", scope, "5", "0.0"])[0] == "array", "VSEARCH")
    require(c.call(["VSET"])[0] == "error", "invalid VSET")
    require(c.call(["VSEARCH"])[0] == "error", "invalid VSEARCH")

    for width in (10, 50):
        cmds = [["VSET", f"stage8_5 pipe {width} {i}", f"pipe answer {i}", scope] for i in range(width)]
        responses = c.pipeline(cmds)
        require(all(r == ("simple", "OK") for r in responses), f"pipeline VSET {width}")
        searches = [["VSEARCH", f"stage8_5 pipe {width} {i}", scope, "5", "0.0"] for i in range(width)]
        responses = c.pipeline(searches)
        require(all(r[0] == "array" for r in responses), f"pipeline VSEARCH {width}")
finally:
    c.close()

errors = []
barrier = threading.Barrier(9)

def worker(i):
    try:
        c = Client(timeout=90)
        try:
            barrier.wait()
            for j in range(6):
                q = f"stage8_5 concurrent {label} {i} {j}"
                require(c.call(["VSET", q, f"answer {i}-{j}", scope]) == ("simple", "OK"), "concurrent VSET")
                require(c.call(["VSEARCH", q, scope, "3", "0.0"])[0] == "array", "concurrent VSEARCH")
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
print("stage8.5 client suite: ok")
PY
}

extract_stat() {
  local prefix="$1"
  local key="$2"
  local file="$3"
  sed -n "s/.*${prefix}: //p" "${file}" | tail -n 1 | tr ' ' '\n' | sed -n "s/^${key}=//p" | tr -d '"'
}

require_eq() {
  local name="$1"
  local actual="$2"
  local expected="$3"
  local file="$4"
  if [[ -z "${actual}" || "${actual}" != "${expected}" ]]; then
    echo "stats check failed ${name}: actual=${actual:-} expected=${expected}" >&2
    tail -n 120 "${file}" >&2
    exit 1
  fi
}

require_gt() {
  local name="$1"
  local actual="$2"
  local threshold="$3"
  local file="$4"
  if [[ -z "${actual}" || "${actual}" -le "${threshold}" ]]; then
    echo "stats check failed ${name}: actual=${actual:-} threshold=${threshold}" >&2
    tail -n 120 "${file}" >&2
    exit 1
  fi
}

mkdir -p "${TMP_DIR}"

CASES=(
  "global_off 0 0 1 2 0 4 500 expect_none"
  "policy_vset_off_vsearch_on 1 0 1 2 0 4 500 expect_vsearch"
  "both_on 1 1 1 2 0 4 500 expect_both"
)

for mode in reactor proactor; do
  for case_def in "${CASES[@]}"; do
    cleanup
    SERVER_PID=""
    SERVER_PORT=""
    read -r label batch_enabled vset_enabled vsearch_enabled vset_size vset_wait vsearch_size vsearch_wait expectation <<<"${case_def}"
    port="$(pick_port)"
    data_dir="${TMP_DIR}/${mode}_${label}_data"
    rm -rf "${data_dir}"
    mkdir -p "${data_dir}"
    conf="${TMP_DIR}/${mode}_${label}.conf"
    log="${TMP_DIR}/${mode}_${label}.log"
    write_conf \
      "${mode}" \
      "${batch_enabled}" \
      "${vset_enabled}" \
      "${vsearch_enabled}" \
      "${vset_size}" \
      "${vset_wait}" \
      "${vsearch_size}" \
      "${vsearch_wait}" \
      "${port}" \
      "${conf}" \
      "${data_dir}"

    "${ROOT_DIR}/kvstore" -c "${conf}" >"${log}" 2>&1 &
    SERVER_PID="$!"
    SERVER_PORT="${port}"
    run_client_suite "${port}" "${mode}_${label}"
    cleanup
    SERVER_PID=""
    SERVER_PORT=""

    batch_count="$(extract_stat "embedding batch stats" "batch_count" "${log}")"
    vset_enabled_stat="$(extract_stat "embedding command batch stats" "embedding_batch_vset_enabled" "${log}")"
    vsearch_enabled_stat="$(extract_stat "embedding command batch stats" "embedding_batch_vsearch_enabled" "${log}")"
    vset_count="$(extract_stat "embedding command batch stats" "embedding_batch_vset_count" "${log}")"
    vsearch_count="$(extract_stat "embedding command batch stats" "embedding_batch_vsearch_count" "${log}")"
    vsearch_avg="$(extract_stat "embedding command batch stats" "embedding_batch_vsearch_avg_size" "${log}")"
    vset_avg="$(extract_stat "embedding command batch stats" "embedding_batch_vset_avg_size" "${log}")"
    vset_fallback="$(extract_stat "embedding command batch stats" "embedding_batch_vset_fallback_single_count" "${log}")"
    vsearch_fallback="$(extract_stat "embedding command batch stats" "embedding_batch_vsearch_fallback_single_count" "${log}")"

    if [[ "${expectation}" == "expect_none" ]]; then
      require_eq "global batch_count" "${batch_count}" "0" "${log}"
      require_eq "vset enabled" "${vset_enabled_stat}" "0" "${log}"
      require_eq "vsearch enabled" "${vsearch_enabled_stat}" "0" "${log}"
      require_eq "vset count" "${vset_count}" "0" "${log}"
      require_eq "vsearch count" "${vsearch_count}" "0" "${log}"
    elif [[ "${expectation}" == "expect_vsearch" ]]; then
      require_eq "vset enabled" "${vset_enabled_stat}" "0" "${log}"
      require_eq "vsearch enabled" "${vsearch_enabled_stat}" "1" "${log}"
      require_eq "vset count" "${vset_count}" "0" "${log}"
      require_gt "vsearch count" "${vsearch_count}" "0" "${log}"
      require_gt "vsearch avg size" "${vsearch_avg}" "1" "${log}"
      require_gt "vset fallback single" "${vset_fallback}" "0" "${log}"
    else
      require_eq "vset enabled" "${vset_enabled_stat}" "1" "${log}"
      require_eq "vsearch enabled" "${vsearch_enabled_stat}" "1" "${log}"
      require_gt "vset count" "${vset_count}" "0" "${log}"
      require_gt "vsearch count" "${vsearch_count}" "0" "${log}"
      require_gt "vset avg size" "${vset_avg}" "1" "${log}"
      require_gt "vsearch avg size" "${vsearch_avg}" "1" "${log}"
    fi
    require_gt "vsearch fallback single" "${vsearch_fallback}" "0" "${log}"
    echo "${mode} ${label}: policy stats ok"
  done
done

echo "stage8.5 embedding batch policy correctness: ok tmp=${TMP_DIR}"
