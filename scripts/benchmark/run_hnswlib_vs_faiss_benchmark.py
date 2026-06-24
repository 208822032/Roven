#!/usr/bin/env python3
"""Formal hnswlib-vs-Faiss benchmark runner.

Index-only uses binary float datasets. Server-path uses real kvstore
VSET/VSEARCH over RESP with deterministic hash embedding because the RESP API
does not accept raw float vectors.
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
DATASET_ROOT = ROOT / "results/vector_native_faiss/datasets"
RESULT_ROOT = ROOT / "results/vector_native_faiss/hnswlib_vs_faiss"
INDEX_ROOT = RESULT_ROOT / "index_only"
SERVER_ROOT = RESULT_ROOT / "server_path"
INDEX_BACKENDS = ["hnswlib_hnsw", "faiss_hnsw", "faiss_flat"]
SERVER_BACKENDS = ["hnswlib_hnsw", "faiss_hnsw", "faiss_flat"]


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


def dataset_id(n):
    return f"smoke_N{n}_nq1000_d64_seed42_ip_norm_top10_scope-empty"


def ensure_dataset(n):
    manifest = DATASET_ROOT / dataset_id(n) / "manifest.json"
    if manifest.exists():
        return
    run([
        sys.executable,
        "scripts/benchmark/gen_vector_benchmark_dataset.py",
        "--count", str(n),
        "--query-count", "1000",
        "--dim", "64",
        "--seed", "42",
        "--metric", "ip",
        "--normalize",
        "--topk", "10",
    ], check=True)


def load_manifest(n):
    path = DATASET_ROOT / dataset_id(n) / "manifest.json"
    with path.open("r", encoding="utf-8") as f:
        return path, json.load(f)


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


def backend_availability():
    faiss_lib = ROOT / "third_party/faiss/build/faiss/libfaiss.a"
    return {
        "hnswlib_hnsw": {
            "available": (ROOT / "third_party/hnswlib/hnswlib/hnswlib.h").exists(),
            "notes": "vendored hnswlib header present",
        },
        "faiss_hnsw": {
            "available": faiss_lib.exists(),
            "notes": "requires libfaiss.a, BLAS/LAPACK, and ENABLE_FAISS=1 build",
        },
        "faiss_flat": {
            "available": faiss_lib.exists(),
            "notes": "Faiss exact flat baseline / recall ground truth",
        },
    }


def build_all(out_dir):
    log = out_dir / "build.log"
    proc = run(["make", "clean"], timeout=120)
    proc2 = run(["make", "ENABLE_FAISS=1", "ENABLE_HNSWLIB=1", "-j", str(os.cpu_count() or 1)], timeout=600)
    log.write_text((proc.stdout or "") + (proc.stderr or "") + (proc2.stdout or "") + (proc2.stderr or ""), encoding="utf-8")
    return {"command": "make clean && make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j$(nproc)", "exit_code": proc2.returncode, "log": str(log)}


def compile_index_runner():
    run([
        "g++", "-O3", "-std=c++17",
        "-I", "third_party/hnswlib",
        "-I", "third_party/faiss",
        "-o", "build/tests/bin/vector_index_only_formal",
        "tests/vector_index_only_formal.cpp",
        "third_party/faiss/build/faiss/libfaiss.a",
        "-fopenmp", "-lblas", "-llapack", "-lpthread", "-lm",
    ], timeout=180, check=True)


def write_csv(path, rows):
    if not rows:
        return
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def unavailable_index_row(run_id, manifest, backend, notes, status="unavailable"):
    return {
        "backend": backend, "dataset_size": manifest["N"], "dim": manifest["dim"],
        "operation": "add_search", "total_ms": "", "qps": "", "avg_us": "",
        "p50_us": "", "p95_us": "", "p99_us": "", "max_us": "",
        "recall_at_10": "", "rss_kb": "", "error_count": 0, "notes": notes,
        "status": status, "run_id": run_id,
    }


def run_index_only(run_id, availability):
    out_dir = INDEX_ROOT / run_id
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    for n in (10000, 100000):
        manifest_path, manifest = load_manifest(n)
        for backend in INDEX_BACKENDS:
            if not availability[backend]["available"]:
                rows.append(unavailable_index_row(run_id, manifest, backend, availability[backend]["notes"]))
                continue
            cmd = [
                "build/tests/bin/vector_index_only_formal",
                "--backend", backend,
                "--dataset", manifest["dataset_path"],
                "--queries", manifest["queries_path"],
                "--n", str(manifest["N"]),
                "--nq", str(manifest["nq"]),
                "--dim", str(manifest["dim"]),
                "--topk", str(manifest["topK"]),
                "--m", "32",
                "--ef-construction", "256",
                "--ef-search", "128",
            ]
            try:
                proc = run(cmd, timeout=360 if n == 100000 else 180)
            except subprocess.TimeoutExpired:
                rows.append(unavailable_index_row(run_id, manifest, backend, "timeout", "timeout"))
                continue
            if proc.returncode != 0:
                rows.append(unavailable_index_row(run_id, manifest, backend, (proc.stderr or proc.stdout or "")[:500], "fail"))
                continue
            for item in csv.DictReader(proc.stdout.splitlines()):
                rows.append({
                    "backend": backend,
                    "dataset_size": manifest["N"],
                    "dim": manifest["dim"],
                    "operation": item["operation"],
                    "total_ms": item["total_ms"],
                    "qps": item["qps"],
                    "avg_us": item["avg_us"],
                    "p50_us": item["p50_us"],
                    "p95_us": item["p95_us"],
                    "p99_us": item["p99_us"],
                    "max_us": item["max_us"],
                    "recall_at_10": item["recall_at_10"],
                    "rss_kb": item["rss_kb"],
                    "error_count": item["error_count"],
                    "notes": f"manifest={manifest_path}; M=32 efConstruction=256 efSearch=128; faiss_flat ground truth",
                    "status": "pass",
                    "run_id": run_id,
                })
    write_csv(out_dir / "raw.csv", rows)
    write_index_summary(out_dir / "summary.md", rows)
    write_manifest(out_dir / "manifest.json", run_id, "index_only", rows)
    return out_dir, rows


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


def config_text(port, sync_port, data_dir, backend):
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
embedding_provider=hash
embedding_dim=64
vector_index_type={backend}
vector_metric=ip
vector_distance_backend=auto
vector_hnsw_m=32
vector_hnsw_ef_construction=256
vector_hnsw_ef_search=128
vector_score_threshold=0.0
vector_scope_overfetch_factor=1
"""


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


