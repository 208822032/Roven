# P0-C hnswlib AddPoint and efSearch Report

Run id: `full_e2e_p0c_add_efsearch_20260624T023000Z`

Result directory:

`results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0c_add_efsearch_20260624T023000Z/`

## 1. Scope

This phase keeps the current product direction:

- `hnswlib_hnsw` is the KVLite primary vector engine.
- `faiss_hnsw` is baseline/fallback only.
- No legacy `native_hnsw` or `native_flat` mainline comparison is reintroduced.
- No HNSW algorithm rewrite, batch add/search, SIMD, quantization, sharding, or RESP protocol change was implemented.
- L3 full e2e latency and L1 index-only recall are reported separately.

## 2. Modified Files

Production code:

- `include/kvs_vector.h`
- `src/core/kvs_command_executor.c`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_hnswlib_ops.c`

Benchmark/report/data files:

- `scripts/benchmark/run_hnswlib_vs_faiss_full_e2e.py`
- `docs/vector_native_faiss_optimization/13_p0c_hnswlib_add_efsearch_report.md`
- `docs/vector_native_faiss_optimization/06_p0_optimization_plan.md`
- `docs/vector_native_faiss_optimization/12_p0b_hnswlib_add_reserve_report.md`
- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0c_add_efsearch_20260624T023000Z/`
- `results/vector_native_faiss/datasets/smoke_N1000_nq1000_d64_seed42_ip_norm_top10_scope-empty/`

Production code was modified only for profiling counters and hnswlib wrapper timing. External protocol semantics are unchanged.

## 3. Build

Clean build command recorded in the final run manifest:

```bash
make clean
make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j"$(nproc)"
```

Build status: passed.

Post-run incremental build after a resize-timing boundary cleanup also passed. That cleanup only changes profiling attribution when fallback resize happens; the formal run has `resize_count=0`, so the measured rows are unaffected.

Faiss build status:

- `third_party/faiss/build/faiss/libfaiss.a`: exists
- `FAISS_OPT_LEVEL=generic`
- `FAISS_ENABLE_MKL=ON`
- `FAISS_ENABLE_GPU=OFF`

## 4. Initial Capacity and Resize

`vector_hnsw_initial_capacity` remains active in the L3 runner.

| Dataset | Initial capacity | Final capacity | Resize count |
|---:|---:|---:|---:|
| 1K | 1000 | 1000 | 0 |
| 10K | 10000 | 10000 | 0 |

The old `max_elements=16 -> ...` resize chain did not reappear.

## 5. AddPoint Profiling Split

New hnswlib wrapper fields:

| Field | Meaning |
|---|---|
| `before_add_prepare_us` | Wrapper-side handle lookup, current size/capacity read, and capacity check before add. |
| `hnswlib_add_point_us` | Time spent in `kvs_hnswlib_index_add()`, which wraps hnswlib `addPoint()`. |
| `after_add_bookkeeping_us` | Wrapper-side post-add bookkeeping placeholder. |
| `stats_update_us` | Cost of updating KVLite profiling counters and hnswlib add count. |
| `resize_or_reserve_us` | Fallback resize cost, empty when no resize occurs. |

The hnswlib internal phases such as neighbor search, level selection, and link updates were not modified. For this phase, ordinary `addPoint()` is a black-box hnswlib internal cost.

## 6. L3 VSET Results

L3 path: TCP/RESP/text `VSET`/hash embedding/executor/backend/response.

| Dataset | efSearch | hnsw avg/index/addPoint/prepare/copy us | Faiss avg/index us | Avg ratio | Index ratio |
|---:|---:|---:|---:|---:|---:|
| 1000 | 32 | 710 / 634 / 633 / 0 / 1 | 150 / 86 | 4.73x | 7.37x |
| 1000 | 64 | 710 / 633 / 632 / 0 / 1 | 150 / 85 | 4.73x | 7.45x |
| 1000 | 128 | 720 / 640 / 640 / 0 / 1 | 150 / 85 | 4.80x | 7.53x |
| 10000 | 32 | 1270 / 1186 / 1186 / 0 / 1 | 270 / 198 | 4.70x | 5.99x |
| 10000 | 64 | 1260 / 1182 / 1181 / 0 / 1 | 270 / 196 | 4.67x | 6.03x |
| 10000 | 128 | 1260 / 1180 / 1179 / 0 / 1 | 270 / 197 | 4.67x | 5.99x |

