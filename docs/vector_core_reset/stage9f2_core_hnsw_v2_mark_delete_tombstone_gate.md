# Stage 9F2: core_hnsw_v2 mark_delete API + tombstone gate

## AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。

本阶段只做 `core_hnsw_v2` index-only tombstone API 和 tombstone behavior gate。未注册 `core_hnsw_v2` backend，未改变 VSEARCH / VINDEX / VDEL runtime behavior，未修改 VSEARCH response，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 current `core_hnsw` 行为，未接 manager write-path delta capture，未实现 shadow replay / background thread / swap。

本 prompt 与 `AGENTS.md` 无硬边界冲突。`AGENTS.md` 中早期 Stage 5 推荐路线属于历史基线；本阶段按用户提供的 Stage 9F2 路线执行，符合 v2 stage discipline。

## Modified / added files

本阶段修改：

- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c`
- `Makefile`

本阶段新增：

- `tests/vector_core_hnsw_v2_tombstone_test.c`
- `tests/vector_core_hnsw_v2_tombstone_test.sh`
- `docs/vector_core_reset/stage9f2_core_hnsw_v2_mark_delete_tombstone_gate.md`

生产代码有变更，但只限 v2 index-only API 和 v2 search tombstone handling。`Makefile` 只接入新增 tombstone test 目标。

## Current call chain

产品路径保持不变：

```text
VSET/VSEARCH/VDEL/VINDEX
  -> existing manager/current_index path
  -> current product backend
```

新增 index-only API：

```text
kvs_hnsw_v2_index_mark_delete(index, record_id)
  -> record_id_to_slot lookup
  -> validate slot in range and slot_to_record_id[slot] == record_id
  -> set deleted_flags[slot]
  -> increment deleted_count once
```

v2 search tombstone path：

```text
kvs_hnsw_v2_index_search()
  -> upper greedy may traverse tombstone nodes
  -> level0 search may push and expand tombstone candidates
  -> top heap only receives active candidates
  -> final result export defensively filters deleted slots
```

## mark_delete API design

Added public index-only API:

```c
int kvs_hnsw_v2_index_mark_delete(kvs_hnsw_v2_index_t *index, uint64_t record_id);
```

Return behavior:

- `index == NULL`: `KVS_VECTOR_ERR_INVALID_ARG`
- uninitialized storage: `KVS_VECTOR_ERR_NOT_INITIALIZED`
- missing `record_id`: `KVS_VECTOR_ERR_NOT_FOUND`
- stale/inconsistent slot mapping: `KVS_VECTOR_ERR_NOT_FOUND`
- existing active `record_id`: `KVS_VECTOR_OK`, sets tombstone
- repeated delete of same `record_id`: `KVS_VECTOR_OK`, no extra `deleted_count`

Missing record_id is a non-mutating not-found result. Future manager replay can choose whether a DELETE not found in a shadow index is a non-fatal no-op; 9F2 does not implement replay policy.

## Tombstone state

`deleted_flags[slot]` remains the tombstone source inside v2. `deleted_count` tracks the number of tombstoned slots and is exposed through existing index-only `kvs_hnsw_v2_index_info()` via `kvs_vector_core_info_t.deleted_count`.

`mark_delete` does not:

- remove graph edges
- move slots
- clear `slot_to_record_id`
- remove `record_id_to_slot`
- free vector storage
- trigger rebuild
- access manager state

Reset / destroy behavior:

- `kvs_hnsw_v2_index_reset()` clears vectors, graph, `deleted_flags`, `deleted_count`, and `record_id_to_slot`.
- `kvs_hnsw_v2_index_destroy()` releases v2 storage and clears the index struct.

## Search behavior

Deleted nodes are retained in the graph and can still be routing nodes:

- upper greedy no longer rejects a neighbor solely because `deleted_flags[neighbor]` is set.
- level0 search marks tombstone neighbors visited, computes their score, pushes them to the candidate heap, and can pop/expand them.
- tombstone nodes are skipped from the bounded top heap and filtered again at result export.

Deleted nodes therefore do not appear in search hits, while graph traversal can still pass through them.

## Tombstone tests

New test:

```text
tests/vector_core_hnsw_v2_tombstone_test.sh
```

Coverage:

- init / reserve / add start with `deleted_count = 0`
- `mark_delete(NULL, id)` rejects invalid arg
- existing id delete succeeds
- `deleted_count` increments once
- repeated delete is idempotent
- missing id returns not-found and keeps state unchanged
- deleted id remains in `record_id_to_slot`
- deleted slot keeps `slot_to_record_id`
- deleted nearest neighbor is filtered from hits
- next active nearest neighbor is returned
- multiple deletes return only active ids
- all deleted returns 0 hits
- reset clears tombstone state and id map
- destroy clears storage
- test uses v2 index-only API and does not touch manager state

## Recall / performance regression

Directly relevant gates after final code:

```text
tests/vector_core_hnsw_v2_tombstone_test.sh PASS
tests/vector_core_hnsw_v2_skeleton_test.sh PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh PASS
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_performance_profile_test.sh PASS
```

Final v2 recall sensitivity from `vector_core_hnsw_v2_search_recall_test`:

| count | dim | ef | recall@10 |
|---:|---:|---:|---:|
| 1000 | 512 | 256 | 1.0000 |
| 5000 | 512 | 256 | 1.0000 |
| 10000 | 512 | 64 | 0.6800 |
| 10000 | 512 | 128 | 0.8840 |
| 10000 | 512 | 256 | 0.9860 |
| 10000 | 512 | 512 | 1.0000 |
| 10000 | 512 | 1024 | 1.0000 |

ef sensitivity remains monotonic non-decreasing. 10K/512d ef=256 remains above the 0.98 gate; ef=512 remains above the 0.99 gate.

Final v2 performance profile key rows:

| count | dim | ef | recall@10 | search_avg_us | build_us |
|---:|---:|---:|---:|---:|---:|
| 1000 | 512 | 256 | 1.0000 | 212.66 | 2,084,802 |
| 5000 | 512 | 256 | 1.0000 | 537.82 | 22,510,016 |
| 10000 | 512 | 128 | 0.8840 | 759.66 | 65,851,295 |
| 10000 | 512 | 256 | 0.9860 | 1,068.92 | 65,851,295 |
| 10000 | 512 | 512 | 1.0000 | 1,593.02 | 65,851,295 |

Additional existing v2 gates were also run before the final `needs_rebuild` no-op correction:

```text
tests/vector_core_hnsw_v2_distance_profile_test.sh PASS
tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh PASS
```

The final correction only removed a side effect from `mark_delete`; direct search/rebuild behavior was then revalidated with tombstone, recall, and performance tests.

## Current core_hnsw regression

All passed after final code:

```text
tests/vector_core_hnsw_correctness_test.sh PASS
tests/vector_core_hnsw_rebuild_test.sh PASS
tests/vector_core_hnsw_rdb_reload_test.sh PASS
tests/vector_core_hnsw_aof_replay_test.sh PASS
```

## Runtime behavior unchanged

Confirmed by scope and code review:

- `core_hnsw_v2` is still not registered as a product backend.
- VSEARCH runtime behavior unchanged.
- VINDEX runtime behavior unchanged.
- VDEL runtime behavior unchanged.
- VSEARCH response unchanged.
- RDB/AOF unchanged.
- `kvs_vector_index_ops_t` unchanged.
- current `core_hnsw` unchanged by this stage.

## Boundary checks

All produced no output:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
```

