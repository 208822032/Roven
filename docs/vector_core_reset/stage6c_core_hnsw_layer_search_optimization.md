# Stage 6C core_hnsw layer search optimization

## 1. 阶段目标

Stage 6C 只优化 `core_hnsw` add-time layer search 路径，目标是降低：

- `hnsw_add_layer_search_us`
- `rebuild_core_add_total_us`

本阶段不修改 RDB/AOF 格式，不修改 `kvs_vector_index_ops_t` 签名，不改变 VSEARCH response 格式，不替换外部 backend，不引入 manager 语义到 vector_core，不降低 `ef_construction` / `ef_search` 默认值。

## 2. 修改/新增文件

本轮 Stage 6C 实际修改：

- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `scripts/bench_vector_core_compare.py`
- `docs/vector_core_reset/stage6c_core_hnsw_layer_search_optimization.md`

生成 benchmark 结果：

- `results/vector_core_benchmark/stage6c_hnsw_layer_search_opt_benchmark_1k_64d.json`
- `results/vector_core_benchmark/stage6c_hnsw_layer_search_opt_benchmark_1k_64d.md`
- `results/vector_core_benchmark/stage6c_hnsw_layer_search_opt_benchmark_10k_64d.json`
- `results/vector_core_benchmark/stage6c_hnsw_layer_search_opt_benchmark_10k_64d.md`

说明：工作区仍包含 Stage 5A-5D / 6A / 6B 的未提交改动。本报告只归因 Stage 6C 上述改动。`scripts/bench_vector_core_compare.py` 中已有 Stage 5D/6B 统计增强，本轮只新增 Stage 6C 输出标题、stage 标记和文件名前缀。

## 3. 是否修改生产代码

是。

生产代码修改范围仅限 `src/vector/core/hnsw/kvs_hnsw_add.c`，属于 `vector_core/core_hnsw` 内部 add-time layer search 优化。

## 4. Stage 6B baseline

| dataset | rebuild_core_add_total_us | layer_search_us | neighbor_select_us | link_us | recall@10 |
|---|---:|---:|---:|---:|---:|
| 1K / 64d | 76,190 | 47,255 | 24,462 | 1,050 | 1.0000 |
| 10K / 64d | 1,126,478 | 796,544 | 267,797 | 11,294 | 0.9870 |

Stage 6B 后 `link_us` 已不再是主要瓶颈，10K 最大剩余热点是 `hnsw_add_layer_search_us`。

## 5. 实际优化点

1. add-time layer search 使用轻量 context reset。
   - 原路径调用 `kvs_hnsw_context_reset(ctx)`，会重置 heap 并清空 `neighbor_buffer`、`distance_buffer`、`hits`。
   - add-time layer search 不使用 `hits`，且 candidate 输出会覆盖 `neighbor_buffer` / `distance_buffer` 的有效区间。
   - Stage 6C 新增 `kvs_hnsw_add_search_context_reset()`，只重置 `candidate_heap` 和 `top_heap`。

2. 只导出实际连接需要的最佳候选。
   - 搜索宽度仍由 `ef_construction` 控制，未降低参数。
   - 连接阶段最多只使用当前 layer capacity：level0 为 `M0`，upper layer 为 `M`。
   - Stage 6C 从 `top_heap.items` 中选择前 `M0/M` 个 score 降序候选，避免 pop/export 全部 `ef_construction` 候选。

3. layer search hot loop 缓存 lower bound。
   - 原路径多次读取 `kvs_vector_heap_top(&ctx->top_heap)` 并通过 helper offer。
   - Stage 6C 在搜索过程中维护 `lower_bound_score`，只有 top heap push/replace 后更新。

4. rebuild/add 无删除快路径。
   - 当 `index->deleted_count == 0` 时，构建路径中的邻居由 graph 连接语义保证存在且处于该 layer。
   - Stage 6C 在该快路径只保留 `visited_tags` 去重，跳过每个 neighbor 的完整 active-at-level 检查。
   - 一旦存在 deleted slot，仍回退到原完整校验。

