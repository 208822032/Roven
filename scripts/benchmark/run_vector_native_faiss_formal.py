#!/usr/bin/env python3
"""Formal native/hnswlib vs Faiss benchmark orchestrator.

The script records unavailable backends instead of fabricating results.
Index-only runs consume dataset.bin/queries.bin directly. Server-path runs use
real kvstore VSET/VSEARCH with deterministic text generated from the same
manifest parameters because the RESP API does not accept raw float vectors.
"""

import argparse
import csv
import json
import os
import platform
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DATASET_ROOT = ROOT / "results/vector_native_faiss/datasets"
INDEX_ROOT = ROOT / "results/vector_native_faiss/index_only"
SERVER_ROOT = ROOT / "results/vector_native_faiss/server_path"
BACKENDS = ["native_flat", "hnswlib_hnsw", "faiss_flat", "faiss_hnsw", "native_hnsw"]


def run(cmd, *, timeout=None, capture=True, check=False, env=None):
    return subprocess.run(
        cmd,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        timeout=timeout,
        check=check,
        env=env,
    )


def utc_now():
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def dataset_id(n):
    return f"smoke_N{n}_nq1000_d64_seed42_ip_norm_top10_scope-empty"


def load_manifest(n):
    path = DATASET_ROOT / dataset_id(n) / "manifest.json"
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    return path, data


def ensure_dataset(n):
    path = DATASET_ROOT / dataset_id(n) / "manifest.json"
    if path.exists():
        return
    cmd = [
        sys.executable,
        "scripts/benchmark/gen_vector_benchmark_dataset.py",
        "--count", str(n),
        "--query-count", "1000",
        "--dim", "64",
        "--seed", "42",
        "--metric", "ip",
        "--normalize",
        "--topk", "10",
    ]
    run(cmd, check=True)


def backend_availability():
    faiss_lib = ROOT / "third_party/faiss/build/faiss/libfaiss.a"
    out = {
        "native_flat": {"available": True, "notes": "built-in exact flat backend"},
        "hnswlib_hnsw": {
            "available": (ROOT / "third_party/hnswlib/hnswlib/hnswlib.h").exists(),
            "notes": "third_party/hnswlib/hnswlib/hnswlib.h present",
        },
        "faiss_flat": {
            "available": faiss_lib.exists(),
            "notes": "requires third_party/faiss/build/faiss/libfaiss.a and BLAS/LAPACK",
        },
        "faiss_hnsw": {
            "available": faiss_lib.exists(),
            "notes": "requires third_party/faiss/build/faiss/libfaiss.a and BLAS/LAPACK",
        },
        "native_hnsw": {
            "available": False,
            "notes": "current code maps native_hnsw to unavailable/removed; use hnswlib_hnsw",
        },
    }
    if not (ROOT / "third_party/faiss/faiss/IndexFlat.h").exists():
        out["faiss_flat"]["notes"] = "faiss headers missing"
        out["faiss_hnsw"]["notes"] = "faiss headers missing"
    elif not faiss_lib.exists():
        out["faiss_flat"]["notes"] = "faiss headers present but libfaiss.a missing"
        out["faiss_hnsw"]["notes"] = "faiss headers present but libfaiss.a missing"
    return out


def compile_default():
    run(["make", "clean"], timeout=120, check=True)
    run(["make", "ENABLE_HNSWLIB=1", "ENABLE_FAISS=0", "-j", str(os.cpu_count() or 1)],
        timeout=300, check=True)


def compile_index_runner():
    cmd = [
        "g++", "-O3", "-std=c++17",
        "-I", "third_party/hnswlib",
        "-o", "build/tests/bin/vector_index_only_formal",
        "tests/vector_index_only_formal.cpp",
        "-lpthread",
    ]
    run(cmd, timeout=120, check=True)


def strict_build_probe(out_dir):
    log = out_dir / "strict_build.log"
    run(["make", "clean"], timeout=120, capture=True)
    proc = run(
        ["make", "ENABLE_FAISS=1", "ENABLE_HNSWLIB=1", "-j", str(os.cpu_count() or 1)],
        timeout=300,
        capture=True,
    )
    log.write_text((proc.stdout or "") + (proc.stderr or ""), encoding="utf-8")
    return {
        "command": "make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j$(nproc)",
        "exit_code": proc.returncode,
        "log": str(log),
    }


