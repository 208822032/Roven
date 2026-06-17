#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage10_3-repl.$$}"
KV_BIN="${KV_BIN:-./kvstore}"
MASTER_PID=""
SLAVE_PID=""

pick_ports() {
  python3 - <<'PY'
import socket
ports = []
for _ in range(3):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    ports.append(s.getsockname()[1])
    s.close()
print(ports[0], ports[1], ports[2])
PY
}

cleanup() {
  for pid_var in MASTER_PID SLAVE_PID; do
    local pid="${!pid_var}"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1; then
      kill -TERM "${pid}" >/dev/null 2>&1 || true
      for _ in $(seq 1 80); do
        if ! kill -0 "${pid}" >/dev/null 2>&1; then break; fi
        sleep 0.1
      done
      if kill -0 "${pid}" >/dev/null 2>&1; then
        kill -9 "${pid}" >/dev/null 2>&1 || true
      fi
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  done
}
trap cleanup EXIT

write_master_conf() {
  local port="$1"
  local sync_port="$2"
  local conf="$3"
  local data_dir="$4"
  cat >"${conf}" <<EOF_CONF
role = master
port = ${port}
sync_port = ${sync_port}
enable_aof = no
aof_fsync_mode = no
aof_filename = ${data_dir}/master.aof
rdb_filename = ${data_dir}/master.rdb
dump_filename = ${data_dir}/master.dat
network_mode = reactor
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
embedding_worker_threads = 2
embedding_prepare_queue_capacity = 1024
embedding_worker_queue_capacity = 1024
embedding_per_thread_runtime_enabled = 0
embedding_batch_enabled = 0
vector_index_type = hnsw
vector_metric = ip
vector_score_threshold = 0.75
vector_hnsw_m = 32
vector_hnsw_ef_construction = 256
vector_hnsw_ef_search = 256
vector_scope_overfetch_factor = 4
EOF_CONF
}

write_slave_conf() {
  local port="$1"
  local master_port="$2"
  local sync_port="$3"
  local conf="$4"
  local data_dir="$5"
  cat >"${conf}" <<EOF_CONF
role = slave
port = ${port}
master_ip = 127.0.0.1
master_port = ${master_port}
sync_port = ${sync_port}
enable_aof = no
aof_fsync_mode = no
aof_filename = ${data_dir}/slave.aof
rdb_filename = ${data_dir}/slave.rdb
dump_filename = ${data_dir}/slave.dat
network_mode = reactor
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
embedding_worker_threads = 2
embedding_prepare_queue_capacity = 1024
embedding_worker_queue_capacity = 1024
embedding_per_thread_runtime_enabled = 0
embedding_batch_enabled = 0
vector_index_type = hnsw
vector_metric = ip
vector_score_threshold = 0.75
vector_hnsw_m = 32
vector_hnsw_ef_construction = 256
vector_hnsw_ef_search = 256
vector_scope_overfetch_factor = 4
EOF_CONF
}

wait_for_port() {
  local port="$1"
  local probe_cmd="${2:-SET}"
  local probe_key="${3:-stage10_3_probe}"
  python3 - "${HOST}" "${port}" "${probe_cmd}" "${probe_key}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
probe_cmd = sys.argv[3]
probe_key = sys.argv[4]

def encode(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode()
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)

deadline = time.time() + 40
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=2)
        if probe_cmd == "SET":
            s.sendall(encode([probe_cmd, probe_key, "1"]))
            data = s.recv(32)
            s.close()
            if data.startswith(b"+OK") or data.startswith(b"+EXIST"):
                raise SystemExit(0)
        elif probe_cmd == "EXIST":
            s.sendall(encode([probe_cmd, probe_key]))
            data = s.recv(32)
            s.close()
            # EXIST returns "+EXIST\r\n" or "+NO EXIST\r\n"
            if data.startswith(b"+EXIST") or data.startswith(b"+NO"):
                raise SystemExit(0)
        else:
            s.sendall(encode([probe_cmd, probe_key, "1"]))
            data = s.recv(32)
            s.close()
            if data.startswith(b"+OK") or data.startswith(b"+EXIST"):
                raise SystemExit(0)
    except OSError:
        time.sleep(0.2)
raise SystemExit("server did not become ready")
PY
}

