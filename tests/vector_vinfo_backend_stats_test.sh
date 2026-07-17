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

BACKEND_STATS_KEYS = (
    "backend_name",
    "backend_size",
    "backend_capacity",
    "backend_dim",
    "backend_metric",
    "backend_memory_bytes",
    "backend_memory_bytes_source",
    "backend_deleted_count",
    "backend_needs_rebuild",
    "backend_resize_count",
    "backend_reserve_count",
    "backend_resize_count_source",
    "backend_reserve_count_source",
)
VSEARCH_PROFILE_KEYS = (
    "vsearch_total_count",
    "vsearch_total_avg_us",
    "vsearch_total_us",
    "vsearch_total_max_us",
    "vsearch_backend_search_count",
    "vsearch_backend_search_avg_us",
    "vsearch_backend_search_total_us",
    "vsearch_backend_search_max_us",
    "vsearch_manager_resolve_count",
    "vsearch_manager_resolve_avg_us",
    "vsearch_manager_resolve_total_us",
    "vsearch_manager_resolve_max_us",
    "vsearch_response_assembly_count",
    "vsearch_response_assembly_avg_us",
    "vsearch_response_assembly_total_us",
    "vsearch_response_assembly_max_us",
    "vsearch_raw_hit_count",
    "vsearch_resolved_hit_count",
    "vsearch_filtered_hit_count",
)
SNAPSHOT_STATUS_KEYS = (
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
sync_mode = disabled
network_mode = reactor
enable_aof = false
rdb_filename = {rdb}
log_level = error
mem_backend = system
embedding_provider = hash
embedding_dim = 64
vector_metric = ip
vector_score_threshold = -1.0
vector_scope_overfetch_factor = 8
vector_index_type = {backend}
vector_snapshot_enabled = false
vector_hnsw_m = 8
vector_hnsw_ef_construction = 64
vector_hnsw_ef_search = 64
vector_hnsw_initial_capacity = 256
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

def assert_backend_stats(info, backend):
    for key in BACKEND_STATS_KEYS + VSEARCH_PROFILE_KEYS + SNAPSHOT_STATUS_KEYS:
        assert key in info, f"{backend} missing VINFO key {key}: {info}"

    assert info["backend_name"] == backend, info
    assert info_int(info, "backend_size") >= 8, info
    assert info_int(info, "backend_capacity") >= info_int(info, "backend_size"), info
    assert info_int(info, "backend_dim") == 64, info
    assert info["backend_metric"] == "ip", info
    assert info["backend_resize_count"] == "n/a", info
    assert info["backend_reserve_count"] == "n/a", info
    assert info["backend_resize_count_source"] == "unsupported", info
    assert info["backend_reserve_count_source"] == "unsupported", info

    source = info["backend_memory_bytes_source"]
    assert source in ("measured", "estimated", "unsupported"), info
    if backend in ("core_flat", "core_hnsw_v2"):
        assert source == "measured", info
        assert info_int(info, "backend_memory_bytes") > 0, info
    else:
        assert source in ("estimated", "unsupported"), info
        if source == "estimated":
            assert info_int(info, "backend_memory_bytes") > 0, info

    if backend == "core_flat":
        assert info_int(info, "snapshot_supported") == 0, info
        for key in [
            "core_flat_size",
            "core_flat_capacity",
            "core_flat_memory_bytes",
            "core_flat_deleted_count",
        ]:
            assert key in info, f"{backend} missing {key}: {info}"
    elif backend == "core_hnsw_v2":
        assert info_int(info, "snapshot_supported") == 1, info

def run_backend(tmp, backend):
    port = pick_port()
    conf = tmp / f"{backend}.conf"
    write_config(conf, port, tmp / f"{backend}.rdb", backend)
    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / f"{backend}.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            info0 = info_map(c.cmd("VINFO"))
            actual_backend = backend
            if info0.get("index_type") != actual_backend:
                raise AssertionError(f"{backend} unavailable or mapped to {info0.get('index_type')}: {info0}")

            for i in range(16):
                scope = "scopeA" if i % 2 == 0 else "scopeB"
                assert_ok(c.cmd("VSET", f"{backend}:q{i}", f"{backend}:a{i}", scope), f"VSET {i}")

            response = c.cmd("VSEARCH", f"{backend}:q0", "scopeA", "5")
            assert contains(response, f"question:{backend}:q0"), response
            assert not contains(response, "record_id"), response
            assert not contains(response, "slot"), response

            info = info_map(c.cmd("VINFO"))
            assert_backend_stats(info, actual_backend)
            assert info_int(info, "vsearch_total_count") > 0, info
            assert info_int(info, "vsearch_backend_search_count") > 0, info
            assert info_int(info, "vsearch_manager_resolve_count") > 0, info
            assert info_int(info, "vsearch_response_assembly_count") > 0, info
            assert info_int(info, "vsearch_raw_hit_count") >= info_int(info, "vsearch_resolved_hit_count"), info
        finally:
            c.close()
    finally:
        stop(proc, log)

with tempfile.TemporaryDirectory(prefix="kvlite_vinfo_backend_stats_") as td:
    tmp = Path(td)
    for backend in BACKENDS:
        run_backend(tmp, backend)

print("vector_vinfo_backend_stats_test PASS")
PY
