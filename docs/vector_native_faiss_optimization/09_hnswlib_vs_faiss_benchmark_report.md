# hnswlib vs Faiss Benchmark Report

Run id: `hnswlib_faiss_20260624T000000Z`

## 1. Current Scope Adjustment

`native_flat` and legacy `native_hnsw` are no longer mainline comparison targets. KVLite's forward vector engine is `hnswlib_hnsw`. Faiss is kept as a reproducible performance baseline and fallback/control group, not as the product main path.

`faiss_flat` is included only as exact search / recall ground truth. The main performance comparison is `hnswlib_hnsw` vs `faiss_hnsw`.

## 2. Test Goal

Measure whether the vendored hnswlib path is correctly usable and how close it is to Faiss under the same 10K/64d and 100K/64d benchmark envelope.

The report separates:

- index-only benchmark: direct index add/search without KVLite RESP/server path.
- server-path benchmark: real `kvstore`, real RESP `VSET`/`VSEARCH`, backend switched by temporary config files.

## 3. Test Environment

- Date: 2026-06-24
- Host: recorded in each `manifest.json`
- Build command: `make clean && make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j$(nproc)`
- Build result: pass
- Result roots:
  - `results/vector_native_faiss/hnswlib_vs_faiss/index_only/hnswlib_faiss_20260624T000000Z/`
  - `results/vector_native_faiss/hnswlib_vs_faiss/server_path/hnswlib_faiss_20260624T000000Z/`

## 4. Faiss / hnswlib Build State

| Backend | Status | Evidence |
|---|---|---|
| `hnswlib_hnsw` | available | vendored `third_party/hnswlib/hnswlib/hnswlib.h`; default build with `ENABLE_HNSWLIB=1` passed |
| `faiss_hnsw` | available | `third_party/faiss/build/faiss/libfaiss.a` exists; build linked BLAS/LAPACK |
| `faiss_flat` | available | same Faiss static library; used as exact baseline |

Faiss build parameters:

- `FAISS_OPT_LEVEL=generic`
- `FAISS_ENABLE_MKL=ON`
- `FAISS_ENABLE_GPU=OFF`

For extreme-performance Faiss comparison, run a separate AVX2 Faiss build. This report uses the current generic Faiss build.

## 5. Dataset And Unified Parameters

Datasets:

- `smoke_N10000_nq1000_d64_seed42_ip_norm_top10_scope-empty`
- `smoke_N100000_nq1000_d64_seed42_ip_norm_top10_scope-empty`

Unified parameters:

| Parameter | Value |
|---|---:|
| seed | 42 |
| dim | 64 |
| metric | ip |
| normalize | true |
| topK | 10 |
| scope | empty |
| M | 32 |
| efConstruction | 256 |
| efSearch | 128 |

## 6. Index-only Results

| Dataset | Backend | Add avg us | Search avg us | Search p95 us | Recall@10 | Status |
|---:|---|---:|---:|---:|---:|---|
| 10K | `hnswlib_hnsw` | 185 | 113 | 119 | 0.994100 | pass |
| 10K | `faiss_hnsw` | 239 | 142 | 148 | 0.996700 | pass |
| 10K | `faiss_flat` | 0 | 180 | 187 | 1.000000 | pass |
| 100K | `hnswlib_hnsw` | 465 | 342 | 372 | 0.899700 | pass |
| 100K | `faiss_hnsw` | 779 | 315 | 353 | 0.927300 | pass |
| 100K | `faiss_flat` | 0 | 1377 | 1956 | 1.000000 | pass |

Index-only interpretation:

- At 10K, hnswlib is faster than Faiss HNSW for both add and search, with slightly lower recall.
- At 100K, hnswlib add remains faster, but search becomes 1.09x slower than Faiss HNSW and recall is lower by 2.76 percentage points.
- The 100K recall drop is real relative to exact `faiss_flat` ground truth and should be addressed by confirming `efSearch` effectiveness and tuning HNSW parameters.

## 7. Server-path Results

Server-path uses real `kvstore` and real RESP `VSET`/`VSEARCH`. Backends were selected through generated `backend_config.conf` files. Logs confirm the server initialized `index_type=hnswlib_hnsw`, `index_type=faiss_hnsw`, and `index_type=faiss_flat` in their respective runs.

| Dataset | Backend | VSET avg us | VSET index avg us | VSEARCH avg us | VSEARCH p95 us | Status |
|---:|---|---:|---:|---:|---:|---|
| 10K | `hnswlib_hnsw` | 1190 | 1115 | 150 | 180 | pass |
| 10K | `faiss_hnsw` | 260 | 187 | 110 | 110 | pass |
| 10K | `faiss_flat` | 60 | 0 | 300 | 590 | pass |
| 100K | `hnswlib_hnsw` | 1190 | 1109 | 170 | 200 | pass |
| 100K | `faiss_hnsw` | 530 | 456 | 150 | 140 | pass |
| 100K | `faiss_flat` | 60 | 0 | 2050 | 3020 | pass |

Important profiling note: current normal RESP `VSEARCH` logs report `vsearch_search_count=0`, so `VSEARCH index_avg_us` is unavailable. The raw CSV intentionally leaves it empty instead of treating `0` as a real index cost.

## 8. hnswlib vs faiss_hnsw Gap Multiples

