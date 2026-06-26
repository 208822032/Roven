# Stage 6A core_hnsw rebuild profiling-only audit

## 1. 阶段目标

Stage 6A 目标是只审计 `core_hnsw` rebuild 耗时拆分，为 Stage 6B targeted rebuild optimization 提供证据。

本阶段不优化 HNSW graph，不修改 memory layout，不修改 neighbor selection 策略，不修改 add/search 语义，不修改 RDB/AOF 格式。

## 2. 是否修改生产代码

已修改生产代码，但只加入低侵入 profiling counters 和 VINFO 暴露：

- `include/vector/kvs_vector_hnsw.h`
- `src/vector/core/hnsw/kvs_hnsw_index.c`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/kvs_vector.c`
- `include/kvs_vector.h`
- `src/core/kvs_operation.c`

额外说明：用户允许修改列表没有显式列出 `include/kvs_vector.h` 和 `src/core/kvs_operation.c`，但验收要求从 `VINFO` 读取 rebuild profile counters，因此必须最小扩展 `kvs_vector_info_t` 和 VINFO RESP 输出。未修改 `src/persistence`。

## 3. 修改/新增文件

- `include/vector/kvs_vector_hnsw.h`：新增 `kvs_hnsw_rebuild_profile_t` 和 profile helper 声明。
- `src/vector/core/hnsw/kvs_hnsw_index.c`：新增 monotonic clock、profile reset、add 聚合 helper。
- `src/vector/core/hnsw/kvs_hnsw_add.c`：记录 add 子阶段耗时。
- `src/vector/kvs_vector.c`：记录 core_hnsw rebuild reset/reserve/manager scan/add/total，并填充 VINFO。
- `include/kvs_vector.h`：新增 rebuild profile VINFO 字段。
- `src/core/kvs_operation.c`：VINFO 输出 core_hnsw rebuild profile 字段。
- `scripts/bench_vector_core_compare.py`：benchmark JSON/Markdown 采集 rebuild profile。
- `tests/vector_core_hnsw_rebuild_profile_test.sh`：新增 profile-only 回归测试。
- `docs/vector_core_reset/stage6a_core_hnsw_rebuild_profile_audit.md`：本报告。

## 4. core_hnsw rebuild 调用链

```text
VINDEX REBUILD
  -> kvs_vector_rebuild_current_index()
  -> current_index.ops->rebuild()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_hnsw_index_reset()
  -> kvs_hnsw_index_reserve(active_records)
  -> kvs_vector_manager_for_each_active_embedding()
       -> scan manager record store capacity
       -> kvs_vector_core_hnsw_rebuild_add_record()
       -> kvs_hnsw_index_add(record_id, vector)
            -> record_id_to_slot lookup
            -> random level
            -> vector copy
            -> greedy search on upper layers
            -> layer search for add
            -> neighbor accept/select
            -> bidirectional link
```

RDB load / AOF replay 后仍由 manager 基于 active records 重建派生 index；本阶段未保存任何 profile map 到 RDB/AOF。

## 5. rebuild profiling 字段

VINFO 对 `core_hnsw` 输出：

```text
rebuild_total_us
rebuild_manager_scan_us
rebuild_core_reset_us
rebuild_core_reserve_us
rebuild_core_add_total_us
rebuild_core_add_avg_us
rebuild_core_add_max_us
rebuild_core_add_count
hnsw_add_random_level_us
hnsw_add_greedy_search_us
hnsw_add_layer_search_us
hnsw_add_neighbor_select_us
hnsw_add_link_us
hnsw_add_vector_copy_us
```

`rebuild_manager_scan_us` 口径是 `kvs_vector_manager_for_each_active_embedding()` 总耗时减去 `rebuild_core_add_total_us`，用于近似 manager scan / callback 调度成本。

## 6. 1K / 10K rebuild profile 结果

配置：64d、topK=10、hash embedding、IP metric、`M=16`、`ef_construction=128`、`ef_search=128`。

| 规模 | rebuild wall ms | rebuild_total_us | manager_scan_us | reset_us | reserve_us | add_total_us | add_avg_us | add_max_us | add_count | recall@10 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 99.687 | 99,377 | 36 | 67 | 1 | 99,273 | 99 | 163 | 1,000 | 1.0000 |
| 10K | 1,374.961 | 1,371,021 | 777 | 786 | 0 | 1,369,458 | 136 | 255 | 10,000 | 0.9870 |

## 7. core_hnsw add-time 热点归因

| 规模 | layer_search_us | neighbor_select_us | link_us | greedy_search_us | vector_copy_us | random_level_us |
|---:|---:|---:|---:|---:|---:|---:|
| 1K | 51,822 | 23,869 | 20,170 | 1,463 | 39 | 27 |
| 10K | 850,988 | 262,145 | 205,229 | 28,336 | 948 | 292 |

瓶颈排序：

1. `hnsw_add_layer_search_us`：10K 约 851ms，是 rebuild add 的最大成本。
2. `hnsw_add_neighbor_select_us`：10K 约 262ms，主要来自 `would_accept` / 邻居扫描与距离比较。
3. `hnsw_add_link_us`：10K 约 205ms，来自双向追加/替换和反向移除。
4. `hnsw_add_greedy_search_us`：10K 约 28ms，不是主瓶颈。
5. `hnsw_add_vector_copy_us` / `hnsw_add_random_level_us`：均小于 1ms，不是当前瓶颈。

## 8. malloc/free 与 reserve 审计

审计结论：

- rebuild 中 `kvs_hnsw_index_reserve(active_records)` 只在 reset 后执行一次；10K 结果中 `reserve_us=0`，因为已有 capacity 足够。
- `kvs_hnsw_index_add()` 热路径没有逐条 malloc/free；它复用 `search_context_pool` 中的 heap、neighbor buffer、distance buffer、hits。
- visited state 使用 `visited_tags` + `current_visit_tag`，add-time layer search 未对每条 add 做 full memset。
- rebuild 主要不是 manager scan、reset、reserve 或 vector copy，而是 add-time 图搜索和邻接维护。

## 9. correctness / recall

本阶段没有改变 response 格式，也没有在 response 中暴露 `record_id` / slot。

benchmark recall：

- 1K `core_hnsw` recall@10 = 1.0000
- 10K `core_hnsw` recall@10 = 0.9870

对比 Stage 5D 记录，未发现 recall 回退。

## 10. tests 结果

已通过：

```text
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"

tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
tests/vector_core_hnsw_rebuild_profile_test.sh
tests/vector_manager_resolve_fastpath_test.sh
tests/vector_vsearch_profile_breakdown_test.sh
tests/vector_vinfo_backend_stats_test.sh
```

## 11. benchmark 结果

已运行：

```text
N=1000 DIM=64 TOPK=10 QUERIES=50 tests/vector_core_benchmark_compare.sh
N=10000 DIM=64 TOPK=10 QUERIES=100 tests/vector_core_benchmark_compare.sh
```

结果文件：

- `results/vector_core_benchmark/stage5d_stats_hardening_benchmark_1k_64d.json`
- `results/vector_core_benchmark/stage5d_stats_hardening_benchmark_1k_64d.md`
- `results/vector_core_benchmark/stage5d_stats_hardening_benchmark_10k_64d.json`
- `results/vector_core_benchmark/stage5d_stats_hardening_benchmark_10k_64d.md`

## 12. 静态边界检查结果

```text
grep -RIn "g_vector_state" src/vector/core include/vector
  仅 include/vector/kvs_vector_manager.h 和 index_internal 注释/manager 声明命中；src/vector/core 无新增 g_vector_state。

grep -RIn "scope|ttl|metadata|question|answer" src/vector/core
  无命中。

grep -RIn "kvs_vector_index_ops_t" include src/vector
  ops 表位置存在，签名未修改。

grep -RIn "record_id_map" src/vector/core include/vector
  core_hnsw 内部仍使用 vector_core 自己的 record_id_to_slot；未引入 manager record_id_map。

git diff -- src/persistence
  无 diff。
```

## 13. 风险评估

风险等级：P1 profiling/audit-only，低到中。

风险点：

- VINFO response array count 增加了 core_hnsw 专属 14 个字段；测试已覆盖字段存在性。
- add 路径增加 `clock_gettime()` profiling 开销，因此 benchmark 结果用于阶段归因，不应直接当作未插桩极限性能。

不涉及：

- HNSW graph 结构优化。
- memory layout 优化。
- neighbor selection 行为改变。
- RDB/AOF 格式修改。
- `kvs_vector_index_ops_t` 签名修改。

## 14. 下一步建议：Stage 6B targeted rebuild optimization

Next Stage:

```text
Stage 6B: core_hnsw add-time layer_search / neighbor_select targeted optimization
```

Reason:

```text
1. 10K rebuild_total_us 约 1.371s，其中 rebuild_core_add_total_us 约 1.369s，manager scan/reset/reserve 不是瓶颈。
2. add 内部最大热点是 hnsw_add_layer_search_us，其次是 hnsw_add_neighbor_select_us 和 hnsw_add_link_us。
3. vector copy、random level、reserve、manager scan 都很小，优先优化它们收益低。
```

Allowed files:

```text
src/vector/core/hnsw/*
include/vector/kvs_vector_hnsw.h
tests/vector_core_hnsw_rebuild_profile_test.sh
docs/vector_core_reset/stage6b_core_hnsw_rebuild_optimization.md
```

Forbidden:

```text
RDB/AOF 格式
kvs_vector_index_ops_t 签名
VSEARCH response 格式
manager scope / TTL / metadata / response 逻辑
external backend 替换
```

Acceptance:

```text
1. 1K / 10K rebuild_core_add_total_us 下降。
2. hnsw_add_layer_search_us 或 neighbor_select_us 明确下降。
3. recall@10 不低于 Stage 6A 基线。
4. HNSW correctness / rebuild / RDB / AOF tests 通过。
5. Stage 5A/5B/5C/5D focused tests 通过。
```
