# Stage 9E3: core_hnsw_v2 distance/SIMD attribution

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只做 `core_hnsw_v2` index-only distance/SIMD attribution：新增 v2-only distance counters、测试输出和本报告。未接入 VINDEX/VSEARCH，未注册 v2 backend，未修改 current `core_hnsw` 行为，未改 RDB/AOF，未改 `kvs_vector_index_ops_t`，未改 VSEARCH response，未让 v2 访问 `g_vector_state`、manager-owned `record_id_map` 或 scope / TTL / metadata / question / answer 等 manager 语义字段。

本 prompt 与 `AGENTS.md` 无冲突。

## 2. Modified / added files

修改：

- `Makefile`
- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c`
- `tests/vector_core_hnsw_v2_performance_profile_test.c`

新增：

- `tests/vector_core_hnsw_v2_distance_profile_test.c`
- `tests/vector_core_hnsw_v2_distance_profile_test.sh`
- `docs/vector_core_reset/stage9e3_core_hnsw_v2_distance_simd_attribution.md`

## 3. Production code changed

v2 index-only production code changed only for profiling counters. HNSW add/search behavior, graph construction semantics, visited/deleted handling, score ordering, ef defaults, RDB/AOF, VSEARCH response and current `core_hnsw` are unchanged.

## 4. Current call chain

Search:

```text
kvs_hnsw_v2_index_search()
  -> upper greedy layers
     -> kvs_hnsw_v2_metric_score()
        -> shared kvs_vector_distance_* backend
  -> level0 bounded heap search
     -> kvs_hnsw_v2_metric_score()
        -> shared kvs_vector_distance_* backend
  -> score-desc result export
```

Build:

```text
kvs_hnsw_v2_index_add()
  -> upper greedy add search
  -> construction layer search
  -> kvs_hnsw_v2_neighbor_select()
  -> forward link
  -> reverse link / reverse prune
     -> existing-neighbor rescoring
     -> kvs_hnsw_v2_neighbor_select()
