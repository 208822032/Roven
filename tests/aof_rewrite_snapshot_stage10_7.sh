#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"
HOST="${KV_HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage10_7-aof.$$}"
CONF_FILE="${TMP_DIR}/kvstore.conf"
LOG_FILE="${TMP_DIR}/kvstore.log"
AOF_FILE="${TMP_DIR}/stage10_7.aof"
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

PORT="${PORT:-$(pick_port)}"

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    kill -TERM "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -z "${KEEP_STAGE10_7_TMP:-}" ]]; then
    rm -rf "${TMP_DIR}"
  else
    printf '[INFO][stage10_7] keeping tmp dir: %s\n' "${TMP_DIR}" >&2
  fi
}
trap cleanup EXIT

mkdir -p "${TMP_DIR}"

write_conf() {
  cat >"${CONF_FILE}" <<EOF_CONF
role = master
port = ${PORT}
sync_port = $((PORT + 1000))
enable_aof = yes
aof_fsync_mode = no
aof_filename = ${AOF_FILE}
rdb_filename = ${TMP_DIR}/stage10_7.rdb
dump_filename = ${TMP_DIR}/stage10_7.dat
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
  python3 - "${HOST}" "${PORT}" <<'PY'
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
        sock.sendall(encode(["EXIST", "__stage10_7_probe__"]))
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
  write_conf
  "$KV_BIN" --config "${CONF_FILE}" >"${LOG_FILE}" 2>&1 &
  SERVER_PID=$!
  wait_for_ready
}

stop_server() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    kill -TERM "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
    SERVER_PID=""
  fi
}

check_logs_no_crash() {
  if grep -E "SIGSEGV|segmentation fault|double free|heap-use-after-free|AddressSanitizer|UndefinedBehaviorSanitizer" "${LOG_FILE}" >/dev/null 2>&1; then
    echo "crash signature found in ${LOG_FILE}" >&2
    exit 1
  fi
}

run_preload_and_shutdown() {
  python3 - "${HOST}" "${PORT}" "${SERVER_PID}" <<'PY'
import os
import signal
import socket
import sys
import threading
import time

host = sys.argv[1]
port = int(sys.argv[2])
server_pid = int(sys.argv[3])

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

ctl = Client(timeout=60)
try:
    require(ctl.send(["SET", "a_keep", "alpha"]) == ("simple", "OK"), "SET a_keep")
    require(ctl.send(["SET", "a_mod", "first"]) == ("simple", "OK"), "SET a_mod")
    require(ctl.send(["MOD", "a_mod", "second"]) == ("simple", "OK"), "MOD a_mod")
    require(ctl.send(["SET", "a_del", "gone"]) == ("simple", "OK"), "SET a_del")
    require(ctl.send(["DEL", "a_del"]) == ("simple", "OK"), "DEL a_del")
    require(ctl.send(["SET", "a_ttl", "live"]) == ("simple", "OK"), "SET a_ttl")
    require(ctl.send(["EXPIRE", "a_ttl", "30"]) == ("simple", "OK"), "EXPIRE a_ttl")
    require(ctl.send(["SET", "a_persist", "persisted"]) == ("simple", "OK"), "SET a_persist")
    require(ctl.send(["EXPIRE", "a_persist", "30"]) == ("simple", "OK"), "EXPIRE a_persist")
    require(ctl.send(["PERSIST", "a_persist"]) == ("simple", "OK"), "PERSIST a_persist")

    require(ctl.send(["RSET", "rb_keep", "beta"]) == ("simple", "OK"), "RSET rb_keep")
    require(ctl.send(["HSET", "h_keep", "gamma"]) == ("simple", "OK"), "HSET h_keep")
    require(ctl.send(["SSET", "s_keep", "delta"]) == ("simple", "OK"), "SSET s_keep")

    for i in range(32):
        require(ctl.send(["SET", f"order_{i:02d}", f"order_value_{i:02d}"]) == ("simple", "OK"),
                f"SET order_{i:02d}")

    bulk_value = "Z" * 2048
    for i in range(512):
        require(ctl.send(["SET", f"bulk_{i:04d}", bulk_value]) == ("simple", "OK"),
                f"SET bulk_{i}")

    require(ctl.send(["VDEL", "stage10_7_q1", "stage10_7_scope1"])[0] == "simple", "preclean q1")
    require(ctl.send(["VDEL", "stage10_7_q2", "stage10_7_scope2"])[0] == "simple", "preclean q2")
    require(ctl.send(["VSET", "stage10_7_q1", "stage10_7_a1", "stage10_7_scope1"]) == ("simple", "OK"), "VSET q1")
    require(ctl.send(["VSET", "stage10_7_q1", "stage10_7_a1_new", "stage10_7_scope1"]) == ("simple", "OK"), "VSET q1 update")
    require(ctl.send(["VSET", "stage10_7_q2", "stage10_7_a2", "stage10_7_scope2"]) == ("simple", "OK"), "VSET q2")
    require(ctl.send(["VDEL", "stage10_7_q2", "stage10_7_scope2"]) == ("simple", "OK"), "VDEL q2")

    writer_state = {"ok": 0, "stopped": False}

    def background_writer():
        c = Client(timeout=5)
        try:
            idx = 0
            while True:
                try:
                    resp = c.send(["SET", f"race_{idx:04d}", f"race_value_{idx:04d}"])
                except Exception:
                    break
                if resp == ("simple", "OK"):
                    writer_state["ok"] += 1
                idx += 1
                time.sleep(0.005)
        finally:
            writer_state["stopped"] = True
            c.close()

    t = threading.Thread(target=background_writer, daemon=True)
    t.start()

    time.sleep(0.3)
    os.kill(server_pid, signal.SIGTERM)
    t.join(timeout=15.0)
    require(writer_state["ok"] > 0, "background writer never committed a write")
finally:
    ctl.close()

print("stage10_7 preload/shutdown: ok")
PY
}

