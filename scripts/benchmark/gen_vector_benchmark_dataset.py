#!/usr/bin/env python3
"""Generate reproducible vector benchmark datasets for KVLite smoke tests."""

import argparse
import json
import math
import random
import struct
from datetime import datetime, timezone
from pathlib import Path


GENERATOR_VERSION = "1"


def l2_normalize(values):
    norm = math.sqrt(sum(v * v for v in values))
    if norm <= 0.0:
        return values
    return [v / norm for v in values]


def make_vectors(count, dim, rng, normalize):
    for _ in range(count):
        row = [rng.uniform(-1.0, 1.0) for _ in range(dim)]
        if normalize:
            row = l2_normalize(row)
        yield row


def write_f32_rows(path, rows):
    with path.open("wb") as f:
        for row in rows:
            f.write(struct.pack("<%df" % len(row), *row))


def build_dataset_id(args):
    norm = "norm" if args.normalize else "raw"
    scope = args.scope if args.scope else "empty"
    return (
        f"smoke_N{args.count}_nq{args.query_count}_d{args.dim}_"
        f"seed{args.seed}_{args.metric}_{norm}_top{args.topk}_scope-{scope}"
    )


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate KVLite native/Faiss benchmark dataset files."
    )
    parser.add_argument("--count", "-N", type=int, default=1000)
    parser.add_argument("--query-count", "--nq", type=int, default=100)
    parser.add_argument("--dim", type=int, default=64)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--metric", default="ip", choices=("ip", "l2", "cosine"))
    parser.add_argument("--normalize", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--topk", type=int, default=10)
    parser.add_argument("--scope", default="")
    parser.add_argument(
        "--output-root",
        default="results/vector_native_faiss/datasets",
        help="Directory that will contain <dataset_id>/",
    )
    parser.add_argument("--dataset-id", default="")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.count <= 0:
        raise SystemExit("--count must be positive")
    if args.query_count <= 0:
        raise SystemExit("--query-count must be positive")
    if args.dim <= 0:
        raise SystemExit("--dim must be positive")
    if args.topk <= 0:
        raise SystemExit("--topk must be positive")

    dataset_id = args.dataset_id or build_dataset_id(args)
    out_dir = Path(args.output_root) / dataset_id
    out_dir.mkdir(parents=True, exist_ok=True)

    dataset_path = out_dir / "dataset.bin"
    queries_path = out_dir / "queries.bin"
    manifest_path = out_dir / "manifest.json"

    rng = random.Random(args.seed)
    write_f32_rows(dataset_path, make_vectors(args.count, args.dim, rng, args.normalize))
    write_f32_rows(queries_path, make_vectors(args.query_count, args.dim, rng, args.normalize))

    manifest = {
        "dataset_id": dataset_id,
        "seed": args.seed,
        "N": args.count,
        "nq": args.query_count,
        "dim": args.dim,
        "metric": args.metric,
        "normalize": args.normalize,
        "topK": args.topk,
        "scope": args.scope,
        "dtype": "float32_le",
        "dataset_path": str(dataset_path),
        "queries_path": str(queries_path),
        "dataset_bytes": dataset_path.stat().st_size,
        "queries_bytes": queries_path.stat().st_size,
        "created_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "generator": {
            "path": "scripts/benchmark/gen_vector_benchmark_dataset.py",
            "version": GENERATOR_VERSION,
        },
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(f"dataset_id={dataset_id}")
    print(f"output_dir={out_dir}")
    print(f"dataset_path={dataset_path}")
    print(f"queries_path={queries_path}")
    print(f"manifest_path={manifest_path}")


if __name__ == "__main__":
    main()
