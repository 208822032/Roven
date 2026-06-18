#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

KV_BIN="${KV_BIN:-./kvstore}"
HOST="${KV_HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage10_9-shutdown.$$}"
CONF_FILE="${TMP_DIR}/kvstore.conf"
LOG_FILE="${TMP_DIR}/kvstore.log"
RDB_FILE="${TMP_DIR}/stage10_9.rdb"
AOF_FILE="${TMP_DIR}/stage10_9.aof"
SERVER_PID=""

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    kill -TERM "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -z "${KEEP_STAGE10_9_TMP:-}" ]]; then
    rm -rf "${TMP_DIR}"
  else
    printf '[INFO][stage10_9] keeping tmp dir: %s\n' "${TMP_DIR}" >&2
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
        if ";" not in line and line.strip().startswith(("static ", "int ", "void ")):
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

kvstore = read("src/core/kvstore.c")
executor = read("src/core/kvs_command_executor.c")
ttl = read("src/core/kvs_ttl_background.c")
worker = read("src/core/kvs_embedding_worker_pool.c")
dist = read("src/distributed/distributed.c")
rdb = read("src/persistence/kvs_rdb.c")
aof_rewrite = read("src/persistence/kvs_aof_rewrite.c")
reactor = read("src/network/reactor.c")
proactor = read("src/network/proactor.c")
doc = read("docs/route_c_perf_arch/53_stage10_9_shutdown_owner_boundary_audit.md")

main = find_func(kvstore, "main")
dest = find_func(kvstore, "dest_kvengine")
executor_stop = find_func(executor, "kvs_command_executor_stop")
submit_normal = find_func(executor, "kvs_command_executor_submit")
submit_prepared = find_func(executor, "kvs_command_executor_submit_prepared")
submit_ttl = find_func(executor, "kvs_command_executor_submit_ttl_expire")
submit_repl = find_func(executor, "kvs_command_executor_submit_replicated")
build_rdb = find_func(executor, "kvs_command_executor_build_rdb_snapshot")
build_aof = find_func(executor, "kvs_command_executor_build_aof_rewrite_snapshot")
ttl_stop = find_func(ttl, "kvs_ttl_background_stop")
worker_stop = find_func(worker, "kvs_embedding_worker_pool_stop")
dist_shutdown = find_func(dist, "distributed_shutdown")
rdb_save = find_func(rdb, "kvs_rdb_save")
aof_bg = find_func(aof_rewrite, "aof_rewrite_background")
aof_buffer = find_func(aof_rewrite, "aof_rewrite_buffer_write")
aof_buffer_unlocked = find_func(aof_rewrite, "aof_rewrite_buffer_write_unlocked")
reactor_submit = find_func(reactor, "reactor_submit_async_command")
reactor_submit_task = find_func(reactor, "reactor_submit_command_task")
proactor_submit = find_func(proactor, "proactor_submit_async_command")
proactor_submit_task = find_func(proactor, "proactor_submit_command_task")

shutdown_pos = main.index("shutdown requested, saving data")
ttl_pos = main.index("kvs_ttl_background_stop()", shutdown_pos)
aof_pos = main.index("aof_fsync(1)", shutdown_pos)
rdb_pos = main.index("kvs_rdb_save(g_cfg.rdb_filename)", shutdown_pos)
exec_stop_pos = main.index("kvs_command_executor_stop()", shutdown_pos)
destroy_pos = main.index("dest_kvengine()", shutdown_pos)
require(shutdown_pos < ttl_pos < aof_pos < rdb_pos < exec_stop_pos < destroy_pos,
        "main shutdown order must stop TTL before final AOF/RDB save, keep executor until after RDB save, and destroy last")

require("distributed_shutdown()" in dest and "kvs_vector_destroy()" in dest and
        dest.index("distributed_shutdown()") < dest.index("kvs_vector_destroy()"),
        "distributed_shutdown must precede vector/store destroy")
