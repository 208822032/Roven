#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

KV_BIN="${KV_BIN:-./kvstore}"
HOST="${KV_HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage10_9a-slave-owner.$$}"
MASTER_PID=""
SLAVE_PID=""

cleanup() {
  for pid in "${SLAVE_PID}" "${MASTER_PID}"; do
    if [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1; then
      kill -TERM "${pid}" >/dev/null 2>&1 || true
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  done
  if [[ -z "${KEEP_STAGE10_9A_TMP:-}" ]]; then
    rm -rf "${TMP_DIR}"
  else
    printf '[INFO][stage10_9a] keeping tmp dir: %s\n' "${TMP_DIR}" >&2
  fi
}
trap cleanup EXIT

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

static_audit() {
  python3 - <<'PY'
import re
from pathlib import Path

ROOT = Path.cwd()

def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")

def require(cond, msg):
    if not cond:
        raise AssertionError(msg)

def find_func(src, name):
    pattern = re.compile(rf"(^|\n)[^\n;]*\b{re.escape(name)}\s*\(")
    for match in pattern.finditer(src):
        line_end = src.find("\n", match.start() + 1)
        if line_end < 0:
            line_end = len(src)
        line = src[match.start():line_end]
        if ";" not in line and line.strip().startswith(("static ", "int ", "void ", "void*")):
            brace = src.find("{", match.start())
            require(brace >= 0, f"missing body for {name}")
            depth = 0
            for idx in range(brace, len(src)):
                if src[idx] == "{":
                    depth += 1
                elif src[idx] == "}":
                    depth -= 1
                    if depth == 0:
                        return src[match.start():idx + 1]
    raise AssertionError(f"missing function {name}")

hdr = read("include/distributed.h")
dist = read("src/distributed/distributed.c")
kvstore = read("src/core/kvstore.c")
executor = read("src/core/kvs_command_executor.c")
doc = read("docs/route_c_perf_arch/54_stage10_9a_slave_connect_thread_owner_audit.md")

distributed_init = find_func(dist, "distributed_init")
distributed_shutdown = find_func(dist, "distributed_shutdown")
slave_connect_master = find_func(dist, "slave_connect_master")
slave_connect_thread = find_func(dist, "slave_connect_thread")
submit_repl = find_func(executor, "kvs_command_executor_submit_replicated")
dest = find_func(kvstore, "dest_kvengine")
main = find_func(kvstore, "main")

for token in [
    "pthread_t slave_connect_thread",
    "int slave_connect_thread_started",
    "int slave_master_sockfd",
]:
    require(token in hdr, f"distributed_config_t missing {token}")

require("pthread_create(&global_dist_config.slave_connect_thread" in distributed_init,
        "slave connect thread id must be stored in global_dist_config")
require("global_dist_config.slave_connect_thread_started = 1" in distributed_init,
        "slave connect thread started flag must be tracked")
require("global_dist_config.slave_master_sockfd = -1" in distributed_init,
        "slave master socket fd must be initialized")

require("shutdown(master_sockfd, SHUT_RDWR)" in distributed_shutdown,
        "distributed_shutdown must wake slave connect thread socket")
require("pthread_join(connect_thread, NULL)" in distributed_shutdown,
        "distributed_shutdown must join slave connect thread")
require("global_dist_config.slave_connect_thread_started = 0" in distributed_shutdown,
        "distributed_shutdown must clear slave connect thread started flag")
require("pthread_mutex_destroy(&global_dist_config.slave_mutex)" in distributed_shutdown,
        "distributed_shutdown must destroy slave mutex after join boundary")

require("if (g_shutdown_flag)" in slave_connect_thread,
        "slave_connect_thread must avoid starting connect after shutdown")
require("distributed_set_slave_master_sockfd(sock)" in slave_connect_master,
        "slave_connect_master must publish master socket fd")
require("distributed_clear_slave_master_sockfd(sock)" in slave_connect_master,
        "slave_connect_master must clear master socket fd")
require("while(!g_shutdown_flag)" in slave_connect_master,
        "slave receive loop must observe shutdown flag")
require("should_apply && !g_shutdown_flag" in slave_connect_master,
        "slave connect thread must not submit replicated tasks during shutdown")
require("kvs_command_executor_submit_replicated(" in slave_connect_master,
        "TCP slave apply must still submit replicated tasks to executor")

for forbidden in [
    "kvs_filter_protocol(",
    "kvs_handler(",
    "kvs_vector_",
    "kvs_rdb_save(",
    "aof_rewrite",
]:
    require(forbidden not in slave_connect_master,
            f"slave connect thread must not call {forbidden}")

require("!g_executor.running || g_executor.stopping" in submit_repl,
        "replicated submit must reject executor stopped/stopping")
require("distributed_shutdown()" in dest and "kvs_vector_destroy()" in dest and
        dest.index("distributed_shutdown()") < dest.index("kvs_vector_destroy()"),
        "distributed shutdown must precede store/vector destroy")
shutdown_pos = kvstore.index("shutdown requested, saving data")
exec_stop_pos = kvstore.index("kvs_command_executor_stop()", shutdown_pos)
destroy_pos = kvstore.index("dest_kvengine()", shutdown_pos)
require(exec_stop_pos < destroy_pos,
        "main shutdown order must be explicit for executor stop before destroy")

for phrase in [
    "APPROVED",
    "slave_connect_thread_started",
    "slave_master_sockfd",
    "kvs_command_executor_submit_replicated",
    "prevents shutdown-time replicated task submission",
]:
    require(phrase in doc, f"Stage 10.9A doc missing phrase: {phrase}")

print("Stage 10.9A slave connect owner static audit passed")
PY
}

write_conf() {
  local role="$1"
  local port="$2"
  local sync_port="$3"
  local master_ip="$4"
  local master_port="$5"
  local conf="$6"
  local dir="$7"
  mkdir -p "${dir}"
  cat >"${conf}" <<EOF_CONF
role = ${role}
port = ${port}
sync_port = ${sync_port}
master_ip = ${master_ip}
master_port = ${master_port}
enable_aof = no
aof_fsync_mode = no
aof_filename = ${dir}/stage10_9a.aof
rdb_filename = ${dir}/stage10_9a.rdb
dump_filename = ${dir}/stage10_9a.dat
network_mode = reactor
mem_backend = pool
sync_mode = tcp
log_level = info
log_async = no
embedding_provider = hash
embedding_dim = 64
embedding_num_threads = 1
embedding_max_text_len = 512
embedding_worker_pool_enabled = no
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

deadline = time.time() + 30
while time.time() < deadline:
    try:
        sock = socket.create_connection((host, port), timeout=2)
        sock.sendall(encode(["EXIST", "__stage10_9a_probe__"]))
        data = sock.recv(64)
        sock.close()
        if data.startswith(b"+EXIST") or data.startswith(b"+NO"):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.1)
raise SystemExit("server did not become ready")
PY
}

