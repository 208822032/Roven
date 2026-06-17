#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
HIGH_ACCEPT_COUNT="${HIGH_ACCEPT_COUNT:-200}"
FD_REUSE_COUNT="${FD_REUSE_COUNT:-120}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage9-worker.$$}"
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

run_client_suite() {
  local port="$1"
  local high_accept="$2"
  local fd_reuse="$3"
  python3 - "${HOST}" "${port}" "${high_accept}" "${fd_reuse}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
high_accept = int(sys.argv[3])
fd_reuse = int(sys.argv[4])


def encode(tokens):
    chunks = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        chunks.append(f"${len(data)}\r\n".encode())
        chunks.append(data)
        chunks.append(b"\r\n")
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
            return ("simple", self.line().decode("utf-8", "replace"))
        if typ == b"-":
            return ("error", self.line().decode("utf-8", "replace"))
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


def wait_ready():
    deadline = time.time() + 40
    while time.time() < deadline:
        try:
            c = Client(timeout=2)
            try:
                if c.send(["SET", "stage9_ready", "1"]) == ("simple", "OK"):
                    return
            finally:
                c.close()
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("server did not become ready")


def basic_vector():
    c = Client()
    try:
        require(c.send(["VDEL", "怎么重置密码", "stage9"]) in [("simple", "OK"), ("simple", "NO EXIST")], "VDEL failed")
        require(c.send(["VSET", "怎么重置密码", "可以在登录页点击忘记密码", "stage9"]) == ("simple", "OK"), "VSET failed")
        require(c.send(["VGET", "怎么重置密码", "stage9"]) == ("bulk", "可以在登录页点击忘记密码"), "VGET mismatch")
        require(c.send(["VSEARCH", "忘记密码怎么办", "stage9", "5", "0.0"])[0] == "array", "VSEARCH failed")
    finally:
        c.close()


def ttl_case():
    c = Client()
    try:
        require(c.send(["SET", "stage9_ttl_key", "1"]) == ("simple", "OK"), "TTL SET failed")
        require(c.send(["EXPIRE", "stage9_ttl_key", "30"]) == ("simple", "OK"), "EXPIRE failed")
        ttl_resp = c.send(["TTL", "stage9_ttl_key"])
        require(ttl_resp[0] == "integer" and ttl_resp[1] > 0, f"TTL unexpected {ttl_resp}")
        require(c.send(["PERSIST", "stage9_ttl_key"]) == ("simple", "OK"), "PERSIST failed")
        require(c.send(["TTL", "stage9_ttl_key"]) == ("integer", -1), "TTL after PERSIST mismatch")
    finally:
        c.close()


def repeat_vset_case():
    c = Client()
    try:
        require(c.send(["VSET", "stage9 repeat question", "stage9 repeat answer old", "stage9_repeat"]) == ("simple", "OK"), "repeat VSET old failed")
        require(c.send(["VSET", "stage9 repeat question", "stage9 repeat answer new", "stage9_repeat"]) == ("simple", "OK"), "repeat VSET new failed")
        require(c.send(["VGET", "stage9 repeat question", "stage9_repeat"]) == ("bulk", "stage9 repeat answer new"), "repeat VSET VGET mismatch")
        require(c.send(["VSEARCH", "stage9 repeat question", "stage9_repeat", "3", "0.0"])[0] == "array", "repeat VSET VSEARCH failed")
    finally:
        c.close()


def scoped_and_delete_case():
    c = Client()
    try:
        require(c.send(["VSET", "stage9 scoped question", "scope one answer", "stage9_scope_one"]) == ("simple", "OK"), "scoped VSET one failed")
        require(c.send(["VSET", "stage9 scoped question", "scope two answer", "stage9_scope_two"]) == ("simple", "OK"), "scoped VSET two failed")
        resp = c.send(["VSEARCH", "stage9 scoped question", "stage9_scope_one", "5", "0.0"])
        require(resp[0] == "array", "scoped VSEARCH failed")
        joined = repr(resp)
        require("stage9_scope_one" in joined, "scoped VSEARCH missing scope one")
        require("stage9_scope_two" not in joined, "scoped VSEARCH leaked scope two")
        require(c.send(["VSET", "stage9 delete question", "stage9 delete answer", "stage9_delete"]) == ("simple", "OK"), "delete VSET failed")
        require(c.send(["VDEL", "stage9 delete question", "stage9_delete"]) == ("simple", "OK"), "delete VDEL failed")
        resp = c.send(["VSEARCH", "stage9 delete question", "stage9_delete", "5", "0.0"])
        require(resp[0] == "array", "delete VSEARCH failed")
        require("stage9 delete answer" not in repr(resp), "deleted record returned by VSEARCH")
    finally:
        c.close()


def invalid_paths():
    c = Client()
    try:
        require(c.send(["VSET"]) [0] == "error", "invalid VSET should error")
        require(c.send(["VSEARCH"]) [0] == "error", "invalid VSEARCH should error")
        require(c.send(["VBOGUS", "x"]) [0] == "error", "unknown vector command should error")
    finally:
        c.close()


def pipeline_order(count):
    c = Client(timeout=60)
    try:
        payload = b""
        expected_types = []
        for i in range(count):
            payload += encode(["VSET", f"stage9 pipe q {i}", f"stage9 pipe a {i}", "stage9_pipe"])
            expected_types.append("simple")
        for i in range(count):
            payload += encode(["VSEARCH", f"stage9 pipe q {i}", "stage9_pipe", "5", "0.0"])
            expected_types.append("array")
        c.send_raw(payload)
        for i, typ in enumerate(expected_types):
            resp = c.read()
            require(resp[0] == typ, f"pipeline response {i} type={resp[0]} expected={typ}")
    finally:
        c.close()


def high_accept_case():
    clients = []
    try:
        for i in range(high_accept):
            c = Client(timeout=30)
            clients.append(c)
            c.send_raw(encode(["VSET", f"stage9 high q {i}", f"stage9 high a {i}", "stage9_high"]))
        for i, c in enumerate(clients):
            require(c.read() == ("simple", "OK"), f"high accept VSET failed at {i}")
    finally:
        for c in clients:
            c.close()


def fd_reuse_case():
    for i in range(fd_reuse):
        c = Client(timeout=30)
        c.send_raw(encode(["VSET", f"stage9 reuse q {i}", f"stage9 reuse a {i}", "stage9_reuse"]))
        c.close()
        c2 = Client(timeout=30)
        try:
            require(c2.send(["SET", f"stage9_reuse:{i}", f"value:{i}"]) == ("simple", "OK"), "SET after reuse failed")
            require(c2.send(["GET", f"stage9_reuse:{i}"]) == ("bulk", f"value:{i}"), "GET after reuse mismatch")
        finally:
            c2.close()


def concurrent_sessions(nclients, nreq):
    clients = [Client(timeout=60) for _ in range(nclients)]
    try:
        for idx, c in enumerate(clients):
            payload = b""
            for j in range(nreq):
                payload += encode(["VSET", f"stage9 concurrent q {idx}-{j}", f"stage9 concurrent a {idx}-{j}", "stage9_concurrent"])
            c.send_raw(payload)
        for idx, c in enumerate(clients):
            for j in range(nreq):
                require(c.read() == ("simple", "OK"), f"concurrent VSET failed client={idx} req={j}")
    finally:
        for c in clients:
            c.close()


wait_ready()
basic_vector()
ttl_case()
repeat_vset_case()
scoped_and_delete_case()
invalid_paths()
pipeline_order(10)
pipeline_order(50)
high_accept_case()
fd_reuse_case()
concurrent_sessions(8, 8)
print("stage9 client suite: ok")
PY
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
    chunks = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode()
        chunks.append(f"${len(data)}\r\n".encode())
        chunks.append(data)
        chunks.append(b"\r\n")
    return b"".join(chunks)

deadline = time.time() + 40
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=2)
        s.sendall(encode(["SET", "stage9_ready_probe", "1"]))
        data = s.recv(16)
        s.close()
        if data.startswith(b"+OK"):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.2)
