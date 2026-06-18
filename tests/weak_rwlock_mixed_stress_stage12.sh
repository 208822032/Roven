#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

KV_BIN="${KV_BIN:-./kvstore}"
HOST="${KV_HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage12-weak-rwlock.$$}"
RUN_MODES="${RUN_MODES:-reactor proactor ntyco}"
CLIENT_LEVELS="${CLIENT_LEVELS:-4 8 16}"
REQUESTS_PER_CLIENT="${REQUESTS_PER_CLIENT:-30}"
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
    for _ in $(seq 1 100); do
      if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      kill -TERM "${SERVER_PID}" >/dev/null 2>&1 || true
      for _ in $(seq 1 50); do
        if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
          break
        fi
        sleep 0.1
      done
    fi
    if kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      echo "stage12 server shutdown hang pid=${SERVER_PID} port=${SERVER_PORT}" >&2
      kill -KILL "${SERVER_PID}" >/dev/null 2>&1 || true
      wait "${SERVER_PID}" >/dev/null 2>&1 || true
      SERVER_PID=""
      SERVER_PORT=""
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
  else
    echo "[INFO][stage12] keeping tmp dir: ${TMP_DIR}" >&2
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
deadline = time.time() + 30
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=2)
        s.sendall(enc(["SET", "stage12_ready_probe", "1"]))
        data = s.recv(64)
        s.close()
        if data.startswith(b"+OK"):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.1)
raise SystemExit("server did not become ready")
PY
}

