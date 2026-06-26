# Stage 6B core_hnsw rebuild targeted optimization

## 1. 阶段目标

Stage 6B 只针对 Stage 6A 已证明的 `core_hnsw` rebuild add-time 热点做 P1 targeted optimization：

- 降低 `kvs_hnsw_index_add()` rebuild 路径耗时。
- 优先处理 `layer_search` / `neighbor_select` / `link` 中的重复工作。
- 不做 HNSW graph memory layout 重构。
- 不修改 RDB/AOF 格式。
- 不修改 `kvs_vector_index_ops_t` 签名。
- 不改变 VSEARCH response 格式。
- 不把 manager scope / TTL / metadata / response 逻辑引入 vector_core。

## 2. 修改/新增文件

Stage 6B 本轮实际修改：

- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `scripts/bench_vector_core_compare.py`
- `docs/vector_core_reset/stage6b_core_hnsw_rebuild_optimization.md`

说明：当前工作区还包含 Stage 5A/5B/5C/5D/6A 的未提交改动，本报告只归因 Stage 6B 上述文件。

## 3. 是否修改生产代码

是。

生产代码修改范围仅限 `src/vector/core/hnsw/kvs_hnsw_add.c`，属于 `vector_core/core_hnsw` 内部 add-time rebuild 优化。

## 4. Stage 6A baseline

Stage 6A baseline：

| dataset | rebuild_total_us | rebuild_core_add_total_us | layer_search_us | neighbor_select_us | link_us | recall@10 |
|---|---:|---:|---:|---:|---:|---:|
| 1K / 64d | 99,377 | 99,273 | 51,822 | 23,869 | 20,170 | 1.0000 |
| 10K / 64d | 1,371,021 | 1,369,458 | 850,988 | 262,145 | 205,229 | 0.9870 |

Stage 6A 结论：`core_hnsw` rebuild 慢主要在 add-time graph construction，不在 manager scan/reset/reserve。

## 5. 优化点列表

1. 删除 `kvs_hnsw_search_layer_for_add()` 的候选结果二次插入排序。
   - `top_heap` 是 min-score heap，pop 顺序为低分到高分。
   - Stage 6B 改为从 buffer 尾部反向写入，直接得到降序结果。
   - 不改变候选集合，不改变 score，不改变搜索参数。

2. 合并双向边写入前的 accept 判断和 append/replace 决策。
   - 原路径先 `would_accept(a,b)` / `would_accept(b,a)`，再 `append_or_replace(a,b)` / `append_or_replace(b,a)`。
   - 两个阶段会重复查重、重复扫描最差邻居、重复计算 slot-neighbor distance。
   - Stage 6B 改为 `plan_append_or_replace()` 一次计算 accepted / removed / replace_pos，再 `apply_append_or_replace()` 执行写入。

3. 合并 duplicate 检查与 worst-neighbor 扫描。
   - 当邻接表已满时，一次循环同时检查 duplicate 并寻找 worst neighbor。
   - 当邻接表未满时，只扫描 duplicate，不计算无用 distance。

## 6. layer_search 优化说明

`kvs_hnsw_search_layer_for_add()` 原逻辑：

1. 从 `ctx->top_heap` pop 到 `neighbor_buffer`。
2. 再对 `neighbor_buffer` 做插入排序，按 score 降序。

Stage 6B 逻辑：

1. 读取 `top_heap.size` 作为候选数量。
2. 由于 `top_heap` 是 min-score heap，pop 出来的顺序是从差到好。
3. 从 buffer 尾部向前写入，得到 score 降序。

该优化减少每层 add search 后最多 `ef_construction` 个候选的二次排序开销，保持输出顺序仍为 score 降序。

## 7. neighbor_select 优化说明

Stage 6B 没有替换 neighbor selection 策略，也没有降低 `ef_construction`。

优化仅减少重复工作：

- `plan_append_or_replace()` 同时完成 active/self 校验、duplicate 检查、容量判断、最差邻居扫描、候选是否可替换判断。
- 双向边仍要求 a->b 与 b->a 两侧都 accepted 后才写入。
- candidate distance 与当前邻居 distance 只在需要决策时计算。

## 8. link 优化说明

Stage 6B 的主要收益来自 link 阶段。

原路径在 link 写入前后重复执行：

- duplicate 扫描。
- full-neighbor worst 扫描。
- slot-neighbor distance 计算。

Stage 6B 把决策结果缓存为：

- `accepted`
- `removed`
- `replace_pos`

随后 apply 阶段只按计划写入 append 或 replace，不再重复做 accept 决策。被替换的反向边仍通过 `kvs_hnsw_layer_remove_neighbor()` 清理。

## 9. 不变量检查

保持的不变量：

- no self edge：`a == b` 直接跳过；plan/apply 都拒绝 `slot == neighbor`。
- no duplicate edge：plan 阶段扫描已有邻居，duplicate 只视为 accepted，不追加新边。
- level0 双向边语义保持：仍在 `kvs_hnsw_add_bidirectional_edge()` 中同时处理 a->b 与 b->a。
- upper layer 双向边语义保持：同一 helper 对所有 level 生效。
- deleted_flags 语义保持：active-at-level 校验仍包含 `!deleted_flags[slot]`。
- record_id_to_slot 语义保持：未修改 id-slot map。
- search 输出仍为 `record_id + score`。
- core 不处理 scope / TTL / metadata / question / answer。
- manager 仍负责 response。

