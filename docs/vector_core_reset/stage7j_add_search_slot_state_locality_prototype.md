# Stage 7J: add-search hot per-slot state locality prototype

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段依据：

- `docs/vector_core_reset/stage7h_level0_neighbor_visited_layout_audit.md`
- `docs/vector_core_reset/stage7i_level0_visited_slot_locality_prototype.md`

本阶段只做独立窄范围 A prototype：co-locate add-time `layer_search` hot per-slot state。未做 broad layout refactor，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未做 upper_neighbors compact，未做 graph persistence / shard / scope-aware search，未降低 `ef_construction` / `ef_search`，未引入 manager 语义字段到 vector_core。

本 prompt 与 `AGENTS.md` 未发现冲突。`README.md` 为既有无关修改，本阶段未处理。

## 2. Modified / added files

Stage 7J 修改文件：

- `include/kvs_vector.h`
- `include/vector/kvs_vector_hnsw.h`
- `src/core/kvs_operation.c`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/core/hnsw/kvs_hnsw_delete.c`
- `src/vector/core/hnsw/kvs_hnsw_index.c`
- `src/vector/kvs_vector.c`
- `tests/vcmd_profile_bench.c`
- `tests/vector_core_hnsw_rebuild_profile_test.sh`

Stage 7J 新增文件：

- `docs/vector_core_reset/stage7j_add_search_slot_state_locality_prototype.md`

工作树中仍保留前序 Stage 7E / 7F / 7G / 7H / 7I 未提交修改、Stage 7G 的 `src/vector/kvs_vector_distance_avx2.c` 修改，以及既有 `README.md` 修改；本阶段未回退或处理这些既有改动。

## 3. Whether production code changed

是。生产代码新增一个 core_hnsw runtime-only A prototype：

```text
kvs_hnsw_slot_hot_state_t:
  uint32_t visited_tag
  uint8_t level
  uint8_t deleted
  uint16_t reserved

kvs_hnsw_index_t.slot_hot_state:
  capacity-sized runtime mirror
```

`kvs_hnsw_search_layer_for_add()` 改为在 add-time layer_search hot path 中读写 `slot_hot_state[slot].visited_tag`，并通过同一结构读取 add-time active-at-level 所需的 `level/deleted` mirror。原 `visited_tags`、`levels`、`deleted_flags` 仍保留，VSEARCH search path 仍使用原数组。

## 4. Current call chain

```text
vcmd_profile_bench ann_core
  -> kvs_vector_restore_record_with_embedding()
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_vector_manager_for_each_active_embedding()
  -> kvs_hnsw_index_add()
  -> kvs_hnsw_search_layer_for_add()
  -> slot_hot_state visited_tag check / mark
  -> slot_hot_state level/deleted check when deleted records exist
