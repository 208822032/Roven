#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

KV_BIN="${KV_BIN:-./kvstore}"

if [[ ! -x "$KV_BIN" ]]; then
  printf '[INFO][build] building kvstore\n'
  make -j2 kvstore
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

BACKENDS = ("core_flat", "core_hnsw_v2")

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

def result_questions(resp):
    questions = []
    for item in flat(resp):
        if item.startswith("question:"):
            questions.append(item.split(":", 1)[1])
    return questions

def assert_ok(resp, label):
    assert not isinstance(resp, dict), f"{label} failed: {resp!r}"
    assert resp == "OK" or "OK" in str(resp), f"{label} expected OK, got {resp!r}"

def assert_no_internal_ids(resp):
    assert not contains(resp, "record_id"), resp
    assert not contains(resp, "slot"), resp

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

def write_config(path, port, rdb, backend, threshold="-1.0", aof=None):
    enable_aof = "yes" if aof else "false"
    aof_lines = ""
    if aof:
        aof_lines = f"aof_fsync_mode = always\naof_filename = {aof}\n"
    path.write_text(f"""port = {port}
role = master
sync_port = {port + 1000}
sync_mode = disabled
network_mode = reactor
enable_aof = {enable_aof}
{aof_lines}rdb_filename = {rdb}
log_level = error
mem_backend = system
embedding_provider = hash
embedding_dim = 64
vector_metric = ip
vector_score_threshold = {threshold}
vector_scope_overfetch_factor = 8
vector_index_type = {backend}
vector_snapshot_enabled = false
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

def run_live_backend_case(tmp, backend):
    port = pick_port()
    conf = tmp / f"{backend}.conf"
    write_config(conf, port, tmp / f"{backend}.rdb", backend)
    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / f"{backend}.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            info = info_map(c.cmd("VINFO"))
            assert info.get("index_type") == backend, info

            for cmd in [
                ("VSET", f"{backend}:alpha", "answer-alpha", "scopeA"),
                ("VSET", f"{backend}:beta", "answer-beta", "scopeA"),
                ("VSET", f"{backend}:gamma", "answer-gamma", "scopeB"),
                ("VSET", f"{backend}:delta", "answer-delta", "scopeB"),
            ]:
                assert_ok(c.cmd(*cmd), " ".join(cmd))

            scope_a = c.cmd("VSEARCH", f"{backend}:alpha", "scopeA", "4")
            scope_b = c.cmd("VSEARCH", f"{backend}:gamma", "scopeB", "4")
            assert contains(scope_a, "scope:scopeA"), scope_a
            assert not contains(scope_a, "scope:scopeB"), scope_a
            assert contains(scope_b, "scope:scopeB"), scope_b
            assert not contains(scope_b, "scope:scopeA"), scope_b
            assert contains(scope_a, f"question:{backend}:alpha"), scope_a
            assert_no_internal_ids(scope_a)

            assert_ok(c.cmd("VSET", f"{backend}:alpha", "answer-alpha-new", "scopeA"), "update alpha")
            updated = c.cmd("VSEARCH", f"{backend}:alpha", "scopeA", "8")
            assert contains(updated, "answer:answer-alpha-new"), updated
            assert count_contains(updated, f"question:{backend}:alpha") == 1, updated

            assert_ok(c.cmd("VDEL", f"{backend}:beta", "scopeA"), "delete beta")
            repeated = c.cmd("VDEL", f"{backend}:beta", "scopeA")
            assert repeated == "NO EXIST" or (
                isinstance(repeated, dict) and "NO EXIST" in repeated.get("error", "")
            ), repeated

            for i in range(32):
                assert_ok(
                    c.cmd("VSET", f"{backend}:reuse:{i}", f"answer-reuse-{i}", "scopeA"),
                    f"reuse insert {i}",
                )
            reuse_search = c.cmd("VSEARCH", f"{backend}:reuse:31", "scopeA", "16")
            assert contains(reuse_search, f"question:{backend}:reuse:31"), reuse_search
            assert not contains(reuse_search, f"question:{backend}:beta"), reuse_search
            assert_no_internal_ids(reuse_search)

            assert_ok(c.cmd("VINDEX", "REBUILD"), "VINDEX REBUILD")
            rebuilt = c.cmd("VSEARCH", f"{backend}:alpha", "scopeA", "16")
            assert contains(rebuilt, f"question:{backend}:alpha"), rebuilt
            assert not contains(rebuilt, f"question:{backend}:beta"), rebuilt
            assert count_contains(rebuilt, f"question:{backend}:alpha") == 1, rebuilt
            assert_no_internal_ids(rebuilt)
        finally:
            c.close()
    finally:
        stop(proc, log)

def run_threshold_case(tmp):
    backend = "core_flat"
    port = pick_port()
    conf = tmp / "threshold.conf"
    write_config(conf, port, tmp / "threshold.rdb", backend, threshold="1000")
    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / "threshold.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert info_map(c.cmd("VINFO")).get("index_type") == backend
            assert_ok(c.cmd("VSET", "threshold-alpha", "answer", "scopeT"), "threshold VSET")
            resp = c.cmd("VSEARCH", "threshold-alpha", "scopeT", "4")
            assert result_questions(resp) == [], resp
        finally:
            c.close()
    finally:
        stop(proc, log)

def run_rdb_case(tmp):
    backend = "core_flat"
    port = pick_port()
    rdb = tmp / "fastpath.rdb"
    conf = tmp / "rdb.conf"
    write_config(conf, port, rdb, backend)

    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / "rdb-first.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert_ok(c.cmd("VSET", "rdb-alive", "answer-rdb", "scopeR"), "rdb alive")
            assert_ok(c.cmd("VSET", "rdb-gone", "answer-gone", "scopeR"), "rdb gone")
            assert_ok(c.cmd("VDEL", "rdb-gone", "scopeR"), "rdb delete")
            assert contains(c.cmd("VSEARCH", "rdb-alive", "scopeR", "4"), "question:rdb-alive")
        finally:
            c.close()
    finally:
        stop(proc, log)

    assert rdb.exists() and rdb.stat().st_size > 0, f"RDB not generated: {rdb}"

    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / "rdb-reload.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert info_map(c.cmd("VINFO")).get("index_type") == backend
            resp = c.cmd("VSEARCH", "rdb-alive", "scopeR", "4")
            assert contains(resp, "question:rdb-alive"), resp
            assert not contains(resp, "question:rdb-gone"), resp
            assert_no_internal_ids(resp)
        finally:
            c.close()
    finally:
        stop(proc, log)

def run_aof_case(tmp):
    backend = "core_hnsw_v2"
    port = pick_port()
    rdb = tmp / "fastpath-aof.rdb"
    aof = tmp / "fastpath.aof"
    conf = tmp / "aof.conf"
    write_config(conf, port, rdb, backend, aof=aof)

    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / "aof-first.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert_ok(c.cmd("VSET", "aof-alive", "answer-aof", "scopeAOF"), "aof alive")
            assert_ok(c.cmd("VSET", "aof-update", "old", "scopeAOF"), "aof update old")
            assert_ok(c.cmd("VSET", "aof-update", "new", "scopeAOF"), "aof update new")
            assert_ok(c.cmd("VSET", "aof-gone", "answer-gone", "scopeAOF"), "aof gone")
            assert_ok(c.cmd("VDEL", "aof-gone", "scopeAOF"), "aof delete")
            assert contains(c.cmd("VSEARCH", "aof-alive", "scopeAOF", "5"), "question:aof-alive")
        finally:
            c.close()
    finally:
        stop(proc, log)

    assert aof.exists() and aof.stat().st_size > 0, f"AOF not generated: {aof}"

    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / "aof-replay.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert info_map(c.cmd("VINFO")).get("index_type") == backend
            resp = c.cmd("VSEARCH", "aof-alive", "scopeAOF", "5")
            assert contains(resp, "question:aof-alive"), resp
            assert contains(c.cmd("VGET", "aof-update", "scopeAOF"), "new")
            assert not contains(c.cmd("VSEARCH", "aof-gone", "scopeAOF", "5"), "question:aof-gone")
            assert_no_internal_ids(resp)
        finally:
            c.close()
    finally:
        stop(proc, log)

with tempfile.TemporaryDirectory(prefix="kvlite_manager_resolve_fastpath_") as td:
    tmp = Path(td)
    for backend in BACKENDS:
        run_live_backend_case(tmp, backend)
    run_threshold_case(tmp)
    run_rdb_case(tmp)
    run_aof_case(tmp)

print("vector_manager_resolve_fastpath_test PASS")
PY