5. add-time heap item 不再读取 record_id。
   - add-time layer search 最终只导出 slot + score 给 graph 连接，不返回 search response。
   - Stage 6C 将 add-time candidate item 的 `record_id` 置为 0，避免无用 `slot_to_record_id` 读取。

## 6. layer_search 优化说明

Stage 6C 保持 HNSW 搜索宽度和候选接受条件：

- `ef_construction` 仍控制 candidate expansion 和 top heap 容量。
- candidate expansion 仍使用 `visited_tags` 防重复访问。
- top heap 仍保留本次 layer search 的 top `ef_construction` 候选。
- 输出候选仍按 score 降序。

变化只发生在 add-time layer search 内部：

- reset 少清空无关 buffer。
- lower bound 少做 heap top/helper 往返。
- 导出阶段只选出连接阶段会使用的 top `M0/M`。
- 无删除 rebuild 路径减少 per-neighbor active 检查。

## 7. 不变量检查

保持的不变量：

- no self edge：`kvs_hnsw_add_bidirectional_edge()` 仍跳过 `a == b`。
- no duplicate edge：Stage 6B plan/apply 路径仍做 duplicate 检查。
- level0 双向边语义保持。
- upper layer 双向边语义保持。
- deleted_flags 语义保持：有 deleted slot 时仍执行完整 active-at-level 校验。
- record_id_to_slot 语义保持：未修改 id-slot map。
- search 输出仍为 `record_id + score`：VSEARCH search path 未修改。
- core 不处理 scope / TTL / metadata / question / answer。
- manager 仍负责 response。

## 8. 1K before/after profile

| metric | Stage 6B before | Stage 6C after | change |
|---|---:|---:|---:|
| rebuild_core_add_total_us | 76,190 | 71,891 | -5.64% |
| hnsw_add_layer_search_us | 47,255 | 42,984 | -9.04% |
| hnsw_add_neighbor_select_us | 24,462 | 24,624 | +0.66% |
| hnsw_add_link_us | 1,050 | 946 | -9.90% |
| recall@10 | 1.0000 | 1.0000 | no regression |

1K benchmark output:

- `results/vector_core_benchmark/stage6c_hnsw_layer_search_opt_benchmark_1k_64d.json`
- `results/vector_core_benchmark/stage6c_hnsw_layer_search_opt_benchmark_1k_64d.md`

## 9. 10K before/after profile

| metric | Stage 6B before | Stage 6C after | change |
|---|---:|---:|---:|
| rebuild_core_add_total_us | 1,126,478 | 1,059,757 | -5.92% |
| hnsw_add_layer_search_us | 796,544 | 730,361 | -8.31% |
| hnsw_add_neighbor_select_us | 267,797 | 267,038 | -0.28% |
| hnsw_add_link_us | 11,294 | 11,520 | +2.00% |
| recall@10 | 0.9870 | 0.9870 | no regression |

10K benchmark output:

- `results/vector_core_benchmark/stage6c_hnsw_layer_search_opt_benchmark_10k_64d.json`
- `results/vector_core_benchmark/stage6c_hnsw_layer_search_opt_benchmark_10k_64d.md`

## 10. Pass 判断

Acceptable pass：

- 10K `hnsw_add_layer_search_us` 下降 >= 8%：PASS，`-8.31%`。
- 10K `rebuild_core_add_total_us` 下降 >= 5%：PASS，`-5.92%`。
- recall@10 不低于 0.9870：PASS，`0.9870`。

Strong pass：

- 10K `hnsw_add_layer_search_us` 下降 >= 15%：未达到。
- 10K `rebuild_core_add_total_us` 下降 >= 10%：未达到。

结论：Stage 6C 达到 acceptable pass，未达到 strong pass。

## 11. tests 结果

构建：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：PASS。

测试：

```bash
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
tests/vector_core_hnsw_rebuild_profile_test.sh
tests/vector_manager_resolve_fastpath_test.sh
tests/vector_vsearch_profile_breakdown_test.sh
tests/vector_vinfo_backend_stats_test.sh
```

