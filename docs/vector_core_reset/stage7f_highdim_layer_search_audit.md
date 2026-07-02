# Stage 7F: high-dim layer_search profiling / attribution audit

## 1. AGENTS.md 遵守情况

已读取并遵守项目根目录 `AGENTS.md`。

本阶段依据 `docs/vector_core_reset/stage7e_neighbor_select_score_reuse_optimization.md`，只对 high-dim `kvs_hnsw_search_layer_for_add()` 增加 profiling / audit / attribution 字段。未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未做算法优化，未做 memory layout 重构，未做 metadata compact，未做 upper_neighbors compact，未降低 `ef_construction` / `ef_search`。

本 prompt 与 `AGENTS.md` 未发现冲突。

## 2. 修改 / 新增文件

Stage 7F 修改文件：

- `include/vector/kvs_vector_hnsw.h`
- `include/kvs_vector.h`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/kvs_vector.c`
- `src/core/kvs_operation.c`
- `tests/vcmd_profile_bench.c`
- `tests/vector_core_hnsw_rebuild_profile_test.sh`

Stage 7F 新增文件：

- `docs/vector_core_reset/stage7f_highdim_layer_search_audit.md`

工作树中仍保留 Stage 7E 未提交文件 / 修改：

- `src/vector/core/hnsw/kvs_hnsw_index.c`
- `src/vector/core/hnsw/kvs_hnsw_info.c`
- `docs/vector_core_reset/stage7e_neighbor_select_score_reuse_optimization.md`

未处理无关工作树修改：

- `README.md`

## 3. 是否严格遵守 Stage 7E 后续路线

是。

Stage 7E 结论是 level0 worst-neighbor distance recompute 热点已经解决，剩余 high-dim rebuild 主耗时转移到 `hnsw_add_layer_search_us`。本阶段没有继续扩展 neighbor_select 优化，只保留 Stage 7E 既有字段，并新增 layer_search 内部归因字段。

## 4. 是否只做 profiling / audit

是。

新增代码只做计数和低频时间拆分：

- 每次 `kvs_hnsw_search_layer_for_add()` 调用计数。
- 每个邻居扫描、visited skip、active check、距离计算、heap push/pop/replace、lower_bound 更新做整数计数。
- 每次 layer_search 调用只在入口 / 出口读取时间，用于拆分 level0 vs upper layer_search 时间。

未修改候选接受规则、heap 比较规则、图连接规则、搜索参数或 recall 相关逻辑。

## 5. 当前调用链

```text
vcmd_profile_bench ann_core
  -> kvs_vector_restore_record_with_embedding()
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_vector_manager_for_each_active_embedding()
  -> kvs_hnsw_index_add()
  -> kvs_hnsw_search_layer_for_add()
  -> kvs_hnsw_connect_layer_from_context()
```

## 6. 新增 layer_search profiling 字段

新增字段：

- `layer_search_call_count`
- `layer_search_level0_call_count`
- `layer_search_upper_call_count`
- `layer_search_level0_us`
- `layer_search_upper_us`
- `layer_search_distance_compute_count`
- `layer_search_visited_skip_count`
- `layer_search_neighbor_visit_count`
- `layer_search_level0_neighbor_scan_count`
- `layer_search_upper_neighbor_scan_count`
- `layer_search_candidate_heap_push_count`
- `layer_search_candidate_heap_pop_count`
- `layer_search_candidate_heap_replace_count`
- `layer_search_top_heap_push_count`
- `layer_search_top_heap_replace_count`
- `layer_search_lower_bound_update_count`
- `layer_search_deleted_active_check_count`

兼容保留字段：

- Stage 6A `hnsw_add_*` 时间字段。
- Stage 7A `add_*` 粗粒度事件字段。
- Stage 7D / 7E `neighbor_select_*` 字段。

## 7. Before behavior

Stage 7E 后 high-dim rebuild 剩余主耗时为 `hnsw_add_layer_search_us`，但 layer_search 内部只通过 Stage 7A 的粗粒度 `add_*` 字段观察，无法直接区分：

- level0 vs upper layer_search 时间。
- distance compute 与 neighbor scan 的比例。
- visited skip 压力。
- candidate heap / top heap 操作压力。
- deleted/active check 是否影响当前 workload。

## 8. After design

在 `kvs_hnsw_search_layer_for_add()` 内维护 runtime profile counters：

- `layer_search_level0_us` / `layer_search_upper_us` 从函数入口到出口累计。
- `layer_search_distance_compute_count` 统计 entry 初始距离计算和新访问邻居距离计算。
- `layer_search_neighbor_visit_count` 统计扫描到的邻居槽位数。
- `layer_search_visited_skip_count` 统计 visited tag 命中跳过。
- `layer_search_level0_neighbor_scan_count` / `layer_search_upper_neighbor_scan_count` 拆分邻居扫描层级。
- `layer_search_candidate_heap_push_count` / `pop_count` / `replace_count` 统计 candidate heap 管理。
- `layer_search_top_heap_push_count` / `replace_count` 统计 top heap 管理。
- `layer_search_lower_bound_update_count` 统计 lower_bound 重设次数。
- `layer_search_deleted_active_check_count` 统计 deleted/active check 次数。

当前 add-time candidate heap 没有 replace 路径，因此 `layer_search_candidate_heap_replace_count` 在本轮数据中为 0。

## 9. Safety invariants

- 新字段是 runtime observability state，不进入 RDB/AOF。
- 新字段通过 VINFO / benchmark 输出，不改变 VSEARCH response。
- `kvs_vector_index_ops_t` 未改。
- `ef_construction` / `ef_search` 未降低。
- core 仍只处理 record_id / vector / score / index-local graph state。
- `src/vector/core` 未访问 `g_vector_state`、`record_id_map`、scope、TTL、metadata、question、answer。

## 10. Benchmark result

命令：

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 768 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 1024 10 50
```

