#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

HOST="${KV_HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage10_10-ttl-scanner.$$}"
SERVER_PID=""
SERVER_PORT=""

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    kill -TERM "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -z "${KEEP_STAGE10_10_TMP:-}" ]]; then
    rm -rf "${TMP_DIR}"
  else
    printf '[INFO][stage10_10] keeping tmp dir: %s\n' "${TMP_DIR}" >&2
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

ttl = read("src/core/kvs_ttl_background.c")
executor = read("src/core/kvs_command_executor.c")
hdr = read("include/kvs_command_executor.h")

ttl_thread = find_func(ttl, "ttl_scan_thread_main")
ttl_stop = find_func(ttl, "kvs_ttl_background_stop")
submit_scan = find_func(executor, "kvs_command_executor_submit_ttl_scan")
run_scan = find_func(executor, "kvs_command_executor_run_ttl_scan")
flush_scan = find_func(executor, "kvs_command_executor_ttl_scan_flush")
expire_candidate = find_func(executor, "kvs_command_executor_ttl_expire_candidate")
exec_scan = find_func(executor, "kvs_command_executor_execute_ttl_scan")
exec_stop = find_func(executor, "kvs_command_executor_stop")
exec_thread = find_func(executor, "kvs_command_executor_thread_main")

for forbidden in [
    "g_kv_data_rwlock",
    "pthread_rwlock_rdlock",
    "pthread_rwlock_wrlock",
    "kvs_get_global_array",
    "kvs_get_global_hash",
    "kvs_get_global_rbtree",
    "kvs_get_global_skiptable",
    "kv_is_expired",
    "ttl_expire_observed_key",
    "ttl_set_any_key_expire_at_ms",
]:
    require(forbidden not in ttl, f"TTL background still directly touches owner data: {forbidden}")

require("kvs_command_executor_submit_ttl_scan(32)" in ttl_thread,
        "TTL background must submit ttl_scan_task")
require("global_dist_config.role == ROLE_SLAVE" in ttl_thread,
        "slave TTL scanner trigger must remain disabled")
require("g_ttl_scan_stop = 1" in ttl_stop and "pthread_join(g_ttl_scan_thread" in ttl_stop,
        "TTL scanner stop must join trigger thread")

for token in [
    "typedef struct kvs_ttl_scan_task_s",
    "ttl_scan_task_submitted",
    "ttl_scan_queue_depth",
    "int kvs_command_executor_submit_ttl_scan(int budget)",
]:
    require(token in hdr, f"header missing {token}")

require("!g_executor.running || g_executor.stopping" in submit_scan,
        "ttl_scan_task submit must reject stopped/stopping executor")
require("ttl_scan_task_submitted" in submit_scan and "ttl_scan_queue_depth" in submit_scan,
        "ttl_scan_task submit must maintain stats/depth")
require("ttl_scan_head" in exec_thread and "kvs_command_executor_execute_ttl_scan" in exec_thread,
        "executor thread must consume ttl_scan_task")
require("pthread_rwlock_wrlock(&g_kv_data_rwlock)" in exec_scan or
        "kvs_owner_rwlock_wrlock(&g_kv_data_rwlock" in exec_scan,
        "executor must own TTL scan under write lock for current stage")
for token in [
    "kvs_get_global_array",
    "kvs_get_global_hash",
    "kvs_get_global_rbtree",
    "kvs_get_global_skiptable",
]:
    require(token in run_scan, f"executor scan missing owner operation {token}")
require("kvs_command_executor_ttl_expire_candidate" in flush_scan,
        "executor scan flush must route candidates through expire candidate helper")
require("ttl_expire_observed_key" in expire_candidate,
        "executor scan flush must re-check and delete with observed expire timestamp")
require("observed_expire_at_ms" in executor,
        "observed expire timestamp ABA guard must remain present")
require("ttl_scan_task = g_executor.ttl_scan_head" in exec_stop and
        "ttl_scan_queue_depth" in exec_stop and
        "ttl_scan_task_dropped" in exec_stop,
        "executor stop must drain/drop ttl_scan_task queue")
require("ttl scanner executor stats:" in executor,
        "executor shutdown log must expose ttl scanner stats")

print("Stage 10.10 TTL scanner owner static audit passed")
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
mem_backend = pool
sync_mode = tcp
log_level = info
log_async = no
embedding_provider = local_c
embedding_model_path = models/bge-small-zh-v1.5/model.onnx
embedding_dim = 512
embedding_worker_pool_enabled = 0
vector_index_type = hnsw
vector_metric = ip
EOF_CONF
}

run_client_suite() {
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
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)

class Client:
    def __init__(self, timeout=20):
        self.s = socket.create_connection((host, port), timeout=timeout)
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
            return ("bulk", data.decode())
        raise RuntimeError(f"bad type {typ!r}")

    def send(self, tokens):
        self.s.sendall(enc(tokens))
        return self.read()

def require(cond, msg):
    if not cond:
        raise AssertionError(msg)

def eventually(fn, timeout=8.0, interval=0.2):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            return True
        time.sleep(interval)
    return False

