# Stage 8A: core_flat / core_hnsw architecture root-cause analysis for FAISS parity

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只做 architecture / profiling / benchmark / audit / design，未实现正式优化，未做正式 refactor，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未降低 recall，未删除既有 backend，未处理既有无关 `README.md` 修改。

本 prompt 允许重新评估既有架构路线；这与 `AGENTS.md` 的硬边界不冲突。约束影响是：即使性能和稳定性是主要目标，本阶段也不能擅自改变持久化、公开响应、index ops 签名或 manager/core 边界。未发现需要停止执行的冲突。

## 2. Modified / added files

新增文件：

- `docs/vector_core_reset/stage8a_core_flat_hnsw_faiss_parity_architecture_audit.md`

本阶段未修改生产代码。工作树中已有 Stage 7E-7L 代码/文档改动和无关 `README.md` 修改，本阶段未回退或处理。

## 3. Current call chains

core_flat:

```text
VSEARCH / direct index search
  -> kvs_vector_index_search_current()
  -> kvs_vector_core_flat_ops_search()
  -> kvs_flat_index_search()
  -> scalar distance loop + topK min-heap
  -> kvs_vector_manager_resolve_hits()
```

core_hnsw rebuild:

```text
VINDEX REBUILD / ann_core rebuild
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_vector_manager_for_each_active_embedding()
  -> kvs_hnsw_index_add()
  -> greedy upper search
  -> kvs_hnsw_search_layer_for_add()
  -> neighbor_select score reuse
  -> bidirectional link maintenance
```

FAISS server backend:

```text
VSEARCH
  -> kvs_vector_faiss_hnsw_ops_search()
  -> kvs_faiss_index_search()
  -> faiss::IndexIDMap2(IndexFlatIP or IndexHNSWFlat)::search()
  -> kvs_vector_manager_resolve_hits()
```

## 4. core_flat architecture

Current data structure:

- `vectors`: contiguous `capacity * dim` float array.
- `slot_to_record_id`: contiguous record id array.
- `deleted_flags`: separate byte array.
- `id_to_slot`: core-local record_id -> slot map.
- Search is single-threaded full scan.
- Distance kernels in `src/vector/core/flat/kvs_flat_search.c` are local scalar loops, not the shared AVX2 backend.
- topK uses per-query heap with one candidate at a time.
- Delete is logical mark-delete; rebuild compacts by replaying active records from manager store.

Value:

- Good as a simple exact fallback, correctness oracle, small-N backend, and benchmark ground truth.
- Useful for debugging manager/core boundary because it has minimal graph state.

Gap versus FAISS IndexFlat:

- FAISS uses heavily optimized distance kernels, batch search paths, better cache/prefetch behavior, and can use threading/OpenMP depending build/runtime.
- core_flat currently has no SIMD through the shared distance backend, no query batching, no parallel scan, no aligned allocation contract, and heap-per-query scalar topK control flow.
- Therefore core_flat should not be expected to match FAISS IndexFlat at medium/high N or high dimension without a targeted rewrite.

## 5. core_hnsw architecture

Current data structure:

- `vectors`: contiguous slot-major float rows.
- `levels`, `deleted_flags`, `visited_tags`, `slot_to_record_id`: separate per-slot arrays.
- `level0_neighbors`: fixed-width `capacity * M0` row-major uint32 adjacency.
- `level0_neighbor_scores`: fixed-width cached scores added by Stage 7E.
- `level0_counts`, `level0_worst_*`: separate row count and worst-neighbor cache state.
- `upper_neighbors`: preallocated `capacity * KVS_HNSW_MAX_LEVEL * M` uint32 array.
- `upper_counts`: separate count array.
- Search contexts own candidate heap, top heap, neighbor buffer, distance buffer, hits buffer.

Current behavior:

- Rebuild is single-threaded repeated `kvs_hnsw_index_add()`.
- Graph is derived runtime state only; RDB/AOF persist records and rebuild graph on load/replay.
- Delete is mark-delete; cleanup requires rebuild.
- Stage 7E removed level0 worst-neighbor distance recompute.
- Stage 7G reduced high-dim distance/call overhead.
- Stage 7I/7J layout prototypes were removed by Stage 7K because they did not reduce `level0_us`.
- Stage 7L shows the remaining add-time cost is mostly level0 layer_search.

Gap versus FAISS IndexHNSW:

