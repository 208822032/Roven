#!/usr/bin/env python3
"""Run deterministic real-embedding cache shards and merge them atomically."""

from __future__ import annotations

import argparse
import concurrent.futures
import copy
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


CORE_FILES = ("embeddings.f32", "query_embeddings.f32", "records.jsonl", "queries.jsonl")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def output_entry(path: Path, rows: int | None = None, shape: list[int] | None = None) -> dict:
    result = {"path": path.name, "sha256": sha256(path), "size_bytes": path.stat().st_size}
    if rows is not None:
        result["rows"] = rows
    if shape is not None:
        result.update({"shape": shape, "dtype": "float32", "byte_order": "little"})
    return result


def run_cache_validate(generator: Path, directory: Path) -> dict:
    manifest = load_json(directory / "manifest.json")
    subprocess.run(
        [str(generator), "validate", "--dataset-id", manifest["dataset_id"], "--output-dir", str(directory)],
        check=True,
    )
    return manifest


def validate_shard(generator: Path, shard_dir: Path, expected: dict) -> dict:
    manifest = run_cache_validate(generator, shard_dir)
    shard = manifest.get("shard", {})
    for key in ("start_row", "end_row"):
        if shard.get(key) != expected[key]:
            raise ValueError(f"{shard_dir}: shard {key}={shard.get(key)!r}, expected {expected[key]}")
    if shard.get("row_count") != expected["end_row"] - expected["start_row"]:
        raise ValueError(f"{shard_dir}: shard row_count mismatch")
    if not shard.get("global_numeric_ids_preserved") or not shard.get("query_cache_reused"):
        raise ValueError(f"{shard_dir}: global IDs or query-cache reuse not confirmed")
    params = manifest["params"]
    for key in ("dim", "max_text_len", "input_format"):
        if params.get(key) != expected[key]:
            raise ValueError(f"{shard_dir}: params.{key} provenance mismatch")
    if params.get("batch_size") != expected["batch_size"] or params.get("num_threads") != expected["num_threads"]:
        raise ValueError(f"{shard_dir}: worker configuration mismatch")
    if manifest["inputs"]["records_tsv"]["sha256"] != expected["records_tsv_sha256"]:
        raise ValueError(f"{shard_dir}: records TSV checksum mismatch")
    expected_row = expected["start_row"]
    with (shard_dir / "records.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            row = json.loads(line)
            if row.get("row") != expected_row or row.get("id") != expected_row:
                raise ValueError(f"{shard_dir}: global row/id mismatch at {expected_row}")
            expected_row += 1
    if expected_row != expected["end_row"]:
        raise ValueError(f"{shard_dir}: records.jsonl row count mismatch")
    return manifest


def process_rss_kb(pid: int) -> int:
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        pass
    return 0


def generate_one(generator: Path, args: argparse.Namespace, shard: dict, expected: dict) -> dict:
    shard_dir = Path(shard["directory"])
    if shard_dir.exists():
        if not args.resume:
            raise FileExistsError(f"shard already exists without --resume: {shard_dir}")
        validate_shard(generator, shard_dir, expected | shard)
        return {"status": "reused", "shard": shard, "peak_rss_kb": 0, "pid": None}

    log_path = shard_dir.parent / f"{shard_dir.name}.log"
    cmd = [
        str(generator), "generate",
        "--dataset-id", shard["dataset_id"],
        "--records-tsv", str(args.records_tsv),
        "--queries-tsv", str(args.queries_tsv),
        "--input-format", "id_text",
        "--output-dir", str(shard_dir),
        "--start-row", str(shard["start_row"]),
        "--end-row", str(shard["end_row"]),
        "--expected-record-count", str(shard["end_row"] - shard["start_row"]),
        "--expected-query-count", str(expected["query_count"]),
        "--query-cache-dir", str(args.base_cache_dir),
        "--batch-size", str(args.batch_size),
        "--num-threads", str(args.threads_per_worker),
        "--max-text-len", str(args.max_text_len),
        "--model-path", str(args.model_path),
    ]
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8") as log:
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
        peak_rss = 0
        while proc.poll() is None:
            peak_rss = max(peak_rss, process_rss_kb(proc.pid))
            time.sleep(0.1)
        peak_rss = max(peak_rss, process_rss_kb(proc.pid))
    if proc.returncode != 0:
        for stale in shard_dir.parent.glob(f".tmp.{shard['dataset_id']}.*"):
            shutil.rmtree(stale, ignore_errors=True)
        raise subprocess.CalledProcessError(proc.returncode, cmd)
    manifest = validate_shard(generator, shard_dir, expected | shard)
    return {
        "status": "generated",
        "shard": shard,
        "peak_rss_kb": max(peak_rss, manifest.get("resource_usage", {}).get("peak_rss_kb", 0)),
        "wall_us": round((time.monotonic() - started) * 1_000_000),
        "pid": proc.pid,
        "log": str(log_path),
    }


def verify_base_prefix(base_dir: Path, records_tsv: Path, start_row: int) -> dict:
    manifest = load_json(base_dir / "manifest.json")
    if manifest["params"]["record_count"] != start_row:
        raise ValueError("base cache record_count must equal --start-row")
    with records_tsv.open(encoding="utf-8") as source, (base_dir / "records.jsonl").open(encoding="utf-8") as cached:
        if source.readline().rstrip("\r\n") != "pid\ttext":
            raise ValueError("records TSV header must be pid<TAB>text")
        for row_id in range(start_row):
            source_line = source.readline()
            cached_line = cached.readline()
            if not source_line or not cached_line:
                raise ValueError("base cache/source prefix ended early")
            pid = source_line.split("\t", 1)[0]
            row = json.loads(cached_line)
            if row.get("row") != row_id or row.get("id") != row_id or row.get("pid") != pid:
                raise ValueError(f"base cache is not the source prefix at row {row_id}")
    return manifest


def concatenate(inputs: list[Path], output: Path) -> None:
    with output.open("wb") as dst:
        for path in inputs:
            with path.open("rb") as src:
                shutil.copyfileobj(src, dst, 1024 * 1024)


def merge_cache(args: argparse.Namespace, base: dict, shards: list[dict], run_metrics: dict) -> Path:
    merged_dir = Path(args.output_root) / "merged_cache"
    if merged_dir.exists():
        if args.resume:
            existing = run_cache_validate(Path(args.generator), merged_dir)
            if existing.get("sharding", {}).get("range_end_row") == args.end_row:
                return merged_dir
        raise FileExistsError(f"merged output already exists: {merged_dir}")

    tmp_dir = Path(tempfile.mkdtemp(prefix=".tmp.merged_cache.", dir=args.output_root))
    try:
        shard_dirs = [Path(item["directory"]) for item in shards]
        concatenate([Path(args.base_cache_dir) / "embeddings.f32"] + [p / "embeddings.f32" for p in shard_dirs],
                    tmp_dir / "embeddings.f32")
        concatenate([Path(args.base_cache_dir) / "records.jsonl"] + [p / "records.jsonl" for p in shard_dirs],
                    tmp_dir / "records.jsonl")
        for name in ("query_embeddings.f32", "queries.jsonl"):
            shutil.copyfile(Path(args.base_cache_dir) / name, tmp_dir / name)

        manifest = copy.deepcopy(base)
        manifest["dataset_id"] = args.dataset_id
        manifest["created_at_utc"] = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        manifest["generator"] = {
            "name": "run_real_embedding_cache_shards.py",
            "version": "1",
            "source_sha256": sha256(Path(__file__)),
        }
        manifest["inputs"]["records_tsv"] = {
            "path": str(Path(args.records_tsv).resolve()),
            "sha256": run_metrics["records_tsv_sha256"],
            "size_bytes": Path(args.records_tsv).stat().st_size,
            "rows": args.end_row,
        }
        manifest["params"].update({
            "record_count": args.end_row,
            "query_count": base["params"]["query_count"],
            "batch_size": args.batch_size,
            "num_threads": args.threads_per_worker,
            "expected_record_count": args.end_row,
        })
        dim = manifest["params"]["dim"]
        query_count = manifest["params"]["query_count"]
        manifest["outputs"] = {
            "embeddings_f32": output_entry(tmp_dir / "embeddings.f32", shape=[args.end_row, dim]),
            "query_embeddings_f32": output_entry(tmp_dir / "query_embeddings.f32", shape=[query_count, dim]),
            "records_jsonl": output_entry(tmp_dir / "records.jsonl", rows=args.end_row),
            "queries_jsonl": output_entry(tmp_dir / "queries.jsonl", rows=query_count),
        }
        manifest["sharding"] = {
            "schema_version": 1,
            "base_cache_dir": str(Path(args.base_cache_dir).resolve()),
            "base_record_count": args.start_row,
            "range_start_row": args.start_row,
            "range_end_row": args.end_row,
            "worker_count": args.workers,
            "threads_per_worker": args.threads_per_worker,
            "batch_size": args.batch_size,
            "shards": [{k: item[k] for k in ("dataset_id", "start_row", "end_row", "directory")} for item in shards],
            "ranges_contiguous": True,
            "global_row_order_preserved": True,
            "query_cache_reused": True,
            "core_outputs_byte_identical_to_reference": run_metrics.get("core_outputs_byte_identical_to_reference"),
        }
        manifest["parallel_run"] = run_metrics
        with (tmp_dir / "manifest.json").open("w", encoding="utf-8") as stream:
            json.dump(manifest, stream, ensure_ascii=False, sort_keys=True, indent=2)
            stream.write("\n")
        os.replace(tmp_dir, merged_dir)
    except Exception:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        raise
    run_cache_validate(Path(args.generator), merged_dir)
    return merged_dir


def compare_reference(merged_dir: Path, reference_dir: Path) -> dict:
    checks = {}
    for name in CORE_FILES:
        checks[name] = {
            "actual_sha256": sha256(merged_dir / name),
            "reference_sha256": sha256(reference_dir / name),
        }
        checks[name]["identical"] = checks[name]["actual_sha256"] == checks[name]["reference_sha256"]
    if not all(item["identical"] for item in checks.values()):
        raise ValueError("merged core outputs are not byte-identical to reference cache")
    return checks


def ranges(start: int, end: int, workers: int, shard_size: int | None) -> list[tuple[int, int]]:
    size = shard_size or math.ceil((end - start) / workers)
    return [(row, min(row + size, end)) for row in range(start, end, size)]


def command_run(args: argparse.Namespace) -> int:
    args.generator = Path(args.generator).resolve()
    args.records_tsv = Path(args.records_tsv).resolve()
    args.queries_tsv = Path(args.queries_tsv).resolve()
    args.base_cache_dir = Path(args.base_cache_dir).resolve()
    args.model_path = Path(args.model_path).resolve()
    args.output_root = Path(args.output_root).resolve()
    args.output_root.mkdir(parents=True, exist_ok=True)
    if args.end_row <= args.start_row or args.workers <= 0 or args.threads_per_worker <= 0:
        raise ValueError("invalid range or worker/thread count")
    base = run_cache_validate(args.generator, args.base_cache_dir)
    verify_base_prefix(args.base_cache_dir, args.records_tsv, args.start_row)
    records_sha = sha256(args.records_tsv)
    query_count = base["params"]["query_count"]
    expected = {
        "dim": base["params"]["dim"],
        "max_text_len": args.max_text_len,
        "input_format": "id_text",
        "batch_size": args.batch_size,
        "num_threads": args.threads_per_worker,
        "records_tsv_sha256": records_sha,
        "query_count": query_count,
    }
    shard_root = args.output_root / "shards"
    shard_root.mkdir(exist_ok=True)
    shards = []
    for start, end in ranges(args.start_row, args.end_row, args.workers, args.shard_size):
        dataset_id = f"{args.dataset_id}_rows_{start}_{end}"
        shards.append({
            "dataset_id": dataset_id,
            "start_row": start,
            "end_row": end,
            "directory": str(shard_root / dataset_id),
        })

    before = os.times()
    wall_start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(generate_one, args.generator, args, shard, expected) for shard in shards]
        results = [future.result() for future in futures]
    wall_seconds = time.monotonic() - wall_start
    after = os.times()
    generated_rows = sum(
        item["shard"]["end_row"] - item["shard"]["start_row"]
        for item in results if item["status"] == "generated"
    )
    child_cpu_seconds = (after.children_user - before.children_user) + (after.children_system - before.children_system)
    metrics = {
        "schema_version": 1,
        "dataset_id": args.dataset_id,
        "records_tsv_sha256": records_sha,
        "range_start_row": args.start_row,
        "range_end_row": args.end_row,
        "generated_record_count": generated_rows,
        "worker_count": args.workers,
        "threads_per_worker": args.threads_per_worker,
        "batch_size": args.batch_size,
        "wall_seconds": wall_seconds,
        "aggregate_records_per_second": generated_rows / wall_seconds if wall_seconds and generated_rows else None,
        "child_cpu_seconds": child_cpu_seconds,
        "aggregate_cpu_utilization_percent": child_cpu_seconds / wall_seconds * 100.0 if wall_seconds else None,
        "sum_worker_peak_rss_kb": sum(item["peak_rss_kb"] for item in results),
        "max_worker_peak_rss_kb": max((item["peak_rss_kb"] for item in results), default=0),
        "worker_results": results,
        "core_outputs_byte_identical_to_reference": None,
    }
    merged = merge_cache(args, base, shards, metrics)
    if args.reference_cache_dir:
        checks = compare_reference(merged, Path(args.reference_cache_dir).resolve())
        metrics["core_outputs_byte_identical_to_reference"] = True
        metrics["reference_checks"] = checks
        manifest = load_json(merged / "manifest.json")
        manifest["parallel_run"] = metrics
        manifest["sharding"]["core_outputs_byte_identical_to_reference"] = True
        with (merged / "manifest.json").open("w", encoding="utf-8") as stream:
            json.dump(manifest, stream, ensure_ascii=False, sort_keys=True, indent=2)
            stream.write("\n")
    with (args.output_root / "run_result.json").open("w", encoding="utf-8") as stream:
        json.dump(metrics, stream, ensure_ascii=False, sort_keys=True, indent=2)
        stream.write("\n")
    print(json.dumps({
        "merged_cache": str(merged),
        "aggregate_records_per_second": metrics["aggregate_records_per_second"],
        "aggregate_cpu_utilization_percent": metrics["aggregate_cpu_utilization_percent"],
        "sum_worker_peak_rss_kb": metrics["sum_worker_peak_rss_kb"],
    }, sort_keys=True))
    return 0