c = Client()
try:
    require(c.send(["SET", "s10_10_ready", "1"]) == ("simple", "OK"), "ready SET failed")

    for i in range(48):
        require(c.send(["SET", f"s10_10_active_{i}", "v"]) == ("simple", "OK"), "active SET failed")
        require(c.send(["EXPIRE", f"s10_10_active_{i}", "1"]) == ("simple", "OK"), "active EXPIRE failed")

    require(c.send(["SET", "s10_10_aba", "old"]) == ("simple", "OK"), "ABA SET old failed")
    require(c.send(["EXPIRE", "s10_10_aba", "1"]) == ("simple", "OK"), "ABA EXPIRE old failed")
    time.sleep(1.2)
    require(c.send(["SET", "s10_10_aba", "new"]) in [("simple", "EXIST"), ("simple", "OK")],
            "ABA SET new failed")
    require(c.send(["EXPIRE", "s10_10_aba", "30"]) == ("simple", "OK"), "ABA EXPIRE new failed")

    require(c.send(["SET", "s10_10_persist", "v"]) == ("simple", "OK"), "persist SET failed")
    require(c.send(["EXPIRE", "s10_10_persist", "1"]) == ("simple", "OK"), "persist EXPIRE failed")
    require(c.send(["PERSIST", "s10_10_persist"]) == ("simple", "OK"), "persist failed")

    require(c.send(["SET", "s10_10_update", "v"]) == ("simple", "OK"), "update SET failed")
    require(c.send(["EXPIRE", "s10_10_update", "1"]) == ("simple", "OK"), "update EXPIRE short failed")
    time.sleep(0.5)
    require(c.send(["EXPIRE", "s10_10_update", "30"]) == ("simple", "OK"), "update EXPIRE long failed")

    time.sleep(1.4)

    def active_gone():
        return all(c.send(["TTL", f"s10_10_active_{i}"]) == ("integer", -2)
                   for i in range(0, 48, 8))

    require(eventually(active_gone), "active expire did not clear sampled keys")
    require(c.send(["GET", "s10_10_aba"]) == ("bulk", "new"), "ABA key was deleted")
    require(c.send(["GET", "s10_10_persist"]) == ("bulk", "v"), "PERSIST key was deleted")
    ttl = c.send(["TTL", "s10_10_update"])
    require(ttl[0] == "integer" and ttl[1] > 20, f"updated TTL was not preserved: {ttl}")
    c.s.sendall(enc(["QUIT"]))
finally:
    c.close()

print("Stage 10.10 TTL scanner owner client suite passed")
PY
}

wait_for_exit() {
  for _ in $(seq 1 120); do
    if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      wait "${SERVER_PID}" || true
      SERVER_PID=""
      SERVER_PORT=""
      return 0
    fi
    sleep 0.1
  done
  echo "server did not exit" >&2
  return 1
}

verify_stats() {
  local log="$1"
  python3 - "${log}" <<'PY'
import re
import sys

log = sys.argv[1]
ttl = ""
scan = ""
for line in open(log, errors="replace"):
    if "ttl executor stats:" in line:
        ttl = line
    if "ttl scanner executor stats:" in line:
        scan = line
if not ttl:
    raise SystemExit("missing ttl executor stats")
if not scan:
    raise SystemExit("missing ttl scanner executor stats")

def fields(line):
    return {k: int(v) for k, v in re.findall(r"([A-Za-z0-9_]+)=([0-9]+)", line)}

t = fields(ttl)
s = fields(scan)
if s.get("ttl_scan_task_submitted", 0) <= 0:
    raise SystemExit(f"ttl scan submitted not positive: {s}")
if s.get("ttl_scan_task_executed", 0) <= 0:
    raise SystemExit(f"ttl scan executed not positive: {s}")
if s.get("ttl_scan_queue_depth", -1) != 0:
    raise SystemExit(f"ttl scan queue not drained: {s}")
if t.get("ttl_expire_task_submitted", 0) <= 0:
    raise SystemExit(f"ttl expire candidates not positive: {t}")
if t.get("ttl_expire_queue_depth", -1) != 0:
    raise SystemExit(f"legacy ttl expire queue not drained: {t}")
if t.get("ttl_expire_deleted_keys", 0) <= 0:
    raise SystemExit(f"ttl deleted keys not positive: {t}")
print("Stage 10.10 TTL scanner owner stats passed")
PY
}

mkdir -p "${TMP_DIR}"
static_audit

for mode in reactor proactor; do
  cleanup
  SERVER_PID=""
  SERVER_PORT=""
  port="$(pick_port)"
  data_dir="${TMP_DIR}/${mode}_data"
  conf="${TMP_DIR}/${mode}.conf"
  log="${TMP_DIR}/${mode}.log"
  mkdir -p "${data_dir}"
  write_conf "${mode}" "${port}" "${conf}" "${data_dir}"
  ./kvstore -c "${conf}" >"${log}" 2>&1 &
  SERVER_PID="$!"
  SERVER_PORT="${port}"
  python3 - "${HOST}" "${port}" <<'PY'
import socket
import sys
import time
host = sys.argv[1]
port = int(sys.argv[2])
deadline = time.time() + 30
while time.time() < deadline:
    try:
        s = socket.create_connection((host, port), timeout=1)
        s.close()
        raise SystemExit(0)
    except OSError:
        time.sleep(0.1)
raise SystemExit("server not ready")
PY
  run_client_suite "${port}"
  wait_for_exit
  verify_stats "${log}"
  if grep -E "SIGSEGV|segmentation fault|double free|heap-use-after-free|AddressSanitizer|UndefinedBehaviorSanitizer" "${log}" >/dev/null 2>&1; then
    echo "crash signature found in ${log}" >&2
    exit 1
  fi
  echo "${mode} Stage 10.10 TTL scanner owner suite: ok log=${log}"
done

echo "stage10_10 ttl scanner owner: ok tmp=${TMP_DIR}"
