#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN="./build/tests/bin/vector_core_hnsw_v2_default_backend_test"
KV_BIN="${KV_BIN:-./kvstore}"

if [[ ! -x "$BIN" || ! -x "$KV_BIN" ]]; then
  printf '[INFO][build] building hnsw_v2 default backend targets\n'
  make ENABLE_HNSWLIB="${ENABLE_HNSWLIB:-1}" ENABLE_FAISS="${ENABLE_FAISS:-1}" -j2 \
    "$BIN" kvstore
fi

"$BIN"

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

def write_config(path, port, rdb, aof=None, index_type=None):
    aof_enabled = "yes" if aof else "false"
    aof_line = f"aof_filename = {aof}\n" if aof else ""
    index_line = f"vector_index_type = {index_type}\n" if index_type else ""
    path.write_text(f"""port = {port}
role = master
sync_port = {port + 1000}
sync_mode = ebpf
network_mode = reactor
enable_aof = {aof_enabled}
aof_fsync_mode = always
{aof_line}rdb_filename = {rdb}
log_level = error
mem_backend = system
embedding_provider = hash
embedding_dim = 64
vector_metric = ip
vector_score_threshold = -1.0
vector_scope_overfetch_factor = 4
{index_line}vector_hnsw_m = 8
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

def assert_backend(c, expected):
    info = info_map(c.cmd("VINFO"))
    assert info.get("index_type") == expected, info
    assert info.get("backend_name") == expected, info
    return info

def exercise_default_path(c, prefix):
    info = assert_backend(c, "core_hnsw_v2")
    assert info.get("core_hnsw_v2_experimental_enabled") == "0", info
    for key in (
        "core_hnsw_v2_size",
        "core_hnsw_v2_deleted_count",
        "core_hnsw_v2_active_count",
        "core_hnsw_v2_memory_bytes",
        "core_hnsw_v2_ready",
        "rebuild_state",
        "rebuild_delta_count",
        "rebuild_error_code",
    ):
        assert key in info, info

    for cmd in [
        ("VSET", f"{prefix}_alive1", f"{prefix}_a1", "scopeD"),
        ("VSET", f"{prefix}_alive2", f"{prefix}_a2", "scopeD"),
        ("VSET", f"{prefix}_other", "other", "scopeOther"),
        ("VSET", f"{prefix}_update", "old", "scopeD"),
        ("VSET", f"{prefix}_update", "new", "scopeD"),
        ("VSET", f"{prefix}_delete", "gone", "scopeD"),
    ]:
        assert_ok(c.cmd(*cmd), " ".join(cmd))
    assert_ok(c.cmd("VDEL", f"{prefix}_delete", "scopeD"), "delete")
    repeated = c.cmd("VDEL", f"{prefix}_delete", "scopeD")
    assert repeated == "NO EXIST" or isinstance(repeated, dict), repeated

    resp = c.cmd("VSEARCH", f"{prefix}_alive1", "scopeD", "5")
    assert contains(resp, f"question:{prefix}_alive1"), resp
    assert contains(resp, "answer:"), resp
    assert contains(resp, "scope:scopeD"), resp
    assert not contains(resp, "scope:scopeOther"), resp
    assert not contains(resp, "record_id") and not contains(resp, "slot"), resp
    assert contains(c.cmd("VGET", f"{prefix}_update", "scopeD"), "new")

    deleted = c.cmd("VSEARCH", f"{prefix}_delete", "scopeD", "5")
    assert not contains(deleted, f"question:{prefix}_delete"), deleted

    assert_ok(c.cmd("VINDEX", "REBUILD"), "VINDEX REBUILD")
    rebuilt = assert_backend(c, "core_hnsw_v2")
    assert rebuilt.get("core_hnsw_v2_deleted_count") == "0", rebuilt
    assert contains(c.cmd("VSEARCH", f"{prefix}_alive2", "scopeD", "5"), f"question:{prefix}_alive2")
    assert not contains(c.cmd("VSEARCH", f"{prefix}_delete", "scopeD", "5"), f"question:{prefix}_delete")

def smoke_explicit_backend(index_type, expected, tmp, name):
    port = pick_port()
    conf = tmp / f"{name}.conf"
    write_config(conf, port, tmp / f"{name}.rdb", index_type=index_type)
    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / f"{name}.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert_backend(c, expected)
            assert_ok(c.cmd("VSET", f"{name}_q", f"{name}_a", "scopeX"), "explicit VSET")
            assert contains(c.cmd("VSEARCH", f"{name}_q", "scopeX", "3"), f"question:{name}_q")
        finally:
            c.close()
    finally:
        stop(proc, log)

with tempfile.TemporaryDirectory(prefix="kvlite_hnsw_v2_default_") as td:
    tmp = Path(td)

    port = pick_port()
    rdb_conf = tmp / "default_rdb.conf"
    rdb_file = tmp / "default.rdb"
    write_config(rdb_conf, port, rdb_file)
    proc, log = start([str(kv_bin), "--config", str(rdb_conf)], tmp / "default_rdb_first.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            exercise_default_path(c, "rdb")
        finally:
            c.close()
    finally:
        stop(proc, log)
    assert rdb_file.exists() and rdb_file.stat().st_size > 0, rdb_file

    proc, log = start([str(kv_bin), "--config", str(rdb_conf)], tmp / "default_rdb_reload.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert_backend(c, "core_hnsw_v2")
            assert contains(c.cmd("VSEARCH", "rdb_alive1", "scopeD", "5"), "question:rdb_alive1")
            assert not contains(c.cmd("VSEARCH", "rdb_delete", "scopeD", "5"), "question:rdb_delete")
        finally:
            c.close()
    finally:
        stop(proc, log)

    port = pick_port()
    aof_conf = tmp / "default_aof.conf"
    aof_file = tmp / "default.aof"
    write_config(aof_conf, port, tmp / "default_aof.rdb", aof_file)
    proc, log = start([str(kv_bin), "--config", str(aof_conf)], tmp / "default_aof_first.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            exercise_default_path(c, "aof")
        finally:
            c.close()
    finally:
        stop(proc, log)
    assert aof_file.exists() and aof_file.stat().st_size > 0, aof_file

    proc, log = start([str(kv_bin), "--config", str(aof_conf)], tmp / "default_aof_replay.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert_backend(c, "core_hnsw_v2")
            assert contains(c.cmd("VSEARCH", "aof_alive1", "scopeD", "5"), "question:aof_alive1")
            assert not contains(c.cmd("VSEARCH", "aof_delete", "scopeD", "5"), "question:aof_delete")
        finally:
            c.close()
    finally:
        stop(proc, log)

    smoke_explicit_backend("flat", "core_flat", tmp, "explicit_flat")
    smoke_explicit_backend("core_hnsw", "core_hnsw", tmp, "explicit_core_hnsw")

print("vector_core_hnsw_v2_default_backend_test.sh PASS")
PY