def read_keyvals(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def run_index_only(run_id, availability):
    out_dir = INDEX_ROOT / run_id
    out_dir.mkdir(parents=True, exist_ok=True)
    raw_path = out_dir / "raw.csv"
    rows = []
    for n in (10000, 100000):
        manifest_path, manifest = load_manifest(n)
        for backend in ["native_flat", "hnswlib_hnsw", "faiss_flat", "faiss_hnsw", "native_hnsw"]:
            if backend in ("faiss_flat", "faiss_hnsw", "native_hnsw"):
                rows.append(index_unavailable_row(run_id, manifest, backend, availability[backend]["notes"]))
                continue
            if not availability[backend]["available"]:
                rows.append(index_unavailable_row(run_id, manifest, backend, availability[backend]["notes"]))
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
                proc = run(cmd, timeout=240 if n == 100000 else 120, capture=True)
            except subprocess.TimeoutExpired:
                rows.append(index_unavailable_row(run_id, manifest, backend, "timeout", status="timeout"))
                continue
            if proc.returncode != 0:
                note = (proc.stderr or proc.stdout or "").replace("\n", " ")[:500]
                rows.append(index_unavailable_row(run_id, manifest, backend, note, status="fail"))
                continue
            reader = csv.DictReader(proc.stdout.splitlines())
            for item in reader:
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
                    "notes": "pass",
                    "status": "pass",
                    "run_id": run_id,
                    "manifest": str(manifest_path),
                })
    write_csv(raw_path, rows)
    write_index_summary(out_dir / "summary.md", rows)
    write_manifest(out_dir / "manifest.json", run_id, "index_only", rows)
    return out_dir, rows


def index_unavailable_row(run_id, manifest, backend, notes, status="unavailable"):
    return {
        "backend": backend,
        "dataset_size": manifest["N"],
        "dim": manifest["dim"],
        "operation": "add_search",
        "total_ms": "",
        "qps": "",
        "avg_us": "",
        "p50_us": "",
        "p95_us": "",
        "p99_us": "",
        "max_us": "",
        "recall_at_10": "",
        "rss_kb": "",
        "error_count": 0,
        "notes": notes,
        "status": status,
        "run_id": run_id,
        "manifest": "",
    }


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


def run_server_path(run_id, availability):
    out_dir = SERVER_ROOT / run_id
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    port_base = 21000
    for ni, n in enumerate((10000, 100000)):
        manifest_path, manifest = load_manifest(n)
        for bi, backend in enumerate(["native_flat", "hnswlib_hnsw", "faiss_flat", "faiss_hnsw", "native_hnsw"]):
            if not availability[backend]["available"]:
                rows.extend(server_unavailable_rows(run_id, manifest, backend, availability[backend]["notes"]))
                continue
            cfg_backend = "flat" if backend == "native_flat" else backend
            port = port_base + ni * 100 + bi
            sync_port = port + 10000
            case_dir = out_dir / f"{n}_{backend}"
            case_dir.mkdir(parents=True, exist_ok=True)
            conf = case_dir / "backend_config.conf"
            data_dir = case_dir / "data"
            data_dir.mkdir(exist_ok=True)
            conf.write_text(config_text(port, sync_port, data_dir, cfg_backend), encoding="utf-8")
            server_log = case_dir / "server.log"
            case_rows = []
            with server_log.open("w", encoding="utf-8") as logf:
                srv = subprocess.Popen(["./kvstore", "-c", str(conf)], cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT, text=True)
            try:
                if not wait_port(port, timeout=10.0):
                    case_rows.extend(server_unavailable_rows(run_id, manifest, backend, "server did not listen", status="fail"))
                else:
                    prefix = f"{run_id}_{n}_{backend}"
                    scope = ""
                    vset = run_server_client(port, "vset", 1, prefix, scope, n, case_dir / "vset", prepare_only=True)
                    rss_after_vset = proc_rss_kb(srv.pid)
                    case_rows.append(server_row(run_id, manifest, backend, "VSET", vset, n, n, server_log, conf, rss_after_vset))
                    vsearch = run_server_client(port, "vsearch", manifest["nq"], prefix, scope, 0, case_dir / "vsearch")
                    rss_after_vsearch = proc_rss_kb(srv.pid)
                    case_rows.append(server_row(run_id, manifest, backend, "VSEARCH", vsearch, manifest["nq"], n, server_log, conf, rss_after_vsearch))
            finally:
                try:
                    srv.terminate()
                    srv.wait(timeout=5)
                except Exception:
                    srv.kill()
                    srv.wait(timeout=5)
            profile = parse_server_profile(server_log)
            for row in case_rows:
                apply_server_profile(row, profile)
            rows.extend(case_rows)
    raw_path = out_dir / "raw.csv"
    write_csv(raw_path, rows)
    write_server_summary(out_dir / "summary.md", rows)
    write_manifest(out_dir / "manifest.json", run_id, "server_path", rows)
    return out_dir, rows


