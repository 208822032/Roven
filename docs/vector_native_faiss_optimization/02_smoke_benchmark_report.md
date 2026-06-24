# 1K/64d Smoke Benchmark Report

## 1. Goal

This smoke validates the native vs Faiss benchmark foundation at 1K/64d scale only. It is not a formal benchmark, does not run 10K/100K, and must not be used to infer formal latency, QPS, memory, recall, or 100K scaling.

## 2. Files

Documentation:

```text
docs/vector_native_faiss_optimization/README.md
docs/vector_native_faiss_optimization/00_benchmark_plan_review.md
docs/vector_native_faiss_optimization/01_benchmark_methodology.md
docs/vector_native_faiss_optimization/02_smoke_benchmark_report.md
docs/vector_native_faiss_optimization/03_optimization_roadmap.md
```

Script:

```text
scripts/benchmark/gen_vector_benchmark_dataset.py
```

Smoke artifacts:

```text
results/vector_native_faiss/datasets/smoke_N1000_nq100_d64_seed42_ip_norm_top10_scope-empty/dataset.bin
results/vector_native_faiss/datasets/smoke_N1000_nq100_d64_seed42_ip_norm_top10_scope-empty/queries.bin
results/vector_native_faiss/datasets/smoke_N1000_nq100_d64_seed42_ip_norm_top10_scope-empty/manifest.json
results/vector_native_faiss/smoke/native_flat.conf
results/vector_native_faiss/smoke/raw.csv
results/vector_native_faiss/smoke/summary.md
results/vector_native_faiss/smoke/logs/native_flat_server.log
```

## 3. Dataset Manifest Summary

| Field | Value |
|---|---|
| dataset_id | `smoke_N1000_nq100_d64_seed42_ip_norm_top10_scope-empty` |
| N | 1000 |
| nq | 100 |
| dim | 64 |
| seed | 42 |
| metric | ip |
| normalize | true |
| topK | 10 |
| scope | empty |
| dtype | float32_le |
| dataset bytes | 256000 |
| queries bytes | 25600 |

Manifest path:

```text
results/vector_native_faiss/datasets/smoke_N1000_nq100_d64_seed42_ip_norm_top10_scope-empty/manifest.json
```

## 4. Backend Availability

| Backend | Status | Reason |
|---|---|---|
| `native_flat` | PASS | `kvstore` built with `ENABLE_HNSWLIB=0 ENABLE_ONNX_EMBEDDING=0 ENABLE_EBPF=0`; temporary config selected `vector_index_type=flat` |
| `hnswlib_hnsw` / current default HNSW | SKIP | `third_party/hnswlib` checkout is empty in the current workspace, so the HNSWLIB backend cannot be built here |
| `faiss_hnsw` | SKIP | `third_party/faiss` checkout is empty and default `ENABLE_FAISS=0` excludes Faiss |
| `faiss_flat` | SKIP | Same Faiss dependency/build limitation |

## 5. Smoke Results

| Backend | Operation | Requests | Success | Failed | Errors | Result |
|---|---|---:|---:|---:|---:|---|
| `native_flat` | VSET_PREPARE | 1000 | 1000 | 0 | 0 | PASS |
| `native_flat` | VSET | 1 | 1 | 0 | 0 | PASS |
| `native_flat` | VSEARCH | 100 | 100 | 0 | 0 | PASS |

Raw results:

```text
results/vector_native_faiss/smoke/raw.csv
```

Human summary:

```text
results/vector_native_faiss/smoke/summary.md
```

## 6. What This Smoke Cannot Prove

1. It cannot compare native_flat, hnswlib_hnsw, native_hnsw, or Faiss formal performance.
2. It cannot infer 10K or 100K latency, QPS, memory, timeout behavior, or recall.
3. It cannot prove native HNSW is close to or far from Faiss.
4. It cannot compare standalone Faiss API results with KVLite server-path results.
5. It cannot evaluate recall@10; it only validates dataset generation, basic backend selection, VSET/VSEARCH reachability, and output format.

## 7. Next Formal Commands

These commands are templates for the next stage and were not run as formal benchmark in this smoke stage.

```bash
python3 scripts/benchmark/gen_vector_benchmark_dataset.py \
  --count 10000 --query-count 1000 --dim 64 --seed 42 \
  --metric ip --normalize --topk 10

python3 scripts/benchmark/gen_vector_benchmark_dataset.py \
  --count 100000 --query-count 1000 --dim 64 --seed 42 \
  --metric ip --normalize --topk 10
```

Formal execution must use temporary config files to switch backend, for example:

```text
vector_index_type=flat
vector_index_type=hnswlib_hnsw
vector_index_type=faiss_flat
vector_index_type=faiss_hnsw
```

Before formal benchmark, verify hnswlib/Faiss dependencies, index-only runner, server-path runner, profiling split, and recall ground truth.
