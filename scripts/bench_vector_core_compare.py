#!/usr/bin/env python3
import argparse
import json
import os
import re
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


DEFAULT_BACKENDS = [
    "core_flat",
    "core_hnsw",
    "hnswlib_hnsw",
    "faiss_flat",
    "faiss_hnsw",
]

BACKEND_STATS_KEYS = [
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
]

VSEARCH_PROFILE_KEYS = [
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
]

REBUILD_PROFILE_KEYS = [
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
    # Stage 7A: event counters for memory access pattern audit
    "add_distance_compute_count",
    "add_neighbor_visit_count",
    "add_visited_skip_count",
    "add_active_check_count",
    "add_top_heap_push_count",
    "add_top_heap_replace_count",
    "add_candidate_push_count",
    "add_level0_neighbor_scan_count",
    "add_upper_neighbor_scan_count",
]

MEASURED_MEMORY_BACKENDS = {"core_flat", "core_hnsw"}
EXTERNAL_MEMORY_BACKENDS = {"hnswlib_hnsw", "faiss_flat", "faiss_hnsw"}


def pick_port() -> int:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def encode_cmd(*parts: Any) -> bytes:
    out = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        data = str(part).encode()
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)


class RespClient:
    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=60)
        self.file = self.sock.makefile("rb")

    def close(self) -> None:
        self.file.close()
        self.sock.close()

    def cmd(self, *parts: Any) -> Any:
        self.sock.sendall(encode_cmd(*parts))
        return self.read()

    def line(self) -> bytes:
        line = self.file.readline()
        if not line:
            raise RuntimeError("server closed connection")
        return line[:-2]

    def read(self) -> Any:
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


def flatten(obj: Any) -> List[str]:
    if isinstance(obj, list):
        out: List[str] = []
        for item in obj:
            out.extend(flatten(item))
        return out
    return [str(obj)]


def contains(resp: Any, token: str) -> bool:
    return any(token in item for item in flatten(resp))


def info_map(items: Any) -> Dict[str, str]:
    result: Dict[str, str] = {}
    if not isinstance(items, list):
        return result
    for item in items:
        if isinstance(item, str) and ":" in item:
            key, value = item.split(":", 1)
            result[key] = value
    return result


def assert_ok(resp: Any, label: str) -> None:
    if isinstance(resp, dict):
        raise RuntimeError(f"{label} failed: {resp!r}")
    if resp != "OK" and "OK" not in str(resp):
        raise RuntimeError(f"{label} expected OK, got {resp!r}")