Ratio is `hnswlib_hnsw avg_us / faiss_hnsw avg_us`; values above 1.0 mean hnswlib is slower.

| Layer | Dataset | Operation | hnswlib avg us | Faiss HNSW avg us | Ratio |
|---|---:|---|---:|---:|---:|
| index-only | 10K | add | 185 | 239 | 0.77x |
| index-only | 10K | search | 113 | 142 | 0.80x |
| index-only | 100K | add | 465 | 779 | 0.60x |
| index-only | 100K | search | 342 | 315 | 1.09x |
| server-path | 10K | VSET | 1190 | 260 | 4.58x |
| server-path | 10K | VSEARCH | 150 | 110 | 1.36x |
| server-path | 100K | VSET | 1190 | 530 | 2.25x |
| server-path | 100K | VSEARCH | 170 | 150 | 1.13x |

## 9. Recall@10 Comparison

| Dataset | hnswlib_hnsw | faiss_hnsw | faiss_flat |
|---:|---:|---:|---:|
| 10K | 0.994100 | 0.996700 | 1.000000 |
| 100K | 0.899700 | 0.927300 | 1.000000 |

The 10K recall is acceptable for the current parameter set. The 100K recall is lower than Faiss HNSW and must be treated as a tuning/correctness target before claiming parity.

## 10. VSET/add Bottleneck Analysis

The primary bottleneck is server-path hnswlib VSET/add, not standalone hnswlib add:

- 100K index-only hnswlib add avg: 465us.
- 100K server-path hnswlib VSET avg: 1190us.
- 100K server-path hnswlib index add profile: 1109us.
- 100K server-path Faiss HNSW VSET avg: 530us.

The server log shows hnswlib initializes with `max_elements=16` and resizes through `1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072`. That strongly points to capacity/reserve behavior and wrapper add path overhead as P0 targets. The gap is not explained by RESP serialization alone because the recorded hnswlib index-add profile nearly equals end-to-end VSET latency.

## 11. VSEARCH/search Bottleneck Analysis

VSEARCH gap is modest in this run:

- 10K: hnswlib 150us vs Faiss HNSW 110us, 1.36x slower.
- 100K: hnswlib 170us vs Faiss HNSW 150us, 1.13x slower.

However, index/non-index attribution is currently incomplete because `vsearch_search_count=0` in the normal server log. P0 must fix this before deciding whether the remaining VSEARCH delta is index search, metadata lookup, topK export, response serialization, or client/server overhead.

## 12. Server-path Extra Overhead Analysis

Index-only and server-path are not directly comparable because the server path uses RESP, key/value metadata, hash embedding, executor path, and response generation.

Useful signals:

- hnswlib 100K search is faster in server-path avg (170us) than index-only avg (342us), so these two runs are not measuring identical query vectors or data ingestion methods. Do not mix them as a single latency comparison.
- hnswlib VSET server-path is much slower than standalone add, and VSET profile points at hnswlib add/capacity behavior.
- Faiss flat server-path 100K VSEARCH avg is 2050us vs index-only 1377us, showing the wrapper/server path adds visible overhead for exact search.

## 13. hnswlib Main-path Optimization Judgment

hnswlib is viable as KVLite's main vector engine, but the next work should not start with algorithm replacement. The measured bottlenecks are:

1. P0: hnswlib server VSET add path, especially capacity/reserve/resize and per-add overhead.
2. P0: missing VSEARCH index/non-index profiling.
3. P0/P1: 100K recall gap under `efSearch=128`.
4. P1: batch add/search and metadata/result assembly.

Faiss should remain in CI/benchmark as the baseline and fallback. It is not the product main path in this plan.

## 14. P0/P1/P2 Update

P0:

- Confirm `efSearch` is actually applied in hnswlib.
- Add reliable VSEARCH index/non-index profiling.
- Split VSET add timing into ordinary add vs resize/reserve.
- Fix hnswlib `max_elements` / reserve behavior.
- Reduce add/search temporary allocation.
- Preallocate response buffers.
- Output stable VINFO/backend stats.

P1:

- Batch VSET / batch add.
- Batch VSEARCH.
- Metadata lookup optimization.
- Scope-aware search.
- HNSW parameter tuning.
- 512d SIMD / memory-layout analysis.

P2:

- hnswlib graph persistence.
- Index shard.
- Shard executor.
- Multi-scope partitioned index.
- SQ8/PQ quantization.
- Keep Faiss as long-term baseline/fallback.

## 15. Next-stage Recommendation

Next prompt:

```text
在 KVLite 仓库中继续 hnswlib 主路径 P0 优化。基于
docs/vector_native_faiss_optimization/09_hnswlib_vs_faiss_benchmark_report.md
和 run_id hnswlib_faiss_20260624T000000Z，先不改算法，完成：
1. 修复/补齐 VSEARCH index/non-index profiling，确保 server-path raw.csv 能区分 index search、metadata lookup、response serialization；
2. 为 hnswlib_hnsw 增加可配置或导入期 reserve/initial capacity，避免 16 -> ... -> 131072 的 resize 链；
3. 输出 VINFO/backend stats：backend、M、efConstruction、efSearch、capacity、resize_count；
4. 重跑 10K/100K hnswlib vs faiss_hnsw benchmark，并更新 09 报告或新增 P0 rerun 报告。
```
