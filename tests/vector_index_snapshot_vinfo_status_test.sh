#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"
BIN="./build/tests/bin/vector_index_snapshot_vinfo_status_test"

if [[ ! -x "$KV_BIN" || ! -x "$BIN" ]]; then
  printf '[INFO][build] building vector_index_snapshot_vinfo_status targets\n'
  make -j2 "$BIN" kvstore
fi

"$BIN"

python3 - "$ROOT_DIR" "$KV_BIN" <<'PY'
import os
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

SNAPSHOT_KEYS = (
    "snapshot_supported",
    "snapshot_load_attempted",
    "snapshot_loaded",
    "snapshot_valid",
    "loaded_from_snapshot",
    "snapshot_fallback_rebuild_count",
    "snapshot_last_error",
    "snapshot_write_attempted",
    "snapshot_written",
    "snapshot_last_write_error",
    "snapshot_last_write_started_ms",
    "snapshot_last_write_finished_ms",
    "snapshot_last_bytes",
    "snapshot_last_checksum",
)

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

def flat(obj):
    if isinstance(obj, list):
        out = []
        for item in obj:
            out.extend(flat(item))
        return out
    return [str(obj)]

def contains(resp, token):
    return any(token in item for item in flat(resp))

def info_map(items):
    result = {}
    assert isinstance(items, list), f"VINFO must return array, got {items!r}"
    for item in items:
        if isinstance(item, str) and ":" in item:
            key, value = item.split(":", 1)
            result[key] = value
    return result

def info_int(info, key):
    return int(info.get(key, "0") or "0")

def assert_ok(resp, label):
    assert not isinstance(resp, dict), f"{label} failed: {resp!r}"
    assert resp == "OK" or "OK" in str(resp), f"{label} expected OK, got {resp!r}"

def assert_snapshot_keys(info):
    for key in SNAPSHOT_KEYS:
        assert key in info, f"missing {key}: {info}"

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