def run_server_client(port, op, requests, prefix, scope, prepare, out_base, prepare_only=False):
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
    ]
    if scope:
        cmd += ["--scope", scope]
    else:
        cmd += ["--scope", ""]
    if prepare:
        cmd += ["--prepare", str(prepare)]
    if prepare_only:
        cmd += ["--prepare-only"]
    stdout_path = Path(str(out_base) + ".out")
    stderr_path = Path(str(out_base) + ".err")
    try:
        proc = run(cmd, timeout=300 if prepare >= 100000 else 180, capture=True)
        stdout_path.write_text(proc.stdout or "", encoding="utf-8")
        stderr_path.write_text(proc.stderr or "", encoding="utf-8")
        return {
            "proc": proc,
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
            "proc": None,
            "metrics": {},
            "timeout": True,
            "cmd": " ".join(cmd),
            "stdout": str(stdout_path),
            "stderr": str(stderr_path),
            "returncode": None,
        }


def proc_rss_kb(pid):
    status = Path(f"/proc/{pid}/status")
    try:
        for line in status.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("VmRSS:"):
                parts = line.split()
                if len(parts) >= 2:
                    return parts[1]
    except OSError:
        pass
    return ""


def parse_server_profile(server_log):
    profile = {}
    try:
        text = server_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return profile
    patterns = {
        "vset_flat_index_update_avg_us": r"vset_flat_index_update_avg_us=(\d+)",
        "vset_backend_add_avg_us": r"vset_backend_add_avg_us=(\d+)",
        "vsearch_search_avg_us": r"vsearch_search_avg_us=(\d+)",
    }
    for key, pattern in patterns.items():
        matches = re.findall(pattern, text)
        if matches:
            profile[key] = matches[-1]
    return profile


def apply_server_profile(row, profile):
    if row.get("status") not in ("pass", "fail"):
        return
    avg_us = to_float(row.get("avg_us"))
    index_avg = None
    if row.get("operation") == "VSET":
        if row.get("backend") == "native_flat":
            index_avg = to_float(profile.get("vset_flat_index_update_avg_us"))
        elif row.get("backend") == "hnswlib_hnsw":
            index_avg = to_float(profile.get("vset_backend_add_avg_us"))
    elif row.get("operation") == "VSEARCH":
        index_avg = to_float(profile.get("vsearch_search_avg_us"))
    if index_avg is not None:
        row["index_avg_us"] = f"{index_avg:.3f}"
        if avg_us is not None:
            row["non_index_avg_us"] = f"{max(0.0, avg_us - index_avg):.3f}"
    else:
        row["notes"] += "; profile index_avg_us unavailable"


def to_float(value):
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def server_unavailable_rows(run_id, manifest, backend, notes, status="unavailable"):
    return [server_blank_row(run_id, manifest, backend, "VSET", notes, status),
            server_blank_row(run_id, manifest, backend, "VSEARCH", notes, status)]


def server_blank_row(run_id, manifest, backend, op, notes, status):
    return {
        "backend": backend, "dataset_size": manifest["N"], "dim": manifest["dim"],
        "operation": op, "total_ms": "", "qps": "", "avg_us": "", "p50_us": "",
        "p95_us": "", "p99_us": "", "max_us": "", "success_count": 0,
        "error_count": 0, "timeout_count": 0, "rss_kb": "", "index_avg_us": "",
        "non_index_avg_us": "", "notes": notes, "status": status, "run_id": run_id,
    }