VSET conclusion:

- hnswlib VSET remains about `4.7x` slower end-to-end than Faiss at 1K/10K.
- The index-add gap is about `6.0x` at 10K.
- `before_add_prepare_us`, wrapper bookkeeping, stats update, and vector copy are near zero or `1us`.
- The remaining VSET gap is inside hnswlib `addPoint()`, not KVLite wrapper overhead or resize behavior.

## 7. L3 VSEARCH efSearch Tradeoff

L3 path: TCP/RESP/text `VSEARCH`/hash embedding/executor/backend/metadata/response.

| Dataset | efSearch | hnsw avg/p95/p99/index/non-index/resp us | Faiss avg/p95/p99/index/non-index/resp us | Avg ratio |
|---:|---:|---:|---:|---:|
| 1000 | 32 | 210 / 230 / 250 / 138 / 72 / 12 | 80 / 100 / 250 / 15 / 65 / 11 | 2.62x |
| 1000 | 64 | 290 / 320 / 360 / 223 / 67 / 12 | 150 / 150 / 330 / 87 / 63 / 11 | 1.93x |
| 1000 | 128 | 420 / 450 / 510 / 352 / 68 / 12 | 80 / 90 / 220 / 14 / 66 / 11 | 5.25x |
| 10000 | 32 | 260 / 310 / 340 / 193 / 67 / 12 | 130 / 120 / 430 / 55 / 75 / 17 | 2.00x |
| 10000 | 64 | 430 / 460 / 520 / 354 / 76 / 12 | 110 / 90 / 260 / 45 / 65 / 11 | 3.91x |
| 10000 | 128 | 670 / 730 / 780 / 600 / 70 / 13 | 110 / 130 / 220 / 32 / 78 / 19 | 6.09x |

VSEARCH conclusion:

- hnswlib L3 search latency increases monotonically with efSearch.
- hnswlib non-index overhead is stable around `67-76us`; response build is stable around `12-13us`.
- The efSearch latency change is primarily index search cost.
- Faiss L3 rows are noisier and not monotonic in the text/server path; the L1 recall table below is the better recall/efSearch signal.

## 8. L1 Recall Reference

L3 text+hash path currently does not compute recall@10 because it does not persist a flat ground-truth mapping for the generated semantic text queries. To avoid faking recall, recall is reported separately using the L1 index-only tool on binary float datasets with exact flat ground truth.

L1 result file:

`results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0c_add_efsearch_20260624T023000Z/l1_recall_efsearch.csv`

| Dataset | efSearch | hnsw recall/avg/p95/p99 us | Faiss recall/avg/p95/p99 us |
|---:|---:|---:|---:|
| 1000 | 32 | 0.970700 / 19 / 22 / 25 | 0.972100 / 20 / 22 / 26 |
| 1000 | 64 | 0.998200 / 33 / 36 / 38 | 0.998200 / 36 / 38 / 44 |
| 1000 | 128 | 1.000000 / 54 / 57 / 65 | 1.000000 / 62 / 66 / 76 |
| 10000 | 32 | 0.824800 / 32 / 37 / 40 | 0.850400 / 38 / 43 / 47 |
| 10000 | 64 | 0.949400 / 61 / 65 / 69 | 0.962900 / 72 / 77 / 82 |
| 10000 | 128 | 0.994100 / 115 / 122 / 134 | 0.996700 / 144 / 152 / 158 |

## 9. AddPoint Slow Cause Check

