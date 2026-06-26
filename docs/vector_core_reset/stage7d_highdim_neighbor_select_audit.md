# Stage 7D: high-dim neighbor_select profiling audit

## 1. AGENTS.md 遵守情况

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只做 profiling / audit / attribution。未优化 HNSW 算法，未重构 memory layout，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t` 签名，未修改 VSEARCH response 格式，未降低 `ef_construction` / `ef_search`，未牺牲 recall。

本 prompt 与 `AGENTS.md` 未发现冲突。

## 2. 修改 / 新增文件

本轮 Stage 7D 相关修改：

- `include/vector/kvs_vector_hnsw.h`
- `include/kvs_vector.h`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/kvs_vector.c`
- `src/core/kvs_operation.c`
- `tests/vcmd_profile_bench.c`
- `tests/vector_core_hnsw_rebuild_profile_test.sh`
- `docs/vector_core_reset/stage7d_highdim_neighbor_select_audit.md`

生成结果文件：

- `results/vector_core_benchmark/stage7d_highdim_neighbor_select_1k_384d.json`
- `results/vector_core_benchmark/stage7d_highdim_neighbor_select_1k_384d.md`
- `results/vector_core_benchmark/stage7d_highdim_neighbor_select_1k_512d.json`
- `results/vector_core_benchmark/stage7d_highdim_neighbor_select_1k_512d.md`
- `results/vector_core_benchmark/stage7d_highdim_neighbor_select_1k_768d.json`
- `results/vector_core_benchmark/stage7d_highdim_neighbor_select_1k_768d.md`
- `results/vector_core_benchmark/stage7d_highdim_neighbor_select_1k_1024d.json`
- `results/vector_core_benchmark/stage7d_highdim_neighbor_select_1k_1024d.md`

工作树中还存在进入本轮前已有的其他未提交 / 未跟踪文件；本轮未处理无关 diff。

## 3. 是否只做 profiling / audit

是。

生产代码变化仅新增 / 透出 HNSW rebuild profiling counters，并修正 VINFO core_hnsw 数组字段计数以匹配新增字段。未改变索引选择、图结构、邻居选择规则、距离函数、召回参数、持久化格式或用户可见 VSEARCH response。

## 4. 当前调用链

```text
vcmd_profile_bench ann_core
  -> kvs_vector_restore_record_with_embedding()
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_vector_manager_for_each_active_embedding()
  -> kvs_hnsw_index_add()
  -> kvs_hnsw_search_layer_for_add()
  -> kvs_hnsw_connect_layer_from_context()
  -> kvs_hnsw_add_new_slot_bidirectional_edge()
  -> kvs_hnsw_layer_plan_new_slot_append_or_replace()
  -> kvs_hnsw_layer_find_worst_neighbor()
```

## 5. Stage 7C high-dim baseline 复盘

Stage 7C 已确认 direct-vector high-dim harness 真实使用 384 / 512 / 768 / 1024d，并且 `distance_backend=avx2`。

Stage 7C baseline 显示 1K direct-vector 下 neighbor_select 已超过 layer_search：

| dim | Stage 7C rebuild_core_add_total_us | Stage 7C layer_search_us | Stage 7C neighbor_select_us | recall@10 |
| --- | ---: | ---: | ---: | ---: |
| 384 | 700,931 | 185,871 | 497,909 | 1.0000 |
| 512 | 952,357 | 239,645 | 690,969 | 1.0000 |
| 768 | 1,448,042 | 346,079 | 1,077,146 | 1.0000 |
| 1024 | 1,956,233 | 451,981 | 1,467,678 | 1.0000 |

## 6. 新增 profiling 字段

新增 / 透出 neighbor_select 归因字段：

```text
neighbor_select_call_count
neighbor_select_level0_call_count
neighbor_select_upper_call_count
neighbor_select_candidate_total / avg / max
neighbor_select_selected_total / avg / max
neighbor_select_reject_count
neighbor_select_heuristic_prune_count
neighbor_select_distance_recompute_count
neighbor_select_reused_candidate_score_count
neighbor_select_duplicate_scan_count
neighbor_select_worst_scan_count
neighbor_select_worst_cache_hit_count
neighbor_select_level0_* split
neighbor_select_upper_* split
```

这些字段只在 profiling / VINFO / benchmark 输出中使用，不进入 RDB/AOF。

## 7. Stage 7D benchmark 结果