```

## 5. Before behavior

Stage 9E2 already removed array-based candidate/top maintenance, but distance attribution was still too coarse:

- Search had total distance count only.
- Build had total construction distance count, but neighbor_select / reverse_prune distance work was not split.
- No dim sensitivity distance table.
- No scalar vs AVX2 index-only comparison.

## 6. After design

Added v2-only counters:

Search:

- `metric_score_call_count`
- `upper_distance_compute_count`
- `level0_distance_compute_count`
- aggregate and last-query fields in `kvs_hnsw_v2_search_stats_t`

Build:

- `construction_upper_distance_compute_count`
- `construction_level0_distance_compute_count`
- `neighbor_select_distance_compute_count`
- `reverse_prune_distance_compute_count`
- `reverse_prune_neighbor_select_distance_compute_count`

Added `tests/vector_core_hnsw_v2_distance_profile_test.c`:

- 10K dim sensitivity: 384d / 512d / 768d / 1024d.
- Fixed `topK=10`, `queries=50`, `ef_construction=256`, `ef_search=256/512`.
- Backend comparison: fallback scalar vs AVX2 on the same 10K/512d deterministic dataset.
- Prints `distance_us_*_est` using same-backend calibration cost per dot call multiplied by actual HNSW distance count.

Important interpretation rule:

- `distance_us_*_est` is an attribution estimate, not an exact wall-clock split inside HNSW. Per-distance `clock_gettime()` would distort build because build performs about 1B distance calls. Counts and fallback-vs-AVX2 deltas are the stronger evidence.

## 7. Current v2 distance backend

v2 uses shared KVLite distance backend through `kvs_hnsw_v2_metric_score()`:

```text
KVS_VECTOR_METRIC_IP -> kvs_vector_distance_dot()
```

In this environment `g_cfg.vector_distance_backend=auto` selected `avx2`.

v2 vector storage is already 64-byte aligned and padded by `vector_stride`, but the current AVX2 kernels use `_mm256_loadu_ps()`. For the tested dimensions 384 / 512 / 768 / 1024, there is no tail handling cost because all are divisible by 8.

## 8. Search distance attribution

Auto backend: AVX2.

| dim | ef | recall@10 | search_avg_us | p50 | p95 | distance_avg | distance_us_avg_est | distance_ratio_est | upper_distance_avg | level0_distance_avg | visited_avg |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 384 | 256 | 0.9720 | 763.80 | 707 | 1115 | 7,925.98 | 590.16 | 0.7727 | 86.56 | 7,839.42 | 7,839.42 |
| 384 | 512 | 0.9980 | 1,056.30 | 1054 | 1076 | 9,562.36 | 712.04 | 0.6741 | 86.56 | 9,475.80 | 9,475.80 |
| 512 | 256 | 0.9860 | 950.42 | 898 | 1233 | 7,979.48 | 970.12 | 1.0207 | 83.32 | 7,896.16 | 7,896.16 |
| 512 | 512 | 1.0000 | 1,317.58 | 1304 | 1422 | 9,600.14 | 1,167.20 | 0.8859 | 83.32 | 9,516.82 | 9,516.82 |
| 768 | 256 | 0.9420 | 1,648.50 | 1591 | 2044 | 8,050.92 | 1,962.02 | 1.1902 | 90.32 | 7,960.60 | 7,960.60 |
| 768 | 512 | 0.9940 | 2,321.78 | 2295 | 2448 | 9,641.94 | 2,349.78 | 1.0121 | 90.32 | 9,551.62 | 9,551.62 |
| 1024 | 256 | 0.9500 | 2,409.64 | 2371 | 2648 | 8,079.70 | 2,540.88 | 1.0545 | 86.60 | 7,993.10 | 7,993.10 |
| 1024 | 512 | 1.0000 | 3,205.96 | 3175 | 3376 | 9,663.58 | 3,038.78 | 0.9479 | 86.60 | 9,576.98 | 9,576.98 |

Attribution:

- Search distance compute is almost entirely level0.
- `metric_score_call_avg == distance_avg` in every row; wrapper call count matches distance count.
- `level0_distance_avg == visited_avg`; each unique level0 visit computes one score.
- Upper greedy distance is tiny, about 83-91 calls/query.
- Candidate/top heap maintenance is no longer dominant at 10K/512d ef=256: `candidate_select_us_avg=48.10`, `top_maintenance_us_avg=58.48`, while search wall time is `950.42us`.

## 9. Build distance attribution

Auto backend: AVX2.

| dim | build_us | construction_layer_search_us | neighbor_select_us | reverse_prune_us | build_distance_count | construction_distance | ns_distance | reverse_direct_distance | reverse_ns_distance |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 384 | 37,993,272 | 4,201,887 | 31,510,676 | 32,164,131 | 1,047,376,257 | 45,281,302 | 960,454,891 | 41,640,064 | 926,924,709 |
| 512 | 49,880,220 | 5,156,502 | 41,835,246 | 42,811,909 | 1,081,028,852 | 45,468,712 | 993,976,044 | 41,584,096 | 961,155,196 |
| 768 | 75,120,143 | 8,225,194 | 62,096,027 | 64,336,700 | 1,116,187,319 | 45,734,163 | 1,028,832,100 | 41,621,056 | 996,886,734 |
| 1024 | 105,189,190 | 10,970,905 | 87,459,139 | 90,879,272 | 1,133,457,418 | 45,850,166 | 1,045,977,748 | 41,629,504 | 1,014,498,934 |

512d build split:

- Construction layer search distance: `45.47M`, about 4.2% of total build distance calls.
- Neighbor select distance: `993.98M`, about 91.9% of total build distance calls.
- Reverse prune path includes `41.58M` direct existing-neighbor/new-neighbor rescoring plus `961.16M` neighbor_select distance calls.
- Reverse prune remains the build wall-time residual bottleneck: `42.81s` of `49.88s` at 10K/512d.

## 10. Dim sensitivity

Auto backend: AVX2, `count=10000`, `topK=10`, `queries=50`, `ef_construction=256`.

| dim | memory_bytes | build_us | ef=256 recall | ef=256 search_avg_us | ef=256 distance_avg | ef=512 recall | ef=512 search_avg_us | ef=512 distance_avg | graph health |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 384 | 58,984,384 | 37,993,272 | 0.9720 | 763.80 | 7,925.98 | 0.9980 | 1,056.30 | 9,562.36 | duplicate=0 overflow=0 |
| 512 | 64,104,384 | 49,880,220 | 0.9860 | 950.42 | 7,979.48 | 1.0000 | 1,317.58 | 9,600.14 | duplicate=0 overflow=0 |
| 768 | 74,344,384 | 75,120,143 | 0.9420 | 1,648.50 | 8,050.92 | 0.9940 | 2,321.78 | 9,641.94 | duplicate=0 overflow=0 |
| 1024 | 84,584,384 | 105,189,190 | 0.9500 | 2,409.64 | 8,079.70 | 1.0000 | 3,205.96 | 9,663.58 | duplicate=0 overflow=0 |

Observation:

- Distance count stays roughly stable across dimensions, while build/search time grows with dimension.
- This points to per-distance vector compute cost as the driver for high-dim search and a major driver for build.
- 768d/1024d ef=256 recall is lower than 512d; this is dim sensitivity data only. This stage does not change ef defaults or use lower recall to claim speed.

## 11. Scalar vs AVX2 comparison

Same deterministic 10K/512d dataset.

| backend | build_us | ef=256 search_avg_us | ef=512 search_avg_us | ef=256 recall | ef=512 recall | calibrated_us_per_dot |
|---|---:|---:|---:|---:|---:|---:|
| fallback | 436,712,214 | 3,810.22 | 4,766.56 | 0.9860 | 1.0000 | 0.459820 |
| avx2 | 49,880,220 | 950.42 | 1,317.58 | 0.9860 | 1.0000 | 0.121580 |

Result:

- Recall is identical between scalar and AVX2.
- AVX2 improves 10K/512d build wall time by about 8.8x in this run.
- AVX2 improves 10K/512d ef=256 search average by about 4.0x.
- Calibrated dot cost improves about 3.8x.

This confirms v2 is already using the shared AVX2 backend under `auto`, and distance backend choice is material.

## 12. Recall safety

Required 512d gates:

| gate | result |
|---|---:|
| 1K/512d ef=256 recall@10 | 1.0000 |
| 10K/512d ef=256 recall@10 | 0.9860 |
| 10K/512d ef=512 recall@10 | 1.0000 |

`tests/vector_core_hnsw_v2_search_recall_test.sh` ef sensitivity:

| ef | recall@10 |
|---:|---:|
| 64 | 0.6800 |
| 128 | 0.8840 |
| 256 | 0.9860 |
| 512 | 1.0000 |
| 1024 | 1.0000 |

Monotonic non-decreasing: yes.

## 13. Bottleneck judgment

Search:

- Distance is now the primary search bottleneck class.
- Top/candidate heap maintenance is much smaller after Stage 9E2.
- Remaining search distance work is almost entirely level0 unique visits.
- Potential future distance work should consider metric-specialized IP fast path, dispatch hoisting, aligned loads, and possibly pre-normalized/metric-specific kernels. It should not start layout work directly from this stage.

Build:

- Reverse prune remains the residual build bottleneck by wall time.
- Reverse prune is also mostly distance work: at 10K/512d, reverse prune path accounts for about `1.003B` distance-related calls when direct reverse scoring and reverse-prune neighbor_select distance are combined.
- This is not just a kernel problem; it is repeated reverse-prune distance work.

Therefore, for the next build-focused step, the stronger branch is `Stage 9E3b: reverse_prune distance reuse / candidate cache attribution`. For query-search-only work, Stage 9E4 metric-specialized distance fast path is justified by the scalar-vs-AVX2 delta, but productizing v2 still has a build blocker first.

## 14. Safety invariants

Preserved:

- v2 owns only record_id, vector, score, index-local graph/search state, slot map, deleted flags, memory/stat info and profiling counters.
- No manager semantic fields in v2.
- No `g_vector_state` access in v2.
- No `record_id_map` access in v2.
- RDB/AOF unchanged.
- `kvs_vector_index_ops_t` unchanged.
- VSEARCH response unchanged.
- current `core_hnsw` behavior unchanged.
- No SIMD rewrite, layout refactor, graph compact, prefetch optimization or product integration was performed.

## 15. Build and tests

Passed:

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"              PASS
tests/vector_core_hnsw_v2_skeleton_test.sh                    PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh          PASS
tests/vector_core_hnsw_v2_search_recall_test.sh               PASS
tests/vector_core_hnsw_v2_performance_profile_test.sh         PASS
tests/vector_core_hnsw_v2_distance_profile_test.sh            PASS
tests/vector_core_hnsw_correctness_test.sh                    PASS
tests/vector_core_hnsw_rebuild_test.sh                        PASS
tests/vector_core_hnsw_rdb_reload_test.sh                     PASS
tests/vector_core_hnsw_aof_replay_test.sh                     PASS
```

