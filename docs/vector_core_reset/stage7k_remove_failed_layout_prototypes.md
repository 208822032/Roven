# Stage 7K: remove failed layout prototypes / checkpoint cleanup

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段依据：

- `docs/vector_core_reset/stage7i_level0_visited_slot_locality_prototype.md`
- `docs/vector_core_reset/stage7j_add_search_slot_state_locality_prototype.md`

本阶段只清理 Stage 7I/7J 失败 layout prototype 的生产代码和运行时内存开销。未处理既有 `README.md` 修改，未删除 Stage 7I / Stage 7J 报告，未回退 Stage 7E neighbor_select score reuse，未回退 Stage 7G distance/SIMD/alignment optimization，保留 Stage 7H 基础 audit counters。

未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未做新的 layout prototype，未做 broad layout refactor，未做 upper_neighbors compact，未做 graph persistence / shard / scope-aware search，未降低 `ef_construction` / `ef_search`，未引入 manager 语义字段到 vector_core。

本 prompt 与 `AGENTS.md` 未发现冲突。

## 2. Modified / added files

Stage 7K 修改文件：

- `include/kvs_vector.h`
- `include/vector/kvs_vector_hnsw.h`
- `src/core/kvs_operation.c`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/core/hnsw/kvs_hnsw_index.c`
- `src/vector/core/hnsw/kvs_hnsw_info.c`
- `src/vector/kvs_vector.c`
- `tests/vcmd_profile_bench.c`
- `tests/vector_core_hnsw_rebuild_profile_test.sh`

Stage 7K 新增文件：

- `docs/vector_core_reset/stage7k_remove_failed_layout_prototypes.md`

工作树中仍保留前序 Stage 7E / 7F / 7G / 7H / 7I / 7J 未提交改动和既有 `README.md` 修改；本阶段未处理这些无关修改。

## 3. Whether production code changed

是。生产代码移除了 Stage 7I/7J failed prototype runtime mirrors 和相关 stats 字段。

生产代码保留：

- Stage 7E `level0_neighbor_scores` / worst cache score reuse。
- Stage 7G layer_search distance/SIMD/call-overhead optimization。
- Stage 7H layer_search / level0 / visited_tags 基础 audit counters 和 memory breakdown。

## 4. Current call chain

```text
vcmd_profile_bench ann_core
  -> kvs_vector_restore_record_with_embedding()
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_vector_manager_for_each_active_embedding()
  -> kvs_hnsw_index_add()
  -> kvs_hnsw_search_layer_for_add()
  -> level0 scan via level0_neighbors + level0_counts
  -> visited checks via visited_tags
  -> kvs_vector_info()
  -> kvs_vector_core_hnsw_ops_info()
```

## 5. Before behavior

Stage 7I added B prototype:

- `level0_rows`
- `level0_row_stride`
- packed row allocation / reset / reserve / sync / scan path
- `layer_search_level0_packed_row_read_count`
- `layer_search_level0_legacy_count_read_count`
- `core_hnsw_memory_level0_packed_rows_bytes`

Stage 7J added A prototype:

- `slot_hot_state`
- `kvs_hnsw_slot_hot_state_t`
- slot hot state allocation / reset / reserve / delete sync
- add-time layer_search `slot_hot_state` visited path
- `layer_search_hot_state_*`
- `layer_search_legacy_visited_tag_*`
- `layer_search_legacy_active_state_*`
- `core_hnsw_memory_slot_hot_state_bytes`

Both prototypes were mechanically valid but did not lower `level0_us`.

## 6. After design

Removed Stage 7I B prototype:

- Removed packed row struct / fields / allocation / copy / reset / free.
- Removed packed row sync from link, rollback, remove, and clear paths.
- Restored add-time level0 scan to read `level0_neighbors` and `level0_counts`.
- Removed packed row profiling and memory fields from `kvs_vector_info_t`, `kvs_hnsw_rebuild_profile_t`, VINFO, benchmark output, and rebuild profile test.

Removed Stage 7J A prototype:

- Removed `kvs_hnsw_slot_hot_state_t` and `kvs_hnsw_index_t.slot_hot_state`.
- Removed allocation / copy / reset / free / delete sync.
- Restored add-time layer_search visited checks to `visited_tags`.
- Restored active-at-level reads to `levels` + `deleted_flags`.
- Removed hot-state / legacy comparison fields and memory field from public info, internal profile, VINFO, benchmark output, and rebuild profile test.

## 7. Safety invariants

- RDB/AOF compatibility unchanged; core_hnsw graph remains rebuild-derived state.
- `kvs_vector_index_ops_t` unchanged.
- VSEARCH response unchanged.
- VINFO array count was adjusted to match the removed fields.
- `ef_construction` / `ef_search` unchanged.
- No manager semantic fields in vector_core.
- Recall@10 did not regress in cleanup runs.

## 8. Benchmark result

Commands:

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 768 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 1024 10 50
```

Cleanup after values:

| dim | hnsw_add_layer_search_us | layer_search_level0_us | distance_compute | repeated_visit | memory_bytes | recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 68,306 | 68,134 | 499,711 | 6,203,585 | 6,450,384 | 1.0000 |
| 512 | 76,355 | 76,136 | 499,900 | 6,159,800 | 6,974,672 | 1.0000 |
| 768 | 88,680 | 88,522 | 499,513 | 6,057,096 | 8,023,248 | 1.0000 |
| 1024 | 104,204 | 103,969 | 499,762 | 6,033,853 | 9,071,824 | 1.0000 |

Memory cleanup versus failed prototypes:

- Removed Stage 7I packed row mirror memory: about 266,240 bytes in 1K runs.
- Removed Stage 7J slot hot state mirror memory: about 8,192 bytes in 1K runs.
- `core_hnsw_memory_slot_state_bytes` after cleanup: 125,952 bytes in all four 1K high-dim runs.

## 9. Preserved Stage 7H fields

Confirmed still available in benchmark / VINFO profile path:

- `layer_search_level0_us`
- `layer_search_level0_neighbor_scan_count`
- `layer_search_level0_unique_visit_count`
- `layer_search_level0_repeated_visit_count`
- `layer_search_level0_visited_tag_access_count`
- `layer_search_level0_slot_state_touch_count`
- `layer_search_level0_neighbor_row_access_count`
- `layer_search_level0_neighbor_row_sequential_count`
- `layer_search_level0_neighbor_row_random_count`
- `layer_search_level0_neighbor_slot_sequential_count`
- `layer_search_level0_neighbor_slot_random_count`
- `core_hnsw_level0_count_min / avg / p50 / p95 / max`
- non-prototype memory breakdown fields

## 10. Correctness tests

Passed:

```bash
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
tests/vector_core_hnsw_rebuild_profile_test.sh
```

## 11. Static boundary check result

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
- boundary grep: no output.
- `git diff --check`: passed.

## 12. Build result

Passed:

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

An intermediate test run exposed a VINFO RESP array count mismatch after removing fields. The array count was corrected from 104 to 106 core_hnsw conditional fields, and HNSW tests then passed.

## 13. git status --short

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
```

## 14. Next suggested step

Stop layout work. Stage 7I and Stage 7J data do not justify formal A-style or B-style layout refactor.

If optimization continues, it must begin with a new profiling/audit that identifies a different measurable bottleneck. Do not directly start a layout refactor from Stage 7I/7J data.
