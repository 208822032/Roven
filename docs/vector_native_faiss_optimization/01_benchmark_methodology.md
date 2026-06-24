# Benchmark Methodology

This document is the single benchmark design reference for native vs Faiss work. It replaces the previous split dataset, index-only, and server-path design notes.

## 1. Dataset And Manifest

All benchmark layers must use the same generated dataset files. Do not regenerate different data for native and Faiss.

Smoke dataset:

| Field | Value |
|---|---:|
| N | 1000 |
| nq | 100 |
| dim | 64 |
| seed | 42 |
| metric | ip |
| normalize | true |
| topK | 10 |
| scope | empty |
| dtype | float32 little-endian |

Dataset location:

```text
results/vector_native_faiss/datasets/<dataset_id>/
```

Required files:

```text
dataset.bin
queries.bin
manifest.json
```

`dataset.bin` stores `N * dim` float32 values in row-major order. `queries.bin` stores `nq * dim` float32 values in row-major order. When `normalize=true`, every row is L2-normalized during generation.

`manifest.json` must include:

```text
dataset_id,seed,N,nq,dim,metric,normalize,topK,scope,dtype,
dataset_path,queries_path,dataset_bytes,queries_bytes,created_at_utc,generator
```

Reproducibility rules:

1. Use the same `dataset.bin`, `queries.bin`, and manifest for every backend in the same run.
2. Preserve write order across backends.
3. Do not compare runs with different seed, dim, metric, normalize policy, topK, scope, or HNSW parameters.
4. Record the dataset manifest path in every benchmark run manifest.

## 2. Index-only Benchmark

Purpose: isolate backend index implementation cost and recall. This layer must not use RESP, the KVLite server process, network I/O, executor queues, metadata lookup, or response serialization.

Initial backend candidates:

| Backend | Notes |
|---|---|
| `native_flat` | KVLite exact flat baseline |
| `hnswlib_hnsw` | Current non-Faiss HNSW path when hnswlib is available |
| `native_hnsw` | Include only if legacy native HNSW is restored as a selectable backend |
| `faiss_flat` | Faiss exact baseline |
| `faiss_hnsw` | Faiss ANN baseline |

All backends must use the same dataset, query set, metric, normalize policy, topK, write order, and HNSW `M`, `efConstruction`, `efSearch`.

`raw.csv` fields:

```text
run_id,dataset_id,layer,backend,phase,N,nq,dim,metric,normalize,topK,M,efConstruction,efSearch,count,total_us,avg_us,p50_us,p95_us,p99_us,max_us,qps,recall_at_1,recall_at_10,rss_kb,index_memory_bytes,timeout,error_count,notes
```

`summary.md` should report backend availability, add latency/QPS, search latency/QPS, recall@1, recall@10, memory usage, failures, timeouts, and skipped cases.

Run `manifest.json` should record git commit, build flags, CPU model if available, dataset manifest path, backend parameters, command used, timestamps, and exit status.

Recall must use exact flat search as ground truth. Faiss FlatIP can be used for ground truth only when it uses the same input vectors and metric; the selected source must be recorded.

Standalone Faiss API results belong only in this layer. They must not be compared with KVLite server-path latency or QPS.

## 3. Server-path Benchmark

Purpose: measure the real user path through KVLite VSET and VSEARCH while changing only the bottom index backend.

Required path:

```text
client RESP
  -> parser
  -> command handler / command executor
  -> vector metadata and current index abstraction
  -> response serialization
  -> network send
```

Backend selection must use temporary config files. Do not depend on the current `--vector_index_type` CLI parser for Faiss until it accepts `faiss_hnsw` and `faiss_flat` reliably.

Every server-path case must record backend config, dataset manifest, VINFO output, build flags, server log, client command, timeout settings, and exit status.

For smoke, deterministic text keys generated from manifest parameters are acceptable. Formal runs must preserve a one-to-one mapping between dataset rows, query rows, and server commands.

`raw.csv` fields:

```text
run_id,dataset_id,layer,backend,operation,N,nq,dim,metric,normalize,topK,M,efConstruction,efSearch,requests,clients,pipeline,elapsed_ms,qps,avg_ms,p50_ms,p95_ms,p99_ms,max_ms,success,failed,errors,timeout,index_add_avg_us,index_search_avg_us,non_index_avg_ms,notes
```

`summary.md` should include backend availability, VSET result, VSEARCH result, skipped or failed backend reasons, and why smoke results are not formal performance evidence.

Run `manifest.json` should include dataset manifest path, temporary config path, server binary path, client binary path, backend parameters, run timestamps, and exit status for server and client commands.

Do not compare standalone Faiss API latency with server-path VSEARCH latency. Server-path results include parser, executor, metadata, response serialization, and network overhead.

## 4. Profiling Points

Formal server-path profiling should at least separate:

1. VSET total
2. VSET index add
3. VSEARCH total
4. VSEARCH index search
5. metadata lookup
6. response serialization
7. queue wait, if the active executor path has a queue
8. malloc/free count, if practical

At minimum, reports must separate end-to-end latency, index add/search latency, and non-index overhead.

## 5. Minimum Formal Matrix

Do not expand the matrix until the 1K/64d smoke passes for the intended backends.

| Case | Dataset | Dim | Backend | Operation |
|---|---:|---:|---|---|
| C1 | 10K | 64 | `native_hnsw` or `hnswlib_hnsw` | VSET / VSEARCH |
| C2 | 10K | 64 | `faiss_hnsw` | VSET / VSEARCH |
| C3 | 10K | 64 | `native_flat` | VSET / VSEARCH |
| C4 | 10K | 64 | `faiss_flat` | VSET / VSEARCH |
| C5 | 100K | 64 | `native_hnsw` or `hnswlib_hnsw` | VSET / VSEARCH |
| C6 | 100K | 64 | `faiss_hnsw` | VSET / VSEARCH |

Optional, only after the 64d matrix is stable:

| Case | Dataset | Dim | Backend | Operation |
|---|---:|---:|---|---|
| C7 | 10K | 512 | `native_hnsw` or `hnswlib_hnsw` | VSET / VSEARCH |
| C8 | 10K | 512 | `faiss_hnsw` | VSET / VSEARCH |

## 6. Formal Run Entry Points

Generate 10K/64d data:

```bash
python3 scripts/benchmark/gen_vector_benchmark_dataset.py \
  --count 10000 --query-count 1000 --dim 64 --seed 42 \
  --metric ip --normalize --topk 10
```

Generate 100K/64d data:

```bash
python3 scripts/benchmark/gen_vector_benchmark_dataset.py \
  --count 100000 --query-count 1000 --dim 64 --seed 42 \
  --metric ip --normalize --topk 10
```

Formal benchmark execution should run in this order:

1. Generate and verify fixed datasets.
2. Run index-only matrix and compute recall.
3. Run server-path matrix with temporary backend configs.
4. Produce a formal report with raw data, summaries, backend availability, profiling splits, and skipped-case reasons.
5. Open P0/P1/P2 optimization work only after the formal report exists.
