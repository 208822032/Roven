# Stage 3F core_hnsw add report

## Scope

本阶段实现 core_hnsw 的 add 路径，使 `record_id + vector` 可以插入 HNSW index，并建立基础 level0 邻接关系。

本阶段明确未做：

- 未实现对外 HNSW search
- 未实现 mark_delete
- 未实现 rebuild
- 未接入 VINDEX core_hnsw
- 未修改 manager 搜索路径
- 未修改 RDB/AOF 格式
- 未修改 `kvs_vector_index_ops_t` 签名
- 未替换 hnswlib / faiss / old hnsw
- 未做 benchmark

## Modified / Added Files

修改：

```text
Makefile
include/vector/kvs_vector_hnsw.h
```

新增：

```text
src/vector/core/hnsw/kvs_hnsw_add.c
docs/vector_core_reset/stage3f_core_hnsw_add_report.md
```

## Production Code Changes

本阶段新增生产代码：

- `kvs_hnsw_index_add()`
- level0 邻居候选排序
- level0 双向边写入
- level0 满邻居时的简单替换
- IP / L2 / COSINE score helper

本阶段没有修改现有 V 族命令行为，没有接入 manager，没有修改持久化格式。

## Public Add API

新增声明：

```c
int kvs_hnsw_index_add(
	kvs_hnsw_index_t *index,
	uint64_t record_id,
	const float *vector,
	size_t dim
);
```

实现文件：

```text
src/vector/core/hnsw/kvs_hnsw_add.c
```

校验：

- `index != NULL`
- `vector != NULL`
- `dim > 0`
- `dim == index->dim`
- `index->ready == 1`
- 已 reserve 必要数组
- capacity 不足时返回错误，不自动扩容

边界：

- 不访问 `g_vector_state`
- 不处理 question / answer / scope / TTL / metadata
- 只处理 record_id / vector / graph

## Duplicate record_id Strategy

策略：

- 如果 `record_id` 已存在：
  - 覆盖 `vectors[slot * dim]`
  - 不重建 graph 边
  - 不增加 `size`
  - 保持 `slot_to_record_id[slot] = record_id`
  - 如果该 slot 已被 `deleted_flags` 标记，则恢复为未删除，并递减 `deleted_count`
  - 返回成功

选择该策略的原因：

- 本阶段目标是 add 基础图构建。
- duplicate graph update 会引入复杂边重连问题。
- 第一版用“覆盖向量，不重连图”保持行为简单稳定。

## First Node Behavior

第一个节点插入时：

- `slot = 0`
- 写入 vector
- 写入 `slot_to_record_id[0]`
- 写入 `record_id_to_slot`
- 写入 `levels[0]`
- `size = 1`
- `entry_point = 0`
- `has_entry = 1`
- `max_level = new_level`
- 不写任何邻居边
- 返回成功

## Level0 Neighbor Strategy

本阶段只完整维护 level0 图。

存储：

- `level0_neighbors[slot * M0 + i]`
- `level0_counts[slot]`
- 每个 slot 最多 `M0` 个 level0 邻居

候选：

- 对所有现有 active slot 计算 score。
- 按 score 从高到低排序。
- 选择前 `M0` 个作为候选邻居。

双向边：

- 先尝试让已有 neighbor 接受 new slot。
- 如果 neighbor 未满，直接追加。
- 如果 neighbor 已满，仅当 new slot score 优于该 neighbor 当前最差边时替换。
- 如果替换旧边，会同步移除旧节点到该 neighbor 的反向边。
- 只有 neighbor 接受后，才给 new slot 写反向边。
- 禁止 self edge。
- 禁止重复边。

该策略保证测试范围内 level0 双向边基本一致。

## Upper Layer Strategy

本阶段采用低风险方案 A：

- `new_level > 0` 时只写入 `levels[slot]`。
- 如果 `new_level > max_level`，更新：
  - `max_level`
  - `entry_point`
- 暂不写 upper layer 邻接。

原因：

- Stage 3A/B 只建立了 upper layer 基础字段。
- 当前阶段目标是 add 路径和 level0 稳定性。
- upper layer 内存模型和邻接写入留到后续阶段补齐。

## Neighbor Selection

当前是简单选择策略：

- 候选按 score 降序排序。
- 选择前 `M0` 个。
- 不实现 heuristic neighbor selection。
- 不做 diversity pruning。
- 不做复杂图优化。

## Score Semantics

score 方向与 core_flat 一致：越大越好。

当前实现：

- IP：dot score，越大越好
- L2：negative squared distance，越大越好
- COSINE：cosine similarity，越大越好

如果 `index->distance` 已设置，则优先使用该函数。

## Standalone Add Test

临时测试：

```text
/tmp/kvs_hnsw_add_test.c
```

临时内存 shim：

```text
/tmp/kvs_hnsw_test_memory.c
```

编译命令：

```bash
gcc -Iinclude -Wall -Wextra -Werror \
  /tmp/kvs_hnsw_add_test.c \
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
  -o /tmp/kvs_hnsw_add_test
```

执行结果：

```text
kvs_hnsw_add_test PASS
```

覆盖：

- init
- reserve
- reserve 前 add 返回参数错误
- dim 不匹配返回参数错误
- add 第一个节点
- `size = 1`
- `has_entry = 1`
- `entry_point = 0`
- add 多个节点
- `size` 正确增长
- `slot_to_record_id` 正确
- `record_id_to_slot` 可查
- `level0_counts` 合理
- 无 self edge
- 无重复边
- 双向边基本一致
- duplicate record_id add 后 `size` 不增加
- duplicate record_id add 后 vector 被覆盖
- duplicate deleted slot 后恢复 deleted flag 和 deleted_count
- info 返回 size / capacity / memory_bytes 合理
- reset / destroy 正常

## Static Boundary Checks

执行：

```bash
grep -RIn "g_vector_state" \
  include/vector/kvs_vector_hnsw.h \
  src/vector/core/hnsw 2>/dev/null
```

结果：无输出。

执行：

```bash
grep -RIn "question\\|answer\\|scope\\|TTL\\|metadata" \
  include/vector/kvs_vector_hnsw.h \
  src/vector/core/hnsw 2>/dev/null
```

结果：

```text
include/vector/kvs_vector_hnsw.h:20: *   question / answer / scope / TTL / metadata / manager store
```

额外检查：

```bash
rg -n "kvs_hnsw_index_search|VINDEX|kvs_vector_manager|kvs_vector_search_result_t" \
  include/vector/kvs_vector_hnsw.h src/vector/core/hnsw
```

结果：无输出。

## Full Build

执行：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：

```text
✓ 构建完成
二进制: kvstore src/eBPF/ebpf_syncd
eBPF:   src/eBPF/ebpf_kern.o
```

Makefile 已包含：

```text
src/vector/core/hnsw/kvs_hnsw_add.o
```

## Current Git Status

执行：

```bash
git status --short
```

当前输出：

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

说明：

- `docs/vector_core_reset/00...07...` 的删除状态不是本阶段改动内容。
- 本阶段未恢复、未修改这些 unrelated docs 删除。
