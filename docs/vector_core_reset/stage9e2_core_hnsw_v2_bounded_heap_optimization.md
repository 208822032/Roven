# Stage 9E2: core_hnsw_v2 bounded heap optimization

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只修改 `core_hnsw_v2` index-only 实现、v2 测试、Makefile v2 header 依赖和本报告。未接入 VINDEX/VSEARCH，未注册 v2 backend，未修改 current `core_hnsw` 行为，未改 RDB/AOF，未改 `kvs_vector_index_ops_t`，未改 VSEARCH response，未让 v2 访问 `g_vector_state`、manager-owned `record_id_map` 或 scope / TTL / metadata / question / answer 等 manager 语义字段。

本 prompt 与 `AGENTS.md` 无冲突。

## 2. Modified / added files

修改：

- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c`
- `tests/vector_core_hnsw_v2_performance_profile_test.c`
- `tests/vector_core_hnsw_v2_search_recall_test.c`
- `Makefile`

新增：

- `include/vector/kvs_vector_hnsw_v2_heap.h`（CODE-HYGIENE-2 后迁移；仍是 v2 internal helper）
- `docs/vector_core_reset/stage9e2_core_hnsw_v2_bounded_heap_optimization.md`

## 3. Production code changed

v2 index-only production code changed. v2 仍未接入产品 VINDEX/VSEARCH 路径。

## 4. Required config change

已完成强约束：

```c
#define KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION 256
```

旧默认 `128` 已移除；`grep -RIn "KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION 128" include/vector/kvs_vector_hnsw_v2.h && exit 1 || true` 无输出。

## 5. Heap helper design

新增 v2-only internal helper:

```text
include/vector/kvs_vector_hnsw_v2_heap.h
```

实现内容：

- candidate max-heap：root 是 best candidate，score 越大越优。
- bounded top min-heap：root 是 worst candidate，即 lower_bound，O(1) 读取。
- 降序导出：`kvs_hnsw_v2_heap_sort_desc()` 在输出给 neighbor_select / result export 前把 heap 内容转成 score 降序数组。
- tie-break：score 相等时 slot 小者更优；top heap worst tie 为 slot 大者更差，保证 deterministic。
- 不 malloc/free，不访问 manager，不访问 `g_vector_state`，不处理 record metadata，不改变 score polarity。

## 6. Current call chain

Construction:

```text
kvs_hnsw_v2_index_add()
  -> kvs_hnsw_v2_connect_new_node()
     -> upper greedy search
     -> kvs_hnsw_v2_search_layer_for_add()
        -> candidate max-heap pop best
        -> bounded top min-heap lower_bound / add / replace
        -> export top heap as score-desc candidates
     -> kvs_hnsw_v2_neighbor_select()
     -> forward A->B append + reverse B->A best-effort prune
```

Query:

```text
kvs_hnsw_v2_index_search()
  -> upper greedy search
  -> kvs_hnsw_v2_search_level0()
     -> candidate max-heap pop best
     -> bounded top min-heap lower_bound / add / replace
     -> early stop by top heap root
  -> export top heap as score-desc hits