run_reload_validation() {
  python3 - "${HOST}" "${PORT}" <<'PY'
import socket
import sys
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
    require(c.send(["GET", "a_keep"]) == ("bulk", "alpha"), "reload a_keep")
    require(c.send(["GET", "a_mod"]) == ("bulk", "second"), "reload a_mod")
    require(c.send(["GET", "a_del"]) == ("simple", "NO EXIST"), "reload a_del")
    require(c.send(["GET", "a_persist"]) == ("bulk", "persisted"), "reload a_persist")
    require(c.send(["TTL", "a_persist"]) == ("integer", -1), "reload a_persist TTL")

    ttl = c.send(["TTL", "a_ttl"])
    require(ttl[0] == "integer" and 1 <= ttl[1] <= 30, f"reload a_ttl TTL unexpected: {ttl!r}")
    require(c.send(["GET", "a_ttl"]) == ("bulk", "live"), "reload a_ttl")

    require(c.send(["RGET", "rb_keep"]) == ("bulk", "beta"), "reload rb_keep")
    require(c.send(["HGET", "h_keep"]) == ("bulk", "gamma"), "reload h_keep")
    require(c.send(["SGET", "s_keep"]) == ("bulk", "delta"), "reload s_keep")

    for i in range(32):
        expect = ("bulk", f"order_value_{i:02d}")
        require(c.send(["GET", f"order_{i:02d}"]) == expect, f"reload order_{i:02d}")

    require(c.send(["GET", "bulk_0000"]) == ("bulk", "Z" * 2048), "reload bulk_0000")
    require(c.send(["GET", "bulk_0511"]) == ("bulk", "Z" * 2048), "reload bulk_0511")

    require(c.send(["VGET", "stage10_7_q1", "stage10_7_scope1"]) == ("bulk", "stage10_7_a1_new"), "reload VGET q1")
    require(c.send(["VGET", "stage10_7_q2", "stage10_7_scope2"]) == ("simple", "NO EXIST"), "reload VGET q2")
finally:
    c.close()

print("stage10_7 reload validation: ok")
PY
}

start_server
run_preload_and_shutdown
wait "${SERVER_PID}"
SERVER_PID=""
check_logs_no_crash

start_server
run_reload_validation
stop_server
check_logs_no_crash

printf '[PASS][stage10_7] AOF rewrite snapshot save/load regression completed successfully\n'
