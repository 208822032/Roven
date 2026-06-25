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

def write_config(path, port, rdb, aof):
    path.write_text(f"""port = {port}
role = master
sync_port = {port + 1000}
sync_mode = ebpf
network_mode = reactor
enable_aof = yes
aof_fsync_mode = always
aof_filename = {aof}
rdb_filename = {rdb}
log_level = error
mem_backend = system
embedding_provider = hash
embedding_dim = 64
vector_metric = ip
vector_score_threshold = -1.0
vector_scope_overfetch_factor = 4
vector_index_type = core_hnsw
vector_hnsw_m = 8
vector_hnsw_ef_construction = 64
vector_hnsw_ef_search = 64
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

with tempfile.TemporaryDirectory(prefix="kvlite_core_hnsw_aof_") as td:
    tmp = Path(td)
    port = pick_port()
    rdb = tmp / "core_hnsw.rdb"
    aof = tmp / "core_hnsw.aof"
    conf = tmp / "core_hnsw.conf"
    write_config(conf, port, rdb, aof)

    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / "first.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert info_map(c.cmd("VINFO")).get("index_type") == "core_hnsw"
            for cmd in [
                ("VSET", "aof_alive1", "aof_a1", "scopeAOF"),
                ("VSET", "aof_alive2", "aof_a2", "scopeAOF"),
                ("VSET", "aof_update", "old", "scopeAOF"),
                ("VSET", "aof_update", "new", "scopeAOF"),
                ("VSET", "aof_delete", "gone", "scopeAOF"),
            ]:
                assert_ok(c.cmd(*cmd), " ".join(cmd))
            assert_ok(c.cmd("VDEL", "aof_delete", "scopeAOF"), "delete aof_delete")
            assert contains(c.cmd("VSEARCH", "aof_alive1", "scopeAOF", "5"), "question:aof_alive1")
            assert contains(c.cmd("VGET", "aof_update", "scopeAOF"), "new")
        finally:
            c.close()
    finally:
        stop(proc, log)

    assert aof.exists() and aof.stat().st_size > 0, f"AOF not generated: {aof}"

    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / "replay.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            load_info = info_map(c.cmd("VINFO"))
            assert load_info.get("index_type") == "core_hnsw", load_info
            assert contains(c.cmd("VGET", "aof_alive1", "scopeAOF"), "aof_a1")
            assert contains(c.cmd("VGET", "aof_update", "scopeAOF"), "new")
            assert contains(c.cmd("VSEARCH", "aof_alive1", "scopeAOF", "5"), "question:aof_alive1")
            assert contains(c.cmd("VSEARCH", "aof_alive2", "scopeAOF", "5"), "question:aof_alive2")
            assert not contains(c.cmd("VSEARCH", "aof_delete", "scopeAOF", "5"), "question:aof_delete")

            assert_ok(c.cmd("VINDEX", "REBUILD"), "replay VINDEX REBUILD")
            rebuilt = info_map(c.cmd("VINFO"))
            assert rebuilt.get("index_type") == "core_hnsw", rebuilt
            assert rebuilt.get("core_hnsw_deleted_count") == "0", rebuilt
            assert contains(c.cmd("VSEARCH", "aof_alive1", "scopeAOF", "5"), "question:aof_alive1")
            assert not contains(c.cmd("VSEARCH", "aof_delete", "scopeAOF", "5"), "question:aof_delete")
        finally:
            c.close()
    finally:
        stop(proc, log)

print("vector_core_hnsw_aof_replay_test PASS")
PY