def run_server_client(port, op, requests, prefix, prepare, out_base, prepare_only=False):
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
    ]
    if prepare:
        cmd += ["--prepare", str(prepare)]
    if prepare_only:
        cmd += ["--prepare-only"]
    stdout_path = Path(str(out_base) + ".out")
    stderr_path = Path(str(out_base) + ".err")
    try:
        proc = run(cmd, timeout=420 if prepare >= 100000 else 240)
        stdout_path.write_text(proc.stdout or "", encoding="utf-8")
        stderr_path.write_text(proc.stderr or "", encoding="utf-8")
        return {"metrics": read_keyvals(proc.stdout or ""), "timeout": False, "cmd": " ".join(cmd), "stdout": str(stdout_path), "stderr": str(stderr_path), "returncode": proc.returncode}
    except subprocess.TimeoutExpired:
        stdout_path.write_text("", encoding="utf-8")
        stderr_path.write_text("timeout\n", encoding="utf-8")
        return {"metrics": {}, "timeout": True, "cmd": " ".join(cmd), "stdout": str(stdout_path), "stderr": str(stderr_path), "returncode": None}


def parse_server_profile(server_log):
    try:
        text = server_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}
    patterns = {
        "vset_backend_add_avg_us": r"vset_backend_add_avg_us=(\d+)",
        "vsearch_search_avg_us": r"vsearch_search_avg_us=(\d+)",
        "vsearch_search_count": r"vsearch_search_count=(\d+)",
        "vsearch_non_index_avg_us": r"vsearch_non_index_avg_us=(\d+)",
        "vsearch_response_build_avg_us": r"vsearch_response_build_avg_us=(\d+)",
        "vsearch_response_build_count": r"vsearch_response_build_count=(\d+)",
    }
    profile = {}
    for key, pat in patterns.items():
        matches = re.findall(pat, text)
        if matches:
            profile[key] = matches[-1]
    return profile


