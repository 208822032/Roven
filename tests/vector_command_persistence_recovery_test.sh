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

root = Path(sys.argv[1]).resolve()
kv_bin = Path(sys.argv[2])
if not kv_bin.is_absolute():
    kv_bin = (root / kv_bin).resolve()


def pick_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def encode_cmd(*parts):
    output = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        data = str(part).encode()
        output.append(f"${len(data)}\r\n".encode())
        output.append(data)
        output.append(b"\r\n")
    return b"".join(output)


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
            length = int(self.line())
            if length < 0:
                return None
            data = self.file.read(length)
            self.file.read(2)
            return data.decode("utf-8", "replace")
        if prefix == b"*":
            return [self.read() for _ in range(int(self.line()))]
        raise RuntimeError(f"bad RESP prefix {prefix!r}")


def flatten(value):
    if isinstance(value, list):
        output = []
        for item in value:
            output.extend(flatten(item))
        return output
    return [str(value)]


def contains(value, token):
    return any(token in item for item in flatten(value))


def info_map(value):
    result = {}
    assert isinstance(value, list), value
    for item in value:
        if isinstance(item, str) and ":" in item:
            key, field_value = item.split(":", 1)
            result[key] = field_value
    return result


def assert_ok(value, label):
    assert not isinstance(value, dict), f"{label} failed: {value!r}"
    assert value == "OK" or "OK" in str(value), f"{label}: {value!r}"


def write_config(path, port, rdb_path, aof_path=None):
    enable_aof = "yes" if aof_path else "false"
    aof_config = f"aof_filename = {aof_path}\n" if aof_path else ""
    path.write_text(
        f"""port = {port}
role = master
sync_port = {port + 1000}
sync_mode = disabled
network_mode = reactor
enable_aof = {enable_aof}
aof_fsync_mode = always
{aof_config}rdb_filename = {rdb_path}
log_level = info
mem_backend = system
embedding_provider = hash
embedding_dim = 64
vector_metric = ip
vector_score_threshold = -1.0
vector_scope_overfetch_factor = 4
vector_index_type = core_hnsw_v2
vector_snapshot_enabled = true
vector_snapshot_manifest_path = {path.parent / "vector_index.manifest"}
vector_snapshot_index_path = {path.parent / "vector_index.snapshot"}
vector_hnsw_m = 8
vector_hnsw_ef_construction = 64
vector_hnsw_ef_search = 64
"""
    )


def wait_port(port, proc, log_path):
    deadline = time.time() + 10
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                f"server exited with {proc.returncode}: "
                f"{log_path.read_text(errors='replace')}"
            )
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            sock.close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"port {port} not ready")


