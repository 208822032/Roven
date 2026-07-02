# Stage 7H: level0 neighbor access / visited_tags layout audit

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段依据 `docs/vector_core_reset/stage7g_layer_search_distance_alignment_optimization.md`，只做 level0 neighbor access / visited_tags profiling-layout audit。未做 layout refactor，未合并 visited_tags / levels / deleted_flags，未做 upper_neighbors compact，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未做 graph persistence / shard / scope-aware search，未降低 `ef_construction` / `ef_search`。

本 prompt 与 `AGENTS.md` 未发现冲突。静态边界检查曾提示 core 中不能出现 manager 业务 `metadata` 词汇；本阶段已将审计字段命名调整为 `slot_state_*`，保持 vector_core 边界。

## 2. Modified / added files

Stage 7H 修改文件：

- `include/kvs_vector.h`
- `include/vector/kvs_vector_hnsw.h`
- `src/core/kvs_operation.c`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/kvs_vector.c`
- `tests/vcmd_profile_bench.c`
- `tests/vector_core_hnsw_rebuild_profile_test.sh`

Stage 7H 新增文件：

- `docs/vector_core_reset/stage7h_level0_neighbor_visited_layout_audit.md`

工作树中仍保留前序 Stage 7E / 7F / 7G 未提交修改和既有 `README.md` 修改；本阶段未回退或处理这些既有改动。

## 3. Whether production code changed

是，但仅为 profiling / audit / stats 透出：

- 在 add-time `layer_search` 热路径增加整数计数器。
- 在 `kvs_vector_info()` 汇总 core_hnsw layout 只读分布和内存拆分。
- 在 VINFO / benchmark / profile test 中透出审计字段。

未改变 HNSW 搜索行为、候选 heap 规则、邻接表结构、距离计算、搜索参数、RDB/AOF 或 VSEARCH response。

## 4. Current call chain

```text
vcmd_profile_bench ann_core
  -> kvs_vector_restore_record_with_embedding()
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_vector_manager_for_each_active_embedding()
  -> kvs_hnsw_index_add()
  -> kvs_hnsw_search_layer_for_add()
  -> level0 neighbor row scan / visited_tags check / heap update
  -> kvs_vector_info()
  -> kvs_vector_core_hnsw_ops_info()
```

## 5. Current level0 / visited_tags / slot-state layout

Current core_hnsw layout:

- `vectors`: contiguous `capacity * dim` float array, slot-major.
- `levels`: separate `capacity` byte array.
- `deleted_flags`: separate `capacity` byte array.
- `visited_tags`: separate `capacity` uint32 array.
- `level0_neighbors`: contiguous fixed-width rows, `capacity * M0` uint32.
- `level0_neighbor_counts`: `level0_counts`, separate `capacity` uint16 array.
- `level0_neighbor_scores`: contiguous fixed-width rows, `capacity * M0` float.
- `upper_neighbors`: contiguous fixed-width allocation, `capacity * KVS_HNSW_MAX_LEVEL * M` uint32.
- `upper_counts`: separate `capacity * KVS_HNSW_MAX_LEVEL` uint16 array.
- search context: candidate heap, top heap, neighbor buffer, distance buffer, hits buffer in per-context allocations.

Interpretation: row-local level0 neighbor scans are sequential inside each row, but row selection follows HNSW candidate expansion order and is effectively random over slots. `visited_tags`, vector rows, level/count/deleted state, and level0 neighbor rows are separate arrays, so each unique neighbor can touch several independent cache lines.

## 6. New Stage 7H profiling fields

Added runtime counters:

- `layer_search_unique_visit_count`
- `layer_search_repeated_visit_count`
- `layer_search_visited_tag_access_count`
- `layer_search_slot_state_touch_count`
- `layer_search_level0_unique_visit_count`
- `layer_search_level0_repeated_visit_count`
- `layer_search_level0_visited_tag_access_count`
- `layer_search_level0_slot_state_touch_count`
- `layer_search_level0_neighbor_row_access_count`
- `layer_search_level0_neighbor_row_sequential_count`
- `layer_search_level0_neighbor_row_random_count`
- `layer_search_level0_neighbor_slot_sequential_count`
- `layer_search_level0_neighbor_slot_random_count`

Added layout summary fields:

- `core_hnsw_level0_count_min / avg / p50 / p95 / max`
- `core_hnsw_memory_vectors_bytes`
- `core_hnsw_memory_level0_neighbors_bytes`
- `core_hnsw_memory_level0_scores_bytes`
- `core_hnsw_memory_upper_neighbors_bytes`
- `core_hnsw_memory_visited_tags_bytes`
- `core_hnsw_memory_slot_state_bytes`
- `core_hnsw_memory_context_bytes`

## 7. Benchmark result

Commands:

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 768 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 1024 10 50
```