run_master_slave_suite() {
  local master_port="$1"
  local slave_port="$2"
  python3 - "${HOST}" "${master_port}" "${slave_port}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
master_port = int(sys.argv[2])
slave_port = int(sys.argv[3])

def encode(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)

class Client:
    def __init__(self, port, timeout=30):
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

    def send(self, tokens):
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

mc = Client(master_port, timeout=60)
try:
    sc = Client(slave_port, timeout=60)
    try:
        # Test 1: Basic SET replication
        require(mc.send(["SET", "s10_3_basic", "hello"]) == ("simple", "OK"), "master SET failed")
        time.sleep(0.5)
        require(sc.send(["GET", "s10_3_basic"]) == ("bulk", "hello"), "slave GET mismatch")

        # Test 2: DELETE replication
        require(mc.send(["DEL", "s10_3_basic"]) == ("simple", "OK"), "master DEL failed")
        time.sleep(0.5)
        require(sc.send(["GET", "s10_3_basic"]) == ("simple", "NO EXIST"), "slave DEL mismatch")

        # Test 3: TTL replication (EXPIRE)
        require(mc.send(["SET", "s10_3_ttl", "ttl_val"]) == ("simple", "OK"), "master TTL SET failed")
        require(mc.send(["EXPIRE", "s10_3_ttl", "30"]) == ("simple", "OK"), "master EXPIRE failed")
        time.sleep(0.5)
        ttl = sc.send(["TTL", "s10_3_ttl"])
        require(ttl[0] == "integer" and ttl[1] > 0 and ttl[1] <= 30, f"slave TTL mismatch: {ttl}")

        # Test 4: PERSIST replication
        require(mc.send(["SET", "s10_3_persist", "persist_val"]) == ("simple", "OK"), "master PERSIST SET failed")
        require(mc.send(["EXPIRE", "s10_3_persist", "10"]) == ("simple", "OK"), "master EXPIRE failed")
        time.sleep(0.3)
        require(mc.send(["PERSIST", "s10_3_persist"]) == ("simple", "OK"), "master PERSIST failed")
        time.sleep(0.5)
        ttl = sc.send(["TTL", "s10_3_persist"])
        require(ttl == ("integer", -1), f"slave PERSIST mismatch: {ttl}")

        # Test 5: MOD replication
        require(mc.send(["SET", "s10_3_mod", "old_val"]) == ("simple", "OK"), "master MOD SET failed")
        require(mc.send(["MOD", "s10_3_mod", "new_val"]) == ("simple", "OK"), "master MOD failed")
        time.sleep(0.5)
        require(sc.send(["GET", "s10_3_mod"]) == ("bulk", "new_val"), "slave MOD mismatch")

        # Test 6: Sequential ordering (MSG order preserved)
        for i in range(10):
            mc.send(["SET", f"s10_3_order_{i}", str(i)])
        time.sleep(1.0)
        for i in range(10):
            require(sc.send(["GET", f"s10_3_order_{i}"]) == ("bulk", str(i)), f"slave order {i} mismatch")

        # Test 7: Interleaved operations
        mc.send(["SET", "s10_3_inter_a", "1"])
        mc.send(["SET", "s10_3_inter_b", "2"])
        mc.send(["DEL", "s10_3_inter_a"])
        mc.send(["SET", "s10_3_inter_c", "3"])
        time.sleep(0.5)
        require(sc.send(["GET", "s10_3_inter_a"]) == ("simple", "NO EXIST"), "interleaved DEL mismatch")
        require(sc.send(["GET", "s10_3_inter_b"]) == ("bulk", "2"), "interleaved b mismatch")
        require(sc.send(["GET", "s10_3_inter_c"]) == ("bulk", "3"), "interleaved c mismatch")

        print("stage10_3 replication suite: ok")
    finally:
        sc.close()
finally:
    mc.close()
PY
}

verify_repl_stats() {
  local log="$1"
  python3 - "${log}" <<'PY'
import re
import sys

log = sys.argv[1]
repl = ""
ttl = ""
worker = ""
for line in open(log, errors="replace"):
    if "replicated executor stats:" in line:
        repl = line
    if "ttl executor stats:" in line:
        ttl = line
    if "embedding worker pool stats:" in line:
        worker = line
if not repl:
    raise SystemExit("missing replicated executor stats")

def fields(line):
    return {k: int(v) for k, v in re.findall(r"([A-Za-z0-9_]+)=([0-9]+)", line)}

r = fields(repl)
submitted = r.get("repl_task_submitted", 0)
executed = r.get("repl_task_executed", 0)
failed = r.get("repl_task_failed", 0)
dropped = r.get("repl_task_dropped", 0)
if submitted <= 0:
    raise SystemExit(f"repl submitted not positive: {submitted}")
if submitted != executed + failed + dropped:
    raise SystemExit(f"bad repl accounting submitted={submitted} executed={executed} failed={failed} dropped={dropped}")
if r.get("repl_queue_depth", -1) != 0:
    raise SystemExit(f"repl queue not drained: {r.get('repl_queue_depth')}")
if r.get("repl_set_count", 0) <= 0:
    raise SystemExit("repl SET count not positive")
if ttl:
    t = fields(ttl)
    if t.get("ttl_expire_queue_depth", -1) != 0:
        raise SystemExit(f"ttl queue not drained: {t.get('ttl_expire_queue_depth')}")
if worker:
    w = fields(worker)
    if w.get("enabled", 0) == 1 and w.get("queue_depth", -1) != 0:
        raise SystemExit(f"worker queue not drained: {w.get('queue_depth')}")
print("repl stats: ok")
PY
}

