# P0-B hnswlib Add Reserve Report

Run id: `full_e2e_p0b_add_reserve_20260624T012000Z`

Result directory:

`results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0b_add_reserve_20260624T012000Z/`

## 1. Scope

This phase only changes P0-B profiling and capacity behavior for the L3 full e2e path:

- `hnswlib_hnsw` remains the KVLite main vector engine.
- `faiss_hnsw` remains baseline/fallback.
- No HNSW algorithm rewrite, batch add/search, SIMD, quantization, or sharding was implemented.
- L1 index-only results are not mixed with these L3 full e2e results.

## 2. Modified Files

Production code:

- `include/kvs_config.h`
- `include/kvs_vector.h`
- `include/kvs_vector_hnswlib_backend.h`
- `src/core/kvs_config.c`
- `src/core/kvs_operation.c`
- `src/core/kvs_command_executor.c`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_hnswlib_ops.c`
- `src/vector/kvs_vector_hnswlib_backend.cpp`

Benchmark/report files:

- `scripts/benchmark/run_hnswlib_vs_faiss_full_e2e.py`
- `docs/vector_native_faiss_optimization/12_p0b_hnswlib_add_reserve_report.md`
- `docs/vector_native_faiss_optimization/06_p0_optimization_plan.md`
- `docs/vector_native_faiss_optimization/11_full_e2e_p0_profile_report.md`

Production code was modified. The changes are limited to profiling, backend stats, config plumbing, initial capacity, and making the existing `efSearch` config take effect for hnswlib search. External RESP protocol semantics were not changed.

## 3. Build

Build command:

```bash
make clean
make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j"$(nproc)"
```

Build status: passed.

The final benchmark was run after an incremental rebuild that included the `efSearch` fix.

## 4. hnswlib Add Profiling Split

New hnswlib add fields:

| Field | Meaning |
|---|---|
| `ordinary_add_us` | Time inside hnswlib `addPoint()` wrapper, excluding dynamic resize. |
| `resize_or_reserve_us` | Dynamic `resizeIndex()` time when fallback resize occurs. Empty when no resize happens. |
| `label_id_mapping_us` | KVLite id-map update time from existing VSET commit profiling. |
| `vector_copy_or_prepare_us` | Prepared vector/string ownership transfer time from existing VSET commit profiling. |
| `temporary_allocation_us` | Not available yet; kept empty rather than fake 0. |
| `total_backend_add_us` | Total backend add timing around `kvs_vector_index_add_current()`. |

Faiss is not internally split; its `total_backend_add_us` remains the baseline total add cost.

## 5. Initial Capacity / Reserve Strategy

New config:

```text
vector_hnsw_initial_capacity
```

Behavior:

- Default is `0`, preserving the previous ordinary startup behavior.
- For `hnswlib_hnsw`, initial `max_elements` is `max(store.capacity, vector_hnsw_initial_capacity)`.
- Dynamic resize remains as fallback if the index grows beyond initial capacity.
- The full e2e runner sets `vector_hnsw_initial_capacity=dataset_size`.

Benchmark settings:

| Dataset | Configured initial capacity |
|---:|---:|
| 1K | 1000 |
| 10K | 10000 |

## 6. Backend Stats

Backend stats are exposed in server logs, VINFO snapshots, and benchmark `backend_stats`.

Example hnswlib 10K:

```text
backend_name=hnswlib_hnsw size=10000 capacity=10000 max_elements=10000
resize_count=0 last_resize_from=0 last_resize_to=0
M=32 efConstruction=256 efSearch=128 metric=ip normalize=hash_provider_normalized
```

Example Faiss 10K:

```text
backend_name=faiss_hnsw size=10000
M=32 efConstruction=256 efSearch=128 metric=ip normalize=hash_provider_normalized
faiss_ntotal=10000
```

`normalize=hash_provider_normalized` reflects the current hash provider path. It is not an ONNX-provider measurement.

## 7. Resize Count

Previous P0 profile run `full_e2e_p0_profile_20260624T003000Z`:

| Dataset | hnswlib creation/resizes |
|---:|---|
| 1K | created `max_elements=16`, resized to `1024` |
| 10K | created `max_elements=16`, resized to `1024`, `2048`, `4096`, `8192`, `16384` |

Final P0-B run:

| Dataset | Initial capacity | Final capacity | Resize count |
|---:|---:|---:|---:|
| 1K | 1000 | 1000 | 0 |
| 10K | 10000 | 10000 | 0 |

The resize chain is removed for the benchmark path.

## 8. 1K L3 Full E2E Results

| Dataset | Backend | Operation | Avg us | P95 us | Embedding avg us | Index avg us | Ordinary add us | Resize/reserve us | ID map us | Vector copy us | Response avg us | Status |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 1000 | `hnswlib_hnsw` | VSET | 720 | 980 | 5 | 649 | 649 | N/A | 0 | 1 | N/A | pass |
| 1000 | `hnswlib_hnsw` | VSEARCH | 460 | 490 | 5 | 370 | N/A | N/A | N/A | N/A | 12 | pass |
| 1000 | `faiss_hnsw` | VSET | 140 | 200 | 4 | 85 | N/A | N/A | 0 | 1 | N/A | pass |
| 1000 | `faiss_hnsw` | VSEARCH | 80 | 100 | 4 | 14 | N/A | N/A | N/A | N/A | 11 | pass |

1K ratios:

| Operation | Full e2e ratio | Index ratio |
|---|---:|---:|
| VSET | 5.14x | 7.64x |
| VSEARCH | 5.75x | 26.43x |

## 9. 10K L3 Full E2E Results

| Dataset | Backend | Operation | Avg us | P95 us | Embedding avg us | Index avg us | Ordinary add us | Resize/reserve us | ID map us | Vector copy us | Response avg us | Status |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 10000 | `hnswlib_hnsw` | VSET | 1230 | 1460 | 9 | 1151 | 1151 | N/A | 0 | 1 | N/A | pass |
| 10000 | `hnswlib_hnsw` | VSEARCH | 670 | 720 | 9 | 596 | N/A | N/A | N/A | N/A | 12 | pass |
| 10000 | `faiss_hnsw` | VSET | 270 | 340 | 8 | 196 | N/A | N/A | 0 | 0 | N/A | pass |
| 10000 | `faiss_hnsw` | VSEARCH | 110 | 140 | 8 | 49 | N/A | N/A | N/A | N/A | 11 | pass |

10K ratios:

| Operation | Full e2e ratio | Index ratio |
|---|---:|---:|
| VSET | 4.56x | 5.87x |
| VSEARCH | 6.09x | 12.16x |

## 10. Did VSET Gap Converge?

No material convergence yet.

Compared with the previous 10K P0 profile run:

| Metric | Previous | P0-B |
|---|---:|---:|
| hnswlib VSET avg | 1190us | 1230us |
| Faiss VSET avg | 270us | 270us |
| VSET ratio | 4.41x | 4.56x |
| hnswlib add/index avg | 1103us | 1151us |
| hnswlib resize count | 5 resizes in 10K run | 0 |

Reserve fixed capacity churn, but the dominant VSET cost is still `ordinary_add_us` inside hnswlib `addPoint()`.

## 11. VSEARCH Stability

VSEARCH is not directly comparable to the previous P0 profile run because P0-B fixed hnswlib `efSearch` handling.

Previous hnswlib server logs effectively used `efSearch=10` because the hnswlib ops layer set `ef` from `topK`. P0-B now uses configured `efSearch=128`, matching the benchmark manifest and Faiss config. This makes the comparison fairer, but increases hnswlib VSEARCH latency:

| Dataset | hnswlib VSEARCH avg | Faiss VSEARCH avg | Ratio |
|---:|---:|---:|---:|
| 1K | 460us | 80us | 5.75x |
| 10K | 670us | 110us | 6.09x |

The VSEARCH regression is therefore a parameter-correctness exposure, not an add/reserve side effect.

## 12. Remaining Bottlenecks

Current supported attribution:

1. hnswlib VSET is dominated by ordinary `addPoint()` cost.
2. hnswlib resize churn is fixed for the benchmark path, but eliminating resize does not close the VSET gap.
3. KVLite-side id-map and vector-copy costs are near 0-1us and are not the primary VSET bottleneck.
4. With `efSearch=128` truly applied, hnswlib VSEARCH is slower than Faiss HNSW in this L3 run.
5. `temporary_allocation_us`, metadata lookup, and network push are still not independently profiled.

## 13. Next Stage Prompt

```text
在 KVLite 仓库中继续 P0-C，不做 HNSW 算法重写。基于
docs/vector_native_faiss_optimization/12_p0b_hnswlib_add_reserve_report.md
和 run_id full_e2e_p0b_add_reserve_20260624T012000Z：
1. 保持 vector_hnsw_initial_capacity 生效；
2. 针对 hnswlib_hnsw ordinary addPoint 成本继续细分，确认是否来自 hnswlib 内部邻居搜索、内存布局、label lookup 或锁/分配；
3. 增加 efSearch=32/64/128 的 L3 VSEARCH latency/recall tradeoff benchmark，避免把 topK=10 的旧结果和 efSearch=128 混比；
4. 继续只比较 hnswlib_hnsw 与 faiss_hnsw，Faiss 作为 baseline/fallback；
5. 输出 P0-C 报告，明确 VSET add 优化路径和 VSEARCH efSearch 默认值建议。
```

## 14. P0-C Follow-Up

Follow-up report:

`docs/vector_native_faiss_optimization/13_p0c_hnswlib_add_efsearch_report.md`

Run id:

`full_e2e_p0c_add_efsearch_20260624T023000Z`

P0-C confirmed that the P0-B reserve fix remains effective:

| Dataset | Capacity | Resize count |
|---:|---:|---:|
| 1K | 1000 | 0 |
| 10K | 10000 | 0 |

P0-C added a lower-level hnswlib wrapper split. At 10K:

| efSearch | hnswlib VSET avg | hnswlib index add avg | hnswlib addPoint avg | Faiss VSET avg | VSET ratio |
|---:|---:|---:|---:|---:|---:|
| 32 | 1270us | 1186us | 1186us | 270us | 4.70x |
| 64 | 1260us | 1182us | 1181us | 270us | 4.67x |
| 128 | 1260us | 1180us | 1179us | 270us | 4.67x |

Conclusion: P0-B correctly removed resize churn, and P0-C shows the remaining add cost is inside hnswlib `addPoint()` internals. Wrapper prepare, vector copy, bookkeeping, and stats update are not material.

P0-C also added an efSearch L3 latency/L1 recall sweep. Recommended default remains `efSearch=128` for quality; `efSearch=64` is a latency profile with a measured 10K L1 recall drop to `0.949400`.
