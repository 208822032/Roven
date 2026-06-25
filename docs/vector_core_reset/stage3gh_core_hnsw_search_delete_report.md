# Stage 3G + Stage 3H core_hnsw search/delete report

## 1. 修改 / 新增文件列表

本轮修改：

- `include/vector/kvs_vector_hnsw.h`
- `Makefile`

本轮新增：

- `src/vector/core/hnsw/kvs_hnsw_search.c`
- `src/vector/core/hnsw/kvs_hnsw_delete.c`
- `docs/vector_core_reset/stage3gh_core_hnsw_search_delete_report.md`

本轮新增临时 standalone 测试：

- `/tmp/kvs_hnsw_search_delete_test.c`

## 2. 是否修改生产代码

是。本轮在 core_hnsw 内新增 search / mark_delete API 实现，并把新增源文件加入 Makefile。

未修改 manager、VINDEX、RDB/AOF、旧 flat、旧 ANN、SQ8、Faiss、hnswlib 路径。

## 3. search API 和 search 流程说明

新增 API：

```c
int kvs_hnsw_index_search(
	kvs_hnsw_index_t *index,
	const float *query,
	size_t dim,
	size_t topk,
	kvs_vector_hit_t *out,
	size_t *out_count
);
```

行为：

- 校验 `index/query/out/out_count/dim/topk`。
- `dim` 必须等于 `index->dim`。
- 空索引或 `has_entry = 0` 时返回成功，`out_count = 0`。
- 返回结果只包含 `record_id + score`。
- 输出按 score 降序。
- search 内不调用 `kvs_malloc/kvs_free/malloc/free`。
- search 只访问 core_hnsw 自身结构，不访问 manager。

## 4. multilayer greedy search 说明

search 从 `entry_point` 开始，从 `max_level` 向下到 level 1 执行 greedy search：

- 每层只使用该层邻接。
- 在当前层反复选择 score 更高的可搜索邻居。
- 找不到更优邻居时下沉到下一层。
- deleted 节点不会作为 greedy 改进目标。
- 如果某层邻接为空，则直接下沉。

## 5. level0 ef_search 说明

level0 使用 ef_search：

- `ef = max(topk, index->ef_search)`。
- `ef` 不超过 context capacity。
- `candidate_heap` 使用 max heap。
- `top_heap` 使用 min heap。
- 使用 `visited_tags` 防重复访问。
- 遍历 `level0_neighbors`。
- deleted 节点可用于图扩展，但不会进入最终 top heap 和输出结果。
- 最后从 top heap 导出并按 score 降序写入 `out`。

## 6. context pool 使用说明

search 入口 acquire context，出口 release context。

context reset 会清空：

- `candidate_heap`
- `top_heap`
- `neighbor_buffer`
- `distance_buffer`
- `hits`

search 热路径不做动态内存分配。context capacity 不足时，ef 安全截断到 context capacity。

## 7. visited tag 使用说明

level0 search 使用：

- `kvs_hnsw_next_visit_tag(index)`
- `index->visited_tags[slot]`

用于避免同一次搜索重复访问同一 slot。tag 溢出由已有 helper 清空 visited tags 后恢复到 1。

## 8. deleted record 处理说明

search 会跳过 deleted record 的最终输出：

- deleted record 不进入 `top_heap`。
- deleted record 不会出现在 `kvs_vector_hit_t` 输出中。
- deleted record 的图边不做在线移除，避免本阶段引入 graph compaction。

## 9. mark_delete 策略

新增 API：

```c
int kvs_hnsw_index_mark_delete(
	kvs_hnsw_index_t *index,
	uint64_t record_id
);
```

行为：

- 通过 `record_id_to_slot` 找 slot。
- record_id 不存在时返回 map 的 not found 错误码。
- 已删除时幂等返回成功。
- 设置 `deleted_flags[slot] = 1`。
- 维护 `deleted_count`。
- 不删除 `record_id_to_slot` 映射。
- 不修改 graph 边。
- 不做 physical compact。
- 不做 slot relocation。

duplicate add 已删除 record_id 时沿用 Stage 3F 行为：覆盖向量、恢复 deleted flag、递减 `deleted_count`，不重连图。

## 10. standalone reset/rebuild 验证说明

本阶段没有实现 manager-level rebuild。

standalone 测试通过以下方式模拟 rebuild：

1. `kvs_hnsw_index_reset(index)`。
2. 保留已有 capacity。
3. 只对 active 测试数据重新调用 `kvs_hnsw_index_add(record_id, vector)`。
4. search 验证 active record 可查。
5. deleted record 不重新 add，因此不可查。

