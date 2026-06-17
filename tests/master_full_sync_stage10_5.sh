#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage10_5-fullsync.$$}"
KV_BIN="${KV_BIN:-./kvstore}"
MASTER_PID=""
SLAVE_PID=""
MASTER2_PID=""
SLAVE2_PID=""

pick_ports() {
  python3 - <<'PY'
import socket
ports = []
for _ in range(6):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    ports.append(s.getsockname()[1])
    s.close()
print(*ports)
PY
}

stop_pid() {
  local pid="${1:-}"
  if [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1; then
    kill -TERM "${pid}" >/dev/null 2>&1 || true
    for _ in $(seq 1 80); do
      if ! kill -0 "${pid}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${pid}" >/dev/null 2>&1; then
      kill -9 "${pid}" >/dev/null 2>&1 || true
    fi
    wait "${pid}" >/dev/null 2>&1 || true
  fi
}

cleanup() {
  stop_pid "${SLAVE2_PID}"
  stop_pid "${MASTER2_PID}"
  stop_pid "${SLAVE_PID}"
  stop_pid "${MASTER_PID}"
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
embedding_provider = hash
embedding_dim = 64
embedding_num_threads = 1
embedding_max_text_len = 512
embedding_worker_pool_enabled = no
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
embedding_provider = hash
embedding_dim = 64
embedding_num_threads = 1
embedding_max_text_len = 512
embedding_worker_pool_enabled = no
EOF_CONF
}

wait_for_ready() {
  local port="$1"
  local probe_cmd="${2:-SET}"
  local probe_key="${3:-stage10_5_probe}"
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

send_quit() {
  local port="$1"
  python3 - "${HOST}" "${port}" >/dev/null 2>&1 <<'PY' || true
import socket
import sys
host = sys.argv[1]
port = int(sys.argv[2])
s = socket.create_connection((host, port), timeout=5)
s.sendall(b"*1\r\n$4\r\nQUIT\r\n")
try:
    s.recv(64)
except OSError:
    pass
s.close()
PY
}

check_logs_no_crash() {
  local log_file
  for log_file in "$@"; do
    if grep -E "SIGSEGV|segmentation fault|double free|heap-use-after-free|AddressSanitizer|UndefinedBehaviorSanitizer" "${log_file}" >/dev/null 2>&1; then
      echo "crash signature found in ${log_file}" >&2
      exit 1
    fi
  done
}

run_snapshot_replication_suite() {
  local master_port="$1"
  local slave_port="$2"

  python3 - "${HOST}" "${master_port}" "${slave_port}" <<'PY'
import socket
import sys
import threading
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
    def __init__(self, port, timeout=60):
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

def eventually(desc, fn, timeout=25.0, interval=0.2):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        ok, last = fn()
        if ok:
            return last
        time.sleep(interval)
    raise AssertionError(f"{desc}: last={last!r}")

mc = Client(master_port)
sc = Client(slave_port)

try:
    writer_done = {"value": False}

    def writer():
        wc = Client(master_port)
        try:
            require(wc.send(["SET", "during_set", "write_0"]) == ("simple", "OK"), "during SET failed")
            require(wc.send(["MOD", "during_mod", "during_mod_1"]) == ("simple", "OK"), "during MOD failed")
            require(wc.send(["DEL", "during_del"]) == ("simple", "OK"), "during DEL failed")
            for i in range(64):
                require(wc.send(["SET", f"live_{i:02d}", f"live_value_{i:02d}"]) == ("simple", "OK"),
                        f"live SET {i} failed")
                time.sleep(0.01)
            require(wc.send(["MOD", "during_mod", "during_mod_final"]) == ("simple", "OK"), "during final MOD failed")
            writer_done["value"] = True
        finally:
            wc.close()

    t = threading.Thread(target=writer, daemon=True)
    t.start()

    def validate_snapshot_keys():
        require(sc.send(["GET", "snap_set"]) == ("bulk", "value_set"), "snapshot SET missing")
        require(sc.send(["GET", "snap_mod"]) == ("bulk", "value_modded"), "snapshot MOD missing")
        require(sc.send(["GET", "snap_del"]) == ("simple", "NO EXIST"), "snapshot DEL missing")
        require(sc.send(["GET", "snap_persist"]) == ("bulk", "persist_value"), "snapshot PERSIST key missing")
        for i in range(32):
            expect = ("bulk", f"order_value_{i}")
            got = sc.send(["GET", f"order_{i:02d}"])
            require(got == expect, f"snapshot order mismatch {i}: {got!r}")
        return True

    eventually(
        "initial full sync",
        lambda: (sc.send(["GET", "snap_set"]) == ("bulk", "value_set"),
                 sc.send(["GET", "snap_set"]))
    )
    validate_snapshot_keys()
    t.join(timeout=20)
    require(writer_done["value"], "background writer did not finish")

    eventually(
        "incremental SET/MOD/DEL",
        lambda: (
            sc.send(["GET", "during_set"]) == ("bulk", "write_0") and
            sc.send(["GET", "during_mod"]) == ("bulk", "during_mod_final") and
            sc.send(["GET", "during_del"]) == ("simple", "NO EXIST"),
            (
                sc.send(["GET", "during_set"]),
                sc.send(["GET", "during_mod"]),
                sc.send(["GET", "during_del"]),
            )
        ),
        timeout=20.0,
        interval=0.2
    )

    for i in range(64):
        expect = ("bulk", f"live_value_{i:02d}")
        got = eventually(
            f"live key {i}",
            lambda key=f"live_{i:02d}", exp=expect: (sc.send(["GET", key]) == exp, sc.send(["GET", key])),
            timeout=20.0,
            interval=0.1
        )
        require(got == expect, f"live key mismatch {i}: {got!r}")

    time.sleep(7.0)
    require(sc.send(["GET", "snap_persist"]) == ("bulk", "persist_value"),
            "persisted key expired unexpectedly")
    require(sc.send(["GET", "snap_ttl"]) == ("simple", "NO EXIST"),
            "TTL key did not expire after full sync")
finally:
    sc.close()
    mc.close()

print("stage10_5 snapshot replication suite: ok")
PY
}

preload_master_for_snapshot_suite() {
  local master_port="$1"
  python3 - "${HOST}" "${master_port}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
master_port = int(sys.argv[2])

def encode(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)

class Client:
    def __init__(self, timeout=60):
        self.sock = socket.create_connection((host, master_port), timeout=timeout)
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

c = Client()
future_ms = int(time.time() * 1000) + 6000
bulk_value = "X" * 2048
try:
    require(c.send(["SET", "snap_set", "value_set"]) == ("simple", "OK"), "snap_set failed")
    require(c.send(["SET", "snap_mod", "value_old"]) == ("simple", "OK"), "snap_mod set failed")
    require(c.send(["MOD", "snap_mod", "value_modded"]) == ("simple", "OK"), "snap_mod mod failed")
    require(c.send(["SET", "snap_del", "delete_me"]) == ("simple", "OK"), "snap_del set failed")
    require(c.send(["DEL", "snap_del"]) == ("simple", "OK"), "snap_del del failed")
    require(c.send(["SET", "snap_ttl", "ttl_value"]) == ("simple", "OK"), "snap_ttl set failed")
    require(c.send(["PEXPIREAT", "snap_ttl", str(future_ms)]) == ("simple", "OK"), "snap_ttl expire failed")
    require(c.send(["SET", "snap_persist", "persist_value"]) == ("simple", "OK"), "snap_persist set failed")
    require(c.send(["PEXPIREAT", "snap_persist", str(future_ms)]) == ("simple", "OK"), "snap_persist expire failed")
    require(c.send(["PERSIST", "snap_persist"]) == ("simple", "OK"), "snap_persist persist failed")
    require(c.send(["SET", "during_mod", "during_mod_0"]) == ("simple", "OK"), "during_mod preload failed")
    require(c.send(["SET", "during_del", "during_del_0"]) == ("simple", "OK"), "during_del preload failed")
    for i in range(32):
        require(c.send(["SET", f"order_{i:02d}", f"order_value_{i}"]) == ("simple", "OK"),
                f"order preload failed {i}")
    for i in range(800):
        require(c.send(["SET", f"bulk_{i:04d}", bulk_value]) == ("simple", "OK"),
                f"bulk preload failed {i}")
finally:
    c.close()

print("stage10_5 preload: ok")
PY
}

run_shutdown_during_full_sync_suite() {
  local master_port="$1"
  local slave_port="$2"
  local sync_port="$3"
  local data_dir="$4"
  local master_conf="${data_dir}/master_shutdown.conf"
  local slave_conf="${data_dir}/slave_shutdown.conf"
  local master_log="${data_dir}/master_shutdown.log"
  local slave_log="${data_dir}/slave_shutdown.log"

  write_master_conf "${master_port}" "${sync_port}" "${master_conf}" "${data_dir}"
  write_slave_conf "${slave_port}" "${master_port}" "${sync_port}" "${slave_conf}" "${data_dir}"

  (cd "${ROOT_DIR}" && "${KV_BIN}" -c "${master_conf}" >"${master_log}" 2>&1) &
  MASTER2_PID="$!"
  wait_for_ready "${master_port}"
  sleep 0.5

  python3 - "${HOST}" "${master_port}" <<'PY'
import socket
import sys

host = sys.argv[1]
master_port = int(sys.argv[2])

def encode(tokens):
    out = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)

class Client:
    def __init__(self, timeout=60):
        self.sock = socket.create_connection((host, master_port), timeout=timeout)
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

c = Client()
payload = "Y" * 8192
try:
    for i in range(1400):
        require(c.send(["SET", f"shutdown_bulk_{i:04d}", payload]) == ("simple", "OK"),
                f"shutdown preload failed {i}")
finally:
    c.close()

print("stage10_5 shutdown preload: ok")
PY

  (cd "${ROOT_DIR}" && "${KV_BIN}" -c "${slave_conf}" >"${slave_log}" 2>&1) &
  SLAVE2_PID="$!"

  sleep 1.3
  kill -TERM "${MASTER2_PID}" >/dev/null 2>&1 || true

  local exited=0
  for _ in $(seq 1 300); do
    if ! kill -0 "${MASTER2_PID}" >/dev/null 2>&1; then
      exited=1
      break
    fi
    sleep 0.1
  done
  if [[ ${exited} -ne 1 ]]; then
    echo "master did not exit during shutdown/full-sync suite" >&2
    exit 1
  fi
  wait "${MASTER2_PID}" >/dev/null 2>&1 || true
  MASTER2_PID=""

  stop_pid "${SLAVE2_PID}"
  SLAVE2_PID=""

  check_logs_no_crash "${master_log}" "${slave_log}"
  echo "stage10_5 shutdown during full sync: ok"
}

main() {
  rm -rf "${TMP_DIR}"
  mkdir -p "${TMP_DIR}"

  read -r MASTER_PORT SLAVE_PORT SYNC_PORT MASTER2_PORT SLAVE2_PORT SYNC2_PORT < <(pick_ports)
  local master_conf="${TMP_DIR}/master.conf"
  local slave_conf="${TMP_DIR}/slave.conf"
  local master_log="${TMP_DIR}/master.log"
  local slave_log="${TMP_DIR}/slave.log"

  write_master_conf "${MASTER_PORT}" "${SYNC_PORT}" "${master_conf}" "${TMP_DIR}"
  write_slave_conf "${SLAVE_PORT}" "${MASTER_PORT}" "${SYNC_PORT}" "${slave_conf}" "${TMP_DIR}"

  (cd "${ROOT_DIR}" && "${KV_BIN}" -c "${master_conf}" >"${master_log}" 2>&1) &
  MASTER_PID="$!"
  wait_for_ready "${MASTER_PORT}"
  sleep 0.5

  preload_master_for_snapshot_suite "${MASTER_PORT}"

  (cd "${ROOT_DIR}" && "${KV_BIN}" -c "${slave_conf}" >"${slave_log}" 2>&1) &
  SLAVE_PID="$!"
  wait_for_ready "${SLAVE_PORT}" "EXIST"
  sleep 1.0

  run_snapshot_replication_suite "${MASTER_PORT}" "${SLAVE_PORT}"

  send_quit "${SLAVE_PORT}"
  send_quit "${MASTER_PORT}"

  for pid in "${SLAVE_PID}" "${MASTER_PID}"; do
    for _ in $(seq 1 150); do
      if ! kill -0 "${pid}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
  done

  check_logs_no_crash "${master_log}" "${slave_log}"
  echo "stage10_5 master full sync snapshot suite: ok"

  run_shutdown_during_full_sync_suite \
    "${MASTER2_PORT}" \
    "${SLAVE2_PORT}" \
    "${SYNC2_PORT}" \
    "${TMP_DIR}"
}

main "$@"