```

## 7. Before behavior

Stage 9E1a / 9E0 search hot path used array maintenance:

- candidate pop-best: O(ef) linear scan.
- top lower_bound: O(ef) linear scan.
- top replace-worst: O(ef) linear scan.

Stage 9E1a accepted default `ef_construction=256` based on recall recovery but still had array-based construction/search maintenance.

## 8. After design

Construction layer search:

- candidate queue uses max-heap.
- top set uses bounded min-heap.
- lower_bound reads heap root in O(1).
- new candidate is pushed only under the same condition as before: `top_count < ef` or `score > lower_bound`.
- layer search exports top heap to score-desc array before neighbor_select and diagnostic comparison.
- old construction helpers are removed from `kvs_hnsw_v2_add.c`; grep for `kvs_hnsw_v2_build_candidate_pop_best|kvs_hnsw_v2_build_top_lower_bound|kvs_hnsw_v2_build_top_add` in add/search returns no output.

Query search:

- candidate queue uses max-heap.
- top set uses bounded min-heap.
- early stop uses top heap root as lower_bound.
- result export sorts top heap score-desc before filling `record_id + score` hits.
- effective `ef_search` clamp semantics are unchanged.

Context buffers:

- No per-query heap malloc/free was added.
- search heap reuses existing `ctx->candidates` and `ctx->top` buffers.
- `ctx->scratch` remains existing reserve/export scratch capacity; no `context.c` change was needed.
- context memory accounting remains based on existing buffer capacities.

## 9. Safety invariants

Preserved:

- v2 owns only record_id, vector, score, index-local graph/search state, slot map, deleted flags, memory/stat info, and diagnostic/profiling counters.
- visited tag semantics unchanged.
- deleted skip semantics unchanged.
- score polarity unchanged.
- result order remains score descending.
- degree limits remain M/M0.
- duplicate edge count remains zero.
- RDB/AOF unchanged.
- `kvs_vector_index_ops_t` unchanged.
- VSEARCH response unchanged.
- current `core_hnsw` implementation unchanged.

## 10. New profiling fields

Added to `kvs_hnsw_v2_build_stats_t`:

```text
construction_top_push_count
construction_top_replace_count
construction_top_skip_count
construction_heap_sift_count
```

Existing query fields were reused:

```text
search_candidate_push/pop/skip
search_top_push/replace
search_candidate_select_us
search_top_maintenance_us
```

`top_maintenance_us` now measures bounded heap add/replace cost. O(1) lower_bound root reads are not a large separable scan cost anymore.

## 11. Benchmark results

Command:

```bash
tests/vector_core_hnsw_v2_performance_profile_test.sh
```

### 11.1 Build table

| count | build_us | construction_layer_search_us | neighbor_select_us | reverse_prune_us | build_distance_compute_count | candidate_collect_scan_count | construction_push / pop / skip | top_push / replace / skip | heap_sift | memory_bytes |
|---:|---:|---:|---:|---:|---:|---:|---|---|---:|---:|
| 1K | 1,808,811 | 107,524 | 1,507,651 | 1,620,741 | 35,683,398 | 0 | 362,987 / 224,038 / 13,873,271 | 223,294 / 139,693 / 136,703 | 3,061,443 | 6,390,864 |
| 5K | 18,806,635 | 1,450,071 | 16,112,342 | 16,455,759 | 398,817,470 | 0 | 3,457,451 / 1,260,444 / 76,588,255 | 1,255,620 / 2,201,831 / 8,845,195 | 30,790,955 | 32,052,240 |
| 10K | 51,125,964 | 5,225,171 | 42,829,514 | 43,822,316 | 1,081,028,852 | 0 | 8,334,157 / 2,575,201 / 154,620,286 | 2,564,358 / 5,769,799 / 36,405,004 | 75,743,469 | 64,104,384 |

Build did not regress abnormally versus Stage 9E1a ef_construction=256 baseline:

| count | Stage 9E1a build_us | Stage 9E2 build_us | note |
|---:|---:|---:|---|
| 10K | 63,989,013 | 51,125,964 | lower in this run; still far below Stage 9E0 full-candidate 129,107,928 |

Reverse prune remains a major residual build cost; this stage did not optimize reverse prune.

### 11.2 Recall / ef sensitivity

| count | ef=128 recall@10 | ef=256 recall@10 | ef=512 recall@10 |
|---:|---:|---:|---:|
| 1K | 1.0000 | 1.0000 | 1.0000 |
| 5K | 0.9600 | 1.0000 | 1.0000 |
| 10K | 0.8840 | 0.9860 | 1.0000 |

`tests/vector_core_hnsw_v2_search_recall_test.sh` ef sensitivity:

```text
ef=64   recall@10=0.6800
ef=128  recall@10=0.8840
ef=256  recall@10=0.9860
ef=512  recall@10=1.0000
ef=1024 recall@10=1.0000
```

Monotonic non-decreasing: yes.

Recall gates:

- 1K/512d ef=256 recall@10 = 1.0000, passes >= 0.99.
- 10K/512d ef=256 recall@10 = 0.9860, passes >= 0.98.
- 10K/512d ef=512 recall@10 = 1.0000, passes >= 0.99.

### 11.3 Search performance table

| count | ef | recall@10 | search_avg_us | p50_us | p95_us | visited_avg | distance_avg | level0_scan_avg | candidate_select_us_avg | top_maintenance_us_avg |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 128 | 1.0000 | 145.44 | 144 | 149 | 999.76 | 1,038.38 | 8,200.96 | 19.32 | 17.14 |
| 1K | 256 | 1.0000 | 217.14 | 217 | 222 | 1,000.00 | 1,038.62 | 16,384.00 | 35.90 | 27.98 |
| 1K | 512 | 1.0000 | 350.50 | 350 | 359 | 1,000.00 | 1,038.62 | 32,768.00 | 63.32 | 39.88 |
| 5K | 128 | 0.9600 | 384.46 | 366 | 422 | 3,969.42 | 4,049.76 | 8,217.60 | 26.66 | 27.10 |
| 5K | 256 | 1.0000 | 524.34 | 525 | 538 | 4,772.44 | 4,852.78 | 16,390.40 | 48.74 | 52.56 |
| 5K | 512 | 1.0000 | 750.38 | 748 | 767 | 4,987.42 | 5,067.76 | 32,768.00 | 91.50 | 89.64 |
| 10K | 128 | 0.8840 | 643.60 | 612 | 868 | 5,519.50 | 5,602.82 | 8,299.52 | 25.84 | 32.30 |
| 10K | 256 | 0.9860 | 958.78 | 957 | 978 | 7,896.16 | 7,979.48 | 16,410.88 | 53.16 | 62.06 |
| 10K | 512 | 1.0000 | 1,404.54 | 1,401 | 1,448 | 9,516.82 | 9,600.14 | 32,770.56 | 102.16 | 110.66 |

### 11.4 Stage 9E1a / 9E0 before-after comparison

The best apples-to-apples prior table with array query maintenance is Stage 9E0 / Stage 9E1 reported search matrix. Stage 9E1a fixed build default to `ef_construction=256` but did not optimize query search.

| metric, 10K/512d ef=256 | Stage 9E0 / array | Stage 9E1a accepted build config | Stage 9E2 heap |
|---|---:|---:|---:|
| ef_construction | 128/full-candidate construction | 256 | 256 |
| recall@10 | 0.9840 | 0.9860 | 0.9860 |
| search_avg_us | 2,724.66 | not separately restated; same array search class | 958.78 |
| top_maintenance_us_avg | 1,368.86 | not separately restated; same array search class | 62.06 |
| candidate_select_us_avg | 149.68 | not separately restated; same array search class | 53.16 |
| build_us | 129,107,928 | 63,989,013 | 51,125,964 |

Stage 9E2 performance target status:

- `top_maintenance_us_avg` decreased: 1,368.86us -> 62.06us at 10K/ef=256.
- `search_avg_us` decreased: 2,724.66us -> 958.78us at 10K/ef=256.
- Recall did not regress versus Stage 9E1a accepted default: 0.9860 remains 0.9860.

## 12. Graph stats health

| count | level0_degree_avg | level0_p50 | level0_p95 | level0_max | upper_degree_avg | upper_p95 | duplicate | overflow | memory_bytes |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 64.00 | 64 | 64 | 64 | 19.00 | 19 | 0 | 0 | 6,390,864 |
| 5K | 64.00 | 64 | 64 | 64 | 31.53 | 32 | 0 | 0 | 32,052,240 |
| 10K | 64.00 | 64 | 64 | 64 | 31.58 | 32 | 0 | 0 | 64,104,384 |

No degree overflow, duplicate edge, or memory anomaly observed.

## 13. Correctness tests

Passed:

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"     PASS
tests/vector_core_hnsw_v2_skeleton_test.sh             PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh   PASS
tests/vector_core_hnsw_v2_search_recall_test.sh        PASS
tests/vector_core_hnsw_v2_performance_profile_test.sh  PASS
```