## 11. standalone search/delete 测试结果

执行：

```sh
gcc -Iinclude -Wall -Wextra -Werror \
  /tmp/kvs_hnsw_search_delete_test.c \
  /tmp/kvs_hnsw_test_memory.c \
  src/vector/core/kvs_vector_id_slot_map.c \
  src/vector/core/kvs_vector_heap.c \
  src/vector/core/hnsw/kvs_hnsw_index.c \
  src/vector/core/hnsw/kvs_hnsw_graph.c \
  src/vector/core/hnsw/kvs_hnsw_context.c \
  src/vector/core/hnsw/kvs_hnsw_info.c \
  src/vector/core/hnsw/kvs_hnsw_random.c \
  src/vector/core/hnsw/kvs_hnsw_add.c \
  src/vector/core/hnsw/kvs_hnsw_search.c \
  src/vector/core/hnsw/kvs_hnsw_delete.c \
  -lm \
  -o /tmp/kvs_hnsw_search_delete_test

/tmp/kvs_hnsw_search_delete_test
```

结果：

```text
kvs_hnsw_search_delete_test PASS
```

覆盖：

- init / reserve。
- add 60 条向量。
- search top1 返回最近 record_id。
- search topK 返回多个 record_id。
- search 结果按 score 降序。
- mark_delete 后 search 不返回 deleted record。
- `deleted_count` 增加一次。
- repeated mark_delete 不重复增加 `deleted_count`。
- mark_delete 不删除 `record_id_to_slot` 映射。
- duplicate add 已删除 record_id 后恢复 deleted flag。
- reset 后 search 返回空。
- reset 后重新 add/search 正常。
- reset + add 模拟 rebuild。
- info 返回 `size/deleted_count/memory_bytes` 合理。
- destroy 正常。

## 12. 静态边界检查结果

执行：

```sh
grep -RIn "g_vector_state" \
  include/vector/kvs_vector_hnsw.h \
  src/vector/core/hnsw 2>/dev/null
```

结果：无输出。

执行：

```sh
grep -RIn "question\\|answer\\|scope\\|TTL\\|metadata" \
  include/vector/kvs_vector_hnsw.h \
  src/vector/core/hnsw 2>/dev/null
```

结果：仅 `include/vector/kvs_vector_hnsw.h` 的边界注释出现。

执行：

```sh
rg -n "VINDEX|kvs_vector_manager|kvs_vector_search_result_t|record store|g_vector_state" \
  include/vector/kvs_vector_hnsw.h src/vector/core/hnsw
```

结果：无输出。

执行：

```sh
rg -n "kvs_malloc|kvs_free|malloc|free" src/vector/core/hnsw/kvs_hnsw_search.c
```

结果：无输出。

## 13. 全量构建结果

执行：

```sh
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：通过，输出 `✓ 构建完成`。

## 14. 当前 git status --short

```text
 M Makefile
 D docs/vector_core_reset/00_vector_core_slimming_report.md
 D docs/vector_core_reset/01_vector_manager_kernel_design.md
 D docs/vector_core_reset/02_third_party_vector_engine_audit.md
 D docs/vector_core_reset/03_kvlite_vector_manager_kernel_design.md
 D docs/vector_core_reset/04_vector_compute_kernel_design.md
 D docs/vector_core_reset/05_vector_backend_reselection_plan.md
 D docs/vector_core_reset/06_performance_first_self_hnsw_design.md
 D docs/vector_core_reset/07_usearch_inspired_flat_hnsw_core_execution_plan.md
?? docs/vector_core_reset/stage3abc_core_hnsw_lifecycle_context_report.md
?? docs/vector_core_reset/stage3de_core_hnsw_heap_random_report.md
?? docs/vector_core_reset/stage3f2_core_hnsw_multilayer_add_report.md
?? docs/vector_core_reset/stage3f_core_hnsw_add_report.md
?? include/vector/kvs_vector_hnsw.h
?? src/vector/core/hnsw/
```

旧设计文档删除为进入本轮前已有状态，本轮未处理。

## 15. 未接入项确认

本轮明确未做：

- 未接入 VINDEX core_hnsw。
- 未修改 manager 搜索路径。
- 未实现 manager-level rebuild。
- 未修改 RDB/AOF 格式。
- 未修改 `kvs_vector_index_ops_t` 签名。
- 未替换 hnswlib / faiss / old hnsw。
- 未做 benchmark。

core_hnsw 仍不访问 `g_vector_state`，不处理 scope / TTL / metadata / question / answer。