def start(case_dir, config, log_name):
    log_path = case_dir / log_name
    log = open(log_path, "wb")
    proc = subprocess.Popen(
        [str(kv_bin), "--config", str(config)],
        cwd=str(case_dir),
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    wait_port(read_port(config), proc, log_path)
    return proc, log


def read_port(config):
    for line in config.read_text().splitlines():
        if line.startswith("port = "):
            return int(line.split("=", 1)[1].strip())
    raise RuntimeError(f"missing port in {config}")


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
        print(log_path.read_text(errors="replace"))
    assert proc.returncode in (0, -signal.SIGTERM), proc.returncode


def remove_snapshots(case_dir):
    for name in (
        "vector_index.manifest",
        "vector_index.manifest.tmp",
        "vector_index.manifest.bak",
        "vector_index.snapshot",
        "vector_index.snapshot.tmp",
        "vector_index.snapshot.bak",
    ):
        path = case_dir / name
        if path.exists():
            assert path.is_file(), path
            path.unlink()


def seed_records(client, prefix):
    commands = (
        ("VSET", f"{prefix}:alive", "answer-alive", "scopeA"),
        ("VSET", f"{prefix}:other", "answer-other", "scopeB"),
        ("VSET", f"{prefix}:update", "old", "scopeA"),
        ("VSET", f"{prefix}:update", "new", "scopeA"),
        ("VSET", f"{prefix}:update", "new", "scopeA"),
        ("VSET", f"{prefix}:gone", "answer-gone", "scopeA"),
    )
    for command in commands:
        assert_ok(client.cmd(*command), " ".join(command))
    assert_ok(client.cmd("VDEL", f"{prefix}:gone", "scopeA"), "VDEL")
    repeated_delete = client.cmd("VDEL", f"{prefix}:gone", "scopeA")
    assert repeated_delete == "NO EXIST" or isinstance(repeated_delete, dict), repeated_delete

    before = info_map(client.cmd("VINFO"))
    assert before.get("active_records") == "3", before
    assert before.get("backend_size") == "3", before

    invalid = client.cmd("VSET", f"{prefix}:invalid")
    assert isinstance(invalid, dict), invalid
    after = info_map(client.cmd("VINFO"))
    assert after.get("active_records") == "3", after
    assert after.get("backend_size") == "3", after


def wait_async_rebuild(client, label):
    deadline = time.time() + 10
    while time.time() < deadline:
        status = client.cmd("VINDEX", "REBUILD", "STATUS")
        assert isinstance(status, str), (label, status)
        assert "rebuild_async_running:" in status, (label, status)
        if "rebuild_async_running:0" in status:
            assert "last_status:1" in status, (label, status)
            return
        time.sleep(0.01)
    raise RuntimeError(f"{label} did not finish")


def exercise_async_rebuild_commands(client):
    initial = client.cmd("VINDEX", "REBUILD", "STATUS")
    assert isinstance(initial, str) and "rebuild_async_running:0" in initial, initial

    assert_ok(client.cmd("VINDEX", "REBUILD", "ASYNC"), "VINDEX REBUILD ASYNC")
    wait_async_rebuild(client, "async rebuild")

    assert_ok(client.cmd("VINDEX", "REBUILD", "RETRY"), "VINDEX REBUILD RETRY")
    wait_async_rebuild(client, "async rebuild retry")

    idle_cancel = client.cmd("VINDEX", "REBUILD", "CANCEL")
    assert isinstance(idle_cancel, dict), idle_cancel


def verify_recovered(client, prefix):
    info = info_map(client.cmd("VINFO"))
    assert info.get("index_type") == "core_hnsw_v2", info
    assert info.get("active_records") == "3", info
    assert info.get("index_size") == "3", info
    assert info.get("backend_size") == "3", info
    assert info.get("core_hnsw_v2_active_count") == "3", info

    assert contains(client.cmd("VGET", f"{prefix}:alive", "scopeA"), "answer-alive")
    assert contains(client.cmd("VGET", f"{prefix}:update", "scopeA"), "new")
    assert not contains(client.cmd("VGET", f"{prefix}:alive", "scopeB"), "answer-alive")
    assert not contains(client.cmd("VGET", f"{prefix}:gone", "scopeA"), "answer-gone")

    result = client.cmd("VSEARCH", f"{prefix}:alive", "scopeA", "10")
    assert contains(result, f"question:{prefix}:alive"), result
    assert contains(result, f"question:{prefix}:update"), result
    assert not contains(result, f"question:{prefix}:gone"), result
    assert not contains(result, "scope:scopeB"), result

    assert info.get("snapshot_load_attempted") == "1", (prefix, info)


def run_case(tmp, name, mode):
    case_dir = tmp / name
    case_dir.mkdir()
    rdb_path = case_dir / "data.rdb"
    aof_path = case_dir / "appendonly.aof" if mode != "rdb" else None
    config = case_dir / "kvstore.conf"
    write_config(config, pick_port(), rdb_path, aof_path)

    proc, log = start(case_dir, config, "first.log")
    try:
        client = RespClient(read_port(config))
        try:
            seed_records(client, name)
            if mode == "rdb":
                exercise_async_rebuild_commands(client)
        finally:
            client.close()
    finally:
        stop(proc, log)

    assert rdb_path.is_file() and rdb_path.stat().st_size > 0, rdb_path
    if aof_path:
        assert aof_path.is_file() and aof_path.stat().st_size > 0, aof_path

    remove_snapshots(case_dir)
    assert not (case_dir / "vector_index.manifest").exists(), name
    assert not (case_dir / "vector_index.snapshot").exists(), name
    if mode == "aof":
        rdb_path.unlink()

    print(f"[TEST] reload {name} mode={mode}")
    proc, log = start(case_dir, config, "reload.log")
    try:
        client = RespClient(read_port(config))
        try:
            verify_recovered(client, name)
        finally:
            client.close()
    finally:
        stop(proc, log)

    reload_log = (case_dir / "reload.log").read_text(errors="replace")
    assert "fallback rebuild reason=manifest missing" in reload_log, reload_log
    assert (case_dir / "vector_index.manifest").is_file(), name
    assert (case_dir / "vector_index.snapshot").is_file(), name


with tempfile.TemporaryDirectory(prefix="kvlite_vector_recovery_") as temp_dir:
    tmp = Path(temp_dir)
    run_case(tmp, "rdb_only", "rdb")
    run_case(tmp, "aof_only", "aof")
    run_case(tmp, "rdb_and_aof", "both")

print("vector_command_persistence_recovery_test.sh PASS")
PY