Current `core_hnsw` regression scripts were attempted:

```text
tests/vector_core_hnsw_correctness_test.sh   BLOCKED by sandbox socket permission
tests/vector_core_hnsw_rebuild_test.sh       BLOCKED by sandbox socket permission
tests/vector_core_hnsw_rdb_reload_test.sh    BLOCKED by sandbox socket permission
tests/vector_core_hnsw_aof_replay_test.sh    BLOCKED by sandbox socket permission
```

All four failed before test logic with:

```text
PermissionError: [Errno 1] Operation not permitted
  at Python socket.socket() in pick_port()
```

Direct `python3 -c 'import socket; socket.socket()'` in this environment reproduces the same permission error. This is an execution environment limitation, not an observed current `core_hnsw` behavioral failure.

## 14. Static boundary checks

Passed with no output:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION 128" include/vector/kvs_vector_hnsw_v2.h && exit 1 || true
grep -RIn "kvs_hnsw_v2_build_candidate_pop_best\|kvs_hnsw_v2_build_top_lower_bound\|kvs_hnsw_v2_build_top_add" src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c || true
git diff --check
```

`build/lib/libkvstore.a` contains v2 objects:

```text
kvs_hnsw_v2_index.o
kvs_hnsw_v2_storage.o
kvs_hnsw_v2_context.o
kvs_hnsw_v2_info.o
kvs_hnsw_v2_random.o
kvs_hnsw_v2_graph.o
kvs_hnsw_v2_neighbor_select.o
kvs_hnsw_v2_add.o
kvs_hnsw_v2_search.o
```

## 15. git status

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
?? include/vector/kvs_vector_hnsw_v2.h
?? src/vector/core/hnsw_v2/
?? tests/vector_core_hnsw_v2_graph_construction_test.c
?? tests/vector_core_hnsw_v2_graph_construction_test.sh
?? tests/vector_core_hnsw_v2_performance_profile_test.c
?? tests/vector_core_hnsw_v2_performance_profile_test.sh
?? tests/vector_core_hnsw_v2_search_recall_test.c
?? tests/vector_core_hnsw_v2_search_recall_test.sh
?? tests/vector_core_hnsw_v2_skeleton_test.c
?? tests/vector_core_hnsw_v2_skeleton_test.sh
```

Many listed files are pre-existing dirty or untracked files from prior stages. This stage did not process unrelated `README.md`.

## 16. Judgment

Stage 9E2 meets the required success criteria:

1. Default `ef_construction` is 256.
2. v2-only heap helper was added.
3. construction layer search uses heap.
4. query level0 search uses heap.
5. 10K/512d ef=256 recall@10 = 0.9860, passes >= 0.98.
6. ef_search sensitivity is monotonic non-decreasing.
7. 10K/512d ef=256 `top_maintenance_us_avg` and `search_avg_us` both decreased substantially.
8. v2 tests passed.
9. v2 boundary grep produced no output.
10. `git diff --check` passed.

The only incomplete validation item is current `core_hnsw` service-script regression execution, blocked by sandbox socket permission before test logic.

## 17. Next suggested step

Because recall is stable and search maintenance cost fell sharply, the next stage should be:

```text
Stage 9E3: core_hnsw_v2 distance/SIMD attribution
```

Do not enter layout or product integration directly. Reverse prune remains a build-side residual hotspot, but the route requested for successful 9E2 is distance/SIMD attribution next.