def server_blank_row(run_id, manifest, backend, op, notes, status):
    return {
        "backend": backend, "dataset_size": manifest["N"], "dim": manifest["dim"],
        "operation": op, "total_ms": "", "qps": "", "avg_us": "", "p50_us": "",
        "p95_us": "", "p99_us": "", "max_us": "", "success_count": 0,
        "error_count": 0, "timeout_count": 0, "rss_kb": "", "index_avg_us": "",
        "non_index_avg_us": "", "notes": notes, "status": status, "run_id": run_id,
    }


def server_rows_unavailable(run_id, manifest, backend, notes, status="unavailable"):
    return [server_blank_row(run_id, manifest, backend, "VSET", notes, status),
            server_blank_row(run_id, manifest, backend, "VSEARCH", notes, status)]


def to_float(value):
    if value is None or value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def server_row(run_id, manifest, backend, op, result, requests, prepared_count, server_log, conf, rss_kb):
    m = result["metrics"]
    timeout = result["timeout"]
    if op == "VSET" and "prepare_count" in m:
        success = int(m.get("prepare_success", 0) or 0)
        failed = int(m.get("prepare_failed", 0) or 0)
        errors = 0
        total_ms = m.get("prepare_elapsed_ms", "")
        qps = m.get("prepare_qps", "")
        keys = ("prepare_avg_ms", "prepare_p50_ms", "prepare_p95_ms", "prepare_p99_ms", "prepare_max_ms")
    else:
        success = int(m.get("success", 0) or 0)
        failed = int(m.get("failed", 0) or 0)
        errors = int(m.get("errors", 0) or 0)
        total_ms = m.get("elapsed_ms", "")
        qps = m.get("qps", "")
        keys = ("avg_ms", "p50_ms", "p95_ms", "p99_ms", "max_ms")
    error_count = failed + errors
    status = "timeout" if timeout else ("pass" if success == requests and error_count == 0 and result["returncode"] == 0 else "fail")
    vals = []
    for k in keys:
        vals.append(f"{float(m.get(k, 0) or 0) * 1000.0:.3f}" if m.get(k) else "")
    return {
        "backend": backend,
        "dataset_size": manifest["N"],
        "dim": manifest["dim"],
        "operation": op,
        "total_ms": total_ms,
        "qps": qps,
        "avg_us": vals[0],
        "p50_us": vals[1],
        "p95_us": vals[2],
        "p99_us": vals[3],
        "max_us": vals[4],
        "success_count": success,
        "error_count": error_count,
        "timeout_count": 1 if timeout else 0,
        "rss_kb": rss_kb,
        "index_avg_us": "",
        "non_index_avg_us": "",
        "notes": f"cmd={result['cmd']}; stdout={result['stdout']}; stderr={result['stderr']}; conf={conf}; server_log={server_log}; prepare={prepared_count if op == 'VSET' else 0}; text/hash embedding server path",
        "status": status,
        "run_id": run_id,
    }


def apply_profile(row, profile):
    avg = to_float(row.get("avg_us"))
    index_avg = None
    if row["operation"] == "VSET":
        index_avg = to_float(profile.get("vset_backend_add_avg_us"))
    elif row["operation"] == "VSEARCH":
        value = to_float(profile.get("vsearch_search_avg_us"))
        count = to_float(profile.get("vsearch_search_count"))
        if count and count > 0 and value and value > 0:
            index_avg = value
    if index_avg is not None:
        row["index_avg_us"] = f"{index_avg:.3f}"
        if avg is not None:
            row["non_index_avg_us"] = f"{max(0.0, avg - index_avg):.3f}"
    else:
        row["notes"] += "; profile index_avg_us unavailable"


