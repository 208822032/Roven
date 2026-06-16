#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
REQUESTS="${REQUESTS:-200}"
PREPARE_COUNT="${PREPARE_COUNT:-200}"
RESULT_DIR="${RESULT_DIR:-${ROOT_DIR}/results/stage6d}"
TEXT_MODE="${TEXT_MODE:-semantic}"
SERVER_PID=""

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
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

wait_ready() {
  local port="$1"
  python3 - "${HOST}" "${port}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
payload = b"*3\r\n$3\r\nSET\r\n$12\r\nstage6d_ping\r\n$1\r\n1\r\n"
deadline = time.time() + 10
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=1)
        s.sendall(payload)
        data = s.recv(16)
        s.close()
        if data.startswith(b"+OK"):
            sys.exit(0)
    except OSError:
        time.sleep(0.05)
raise SystemExit("server did not become ready")
PY
}

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
enable_aof = no
aof_fsync_mode = no
aof_filename = ${data_dir}/kvlite.aof
rdb_filename = ${data_dir}/kvlite.rdb
dump_filename = ${data_dir}/kvlite.dat
embedding_provider = hash
vector_index_type = hnsw
vector_metric = ip
vector_hnsw_m = 32
vector_hnsw_ef_construction = 128
vector_hnsw_ef_search = 128
vector_score_threshold = 0.0
log_level = info
log_async = no
EOF_CONF
}

run_bench_case() {
  local mode="$1"
  local port="$2"
  local bench_mode="$3"
  local op="$4"
  local clients="$5"
  local pipeline="$6"
  local outfile="$7"
  local prepare_args=()
  if [[ "${op}" == "vsearch" ]]; then
    prepare_args=(--prepare "${PREPARE_COUNT}")
  fi
  "${ROOT_DIR}/tests/vector_benchmark" \
    --mode "${bench_mode}" \
    -h "${HOST}" \
    -p "${port}" \
    -t "${op}" \
    -n "${REQUESTS}" \
    -c "${clients}" \
    -P "${pipeline}" \
    --scope "stage6d_${mode}_${op}" \
    --prefix "stage6d_${mode}_${op}" \
    --text-mode "${TEXT_MODE}" \
    --topk 5 \
    --threshold 0.0 \
    "${prepare_args[@]}" \
    >"${outfile}"
}

run_kv_setget_case() {
  local mode="$1"
  local port="$2"
  local clients="$3"
  local outfile="$4"
  python3 - "${HOST}" "${port}" "${REQUESTS}" "${clients}" "${mode}" >"${outfile}" <<'PY'
import socket
import statistics
import sys
import threading
import time

host = sys.argv[1]
port = int(sys.argv[2])
requests = int(sys.argv[3])
clients = int(sys.argv[4])
mode_name = sys.argv[5]


def encode(tokens):
    chunks = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode()
        chunks.append(f"${len(data)}\r\n".encode())
        chunks.append(data)
        chunks.append(b"\r\n")
    return b"".join(chunks)


class Client:
    def __init__(self):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
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
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk
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
            return ("bulk", data.decode())
        if typ == b":":
            return ("integer", int(self.line()))
        if typ == b"-":
            return ("error", self.line().decode())
        raise RuntimeError(f"bad RESP type {typ!r}")

    def call(self, tokens):
        self.sock.sendall(encode(tokens))
        return self.read()


latencies = []
lock = threading.Lock()
success = 0
failed = 0
errors = 0


def worker(worker_id, count):
    global success, failed, errors
    c = Client()
    local_lat = []
    local_success = 0
    local_failed = 0
    local_errors = 0
    try:
        for i in range(count):
            key = f"stage6d:{mode_name}:{worker_id}:{i}"
            value = f"value:{worker_id}:{i}"
            start = time.perf_counter_ns()
            try:
                set_resp = c.call(["SET", key, value])
                get_resp = c.call(["GET", key])
                elapsed_us = (time.perf_counter_ns() - start) // 1000
                if set_resp == ("simple", "OK") and get_resp == ("bulk", value):
                    local_success += 1
                    local_lat.append(elapsed_us)
                else:
                    local_failed += 1
            except Exception:
                local_errors += 1
    finally:
        c.close()
    with lock:
        success += local_success
        failed += local_failed
        errors += local_errors
        latencies.extend(local_lat)


base = requests // clients
extra = requests % clients
threads = []
start_time = time.perf_counter()
for idx in range(clients):
    count = base + (1 if idx < extra else 0)
    t = threading.Thread(target=worker, args=(idx, count))
    t.start()
    threads.append(t)
for t in threads:
    t.join()
elapsed = max(time.perf_counter() - start_time, 0.001)
latencies.sort()


def pct(p):
    if not latencies:
        return 0.0
    idx = min(len(latencies) - 1, int(len(latencies) * p / 100))
    return latencies[idx] / 1000.0


avg = (statistics.mean(latencies) / 1000.0) if latencies else 0.0
print("mode=throughput")
print("op=kv_setget")
print(f"requests={requests}")
print(f"clients={clients}")
print("pipeline=1")
print(f"success={success}")
print(f"failed={failed}")
print(f"errors={errors}")
print(f"elapsed_ms={elapsed * 1000:.0f}")
print(f"qps={success / elapsed:.2f}")
print(f"avg_ms={avg:.2f}")
print(f"p50_ms={pct(50):.2f}")
print(f"p95_ms={pct(95):.2f}")
print(f"p99_ms={pct(99):.2f}")
PY
}

