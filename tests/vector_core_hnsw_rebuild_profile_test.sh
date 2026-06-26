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

PROFILE_KEYS = [
    "rebuild_total_us",
    "rebuild_manager_scan_us",
    "rebuild_core_reset_us",
    "rebuild_core_reserve_us",
    "rebuild_core_add_total_us",
    "rebuild_core_add_avg_us",
    "rebuild_core_add_max_us",
    "rebuild_core_add_count",
    "hnsw_add_random_level_us",
    "hnsw_add_greedy_search_us",
    "hnsw_add_layer_search_us",
    "hnsw_add_neighbor_select_us",
    "hnsw_add_link_us",
    "hnsw_add_vector_copy_us",
    "neighbor_select_call_count",
    "neighbor_select_level0_call_count",
    "neighbor_select_upper_call_count",
    "neighbor_select_candidate_total",
    "neighbor_select_candidate_max",
    "neighbor_select_selected_total",
    "neighbor_select_selected_max",
    "neighbor_select_reject_count",
    "neighbor_select_heuristic_prune_count",
    "neighbor_select_distance_recompute_count",
    "neighbor_select_reused_candidate_score_count",
    "neighbor_select_duplicate_scan_count",
    "neighbor_select_worst_scan_count",
    "neighbor_select_worst_cache_hit_count",
    "neighbor_select_level0_candidate_total",
    "neighbor_select_upper_candidate_total",
    "neighbor_select_level0_selected_total",
    "neighbor_select_upper_selected_total",
    "neighbor_select_level0_reject_count",
    "neighbor_select_upper_reject_count",
    "neighbor_select_level0_heuristic_prune_count",
    "neighbor_select_upper_heuristic_prune_count",
    "neighbor_select_level0_distance_recompute_count",
    "neighbor_select_upper_distance_recompute_count",
    "neighbor_select_level0_reused_candidate_score_count",
    "neighbor_select_upper_reused_candidate_score_count",
    "neighbor_select_level0_duplicate_scan_count",
    "neighbor_select_upper_duplicate_scan_count",
    "neighbor_select_level0_worst_scan_count",
    "neighbor_select_upper_worst_scan_count",
    "neighbor_select_level0_worst_cache_hit_count",
    "neighbor_select_upper_worst_cache_hit_count",
]

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
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
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
vector_distance_backend = auto
vector_metric = ip
vector_score_threshold = -1.0
vector_scope_overfetch_factor = 4
vector_hnsw_initial_capacity = 128
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

with tempfile.TemporaryDirectory(prefix="kvlite_core_hnsw_rebuild_profile_") as td:
    tmp = Path(td)
    port = pick_port()
    conf = tmp / "core_hnsw_profile.conf"
    write_config(conf, port, tmp / "core_hnsw_profile.rdb")
    proc, log = start([str(kv_bin), "--config", str(conf)], tmp / "server.log")
    try:
        wait_port(port)
        c = RespClient(port)
        try:
            assert info_map(c.cmd("VINFO")).get("index_type") == "core_hnsw"
            for i in range(1000):
                scope = "scopeA" if i % 2 == 0 else "scopeB"
                assert_ok(
                    c.cmd("VSET", f"profile_q_{i:04d}", f"profile_a_{i:04d}", scope),
                    f"VSET {i}",
                )
            assert_ok(c.cmd("VDEL", "profile_q_0002", "scopeA"), "delete profile_q_0002")
            assert_ok(c.cmd("VDEL", "profile_q_0003", "scopeB"), "delete profile_q_0003")

            assert_ok(c.cmd("VINDEX", "REBUILD"), "VINDEX REBUILD")
            info = info_map(c.cmd("VINFO"))
            assert info.get("index_type") == "core_hnsw", info
            assert info.get("distance_backend") in ("avx2", "fallback"), info
            assert info.get("dimension") == "64", info
            assert info.get("backend_dim") == "64", info
            for key in PROFILE_KEYS:
                assert key in info, f"missing {key}: {info}"
                int(info[key])
            assert int(info["rebuild_total_us"]) > 0, info
            assert int(info["rebuild_core_add_total_us"]) > 0, info
            assert int(info["rebuild_core_add_count"]) == 998, info
            assert int(info["rebuild_core_add_max_us"]) >= int(info["rebuild_core_add_avg_us"]), info
            assert int(info["hnsw_add_layer_search_us"]) > 0, info
            assert int(info["hnsw_add_link_us"]) > 0, info
            assert int(info["hnsw_add_vector_copy_us"]) > 0, info
            assert int(info["core_hnsw_deleted_count"]) == 0, info
            assert int(info["core_hnsw_size"]) == 998, info

            sa = c.cmd("VSEARCH", "profile_q_0010", "scopeA", "10")
            sb = c.cmd("VSEARCH", "profile_q_0011", "scopeB", "10")
            assert contains(sa, "scope:scopeA"), sa
            assert not contains(sa, "scope:scopeB"), sa
            assert contains(sb, "scope:scopeB"), sb
            assert not contains(sb, "scope:scopeA"), sb
            assert not contains(c.cmd("VSEARCH", "profile_q_0002", "scopeA", "10"), "question:profile_q_0002")
            assert not contains(c.cmd("VSEARCH", "profile_q_0003", "scopeB", "10"), "question:profile_q_0003")
        finally:
            c.close()
    finally:
        stop(proc, log)

print("vector_core_hnsw_rebuild_profile_test PASS")
PY