run_socket_mix() {
  local port="$1"
  local clients="$2"
  local requests="$3"
  local out_file="$4"
  local deep_checks="$5"
  python3 - "${HOST}" "${port}" "${clients}" "${requests}" "${out_file}" "${deep_checks}" <<'PY'
import json
import socket
import statistics
import sys
import threading
import time

host = sys.argv[1]
port = int(sys.argv[2])
clients = int(sys.argv[3])
requests = int(sys.argv[4])
out_file = sys.argv[5]
deep_checks = int(sys.argv[6])

def encode(tokens):
    chunks = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        chunks += [f"${len(data)}\r\n".encode(), data, b"\r\n"]
    return b"".join(chunks)

class Client:
    def __init__(self, timeout=30):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(timeout)
        self.buf = b""
    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()
    def call(self, tokens):
        self.sock.sendall(encode(tokens))
        return self.read()
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
        if typ == b"-":
            return ("error", self.line().decode())
        if typ == b":":
            return ("integer", int(self.line()))
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
        raise RuntimeError(f"bad RESP type {typ!r}")

def require(cond, msg):
    if not cond:
        raise AssertionError(msg)

lat_us = []
errors = []
lock = threading.Lock()
start = time.monotonic()

def worker(idx):
    local_lat = []
    c = Client(timeout=60)
    try:
        for j in range(requests):
            key = f"s12_kv_c{clients}_{idx}_{j}"
            t0 = time.perf_counter_ns()
            set_resp = c.call(["SET", key, f"value-{idx}-{j}"])
            require(set_resp == ("simple", "OK"), f"SET {set_resp!r}")
            if j % 3 == 0:
                require(c.call(["GET", key]) == ("bulk", f"value-{idx}-{j}"), "GET")
            if j % 5 == 0:
                require(c.call(["EXPIRE", key, "2"]) == ("simple", "OK"), "EXPIRE")
                ttl = c.call(["TTL", key])
                require(ttl[0] == "integer" and ttl[1] >= 0, f"TTL {ttl}")
                require(c.call(["PERSIST", key]) == ("simple", "OK"), "PERSIST")
            if j % 7 == 0:
                require(c.call(["DEL", key]) in [("simple", "OK"), ("simple", "NO EXIST")], "DEL")
            t1 = time.perf_counter_ns()
            local_lat.append((t1 - t0) // 1000)
    except Exception as exc:
        with lock:
            errors.append(f"client={idx}: {exc!r}")
    finally:
        c.close()
    with lock:
        lat_us.extend(local_lat)

threads = [threading.Thread(target=worker, args=(i,)) for i in range(clients)]
for th in threads:
    th.start()
for th in threads:
    th.join()

if deep_checks:
    ctl = Client(timeout=60)
    try:
        for i in range(64):
            require(ctl.call(["SET", f"s12_expire_{i}", "x"]) == ("simple", "OK"), "expire SET")
            require(ctl.call(["EXPIRE", f"s12_expire_{i}", "1"]) == ("simple", "OK"), "expire EXPIRE")
        time.sleep(2.2)
        for i in range(64):
            require(ctl.call(["GET", f"s12_expire_{i}"]) == ("simple", "NO EXIST"), "expire GET")
        for i in range(16):
            q = f"s12 vector question {i}"
            a = f"s12 vector answer {i}"
            scope = f"s12_vector_{i}"
            require(ctl.call(["VSET", q, a, scope]) == ("simple", "OK"), "VSET")
            require(ctl.call(["VGET", q, scope]) == ("bulk", a), "VGET")
            resp = ctl.call(["VSEARCH", q, scope, "5", "0.0"])
            require(resp[0] == "array", f"VSEARCH {resp!r}")
            require(ctl.call(["VSET", q, a + " updated", scope]) == ("simple", "OK"), "repeat VSET")
            require(ctl.call(["VGET", q, scope]) == ("bulk", a + " updated"), "repeat VGET")
            require(ctl.call(["VDEL", q, scope]) in [("simple", "OK"), ("simple", "NO EXIST")], "VDEL")
            deleted = ctl.call(["VSEARCH", q, scope, "5", "0.0"])
            require(deleted[0] in ("array", "error") and (a + " updated") not in repr(deleted),
                    f"deleted vector leaked resp={deleted!r}")
        require(ctl.call(["AOFREWRITE"]) == ("simple", "OK"), "AOFREWRITE")
    finally:
        ctl.close()

elapsed = max(time.monotonic() - start, 0.001)
if errors:
    raise SystemExit("; ".join(errors))
lat_us.sort()
def pct(p):
    if not lat_us:
        return 0.0
    idx = min(len(lat_us) - 1, int(len(lat_us) * p / 100.0))
    return lat_us[idx] / 1000.0
result = {
    "clients": clients,
    "requests_per_client": requests,
    "ops": len(lat_us),
    "qps": len(lat_us) / elapsed,
    "p95_ms": pct(95),
    "p99_ms": pct(99),
    "avg_ms": (statistics.mean(lat_us) / 1000.0) if lat_us else 0.0,
}
with open(out_file, "w", encoding="utf-8") as f:
    json.dump(result, f, sort_keys=True)
    f.write("\n")
print("stage12 socket mix", json.dumps(result, sort_keys=True))
PY
}

check_log() {
  local log="$1"
  local weak="$2"
  if grep -E "SIGSEGV|segmentation fault|double free|heap-use-after-free|AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|data race|shutdown hang|failed to acquire COW snapshot" "${log}" >/dev/null 2>&1; then
    echo "stage12 crash/race signature found in ${log}" >&2
    exit 1
  fi
  if ! grep -q "owner rwlock stats:" "${log}"; then
    echo "stage12 missing owner rwlock stats in ${log}" >&2
    exit 1
  fi
  python3 - "${log}" "${weak}" <<'PY'
import re
import sys
log = sys.argv[1]
weak = int(sys.argv[2])
line = ""
with open(log, encoding="utf-8", errors="replace") as f:
    for current in f:
        if "owner rwlock stats:" in current:
            line = current.strip()
if not line:
    raise SystemExit("missing owner rwlock stats")
stats = {k: int(v) for k, v in re.findall(r"([a-z_]+)=([0-9]+)", line)}
if stats.get("total_calls", 0) <= 0:
    raise SystemExit(f"owner stats total_calls invalid: {line}")
if weak:
    if stats.get("weak_mode") != 1:
        raise SystemExit(f"weak mode stats not enabled: {line}")
    if stats.get("weak_noop_calls", 0) <= 0:
        raise SystemExit(f"weak mode did not no-op executor sites: {line}")
else:
    if stats.get("weak_mode") != 0:
        raise SystemExit(f"real mode stats unexpectedly weak: {line}")
    if stats.get("weak_noop_calls", 0) != 0:
        raise SystemExit(f"real mode had weak no-op calls: {line}")
print(f"stage12 owner stats ok weak={weak}: {line}")
PY
}

run_mode_once() {
  local label="$1"
  local weak="$2"
  local mode="$3"
  local mode_dir="${TMP_DIR}/${label}/${mode}"
  local conf="${mode_dir}/kvstore.conf"
  local log="${mode_dir}/kvstore.log"
  local port

  mkdir -p "${mode_dir}"
  port="$(pick_port)"
  SERVER_PORT="${port}"
  write_conf "${mode}" "${port}" "${conf}" "${mode_dir}"
  echo "stage12 mixed mode=${label} network=${mode}: start port=${port}"
  # Stage 13: default is weak-lock. real-lock rollback requires KVLITE_ROUTE_C_WEAK_RWLOCK=0.
  if [[ "${weak}" == "1" ]]; then
    KVLITE_ROUTE_C_WEAK_RWLOCK=1 KVLITE_EMBEDDING_PROFILE=1 "${KV_BIN}" --config "${conf}" >"${log}" 2>&1 &
  else
    KVLITE_ROUTE_C_WEAK_RWLOCK=0 KVLITE_EMBEDDING_PROFILE=1 "${KV_BIN}" --config "${conf}" >"${log}" 2>&1 &
  fi
  SERVER_PID=$!
  wait_for_ready "${port}"
  local client_array=(${CLIENT_LEVELS})
  local client_count="${#client_array[@]}"
  local client_idx=0
  for clients in "${client_array[@]}"; do
    local deep_checks=0
    client_idx=$((client_idx + 1))
    if [[ "${client_idx}" -eq "${client_count}" ]]; then
      deep_checks=1
    fi
    run_socket_mix "${port}" "${clients}" "${REQUESTS_PER_CLIENT}" "${mode_dir}/c${clients}.json" "${deep_checks}"
  done
  stop_server
  check_log "${log}" "${weak}"
  echo "stage12 mixed mode=${label} network=${mode}: ok log=${log}"
}

run_persistence_and_replication() {
  local label="$1"
  local weak="$2"
  echo "stage12 mixed mode=${label}: persistence/replication begin"
  (
    # Stage 13: default is weak-lock. real-lock rollback requires KVLITE_ROUTE_C_WEAK_RWLOCK=0.
    if [[ "${weak}" == "1" ]]; then
      export KVLITE_ROUTE_C_WEAK_RWLOCK=1
    else
      export KVLITE_ROUTE_C_WEAK_RWLOCK=0
    fi
    bash tests/rdb_snapshot_stage10_6.sh
    bash tests/aof_rewrite_snapshot_stage10_7.sh
    bash tests/master_full_sync_stage10_5.sh
    bash tests/replicated_command_task_stage10_3.sh
  )
  echo "stage12 mixed mode=${label}: persistence/replication ok"
}

run_suite() {
  local label="$1"
  local weak="$2"
  for mode in ${RUN_MODES}; do
    run_mode_once "${label}" "${weak}" "${mode}"
  done
  run_persistence_and_replication "${label}" "${weak}"
}

mkdir -p "${TMP_DIR}"
run_suite "real-lock" 0
run_suite "weak-lock" 1

echo "Stage 12 weak rwlock mixed stress passed tmp=${TMP_DIR}"