mkdir -p "${RESULT_DIR}"

for mode in reactor proactor; do
  cleanup
  SERVER_PID=""

  port="$(pick_port)"
  data_dir="${RESULT_DIR}/${mode}_data"
  mkdir -p "${data_dir}"
  conf="${RESULT_DIR}/${mode}.conf"
  log="${RESULT_DIR}/${mode}.log"
  write_conf "${mode}" "${port}" "${conf}" "${data_dir}"

  "${ROOT_DIR}/kvstore" -c "${conf}" >"${log}" 2>&1 &
  SERVER_PID="$!"
  wait_ready "${port}"

  "${ROOT_DIR}/tests/vector_benchmark" \
    --mode latency \
    -h "${HOST}" \
    -p "${port}" \
    -t vset \
    -n "${PREPARE_COUNT}" \
    --scope "stage6d_prepare_${mode}" \
    --prefix "stage6d_prepare_${mode}" \
    --text-mode "${TEXT_MODE}" \
    >"${RESULT_DIR}/${mode}_prepare_vset.txt"

  run_bench_case "${mode}" "${port}" latency vset 1 1 "${RESULT_DIR}/${mode}_latency_vset.txt"
  run_bench_case "${mode}" "${port}" latency vsearch 1 1 "${RESULT_DIR}/${mode}_latency_vsearch.txt"

  for clients in 4 8 16; do
    run_bench_case "${mode}" "${port}" throughput vset "${clients}" 1 "${RESULT_DIR}/${mode}_throughput_vset_c${clients}.txt"
    run_bench_case "${mode}" "${port}" throughput vsearch "${clients}" 1 "${RESULT_DIR}/${mode}_throughput_vsearch_c${clients}.txt"
  done
  run_kv_setget_case "${mode}" "${port}" 4 "${RESULT_DIR}/${mode}_throughput_kv_setget_c4.txt"

  cleanup
  SERVER_PID=""
  echo "${mode}: results written to ${RESULT_DIR}, log=${log}"
done

python3 - "${RESULT_DIR}" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
rows = []
for path in sorted(root.glob("*.txt")):
    data = {}
    for line in path.read_text(errors="replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            data[k.strip()] = v.strip()
    rows.append((path.name, data))

print("file,mode,op,clients,pipeline,qps,avg_ms,p50_ms,p95_ms,p99_ms,success,failed,errors")
for name, data in rows:
    print(",".join([
        name,
        data.get("mode", ""),
        data.get("op", ""),
        data.get("clients", ""),
        data.get("pipeline", ""),
        data.get("qps", ""),
        data.get("avg_ms", ""),
        data.get("p50_ms", ""),
        data.get("p95_ms", ""),
        data.get("p99_ms", ""),
        data.get("success", ""),
        data.get("failed", ""),
        data.get("errors", ""),
    ]))
PY