def command_validate(args: argparse.Namespace) -> int:
    root = Path(args.output_root).resolve()
    result = load_json(root / "run_result.json")
    merged = root / "merged_cache"
    manifest = run_cache_validate(Path(args.generator).resolve(), merged)
    if manifest.get("sharding", {}).get("range_end_row") != result["range_end_row"]:
        raise ValueError("run result / merged manifest range mismatch")
    for shard in manifest["sharding"]["shards"]:
        expected = {
            **shard,
            "dim": manifest["params"]["dim"],
            "max_text_len": manifest["params"]["max_text_len"],
            "input_format": manifest["params"]["input_format"],
            "batch_size": result["batch_size"],
            "num_threads": result["threads_per_worker"],
            "records_tsv_sha256": result["records_tsv_sha256"],
        }
        validate_shard(Path(args.generator).resolve(), Path(shard["directory"]), expected)
    print(f"[validate] PASS output_root={root} shards={len(manifest['sharding']['shards'])}")
    return 0


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run")
    run.add_argument("--dataset-id", required=True)
    run.add_argument("--records-tsv", required=True)
    run.add_argument("--queries-tsv", required=True)
    run.add_argument("--base-cache-dir", required=True)
    run.add_argument("--output-root", required=True)
    run.add_argument("--start-row", type=int, required=True)
    run.add_argument("--end-row", type=int, required=True)
    run.add_argument("--workers", type=int, required=True)
    run.add_argument("--threads-per-worker", type=int, required=True)
    run.add_argument("--batch-size", type=int, required=True)
    run.add_argument("--shard-size", type=int)
    run.add_argument("--max-text-len", type=int, default=1048576)
    run.add_argument("--model-path", default="models/bge-small-zh-v1.5/model.onnx")
    run.add_argument("--generator", default="build/tools/bin/generate_real_embedding_cache")
    run.add_argument("--reference-cache-dir")
    run.add_argument("--resume", action="store_true")
    run.set_defaults(func=command_run)
    validate = sub.add_parser("validate")
    validate.add_argument("--output-root", required=True)
    validate.add_argument("--generator", default="build/tools/bin/generate_real_embedding_cache")
    validate.set_defaults(func=command_validate)
    return p


def main() -> int:
    try:
        args = parser().parse_args()
        return args.func(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print(f"[error] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
