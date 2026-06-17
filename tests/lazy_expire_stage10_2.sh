#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage10_2-lazy.$$}"
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
        s.sendall(encode(["SET", "stage10_2_ready", "1"]))
        data = s.recv(32)
        s.close()
        if data.startswith(b"+OK"):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.2)
raise SystemExit("server did not become ready")
PY
}

run_lazy_expire_suite() {
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


def basic_lazy_expire():
    """Test 1: GET triggers lazy expire on expired key"""
    c = Client()
    try:
        require(c.send(["SET", "s10_2_basic", "value"]) == ("simple", "OK"), "SET failed")
        require(c.send(["EXPIRE", "s10_2_basic", "1"]) == ("simple", "OK"), "EXPIRE failed")
        ttl = c.send(["TTL", "s10_2_basic"])
        require(ttl[0] == "integer" and 0 <= ttl[1] <= 1, f"bad TTL {ttl}")
        time.sleep(2.2)
        # GET triggers lazy expire
        require(c.send(["GET", "s10_2_basic"]) == ("simple", "NO EXIST"), "lazy expire GET mismatch")
        require(c.send(["TTL", "s10_2_basic"]) == ("integer", -2), "lazy expire TTL mismatch")
        require(c.send(["EXIST", "s10_2_basic"]) == ("simple", "NO EXIST"), "lazy expire EXIST mismatch")

        # Test 2: EXISTS triggers lazy expire
        require(c.send(["SET", "s10_2_exist", "v"]) == ("simple", "OK"), "EXIST SET failed")
        require(c.send(["EXPIRE", "s10_2_exist", "1"]) == ("simple", "OK"), "EXIST EXPIRE failed")
        time.sleep(2.2)
        require(c.send(["EXIST", "s10_2_exist"]) == ("simple", "NO EXIST"), "EXIST lazy expire mismatch")

        # Test 3: DEL on expired key
        require(c.send(["SET", "s10_2_del_exp", "v"]) == ("simple", "OK"), "del_exp SET failed")
        require(c.send(["EXPIRE", "s10_2_del_exp", "1"]) == ("simple", "OK"), "del_exp EXPIRE failed")
        time.sleep(2.2)
        require(c.send(["DEL", "s10_2_del_exp"]) == ("simple", "NO EXIST"), "DEL expired mismatch")
    finally:
        c.close()


def lazy_expire_new_value_protection():
    """Test 4: lazy expire does not delete newly SET value on same key"""
    c = Client(timeout=60)
    try:
        require(c.send(["SET", "s10_2_recheck", "old"]) == ("simple", "OK"), "recheck SET old failed")
        require(c.send(["EXPIRE", "s10_2_recheck", "1"]) == ("simple", "OK"), "recheck EXPIRE old failed")
        time.sleep(2.2)
        # Key is expired; active expire (Stage 10.1) may have already deleted it.
        # SET response may be OK (key was already deleted) or EXIST (key expired but still present).
        set_resp = c.send(["SET", "s10_2_recheck", "new"])
        require(set_resp[0] == "simple" and set_resp[1] in ("OK", "EXIST"),
                f"recheck SET new unexpected: {set_resp}")
        require(c.send(["EXPIRE", "s10_2_recheck", "30"]) == ("simple", "OK"), "recheck EXPIRE new failed")
        # GET should return new value, not trigger delete of new value
        require(c.send(["GET", "s10_2_recheck"]) == ("bulk", "new"), "lazy expire deleted new value")
        ttl = c.send(["TTL", "s10_2_recheck"])
        require(ttl[0] == "integer" and ttl[1] > 0, f"recheck TTL lost {ttl}")

        # Test 5: PERSIST after expire prevents deletion
        require(c.send(["SET", "s10_2_persist_after", "val"]) == ("simple", "OK"), "persist SET failed")
        require(c.send(["EXPIRE", "s10_2_persist_after", "1"]) == ("simple", "OK"), "persist EXPIRE failed")
        time.sleep(2.2)
        # PERSIST may return OK (key expired but still exists) or NO EXIST (active expire already deleted)
        persist_resp = c.send(["PERSIST", "s10_2_persist_after"])
        require(persist_resp[0] == "simple", f"PERSIST after expire unexpected: {persist_resp}")
        # If PERSIST succeeded, verify key survived; if NO EXIST, verify not present
        if persist_resp[1] == "OK":
            require(c.send(["GET", "s10_2_persist_after"]) == ("bulk", "val"), "PERSIST then GET failed")
            require(c.send(["TTL", "s10_2_persist_after"]) == ("integer", -1), "PERSIST TTL not -1")
        else:
            require(c.send(["GET", "s10_2_persist_after"]) == ("simple", "NO EXIST"), "PERSIST NO EXIST but key present")
    finally:
        c.close()


def lazy_expire_ttl_commands():
    """Test 6: TTL/EXPIRE/PEXPIREAT commands on expired keys"""
    c = Client(timeout=60)
    try:
        require(c.send(["SET", "s10_2_ttl_cmd", "v"]) == ("simple", "OK"), "TTL cmd SET failed")
        require(c.send(["EXPIRE", "s10_2_ttl_cmd", "1"]) == ("simple", "OK"), "TTL cmd EXPIRE failed")
        time.sleep(2.2)
        # TTL on expired key should trigger lazy expire and return -2
        require(c.send(["TTL", "s10_2_ttl_cmd"]) == ("integer", -2), "TTL on expired mismatch")
        require(c.send(["GET", "s10_2_ttl_cmd"]) == ("simple", "NO EXIST"), "TTL lazy expire did not delete")

        # EXPIRE on expired key should trigger deletion and return NO EXIST
        # (the key was expired, got lazily deleted, so there's nothing to set TTL on)
        require(c.send(["SET", "s10_2_expire_cmd", "v"]) == ("simple", "OK"), "expire cmd SET failed")
        require(c.send(["EXPIRE", "s10_2_expire_cmd", "1"]) == ("simple", "OK"), "expire cmd EXPIRE failed")
        time.sleep(2.2)
        resp = c.send(["EXPIRE", "s10_2_expire_cmd", "30"])
        require(resp[0] == "simple", f"EXPIRE on expired key unexpected: {resp}")
        # Redis returns 0 for EXPIRE on non-existent key; KVLite returns NO EXIST
        require(resp[1] in ("NO EXIST", "OK"), f"EXPIRE on expired key bad: {resp}")
        require(c.send(["GET", "s10_2_expire_cmd"]) == ("simple", "NO EXIST"), "key survived EXPIRE")

        # PEXPIREAT with past timestamp should delete
        require(c.send(["SET", "s10_2_pexpireat", "v"]) == ("simple", "OK"), "pexpireat SET failed")
        past_ms = int((time.time() - 10) * 1000)
        require(c.send(["PEXPIREAT", "s10_2_pexpireat", str(past_ms)]) == ("simple", "OK"), "PEXPIREAT past failed")
        require(c.send(["GET", "s10_2_pexpireat"]) == ("simple", "NO EXIST"), "PEXPIREAT did not delete")
    finally:
        c.close()


def lazy_plus_active_coexistence():
    """Test 7: active expire (Stage 10.1) and lazy expire coexist safely"""
    c = Client(timeout=60)
    try:
        # Create keys that will be deleted by active expire
        for i in range(40):
            require(c.send(["SET", f"s10_2_active_{i}", f"val_{i}"]) == ("simple", "OK"), f"active SET {i} failed")
            require(c.send(["EXPIRE", f"s10_2_active_{i}", "1"]) == ("simple", "OK"), f"active EXPIRE {i} failed")

        # Create keys that will be deleted by lazy expire (GET trigger)
        for i in range(20):
            require(c.send(["SET", f"s10_2_lazy_{i}", f"val_{i}"]) == ("simple", "OK"), f"lazy SET {i} failed")
            require(c.send(["EXPIRE", f"s10_2_lazy_{i}", "1"]) == ("simple", "OK"), f"lazy EXPIRE {i} failed")

        time.sleep(2.5)

        # Trigger lazy expire via GET on all 20 expired keys
        for i in range(20):
            require(c.send(["GET", f"s10_2_lazy_{i}"]) == ("simple", "NO EXIST"), f"lazy GET {i} failed")

        # Verify active expire also cleared its batch
        def active_gone():
            ok = True
            for j in range(0, 40, 5):
                ok = ok and c.send(["TTL", f"s10_2_active_{j}"]) == ("integer", -2)
            return ok
        require(eventually(active_gone, timeout=10.0), "active expire did not clear keys")

        # Verify server still healthy
        require(c.send(["SET", "s10_2_health", "ok"]) == ("simple", "OK"), "health check failed")
        require(c.send(["GET", "s10_2_health"]) == ("bulk", "ok"), "health check GET failed")
    finally:
        c.close()


def concurrent_lazy_expire():
    """Test 8: concurrent GET/EXIST/DEL mixed with short TTL keys"""
    errors = []

    def worker(idx):
        try:
            c = Client(timeout=60)
            try:
                for j in range(15):
                    key = f"s10_2_concurrent_{idx}_{j}"
                    require(c.send(["SET", key, f"val_{idx}_{j}"])[0] == "simple", f"SET bad for {key}")
                    require(c.send(["EXPIRE", key, "1"]) == ("simple", "OK"), f"EXPIRE bad for {key}")
                    # Mix of lazy expire triggers
                    if j % 3 == 0:
                        time.sleep(1.5)
                        c.send(["GET", key])
                    elif j % 3 == 1:
                        time.sleep(1.5)
                        c.send(["EXIST", key])
                    else:
                        c.send(["DEL", key])
            finally:
                c.close()
        except Exception as exc:
            errors.append(str(exc))

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    require(not errors, "concurrent lazy expire errors: " + repr(errors))


def vector_with_lazy_expire():
    """Test 9: VSET/VSEARCH concurrent with lazy expire GET/EXIST"""
    if not with_vector:
        return
    c = Client(timeout=60)
    try:
        require(c.send(["VSET", "s10_2 vector q1", "s10_2 vector a1", "s10_2_scope"]) == ("simple", "OK"), "VSET q1 failed")
        require(c.send(["VSET", "s10_2 vector q2", "s10_2 vector a2", "s10_2_scope"]) == ("simple", "OK"), "VSET q2 failed")

        # Create short-lived keys
        for i in range(15):
            require(c.send(["SET", f"s10_2_vec_lazy_{i}", "v"]) == ("simple", "OK"), f"vec lazy SET {i} failed")
            require(c.send(["EXPIRE", f"s10_2_vec_lazy_{i}", "1"]) == ("simple", "OK"), f"vec lazy EXPIRE {i} failed")

        time.sleep(2.2)

        # Mix VSEARCH and lazy expire GET
        resp = c.send(["VSEARCH", "s10_2 vector q1", "s10_2_scope", "3", "0.0"])
        require(resp[0] == "array", "VSEARCH failed")
        for i in range(15):
            c.send(["GET", f"s10_2_vec_lazy_{i}"])

        resp = c.send(["VSEARCH", "s10_2 vector q2", "s10_2_scope", "3", "0.0"])
        require(resp[0] == "array", "VSEARCH with lazy expire failed")
    finally:
        c.close()


basic_lazy_expire()
lazy_expire_new_value_protection()
lazy_expire_ttl_commands()
lazy_plus_active_coexistence()
concurrent_lazy_expire()
vector_with_lazy_expire()
print("stage10_2 lazy expire suite: ok")
PY
}