def run_server_path(run_id, availability):
    out_dir = SERVER_ROOT / run_id
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    port_base = 23000
    for ni, n in enumerate((10000, 100000)):
        manifest_path, manifest = load_manifest(n)
        for bi, backend in enumerate(SERVER_BACKENDS):
            if not availability[backend]["available"]:
                rows.extend(server_rows_unavailable(run_id, manifest, backend, availability[backend]["notes"]))
                continue
            port = port_base + ni * 100 + bi
            case_dir = out_dir / f"{n}_{backend}"
            case_dir.mkdir(parents=True, exist_ok=True)
            conf = case_dir / "backend_config.conf"
            data_dir = case_dir / "data"
            data_dir.mkdir(exist_ok=True)
            conf.write_text(config_text(port, port + 10000, data_dir, backend), encoding="utf-8")
            server_log = case_dir / "server.log"
            case_rows = []
            with server_log.open("w", encoding="utf-8") as logf:
                srv = subprocess.Popen(["./kvstore", "-c", str(conf)], cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT, text=True)
            try:
                if not wait_port(port):
                    case_rows.extend(server_rows_unavailable(run_id, manifest, backend, "server did not listen", "fail"))
                else:
                    prefix = f"{run_id}_{n}_{backend}"
                    vset = run_server_client(port, "vset", 1, prefix, n, case_dir / "vset", prepare_only=True)
                    case_rows.append(server_row(run_id, manifest, backend, "VSET", vset, n, n, server_log, conf, proc_rss_kb(srv.pid)))
                    vsearch = run_server_client(port, "vsearch", manifest["nq"], prefix, 0, case_dir / "vsearch")
                    case_rows.append(server_row(run_id, manifest, backend, "VSEARCH", vsearch, manifest["nq"], n, server_log, conf, proc_rss_kb(srv.pid)))
            finally:
                try:
                    srv.terminate()
                    srv.wait(timeout=5)
                except Exception:
                    srv.kill()
                    srv.wait(timeout=5)
            profile = parse_server_profile(server_log)
            for row in case_rows:
                apply_profile(row, profile)
            rows.extend(case_rows)
    write_csv(out_dir / "raw.csv", rows)
    write_server_summary(out_dir / "summary.md", rows)
    write_manifest(out_dir / "manifest.json", run_id, "server_path", rows)
    return out_dir, rows


def write_index_summary(path, rows):
    lines = ["# hnswlib vs Faiss Index-only Benchmark", "", "| Dataset | Backend | Operation | Status | QPS | Avg us | P95 us | Recall@10 | Notes |", "|---:|---|---|---|---:|---:|---:|---:|---|"]
    for r in rows:
        lines.append(f"| {r['dataset_size']} | {r['backend']} | {r['operation']} | {r['status']} | {r['qps']} | {r['avg_us']} | {r['p95_us']} | {r['recall_at_10']} | {r['notes']} |")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_server_summary(path, rows):
    lines = ["# hnswlib vs Faiss Server-path Benchmark", "", "| Dataset | Backend | Operation | Status | QPS | Avg us | P95 us | Success | Errors | Timeouts | Index avg us | Notes |", "|---:|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|"]
    for r in rows:
        lines.append(f"| {r['dataset_size']} | {r['backend']} | {r['operation']} | {r['status']} | {r['qps']} | {r['avg_us']} | {r['p95_us']} | {r['success_count']} | {r['error_count']} | {r['timeout_count']} | {r['index_avg_us']} | {r['notes']} |")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_manifest(path, run_id, layer, rows):
    data = {
        "run_id": run_id,
        "layer": layer,
        "created_at_utc": utc_now(),
        "host": platform.node(),
        "machine": platform.machine(),
        "python": sys.version,
        "rows": len(rows),
        "params": {
            "seed": 42, "dim": 64, "metric": "ip", "normalize": True,
            "topK": 10, "scope": "empty", "M": 32,
            "efConstruction": 256, "efSearch": 128,
        },
        "faiss_build": faiss_build_info(),
    }
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-id", default=f"hnswlib_faiss_{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()

    for n in (10000, 100000):
        ensure_dataset(n)

    index_dir = INDEX_ROOT / args.run_id
    index_dir.mkdir(parents=True, exist_ok=True)
    build = {"command": "skipped", "exit_code": 0, "log": ""}
    if not args.skip_build:
        build = build_all(index_dir)
        if build["exit_code"] != 0:
            raise SystemExit(f"build failed: {build['log']}")
    compile_index_runner()
    availability = backend_availability()

    index_dir, index_rows = run_index_only(args.run_id, availability)
    server_dir, server_rows = run_server_path(args.run_id, availability)

    availability_path = RESULT_ROOT / f"backend_availability_{args.run_id}.json"
    availability_path.parent.mkdir(parents=True, exist_ok=True)
    availability_path.write_text(json.dumps({
        "availability": availability,
        "build": build,
        "faiss_build": faiss_build_info(),
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(f"run_id={args.run_id}")
    print(f"availability={availability_path}")
    print(f"index_only={index_dir}")
    print(f"server_path={server_dir}")
    print(f"index_rows={len(index_rows)} server_rows={len(server_rows)}")


if __name__ == "__main__":
    main()
