#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"

if [[ ! -x "$KV_BIN" ]]; then
  printf '[INFO][build] building kvstore\n'
  make ENABLE_HNSWLIB="${ENABLE_HNSWLIB:-1}" ENABLE_FAISS="${ENABLE_FAISS:-1}" -j2 kvstore
fi

python3 - "$ROOT_DIR" "$KV_BIN" <<'PY'
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

root = Path(sys.argv[1])
kv_bin = Path(sys.argv[2])
if not kv_bin.is_absolute():
    kv_bin = root / kv_bin
kv_bin = kv_bin.resolve()

def pick_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port

def encode_cmd(*parts):
    out = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        data = str(part).encode()
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)

class RespClient:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.file = self.sock.makefile("rb")

    def close(self):
        self.file.close()
        self.sock.close()

    def cmd(self, *parts):
        self.sock.sendall(encode_cmd(*parts))
        return self.read()

    def line(self):
        line = self.file.readline()
        if not line:
            raise RuntimeError("server closed connection")
        return line[:-2]

    def read(self):
        prefix = self.file.read(1)
        if prefix == b"+":
            return self.line().decode("utf-8", "replace")
        if prefix == b"-":
            return {"error": self.line().decode("utf-8", "replace")}
        if prefix == b":":
            return int(self.line())
        if prefix == b"$":
            n = int(self.line())
            if n < 0:
                return None
            data = self.file.read(n)
            self.file.read(2)
            return data.decode("utf-8", "replace")
        if prefix == b"*":
            n = int(self.line())
            return [self.read() for _ in range(n)]
        raise RuntimeError(f"bad RESP prefix {prefix!r}")

def info_map(items):
    result = {}
    assert isinstance(items, list), items
    for item in items:
        if isinstance(item, str) and ":" in item:
            k, v = item.split(":", 1)
            result[k] = v
    return result

def flat(obj):
    if isinstance(obj, list):
        out = []
        for item in obj:
            out.extend(flat(item))
        return out
    return [str(obj)]

def contains(resp, token):
    return any(token in item for item in flat(resp))

def assert_ok(resp, label):
    assert not isinstance(resp, dict), f"{label} failed: {resp!r}"
    assert resp == "OK" or "OK" in str(resp), f"{label} expected OK, got {resp!r}"

def wait_port(port):
    deadline = time.time() + 8
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            s.close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"port {port} not ready")

def write_config(path, port, rdb):
    path.write_text(f"""port = {port}
role = master
sync_port = {port + 1000}
sync_mode = ebpf
network_mode = reactor
enable_aof = false
rdb_filename = {rdb}
log_level = error
mem_backend = system
embedding_provider = hash
embedding_dim = 64
vector_metric = ip
vector_score_threshold = -1.0
vector_scope_overfetch_factor = 4
vector_index_type = core_flat
""")

def start(args, log_path):
    log = open(log_path, "wb")
    proc = subprocess.Popen(args, cwd=str(root), stdout=log, stderr=subprocess.STDOUT)
    return proc, log

def stop(proc, log):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
    log.close()
    assert proc.returncode in (0, -signal.SIGTERM), f"kvstore exit={proc.returncode}"

with tempfile.TemporaryDirectory(prefix="kvlite_core_flat_rebuild_") as td:
    tmp = Path(td)
    port = pick_port()
    conf = tmp / "core_flat.conf"
    write_config(conf, port, tmp / "core_flat.rdb")
    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / "server.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert info_map(c.cmd("VINFO")).get("index_type") == "core_flat"
            for cmd in [
                ("VSET", "ra1", "aa1", "scopeA"),
                ("VSET", "ra2", "aa2", "scopeA"),
                ("VSET", "rb1", "ab1", "scopeB"),
                ("VSET", "rb2", "ab2", "scopeB"),
                ("VSET", "delA", "goneA", "scopeA"),
                ("VSET", "delB", "goneB", "scopeB"),
            ]:
                assert_ok(c.cmd(*cmd), " ".join(cmd))
            assert_ok(c.cmd("VDEL", "delA", "scopeA"), "delete delA")
            assert_ok(c.cmd("VDEL", "delB", "scopeB"), "delete delB")
            before = info_map(c.cmd("VINFO"))
            assert int(before.get("core_flat_deleted_count", "-1")) >= 2, before

            assert_ok(c.cmd("VINDEX", "REBUILD"), "VINDEX REBUILD")
            after = info_map(c.cmd("VINFO"))
            assert after.get("index_type") == "core_flat", after
            assert after.get("core_flat_deleted_count") == "0", after

            sa = c.cmd("VSEARCH", "ra1", "scopeA", "3")
            sb = c.cmd("VSEARCH", "rb1", "scopeB", "3")
            assert contains(sa, "question:ra1") and contains(sa, "question:ra2"), sa
            assert contains(sb, "question:rb1") and contains(sb, "question:rb2"), sb
            assert not contains(sa, "question:rb1") and not contains(sa, "question:rb2"), sa
            assert not contains(sb, "question:ra1") and not contains(sb, "question:ra2"), sb
            assert not contains(c.cmd("VSEARCH", "delA", "scopeA", "5"), "question:delA")
            assert not contains(c.cmd("VSEARCH", "delB", "scopeB", "5"), "question:delB")
        finally:
            c.close()
    finally:
        stop(proc, log)

print("vector_core_flat_rebuild_test PASS")
PY
