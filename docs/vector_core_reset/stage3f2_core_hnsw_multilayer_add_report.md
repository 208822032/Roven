# Stage 3F-2 core_hnsw multilayer add report

## 1. 修改 / 新增文件列表

本轮修改：

- `src/vector/core/hnsw/kvs_hnsw_index.c`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/core/hnsw/kvs_hnsw_info.c`

本轮新增临时测试：

- `/tmp/kvs_hnsw_multilayer_add_test.c`

本轮新增报告：

- `docs/vector_core_reset/stage3f2_core_hnsw_multilayer_add_report.md`

未修改 manager、VINDEX、RDB/AOF、旧 flat、旧 ANN、SQ8、Faiss、hnswlib 路径。

## 2. upper layer 内存模型

level0 继续使用独立存储：

- `level0_neighbors[slot * M0 + i]`
- `level0_counts[slot]`

upper layer 使用固定上限存储：

- `upper_neighbors[(slot * KVS_HNSW_MAX_LEVEL + level) * M + i]`
- `upper_counts[slot * KVS_HNSW_MAX_LEVEL + level]`
- `level` 从 1 开始，level0 不进入 upper storage。

`kvs_hnsw_index_reserve()` 现在一次性分配 upper layer 邻接区，`reset()` 清空所有 upper counts / neighbors，`destroy()` 继续释放 upper storage。`kvs_hnsw_index_info()` 已把 `upper_neighbors` 和多层 `upper_counts` 纳入 `memory_bytes`。

## 3. add 流程补齐说明

`kvs_hnsw_index_add()` 保持原签名不变。本轮补齐：

- 新记录写入 slot / vector / record_id / level。
- 第一条记录设置 `entry_point`、`has_entry`、`max_level`。
- 非第一条记录从旧 `entry_point` 开始。
- 对 `old_max_level > level > new_level` 的层执行 greedy traversal。
- 对 `min(new_level, old_max_level) ... 0` 的层执行 add-time candidate search。
- 每层按 score 降序选择前 `M` 或 `M0`。
- 写入 layer-aware 双向边。
- 当 `new_level > old_max_level` 时，最后更新 `entry_point` 和 `max_level`。

duplicate `record_id` 仍保持 Stage 3F 行为：覆盖向量，不增加 `size`，不重建 graph。

## 4. greedy search layer helper

新增内部静态 helper：

- `kvs_hnsw_greedy_search_layer(...)`

该 helper 只在 add 内部用于高层下降，从当前 entry slot 开始，在指定层不断选择 score 更高的邻居，直到没有更优邻居。它不导出 public search API，也不返回 `kvs_vector_search_result_t`。

## 5. add-time candidate search helper

新增内部静态 helper：

- `kvs_hnsw_search_layer_for_add(...)`

该 helper 只服务 add：

- 使用 `ef_construction` 控制候选规模。
- 复用 `search_context_pool` 的 `candidate_heap` / `top_heap`。
- 使用 `visited_tags` 防重复访问。
- 输出 slot + score 到 context 的 `neighbor_buffer` / `distance_buffer`。

`reserve()` 初始化 context pool 时使用 `max(ef_search, ef_construction)`，避免 add 阶段候选容量小于 `ef_construction`。

## 6. neighbor selection 策略

本阶段采用简单策略：

- 候选按 score 降序。
- level0 选择前 `M0`。
- upper level 选择前 `M`。
- 不做 diversity heuristic。
- 不做 complex pruning。
- 不做 graph compaction。

## 7. 双向边维护策略

新增 layer-aware 双向边 helper：

- 禁止 self edge。
- 禁止 duplicate edge。
- 按层选择 `M0` 或 `M` 容量。
- 邻居未满时追加。
- 邻居已满时，只在新邻居 score 优于当前最差邻居时替换。
- 双向边在双方都可接受时才写入。
- 替换旧邻居时同步移除旧反向边，保持基本双向一致。

## 8. standalone multilayer add 测试结果

执行：

```sh
gcc -Iinclude -Wall -Wextra -Werror \
  /tmp/kvs_hnsw_multilayer_add_test.c \
  /tmp/kvs_hnsw_test_memory.c \
  src/vector/core/kvs_vector_id_slot_map.c \
  src/vector/core/kvs_vector_heap.c \
  src/vector/core/hnsw/kvs_hnsw_index.c \
  src/vector/core/hnsw/kvs_hnsw_graph.c \
  src/vector/core/hnsw/kvs_hnsw_context.c \
  src/vector/core/hnsw/kvs_hnsw_info.c \
  src/vector/core/hnsw/kvs_hnsw_random.c \
  src/vector/core/hnsw/kvs_hnsw_add.c \
  -lm \
  -o /tmp/kvs_hnsw_multilayer_add_test

/tmp/kvs_hnsw_multilayer_add_test
```

结果：

```text
kvs_hnsw_multilayer_add_test PASS
```

覆盖项：

- init / reserve。
- 固定 seed。
- add 220 条记录。
- 存在 `level > 0` 节点。
- `max_level` / `entry_point` 合理。
- level0 / upper layer 无 self edge。
- level0 / upper layer 无 duplicate edge。
- upper layer 只连接 `level >= 当前层` 的节点。
- 双向边基本一致。
- duplicate record_id add 不增加 `size`。
- reset 后 `level0_counts` / `upper_counts` 清零。
- destroy / repeated destroy 不崩溃。

## 9. 静态边界检查结果

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
rg -n "kvs_hnsw_index_search|VINDEX|kvs_vector_manager|kvs_vector_search_result_t" \
  include/vector/kvs_vector_hnsw.h src/vector/core/hnsw
```

结果：无输出。

## 10. 全量构建结果

执行：

```sh
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：通过，输出 `✓ 构建完成`。

## 11. 当前 git status --short

执行时状态：

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
?? docs/vector_core_reset/stage3f_core_hnsw_add_report.md
?? include/vector/kvs_vector_hnsw.h
?? src/vector/core/hnsw/
```

其中旧设计文档删除为既有无关状态，本轮未处理。

## 12. 未实现内容确认

本轮仍未实现：

- 对外 HNSW search API。
- mark_delete。
- rebuild。
- VINDEX core_hnsw 接入。
- manager 搜索路径修改。
- RDB/AOF 格式修改。
- `kvs_vector_index_ops_t` 签名修改。
- hnswlib / faiss / old hnsw 替换。
- benchmark。

core_hnsw 仍不访问 `g_vector_state`，不处理 scope / TTL / metadata / question / answer。
