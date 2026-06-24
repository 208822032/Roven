# Full E2E P0 Profiling Report

Run id: `full_e2e_p0_profile_20260624T003000Z`

Result directory:

`results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0_profile_20260624T003000Z/`

## 1. Scope

This run keeps the current benchmark scope:

- `hnswlib_hnsw` is the KVLite main vector engine.
- `faiss_hnsw` is the baseline/fallback.
- Only L3 full e2e text+embedding is rerun in this phase.
- L1 index-only results are not mixed into the main L3 tables.
- No HNSW algorithm optimization, batch add/search, SIMD, quantization, or sharding was implemented.

The L3 path is:

```text
client/network
-> RESP
-> text VSET/VSEARCH
-> server hash embedding provider
-> handler/executor
-> vector backend
-> result assembly / metadata lookup
-> RESP response serialization
-> network response
```

## 2. Modified Files

Production profiling changes:

- `include/kvs_vector.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_embedding.c`
- `src/core/kvs_operation.c`
- `src/core/kvs_command_task.c`
- `src/core/kvs_command_executor.c`
- `src/embedding/embedding_runtime.c`

Benchmark/report changes:

- `scripts/benchmark/run_hnswlib_vs_faiss_full_e2e.py`
- `scripts/benchmark/run_hnswlib_vs_faiss_benchmark.py`
- `docs/vector_native_faiss_optimization/11_full_e2e_p0_profile_report.md`
- `docs/vector_native_faiss_optimization/10_full_e2e_text_embedding_benchmark_report.md`
- `docs/vector_native_faiss_optimization/06_p0_optimization_plan.md`

Result files:

- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0_profile_20260624T003000Z/manifest.json`
- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0_profile_20260624T003000Z/backend_config.conf`
- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0_profile_20260624T003000Z/server.log`
- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0_profile_20260624T003000Z/raw.csv`
- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0_profile_20260624T003000Z/summary.md`

Production code was modified, but only for profiling and log export. External protocol semantics and backend behavior were not changed.

## 3. Build And Environment

Build command:

```bash
make clean
make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j"$(nproc)"
```

Build status: passed.

Faiss build state from `manifest.json`:

| Field | Value |
|---|---|
| `libfaiss.a` | present |
| `FAISS_OPT_LEVEL` | `generic` |
| `FAISS_ENABLE_MKL` | `ON` |
| `FAISS_ENABLE_GPU` | `OFF` |

Unified benchmark parameters:

| Parameter | Value |
|---|---|
| dim | 64 |
| embedding provider | `hash` |
| text mode | `semantic` |
| metric | IP |
| topK | 10 |
| scope | empty |
| M | 32 |
| efConstruction | 256 |
| efSearch | 128 |
| query count | 1000 |

## 4. Profiling Fixes

### VSEARCH Index Profiling

The normal RESP `VSEARCH` path now records backend search time inside `kvs_vector_index_search_current()`.

New/filled fields:

- `vsearch_search_count`
- `vsearch_search_total_us`
- `vsearch_search_avg_us`
- `vsearch_search_max_us`

Before this phase, server logs showed `vsearch_search_count=0`. In the final run each backend/size reports `vsearch_search_count=1000`.

### Hash Embedding Profiling

The active hash provider path in `src/vector/kvs_vector_embedding.c` now contributes to embedding runtime stats.

New/filled fields:

- `runtime_encode_count`
- `embedding_encode_avg_us`
- `embedding_encode_max_us`
- `embedding_dim` via provider configuration/logs

Final logs show:

| Dataset | Backend | `runtime_encode_count` | `embedding_encode_avg_us` |
|---:|---|---:|---:|
| 1K | `hnswlib_hnsw` | 2000 | 4 |
| 1K | `faiss_hnsw` | 2000 | 4 |
| 10K | `hnswlib_hnsw` | 11000 | 8 |
| 10K | `faiss_hnsw` | 11000 | 8 |

These values are run-level averages across VSET and VSEARCH commands in the same server process. They are useful for this hash-provider L3 run, but they are not ONNX measurements.

### VSEARCH Response Serialization Profiling

The normal `VSEARCH` response path now records RESP response build time.

New/filled fields:

- `vsearch_response_build_count`
- `vsearch_response_build_total_us`
- `vsearch_response_build_avg_us`
- `vsearch_response_build_max_us`

Final logs show `vsearch_response_build_count=1000` for every backend/size. `network_push_us` remains unavailable; it is not reported as zero in `raw.csv`.

## 5. 1K Smoke Results

| Dataset | Backend | Operation | QPS | Avg us | P95 us | Embedding avg us | Index avg us | Full non-index avg us | Response build avg us | Success | Errors | Status |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 1000 | `hnswlib_hnsw` | VSET | 1415.82 | 710.000 | 930.000 | 4.000 | 630.000 | 80.000 | N/A | 1000 | 0 | pass |
| 1000 | `hnswlib_hnsw` | VSEARCH | 7172.01 | 140.000 | 160.000 | 4.000 | 71.000 | 69.000 | 11.000 | 1000 | 0 | pass |
| 1000 | `faiss_hnsw` | VSET | 6449.82 | 150.000 | 220.000 | 4.000 | 82.000 | 68.000 | N/A | 1000 | 0 | pass |
| 1000 | `faiss_hnsw` | VSEARCH | 6205.63 | 160.000 | 100.000 | 4.000 | 101.000 | 59.000 | 11.000 | 1000 | 0 | pass |

1K gap, `hnswlib_hnsw avg_us / faiss_hnsw avg_us`:

| Operation | End-to-end ratio | Index ratio |
|---|---:|---:|
| VSET | 4.73x | 7.68x |
| VSEARCH | 0.88x | 0.70x |

## 6. 10K Full E2E Results

| Dataset | Backend | Operation | QPS | Avg us | P95 us | Embedding avg us | Index avg us | Full non-index avg us | Response build avg us | Success | Errors | Status |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 10000 | `hnswlib_hnsw` | VSET | 841.80 | 1190.000 | 1390.000 | 8.000 | 1103.000 | 87.000 | N/A | 10000 | 0 | pass |
| 10000 | `hnswlib_hnsw` | VSEARCH | 6315.16 | 160.000 | 180.000 | 8.000 | 86.000 | 74.000 | 11.000 | 1000 | 0 | pass |
| 10000 | `faiss_hnsw` | VSET | 3745.55 | 270.000 | 330.000 | 8.000 | 190.000 | 80.000 | N/A | 10000 | 0 | pass |
| 10000 | `faiss_hnsw` | VSEARCH | 6063.51 | 160.000 | 280.000 | 8.000 | 99.000 | 61.000 | 11.000 | 1000 | 0 | pass |

10K gap, `hnswlib_hnsw avg_us / faiss_hnsw avg_us`:

| Operation | End-to-end ratio | Index ratio |
|---|---:|---:|
| VSET | 4.41x | 5.81x |
| VSEARCH | 1.00x | 0.87x |

## 7. Attribution

The strongest supported bottleneck remains VSET/add.

For 10K VSET:

- hnswlib full e2e avg: 1190us
- Faiss full e2e avg: 270us
- hnswlib backend add avg: 1103us
- Faiss backend add avg: 190us
- full non-index overhead is close: hnswlib 87us vs Faiss 80us

This means the 10K VSET gap is primarily backend add/index cost, not RESP, hash embedding, or response serialization.

For 10K VSEARCH:

- both full e2e avg values are 160us in this run
- hnswlib backend search avg is 86us
- Faiss backend search avg is 99us
- response build avg is 11us for both
- full non-index overhead is 74us for hnswlib and 61us for Faiss

This run does not show a material hnswlib search deficit at 10K. Remaining VSEARCH overhead is outside the backend search call and should be profiled further before optimization.

## 8. Remaining Profiling Limits

Reliable now:

- L3 VSET/VSEARCH full e2e latency.
- Hash embedding encode count/average/max for the whole backend run.
- VSET backend add average.
- VSEARCH backend search count/average/max.
- VSEARCH RESP response build average.
- `raw.csv` leaves unavailable fields empty/N/A rather than using fake zero-cost values.

Still unavailable or incomplete:

- VSET response build profiling on this prepare-only benchmark path.
- Metadata lookup as a standalone field.
- Network push/send as a standalone field.
- Per-operation embedding deltas separated between VSET and VSEARCH.
- ONNX embedding cost.
- Recall@10 for full e2e text mode.

## 9. P0/P1/P2 Update

P0:

1. Quantify and reduce hnswlib VSET backend add cost.
2. Split hnswlib add into ordinary add vs resize/reserve cost.
3. Add backend stats for current capacity, resize count, effective M/efConstruction/efSearch.
4. Keep VSEARCH profiling active and add metadata lookup / network push stages.
5. Preallocate common VSEARCH response buffers only after metadata/network cost is visible.

P1:

1. Add batch VSET and batch add API after single-add bottleneck is quantified.
2. Add batch VSEARCH only after the current single-query path remains stable.
3. Optimize metadata lookup and scope-aware filtering if the next profile shows non-index overhead dominates.
4. Revisit HNSW parameter combinations after add/resize behavior is controlled.

P2:

1. Persist hnswlib graph state.
2. Add index shard and shard executor designs.
3. Add multi-scope partitioned indexes.
4. Keep Faiss as long-term baseline/fallback.

## 10. Next Stage Prompt

```text
在 KVLite 仓库中继续 hnswlib_hnsw P0 优化，不做 HNSW 算法重写。基于
docs/vector_native_faiss_optimization/11_full_e2e_p0_profile_report.md
和 run_id full_e2e_p0_profile_20260624T003000Z，重点处理 VSET/add：
1. 为 hnswlib_hnsw 增加 add 阶段细分 profiling，区分 ordinary add、resize/reserve、label/id 映射、临时分配；
2. 暴露 backend stats：backend name、size、capacity/max_elements、resize_count、M、efConstruction、efSearch；
3. 设计并实现最小 reserve/initial capacity 配置或导入前 reserve 路径；
4. 重跑 L3 full_e2e 1K/10K hnswlib_hnsw vs faiss_hnsw；
5. 保持 L1 与 L3 结果分层，报告 VSET add gap 是否收敛。
```

## P0-B Follow-up

Follow-up report:

`docs/vector_native_faiss_optimization/12_p0b_hnswlib_add_reserve_report.md`

Run id:

`full_e2e_p0b_add_reserve_20260624T012000Z`

P0-B changes:

- Added `vector_hnsw_initial_capacity`.
- Set benchmark initial capacity to dataset size.
- Added hnswlib add split fields: ordinary add, resize/reserve, id-map, vector-copy/prepare, total backend add.
- Exposed backend stats in VINFO/server logs/benchmark output.
- Fixed hnswlib `efSearch` so configured `efSearch=128` is actually applied.

P0-B result:

| Dataset | Operation | hnswlib avg us | Faiss avg us | Ratio | hnswlib index/add avg us | hnswlib resize count |
|---:|---|---:|---:|---:|---:|---:|
| 1K | VSET | 720 | 140 | 5.14x | 649 | 0 |
| 1K | VSEARCH | 460 | 80 | 5.75x | 370 | 0 |
| 10K | VSET | 1230 | 270 | 4.56x | 1151 | 0 |
| 10K | VSEARCH | 670 | 110 | 6.09x | 596 | 0 |

The previous resize chain is removed, but VSET remains dominated by ordinary hnswlib `addPoint()` cost. VSEARCH is now measured with `efSearch=128`; it should not be compared directly with the previous run where hnswlib effectively used `topK=10`.