结果：全部 PASS。

## 12. benchmark 结果

运行：

```bash
python3 scripts/bench_vector_core_compare.py --corrected --stage6c --kv-bin ./kvstore --n 1000 --dim 64 --topk 10 --queries 50
python3 scripts/bench_vector_core_compare.py --corrected --stage6c --kv-bin ./kvstore --n 10000 --dim 64 --topk 10 --queries 100
```

结果：

- 1K core_hnsw status：PASS，recall@10 `1.0000`。
- 10K core_hnsw status：PASS，recall@10 `0.9870`。
- 10K layer search 与 add total 达到 acceptable pass。

## 13. 静态边界检查结果

执行：

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
grep -RIn "record_id_map" src/vector/core include/vector 2>/dev/null
git diff -- src/persistence
```

结果：

- `src/vector/core` 未新增 `g_vector_state`。
- `src/vector/core` 未出现 scope / ttl / metadata / question / answer。
- `kvs_vector_index_ops_t` 签名未修改。
- `record_id_map` 未出现在 `src/vector/core`；仅 manager/record 头文件中存在。
- `git diff -- src/persistence` 为空，RDB/AOF 格式未修改。

## 14. git status --short

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
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_hnswlib_ops.c
 M src/vector/kvs_vector_index.c
?? AGENTS.md
?? docs/vector_core_reset/stage5a_manager_resolve_fastpath_report.md
?? docs/vector_core_reset/stage5b_vsearch_profile_breakdown_report.md
?? docs/vector_core_reset/stage5c_vinfo_backend_stats_report.md
?? docs/vector_core_reset/stage5d_vinfo_benchmark_stats_hardening_report.md
?? docs/vector_core_reset/stage6a_core_hnsw_rebuild_profile_audit.md
?? docs/vector_core_reset/stage6b_core_hnsw_rebuild_optimization.md
?? docs/vector_core_reset/stage6c_core_hnsw_layer_search_optimization.md
?? docs/vector_core_reset/优化步骤.md
?? tests/vector_core_hnsw_rebuild_profile_test.sh
?? tests/vector_manager_resolve_fastpath_test.sh
?? tests/vector_vinfo_backend_stats_test.sh
?? tests/vector_vsearch_profile_breakdown_test.sh
```

## 15. 下一步建议

Next Stage:

```text
Stage 6D: core_hnsw neighbor_select targeted optimization / audit
```

Reason:

1. Stage 6B 已将 `link_us` 从主要瓶颈中移除。
2. Stage 6C 将 10K `layer_search_us` 降低到 `730,361us`，达到 acceptable pass，但仍是最大单项。
3. 10K `hnsw_add_neighbor_select_us = 267,038us` 基本未下降，已成为更清晰的第二大热点。
4. 下一步应先审计/优化 neighbor_select 的重复 distance compute、worst-neighbor scan、双向 accepted 决策缓存，而不是做 memory layout 大重构。

Allowed files:

```text
src/vector/core/hnsw/kvs_hnsw_add.c
src/vector/core/hnsw/kvs_hnsw_search.c
src/vector/core/hnsw/kvs_hnsw_index.c
include/vector/kvs_vector_hnsw.h
scripts/bench_vector_core_compare.py
tests/vector_core_hnsw_rebuild_profile_test.sh
docs/vector_core_reset/stage6d_core_hnsw_neighbor_select_optimization.md
```

Forbidden:

```text
RDB/AOF format
kvs_vector_index_ops_t signature
VSEARCH response format
manager scope / TTL / metadata / response logic
external backend replacement
graph persistence
memory layout large rewrite
scope-aware search
index sharding / partitioning
lowering ef_construction / ef_search defaults
```

Acceptance:

```text
10K hnsw_add_neighbor_select_us clearly decreases.
10K rebuild_core_add_total_us continues to decrease.
recall@10 remains >= 0.9870.
HNSW correctness/rebuild/RDB/AOF tests pass.
Stage 5A-5D focused tests pass.
Static boundary checks remain clean.
```