- FAISS implementation benefits from mature HNSW construction/search loops, optimized distance kernels, tuned data structures, and better-tested parameter semantics.
- Current core_hnsw level0 rows saturate near M0 and create high repeated visited skip pressure.
- Rebuild is incremental add replay, not a specialized bulk build.
- Memory layout still uses multiple independent arrays and large upper-layer preallocation.
- Parameter defaults are not proven equivalent to FAISS/hnswlib for larger N; direct-vector 10K/512d recall was only 0.6240 with current ann_core defaults.
- Stability surface is larger: custom graph construction, custom heaps, custom delete/rebuild semantics, many profiling fields, and several failed layout prototypes.

## 6. Benchmark data

### 6.1 Direct-vector core_hnsw high-dimensional profile

Command family:

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 <dim> 10 50
./build/tests/bin/vcmd_profile_bench ann_core 10000 512 10 50
```

This is the best current evidence for core_hnsw internal high-dimensional rebuild attribution. It bypasses RESP/network/text embedding and uses synthetic direct vectors.

| N | dim | rebuild_wall_us | rebuild_core_add_total_us | layer_search_us | level0_us | level0_distance | level0_scan | repeated_skip | search_avg_us | recall@10 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1K | 384 | 81,886 | 81,689 | 69,404 | 69,229 | 499,116 | 6,689,215 | 6,191,098 | 129 | 1.0000 |
| 1K | 512 | 90,165 | 89,846 | 76,381 | 76,146 | 499,080 | 6,638,541 | 6,140,460 | 145 | 1.0000 |
| 1K | 768 | 104,290 | 103,925 | 90,472 | 90,332 | 499,106 | 6,548,273 | 6,050,166 | 173 | 1.0000 |
| 1K | 1024 | 120,379 | 120,101 | 104,826 | 104,592 | 499,093 | 6,517,304 | 6,019,210 | 212 | 1.0000 |
| 10K | 512 | 2,645,074 | 2,640,815 | 2,398,373 | 2,383,517 | 22,675,838 | 42,071,252 | 19,405,413 | 912 | 0.6240 |

Interpretation:

- At 1K, Stage 7E/7G made core_hnsw usable and recall remains 1.0.
- At 10K/512d, rebuild cost and level0 search cost grow sharply, and current direct-vector recall@10 is not competitive.
- The 10K result is the strongest signal that the root issue is not only micro-optimization overhead; current graph construction/search behavior and parameters do not scale to FAISS parity.

### 6.2 Server-path backend comparison

Command family:

```bash
python3 scripts/bench_vector_core_compare.py --kv-bin ./kvstore --n 1000 --dim <dim> --topk 10 --queries 50 \
  --backend core_flat --backend core_hnsw --backend faiss_flat --backend faiss_hnsw --backend hnswlib_hnsw \
  --out-dir results/vector_core_benchmark/stage8a