## 10. 1K before/after profile

| metric | Stage 6A before | Stage 6B after | change |
|---|---:|---:|---:|
| rebuild_total_us | 99,377 | 76,290 | -23.2% |
| rebuild_core_add_total_us | 99,273 | 76,190 | -23.3% |
| hnsw_add_layer_search_us | 51,822 | 47,255 | -8.8% |
| hnsw_add_neighbor_select_us | 23,869 | 24,462 | +2.5% |
| hnsw_add_link_us | 20,170 | 1,050 | -94.8% |
| recall@10 | 1.0000 | 1.0000 | no regression |

## 11. 10K before/after profile

| metric | Stage 6A before | Stage 6B after | change |
|---|---:|---:|---:|
| rebuild_total_us | 1,371,021 | 1,128,712 | -17.7% |
| rebuild_core_add_total_us | 1,369,458 | 1,126,478 | -17.7% |
| hnsw_add_layer_search_us | 850,988 | 796,544 | -6.4% |
| hnsw_add_neighbor_select_us | 262,145 | 267,797 | +2.2% |
| hnsw_add_link_us | 205,229 | 11,294 | -94.5% |
| recall@10 | 0.9870 | 0.9870 | no regression |

## 12. rebuild_core_add_total_us 是否下降

是。

- 1K：`99,273 -> 76,190`，下降 23.3%。
- 10K：`1,369,458 -> 1,126,478`，下降 17.7%。

Stage 6B 达到 acceptable pass：10K `rebuild_core_add_total_us` 下降超过 10%，且 recall@10 未回退。

## 13. recall@10 是否回退

未回退。

- 1K：Stage 6A `1.0000`，Stage 6B `1.0000`。
- 10K：Stage 6A `0.9870`，Stage 6B `0.9870`。

## 14. tests 结果

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

## 15. benchmark 结果

运行：

```bash
python3 scripts/bench_vector_core_compare.py --corrected --stage6b --kv-bin ./kvstore --n 1000 --dim 64 --topk 10 --queries 50
python3 scripts/bench_vector_core_compare.py --corrected --stage6b --kv-bin ./kvstore --n 10000 --dim 64 --topk 10 --queries 100
```

输出：

```text
results/vector_core_benchmark/stage6b_hnsw_rebuild_opt_benchmark_1k_64d.json
results/vector_core_benchmark/stage6b_hnsw_rebuild_opt_benchmark_1k_64d.md
results/vector_core_benchmark/stage6b_hnsw_rebuild_opt_benchmark_10k_64d.json
results/vector_core_benchmark/stage6b_hnsw_rebuild_opt_benchmark_10k_64d.md
```

说明：Stage 6B 修正了 benchmark 脚本输出命名，新增 `--stage6b`，避免本阶段结果继续落到 `stage5d_stats_hardening_benchmark_*`。

## 16. 静态边界检查结果

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
- `record_id_map` 仅出现在 manager/record 头文件，不在 `src/vector/core`。
- `git diff -- src/persistence` 为空，RDB/AOF 格式未修改。

## 17. git status --short

当前工作区包含 Stage 5A-5D、Stage 6A、Stage 6B 的累计未提交改动。Stage 6B 不清理 unrelated diff。

主要 Stage 6B 相关状态：

```text
 M scripts/bench_vector_core_compare.py
 M src/vector/core/hnsw/kvs_hnsw_add.c
?? docs/vector_core_reset/stage6b_core_hnsw_rebuild_optimization.md
```

## 18. 下一步建议

Next Stage:
  Stage 6C: core_hnsw layer_search targeted optimization audit + implementation

Reason:
  1. Stage 6B 已基本消除 link 阶段重复扫描问题，10K `hnsw_add_link_us` 下降 94.5%。
  2. 10K 剩余最大热点仍是 `hnsw_add_layer_search_us = 796,544`，占 add total 约 70.7%。
  3. `neighbor_select_us` 本轮没有实质下降，且略升；下一步应避免继续在 link 上投入，转向 layer search 的 candidate expansion / heap 操作 / distance reuse。

Allowed files:
  `src/vector/core/hnsw/*`, `include/vector/kvs_vector_hnsw.h`, `tests/vector_core_hnsw_rebuild_profile_test.sh`, `docs/vector_core_reset/stage6c_core_hnsw_layer_search_optimization.md`

Forbidden:
  RDB/AOF 格式、`kvs_vector_index_ops_t` 签名、VSEARCH response、manager scope/TTL/metadata/response 逻辑、external backend replacement、graph persistence、memory layout 大重构。

Acceptance:
  10K `hnsw_add_layer_search_us` 明确下降，10K `rebuild_core_add_total_us` 继续下降，recall@10 不低于 0.9870，HNSW correctness/rebuild/RDB/AOF 与 Stage 5A-5D focused tests 全部通过。