client_suite() {
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

def eventually(fn, timeout=20):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            if fn():
                return
        except Exception as exc:
            last = exc
        time.sleep(0.2)
    raise AssertionError(f"condition not met, last={last!r}")

master = Client(master_port)
slave = Client(slave_port)
try:
    require(master.send(["SET", "s10_9a_keep", "alpha"]) == ("simple", "OK"), "master SET")
    eventually(lambda: slave.send(["GET", "s10_9a_keep"]) == ("bulk", "alpha"))

    require(master.send(["SET", "s10_9a_del", "gone"]) == ("simple", "OK"), "master SET del")
    eventually(lambda: slave.send(["GET", "s10_9a_del"]) == ("bulk", "gone"))
    require(master.send(["DEL", "s10_9a_del"]) == ("simple", "OK"), "master DEL")
    eventually(lambda: slave.send(["GET", "s10_9a_del"]) == ("simple", "NO EXIST"))

    require(master.send(["SET", "s10_9a_ttl", "live"]) == ("simple", "OK"), "master SET ttl")
    require(master.send(["EXPIRE", "s10_9a_ttl", "30"]) == ("simple", "OK"), "master EXPIRE")
    eventually(lambda: slave.send(["TTL", "s10_9a_ttl"])[0] == "integer")
finally:
    master.close()
    slave.close()
PY
}

check_logs() {
  local log_file
  for log_file in "$@"; do
    if grep -E "SIGSEGV|segmentation fault|double free|heap-use-after-free|AddressSanitizer|UndefinedBehaviorSanitizer" "${log_file}" >/dev/null 2>&1; then
      echo "crash signature found in ${log_file}" >&2
      exit 1
    fi
  done
}

dynamic_smoke() {
  if [[ ! -x "${KV_BIN}" ]]; then
    echo "未运行动态 smoke：${KV_BIN} 不存在或不可执行"
    return 0
  fi

  local master_port slave_port sync_port
  local master_dir slave_dir master_conf slave_conf master_log slave_log
  master_port="${MASTER_PORT:-$(pick_port)}"
  slave_port="${SLAVE_PORT:-$(pick_port)}"
  sync_port=$((master_port + 1000))
  master_dir="${TMP_DIR}/master"
  slave_dir="${TMP_DIR}/slave"
  master_conf="${master_dir}/kvstore.conf"
  slave_conf="${slave_dir}/kvstore.conf"
  master_log="${master_dir}/kvstore.log"
  slave_log="${slave_dir}/kvstore.log"

  write_conf master "${master_port}" "${sync_port}" "" "0" "${master_conf}" "${master_dir}"
  write_conf slave "${slave_port}" "${sync_port}" "${HOST}" "${master_port}" "${slave_conf}" "${slave_dir}"

  "${KV_BIN}" --config "${master_conf}" >"${master_log}" 2>&1 &
  MASTER_PID=$!
  wait_for_ready "${master_port}"

  "${KV_BIN}" --config "${slave_conf}" >"${slave_log}" 2>&1 &
  SLAVE_PID=$!
  wait_for_ready "${slave_port}"

  client_suite "${master_port}" "${slave_port}"

  kill -TERM "${SLAVE_PID}" >/dev/null 2>&1 || true
  for _ in $(seq 1 100); do
    if ! kill -0 "${SLAVE_PID}" >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
  done
  if kill -0 "${SLAVE_PID}" >/dev/null 2>&1; then
    echo "slave shutdown timed out" >&2
    exit 1
  fi
  wait "${SLAVE_PID}" >/dev/null 2>&1 || true
  SLAVE_PID=""

  kill -TERM "${MASTER_PID}" >/dev/null 2>&1 || true
  for _ in $(seq 1 100); do
    if ! kill -0 "${MASTER_PID}" >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
  done
  if kill -0 "${MASTER_PID}" >/dev/null 2>&1; then
    echo "master shutdown timed out" >&2
    exit 1
  fi
  wait "${MASTER_PID}" >/dev/null 2>&1 || true
  MASTER_PID=""

  grep -q "connected to master, starting synchronization" "${slave_log}"
  grep -q "replicated executor stats:" "${slave_log}"
  check_logs "${master_log}" "${slave_log}"
  echo "Stage 10.9A slave connect owner dynamic smoke passed"
}

static_audit
dynamic_smoke
echo "Stage 10.9A slave connect owner audit passed"