```

## 5. A prototype minimal structure

选择 A：add-search hot per-slot state locality。

最小结构只包含 vector_core 可拥有的 index-local add-search state：

- `visited_tag`: add-time `layer_search` visited marker。
- `level`: core_hnsw slot level mirror。
- `deleted`: core_hnsw deleted flag mirror。

没有迁移 manager 字段；没有 scope / TTL / metadata / question / answer；没有改变 record store、response、RDB/AOF 或 index ops。

## 6. Stage 7I B packed row mirror status

Stage 7I B packed row mirror 保留为对照和既有 profiling 基线，未作为正式 refactor 基础：

- `level0_rows` 仍存在。
- `layer_search_level0_legacy_count_read_count` 仍为 0。
- Stage 7J 不继续扩大 B，不把 `level0_rows` 变成正式 graph layout replacement。

原因：Stage 7I 数据显示 B 机械有效但不降低 `level0_us`，且增加约 266,240 bytes packed row mirror 内存。

## 7. New Stage 7J fields

新增字段：

- `layer_search_hot_state_read_count`
- `layer_search_hot_state_write_count`
- `layer_search_hot_active_state_read_count`
- `layer_search_legacy_visited_tag_read_count`
- `layer_search_legacy_visited_tag_write_count`
- `layer_search_legacy_active_state_read_count`
- `core_hnsw_memory_slot_hot_state_bytes`

保留 Stage 7H / 7I fields，包括：

- `layer_search_level0_neighbor_scan_count`
- `layer_search_level0_repeated_visit_count`
- `layer_search_level0_packed_row_read_count`
- `layer_search_level0_legacy_count_read_count`
- `core_hnsw_memory_slot_state_bytes`

## 8. Benchmark result

Before values are Stage 7I final values. After values are Stage 7J runs after clean rebuild.

| dim | before level0_us | after level0_us | before repeated | after repeated | before memory_bytes | after memory_bytes | recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 69,571 | 80,990 | 6,191,098 | 6,191,098 | 6,716,624 | 6,716,624 | 1.0000 |
| 512 | 76,757 | 80,331 | 6,140,460 | 6,140,460 | 7,240,912 | 7,240,912 | 1.0000 |
| 768 | 89,853 | 94,242 | 6,050,166 | 6,050,166 | 8,289,488 | 8,289,488 | 1.0000 |
| 1024 | 104,727 | 147,866 | 6,019,210 | 6,019,210 | 9,338,064 | 9,338,064 | 1.0000 |

Stage 7J detail:

| dim | layer_search_us | level0_us | distance_compute | level0_scan | repeated | hot_state_read | hot_state_write | legacy_visited_read | legacy_visited_write | slot_hot_state_bytes | slot_state_bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 81,182 | 80,990 | 499,711 | 6,689,215 | 6,191,098 | 6,702,263 | 499,711 | 0 | 0 | 8,192 | 400,384 |
| 512 | 80,558 | 80,331 | 499,900 | 6,638,541 | 6,140,460 | 6,658,661 | 499,900 | 0 | 0 | 8,192 | 400,384 |
| 768 | 94,388 | 94,242 | 499,513 | 6,548,273 | 6,050,166 | 6,555,581 | 499,513 | 0 | 0 | 8,192 | 400,384 |
| 1024 | 152,181 | 147,866 | 499,762 | 6,517,304 | 6,019,210 | 6,532,578 | 499,762 | 0 | 0 | 8,192 | 400,384 |

## 9. Interpretation

The A prototype worked mechanically:

- add-time `layer_search` visited checks moved from `visited_tags` to `slot_hot_state`.
- `layer_search_legacy_visited_tag_read_count` and `layer_search_legacy_visited_tag_write_count` are 0.
- Search volume did not change: distance compute, level0 scan, repeated visit, and recall are equivalent to Stage 7I.
- Recall@10 stayed 1.0000 for 384/512/768/1024d.

It did not produce a runtime win:

- `level0_us` did not decline versus Stage 7I.
- Repeated visit pressure is unchanged because this prototype does not reduce duplicate neighbor scans.
- It adds 8,192 bytes for the 1K benchmark due to `slot_hot_state`.

Conclusion: this narrow A prototype does not justify entering a formal A-style layout refactor.

## 10. Safety invariants

- No RDB/AOF format change.
- No `kvs_vector_index_ops_t` change.
- No VSEARCH response change.
- No `ef_construction` / `ef_search` change.
- No graph persistence.
- No manager semantic fields in vector_core.
- Recall@10 did not regress.
- `slot_hot_state` is runtime rebuild-derived state and is not persisted.

## 11. Correctness tests

Passed:

```bash
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
tests/vector_core_hnsw_rebuild_profile_test.sh
```

## 12. Static boundary check result

Commands:

```bash
grep -RIn "g_vector_state" src/vector/core || true
grep -RIn "record_id_map" src/vector/core || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core || true
git diff --check
```

Result: no output from boundary grep; `git diff --check` passed.

## 13. Build result

Passed:

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

## 14. git status --short

```text
 M README.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_delete.c
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
```

## 15. Next suggested step

Do not enter a formal A-style layout refactor from Stage 7J data. Both narrow layout prototypes have now failed to show a stable `level0_us` win:

- B removes legacy count reads but does not improve runtime and adds ~266 KB at 1K.
- A removes add-time legacy visited_tag hot-path reads but does not improve runtime and adds 8 KB at 1K.

Based only on Stage 7J data, stop layout work for now. If optimization continues, it should not be a layout refactor unless a new audit identifies a different, measurable bottleneck.
