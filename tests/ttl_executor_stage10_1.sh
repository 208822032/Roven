#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage10_1-ttl.$$}"
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
  local enabled="$2"
  local threads="$3"
  local port="$4"
  local conf="$5"
  local data_dir="$6"
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
embedding_worker_pool_enabled = ${enabled}
embedding_worker_threads = ${threads}
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

wait_for_ready() {
  local port="$1"
  python3 - "${HOST}" "${port}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])

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
        s.sendall(encode(["SET", "stage10_1_ready_probe", "1"]))
        data = s.recv(32)
        s.close()
        if data.startswith(b"+OK"):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.2)
raise SystemExit("server did not become ready")
PY
}

run_ttl_client_suite() {
  local port="$1"
  local with_vector="$2"
  python3 - "${HOST}" "${port}" "${with_vector}" <<'PY'
import socket
import sys
import threading
import time

host = sys.argv[1]
port = int(sys.argv[2])
with_vector = int(sys.argv[3])


def encode(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)


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

    def send_raw(self, data):
        self.sock.sendall(data)

    def send(self, tokens):
        self.send_raw(encode(tokens))
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


def eventually(fn, timeout=8.0, interval=0.2):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = fn()
        if last:
            return True
        time.sleep(interval)
    return False


def basic_ttl():
    c = Client()
    try:
        require(c.send(["SET", "s10_basic", "value"]) == ("simple", "OK"), "SET failed")
        require(c.send(["EXPIRE", "s10_basic", "1"]) == ("simple", "OK"), "EXPIRE failed")
        ttl = c.send(["TTL", "s10_basic"])
        require(ttl[0] == "integer" and 0 <= ttl[1] <= 1, f"bad TTL {ttl}")
        time.sleep(2.2)
        require(c.send(["GET", "s10_basic"]) == ("simple", "NO EXIST"), "expired GET mismatch")
        require(c.send(["TTL", "s10_basic"]) == ("integer", -2), "expired TTL mismatch")

        require(c.send(["SET", "s10_persist", "value"]) == ("simple", "OK"), "persist SET failed")
        require(c.send(["EXPIRE", "s10_persist", "1"]) == ("simple", "OK"), "persist EXPIRE failed")
        require(c.send(["PERSIST", "s10_persist"]) == ("simple", "OK"), "PERSIST failed")
        time.sleep(1.4)
        require(c.send(["GET", "s10_persist"]) == ("bulk", "value"), "PERSIST value expired")

        require(c.send(["SET", "s10_del", "value"]) == ("simple", "OK"), "delete SET failed")
        require(c.send(["EXPIRE", "s10_del", "30"]) == ("simple", "OK"), "delete EXPIRE failed")
        require(c.send(["DEL", "s10_del"]) == ("simple", "OK"), "DEL failed")
        require(c.send(["TTL", "s10_del"]) == ("integer", -2), "DEL left TTL metadata")
    finally:
        c.close()


def active_expire_and_recheck():
    c = Client(timeout=60)
    try:
        for i in range(80):
            require(c.send(["SET", f"s10_active_{i}", f"value_{i}"]) == ("simple", "OK"), "active SET failed")
            require(c.send(["EXPIRE", f"s10_active_{i}", "1"]) == ("simple", "OK"), "active EXPIRE failed")
        require(c.send(["SET", "s10_recheck", "old"]) == ("simple", "OK"), "recheck SET old failed")
        require(c.send(["EXPIRE", "s10_recheck", "1"]) == ("simple", "OK"), "recheck EXPIRE old failed")
        time.sleep(1.2)
        require(c.send(["SET", "s10_recheck", "new"]) == ("simple", "EXIST"), "recheck SET new failed")
        require(c.send(["EXPIRE", "s10_recheck", "30"]) == ("simple", "OK"), "recheck EXPIRE new failed")

        def gone():
            ok = True
            for j in range(0, 80, 10):
                ok = ok and c.send(["TTL", f"s10_active_{j}"]) == ("integer", -2)
            return ok

        require(eventually(gone, timeout=8.0), "active expire did not clear keys")
        require(c.send(["GET", "s10_recheck"]) == ("bulk", "new"), "old TTL task deleted new value")
        ttl = c.send(["TTL", "s10_recheck"])
        require(ttl[0] == "integer" and ttl[1] > 0, f"recheck TTL lost {ttl}")
    finally:
        c.close()


def concurrent_ttl():
    errors = []

    def worker(idx):
        try:
            c = Client(timeout=60)
            try:
                for j in range(20):
                    key = f"s10_concurrent_{idx}_{j}"
                    r = c.send(["SET", key, f"value_{idx}_{j}"])
                    require(r[0] == "simple", f"SET bad {r}")
                    require(c.send(["EXPIRE", key, "1"]) == ("simple", "OK"), "EXPIRE bad")
                    if j % 3 == 0:
                        c.send(["GET", key])
                    if j % 5 == 0:
                        c.send(["DEL", key])
            finally:
                c.close()
        except Exception as exc:
            errors.append(str(exc))

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(6)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    require(not errors, "concurrent errors: " + repr(errors))


def vector_mix():
    if not with_vector:
        return
    c = Client(timeout=60)
    try:
        require(c.send(["VSET", "stage10 ttl vector q1", "stage10 ttl vector a1", "stage10_ttl"]) == ("simple", "OK"), "VSET q1 failed")
        require(c.send(["VSET", "stage10 ttl vector q2", "stage10 ttl vector a2", "stage10_ttl"]) == ("simple", "OK"), "VSET q2 failed")
        for i in range(20):
            require(c.send(["SET", f"s10_vector_ttl_{i}", "v"]) == ("simple", "OK"), "vector TTL SET failed")
            require(c.send(["EXPIRE", f"s10_vector_ttl_{i}", "1"]) == ("simple", "OK"), "vector TTL EXPIRE failed")
        resp = c.send(["VSEARCH", "stage10 ttl vector q1", "stage10_ttl", "5", "0.0"])
        require(resp[0] == "array", "VSEARCH failed")
        time.sleep(2.2)
        resp = c.send(["VSEARCH", "stage10 ttl vector q2", "stage10_ttl", "5", "0.0"])
        require(resp[0] == "array", "VSEARCH after TTL failed")
    finally:
        c.close()


basic_ttl()
active_expire_and_recheck()
concurrent_ttl()
vector_mix()
print("stage10_1 ttl client suite: ok")
PY
}