| dim | rebuild_core_add_total_us | hnsw_add_layer_search_us | layer_search_level0_us | distance_compute | level0_scan | level0_unique | level0_repeated | visited_skip_ratio | row_random_ratio | row-inner_sequential_ratio | recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 82,319 | 69,139 | 68,969 | 499,711 | 6,689,215 | 499,116 | 6,191,098 | 92.55% | 99.66% | 98.39% | 1.0000 |
| 512 | 89,196 | 75,273 | 75,027 | 499,900 | 6,638,541 | 499,080 | 6,140,460 | 92.50% | 99.64% | 98.39% | 1.0000 |
| 768 | 102,901 | 88,607 | 88,470 | 499,513 | 6,548,273 | 499,106 | 6,050,166 | 92.39% | 99.65% | 98.39% | 1.0000 |
| 1024 | 118,241 | 102,330 | 102,089 | 499,762 | 6,517,304 | 499,093 | 6,019,210 | 92.36% | 99.67% | 98.39% | 1.0000 |

## 8. Neighbor distribution

| dim | level0_count_min | avg | p50 | p95 | max |
| --- | ---: | ---: | ---: | ---: | ---: |
| 384 | 43 | 62 | 64 | 64 | 64 |
| 512 | 48 | 62 | 64 | 64 | 64 |
| 768 | 47 | 62 | 64 | 64 | 64 |
| 1024 | 47 | 62 | 64 | 64 | 64 |

Most level0 rows are full or near full. The current workload therefore scans mostly fixed-width dense rows, not sparse adjacency lists.

## 9. Memory breakdown

| dim | backend_memory_bytes | vectors | level0_neighbors | level0_scores | upper_neighbors | visited_tags | slot_state | search_context |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 6,450,384 | 1,572,864 | 262,144 | 262,144 | 4,194,304 | 4,096 | 125,952 | 28,880 |
| 512 | 6,974,672 | 2,097,152 | 262,144 | 262,144 | 4,194,304 | 4,096 | 125,952 | 28,880 |
| 768 | 8,023,248 | 3,145,728 | 262,144 | 262,144 | 4,194,304 | 4,096 | 125,952 | 28,880 |
| 1024 | 9,071,824 | 4,194,304 | 262,144 | 262,144 | 4,194,304 | 4,096 | 125,952 | 28,880 |

Memory implication: for 1K high-dim runs, vectors and preallocated upper_neighbors dominate bytes. However Stage 7H timing attribution remains level0 layer_search; the upper_neighbors byte footprint is not evidence that upper layer_search is the runtime hotspot.

## 10. Audit conclusion

Current layout is partly reasonable and partly inefficient:

- Reasonable: each level0 neighbor row is fixed-width and row-internal scans are sequential; `row-inner_sequential_ratio` is about 98.39%.
- Inefficient: row access order is random over slots; `row_random_ratio` is about 99.6% across all dimensions.
- Inefficient: repeated visit pressure is high; about 92.4% to 92.6% of level0 scanned neighbor entries are already visited.
- Inefficient: per unique visit still touches separate arrays for visited tag, slot state, vector row, and heap state.
- Degree distribution is saturated: p50/p95/max are all 64, so reducing wasted repeated scans or improving row/visited locality is more plausible than optimizing sparse-list handling.

This supports entering a layout/design stage, but not a broad refactor without a narrow target.

## 11. Minimal next-stage refactor candidate

If continuing based on Stage 7H data, the minimum viable next step should be a scoped layout design/prototype for level0 search locality, choosing one target:

1. `visited_tags` / per-slot state locality: pack or co-locate only add-search hot per-slot state needed by level0 (`visited_tag`, level, deleted flag or equivalent active-at-level data), while preserving manager/core boundaries and RDB/AOF behavior.
2. level0 neighbor access/layout: keep row semantics but audit whether level0 counts + neighbor row header can be accessed with fewer independent loads before scanning the row.
3. stop heap/candidate management changes for now unless a follow-up profile shows heap operations dominate after locality work.

Do not directly implement a full layout refactor from this report alone. A next stage should first define the smallest data structure change and prove equivalence against recall and graph behavior.

## 12. Safety invariants

- No HNSW behavior change.
- No `ef_construction` / `ef_search` change.
- No graph persistence or serialization change.
- No RDB/AOF format change.
- No VSEARCH response change.
- No `kvs_vector_index_ops_t` change.
- vector_core still owns only record_id / vector / score / index-local graph/search state and runtime stats.

## 13. Correctness tests

Passed:

```bash
tests/vector_core_hnsw_rebuild_profile_test.sh
```

The Stage 7H prompt only required the profile test for this stage. Recall@10 was also checked by the four benchmark runs and remained 1.0000 for 384/512/768/1024d.

## 14. Static boundary check result

Commands:

```bash
grep -RIn "g_vector_state" src/vector/core || true
grep -RIn "record_id_map" src/vector/core || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core || true
git diff --check
```

Result: no output; `git diff --check` passed.

## 15. Build result

Passed:

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

## 16. git status --short

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
```

## 17. Next suggested step

Based only on Stage 7H data, the next step should be a narrow layout design/prototype audit for level0 locality, focused first on `visited_tags` / per-slot state locality or level0 row header/count access. Do not directly start a broad layout refactor; require a scoped design with behavior equivalence and recall validation before implementation.