```

Important fairness warning:

- This script uses text/hash embedding path; the report itself marks `benchmark_requested_dim_effective_for_hash_text_path: false`.
- Therefore 384/512/768/1024 in this script are not reliable high-dimensional vector-kernel comparisons.
- It is useful for VSEARCH endpoint, manager resolve, response assembly, rebuild/reload smoke, and backend availability.
- FAISS server-path 1K search had anomalously high `vsearch_backend_search_avg_us`; index-only FAISS baseline below does not reproduce that. This must be treated as a FAISS adapter/runtime measurement issue, not FAISS core-kernel evidence.

| N | dim | backend | VSEARCH avg ms | backend avg us | resolve avg us | response avg us | recall@10 | rebuild wall ms | memory bytes | memory source |
| ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1K | 384 | core_flat | 0.110 | 49 | 1 | 8 | 1.000 | 0.421 | 304,128 | measured |
| 1K | 384 | core_hnsw | 0.103 | 46 | 1 | 8 | 1.000 | 37.872 | 2,766,032 | measured |
| 1K | 384 | faiss_flat | 34.499 | 32,610 | 5 | 15 | 1.000 | 0.421 | 264,000 | estimated |
| 1K | 384 | faiss_hnsw | 24.550 | 23,610 | 4 | 12 | 0.990 | 0.396 | 392,000 | estimated |
| 1K | 384 | hnswlib_hnsw | 0.440 | 346 | 2 | 9 | 1.000 | 0.438 | 447,816 | estimated |
| 1K | 512 | core_flat | 0.123 | 49 | 1 | 8 | 1.000 | 0.451 | 304,128 | measured |
| 1K | 512 | core_hnsw | 0.116 | 46 | 1 | 8 | 1.000 | 37.157 | 2,766,032 | measured |
| 1K | 512 | faiss_flat | 33.358 | 30,872 | 447 | 13 | 1.000 | 0.402 | 264,000 | estimated |
| 1K | 512 | faiss_hnsw | 22.898 | 17,815 | 4 | 11 | 0.990 | 0.404 | 392,000 | estimated |
| 1K | 512 | hnswlib_hnsw | 0.405 | 345 | 2 | 8 | 1.000 | 0.389 | 447,816 | estimated |
| 1K | 768 | core_flat | 0.110 | 49 | 1 | 8 | 1.000 | 0.421 | 304,128 | measured |
| 1K | 768 | core_hnsw | 0.143 | 46 | 2 | 9 | 1.000 | 48.310 | 2,766,032 | measured |
| 1K | 768 | faiss_flat | 35.091 | 33,679 | 253 | 13 | 1.000 | 0.729 | 264,000 | estimated |
| 1K | 768 | faiss_hnsw | 23.081 | 22,771 | 4 | 11 | 0.990 | 0.398 | 392,000 | estimated |
| 1K | 768 | hnswlib_hnsw | 0.405 | 344 | 2 | 8 | 1.000 | 0.385 | 447,816 | estimated |
| 1K | 1024 | core_flat | 0.129 | 49 | 1 | 8 | 1.000 | 0.442 | 304,128 | measured |
| 1K | 1024 | core_hnsw | 0.103 | 46 | 1 | 8 | 1.000 | 37.355 | 2,766,032 | measured |
| 1K | 1024 | faiss_flat | 32.679 | 29,829 | 4 | 294 | 1.000 | 0.399 | 264,000 | estimated |
| 1K | 1024 | faiss_hnsw | 0.310 | 238 | 2 | 9 | 0.990 | 0.418 | 392,000 | estimated |
| 1K | 1024 | hnswlib_hnsw | 0.429 | 346 | 2 | 8 | 1.000 | 0.411 | 447,816 | estimated |
| 10K | 512 | core_flat | 0.460 | 371 | 3 | 9 | 1.000 | 1.949 | 4,866,048 | measured |
| 10K | 512 | core_hnsw | 0.134 | 50 | 3 | 9 | 0.992 | 560.099 | 44,038,352 | measured |
| 10K | 512 | faiss_flat | 6.060 | 5,850 | 4 | 10 | 0.998 | 3.886 | 2,640,000 | estimated |
| 10K | 512 | faiss_hnsw | 0.094 | 34 | 3 | 8 | 0.996 | 4.951 | 3,920,000 | estimated |
| 10K | 512 | hnswlib_hnsw | 0.511 | 413 | 3 | 11 | 0.996 | 5.125 | 4,020,816 | estimated |

### 6.3 FAISS index-only baseline

Command family:

```bash
OMP_NUM_THREADS=1 ./build/tests/bin/faiss_baseline flat_ip 1000 <dim> 50 10
OMP_NUM_THREADS=1 ./build/tests/bin/faiss_baseline hnsw 1000 <dim> 50 10 32 128 256
OMP_NUM_THREADS=1 ./build/tests/bin/faiss_baseline flat_ip 10000 512 50 10
OMP_NUM_THREADS=1 ./build/tests/bin/faiss_baseline hnsw 10000 512 50 10 32 128 256
```

This benchmark is not server-path comparable, but it is better evidence for FAISS kernel capability than the current server adapter results.

| N | dim | FAISS backend | add QPS | search QPS | search p50 us | search p95 us | recall@10 | RSS KB |
| ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1K | 384 | flat_ip | 542,594 | 15,679 | 63 | 67 | 1.0000 | 12,744 |
| 1K | 384 | hnsw | 8,541 | 6,158 | 162 | 168 | 1.0000 | 13,572 |
| 1K | 512 | flat_ip | 482,160 | 11,196 | 88 | 93 | 1.0000 | 14,340 |
| 1K | 512 | hnsw | 6,816 | 5,658 | 176 | 187 | 1.0000 | 15,092 |
| 1K | 768 | flat_ip | 315,457 | 7,034 | 141 | 150 | 1.0000 | 17,452 |
| 1K | 768 | hnsw | 4,877 | 4,992 | 199 | 207 | 1.0000 | 18,064 |
| 1K | 1024 | flat_ip | 215,054 | 5,139 | 193 | 205 | 1.0000 | 20,564 |
| 1K | 1024 | hnsw | 3,783 | 4,443 | 225 | 233 | 1.0000 | 21,372 |
| 10K | 512 | flat_ip | 256,845 | 979 | 858 | 891 | 1.0000 | 68,332 |
| 10K | 512 | hnsw | 1,171 | 1,323 | 749 | 825 | 0.9740 | 75,328 |

## 7. Fairness and measurement gaps

Current benchmark set is not sufficient for a final default-backend decision:

- Direct-vector `vcmd_profile_bench ann_core` is high-dimensional and has exact recall, but only exercises core_hnsw.
- `bench_vector_core_compare.py` compares all backends through V commands, but high-dimensional `--dim` is not effective for the hash text path.
- FAISS server adapter data has a 1K search anomaly not reproduced by `faiss_baseline`; this requires a separate adapter/runtime audit.
- FAISS memory in VINFO is estimated, not measured.
- Server-path rebuild numbers for external backends are not equivalent to core_hnsw rebuild profiles.
- Dataset generation differs across tools; direct-vector, text hash path, and FAISS baseline are not the same workload.
- Parameter equivalence is incomplete: FAISS/hnswlib/core_hnsw may use same nominal M/ef values but not identical construction heuristics, normalization, or ID-map behavior.

Minimum fair benchmark needed before switching defaults:

- One generated float32 dataset and query set shared by core_flat, core_hnsw, faiss_flat, faiss_hnsw, and hnswlib.
- Same metric, normalization policy, N, dim, topK, M, efConstruction, efSearch.
- Separate index-only build/search from server-path VSEARCH.
- Report recall against the same exact flat ground truth.
- Report measured RSS or allocator-owned memory consistently, not mixed measured/estimated memory.

## 8. Root-cause ranking

1. Algorithmic root cause: current core_hnsw incremental construction/search does not scale to FAISS parity at larger N. Direct-vector 10K/512d shows high rebuild cost and recall@10 0.6240.
2. Data-structure/layout root cause: level0 rows saturate, row expansion scans dense rows, and visited skip pressure remains high. Stage 7L still shows millions of repeated visits at 1K and tens of millions at 10K.
3. Implementation overhead: core_flat is scalar and heap-per-query; core_hnsw still has custom heap/control-flow overhead. Stage 7E/7G removed the largest obvious micro-hotspots, so remaining gains are less likely from small local tweaks.
4. Benchmark/measurement issue: server-path FAISS results are currently inconsistent with FAISS index-only baseline, so they cannot be used as FAISS kernel evidence.
5. API/architecture constraint: manager/core boundary is not the dominant remaining core_hnsw rebuild bottleneck. Stage 5A made resolve O(raw_topk), and current resolve/response costs are generally single-digit microseconds per query in the server-path benchmark.
6. Stability/maintenance risk: self-maintained HNSW now includes graph construction, deletion semantics, rebuild, many profiling fields, and failed layout prototype history. Keeping it FAISS-competitive would require a larger engineering investment than continuing Stage 7-style local patches.

## 9. Why Stage 5-7 local optimization is near its limit

Stage 5 resolved the obvious manager fast-resolve issue. Stage 7E removed level0 worst-neighbor distance recomputation. Stage 7G reduced distance/SIMD/call overhead. Stage 7I/7J tested two plausible locality prototypes and both failed to reduce `level0_us`; Stage 7K removed them. Stage 7L showed remaining work is split between repeated visited scans and unique distance computes.

The remaining profile is no longer a single accidental wrapper/recompute bug. It reflects the construction/search strategy and graph/row expansion behavior. Continuing to add narrow counters or mirrors is unlikely to close the FAISS gap unless a new audit finds a different quantified bottleneck.

## 10. Manager/backend boundary conclusion

The manager/backend boundary is not the root cause for core_hnsw rebuild performance:

- Rebuild profile shows most time inside `kvs_hnsw_index_add()` / `layer_search`.
- Direct-vector ann_core bypasses VSEARCH response path and still shows the same core_hnsw layer_search bottleneck.
- Server-path manager resolve and response assembly are usually about 1-13 us/query for core backends in this run.

The boundary still matters for FAISS adapter correctness and measurement:

- FAISS server-path 1K results are anomalous compared with FAISS index-only baseline.
- This should be audited before making FAISS default, but it is separate from the core_hnsw rebuild root cause.

## 11. Recommendation

If performance and stability are the only goals, do not continue trying to reach FAISS parity through small core_hnsw patches.

Recommended direction:

- Keep `core_flat` as exact, simple, stable fallback and correctness oracle.
- Do not make current `core_hnsw` the long-term high-performance default without either a full rewrite or a clear product reason to avoid FAISS.
- Treat FAISS as the primary candidate for default ANN / flat performance backend, after fixing the adapter benchmark anomaly and validating persistence/rebuild/delete semantics under KVLite manager rules.
- If a self-contained C backend is mandatory, introduce a new rewritten core_hnsw backend beside the old one rather than broad-editing the current implementation in place.

## 12. Roadmap

P0: immediate decisions / validation

- Build one fair index-only benchmark harness shared by core_flat, core_hnsw, FAISS, and hnswlib using identical float datasets.
- Audit the FAISS server adapter anomaly where 1K server-path FAISS search is tens of milliseconds while FAISS index-only is sub-millisecond.
- Add measured or clearly unsupported FAISS memory reporting; do not rely on current estimates for default-backend decisions.
- Decide whether product policy allows FAISS as default dependency.

P1: high-return refactor candidates

- If FAISS default is acceptable: harden FAISS adapter as a first-class backend while keeping RDB/AOF record-source semantics unchanged.
- If core_flat remains performance-relevant: rewrite core_flat search around shared AVX2/batch/parallel exact scan and a lower-overhead topK path.
- If self HNSW is still required: create a new experimental core_hnsw_v2 with bulk-build-oriented design, explicit parameter parity tests, measured memory, and index-only benchmark parity gates.

P2: optional optimizations

- Parallel rebuild/bulk add after backend choice is settled.
- Memory pool/aligned vector storage for exact and ANN backends.
- Better deletion/rebuild scheduling policy based on tombstone ratio and recall impact.
- Heap/candidate timing split only if a new profile proves it material.

Not recommended:

- Do not restore Stage 7I/7J layout prototypes.
- Do not continue Stage 7-style boundary/locality micro-optimization as the main route.
- Do not directly refactor layout again without a new quantified bottleneck.
- Do not lower `ef_construction` / `ef_search` to claim speed.
- Do not make FAISS default until the server-path anomaly and memory/stat reporting are audited.

## 13. Minimal broad-refactor boundary if approved

If choosing a broad backend route, the smallest safe boundary is:

- Add or promote a backend behind the existing `kvs_vector_index_ops_t`; do not change ops signatures.
- Keep record store as source of truth; RDB/AOF continue saving records only.
- Rebuild external/default backend from active records on load/replay exactly as today.
- Keep core_flat and current core_hnsw available for fallback and regression comparison.
- Gate rollout with:
  - exact shared-dataset recall tests;
  - VSET/VSEARCH/VDEL/VINDEX/VINFO behavior tests;
  - RDB reload and AOF replay tests;
  - memory source checks;
  - rollback by config to `core_flat` or current `core_hnsw`.

## 14. Correctness tests

Passed:

```bash
tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
tests/vector_core_hnsw_rebuild_profile_test.sh
```

## 15. Static boundary check result

Commands:

```bash
grep -RIn "level0_rows\|level0_row_stride\|packed_row\|slot_hot_state\|kvs_hnsw_slot_hot_state_t" include src tests || true
grep -RIn "g_vector_state" src/vector/core || true
grep -RIn "record_id_map" src/vector/core || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core || true
git diff --check
```

Result: no output.

## 16. Build result

Passed:

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

## 17. git status --short

Final status after adding this Stage 8A report:

```text
 M README.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_index.c
 M src/vector/core/hnsw/kvs_hnsw_info.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_distance_avx2.c
 M tests/vcmd_profile_bench.c
 M tests/vector_core_hnsw_rebuild_profile_test.sh
