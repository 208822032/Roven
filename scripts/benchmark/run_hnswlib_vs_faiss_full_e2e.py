#!/usr/bin/env python3
"""Full end-to-end text+embedding benchmark for hnswlib vs Faiss.

This runner intentionally uses the real kvstore server and the existing
vector_benchmark client in text mode:

client/network -> RESP -> VSET/VSEARCH text command -> server embedding
provider -> executor/handler -> vector backend -> metadata/response -> network.
"""

import argparse
import csv
import json
import os
import platform
import re
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RESULT_ROOT = ROOT / "results/vector_native_faiss/hnswlib_vs_faiss/full_e2e"
BACKENDS = ["hnswlib_hnsw", "faiss_hnsw"]


def run(cmd, *, timeout=None, capture=True, check=False):
    return subprocess.run(
        cmd,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        timeout=timeout,
        check=check,
    )


def utc_now():
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def faiss_build_info():
    cache = ROOT / "third_party/faiss/build/CMakeCache.txt"
    info = {
        "libfaiss_a": str(ROOT / "third_party/faiss/build/faiss/libfaiss.a"),
        "libfaiss_a_exists": (ROOT / "third_party/faiss/build/faiss/libfaiss.a").exists(),
        "FAISS_OPT_LEVEL": "",
        "FAISS_ENABLE_MKL": "",
        "FAISS_ENABLE_GPU": "",
    }
    if cache.exists():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            for key in ("FAISS_OPT_LEVEL", "FAISS_ENABLE_MKL", "FAISS_ENABLE_GPU"):
                if line.startswith(key + ":"):
                    info[key] = line.split("=", 1)[-1].strip()
    return info