## 16. Static boundary checks

Commands:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
git diff --check
```

Result: all no output / PASS.

## 17. git status --short

```text
 M AGENTS.md
 M Makefile
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
?? docs/vector_core_reset/hnsw_v2实施方案.md
?? docs/vector_core_reset/stage7e_neighbor_select_score_reuse_optimization.md
?? docs/vector_core_reset/stage7f_highdim_layer_search_audit.md
?? docs/vector_core_reset/stage7g_layer_search_distance_alignment_optimization.md
?? docs/vector_core_reset/stage7h_level0_neighbor_visited_layout_audit.md
?? docs/vector_core_reset/stage7i_level0_visited_slot_locality_prototype.md
?? docs/vector_core_reset/stage7j_add_search_slot_state_locality_prototype.md
?? docs/vector_core_reset/stage7k_remove_failed_layout_prototypes.md
?? docs/vector_core_reset/stage7l_post_layout_layer_search_hotspot_audit.md
?? docs/vector_core_reset/stage8a_core_flat_hnsw_faiss_parity_architecture_audit.md
?? docs/vector_core_reset/stage9a1_core_hnsw_bidirectional_link_acceptance_audit.md
?? docs/vector_core_reset/stage9a_core_hnsw_v2_design_boundary.md
?? docs/vector_core_reset/stage9b_core_hnsw_v2_index_only_skeleton.md
?? docs/vector_core_reset/stage9c_core_hnsw_v2_graph_construction.md
?? docs/vector_core_reset/stage9d_core_hnsw_v2_search_recall_gate.md
?? docs/vector_core_reset/stage9e0_core_hnsw_v2_performance_attribution.md
?? docs/vector_core_reset/stage9e1_core_hnsw_v2_add_time_layer_search.md
?? docs/vector_core_reset/stage9e1a_core_hnsw_v2_candidate_quality_attribution.md
?? docs/vector_core_reset/stage9e2_core_hnsw_v2_bounded_heap_optimization.md
?? docs/vector_core_reset/stage9e3_core_hnsw_v2_distance_simd_attribution.md
?? include/vector/kvs_vector_hnsw_v2.h
?? src/vector/core/hnsw_v2/
?? tests/vector_core_hnsw_v2_distance_profile_test.c
?? tests/vector_core_hnsw_v2_distance_profile_test.sh
?? tests/vector_core_hnsw_v2_graph_construction_test.c
?? tests/vector_core_hnsw_v2_graph_construction_test.sh
?? tests/vector_core_hnsw_v2_performance_profile_test.c
?? tests/vector_core_hnsw_v2_performance_profile_test.sh
?? tests/vector_core_hnsw_v2_search_recall_test.c
?? tests/vector_core_hnsw_v2_search_recall_test.sh
?? tests/vector_core_hnsw_v2_skeleton_test.c
?? tests/vector_core_hnsw_v2_skeleton_test.sh
```

`README.md` and many Stage 7-9 files were pre-existing dirty/untracked state and were not processed by this stage.

## 18. Next suggested step

Do not enter layout/product integration directly.

Recommended next step:

```text
Stage 9E3b: core_hnsw_v2 reverse_prune distance reuse / candidate cache attribution
```

Reason:

- Search is distance-dominated and AVX2 is already material.
- Build remains the bigger product blocker.
- Build residual is dominated by reverse_prune, and reverse_prune is dominated by repeated distance work.

After reverse_prune attribution, decide whether to do a formal reverse_prune reuse optimization or a metric-specialized SIMD/distance fast path.
