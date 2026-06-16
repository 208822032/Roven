#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-}"
HIGH_ACCEPT_COUNT="${HIGH_ACCEPT_COUNT:-500}"
FD_REUSE_COUNT="${FD_REUSE_COUNT:-300}"
TMP_DIR="${TMP_DIR:-/tmp/kvlite-stage6d-stability.$$}"
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

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

mkdir -p "${TMP_DIR}"
if [[ -z "${PORT}" ]]; then
  PORT="$(pick_port)"
fi

CONF="${TMP_DIR}/kvlite-proactor-6d.conf"
LOG="${TMP_DIR}/kvlite-proactor-6d.log"
cat >"${CONF}" <<EOF_CONF
role = master
port = ${PORT}
sync_port = $((PORT + 1000))
network_mode = proactor
enable_aof = no
aof_fsync_mode = no
aof_filename = ${TMP_DIR}/kvlite.aof
rdb_filename = ${TMP_DIR}/kvlite.rdb
dump_filename = ${TMP_DIR}/kvlite.dat
embedding_provider = hash
vector_index_type = hnsw
vector_metric = ip
vector_hnsw_m = 32
vector_hnsw_ef_construction = 128
vector_hnsw_ef_search = 128
vector_score_threshold = 0.0
log_level = info
log_async = no
EOF_CONF

"${ROOT_DIR}/kvstore" -c "${CONF}" >"${LOG}" 2>&1 &
SERVER_PID="$!"

python3 - "${HOST}" "${PORT}" "${HIGH_ACCEPT_COUNT}" "${FD_REUSE_COUNT}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
high_accept_count = int(sys.argv[3])
fd_reuse_count = int(sys.argv[4])


def encode(tokens):
    chunks = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        data = str(token).encode("utf-8")
        chunks.append(f"${len(data)}\r\n".encode())
        chunks.append(data)
        chunks.append(b"\r\n")
    return b"".join(chunks)


class Client:
    def __init__(self, timeout=5):
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


def wait_ready():
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            c = Client(timeout=1)
            try:
                if c.send(["SET", "stage6d_ready", "1"]) == ("simple", "OK"):
                    return
            finally:
                c.close()
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def require(cond, msg):
    if not cond:
        raise AssertionError(msg)


def high_accept():
    clients = []
    try:
        for i in range(high_accept_count):
            c = Client(timeout=10)
            clients.append(c)
            c.send_raw(encode(["SET", f"ha:{i}", f"v:{i}"]))
        for i, c in enumerate(clients):
            require(c.read() == ("simple", "OK"), f"high accept SET failed at {i}")
    finally:
        for c in clients:
            c.close()
    print(f"high_accept={high_accept_count}: ok")


def fd_reuse():
    for i in range(fd_reuse_count):
        c = Client(timeout=5)
        c.send_raw(encode(["VSET", f"fdreuse-question-{i}", f"fdreuse-answer-{i}", "stage6d_fdreuse"]))
        c.close()
        c2 = Client(timeout=5)
        try:
            require(c2.send(["SET", f"fdreuse:{i}", f"value:{i}"]) == ("simple", "OK"), "SET after fd reuse failed")
            require(c2.send(["GET", f"fdreuse:{i}"]) == ("bulk", f"value:{i}"), "GET after fd reuse mismatch")
        finally:
            c2.close()
    print(f"fd_reuse={fd_reuse_count}: ok")


def pipeline_kv(count):
    c = Client(timeout=15)
    try:
        payload = b""
        expected = []
        for i in range(count):
            payload += encode(["SET", f"pipe:{count}:{i}", f"value:{i}"])
            expected.append(("simple", "OK"))
            payload += encode(["GET", f"pipe:{count}:{i}"])
            expected.append(("bulk", f"value:{i}"))
        c.send_raw(payload)
        got = [c.read() for _ in expected]
        require(got == expected, f"pipeline kv order mismatch count={count}")
    finally:
        c.close()
    print(f"pipeline_kv={count * 2} replies: ok")


def pipeline_vector(count):
    c = Client(timeout=30)
    try:
        payload = b""
        expected_prefix = []
        for i in range(count):
            payload += encode(["VSET", f"pipe-vector-question-{i}", f"pipe-vector-answer-{i}", "stage6d_pipe"])
            expected_prefix.append("simple")
        for i in range(count):
            payload += encode(["VSEARCH", f"pipe-vector-question-{i}", "stage6d_pipe", "5", "0.0"])
            expected_prefix.append("array")
        c.send_raw(payload)
        for i, expected_type in enumerate(expected_prefix):
            resp = c.read()
            require(resp[0] == expected_type, f"pipeline vector response {i} type={resp[0]} expected={expected_type}")
    finally:
        c.close()
    print(f"pipeline_vector={count * 2} replies: ok")


wait_ready()
high_accept()
fd_reuse()
pipeline_kv(10)
pipeline_kv(50)
pipeline_vector(10)
pipeline_vector(50)
print("stage6d proactor stability: ok")
PY

cleanup
SERVER_PID=""

echo "log=${LOG}"