raise SystemExit("server did not become ready")
PY
}

mkdir -p "${TMP_DIR}"

for mode in reactor proactor; do
  for combo in "0 1 disabled" "1 1 threads1" "1 2 threads2"; do
    cleanup
    SERVER_PID=""
    read -r enabled threads label <<<"${combo}"
    port="$(pick_port)"
    data_dir="${TMP_DIR}/${mode}_${label}_data"
    mkdir -p "${data_dir}"
    conf="${TMP_DIR}/${mode}_${label}.conf"
    log="${TMP_DIR}/${mode}_${label}.log"
    write_conf "${mode}" "${enabled}" "${threads}" "${port}" "${conf}" "${data_dir}"
    (cd "${ROOT_DIR}" && ./kvstore -c "${conf}" >"${log}" 2>&1) &
    SERVER_PID="$!"
    SERVER_PORT="${port}"
    wait_for_ready "${port}"
    (cd "${ROOT_DIR}" && bash tests/semantic_onnx_test.sh "${HOST}" "${port}")
    run_client_suite "${port}" "${HIGH_ACCEPT_COUNT}" "${FD_REUSE_COUNT}"
    cleanup
    SERVER_PID=""
    SERVER_PORT=""
    if [[ "${enabled}" == "1" ]]; then
      if ! grep -q "embedding worker pool stats: enabled=" "${log}"; then
        echo "missing worker pool stats in ${log}" >&2
        exit 1
      fi
      python3 - "${log}" <<'PY'
import re
import sys

log = sys.argv[1]
worker = ""
executor = ""
for line in open(log, errors="replace"):
    if "embedding worker pool stats:" in line:
        worker = line
    if "embedding executor bottleneck stats:" in line:
        executor = line
if not worker or not executor:
    raise SystemExit("missing stats lines")

def fields(line):
    return {k: int(v) for k, v in re.findall(r"([A-Za-z0-9_]+)=([0-9]+)", line)}

w = fields(worker)
e = fields(executor)
submitted = w.get("submitted", 0)
completed = w.get("completed", 0)
failed = w.get("failed", 0)
dispatch_failed = w.get("dispatch_failed", 0)
if submitted <= 0:
    raise SystemExit(f"bad submitted={submitted}")
if submitted != completed + failed + dispatch_failed:
    raise SystemExit(f"bad prepare accounting submitted={submitted} completed={completed} failed={failed} dispatch_failed={dispatch_failed}")
if w.get("queue_depth", -1) != 0:
    raise SystemExit(f"prepare queue not drained: {w.get('queue_depth')}")
if e.get("prepared_task_executed", 0) <= 0:
    raise SystemExit(f"prepared_task_executed not positive: {e.get('prepared_task_executed', 0)}")
if e.get("prepared_queue_depth", -1) != 0:
    raise SystemExit(f"prepared queue not drained: {e.get('prepared_queue_depth')}")
PY
    fi
    echo "${mode} ${label}: ok log=${log}"
  done
done

echo "stage9 embedding worker pool: ok tmp=${TMP_DIR}"