def write_config(path, port, rdb, backend):
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
vector_index_type = {backend}
vector_hnsw_m = 8
vector_hnsw_ef_construction = 64
vector_hnsw_ef_search = 64
vector_hnsw_initial_capacity = 256
""")

def start(conf, log_path):
    log = open(log_path, "wb")
    proc = subprocess.Popen(
        [str(kv_bin), "--config", str(conf)],
        cwd=str(root),
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    wait_port(read_port(conf))
    return proc, log

def read_port(conf):
    for line in conf.read_text().splitlines():
        if line.startswith("port = "):
            return int(line.split("=", 1)[1].strip())
    raise RuntimeError(f"missing port in {conf}")

def stop(proc, log):
    log_path = Path(log.name)
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
    log.close()
    if proc.returncode not in (0, -signal.SIGTERM):
        try:
            print(log_path.read_text(errors="replace"))
        except OSError:
            pass
    assert proc.returncode in (0, -signal.SIGTERM), f"kvstore exit={proc.returncode}"

def open_client(conf):
    return RespClient(read_port(conf))

def write_records(c, prefix):
    for cmd in [
        ("VSET", f"{prefix}_a", "answer-a", "scopeS"),
        ("VSET", f"{prefix}_b", "answer-b", "scopeS"),
        ("VSET", f"{prefix}_c", "answer-c", "scopeOther"),
    ]:
        assert_ok(c.cmd(*cmd), " ".join(cmd))
    assert contains(c.cmd("VSEARCH", f"{prefix}_a", "scopeS", "3"), f"question:{prefix}_a")

def assert_search_works(c, prefix):
    assert contains(c.cmd("VSEARCH", f"{prefix}_a", "scopeS", "3"), f"question:{prefix}_a")

def unlink_snapshot_files(snapshot_dir):
    for name in (
        "vector_index.manifest",
        "vector_index.manifest.tmp",
        "vector_index.manifest.bak",
        "vector_index.snapshot",
        "vector_index.snapshot.tmp",
        "vector_index.snapshot.bak",
    ):
        path = snapshot_dir / name
        try:
            if path.is_dir():
                path.rmdir()
            else:
                path.unlink()
        except FileNotFoundError:
            pass

def run_fresh_no_snapshot(tmp):
    conf = tmp / "fresh.conf"
    unlink_snapshot_files(root)
    write_config(conf, pick_port(), tmp / "fresh.rdb", "core_hnsw_v2")
    proc, log = start(conf, tmp / "fresh.log")
    try:
        c = open_client(conf)
        try:
            info = info_map(c.cmd("VINFO"))
            assert_snapshot_keys(info)
            assert info.get("index_type") == "core_hnsw_v2", info
            assert info_int(info, "snapshot_supported") == 1, info
            assert info_int(info, "snapshot_loaded") == 0, info
            assert info_int(info, "loaded_from_snapshot") == 0, info
        finally:
            c.close()
    finally:
        stop(proc, log)

def run_non_v2(tmp):
    conf = tmp / "flat.conf"
    unlink_snapshot_files(root)
    write_config(conf, pick_port(), tmp / "flat.rdb", "core_flat")
    proc, log = start(conf, tmp / "flat.log")
    try:
        c = open_client(conf)
        try:
            info = info_map(c.cmd("VINFO"))
            assert_snapshot_keys(info)
            assert info.get("index_type") == "core_flat", info
            assert info_int(info, "snapshot_supported") == 0, info
            assert info_int(info, "snapshot_load_attempted") == 0, info
            assert info_int(info, "snapshot_write_attempted") == 0, info
        finally:
            c.close()
    finally:
        stop(proc, log)

def run_snapshot_status_lifecycle(tmp):
    snapshot_dir = root
    rdb = tmp / "v2.rdb"
    conf = tmp / "v2.conf"
    unlink_snapshot_files(snapshot_dir)
    write_config(conf, pick_port(), rdb, "core_hnsw_v2")

    proc, log = start(conf, tmp / "v2_first.log")
    try:
        c = open_client(conf)
        try:
            write_records(c, "snap")
            assert_ok(c.cmd("VINDEX", "REBUILD"), "initial VINDEX REBUILD")
            info = info_map(c.cmd("VINFO"))
            assert info_int(info, "snapshot_supported") == 1, info
            assert info_int(info, "snapshot_write_attempted") == 1, info
            assert info_int(info, "snapshot_written") == 1, info
            assert info_int(info, "snapshot_last_bytes") > 0, info
            assert info_int(info, "snapshot_last_checksum") != 0, info
        finally:
            c.close()
    finally:
        stop(proc, log)
    assert rdb.exists() and rdb.stat().st_size > 0, rdb

    proc, log = start(conf, tmp / "v2_valid_snapshot_reload.log")
    try:
        c = open_client(conf)
        try:
            info = info_map(c.cmd("VINFO"))
            assert info_int(info, "snapshot_load_attempted") == 1, info
            assert info_int(info, "snapshot_loaded") == 1, info
            assert info_int(info, "snapshot_valid") == 1, info
            assert info_int(info, "loaded_from_snapshot") == 1, info
            assert info_int(info, "snapshot_write_attempted") == 0, info
            assert_search_works(c, "snap")
        finally:
            c.close()
    finally:
        stop(proc, log)

    (snapshot_dir / "vector_index.snapshot.tmp").mkdir()
    proc, log = start(conf, tmp / "v2_write_failure.log")
    try:
        c = open_client(conf)
        try:
            assert_ok(c.cmd("VINDEX", "REBUILD"), "write failure VINDEX REBUILD")
            info = info_map(c.cmd("VINFO"))
            assert info_int(info, "snapshot_write_attempted") == 1, info
            assert info_int(info, "snapshot_written") == 0, info
            assert info.get("snapshot_last_write_error"), info
            assert info_int(info, "snapshot_last_bytes") == 0, info
            assert info_int(info, "snapshot_last_checksum") == 0, info
            assert_search_works(c, "snap")
        finally:
            c.close()
    finally:
        stop(proc, log)
        os.rmdir(snapshot_dir / "vector_index.snapshot.tmp")

def backup_root_snapshot_files(tmp):
    backup = tmp / "root_snapshot_backup"
    backup.mkdir()
    names = (
        "vector_index.manifest",
        "vector_index.manifest.tmp",
        "vector_index.manifest.bak",
        "vector_index.snapshot",
        "vector_index.snapshot.tmp",
        "vector_index.snapshot.bak",
    )
    for name in names:
        src = root / name
        if src.exists():
            dst = backup / name
            src.rename(dst)
    return backup

def restore_root_snapshot_files(backup):
    unlink_snapshot_files(root)
    for src in backup.iterdir():
        src.rename(root / src.name)

with tempfile.TemporaryDirectory(prefix="kvlite_snapshot_vinfo_") as td:
    tmp = Path(td)
    backup = backup_root_snapshot_files(tmp)
    try:
        run_fresh_no_snapshot(tmp)
        run_non_v2(tmp)
        run_snapshot_status_lifecycle(tmp)
    finally:
        restore_root_snapshot_files(backup)

print("vector_index_snapshot_vinfo_status_test PASS")
PY