verify_ttl_stats() {
  local log="$1"
  python3 - "${log}" <<'PY'
import re
import sys

log = sys.argv[1]
ttl = ""
worker = ""
for line in open(log, errors="replace"):
    if "ttl executor stats:" in line:
        ttl = line
    if "embedding worker pool stats:" in line:
        worker = line
if not ttl:
    raise SystemExit("missing ttl executor stats")

def fields(line):
    return {k: int(v) for k, v in re.findall(r"([A-Za-z0-9_]+)=([0-9]+)", line)}

t = fields(ttl)
submitted = t.get("ttl_expire_task_submitted", 0)
executed = t.get("ttl_expire_task_executed", 0)
failed = t.get("ttl_expire_task_failed", 0)
dropped = t.get("ttl_expire_task_dropped", 0)
if submitted <= 0:
    raise SystemExit(f"ttl submitted not positive: {submitted}")
if submitted != executed + failed + dropped:
    raise SystemExit(f"bad ttl accounting submitted={submitted} executed={executed} failed={failed} dropped={dropped}")
if t.get("ttl_expire_queue_depth", -1) != 0:
    raise SystemExit(f"ttl queue not drained: {t.get('ttl_expire_queue_depth')}")
if t.get("ttl_expire_deleted_keys", 0) <= 0:
    raise SystemExit("ttl deleted keys not positive")
if worker:
    w = fields(worker)
    if w.get("enabled", 0) == 1 and w.get("queue_depth", -1) != 0:
        raise SystemExit(f"worker queue not drained: {w.get('queue_depth')}")
print("ttl stats: ok")
PY
}

run_shutdown_case() {
  local mode="$1"
  local port="$2"
  python3 - "${HOST}" "${port}" <<'PY'
import socket
import sys
import threading
import time

host = sys.argv[1]
port = int(sys.argv[2])

def encode(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)

def writer(idx):
    try:
        s = socket.create_connection((host, port), timeout=10)
        s.settimeout(20)
        payload = b""
        for j in range(30):
            payload += encode(["SET", f"s10_shutdown_{idx}_{j}", "v"])
            payload += encode(["EXPIRE", f"s10_shutdown_{idx}_{j}", "1"])
        s.sendall(payload)
        for _ in range(60):
            s.recv(128)
        s.close()
    except OSError:
        pass

threads = [threading.Thread(target=writer, args=(i,)) for i in range(4)]
for t in threads:
    t.start()
time.sleep(1.2)
s = socket.create_connection((host, port), timeout=10)
s.sendall(encode(["QUIT"]))
try:
    s.recv(128)
except OSError:
    pass
s.close()
for t in threads:
    t.join()
PY

  for _ in $(seq 1 120); do
    if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      wait "${SERVER_PID}"
      SERVER_PID=""
      SERVER_PORT=""
      echo "${mode} shutdown during ttl expire: ok"
      return 0
    fi
    sleep 0.1
  done
  echo "${mode} shutdown during ttl expire timed out" >&2
  return 1
}

mkdir -p "${TMP_DIR}"

for mode in reactor proactor; do
  cleanup
  SERVER_PID=""
  SERVER_PORT=""
  port="$(pick_port)"
  data_dir="${TMP_DIR}/${mode}_data"
  conf="${TMP_DIR}/${mode}.conf"
  log="${TMP_DIR}/${mode}.log"
  mkdir -p "${data_dir}"
  write_conf "${mode}" 1 2 "${port}" "${conf}" "${data_dir}"
  (cd "${ROOT_DIR}" && ./kvstore -c "${conf}" >"${log}" 2>&1) &
  SERVER_PID="$!"
  SERVER_PORT="${port}"
  wait_for_ready "${port}"
  run_ttl_client_suite "${port}" 1
  cleanup
  SERVER_PID=""
  SERVER_PORT=""
  verify_ttl_stats "${log}"
  echo "${mode} ttl executor suite: ok log=${log}"

  port="$(pick_port)"
  data_dir="${TMP_DIR}/${mode}_shutdown_data"
  conf="${TMP_DIR}/${mode}_shutdown.conf"
  log="${TMP_DIR}/${mode}_shutdown.log"
  mkdir -p "${data_dir}"
  write_conf "${mode}" 1 2 "${port}" "${conf}" "${data_dir}"
  (cd "${ROOT_DIR}" && ./kvstore -c "${conf}" >"${log}" 2>&1) &
  SERVER_PID="$!"
  SERVER_PORT="${port}"
  wait_for_ready "${port}"
  run_shutdown_case "${mode}" "${port}"
  SERVER_PID=""
  SERVER_PORT=""
  if grep -E "SIGSEGV|segmentation fault|double free|heap-use-after-free|AddressSanitizer|UndefinedBehaviorSanitizer" "${log}" >/dev/null 2>&1; then
    echo "crash signature found in ${log}" >&2
    exit 1
  fi
done

echo "stage10_1 ttl executor: ok tmp=${TMP_DIR}"
