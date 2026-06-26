# Stage 6D core_hnsw neighbor_select optimization / audit

## 1. 阶段目标

Stage 6D 只优化 `core_hnsw` add-time neighbor selection 路径，重点降低 `hnsw_add_neighbor_select_us` 与 `rebuild_core_add_total_us`。本阶段不修改 RDB/AOF 格式，不修改 `kvs_vector_index_ops_t` 签名，不改变 VSEARCH response，不替换 backend，不引入 manager 语义到 vector_core。

## 2. 修改/新增文件

- `include/vector/kvs_vector_hnsw.h`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/core/hnsw/kvs_hnsw_index.c`
- `src/vector/core/hnsw/kvs_hnsw_info.c`
- `scripts/bench_vector_core_compare.py`
- `docs/vector_core_reset/stage6d_core_hnsw_neighbor_select_optimization.md`
- `results/vector_core_benchmark/stage6d_hnsw_neighbor_select_opt_benchmark_1k_64d.json`
- `results/vector_core_benchmark/stage6d_hnsw_neighbor_select_opt_benchmark_1k_64d.md`
- `results/vector_core_benchmark/stage6d_hnsw_neighbor_select_opt_benchmark_10k_64d.json`
- `results/vector_core_benchmark/stage6d_hnsw_neighbor_select_opt_benchmark_10k_64d.md`

说明：工作区仍包含 Stage 5A-5D / 6A / 6B / 6C 的未提交改动。本报告只归因 Stage 6D 上述改动。

## 3. 是否修改生产代码

是。生产代码修改限于 `core_hnsw` 内部 add-time neighbor selection、runtime cache 生命周期和 memory stats；未修改 manager、RDB/AOF、外部 backend 或 response 格式。

## 4. Stage 6C baseline

| dataset | rebuild_core_add_total_us | layer_search_us | neighbor_select_us | link_us | recall@10 |
|---|---:|---:|---:|---:|---:|
| 1K / 64d | 71,891 | 42,984 | 24,624 | 946 | 1.0000 |
| 10K / 64d | 1,059,757 | 730,361 | 267,038 | 11,520 | 0.9870 |

## 5. neighbor_select 当前调用链

```text
VINDEX REBUILD / RDB load rebuild
  -> manager 遍历 active records
  -> core_hnsw reset/reserve
  -> kvs_hnsw_index_add(record_id, vector)
  -> kvs_hnsw_search_layer_for_add(...)
  -> kvs_hnsw_connect_layer_from_context(...)
  -> kvs_hnsw_add_new_slot_bidirectional_edge(...)
  -> kvs_hnsw_layer_plan_new_slot_append_or_replace(...)
  -> kvs_hnsw_layer_apply_append_or_replace(...)