verify_slave_stats() {
  local log="$1"
  python3 - "${log}" <<'PY'
import re
import sys

log = sys.argv[1]
repl = ""
for line in open(log, errors="replace"):
    if "replicated executor stats:" in line:
        repl = line
if not repl:
    raise SystemExit("slave missing replicated executor stats")

def fields(line):
    return {k: int(v) for k, v in re.findall(r"([A-Za-z0-9_]+)=([0-9]+)", line)}

r = fields(repl)
submitted = r.get("repl_task_submitted", 0)
executed = r.get("repl_task_executed", 0)
failed = r.get("repl_task_failed", 0)
dropped = r.get("repl_task_dropped", 0)
if submitted <= 0:
    raise SystemExit(f"slave repl submitted not positive: {submitted}")
if submitted != executed + failed + dropped:
    raise SystemExit(f"bad slave repl accounting submitted={submitted} executed={executed} failed={failed} dropped={dropped}")
if r.get("repl_queue_depth", -1) != 0:
    raise SystemExit(f"slave repl queue not drained: {r.get('repl_queue_depth')}")
print("slave repl stats: ok")
PY
}

mkdir -p "${TMP_DIR}"

# Test: Master-Slave replication with replicated_command_task
read -r MASTER_PORT SLAVE_PORT SYNC_PORT < <(pick_ports)
master_data="${TMP_DIR}/master_data"
slave_data="${TMP_DIR}/slave_data"
master_conf="${TMP_DIR}/master.conf"
slave_conf="${TMP_DIR}/slave.conf"
master_log="${TMP_DIR}/master.log"
slave_log="${TMP_DIR}/slave.log"
mkdir -p "${master_data}" "${slave_data}"

write_master_conf "${MASTER_PORT}" "${SYNC_PORT}" "${master_conf}" "${master_data}"
write_slave_conf "${SLAVE_PORT}" "${MASTER_PORT}" "${SYNC_PORT}" "${slave_conf}" "${slave_data}"

# Start master first, wait for it, then start slave
# (slave needs master's sync server to be listening)
(cd "${ROOT_DIR}" && "${KV_BIN}" -c "${master_conf}" >"${master_log}" 2>&1) &
MASTER_PID="$!"
wait_for_port "${MASTER_PORT}"
sleep 0.5

(cd "${ROOT_DIR}" && "${KV_BIN}" -c "${slave_conf}" >"${slave_log}" 2>&1) &
SLAVE_PID="$!"
wait_for_port "${SLAVE_PORT}" "EXIST"

# Warm up connection
python3 - "${HOST}" "${MASTER_PORT}" <<'PY' >/dev/null 2>&1
import socket, sys, time
host, port = sys.argv[1], int(sys.argv[2])
def encode(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for t in tokens:
        d = str(t).encode()
        out.append(f"${len(d)}\r\n".encode())
        out.append(d)
        out.append(b"\r\n")
    return b"".join(out)
s = socket.create_connection((host, port), timeout=10)
s.sendall(encode(["SET", "__warmup__", "1"]))
s.recv(32)
s.close()
PY
sleep 1

# Run the core master-slave test suite
run_master_slave_suite "${MASTER_PORT}" "${SLAVE_PORT}"

# Send QUIT for clean shutdown to both servers
python3 - "${HOST}" "${SLAVE_PORT}" <<'PY' >/dev/null 2>&1 || true
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=5)
s.sendall(b"*1\r\n$4\r\nQUIT\r\n")
try: s.recv(64)
except OSError: pass
s.close()
PY

python3 - "${HOST}" "${MASTER_PORT}" <<'PY' >/dev/null 2>&1 || true
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=5)
s.sendall(b"*1\r\n$4\r\nQUIT\r\n")
try: s.recv(64)
except OSError: pass
s.close()
PY

# Wait for clean exit
for pid_spec in "SLAVE_PID:${SLAVE_PID}" "MASTER_PID:${MASTER_PID}"; do
  IFS=':' read -r name pid <<< "$pid_spec"
  for _ in $(seq 1 150); do
    if ! kill -0 "${pid}" >/dev/null 2>&1; then break; fi
    sleep 0.1
  done
  if kill -0 "${pid}" >/dev/null 2>&1; then
    kill -TERM "${pid}" >/dev/null 2>&1 || true
    sleep 2
    kill -9 "${pid}" >/dev/null 2>&1 || true
  fi
  wait "${pid}" >/dev/null 2>&1 || true
  eval "${name}=''"
done

# Verify replicated stats on slave log
verify_repl_stats "${slave_log}"

# Check for crash signatures
for log_file in "${master_log}" "${slave_log}"; do
  if grep -E "SIGSEGV|segmentation fault|double free|heap-use-after-free|AddressSanitizer|UndefinedBehaviorSanitizer" "${log_file}" >/dev/null 2>&1; then
    echo "crash signature found in ${log_file}" >&2
    exit 1
  fi
done

echo "stage10_3 replicated command task: ok tmp=${TMP_DIR}"