def percentile(values: List[float], pct: float) -> Optional[float]:
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    pos = (pct / 100.0) * (len(ordered) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    if lo == hi:
        return ordered[lo]
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def latency_summary(values: List[float]) -> Dict[str, Optional[float]]:
    if not values:
        return {"avg_ms": None, "p50_ms": None, "p95_ms": None}
    return {
        "avg_ms": statistics.fmean(values),
        "p50_ms": percentile(values, 50),
        "p95_ms": percentile(values, 95),
    }


def int_info(info: Dict[str, str], key: str) -> Optional[int]:
    value = info.get(key)
    if value is None or value == "":
        return None
    try:
        return int(value)
    except ValueError:
        return None


def vsearch_profile_summary(info: Dict[str, str]) -> Dict[str, Optional[int]]:
    return {key: int_info(info, key) for key in VSEARCH_PROFILE_KEYS}


def rebuild_profile_summary(info: Dict[str, str]) -> Dict[str, Optional[int]]:
    return {key: int_info(info, key) for key in REBUILD_PROFILE_KEYS}


def backend_stats_summary(info: Dict[str, str]) -> Dict[str, Any]:
    keys = [
        "backend_name",
        "backend_metric",
        "backend_memory_bytes_source",
        "backend_resize_count",
        "backend_reserve_count",
        "backend_resize_count_source",
        "backend_reserve_count_source",
    ]
    out: Dict[str, Any] = {key: info.get(key) for key in keys}
    for key in [
        "backend_size",
        "backend_capacity",
        "backend_dim",
        "backend_memory_bytes",
        "backend_deleted_count",
        "backend_needs_rebuild",
    ]:
        out[key] = int_info(info, key)
    source = out.get("backend_memory_bytes_source")
    memory = out.get("backend_memory_bytes")
    out["backend_memory_warning"] = (
        "unsupported" if source == "unsupported" or memory is None else
        "estimated" if source == "estimated" else
        ""
    )
    return out


def validate_stats_fields(backend: str, info: Dict[str, str]) -> Dict[str, Any]:
    missing_backend = [key for key in BACKEND_STATS_KEYS if key not in info]
    missing_profile = [key for key in VSEARCH_PROFILE_KEYS if key not in info]
    memory_source = info.get("backend_memory_bytes_source", "")
    memory_bytes = int_info(info, "backend_memory_bytes")

    memory_ok = memory_source in {"measured", "estimated", "unsupported"}
    if backend in MEASURED_MEMORY_BACKENDS:
        memory_ok = memory_ok and memory_source == "measured" and (memory_bytes or 0) > 0
    if backend in EXTERNAL_MEMORY_BACKENDS:
        memory_ok = memory_ok and memory_source in {"estimated", "unsupported"}

    resize_ok = (
        info.get("backend_resize_count") == "n/a"
        and info.get("backend_reserve_count") == "n/a"
        and info.get("backend_resize_count_source") == "unsupported"
        and info.get("backend_reserve_count_source") == "unsupported"
    )

    raw_hits = int_info(info, "vsearch_raw_hit_count")
    resolved_hits = int_info(info, "vsearch_resolved_hit_count")
    profile_ok = (
        not missing_profile
        and (int_info(info, "vsearch_total_count") or 0) > 0
        and (int_info(info, "vsearch_backend_search_count") or 0) > 0
        and (int_info(info, "vsearch_manager_resolve_count") or 0) > 0
        and (int_info(info, "vsearch_response_assembly_count") or 0) > 0
        and raw_hits is not None
        and resolved_hits is not None
        and raw_hits >= resolved_hits
    )

    return {
        "missing_backend_stats_keys": missing_backend,
        "missing_vsearch_profile_keys": missing_profile,
        "backend_stats_fields_ok": not missing_backend,
        "vsearch_profile_fields_ok": profile_ok,
        "memory_source_ok": memory_ok,
        "resize_reserve_source_ok": resize_ok,
    }


def timed_cmd(client: RespClient, *parts: Any) -> Tuple[Any, float]:
    start = time.perf_counter()
    resp = client.cmd(*parts)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return resp, elapsed_ms


def wait_port(port: int, timeout: float = 120.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            sock.close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"port {port} not ready")


def write_config(
    path: Path,
    port: int,
    rdb: Path,
    backend: str,
    dim: int,
    n: int,
    m: int,
    ef_construction: int,
    ef_search: int,
) -> None:
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
embedding_dim = {dim}
embedding_batch_enabled = false
embedding_batch_vsearch_enabled = false
vector_metric = ip
vector_score_threshold = -1.0
vector_scope_overfetch_factor = 4
vector_index_type = {backend}
vector_hnsw_m = {m}
vector_hnsw_ef_construction = {ef_construction}
vector_hnsw_ef_search = {ef_search}
vector_hnsw_initial_capacity = {n + 128}
""")


def start_server(kv_bin: Path, root: Path, conf: Path, log_path: Path) -> Tuple[subprocess.Popen, Any, float]:
    log = open(log_path, "wb")
    start = time.perf_counter()
    proc = subprocess.Popen(
        [str(kv_bin), "--config", str(conf)],
        cwd=str(root),
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    return proc, log, start


def stop_server(proc: subprocess.Popen, log: Any) -> None:
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=20)
    log.close()


def records(n: int) -> Iterable[Tuple[str, str, str]]:
    for i in range(n):
        scope = "scopeA" if i % 2 == 0 else "scopeB"
        question = f"bench_q_{i:08d}_{scope}"
        answer = f"bench_a_{i:08d}_{scope}"
        yield question, answer, scope


def query_records(n: int, query_count: int) -> List[Tuple[str, str]]:
    if query_count <= 0:
        return []
    if n <= query_count:
        indexes = list(range(n))
    else:
        step = max(1, n // query_count)
        indexes = [(i * step) % n for i in range(query_count)]
    out = []
    for i in indexes[:query_count]:
        scope = "scopeA" if i % 2 == 0 else "scopeB"
        out.append((f"bench_q_{i:08d}_{scope}", scope))
    return out


def extract_questions(resp: Any) -> List[str]:
    found: List[str] = []
    for item in flatten(resp):
        for match in re.finditer(r"question:([^ \t\r\n,|]+)", item):
            found.append(match.group(1))
    return found


def recall_at_k(actual: List[str], expected: List[str], k: int) -> Optional[float]:
    if not expected:
        return None
    actual_set = set(actual[:k])
    expected_top = expected[:k]
    return len(actual_set.intersection(expected_top)) / float(len(expected_top))


def run_backend(
    backend: str,
    args: argparse.Namespace,
    root: Path,
    kv_bin: Path,
    tmp_root: Path,
    ground_truth: Optional[Dict[str, List[str]]],
) -> Dict[str, Any]:
    port = pick_port()
    run_dir = tmp_root / backend
    run_dir.mkdir(parents=True, exist_ok=True)
    rdb = run_dir / f"{backend}.rdb"
    conf = run_dir / f"{backend}.conf"
    log_path = run_dir / f"{backend}.log"
    write_config(
        conf,
        port,
        rdb,
        backend,
        args.dim,
        args.n,
        args.hnsw_m,
        args.ef_construction,
        args.ef_search,
    )

    result: Dict[str, Any] = {
        "backend": backend,
        "status": "pass",
        "notes": "",
        "n": args.n,
        "dim": args.dim,
        "topk": args.topk,
        "queries": args.queries,
    }

    proc, log, start_ts = start_server(kv_bin, root, conf, log_path)
    try:
        wait_port(port)
        startup_ms = (time.perf_counter() - start_ts) * 1000.0
        client = RespClient(port)
        try:
            initial_info = info_map(client.cmd("VINFO"))
            actual_backend = initial_info.get("index_type")
            result["startup_ms"] = startup_ms
            result["initial_info"] = initial_info
            if actual_backend != backend:
                result["status"] = "unavailable"
                result["notes"] = f"requested {backend}, VINFO index_type={actual_backend!r}"
                return result

            vset_lat: List[float] = []
            load_start = time.perf_counter()
            for question, answer, scope in records(args.n):
                resp, elapsed = timed_cmd(client, "VSET", question, answer, scope)
                assert_ok(resp, f"{backend} VSET {question}")
                vset_lat.append(elapsed)
            result["vset_elapsed_ms"] = (time.perf_counter() - load_start) * 1000.0
            result["vset_latency"] = latency_summary(vset_lat)

            queries = query_records(args.n, args.queries)
            search_lat: List[float] = []
            hits_by_query: Dict[str, List[str]] = {}
            for question, scope in queries:
                resp, elapsed = timed_cmd(client, "VSEARCH", question, scope, args.topk)
                if isinstance(resp, dict):
                    raise RuntimeError(f"{backend} VSEARCH {question} failed: {resp!r}")
                search_lat.append(elapsed)
                hits_by_query[f"{question}|{scope}"] = extract_questions(resp)
            result["vsearch_latency"] = latency_summary(search_lat)
            result["hits_by_query"] = hits_by_query if backend == "core_flat" else None

            if ground_truth is not None:
                recalls = []
                for key, expected in ground_truth.items():
                    actual = hits_by_query.get(key, [])
                    value = recall_at_k(actual, expected, args.topk)
                    if value is not None:
                        recalls.append(value)
                result["recall_at_10"] = statistics.fmean(recalls) if recalls else None
            else:
                result["recall_at_10"] = 1.0 if backend == "core_flat" else None

            info_after_search = info_map(client.cmd("VINFO"))
            result["memory_bytes"] = {
                "index_memory_bytes": int(info_after_search.get("index_memory_bytes", "0") or 0),
                "vector_memory_bytes": int(info_after_search.get("vector_memory_bytes", "0") or 0),
                "total_vector_memory_bytes": int(info_after_search.get("total_vector_memory_bytes", "0") or 0),
            }
            result["backend_stats"] = backend_stats_summary(info_after_search)
            result["vsearch_profile"] = vsearch_profile_summary(info_after_search)
            result["stats_field_check"] = validate_stats_fields(backend, info_after_search)
            result["backend_memory_bytes"] = result["backend_stats"].get("backend_memory_bytes")
            result["backend_memory_source"] = result["backend_stats"].get("backend_memory_bytes_source")
            result["info_after_search"] = info_after_search

            rebuild_resp, rebuild_ms = timed_cmd(client, "VINDEX", "REBUILD")
            assert_ok(rebuild_resp, f"{backend} VINDEX REBUILD")
            info_after_rebuild = info_map(client.cmd("VINFO"))
            result["rebuild"] = {
                "wall_ms": rebuild_ms,
                "last_rebuild_ms": int(info_after_rebuild.get("last_rebuild_ms", "-1") or -1),
                "last_rebuild_status": int(info_after_rebuild.get("last_rebuild_status", "-999") or -999),
                "profile": rebuild_profile_summary(info_after_rebuild),
            }

            smoke: Dict[str, Any] = {}
            update_question = "bench_q_00000000_scopeA"
            assert_ok(client.cmd("VSET", update_question, "bench_a_updated", "scopeA"), f"{backend} update")
            smoke["update_visible"] = contains(client.cmd("VGET", update_question, "scopeA"), "bench_a_updated")
            smoke_search = client.cmd("VSEARCH", update_question, "scopeA", args.topk)
            smoke["update_no_duplicate"] = extract_questions(smoke_search).count(update_question) <= 1
            delete_question = "bench_q_00000002_scopeA" if args.n > 2 else update_question
            delete_resp = client.cmd("VDEL", delete_question, "scopeA")
            assert_ok(delete_resp, f"{backend} delete")
            smoke["delete_hidden"] = delete_question not in extract_questions(
                client.cmd("VSEARCH", delete_question, "scopeA", args.topk)
            )
            scope_resp = client.cmd("VSEARCH", "bench_q_00000000_scopeA", "scopeA", args.topk)
            smoke["scope_isolated"] = not contains(scope_resp, "scope:scopeB")
            smoke["vinfo_index_type"] = info_map(client.cmd("VINFO")).get("index_type") == backend
            result["correctness_smoke"] = smoke
        finally:
            client.close()
    except Exception as exc:
        result["status"] = "fail"
        result["notes"] = str(exc)
    finally:
        stop_server(proc, log)

    if result["status"] != "pass":
        return result

    proc, log, reload_start = start_server(kv_bin, root, conf, run_dir / f"{backend}_reload.log")
    try:
        wait_port(port)
        reload_ms = (time.perf_counter() - reload_start) * 1000.0
        client = RespClient(port)
        try:
            reload_info = info_map(client.cmd("VINFO"))
            result["rdb_load_rebuild"] = {
                "startup_to_ready_ms": reload_ms,
                "index_type": reload_info.get("index_type"),
                "active_records": int(reload_info.get("active_records", "0") or 0),
                "last_rebuild_ms": int(reload_info.get("last_rebuild_ms", "-1") or -1),
            }
        finally:
            client.close()
    except Exception as exc:
        result["status"] = "fail"
        result["notes"] = f"RDB reload failed: {exc}"
    finally:
        stop_server(proc, log)

    return result


def fmt_ms(value: Optional[float]) -> str:
    if value is None:
        return "n/a"
    return f"{value:.3f}"


def fmt_source(value: Any) -> str:
    if value == "estimated":
        return "estimated (not measured)"
    if value == "unsupported":
        return "unsupported"
    if value == "measured":
        return "measured"
    return "n/a"


def write_markdown(path: Path, data: Dict[str, Any]) -> None:
    rows = data["results"]
    stage = data.get("stage", "unknown")
    title = "Stage 5D VINFO / benchmark stats hardening benchmark"
    if stage == "6B":
        title = "Stage 6B core_hnsw rebuild optimization benchmark"
    elif stage == "6C":
        title = "Stage 6C core_hnsw layer search optimization benchmark"
    elif stage == "6D":
        title = "Stage 6D core_hnsw neighbor select optimization benchmark"
    elif stage == "7B":
        title = "Stage 7B core_hnsw AVX2 distance backend benchmark"
    lines = [
        f"# {title} {data['n']} / {data['dim']}d",
        "",
        f"- generated_at: `{data['generated_at']}`",
        f"- stage: `{stage}`",
        f"- stage5d_stats_hardening_benchmark: `{data.get('is_corrected_benchmark', False)}`",
        f"- benchmark_name: `{data.get('benchmark_name', 'unknown')}`",
        f"- overfetch_strategy: `{data.get('overfetch_strategy', 'none')}`",
        f"- backend_search_timing_source: `{data.get('backend_search_timing_source', 'unknown')}`",
        f"- memory_source_policy: `{data.get('memory_source_policy', 'unknown')}`",
        f"- rebuild_profile_keys: `{', '.join(data.get('rebuild_profile_keys', []))}`",
        f"- embedding_provider: `{data['config']['embedding_provider']}`",
        f"- text_embedding_path_dim: `{data.get('text_embedding_path_dim', 'unknown')}`",
        f"- benchmark_requested_dim_effective_for_hash_text_path: `{data.get('benchmark_requested_dim_effective_for_hash_text_path', 'unknown')}`",
        f"- direct_vector_synthetic_dim: `{data.get('direct_vector_synthetic_dim', 'not_used')}`",
        f"- onnx_real_embedding_dim: `{data.get('onnx_real_embedding_dim', 'not_validated_by_this_script')}`",
        f"- vector_metric: `{data['config']['vector_metric']}`",
        f"- topk: `{data['topk']}`",
        f"- queries: `{data['queries']}`",
        "",
        "| backend | status | endpoint VSEARCH avg ms | backend search avg us (estimated) | manager resolve avg us | response assembly avg us | raw hits/query | resolved hits/query | recall@10 | backend size | backend capacity | backend memory bytes | memory source | resize/reserve | stats fields | rebuild wall ms | rebuild add total us | rebuild manager scan us | RDB load+rebuild ms | notes |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|---:|---:|---:|---:|---|",
    ]
    for row in rows:
        search = row.get("vsearch_latency") or {}
        rebuild = row.get("rebuild") or {}
        rebuild_profile = rebuild.get("profile") or {}
        rdb = row.get("rdb_load_rebuild") or {}
        profile = row.get("vsearch_profile") or {}
        backend_stats = row.get("backend_stats") or {}
        field_check = row.get("stats_field_check") or {}
        recall = row.get("recall_at_10")
        profile_count = profile.get("vsearch_manager_resolve_count") or 0
        raw_hits = profile.get("vsearch_raw_hit_count")
        resolved_hits = profile.get("vsearch_resolved_hit_count")
        raw_per_query = "n/a"
        resolved_per_query = "n/a"
        if profile_count and raw_hits is not None:
            raw_per_query = f"{raw_hits / profile_count:.2f}"
        if profile_count and resolved_hits is not None:
            resolved_per_query = f"{resolved_hits / profile_count:.2f}"
        lines.append(
            "| {backend} | {status} | {search_avg} | "
            "{backend_search_us} | {manager_resolve_us} | {response_assembly_us} | "
            "{raw_hits} | {resolved_hits} | {recall} | {backend_size} | "
            "{backend_capacity} | {backend_memory} | {memory_source} | {resize_reserve} | "
            "{stats_fields} | "
            "{rebuild_ms} | {rebuild_add_us} | {rebuild_scan_us} | {rdb_ms} | {notes} |".format(
                backend=row.get("backend", ""),
                status=row.get("status", ""),
                search_avg=fmt_ms(search.get("avg_ms")),
                backend_search_us=profile.get("vsearch_backend_search_avg_us", "n/a"),
                manager_resolve_us=profile.get("vsearch_manager_resolve_avg_us", "n/a"),
                response_assembly_us=profile.get("vsearch_response_assembly_avg_us", "n/a"),
                raw_hits=raw_per_query,
                resolved_hits=resolved_per_query,
                recall="n/a" if recall is None else f"{recall:.4f}",
                backend_size=backend_stats.get("backend_size", "n/a"),
                backend_capacity=backend_stats.get("backend_capacity", "n/a"),
                backend_memory=backend_stats.get("backend_memory_bytes", "n/a"),
                memory_source=fmt_source(backend_stats.get("backend_memory_bytes_source")),
                resize_reserve=(
                    f"{backend_stats.get('backend_resize_count', 'n/a')}"
                    f"({backend_stats.get('backend_resize_count_source', 'n/a')})/"
                    f"{backend_stats.get('backend_reserve_count', 'n/a')}"
                    f"({backend_stats.get('backend_reserve_count_source', 'n/a')})"
                ),
                stats_fields=(
                    "ok"
                    if field_check.get("backend_stats_fields_ok")
                    and field_check.get("vsearch_profile_fields_ok")
                    and field_check.get("memory_source_ok")
                    and field_check.get("resize_reserve_source_ok")
                    else "check"
                ),
                rebuild_ms=fmt_ms(rebuild.get("wall_ms")),
                rebuild_add_us=rebuild_profile.get("rebuild_core_add_total_us", "n/a"),
                rebuild_scan_us=rebuild_profile.get("rebuild_manager_scan_us", "n/a"),
                rdb_ms=fmt_ms(rdb.get("startup_to_ready_ms")),
                notes=str(row.get("notes", "")).replace("|", "/"),
            )
        )
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kv-bin", default="./kvstore")
    parser.add_argument("--n", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=64)
    parser.add_argument("--topk", type=int, default=10)
    parser.add_argument("--queries", type=int, default=100)
    parser.add_argument("--backend", action="append", choices=DEFAULT_BACKENDS)
    parser.add_argument("--out-dir", default="results/vector_core_benchmark")
    parser.add_argument("--hnsw-m", type=int, default=16)
    parser.add_argument("--ef-construction", type=int, default=128)
    parser.add_argument("--ef-search", type=int, default=128)
    parser.add_argument("--corrected", action="store_true",
                        help="Mark as Stage 5D stats hardening benchmark")
    parser.add_argument("--stage6b", action="store_true",
                        help="Write Stage 6B core_hnsw rebuild optimization benchmark outputs")
    parser.add_argument("--stage6c", action="store_true",
                        help="Write Stage 6C core_hnsw layer search optimization benchmark outputs")
    parser.add_argument("--stage6d", action="store_true",
                        help="Write Stage 6D core_hnsw neighbor select optimization benchmark outputs")
    parser.add_argument("--stage6e", action="store_true",
                        help="Write Stage 6E core_hnsw layer search remaining hotspot benchmark outputs")
    parser.add_argument("--stage7a", action="store_true",
                        help="Write Stage 7A core_hnsw memory layout / cache locality audit benchmark outputs")
    parser.add_argument("--stage7b", action="store_true",
                        help="Write Stage 7B core_hnsw AVX2 distance backend benchmark outputs")
    args = parser.parse_args()

    root = Path.cwd()
    kv_bin = Path(args.kv_bin)
    if not kv_bin.is_absolute():
        kv_bin = root / kv_bin
    kv_bin = kv_bin.resolve()
    if not kv_bin.exists():
        raise SystemExit(f"kvstore binary not found: {kv_bin}")

    out_dir = root / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    if args.stage7b:
        prefix = "stage7b_hnsw_avx2_distance_backend_benchmark"
    elif args.stage7a:
        prefix = "stage7a_hnsw_memory_layout_audit_benchmark"
    elif args.stage6e:
        prefix = "stage6e_hnsw_layer_search_hotspot_benchmark"
    elif args.stage6d:
        prefix = "stage6d_hnsw_neighbor_select_opt_benchmark"
    elif args.stage6c:
        prefix = "stage6c_hnsw_layer_search_opt_benchmark"
    elif args.stage6b:
        prefix = "stage6b_hnsw_rebuild_opt_benchmark"
    elif args.corrected:
        prefix = "stage5d_stats_hardening_benchmark"
    else:
        prefix = "stage4_vector_core_benchmark"
    result_base = out_dir / f"{prefix}_{args.n // 1000 if args.n % 1000 == 0 else args.n}k_{args.dim}d"
    if args.n < 1000:
        result_base = out_dir / f"{prefix}_{args.n}_{args.dim}d"

    backends = args.backend or DEFAULT_BACKENDS
    if "core_flat" not in backends:
        backends = ["core_flat"] + backends

    with tempfile.TemporaryDirectory(prefix="kvlite_stage5d_vector_bench_") as td:
        tmp_root = Path(td)
        results: List[Dict[str, Any]] = []
        gt: Optional[Dict[str, List[str]]] = None

        for backend in backends:
            print(f"[bench] backend={backend} n={args.n} dim={args.dim}", flush=True)
            row = run_backend(backend, args, root, kv_bin, tmp_root, gt)
            if backend == "core_flat" and row.get("status") == "pass":
                gt = row.get("hits_by_query") or {}
            if row.get("backend") != "core_flat":
                row.pop("hits_by_query", None)
            results.append(row)

    data = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "n": args.n,
        "dim": args.dim,
        "topk": args.topk,
        "queries": args.queries,
        "is_corrected_benchmark": args.corrected,
        "stage": "7B" if args.stage7b else "7A" if args.stage7a else "6E" if args.stage6e else "6D" if args.stage6d else "6C" if args.stage6c else "6B" if args.stage6b else "5D",
        "benchmark_name": prefix,
        "overfetch_strategy": "topk*4+32",
        "backend_stats_keys": BACKEND_STATS_KEYS,
        "vsearch_profile_keys": VSEARCH_PROFILE_KEYS,
        "rebuild_profile_keys": REBUILD_PROFILE_KEYS,
        "backend_search_timing_source": "estimated_from_ops_search_wall_us_minus_manager_resolve_us",
        "memory_source_policy": "core_flat/core_hnsw measured; hnswlib/faiss estimated-or-unsupported",
        "text_embedding_path_dim": 64,
        "benchmark_requested_dim_effective_for_hash_text_path": False,
        "direct_vector_synthetic_dim": None,
        "onnx_real_embedding_dim": None,
        "onnx_real_embedding_dim_source": "config/kvstore-embedding.conf embedding_dim=512; provider runtime validates embedding_dimension(handle)",
        "config": {
            "embedding_provider": "hash",
            "embedding_dim": args.dim,
            "vector_metric": "ip",
            "vector_hnsw_m": args.hnsw_m,
            "vector_hnsw_ef_construction": args.ef_construction,
            "vector_hnsw_ef_search": args.ef_search,
        },
        "results": results,
    }
    json_path = result_base.with_suffix(".json")
    md_path = result_base.with_suffix(".md")
    json_path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    write_markdown(md_path, data)

    print(f"[bench] wrote {json_path}")
    print(f"[bench] wrote {md_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
