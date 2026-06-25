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
    assert isinstance(items, list), f"VINFO must return array, got {items!r}"
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

def count_contains(resp, token):
    return sum(1 for item in flat(resp) if token in item)

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

def write_config(path, port, rdb, index_type):
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
vector_index_type = {index_type}
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

def assert_core_hnsw_info(info):
    assert info.get("index_type") == "core_hnsw", info
    assert info.get("core_hnsw_m") == "8", info
    assert info.get("core_hnsw_m0") == "16", info
    assert info.get("core_hnsw_ef_construction") == "64", info
    assert info.get("core_hnsw_ef_search") == "64", info
    for key in (
        "core_hnsw_size",
        "core_hnsw_capacity",
        "core_hnsw_max_level",
        "core_hnsw_deleted_count",
    ):
        assert key in info, info

with tempfile.TemporaryDirectory(prefix="kvlite_core_hnsw_correctness_") as td:
    tmp = Path(td)

    default_port = pick_port()
    default_conf = tmp / "default.conf"
    write_config(default_conf, default_port, tmp / "default.rdb", "hnsw")
    proc, log = start([str(kv_bin), "--config", str(default_conf)], tmp / "default.log")
    try:
        wait_port(default_port)
        c = RespClient(default_port)
        try:
            assert info_map(c.cmd("VINFO")).get("index_type") != "core_hnsw"
        finally:
            c.close()
    finally:
        stop(proc, log)

    cli_port = pick_port()
    cli_conf = tmp / "cli.conf"
    write_config(cli_conf, cli_port, tmp / "cli.rdb", "flat")
    proc, log = start([str(kv_bin), "--config", str(cli_conf), "--vector_index_type", "core_hnsw"], tmp / "cli.log")
    try:
        wait_port(cli_port)
        c = RespClient(cli_port)
        try:
            assert info_map(c.cmd("VINFO")).get("index_type") == "core_hnsw"
        finally:
            c.close()
    finally:
        stop(proc, log)

    port = pick_port()
    conf = tmp / "core_hnsw.conf"
    write_config(conf, port, tmp / "core_hnsw.rdb", "core_hnsw")
    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / "server.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert_core_hnsw_info(info_map(c.cmd("VINFO")))

            for cmd in [
                ("VSET", "q1", "a1", "scopeA"),
                ("VSET", "q2", "a2", "scopeA"),
                ("VSET", "q3", "a3", "scopeB"),
                ("VSET", "q4", "a4", "scopeB"),
            ]:
                assert_ok(c.cmd(*cmd), " ".join(cmd))

            sa = c.cmd("VSEARCH", "q1", "scopeA", "4")
            sb = c.cmd("VSEARCH", "q3", "scopeB", "4")
            assert contains(sa, "scope:scopeA"), sa
            assert not contains(sa, "scope:scopeB"), sa
            assert contains(sb, "scope:scopeB"), sb
            assert not contains(sb, "scope:scopeA"), sb
            assert not contains(sa, "record_id") and not contains(sa, "slot"), sa

            high_threshold = c.cmd("VSEARCH", "q1", "scopeA", "4", "1000")
            assert high_threshold == [] or not contains(high_threshold, "question:"), high_threshold

            assert_ok(c.cmd("VSET", "q1", "a1_new", "scopeA"), "update q1")
            assert contains(c.cmd("VGET", "q1", "scopeA"), "a1_new")
            updated = c.cmd("VSEARCH", "q1", "scopeA", "4")
            assert contains(updated, "answer:a1_new"), updated
            assert count_contains(updated, "question:q1") == 1, updated

            before_delete = info_map(c.cmd("VINFO"))
            assert_core_hnsw_info(before_delete)
            assert int(before_delete.get("core_hnsw_size", "0")) >= 4, before_delete

            assert_ok(c.cmd("VDEL", "q1", "scopeA"), "VDEL q1")
            deleted_search = c.cmd("VSEARCH", "q1", "scopeA", "4")
            assert not contains(deleted_search, "question:q1"), deleted_search
            after_delete = info_map(c.cmd("VINFO"))
            assert after_delete.get("core_hnsw_deleted_count") == "1", after_delete
            second_delete = c.cmd("VDEL", "q1", "scopeA")
            assert second_delete == "NO EXIST" or (isinstance(second_delete, dict) and "NO EXIST" in second_delete.get("error", "")), second_delete
            after_repeat = info_map(c.cmd("VINFO"))
            assert after_repeat.get("core_hnsw_deleted_count") == "1", after_repeat
        finally:
            c.close()
    finally:
        stop(proc, log)

print("vector_core_hnsw_correctness_test PASS")
PY