# Stage 13: dest_kvengine routes through owner rwlock wrapper.
# The shutdown path falls back to real lock on non-executor thread.
require("kvs_owner_rwlock_wrlock(&g_kv_data_rwlock" in dest,
        "destroy must use owner rwlock wrapper for protection")

require("kvs_command_executor_build_rdb_snapshot(&snapshot)" in rdb_save,
        "RDB save must build snapshot via command executor")
require("kvs_command_executor_build_aof_rewrite_snapshot(&snapshot)" in aof_bg,
        "AOF background rewrite must build snapshot via command executor")
require("aof_rewrite_buffer_write_unlocked(fd)" in aof_buffer and
        "kvs_command_executor_build_aof_rewrite_snapshot(&snapshot)" in aof_buffer_unlocked,
        "AOF rewrite buffer write must build snapshot via command executor")

for name, func in [
    ("normal submit", submit_normal),
    ("TTL submit", submit_ttl),
    ("replicated submit", submit_repl),
    ("RDB snapshot", build_rdb),
    ("AOF snapshot", build_aof),
]:
    require("!g_executor.running || g_executor.stopping" in func,
            f"{name} must reject work after executor stop starts")
require("!g_executor.running" in submit_prepared,
        "prepared submit must reject work after executor is no longer running")

require("kvs_embedding_worker_pool_stop()" in executor_stop,
        "executor stop must stop embedding worker pool")
require("pthread_join(g_executor.thread" in executor_stop,
        "executor stop must join executor thread")
for token in [
    "g_executor.cmd_head = NULL",
    "g_executor.prepared_head = NULL",
    "g_executor.repl_head = NULL",
    "g_executor.ttl_head = NULL",
    "g_executor.rdb_snapshot_head = NULL",
    "g_executor.aof_snapshot_head = NULL",
    "g_executor.resp_head",
]:
    require(token in executor_stop, f"executor stop missing queue cleanup token {token}")

require(("__atomic_store_n(&g_ttl_scan_stop, 1" in ttl_stop or
         "g_ttl_scan_stop = 1" in ttl_stop) and
        "pthread_join(g_ttl_scan_thread" in ttl_stop,
        "TTL background stop must request stop and join scanner")
require("pthread_join(g_pool.threads[i]" in worker_stop,
        "embedding worker pool stop must join worker threads")
require("pthread_join(global_dist_config.master_thread" in dist_shutdown and
        "sync_backend_shutdown()" in dist_shutdown,
        "distributed shutdown must join master thread and stop sync backend")

require("return p_process_received_data(ext) < 0 ? -1 : 1;" not in proactor_submit,
        "proactor async submit failure must not fall back to synchronous handler")
require("kvs_command_executor_submit(task)" in reactor_submit_task,
        "reactor normal path must submit to executor")
require("kvs_command_executor_submit(task)" in proactor_submit_task,
        "proactor normal path must submit to executor")

for phrase in [
    "CONDITIONALLY_READY",
    "kvs_ttl_background_stop() before final save",
    "RDB final save",
    "AOF final save and rewrite",
    "TCP slave connect thread owner is not fully joined",
    "eBPF REPLSYNC/is_replication fallback",
]:
    require(phrase in doc or phrase in read("docs/route_c_perf_arch/58_stage10_12_ebpf_replication_fallback_executorization.md"),
            f"shutdown/eBPF docs missing phrase: {phrase}")

print("Stage 10.9 shutdown owner static audit passed")
PY
}

write_conf() {
  local port="$1"
  mkdir -p "${TMP_DIR}"
  cat >"${CONF_FILE}" <<EOF_CONF
role = master
port = ${port}
sync_port = $((port + 1000))
enable_aof = yes
aof_fsync_mode = no
aof_filename = ${AOF_FILE}
rdb_filename = ${RDB_FILE}
dump_filename = ${TMP_DIR}/stage10_9.dat
network_mode = reactor
mem_backend = pool
sync_mode = tcp
log_level = info
log_async = no
embedding_provider = hash
embedding_dim = 64
embedding_num_threads = 1
embedding_max_text_len = 512
embedding_worker_pool_enabled = yes
embedding_worker_threads = 2
embedding_worker_queue_capacity = 128
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
        sock.sendall(encode(["EXIST", "__stage10_9_probe__"]))
        data = sock.recv(64)
        sock.close()
        if data.startswith(b"+EXIST") or data.startswith(b"+NO"):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.1)