def server_row(run_id, manifest, backend, op, result, requests, prepared_count, server_log, conf, rss_kb):
    m = result["metrics"]
    timeout = result["timeout"]
    if op == "VSET" and "prepare_count" in m:
        success = int(m.get("prepare_success", 0) or 0)
        failed = int(m.get("prepare_failed", 0) or 0)
        errors = 0
        total_ms = m.get("prepare_elapsed_ms", "")
        qps = m.get("prepare_qps", "")
        avg_ms_key = "prepare_avg_ms"
        p50_ms_key = "prepare_p50_ms"
        p95_ms_key = "prepare_p95_ms"
        p99_ms_key = "prepare_p99_ms"
        max_ms_key = "prepare_max_ms"
    else:
        success = int(m.get("success", 0) or 0)
        failed = int(m.get("failed", 0) or 0)
        errors = int(m.get("errors", 0) or 0)
        total_ms = m.get("elapsed_ms", "")
        qps = m.get("qps", "")
        avg_ms_key = "avg_ms"
        p50_ms_key = "p50_ms"
        p95_ms_key = "p95_ms"
        p99_ms_key = "p99_ms"
        max_ms_key = "max_ms"
    avg_ms = float(m.get(avg_ms_key, 0) or 0)
    error_count = errors + failed
    status = "timeout" if timeout else ("pass" if success == requests and error_count == 0 and result.get("returncode") == 0 else "fail")
    stderr_note = ""
    if status == "fail":
        stderr_note = read_short_file(result.get("stderr", ""))
    return {
        "backend": backend,
        "dataset_size": manifest["N"],
        "dim": manifest["dim"],
        "operation": op,
        "total_ms": total_ms,
        "qps": qps,
        "avg_us": f"{avg_ms * 1000.0:.3f}" if m.get(avg_ms_key) else "",
        "p50_us": f"{float(m.get(p50_ms_key, 0) or 0) * 1000.0:.3f}" if m.get(p50_ms_key) else "",
        "p95_us": f"{float(m.get(p95_ms_key, 0) or 0) * 1000.0:.3f}" if m.get(p95_ms_key) else "",
        "p99_us": f"{float(m.get(p99_ms_key, 0) or 0) * 1000.0:.3f}" if m.get(p99_ms_key) else "",
        "max_us": f"{float(m.get(max_ms_key, 0) or 0) * 1000.0:.3f}" if m.get(max_ms_key) else "",
        "success_count": success,
        "error_count": error_count,
        "timeout_count": 1 if timeout else 0,
        "rss_kb": rss_kb,
        "index_avg_us": "",
        "non_index_avg_us": "",
        "notes": f"cmd={result['cmd']}; stdout={result.get('stdout', '')}; stderr={result.get('stderr', '')}; conf={conf}; server_log={server_log}; prepare={prepared_count if op == 'VSET' else 0}; server path uses text/hash embedding generated from manifest parameters{stderr_note}",
        "status": status,
        "run_id": run_id,
    }


def read_short_file(path):
    if not path:
        return ""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return ""
    if not text:
        return ""
    return "; stderr=" + text.replace("\n", " ")[:300]


def write_csv(path, rows):
    if not rows:
        return
    keys = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)


def write_index_summary(path, rows):
    lines = ["# Index-only Formal Benchmark", "", "| Dataset | Backend | Operation | Status | QPS | Avg us | P95 us | Recall@10 | Notes |", "|---:|---|---|---|---:|---:|---:|---:|---|"]
    for r in rows:
        lines.append(f"| {r['dataset_size']} | {r['backend']} | {r['operation']} | {r['status']} | {r['qps']} | {r['avg_us']} | {r['p95_us']} | {r['recall_at_10']} | {r['notes']} |")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_server_summary(path, rows):
    lines = ["# Server-path Formal Benchmark", "", "| Dataset | Backend | Operation | Status | QPS | Avg us | P95 us | Success | Errors | Timeouts | Notes |", "|---:|---|---|---|---:|---:|---:|---:|---:|---:|---|"]
    for r in rows:
        lines.append(f"| {r['dataset_size']} | {r['backend']} | {r['operation']} | {r['status']} | {r['qps']} | {r['avg_us']} | {r['p95_us']} | {r['success_count']} | {r['error_count']} | {r['timeout_count']} | {r['notes']} |")
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
    }
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-id", default=f"formal_{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()

    for n in (10000, 100000):
        ensure_dataset(n)

    index_dir = INDEX_ROOT / args.run_id
    index_dir.mkdir(parents=True, exist_ok=True)
    strict_probe = strict_build_probe(index_dir)
    availability = backend_availability()
    availability["strict_build"] = strict_probe

    if not args.skip_build or not (ROOT / "kvstore").exists():
        compile_default()
    compile_index_runner()

    index_dir, index_rows = run_index_only(args.run_id, availability)
    server_dir, server_rows = run_server_path(args.run_id, availability)

    availability_path = ROOT / f"results/vector_native_faiss/backend_availability_{args.run_id}.json"
    availability_path.write_text(json.dumps(availability, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"run_id={args.run_id}")
    print(f"availability={availability_path}")
    print(f"index_only={index_dir}")
    print(f"server_path={server_dir}")
    print(f"index_rows={len(index_rows)} server_rows={len(server_rows)}")


if __name__ == "__main__":
    main()