`git diff --check`: PASS.

## Build

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
```

## Stage 9F3 recommendation

Recommended:

```text
Stage 9F3: manager write-path delta capture skeleton
```

Reason:

- 9F1 established manager-owned rebuild context and delta log skeleton.
- 9F2 adds v2 tombstone API needed for future DELETE replay into a shadow index.
- The next narrow step can capture manager-owned VSET/VDEL/TTL deltas into the existing rebuild context skeleton.

Constraints for 9F3:

- do not register v2 backend
- do not change VSEARCH behavior
- do not implement shadow swap
- do not change RDB/AOF
- keep vector_core free of manager semantics

## git status --short

```text
 M AGENTS.md
 M Makefile
 M README.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_index.c
 M src/vector/core/hnsw/kvs_hnsw_info.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_distance_avx2.c
 M tests/vcmd_profile_bench.c
 M tests/vector_core_hnsw_rebuild_profile_test.sh
?? "docs/vector_core_reset/hnsw_v2\345\256\236\346\226\275\346\226\271\346\241\210.md"
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
?? docs/vector_core_reset/stage9e3b_reverse_prune_attribution_rebuild_strategy_gate.md
?? docs/vector_core_reset/stage9e3c_core_hnsw_v2_reverse_prune_score_reuse.md
?? docs/vector_core_reset/stage9f0_core_hnsw_v2_integration_readiness_background_rebuild.md
?? docs/vector_core_reset/stage9f1_manager_owned_rebuild_state_machine_skeleton.md
?? docs/vector_core_reset/stage9f2_core_hnsw_v2_mark_delete_tombstone_gate.md
?? include/vector/kvs_vector_hnsw_v2.h
?? include/vector/kvs_vector_rebuild.h
?? src/vector/core/hnsw_v2/
?? src/vector/kvs_vector_rebuild.c
?? tests/vector_core_hnsw_v2_distance_profile_test.c
?? tests/vector_core_hnsw_v2_distance_profile_test.sh
?? tests/vector_core_hnsw_v2_graph_construction_test.c
?? tests/vector_core_hnsw_v2_graph_construction_test.sh
?? tests/vector_core_hnsw_v2_performance_profile_test.c
?? tests/vector_core_hnsw_v2_performance_profile_test.sh
?? tests/vector_core_hnsw_v2_reverse_prune_profile_test.c
?? tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh
?? tests/vector_core_hnsw_v2_search_recall_test.c
?? tests/vector_core_hnsw_v2_search_recall_test.sh
?? tests/vector_core_hnsw_v2_skeleton_test.c
?? tests/vector_core_hnsw_v2_skeleton_test.sh
?? tests/vector_core_hnsw_v2_tombstone_test.c
?? tests/vector_core_hnsw_v2_tombstone_test.sh
?? tests/vector_rebuild_state_machine_test.c
?? tests/vector_rebuild_state_machine_test.sh
```

Only the files listed in "Modified / added files" are Stage 9F2 changes. Other dirty files were already present from prior stages or unrelated work and were not processed by this stage.
