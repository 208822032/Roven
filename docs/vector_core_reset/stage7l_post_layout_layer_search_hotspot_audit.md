# Stage 7L: post-layout remaining layer_search hotspot audit

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段依据：

- `docs/vector_core_reset/stage7k_remove_failed_layout_prototypes.md`
- `docs/vector_core_reset/stage7h_level0_neighbor_visited_layout_audit.md`
- `docs/vector_core_reset/stage7g_layer_search_distance_alignment_optimization.md`

本阶段只做 profiling / audit / attribution。未做 optimization，未做 layout prototype，未恢复 Stage 7I/7J failed prototype，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未做 upper_neighbors compact，未做 graph persistence / shard / scope-aware search，未降低 `ef_construction` / `ef_search`，未引入 manager 语义字段到 vector_core。

本 prompt 与 `AGENTS.md` 未发现冲突。

## 2. Modified / added files

Stage 7L 修改文件：

- `include/kvs_vector.h`
- `include/vector/kvs_vector_hnsw.h`
- `src/core/kvs_operation.c`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/kvs_vector.c`
- `tests/vcmd_profile_bench.c`
- `tests/vector_core_hnsw_rebuild_profile_test.sh`

Stage 7L 新增文件：

- `docs/vector_core_reset/stage7l_post_layout_layer_search_hotspot_audit.md`

工作树中仍保留前序 Stage 7E / 7F / 7G / 7H / 7I / 7J / 7K 未提交改动和既有 `README.md` 修改；本阶段未处理这些无关修改。

## 3. Whether production code changed

是，但只增加 runtime profiling counters 和 VINFO/benchmark 透出字段。

未改变 HNSW 搜索行为、visited 写入规则、heap 决策、distance compute 条件、邻居扫描顺序、搜索参数、RDB/AOF 或 VSEARCH response。

## 4. Current call chain

```text
vcmd_profile_bench ann_core
  -> kvs_vector_restore_record_with_embedding()
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_vector_manager_for_each_active_embedding()
  -> kvs_hnsw_index_add()
  -> kvs_hnsw_search_layer_for_add()
  -> level0 row expansion / visited check / distance compute / heap update
  -> kvs_vector_info()
  -> VINFO / vcmd_profile_bench output
```

## 5. New Stage 7L profiling fields

新增字段：

- `layer_search_level0_candidate_row_expansion_count`
- `layer_search_level0_distance_compute_count`
- `layer_search_upper_distance_compute_count`
- `layer_search_level0_deleted_active_check_count`
- `layer_search_upper_deleted_active_check_count`
- `layer_search_candidate_heap_skip_count`
- `layer_search_candidate_heap_full_skip_count`
- `layer_search_top_heap_skip_count`
- `layer_search_level0_candidate_heap_push_count`
- `layer_search_level0_candidate_heap_pop_count`
- `layer_search_level0_candidate_heap_skip_count`
- `layer_search_level0_top_heap_push_count`
- `layer_search_level0_top_heap_replace_count`
- `layer_search_level0_top_heap_skip_count`
- `layer_search_level0_lower_bound_update_count`

保留 Stage 7E / 7G / 7H 字段，并继续通过 `tests/vector_core_hnsw_rebuild_profile_test.sh` 做 machine-check。

## 6. Benchmark result

Commands:

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 768 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 1024 10 50
```

| dim | hnsw_add_layer_search_us | level0_us | distance_compute | level0_distance | level0_scan | level0_unique | level0_repeated | row_expansions | recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 69,560 | 69,380 | 499,711 | 499,116 | 6,689,215 | 499,116 | 6,191,098 | 107,612 | 1.0000 |
| 512 | 76,705 | 76,473 | 499,900 | 499,080 | 6,638,541 | 499,080 | 6,140,460 | 106,744 | 1.0000 |
| 768 | 90,478 | 90,331 | 499,513 | 499,106 | 6,548,273 | 499,106 | 6,050,166 | 105,243 | 1.0000 |
| 1024 | 104,012 | 103,775 | 499,762 | 499,093 | 6,517,304 | 499,093 | 6,019,210 | 104,751 | 1.0000 |

Heap attribution:

| dim | level0_top_push | level0_top_replace | level0_top_skip | level0_candidate_push | level0_candidate_pop | level0_candidate_skip | lower_bound_update |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 119,744 | 130,824 | 248,548 | 233,663 | 108,483 | 265,453 | 137,172 |
| 512 | 119,744 | 133,892 | 245,444 | 235,105 | 107,615 | 263,975 | 140,198 |
| 768 | 119,744 | 137,402 | 241,960 | 236,221 | 106,114 | 262,885 | 143,583 |
| 1024 | 119,744 | 138,529 | 240,820 | 236,730 | 105,622 | 262,363 | 144,786 |