命令：

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 768 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 1024 10 50
```

| dim | rebuild_core_add_total_us | layer_search_us | neighbor_select_us | recompute_count | level0_recompute | upper_recompute | reused_score | recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 703,775 | 186,210 | 500,293 | 1,817,312 | 1,816,128 | 1,184 | 30,766 | 1.0000 |
| 512 | 953,872 | 239,626 | 692,391 | 1,820,576 | 1,816,064 | 4,512 | 30,834 | 1.0000 |
| 768 | 1,449,984 | 345,658 | 1,078,784 | 1,825,216 | 1,825,216 | 0 | 30,694 | 1.0000 |
| 1024 | 1,962,126 | 452,816 | 1,472,749 | 1,834,272 | 1,832,000 | 2,272 | 30,770 | 1.0000 |

Candidate / selection shape:

| dim | calls | candidate_total | candidate_avg | candidate_max | selected_avg | reject_count | worst_scan_count | worst_cache_hit |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 1,033 | 62,512 | 60.52 | 64 | 60.52 | 2,772 | 1,817,312 | 2,352 |
| 512 | 1,039 | 62,704 | 60.35 | 64 | 60.35 | 2,722 | 1,820,576 | 2,317 |
| 768 | 1,028 | 62,327 | 60.63 | 64 | 60.63 | 2,564 | 1,825,216 | 2,175 |
| 1024 | 1,037 | 62,579 | 60.35 | 64 | 60.35 | 2,476 | 1,834,272 | 2,074 |

## 8. neighbor_select 热点归因

结论：

```text
high-dim neighbor_select 主要热点是 level0 worst-neighbor scan 中的距离重算。
```

依据：

- `candidate_avg` 稳定在约 60，`candidate_max=64`，说明高维耗时增长不是候选数量增长导致。
- `neighbor_select_distance_recompute_count` 约 181-183 万次，且几乎全部来自 level0。
- `neighbor_select_worst_scan_count` 与 `neighbor_select_distance_recompute_count` 完全一致，说明 recompute 主要来自寻找现有邻居列表 worst neighbor。
- `reused_candidate_score_count` 约 3.07 万次，只覆盖候选本身得分复用；现有邻居 worst scan 仍大量重算距离。
- `upper_recompute` 极低或为 0，upper layer 不是 1K high-dim 的主要来源。
- 随 dim 从 384 到 1024 增长，neighbor_select_us 从 500,293 增至 1,472,749，符合固定 recompute 次数下单次高维距离成本增长。

## 9. recall@10 是否回退

未观察到回退。

Stage 7D 四组结果均为：

```text
recall@10 = 1.0000
```

## 10. 正确性测试

通过：

```bash
tests/vector_core_hnsw_rebuild_profile_test.sh
```

输出：

```text
vector_core_hnsw_rebuild_profile_test PASS
```

## 11. 静态边界检查

已运行：

```bash
grep -RIn "g_vector_state" src/vector/core || true
grep -RIn "record_id_map" src/vector/core || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core || true
```

结果：无输出。

说明：

- `src/vector/core` 未访问 `g_vector_state`。
- `record_id_map` 未进入 `src/vector/core`。
- `scope / ttl / metadata / question / answer` 未进入 `src/vector/core`。

## 12. Build / diff check

Build：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：通过。

补充说明：在未 clean 的增量构建后，测试进程曾出现 provider 状态异常；`make clean` 后完整构建恢复正常。原因是本工作树已有结构体布局相关未提交改动，必须保证所有对象用同一头文件版本重新编译。

Diff check：

```bash
git diff --check
```

结果：通过，无输出。

## 13. git status --short

```text
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M include/vector/kvs_vector_manager.h
 M include/vector/kvs_vector_record.h
 M scripts/bench_vector_core_compare.py
 M src/core/kvs_command_executor.c
 M src/core/kvs_command_task.c
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_index.c
 M src/vector/core/hnsw/kvs_hnsw_info.c
 M src/vector/core/hnsw/kvs_hnsw_search.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_distance_avx2.c
 M src/vector/kvs_vector_hnswlib_ops.c
 M src/vector/kvs_vector_index.c
 M tests/vcmd_profile_bench.c
?? AGENTS.md
?? docs/vector_core_reset/stage5a_manager_resolve_fastpath_report.md
?? docs/vector_core_reset/stage5b_vsearch_profile_breakdown_report.md
?? docs/vector_core_reset/stage5c_vinfo_backend_stats_report.md
?? docs/vector_core_reset/stage5d_vinfo_benchmark_stats_hardening_report.md
?? docs/vector_core_reset/stage6a_core_hnsw_rebuild_profile_audit.md
?? docs/vector_core_reset/stage6b_core_hnsw_rebuild_optimization.md
?? docs/vector_core_reset/stage6c_core_hnsw_layer_search_optimization.md
?? docs/vector_core_reset/stage6d_core_hnsw_neighbor_select_optimization.md
?? docs/vector_core_reset/stage6e_core_hnsw_layer_search_remaining_hotspot.md
?? docs/vector_core_reset/stage7a_core_hnsw_memory_layout_cache_audit.md
?? docs/vector_core_reset/stage7b_core_hnsw_avx2_distance_backend.md
?? docs/vector_core_reset/stage7c_direct_vector_highdim_harness.md
?? docs/vector_core_reset/stage7d_highdim_neighbor_select_audit.md
?? "docs/vector_core_reset/\344\274\230\345\214\226\346\255\245\351\252\244.md"
?? tests/vector_core_hnsw_rebuild_profile_test.sh
?? tests/vector_manager_resolve_fastpath_test.sh
?? tests/vector_vinfo_backend_stats_test.sh
?? tests/vector_vsearch_profile_breakdown_test.sh
```

## 14. 下一阶段建议

建议下一阶段优先进入：

```text
neighbor_select score reuse / prune optimization
```

理由：

- 当前高维 hotspot 不在 upper layer，也不是候选数量增长。
- 主要成本是 level0 existing-neighbor worst scan 的大量距离重算。
- 现有 candidate score reuse 只覆盖新候选得分，不能覆盖 worst scan 对现有邻居的重复距离计算。

可作为候选的次优方向：

```text
distance / SIMD / alignment optimization
```

理由：

- high-dim 下固定 recompute 次数乘以维度，距离核成本仍显著。
- 但 Stage 7B 已启用 AVX2，继续优化需要更细粒度 distance kernel attribution。

不建议下一阶段优先做：

```text
metadata compact
upper_neighbors compact
```

理由：

- Stage 7D 数据显示 upper layer recompute 极低。
- 当前主热点不是 metadata 访问，也不是 upper neighbor storage 访问。