verify_stats() {
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
if t.get("ttl_expire_queue_depth", -1) != 0:
    raise SystemExit(f"ttl queue not drained: {t.get('ttl_expire_queue_depth')}")
if worker:
    w = fields(worker)
    if w.get("enabled", 0) == 1 and w.get("queue_depth", -1) != 0:
        raise SystemExit(f"worker queue not drained: {w.get('queue_depth')}")
print("ttl/worker stats drain: ok")
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

def worker(idx):
    try:
        s = socket.create_connection((host, port), timeout=10)
        s.settimeout(20)
        payload = b""
        for j in range(25):
            payload += encode(["SET", f"s10_2_shutdown_{idx}_{j}", "v"])
            payload += encode(["EXPIRE", f"s10_2_shutdown_{idx}_{j}", "1"])
        s.sendall(payload)
        for _ in range(50):
            s.recv(128)
        # Trigger some lazy expire GETs
        for j in range(0, 25, 3):
            s.sendall(encode(["GET", f"s10_2_shutdown_{idx}_{j}"]))
            s.recv(128)
        s.close()
    except OSError:
        pass

threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
for t in threads:
    t.start()
time.sleep(1.5)
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
      echo "${mode} shutdown during lazy expire: ok"
      return 0
    fi
    sleep 0.1
  done
  echo "${mode} shutdown during lazy expire timed out" >&2
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
  run_lazy_expire_suite "${port}" 1
  cleanup
  SERVER_PID=""
  SERVER_PORT=""
  verify_stats "${log}"
  echo "${mode} lazy expire suite: ok log=${log}"

  # Shutdown during lazy expire
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

echo "stage10_2 lazy expire: ok tmp=${TMP_DIR}"
