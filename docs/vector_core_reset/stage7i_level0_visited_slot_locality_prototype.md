# Stage 7I: level0 visited/per-slot locality design prototype

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段依据 `docs/vector_core_reset/stage7h_level0_neighbor_visited_layout_audit.md`，只做最小 layout design/prototype 验证。未做 broad layout refactor，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未做 upper_neighbors compact，未做 graph persistence / shard / scope-aware search，未降低 `ef_construction` / `ef_search`，未引入 manager 语义字段到 vector_core。

本 prompt 与 `AGENTS.md` 未发现冲突。`README.md` 为既有无关修改，本阶段未处理。

## 2. Modified / added files

Stage 7I 修改文件：

- `include/kvs_vector.h`
- `include/vector/kvs_vector_hnsw.h`
- `src/core/kvs_operation.c`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/core/hnsw/kvs_hnsw_index.c`
- `src/vector/core/hnsw/kvs_hnsw_info.c`
- `src/vector/kvs_vector.c`
- `tests/vcmd_profile_bench.c`
- `tests/vector_core_hnsw_rebuild_profile_test.sh`

Stage 7I 新增文件：

- `docs/vector_core_reset/stage7i_level0_visited_slot_locality_prototype.md`

工作树中仍保留前序 Stage 7E / 7F / 7G / 7H 未提交修改和既有 `README.md` 修改；本阶段未回退或处理这些既有改动。

## 3. Whether production code changed

是。生产代码增加了一个 core_hnsw 内存态 prototype：

- 新增 level0 packed row 镜像：每行包含 `count` header 和该行 `M0` 个 neighbor。
- `kvs_hnsw_search_layer_for_add()` 的 level0 row 扫描从 packed row 一次读取 count + neighbor base。
- 旧 `level0_neighbors` / `level0_counts` 仍保留为 graph 存储，连接/删除/重置路径同步 packed row 镜像。
- VSEARCH 路径仍使用旧 graph 存储，RDB/AOF 仍只保存记录并 rebuild，不保存 graph 或 packed row。

未改变 HNSW 搜索参数、候选 heap 规则、距离计算、graph 持久化语义或 VSEARCH response。

## 4. Current call chain

```text
vcmd_profile_bench ann_core
  -> kvs_vector_restore_record_with_embedding()
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_vector_manager_for_each_active_embedding()
  -> kvs_hnsw_index_add()
  -> kvs_hnsw_search_layer_for_add()
  -> level0 packed row scan
  -> kvs_vector_info()
  -> kvs_vector_core_hnsw_ops_info()
```

## 5. Prototype choice

选择 B：optimize level0 row header/count access。

原因：

- Stage 7H 显示 row 内扫描基本顺序，row 访问随机，且每次 row 展开前需要独立读取 `level0_counts` 和 neighbor row base。
- B 可以用最小镜像结构验证 count + row header locality，不需要合并 `visited_tags / levels / deleted_flags`，风险比 A 小。
- 当前 rebuild benchmark 没有删除记录，`deleted_flags`/active-at-level 合并在本数据集上难以证明收益；因此暂不选择 A。

## 6. Layout / data structure changes

新增：

```text
kvs_hnsw_index_t.level0_rows
kvs_hnsw_index_t.level0_row_stride
kvs_hnsw_level0_row_t:
  uint16_t count
  uint16_t reserved
  uint32_t neighbors[M0] stored after header by dynamic stride
```

保持不变：

- `level0_neighbors`
- `level0_counts`
- `level0_neighbor_scores`
- `visited_tags`
- `levels`
- `deleted_flags`
- `upper_neighbors`

设计含义：这是一个 add-search hot-path prototype mirror，不是正式 graph layout replacement。

## 7. New Stage 7I fields

新增 profiling / memory fields：

- `layer_search_level0_packed_row_read_count`
- `layer_search_level0_legacy_count_read_count`
- `core_hnsw_memory_level0_packed_rows_bytes`

并保留 Stage 7H fields：

- `layer_search_level0_neighbor_scan_count`
- `layer_search_level0_unique_visit_count`
- `layer_search_level0_repeated_visit_count`
- `layer_search_level0_slot_state_touch_count`
- `core_hnsw_memory_slot_state_bytes`

## 8. Benchmark result

Commands:

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 768 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 1024 10 50
```

Before values are from Stage 7H. After values are from final Stage 7I runs after clean rebuild.

| dim | before level0_us | after level0_us | before repeated | after repeated | before memory_bytes | after memory_bytes | recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 68,969 | 69,571 | 6,191,098 | 6,191,098 | 6,450,384 | 6,716,624 | 1.0000 |
| 512 | 75,027 | 76,757 | 6,140,460 | 6,140,460 | 6,974,672 | 7,240,912 | 1.0000 |
| 768 | 88,470 | 89,853 | 6,050,166 | 6,050,166 | 8,023,248 | 8,289,488 | 1.0000 |
| 1024 | 102,089 | 104,727 | 6,019,210 | 6,019,210 | 9,071,824 | 9,338,064 | 1.0000 |

Stage 7I detail:

| dim | layer_search_us | level0_us | distance_compute | level0_scan | unique | repeated | slot_state_touch | packed_row_read | legacy_count_read | packed_rows_bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 69,740 | 69,571 | 499,711 | 6,689,215 | 499,116 | 6,191,098 | 7,188,331 | 107,612 | 0 | 266,240 |
| 512 | 77,006 | 76,757 | 499,900 | 6,638,541 | 499,080 | 6,140,460 | 7,137,621 | 106,744 | 0 | 266,240 |
| 768 | 90,012 | 89,853 | 499,513 | 6,548,273 | 499,106 | 6,050,166 | 7,047,379 | 105,243 | 0 | 266,240 |
| 1024 | 104,965 | 104,727 | 499,762 | 6,517,304 | 499,093 | 6,019,210 | 7,016,397 | 104,751 | 0 | 266,240 |

## 9. Interpretation

The B prototype worked mechanically:

- `layer_search_level0_packed_row_read_count` equals `layer_search_level0_neighbor_row_access_count`.
- `layer_search_level0_legacy_count_read_count` is 0 in all high-dim runs.
- Search volume is unchanged: distance compute, scan, unique, repeated, and recall remain equivalent to Stage 7H.

However it did not produce a stable runtime win:

- `level0_us` did not decline versus Stage 7H.
- Repeated visit pressure is unchanged because this prototype does not reduce duplicate neighbor scans or visited skip pressure.
- Memory increases by 266,240 bytes for the 1K high-dim runs due to the packed row mirror.

Conclusion: B by itself is not a good candidate for formal layout refactor.

## 10. Safety invariants

- No RDB/AOF format change.
- No `kvs_vector_index_ops_t` change.
- No VSEARCH response change.
- No `ef_construction` / `ef_search` change.
- No graph persistence.
- No manager semantic fields in vector_core.
- Recall@10 did not regress.
- Packed rows are runtime rebuild-derived state and are not persisted.

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
```

## 15. Next suggested step

Based only on Stage 7I data, do not enter a formal B-style level0 row header/count layout refactor. It adds memory and removes legacy count reads, but does not reduce `level0_us` or repeated visit pressure.

If continuing, the minimum next boundary is either:

1. stop layout work for now, or
2. run a separate narrow A prototype focused only on add-search hot per-slot state locality (`visited_tag` plus core-local level/deleted state), with the same requirement that recall and search volume remain unchanged.

Do not start a broad layout refactor from Stage 7I data.