| Possible cause | Finding |
|---|---|
| Repeated vector copy | Not material. `vector_copy_or_prepare_us` is `0-1us`. |
| Per-add normalize | Not visible in add path. Hash embedding/normalization cost is profiled separately at about `5-11us`. |
| Label/id mapping | Not material in this run; id-map fields are empty or near zero. |
| Wrapper lock/repeated lookup | Not supported by current measurements. Wrapper prepare/bookkeeping/stats are near zero. |
| hnswlib/Faiss param mismatch | Backend stats show both paths using `M=32`, `efConstruction=256`, and configured efSearch. |
| Resize/reserve | Removed. `resize_count=0` for 1K and 10K. |
| `efConstruction=256` add cost | Likely relevant, but both paths are configured with the same value. The measured gap is therefore implementation/backend behavior, not a known config mismatch. |

Attribution: the current VSET gap comes from hnswlib `addPoint()` internals, not from KVLite wrapper overhead or parameter mismatch.

## 10. Recommended efSearch

Recommended default remains `efSearch=128` when recall quality is the primary requirement:

- 10K L1 hnswlib recall@10 is `0.994100` at efSearch 128.
- efSearch 64 is much faster in L3 but only reaches `0.949400` recall@10 at 10K.
- efSearch 32 is appropriate only for latency-oriented smoke/low-recall profiles at 10K (`0.824800` recall@10).

Operational recommendation:

- Default/quality profile: `efSearch=128`.
- Latency profile: `efSearch=64`, only if recall loss is acceptable for the workload.
- Smoke/diagnostic profile: `efSearch=32`.

## 11. Did the Gap Shrink?

No. P0-C improves attribution and exposes the efSearch tradeoff, but it does not implement a performance optimization.

At 10K:

- VSET remains `4.67-4.70x` slower than Faiss end-to-end.
- hnswlib addPoint/index add remains about `6.0x` slower than Faiss add.
- VSEARCH at efSearch 128 is `6.09x` slower end-to-end in L3, mainly due to index search cost.

## 12. Next Optimization Path

P0 next steps:

1. Add source-level profiling inside vendored hnswlib behind a compile-time flag, limited to `addPoint()` phases: random level selection, greedy search/update probability, neighbor selection, link updates, memory writes.
2. Add an efConstruction sweep for add cost and recall: `64/128/256`, keeping L3 and L1 separated.
3. Add a minimal hnswlib add allocator/copy audit: visited-list pool, link-list allocations, label lookup, and vector storage copy.
4. Keep `vector_hnsw_initial_capacity` mandatory for bulk benchmarks and future 100K runs.

P1 candidates:

1. Batch VSET/add API and executor path.
2. Batch VSEARCH for amortized server overhead.
3. Metadata lookup and response buffer tuning after index cost is controlled.
4. 512d-specific SIMD/memory-layout analysis.

P2 candidates:

1. hnswlib graph persistence.
2. Index shard and shard executor.
3. Multi-scope partitioned indexes.
4. Faiss retained as long-term baseline/fallback.

## 13. Next Stage Prompt

```text
你现在在 KVLite 仓库中工作。

当前主路径仍为 hnswlib_hnsw，Faiss 只作为 baseline/fallback。
基于 docs/vector_native_faiss_optimization/13_p0c_hnswlib_add_efsearch_report.md
和 run_id full_e2e_p0c_add_efsearch_20260624T023000Z，进入下一阶段 P0-D。

目标：
1. 不做 HNSW 算法重写；
2. 在可开关的 profiling 宏下进入 vendored hnswlib addPoint 源码级 profiling；
3. 拆分 random level、search/update candidates、neighbor selection、link update、memory allocation/copy；
4. 同时做 efConstruction=64/128/256 的 L3 VSET add latency 与 L1 recall 对照；
5. 保持 L1 index-only、L3 full e2e 分层，不混表比较；
6. 输出 P0-D 报告，判断是否优先调低 efConstruction、做 batch add，还是继续 hnswlib 内部内存布局优化。
```