```

## 6. 实际优化点

1. 新增 new-slot 专用双向连接 helper。
   - 新插入 slot 的当前 layer 邻接表从空开始，连接候选来自同一次 layer search 的去重输出。
   - Stage 6D 在 `slot -> candidate` 方向跳过通用 plan 的重复扫描，直接 append；`candidate -> slot` 仍保持原接受/替换规则。

2. 复用 layer_search 已有 candidate score。
   - `ctx->distance_buffer[i]` 已保存新向量与候选 slot 的 score。
   - 内置 IP/L2/cosine 距离为对称 score，`candidate -> new_slot` 接受判断直接复用该 score。
   - 若 `index->distance` 为自定义函数，则回退重新计算，避免假设非对称距离。

3. level0 worst-neighbor runtime cache。
   - 新增 `level0_worst_pos/score/valid`，只缓存每个 level0 slot 当前最差邻居。
   - 满表 candidate 接受判断从重复 O(M0) 扫描变为可复用缓存命中；邻接 append/replace/remove 后失效。
   - 缓存仅在 `deleted_count == 0` 时读写；有删除时回退全量扫描，保持 deleted_flags 语义。
   - 缓存不进入 RDB/AOF，不改变 graph persistence。

4. memory stats 同步补齐。
   - `kvs_hnsw_index_info()` 将新增 runtime cache 纳入 `memory_bytes`，保持 VINFO backend memory stats 口径。

## 7. distance reuse / scan reduction 说明

- 重复 candidate distance：新 slot 与候选的 score 在 layer search 已经计算，Stage 6D 将其传入 neighbor selection。
- selected-neighbor distance：level0 满表时，最差邻居 score/position 被缓存，后续同一旧 slot 的接受判断避免重复扫描其当前邻接表。
- would_accept / heuristic select：策略仍是 “如果候选 score 大于当前 worst score，则替换 worst”，没有替换成不同 neighbor selection 策略。
- hot path malloc/free：未新增 per-edge malloc/free；cache 数组在 reserve 时分配。

## 8. 不变量检查

- no self edge：`slot == neighbor` 仍直接跳过。
- no duplicate edge：通用 fallback 仍执行 duplicate 检查；new slot 候选来自同次 layer search 的 visited 去重，且 new slot 邻接表从空开始。
- level0 / upper layer 双向边语义保持：只有双方都接受时才应用连接。
- deleted_flags 语义保持：有删除时 worst cache 不启用。
- record_id_to_slot 语义保持：未修改 id-slot map。
- search 输出仍为 `record_id + score`：VSEARCH search path 未改。
- core 不处理 scope / TTL / metadata / question / answer。
- manager 仍负责 response。

## 9. 1K before/after profile

| metric | Stage 6C before | Stage 6D after | change |
|---|---:|---:|---:|
| rebuild_core_add_total_us | 71,891 | 60,095 | -16.41% |
| hnsw_add_layer_search_us | 42,984 | 40,587 | -5.58% |
| hnsw_add_neighbor_select_us | 24,624 | 15,378 | -37.55% |
| hnsw_add_link_us | 946 | 1,021 | 7.93% |
| recall@10 | 1.0000 | 1.0000 | no regression |

- `results/vector_core_benchmark/stage6d_hnsw_neighbor_select_opt_benchmark_1k_64d.json`
- `results/vector_core_benchmark/stage6d_hnsw_neighbor_select_opt_benchmark_1k_64d.md`

## 10. 10K before/after profile

| metric | Stage 6C before | Stage 6D after | change |
|---|---:|---:|---:|
| rebuild_core_add_total_us | 1,059,757 | 910,539 | -14.08% |
| hnsw_add_layer_search_us | 730,361 | 682,628 | -6.54% |
| hnsw_add_neighbor_select_us | 267,038 | 168,132 | -37.04% |
| hnsw_add_link_us | 11,520 | 11,217 | -2.63% |
| recall@10 | 0.9870 | 0.9870 | no regression |

- `results/vector_core_benchmark/stage6d_hnsw_neighbor_select_opt_benchmark_10k_64d.json`
- `results/vector_core_benchmark/stage6d_hnsw_neighbor_select_opt_benchmark_10k_64d.md`

## 11. Pass 判断

- acceptable neighbor_select <= -8%：PASS，`-37.04%`。
- acceptable add_total <= -3%：PASS，`-14.08%`。
- strong neighbor_select <= -15%：PASS，`-37.04%`。
- strong add_total <= -5%：PASS，`-14.08%`。
- recall@10 >= 0.9870：PASS，`0.9870`。

结论：Stage 6D 达到 strong pass。

## 12. tests 结果

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

## 13. benchmark 结果

```bash
python3 scripts/bench_vector_core_compare.py --corrected --stage6d --kv-bin ./kvstore --n 1000 --dim 64 --topk 10 --queries 50
python3 scripts/bench_vector_core_compare.py --corrected --stage6d --kv-bin ./kvstore --n 10000 --dim 64 --topk 10 --queries 100
```

- 1K core_hnsw status：PASS，recall@10 `1.0000`。
- 10K core_hnsw status：PASS，recall@10 `0.9870`。
- 10K neighbor_select 与 add_total 达到 strong pass。

## 14. 静态边界检查结果

执行：

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
grep -RIn "record_id_map" src/vector/core include/vector 2>/dev/null
git diff -- src/persistence
```

- `src/vector/core` 未新增 `g_vector_state`。grep 输出仅包含 include 中既有 manager/internal 声明或注释。
- `src/vector/core` 未出现 scope / ttl / metadata / question / answer。
- `kvs_vector_index_ops_t` 签名未修改。
- `record_id_map` 未出现在 `src/vector/core`；仅 manager/record header 中存在。
- `git diff -- src/persistence` 为空，RDB/AOF 格式未修改。

## 15. git status --short

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
?? "docs/vector_core_reset/\344\274\230\345\214\226\346\255\245\351\252\244.md"
?? tests/vector_core_hnsw_rebuild_profile_test.sh
?? tests/vector_manager_resolve_fastpath_test.sh
?? tests/vector_vinfo_backend_stats_test.sh
?? tests/vector_vsearch_profile_breakdown_test.sh
```

## 16. 下一步建议

Next Stage:

```text
Stage 6E: core_hnsw layer_search remaining hotspot audit / targeted optimization
```

Reason:

1. Stage 6D 后 10K `hnsw_add_neighbor_select_us` 从 `267,038us` 降到 `168,132us`，已不再是唯一主要瓶颈。
2. 10K `hnsw_add_layer_search_us = 682,628us` 仍是最大项，约占 add total 的主要部分。
3. 下一步应继续在 layer_search 内部做 targeted audit/optimization，例如 distance buffer reuse、candidate heap push/pop、visited/tag hot path，而不是做 memory layout 大重构、graph persistence、scope-aware search 或 sharding。

