#!/usr/bin/env python3
"""
Stage 4A: Exact-backend consistency diagnostic.

Compares core_flat vs faiss_flat at small-to-medium scales to identify
the root cause of faiss_flat recall@10 anomaly.

Only tests 2 backends: core_flat, faiss_flat.
"""

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
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
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
vector_hnsw_m = 16
vector_hnsw_ef_construction = 128
vector_hnsw_ef_search = 128
vector_hnsw_initial_capacity = {n + 128}
""")


def start_server(kv_bin: Path, root: Path, conf: Path, log_path: Path):
    log = open(log_path, "wb")
    proc = subprocess.Popen(
        [str(kv_bin), "--config", str(conf)],
        cwd=str(root),
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    return proc, log


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


def query_set(n: int, query_count: int) -> List[Tuple[str, str, int]]:
    """Return (question, scope, index) for each query."""
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
        out.append((f"bench_q_{i:08d}_{scope}", scope, i))
    return out


def extract_hits(resp: Any) -> List[Tuple[str, float]]:
    """Extract (question, score) pairs from VSEARCH response.
    Format: score:N.NNNNNN|scope:...|question:...|answer:...
    """
    hits: List[Tuple[str, float]] = []
    for item in flatten(resp):
        qm = re.search(r"question:([^ \t\r\n,|]+)", item)
        sm = re.search(r"score:([-]?\d+\.?\d*)", item)
        if qm:
            score = float(sm.group(1)) if sm else float("nan")
            hits.append((qm.group(1), score))
    return hits


def recall_at_k(actual: List[str], expected: List[str], k: int) -> Optional[float]:
    if not expected:
        return None
    actual_set = set(actual[:k])
    expected_top = expected[:k]
    return len(actual_set.intersection(expected_top)) / float(len(expected_top))


def run_one_backend(
    backend: str,
    kv_bin: Path,
    root: Path,
    tmp_root: Path,
    args: argparse.Namespace,
) -> Dict[str, Any]:
    """Run a single backend and return detailed search results."""
    port = pick_port()
    run_dir = tmp_root / backend
    run_dir.mkdir(parents=True, exist_ok=True)
    rdb = run_dir / f"{backend}.rdb"
    conf = run_dir / f"{backend}.conf"
    log_path = run_dir / f"{backend}.log"

    write_config(conf, port, rdb, backend, args.dim, args.n)

    proc, log = start_server(kv_bin, root, conf, log_path)
    result: Dict[str, Any] = {"backend": backend, "status": "pass", "error": None}

    try:
        wait_port(port)
        client = RespClient(port)
        try:
            # Verify backend
            actual_backend = info_map(client.cmd("VINFO")).get("index_type")
            if actual_backend != backend:
                result["status"] = "unavailable"
                result["error"] = f"expected {backend}, got {actual_backend}"
                return result

            # Insert all records
            for question, answer, scope in records(args.n):
                assert_ok(client.cmd("VSET", question, answer, scope),
                          f"{backend} VSET {question}")

            # Read VINFO for stats
            vinfo = info_map(client.cmd("VINFO"))
            result["active_records"] = int(vinfo.get("active_records", "0") or 0)
            result["index_type"] = vinfo.get("index_type")
            result["total_vector_memory_bytes"] = int(
                vinfo.get("total_vector_memory_bytes", "0") or 0)

            # Run queries
            queries = query_set(args.n, args.queries)
            per_query: List[Dict[str, Any]] = []

            for question, scope, idx in queries:
                resp = client.cmd("VSEARCH", question, scope, args.topk)
                if isinstance(resp, dict):
                    per_query.append({
                        "query_idx": idx,
                        "question": question,
                        "scope": scope,
                        "error": str(resp),
                        "hits": [],
                        "num_results": 0,
                    })
                    continue

                hits = extract_hits(resp)
                per_query.append({
                    "query_idx": idx,
                    "question": question,
                    "scope": scope,
                    "hits": hits,  # list of (question, score)
                    "num_results": len(hits),
                    "questions": [h[0] for h in hits],
                    "scores": [h[1] for h in hits],
                })

            result["per_query"] = per_query
            result["vsearch_total_results"] = sum(
                q["num_results"] for q in per_query)
        finally:
            client.close()
    except Exception as exc:
        result["status"] = "fail"
        result["error"] = str(exc)
    finally:
        stop_server(proc, log)

    return result


def compare_results(
    core_result: Dict[str, Any],
    faiss_result: Dict[str, Any],
    args: argparse.Namespace,
) -> Dict[str, Any]:
    """Compare core_flat and faiss_flat results query-by-query."""
    core_queries = {q["question"]: q for q in (core_result.get("per_query") or [])}
    faiss_queries = {q["question"]: q for q in (faiss_result.get("per_query") or [])}

    comparison: Dict[str, Any] = {
        "n": args.n,
        "dim": args.dim,
        "topk": args.topk,
        "queries": args.queries,
        "total_queries": len(core_queries),
    }

    per_query_compare: List[Dict[str, Any]] = []
    recalls = []
    exact_matches = 0
    first_mismatch = None

    for question, cq in sorted(core_queries.items()):
        fq = faiss_queries.get(question)
        if fq is None:
            continue

        c_questions = cq["questions"][:args.topk]
        f_questions = fq["questions"][:args.topk]
        c_set = set(c_questions)
        f_set = set(f_questions)

        intersection = c_set & f_set
        missing = c_set - f_set  # in core_flat but not faiss_flat
        extra = f_set - c_set     # in faiss_flat but not core_flat

        r = recall_at_k(f_questions, c_questions, args.topk)
        if r is not None:
            recalls.append(r)

        is_exact = (c_questions == f_questions)
        if is_exact:
            exact_matches += 1

        qc = {
            "question": question,
            "scope": cq["scope"],
            "core_num_results": cq["num_results"],
            "faiss_num_results": fq["num_results"],
            "intersection_size": len(intersection),
            "missing_count": len(missing),
            "extra_count": len(extra),
            "recall_at_k": r,
            "exact_match": is_exact,
        }

        if not is_exact and first_mismatch is None:
            first_mismatch = {
                "question": question,
                "scope": cq["scope"],
                "query_idx": cq["query_idx"],
                "core_topk": [
                    {"question": q, "score": s}
                    for q, s in zip(c_questions, cq["scores"][:args.topk])
                ],
                "faiss_topk": [
                    {"question": q, "score": s}
                    for q, s in zip(f_questions, fq["scores"][:args.topk])
                ],
                "intersection": sorted(intersection),
                "missing": sorted(missing),
                "extra": sorted(extra),
                "core_num_results": cq["num_results"],
                "faiss_num_results": fq["num_results"],
            }

        # If mismatch, include detailed diff
        if not is_exact:
            qc["missing_keys"] = sorted(missing)
            qc["extra_keys"] = sorted(extra)
            qc["core_topk_detail"] = [
                {"question": q, "score": s}
                for q, s in zip(c_questions, cq["scores"][:args.topk])
            ]
            qc["faiss_topk_detail"] = [
                {"question": q, "score": s}
                for q, s in zip(f_questions, fq["scores"][:args.topk])
            ]

        per_query_compare.append(qc)

    comparison["per_query"] = per_query_compare
    comparison["aggregate"] = {
        "recall_at_k_mean": statistics.fmean(recalls) if recalls else None,
        "recall_at_k_min": min(recalls) if recalls else None,
        "recall_at_k_max": max(recalls) if recalls else None,
        "exact_match_rate": exact_matches / len(per_query_compare) if per_query_compare else 0,
        "total_exact_matches": exact_matches,
        "total_queries_with_results": len(per_query_compare),
    }
    comparison["first_mismatch"] = first_mismatch

    return comparison


def print_diagnostic(comparison: Dict[str, Any]) -> None:
    """Print human-readable diagnostic summary."""
    agg = comparison["aggregate"]
    fm = comparison.get("first_mismatch")

    print()
    print("=" * 72)
    print(f"  Exact Consistency Diagnostic: n={comparison['n']} dim={comparison['dim']}d")
    print("=" * 72)
    print(f"  recall@10  mean: {agg['recall_at_k_mean']:.4f}" if agg['recall_at_k_mean'] is not None else "  recall@10  mean: N/A")
    print(f"             min:  {agg['recall_at_k_min']:.4f}" if agg['recall_at_k_min'] is not None else "")
    print(f"             max:  {agg['recall_at_k_max']:.4f}" if agg['recall_at_k_max'] is not None else "")
    print(f"  exact match rate: {agg['exact_match_rate']:.4f} ({agg['total_exact_matches']}/{agg['total_queries_with_results']})")
    print()

    # Per-query summary stats
    pq = comparison.get("per_query") or []
    mismatches = [q for q in pq if not q["exact_match"]]
    print(f"  Queries with any difference: {len(mismatches)}/{len(pq)}")
    if mismatches:
        # Analyze mismatch patterns
        fewer_results = [q for q in mismatches if q["faiss_num_results"] < q["core_num_results"]]
        more_results = [q for q in mismatches if q["faiss_num_results"] > q["core_num_results"]]
        num_diff_only = [q for q in mismatches if q["faiss_num_results"] == q["core_num_results"]]

        print(f"    FAISS returned fewer results:  {len(fewer_results)}")
        print(f"    FAISS returned more results:    {len(more_results)}")
        print(f"    Same count but different keys:  {len(num_diff_only)}")

        if fewer_results:
            avg_diff = sum(q["core_num_results"] - q["faiss_num_results"] for q in fewer_results) / len(fewer_results)
            print(f"    Avg result deficit (fewer):     {avg_diff:.1f}")

    print()

    # Print first mismatch detail
    if fm:
        print("-" * 72)
        print(f"  FIRST MISMATCH")
        print(f"  query:   {fm['question']}")
        print(f"  scope:   {fm['scope']}")
        print(f"  idx:     {fm['query_idx']}")
        print(f"  core_flat results:  {fm['core_num_results']}")
        print(f"  faiss_flat results: {fm['faiss_num_results']}")
        print()
        print(f"  intersection: {len(fm['intersection'])} keys")
        print(f"  missing (core has, faiss lacks): {len(fm['missing'])} keys")
        print(f"  extra   (faiss has, core lacks): {len(fm['extra'])} keys")
        print()
        print(f"  {'core_flat top-10':<50s} | {'faiss_flat top-10':<50s}")
        print(f"  {'-'*48} | {'-'*48}")
        max_len = max(len(fm["core_topk"]), len(fm["faiss_topk"]))
        for i in range(max_len):
            c = fm["core_topk"][i] if i < len(fm["core_topk"]) else None
            f = fm["faiss_topk"][i] if i < len(fm["faiss_topk"]) else None
            c_str = f"{c['question']} ({c['score']:.6f})" if c else ""
            f_str = f"{f['question']} ({f['score']:.6f})" if f else ""
            marker = " <>" if (c and f and c["question"] != f["question"]) else ""
            print(f"  {c_str:<50s} | {f_str:<50s}{marker}")

        if fm["missing"]:
            print(f"\n  Missing keys (in core_flat but NOT in faiss_flat):")
            for k in fm["missing"]:
                print(f"    - {k}")
        if fm["extra"]:
            print(f"\n  Extra keys (in faiss_flat but NOT in core_flat):")
            for k in fm["extra"]:
                print(f"    + {k}")

    # Score direction check: verify higher score = better rank for both
    print()
    print("-" * 72)
    print("  SCORE DIRECTION CHECK (first query with results)")
    for q in (pq or [])[:1]:
        c_scores = [h["score"] for h in q.get("core_topk_detail", [])]
        f_scores = [h["score"] for h in q.get("faiss_topk_detail", [])]
        if c_scores:
            c_desc = all(c_scores[i] >= c_scores[i+1] for i in range(len(c_scores)-1))
            print(f"  core_flat scores descending: {c_desc}  values: {[f'{s:.4f}' for s in c_scores[:5]]}")
        if f_scores:
            f_desc = all(f_scores[i] >= f_scores[i+1] for i in range(len(f_scores)-1))
            print(f"  faiss_flat scores descending: {f_desc}  values: {[f'{s:.4f}' for s in f_scores[:5]]}")

    print()
    print("=" * 72)


def print_ann_audit(
    comparison: Dict[str, Any],
    core_result: Dict[str, Any],
    test_result: Dict[str, Any],
    args: argparse.Namespace,
) -> None:
    """Print ANN overfetch audit summary for hnswlib_hnsw vs core_flat."""
    agg = comparison["aggregate"]
    pq = comparison.get("per_query") or []

    print()
    print("=" * 72)
    print(f"  ANN Overfetch Audit: core_flat vs hnswlib_hnsw")
    print(f"  n={comparison['n']} dim={comparison['dim']}d topk={comparison['topk']}")
    print("=" * 72)
    print(f"  recall@10  mean: {agg['recall_at_k_mean']:.4f}" if agg['recall_at_k_mean'] is not None else "  recall@10  mean: N/A")
    print(f"             min:  {agg['recall_at_k_min']:.4f}" if agg['recall_at_k_min'] is not None else "")
    print(f"             max:  {agg['recall_at_k_max']:.4f}" if agg['recall_at_k_max'] is not None else "")
    print(f"  exact match rate: {agg['exact_match_rate']:.4f} ({agg['total_exact_matches']}/{agg['total_queries_with_results']})")
    print()

    # Analyze result count distribution
    core_counts = [q["core_num_results"] for q in pq]
    test_counts = [q.get("faiss_num_results") or q.get("hnswlib_num_results", 0) for q in pq]

    # Need to fix: comparison uses "faiss_num_results" key from compare_results
    # Let's check what keys are used
    test_key = "faiss_num_results"  # compare_results always uses this key

    insufficient = [q for q in pq if q.get(test_key, 0) < args.topk]
    sufficient = [q for q in pq if q.get(test_key, 0) >= args.topk]

    print(f"  --- Result Count Analysis ---")
    print(f"  core_flat:     all queries returned {args.topk} results")
    print(f"  hnswlib_hnsw:  {len(insufficient)}/{len(pq)} queries returned < {args.topk} results")
    if core_counts:
        print(f"  core_flat avg results:     {sum(core_counts)/len(core_counts):.1f}")
    if test_counts:
        print(f"  hnswlib_hnsw avg results:  {sum(test_counts)/len(test_counts):.1f}")
    print()

    # Scope filtering impact
    if insufficient:
        avg_deficit = sum(args.topk - q.get(test_key, 0) for q in insufficient) / len(insufficient)
        print(f"  --- Scope Filtering Impact ---")
        print(f"  Queries with insufficient results: {len(insufficient)}")
        print(f"  Average deficit: {avg_deficit:.1f} results below topk={args.topk}")
        print(f"  This indicates scope filtering is dropping candidates.")
        print(f"  With overfetch fix, hnswlib now requests topk*4+32 candidates,")
        print(f"  giving scope filter enough pool to fill topk results.")
    else:
        print(f"  --- Scope Filtering ---")
        print(f"  All queries return >= topk results. Overfetch is sufficient.")
    print()

    # Recall breakdown
    if agg['recall_at_k_mean'] is not None:
        high_recall = sum(1 for q in pq if (q.get("recall_at_k") or 0) >= 0.95)
        mid_recall = sum(1 for q in pq if 0.8 <= (q.get("recall_at_k") or 0) < 0.95)
        low_recall = sum(1 for q in pq if (q.get("recall_at_k") or 0) < 0.8)
        print(f"  --- Recall Distribution ---")
        print(f"  recall >= 0.95:  {high_recall}/{len(pq)}")
        print(f"  recall 0.80-0.95: {mid_recall}/{len(pq)}")
        print(f"  recall < 0.80:   {low_recall}/{len(pq)}")
        print()
        if low_recall > 0:
            print(f"  NOTE: {low_recall} queries have recall < 0.80.")
            print(f"  If overfetch is now sufficient, this is ANN approximation error,")
            print(f"  not an overfetch deficiency.")
        else:
            print(f"  All queries have recall >= 0.80 with corrected overfetch.")
    print()

    # First mismatch detail
    fm = comparison.get("first_mismatch")
    if fm:
        print("-" * 72)
        print(f"  FIRST MISMATCH (example)")
        print(f"  query:   {fm['question']}")
        print(f"  core_flat results:  {fm['core_num_results']}")
        print(f"  hnswlib results:    {fm.get('faiss_num_results', '?')}")
        print(f"  intersection: {len(fm['intersection'])} keys")
        print(f"  missing: {len(fm['missing'])} keys")
        print(f"  extra:   {len(fm['extra'])} keys")
        print()

    print("=" * 72)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Stage 4A: Exact-backend consistency diagnostic"
    )
    parser.add_argument("--kv-bin", default="./kvstore")
    parser.add_argument("--n", type=int, default=100, help="Number of records")
    parser.add_argument("--dim", type=int, default=64)
    parser.add_argument("--topk", type=int, default=10)
    parser.add_argument("--queries", type=int, default=50,
                        help="Number of queries (default 50)")
    parser.add_argument("--out-dir", default="results/vector_core_benchmark")
    parser.add_argument("--mode", choices=["exact", "ann-overfetch-audit"],
                        default="exact",
                        help="exact: core_flat vs faiss_flat; "
                             "ann-overfetch-audit: core_flat vs hnswlib_hnsw")
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

    if args.mode == "ann-overfetch-audit":
        backends = ["core_flat", "hnswlib_hnsw"]
    else:
        backends = ["core_flat", "faiss_flat"]

    with tempfile.TemporaryDirectory(prefix="kvlite_stage4_consistency_") as td:
        tmp_root = Path(td)
        results: Dict[str, Any] = {}

        for backend in backends:
            print(f"[diag] backend={backend} n={args.n} dim={args.dim}", flush=True)
            results[backend] = run_one_backend(
                backend, kv_bin, root, tmp_root, args
            )
            status = results[backend]["status"]
            print(f"[diag]   status={status} "
                  f"active={results[backend].get('active_records','?')} "
                  f"total_results={results[backend].get('vsearch_total_results','?')}",
                  flush=True)
            if status != "pass":
                print(f"[diag]   ERROR: {results[backend].get('error')}", flush=True)

    # Compare
    test_backend = backends[1]  # faiss_flat or hnswlib_hnsw
    if results.get("core_flat", {}).get("status") == "pass" and \
       results.get(test_backend, {}).get("status") == "pass":
        comparison = compare_results(results["core_flat"], results[test_backend], args)

        if args.mode == "ann-overfetch-audit":
            print_ann_audit(comparison, results["core_flat"], results[test_backend], args)
        else:
            print_diagnostic(comparison)

        # Save detailed JSON
        tag = f"{args.n}_{args.dim}d" if args.n < 1000 else f"{args.n//1000}k_{args.dim}d"
        mode_tag = "ann_audit" if args.mode == "ann-overfetch-audit" else "exact_consistency"
        json_path = out_dir / f"stage4b_{mode_tag}_{tag}.json"
        json_path.write_text(json.dumps(comparison, indent=2, sort_keys=True) + "\n")
        print(f"[diag] wrote {json_path}")

        if args.mode == "ann-overfetch-audit":
            # ANN audit: flag if recall is low due to overfetch vs approximation
            agg = comparison["aggregate"]
            if agg["recall_at_k_mean"] is not None:
                print(f"[diag] ANN audit recall@10 mean={agg['recall_at_k_mean']:.4f}")
            return 0
        else:
            if comparison["aggregate"]["exact_match_rate"] < 1.0:
                print("[diag] WARNING: exact match rate < 1.0 — anomaly confirmed")
                return 1
            else:
                print("[diag] OK: exact match rate = 1.0")
                return 0
    else:
        print("[diag] ERROR: one or both backends failed")
        return 2


if __name__ == "__main__":
    sys.exit(main())