?? docs/vector_core_reset/stage7e_neighbor_select_score_reuse_optimization.md
?? docs/vector_core_reset/stage7f_highdim_layer_search_audit.md
?? docs/vector_core_reset/stage7g_layer_search_distance_alignment_optimization.md
?? docs/vector_core_reset/stage7h_level0_neighbor_visited_layout_audit.md
?? docs/vector_core_reset/stage7i_level0_visited_slot_locality_prototype.md
?? docs/vector_core_reset/stage7j_add_search_slot_state_locality_prototype.md
?? docs/vector_core_reset/stage7k_remove_failed_layout_prototypes.md
?? docs/vector_core_reset/stage7l_post_layout_layer_search_hotspot_audit.md
?? docs/vector_core_reset/stage8a_core_flat_hnsw_faiss_parity_architecture_audit.md
```

## 18. Next suggested step

Run a Stage 8B benchmark fairness / FAISS adapter audit before any implementation:

- one shared float dataset;
- index-only and server-path separated;
- core_flat/core_hnsw/faiss_flat/faiss_hnsw/hnswlib compared under identical parameters;
- FAISS server-path anomaly attributed;
- memory source fixed or explicitly unsupported.

Only after that should the project decide whether to make FAISS the default backend, rewrite core_flat, or start a new core_hnsw_v2.