Derived ratios:

| dim | entries / row expansion | repeated skip ratio | unique scan ratio | unique-to-distance | top skip / distance | candidate skip / distance |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 62.16 | 92.55% | 7.46% | 1.00 | 49.80% | 53.18% |
| 512 | 62.19 | 92.50% | 7.52% | 1.00 | 49.18% | 52.89% |
| 768 | 62.22 | 92.39% | 7.62% | 1.00 | 48.48% | 52.67% |
| 1024 | 62.22 | 92.36% | 7.66% | 1.00 | 48.25% | 52.57% |

Memory / active check:

| dim | backend_memory_bytes | slot_state_bytes | level0_active_check | upper_active_check |
| --- | ---: | ---: | ---: | ---: |
| 384 | 6,450,384 | 125,952 | 0 | 0 |
| 512 | 6,974,672 | 125,952 | 0 | 0 |
| 768 | 8,023,248 | 125,952 | 0 | 0 |
| 1024 | 9,071,824 | 125,952 | 0 | 0 |

## 7. Hotspot attribution

Observed remaining `layer_search` cost is almost entirely level0:

- `level0_us` is 99.7%+ of `hnsw_add_layer_search_us`.
- `upper_distance_compute_count` is only 407 to 820 per run; level0 distance compute is about 499K per run.
- `level0_unique_visit_count` equals `level0_distance_compute_count`, so distance work is tied to unique visits, not extra recomputation.
- `level0_scan` is 6.5M to 6.7M entries, with 92.36% to 92.55% repeated visited skips.
- Each level0 candidate row expansion scans about 62.2 entries, matching the saturated level0 count distribution from Stage 7H.
- Heap operations are non-trivial but smaller than scan pressure: about 120K top pushes, 131K to 139K top replaces, 241K to 249K top skips, and 262K to 265K candidate skips.
- Active/deleted/level checks are not a current bottleneck in these runs because `deleted_count == 0`; active check counters are 0.

Conclusion: remaining level0 cost is attributable to two measurable sources:

1. repeated visited skip loop over dense level0 rows, about 6.0M to 6.2M repeated entries per 1K rebuild;
2. unique visit distance compute, about 499K AVX2 distance computations, scaling with dimension.

Heap/candidate management is visible, but current counters do not show it dominating the remaining cost. Active/deleted checks are not relevant for this no-delete workload.

## 8. Safety invariants

- HNSW behavior unchanged.
- No optimization or refactor was implemented.
- No Stage 7I/7J prototype was restored.
- `ef_construction` / `ef_search` unchanged.
- RDB/AOF compatibility unchanged.
- `kvs_vector_index_ops_t` unchanged.
- VSEARCH response unchanged.
- vector_core still owns only record_id / vector / score / index-local graph/search state and runtime stats.

## 9. Correctness tests

Passed:

```bash
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
tests/vector_core_hnsw_rebuild_profile_test.sh
```

Recall@10 did not regress:

| dim | recall@10 |
| --- | ---: |
| 384 | 1.0000 |
| 512 | 1.0000 |
| 768 | 1.0000 |
| 1024 | 1.0000 |

## 10. Static boundary check result

Commands:

```bash
grep -RIn "level0_rows\|level0_row_stride\|packed_row\|slot_hot_state\|kvs_hnsw_slot_hot_state_t" include src tests || true
grep -RIn "g_vector_state" src/vector/core || true
grep -RIn "record_id_map" src/vector/core || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core || true
git diff --check
```

Result:

- failed prototype symbol grep: no output.
- boundary greps: no output.
- `git diff --check`: passed.

## 11. Build result

Passed:

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

After renaming benchmark output from Stage 7F to Stage 7L, an incremental rebuild also passed.

## 12. git status --short

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
```

## 13. Next suggested step

Do not restore layout work and do not enter a layout refactor from this stage.

Based only on Stage 7L data, valid next candidates are:

1. profiling/audit of repeated visited skip loop control flow, because repeated entries are 92.36% to 92.55% of level0 scans;
2. profiling/audit of remaining unique-visit distance compute cost after Stage 7G, because level0 distance compute remains about 499K and scales with dimension;
3. profiling/audit of heap/candidate management only if a lower-overhead timing split proves heap operations are a meaningful fraction of `level0_us`;
4. stop optimization if the remaining rebuild cost is acceptable.

Any next stage must start as profiling/audit unless it chooses one of these quantified sources and defines a narrow optimization boundary. Do not directly resume layout prototypes or broad refactor.