def wait_port(port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket() as s:
            s.settimeout(0.2)
            try:
                s.connect(("127.0.0.1", port))
                return True
            except OSError:
                time.sleep(0.1)
    return False


def read_keyvals(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def proc_rss_kb(pid):
    try:
        for line in Path(f"/proc/{pid}/status").read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("VmRSS:"):
                return line.split()[1]
    except OSError:
        return ""
    return ""


def send_vinfo(port):
    request = b"*1\r\n$5\r\nVINFO\r\n"
    chunks = []
    with socket.socket() as s:
        s.settimeout(5.0)
        s.connect(("127.0.0.1", port))
        s.sendall(request)
        while True:
            data = s.recv(65536)
            if not data:
                break
            chunks.append(data)
            if len(data) < 65536:
                break
    return b"".join(chunks).decode("utf-8", errors="replace")


def config_text(port, sync_port, data_dir, backend, provider, dim, initial_capacity, ef_search, ef_construction):
    return f"""port={port}
sync_port={sync_port}
role=master
enable_aof=no
aof_fsync_mode=no
aof_filename={data_dir}/kvlite.aof
rdb_filename={data_dir}/kvlite.rdb
dump_filename={data_dir}/kvlite.dat
network_mode=reactor
mem_backend=pool
max_value_size=10485760
sync_mode=tcp
log_level=info
log_async=no
embedding_provider={provider}
embedding_dim={dim}
vector_index_type={backend}
vector_metric=ip
vector_distance_backend=auto
vector_hnsw_m=32
vector_hnsw_ef_construction={ef_construction}
vector_hnsw_ef_search={ef_search}
vector_hnsw_initial_capacity={initial_capacity}
vector_score_threshold=0.0
vector_scope_overfetch_factor=1
"""


def run_client(port, op, requests, prefix, prepare, out_base, text_mode, prepare_only=False):
    cmd = [
        "build/tests/bin/vector_benchmark",
        "--mode", "latency",
        "-h", "127.0.0.1",
        "-p", str(port),
        "-t", op,
        "-n", str(requests),
        "-c", "1",
        "-P", "1",
        "--prefix", prefix,
        "--topk", "10",
        "--threshold", "0.0",
        "--scope", "",
        "--text-mode", text_mode,
    ]
    if prepare:
        cmd += ["--prepare", str(prepare)]
    if prepare_only:
        cmd += ["--prepare-only"]
    stdout_path = Path(str(out_base) + ".out")
    stderr_path = Path(str(out_base) + ".err")
    try:
        proc = run(cmd, timeout=300 if prepare <= 1000 else 600)
        stdout_path.write_text(proc.stdout or "", encoding="utf-8")
        stderr_path.write_text(proc.stderr or "", encoding="utf-8")
        return {
            "metrics": read_keyvals(proc.stdout or ""),
            "timeout": False,
            "cmd": " ".join(cmd),
            "stdout": str(stdout_path),
            "stderr": str(stderr_path),
            "returncode": proc.returncode,
        }
    except subprocess.TimeoutExpired:
        stdout_path.write_text("", encoding="utf-8")
        stderr_path.write_text("timeout\n", encoding="utf-8")
        return {
            "metrics": {},
            "timeout": True,
            "cmd": " ".join(cmd),
            "stdout": str(stdout_path),
            "stderr": str(stderr_path),
            "returncode": None,
        }


def last_int(pattern, text):
    matches = re.findall(pattern, text)
    return int(matches[-1]) if matches else None


def last_str(pattern, text):
    matches = re.findall(pattern, text)
    return matches[-1] if matches else None


def parse_profile(server_log):
    try:
        text = server_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}
    profile = {}
    for key, pat in {
        "vset_backend_add_avg_us": r"vset_backend_add_avg_us=(\d+)",
        "vset_vector_copy_avg_us": r"vset_vector_copy_avg_us=(\d+)",
        "vset_vector_copy_count": r"vset_vector_copy_count=(\d+)",
        "vset_id_map_update_avg_us": r"vset_id_map_update_avg_us=(\d+)",
        "vset_id_map_update_count": r"vset_id_map_update_count=(\d+)",
        "hnswlib_capacity": r"(?:hnswlib_capacity|capacity|max_elements)=(\d+)",
        "hnswlib_ntotal": r"hnswlib_ntotal=(\d+)",
        "faiss_ntotal": r"faiss_ntotal=(\d+)",
        "backend_size": r" size=(\d+)",
        "backend_m": r" M=(\d+)",
        "backend_ef_construction": r" efConstruction=(\d+)",
        "backend_ef_search": r" efSearch=(\d+)",
        "vsearch_search_avg_us": r"vsearch_search_avg_us=(\d+)",
        "vsearch_search_count": r"vsearch_search_count=(\d+)",
        "vsearch_non_index_avg_us": r"vsearch_non_index_avg_us=(\d+)",
        "vsearch_response_build_avg_us": r"vsearch_response_build_avg_us=(\d+)",
        "vsearch_response_build_count": r"vsearch_response_build_count=(\d+)",
        "embedding_encode_avg_us": r"embedding_encode_avg_us=(\d+)",
        "runtime_encode_count": r"runtime_encode_count=(\d+)",
        "vset_response_build_avg_us": r"vset_response_build_avg_us=(\d+)",
        "vset_response_build_count": r"vset_response_build_count=(\d+)",
    }.items():
        value = last_int(pat, text)
        if value is not None:
            profile[key] = value
    for key, pat in {
        "backend_name": r"backend_name=([^ ]+)",
        "backend_metric": r" metric=([^ ]+)",
        "backend_normalize": r" normalize=([^ ]+)",
    }.items():
        value = last_str(pat, text)
        if value is not None:
            profile[key] = value
    profile["server_log_confirms_hash_provider"] = "hash embedding provider ready" in text
    return profile


def metric_us(metrics, keys):
    out = []
    for key in keys:
        if metrics.get(key):
            out.append(f"{float(metrics[key]) * 1000.0:.3f}")
        else:
            out.append("")
    return out


def row_from_result(run_id, backend, dataset_size, query_size, dim, provider, ef_search, ef_construction, op, result, expected, rss_kb, conf, server_log):
    metrics = result["metrics"]
    if op == "VSET" and "prepare_count" in metrics:
        success = int(metrics.get("prepare_success", 0) or 0)
        failed = int(metrics.get("prepare_failed", 0) or 0)
        total_ms = metrics.get("prepare_elapsed_ms", "")
        qps = metrics.get("prepare_qps", "")
        lat = metric_us(metrics, ("prepare_avg_ms", "prepare_p50_ms", "prepare_p95_ms", "prepare_p99_ms", "prepare_max_ms"))
    else:
        success = int(metrics.get("success", 0) or 0)
        failed = int(metrics.get("failed", 0) or 0)
        failed += int(metrics.get("errors", 0) or 0)
        total_ms = metrics.get("elapsed_ms", "")
        qps = metrics.get("qps", "")
        lat = metric_us(metrics, ("avg_ms", "p50_ms", "p95_ms", "p99_ms", "max_ms"))
    timeout_count = 1 if result["timeout"] else 0
    status = "timeout" if result["timeout"] else ("pass" if success == expected and failed == 0 and result["returncode"] == 0 else "fail")
    return {
        "backend": backend,
        "dataset_size": dataset_size,
        "query_size": query_size,
        "dim": dim,
        "ef_construction": ef_construction,
        "ef_search": ef_search,
        "embedding_provider": provider,
        "operation": op,
        "total_ms": total_ms,
        "qps": qps,
        "avg_us": lat[0],
        "p50_us": lat[1],
        "p95_us": lat[2],
        "p99_us": lat[3],
        "max_us": lat[4],
        "success_count": success,
        "error_count": failed,
        "timeout_count": timeout_count,
        "embedding_avg_us": "",
        "index_avg_us": "",
        "non_index_avg_us": "",
        "response_avg_us": "",
        "label_id_mapping_us": "",
        "vector_copy_or_prepare_us": "",
        "total_backend_add_us": "",
        "backend_stats": "",
        "rss_kb": rss_kb,
        "notes": f"cmd={result['cmd']}; conf={conf}; server_log={server_log}; text_mode=semantic; real TCP RESP text VSET/VSEARCH",
        "status": status,
        "run_id": run_id,
    }


def apply_profile(row, profile):
    avg = float(row["avg_us"]) if row.get("avg_us") else None
    if profile.get("runtime_encode_count", 0) > 0 and profile.get("embedding_encode_avg_us", 0) > 0:
        row["embedding_avg_us"] = f"{profile['embedding_encode_avg_us']:.3f}"
    else:
        row["notes"] += "; embedding profile unavailable for current provider"
    index_avg = None
    if row["operation"] == "VSET" and profile.get("vset_backend_add_avg_us", 0) > 0:
        index_avg = float(profile["vset_backend_add_avg_us"])
    elif row["operation"] == "VSEARCH" and profile.get("vsearch_search_count", 0) > 0 and profile.get("vsearch_search_avg_us", 0) > 0:
        index_avg = float(profile["vsearch_search_avg_us"])
    if index_avg is not None:
        row["index_avg_us"] = f"{index_avg:.3f}"
        if avg is not None:
            row["non_index_avg_us"] = f"{max(0.0, avg - index_avg):.3f}"
    else:
        row["notes"] += "; index profile unavailable" if row["operation"] == "VSEARCH" else ""
    if row["operation"] == "VSEARCH" and profile.get("vsearch_response_build_count", 0) > 0:
        row["response_avg_us"] = f"{profile.get('vsearch_response_build_avg_us', 0):.3f}"
    elif profile.get("vset_response_build_count", 0) > 0 and profile.get("vset_response_build_avg_us", 0) > 0:
        row["response_avg_us"] = f"{profile['vset_response_build_avg_us']:.3f}"
    else:
        row["notes"] += "; response profile unavailable"
    if row["operation"] == "VSET":
        if row["index_avg_us"]:
            row["total_backend_add_us"] = row["index_avg_us"]
        if profile.get("vset_id_map_update_count", 0) > 0:
            row["label_id_mapping_us"] = f"{profile['vset_id_map_update_avg_us']:.3f}"
        if profile.get("vset_vector_copy_count", 0) > 0:
            row["vector_copy_or_prepare_us"] = f"{profile['vset_vector_copy_avg_us']:.3f}"
    stats = []
    for key in (
        "hnswlib_ntotal", "hnswlib_capacity", "faiss_ntotal",
        "backend_name", "backend_size", "backend_m", "backend_ef_construction",
        "backend_ef_search", "backend_metric", "backend_normalize",
    ):
        if key in profile:
            stats.append(f"{key}={profile[key]}")
    if stats:
        row["backend_stats"] = ";".join(stats)


def write_csv(path, rows):
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def write_summary(path, rows):
    lines = [
        "# hnswlib vs Faiss Full E2E Text+Embedding Benchmark",
        "",
        "| Dataset | efConstruction | efSearch | Backend | Operation | Status | QPS | Avg us | P95 us | Success | Errors | Embedding avg us | Index avg us | Backend add us | ID map us | Vector copy us | Response avg us | Backend stats |",
        "|---:|---:|---:|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for r in rows:
        lines.append(
            f"| {r['dataset_size']} | {r['ef_construction']} | {r['ef_search']} | {r['backend']} | {r['operation']} | {r['status']} | "
            f"{r['qps']} | {r['avg_us']} | {r['p95_us']} | {r['success_count']} | "
            f"{r['error_count']} | {r['embedding_avg_us']} | {r['index_avg_us']} | "
            f"{r['total_backend_add_us']} | {r['label_id_mapping_us']} | "
            f"{r['vector_copy_or_prepare_us']} | {r['response_avg_us']} | {r['backend_stats']} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_all(out_dir):
    log = out_dir / "build.log"
    proc = run(["make", "clean"], timeout=120)
    proc2 = run(["make", "ENABLE_FAISS=1", "ENABLE_HNSWLIB=1", "-j", str(os.cpu_count() or 1)], timeout=600)
    log.write_text((proc.stdout or "") + (proc.stderr or "") + (proc2.stdout or "") + (proc2.stderr or ""), encoding="utf-8")
    if proc.returncode != 0 or proc2.returncode != 0:
        raise SystemExit(f"build failed: {log}")
    return {"command": "make clean && make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j$(nproc)", "log": str(log), "exit_code": proc2.returncode}


def run_full_e2e(run_id, sizes, query_size, provider, dim, skip_build, ef_search_list, ef_construction_list, backends):
    out_dir = RESULT_ROOT / run_id
    out_dir.mkdir(parents=True, exist_ok=True)
    build = {"command": "skipped", "log": "", "exit_code": 0}
    if not skip_build:
        build = build_all(out_dir)
    rows = []
    port_base = 24000
    config_concat = []
    log_concat = []
    for si, size in enumerate(sizes):
        for ci, ef_construction in enumerate(ef_construction_list):
            for ei, ef_search in enumerate(ef_search_list):
                for bi, backend in enumerate(backends):
                    port = port_base + si * 1000 + ci * 200 + ei * 20 + bi
                    case_dir = out_dir / f"{size}_efc{ef_construction}_efs{ef_search}_{backend}"
                    case_dir.mkdir(parents=True, exist_ok=True)
                    data_dir = case_dir / "data"
                    data_dir.mkdir(exist_ok=True)
                    conf = case_dir / "backend_config.conf"
                    conf.write_text(
                        config_text(port, port + 10000, data_dir, backend, provider, dim, size, ef_search, ef_construction),
                        encoding="utf-8",
                    )
                    config_concat.append(f"# {size} efConstruction={ef_construction} efSearch={ef_search} {backend}\n{conf.read_text(encoding='utf-8')}\n")
                    server_log = case_dir / "server.log"
                    case_rows = []
                    with server_log.open("w", encoding="utf-8") as logf:
                        srv = subprocess.Popen(["./kvstore", "-c", str(conf)], cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT, text=True)
                    try:
                        if not wait_port(port):
                            raise RuntimeError(f"server did not listen on {port}")
                        prefix = f"{run_id}_{size}_efc{ef_construction}_efs{ef_search}_{backend}"
                        vset = run_client(port, "vset", 1, prefix, size, case_dir / "vset", "semantic", prepare_only=True)
                        case_rows.append(row_from_result(run_id, backend, size, query_size, dim, provider, ef_search, ef_construction, "VSET", vset, size, proc_rss_kb(srv.pid), conf, server_log))
                        try:
                            (case_dir / "vinfo_after_vset.resp").write_text(send_vinfo(port), encoding="utf-8")
                        except OSError as exc:
                            (case_dir / "vinfo_after_vset.resp").write_text(f"error: {exc}\n", encoding="utf-8")
                        vsearch = run_client(port, "vsearch", query_size, prefix, 0, case_dir / "vsearch", "semantic")
                        case_rows.append(row_from_result(run_id, backend, size, query_size, dim, provider, ef_search, ef_construction, "VSEARCH", vsearch, query_size, proc_rss_kb(srv.pid), conf, server_log))
                        try:
                            (case_dir / "vinfo_after_vsearch.resp").write_text(send_vinfo(port), encoding="utf-8")
                        except OSError as exc:
                            (case_dir / "vinfo_after_vsearch.resp").write_text(f"error: {exc}\n", encoding="utf-8")
                    finally:
                        try:
                            srv.terminate()
                            srv.wait(timeout=5)
                        except Exception:
                            srv.kill()
                            srv.wait(timeout=5)
                    profile = parse_profile(server_log)
                    for row in case_rows:
                        apply_profile(row, profile)
                    rows.extend(case_rows)
                    log_concat.append(f"# {size} efConstruction={ef_construction} efSearch={ef_search} {backend}\n{server_log.read_text(encoding='utf-8', errors='replace')}\n")
    (out_dir / "backend_config.conf").write_text("\n".join(config_concat), encoding="utf-8")
    (out_dir / "server.log").write_text("\n".join(log_concat), encoding="utf-8")
    write_csv(out_dir / "raw.csv", rows)
    write_summary(out_dir / "summary.md", rows)
    manifest = {
        "run_id": run_id,
        "layer": "full_e2e_text_embedding",
        "created_at_utc": utc_now(),
        "host": platform.node(),
        "machine": platform.machine(),
        "python": sys.version,
        "sizes": sizes,
        "query_size": query_size,
        "params": {
            "dim": dim,
            "embedding_provider": provider,
            "text_mode": "semantic",
            "topK": 10,
            "scope": "empty",
            "M": 32,
            "efConstruction": ef_construction_list,
            "efSearch": ef_search_list,
            "vector_hnsw_initial_capacity": "dataset_size",
            "backends": backends,
        },
        "build": build,
        "faiss_build": faiss_build_info(),
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return out_dir, rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-id", default=f"full_e2e_{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}")
    parser.add_argument("--sizes", default="1000,10000")
    parser.add_argument("--query-size", type=int, default=1000)
    parser.add_argument("--embedding-provider", default="hash")
    parser.add_argument("--dim", type=int, default=64)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--ef-search-list", default="128")
    parser.add_argument("--ef-construction-list", default="256")
    parser.add_argument("--backend", action="append", choices=BACKENDS)
    parser.add_argument("--backends", default="")
    args = parser.parse_args()
    sizes = [int(x) for x in args.sizes.split(",") if x.strip()]
    ef_search_list = [int(x) for x in args.ef_search_list.split(",") if x.strip()]
    ef_construction_list = [int(x) for x in args.ef_construction_list.split(",") if x.strip()]
    selected_backends = list(BACKENDS)
    if args.backends:
        selected_backends = [x.strip() for x in args.backends.split(",") if x.strip()]
    if args.backend:
        selected_backends = args.backend
    invalid = [b for b in selected_backends if b not in BACKENDS]
    if invalid:
        raise SystemExit(f"unsupported backend(s): {','.join(invalid)}")
    out_dir, rows = run_full_e2e(
        args.run_id,
        sizes,
        args.query_size,
        args.embedding_provider,
        args.dim,
        args.skip_build,
        ef_search_list,
        ef_construction_list,
        selected_backends,
    )
    print(f"run_id={args.run_id}")
    print(f"full_e2e={out_dir}")
    print(f"rows={len(rows)}")


if __name__ == "__main__":
    main()