结果文件：

- `results/vector_core_benchmark/stage7f_highdim_layer_search_1k_384d.json`
- `results/vector_core_benchmark/stage7f_highdim_layer_search_1k_512d.json`
- `results/vector_core_benchmark/stage7f_highdim_layer_search_1k_768d.json`
- `results/vector_core_benchmark/stage7f_highdim_layer_search_1k_1024d.json`

| dim | rebuild_core_add_total_us | hnsw_add_layer_search_us | layer_search_level0_us | layer_search_upper_us | hnsw_add_neighbor_select_us | hnsw_add_link_us | recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 219,586 | 196,743 | 196,400 | 275 | 5,491 | 2,930 | 1.0000 |
| 512 | 276,733 | 247,848 | 247,322 | 481 | 6,920 | 2,981 | 1.0000 |
| 768 | 407,561 | 377,044 | 376,654 | 332 | 5,186 | 3,199 | 1.0000 |
| 1024 | 509,231 | 465,741 | 465,008 | 674 | 7,338 | 3,247 | 1.0000 |

## 11. layer_search attribution result

| dim | calls | distance_compute | neighbor_visit | visited_skip | level0_scan | upper_scan | candidate_push | candidate_pop | candidate_replace | top_push | top_replace | lower_bound_updates | active_checks |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 1,033 | 499,711 | 6,702,263 | 6,203,585 | 6,689,215 | 13,048 | 234,258 | 109,078 | 0 | 120,339 | 130,824 | 137,287 | 0 |
| 512 | 1,039 | 499,900 | 6,658,668 | 6,159,807 | 6,638,548 | 20,120 | 235,925 | 108,435 | 0 | 120,564 | 133,891 | 140,329 | 0 |
| 768 | 1,028 | 499,513 | 6,555,579 | 6,057,094 | 6,548,271 | 7,308 | 236,628 | 106,521 | 0 | 120,151 | 137,401 | 143,679 | 0 |
| 1024 | 1,037 | 499,762 | 6,532,577 | 6,033,852 | 6,517,303 | 15,274 | 237,399 | 106,291 | 0 | 120,413 | 138,529 | 144,899 | 0 |

归因：

- `layer_search_level0_us` 占 `hnsw_add_layer_search_us` 的 99.8% 以上，upper layer_search 不是当前 high-dim 主因。
- 每轮约 500K 次 distance compute，且 rebuild 时间随维度从 384d 到 1024d 基本线性增长，distance/SIMD/alignment 是可验证候选方向。
- 每轮约 6.5M-6.7M 次 level0 neighbor scan，其中约 6.0M-6.2M 次 visited skip，说明 level0 邻接访问和 visited tag 检查也是主要归因来源。
- top heap replace 约 131K-139K 次，candidate heap push/pop 约 234K/106K，heap 管理有成本，但从数量级看低于 neighbor scan / visited skip 压力。
- `layer_search_deleted_active_check_count` 为 0，本轮 rebuild 无 deleted active-check 热点。
- `neighbor_select_level0_distance_recompute_count` 保持 0，Stage 7E 结论未回退。

## 12. recall@10 是否回退

未回退。

| dim | recall@10 |
| --- | ---: |
| 384 | 1.0000 |
| 512 | 1.0000 |
| 768 | 1.0000 |
| 1024 | 1.0000 |

## 13. 正确性测试结果

Passed:

```bash
tests/vector_core_hnsw_rebuild_profile_test.sh
```

本阶段按 prompt 只必跑 rebuild profile test。Stage 7F 是 profiling / audit，不改变 HNSW graph 决策或 VSEARCH response。

## 14. Static boundary check result

Commands:

```bash
grep -RIn "g_vector_state" src/vector/core || true
grep -RIn "record_id_map" src/vector/core || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core || true
```

Result: no output.

`git diff --check`: passed.

Stage 7F files also pass targeted whitespace check:

```bash
git diff --check -- include/kvs_vector.h include/vector/kvs_vector_hnsw.h src/core/kvs_operation.c src/vector/core/hnsw/kvs_hnsw_add.c src/vector/core/hnsw/kvs_hnsw_index.c src/vector/core/hnsw/kvs_hnsw_info.c src/vector/kvs_vector.c tests/vcmd_profile_bench.c tests/vector_core_hnsw_rebuild_profile_test.sh docs/vector_core_reset/stage7e_neighbor_select_score_reuse_optimization.md docs/vector_core_reset/stage7f_highdim_layer_search_audit.md
```

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
 M tests/vcmd_profile_bench.c
 M tests/vector_core_hnsw_rebuild_profile_test.sh
?? docs/vector_core_reset/stage7e_neighbor_select_score_reuse_optimization.md
?? docs/vector_core_reset/stage7f_highdim_layer_search_audit.md
```

## 17. Next suggested step

Based only on Stage 7F data, the next route should choose one of:

- `layer_search` distance/SIMD/alignment optimization, because distance compute count is about 500K and time scales with dimension.
- visited_tags / metadata compact, because level0 neighbor scan is about 6.5M-6.7M and visited skip is about 6.0M-6.2M.
- level0 neighbor access/layout optimization, because more than 99.7% of neighbor scans are level0.
- heap/candidate management optimization only if later profiling shows heap operation cost dominates wall time; current counts are materially lower than scan / skip pressure.
- stop optimizing if these high-dim rebuild numbers are already acceptable.

Do not continue neighbor_select optimization unless a correctness issue appears.