raise SystemExit("server did not become ready")
PY
}

start_server() {
  local port="$1"
  write_conf "${port}"
  "${KV_BIN}" --config "${CONF_FILE}" >"${LOG_FILE}" 2>&1 &
  SERVER_PID=$!
  wait_for_ready "${port}"
}

stop_server() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    kill -TERM "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
    SERVER_PID=""
  fi
}

check_log() {
  if grep -E "SIGSEGV|segmentation fault|double free|heap-use-after-free|AddressSanitizer|UndefinedBehaviorSanitizer" "${LOG_FILE}" >/dev/null 2>&1; then
    echo "crash signature found in ${LOG_FILE}" >&2
    exit 1
  fi
  grep -q "RDB snapshot saved" "${LOG_FILE}"
  grep -q "embedding worker pool stats:" "${LOG_FILE}"
  grep -q "ttl executor stats:" "${LOG_FILE}"
}

run_client_round() {
  local port="$1"
  local mode="$2"
  python3 - "${HOST}" "${port}" "${mode}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
mode = sys.argv[3]

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

c = Client(timeout=60)
try:
    if mode == "write":
        require(c.send(["SET", "s10_9_keep", "alpha"]) == ("simple", "OK"), "SET keep")
        require(c.send(["SET", "s10_9_ttl", "live"]) == ("simple", "OK"), "SET ttl")
        require(c.send(["EXPIRE", "s10_9_ttl", "30"]) == ("simple", "OK"), "EXPIRE ttl")
        vset = c.send(["VSET", "stage10.9 shutdown question", "stage10.9 shutdown answer", "stage10_9"])
        if vset != ("simple", "OK"):
            print(f"stage10_9 optional VSET skipped: {vset!r}")
        for idx in range(128):
            require(c.send(["SET", f"s10_9_bulk_{idx}", "v"]) == ("simple", "OK"), "bulk SET")
            require(c.send(["EXPIRE", f"s10_9_bulk_{idx}", "2"]) == ("simple", "OK"), "bulk EXPIRE")
    else:
        require(c.send(["GET", "s10_9_keep"]) == ("bulk", "alpha"), "reload keep")
        ttl = c.send(["TTL", "s10_9_ttl"])
        require(ttl[0] == "integer" and 1 <= ttl[1] <= 30, f"reload ttl {ttl!r}")
        vsearch = c.send(["VSEARCH", "stage10.9 shutdown question", "stage10_9", "5", "0.0"])
        if vsearch[0] == "error":
            print(f"stage10_9 optional VSEARCH skipped: {vsearch!r}")
        else:
            require(vsearch[0] == "array", f"bad VSEARCH {vsearch!r}")
finally:
    c.close()
PY
}

dynamic_smoke() {
  if [[ ! -x "${KV_BIN}" ]]; then
    echo "未运行动态 smoke：${KV_BIN} 不存在或不可执行"
    return 0
  fi

  local port
  port="${PORT:-$(pick_port)}"
  start_server "${port}"
  run_client_round "${port}" write
  kill -TERM "${SERVER_PID}" >/dev/null 2>&1 || true
  wait "${SERVER_PID}"
  SERVER_PID=""
  check_log

  : >"${LOG_FILE}"
  start_server "${port}"
  run_client_round "${port}" read
  kill -TERM "${SERVER_PID}" >/dev/null 2>&1 || true
  wait "${SERVER_PID}"
  SERVER_PID=""
  check_log

  echo "Stage 10.9 shutdown owner dynamic smoke passed"
}

static_audit
dynamic_smoke
echo "Stage 10.9 shutdown owner audit passed"
