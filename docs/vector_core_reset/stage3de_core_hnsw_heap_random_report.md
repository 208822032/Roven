# Stage 3D/3E core_hnsw heap and random report

## Scope

本阶段完成：

- 确认 HNSW 复用通用 `kvs_vector_heap`
- 实现 HNSW deterministic random level
- 为后续 Stage 3F HNSW add 做准备

本阶段明确未做：

- 未实现 HNSW add
- 未实现 HNSW search
- 未实现 neighbor selection
- 未实现 mark_delete / rebuild
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
src/vector/core/hnsw/kvs_hnsw_index.c
```

新增：

```text
src/vector/core/hnsw/kvs_hnsw_random.c
docs/vector_core_reset/stage3de_core_hnsw_heap_random_report.md
```

## Production Code Changes

本阶段有生产代码新增和小范围修改：

- `kvs_hnsw_random_t`
- deterministic PRNG
- random level API
- `kvs_hnsw_index_t.rng`
- `kvs_hnsw_index_next_level()`
- Makefile 新增 `src/vector/core/hnsw/kvs_hnsw_random.c`

本阶段没有修改 V 族命令行为，没有接入 VINDEX，没有改 manager 搜索路径。

## Heap Reuse Confirmation

已审查：

```text
include/vector/kvs_vector_heap.h
src/vector/core/kvs_vector_heap.c
```

当前通用 heap 已满足 HNSW 后续 search context 需求：

- 支持 min heap：`kvs_vector_heap_min_score_less`
- 支持 max heap：`kvs_vector_heap_max_score_less`
- 支持 `push`
- 支持 `pop`
- 支持 `top`
- 支持 `replace_top`
- 支持 `reset`
- 不依赖 C++ STL
- 使用 `kvs_malloc / kvs_free`
- 可用于 `candidate_heap`
- 可用于 `top_heap`
- 可被 search context pool 预分配和复用

结论：

- 不新增重复 hnsw 专用 heap。
- HNSW 继续复用通用 `kvs_vector_heap`。
- 当前 `kvs_hnsw_search_context_t` 已包含：

```text
candidate_heap
top_heap
neighbor_buffer
distance_buffer
hits
```

context init 已预分配 candidate/top heap；context reset/release 会清空 heap size，不在未来 search 热路径做 heap malloc/free。

## Random Level Implementation

新增文件：

```text
src/vector/core/hnsw/kvs_hnsw_random.c
```

新增类型：

```c
typedef struct kvs_hnsw_random_s {
	uint64_t state;
} kvs_hnsw_random_t;
```

新增 API：

```text
kvs_hnsw_random_init
kvs_hnsw_random_next_u32
kvs_hnsw_random_next_unit
kvs_hnsw_random_level
kvs_hnsw_index_next_level
```

实现说明：

- 使用 splitmix64 风格 deterministic PRNG。
- 不使用 `rand()`。
- 不使用全局随机状态。
- 同 seed 输出可复现。
- `kvs_hnsw_random_next_unit()` 返回 `[0, 1)` float。
- `kvs_hnsw_random_level()` 使用 `1 / M` 阈值循环生成递减层级。
- `M < 2` 时回退到 `KVS_HNSW_DEFAULT_M = 16`。
- 最大 level 使用 `KVS_HNSW_MAX_LEVEL = 32` 限制，避免极端无限增长。

`kvs_hnsw_index_t` 新增：

```text
rng
```

`kvs_hnsw_index_init()` 初始化 rng：

```text
seed = 0x9e3779b97f4a7c15 ^ (dim << 32) ^ metric
```

本阶段只提供 helper，暂时不在 add 中使用，因为 Stage 3F 才实现 add。

## Random Level Distribution Test

临时测试：

```text
/tmp/kvs_hnsw_random_heap_test.c
```

覆盖：

- 同 seed 下 `next_u32` 输出可复现。
- 连续 `next_u32` 输出不全相同。
- `next_unit` 在 `[0, 1)`。
- 采样 20000 次 random level。
- level 0 数量最多。
- level 1、2、3 呈递减趋势。
- level 不超过 `KVS_HNSW_MAX_LEVEL`。

执行结果：

```text
kvs_hnsw_random_heap_test PASS
```

## Context Heap Reset / Reuse Test

同一临时测试覆盖：

- acquire 后 `candidate_heap` 和 `top_heap` 均可用。
- `candidate_heap` 使用 max heap comparator。
- `top_heap` 使用 min heap comparator。
- 覆盖 `push / pop / top / replace_top`。
- release 后 heap size 被 reset 为 0。
- 再次 acquire 可复用同一个 context。
- 多轮 init/destroy 不崩溃。

执行结果：

```text
kvs_hnsw_random_heap_test PASS
```

## Standalone Build Command

测试编译命令：

```bash
gcc -Iinclude -Wall -Wextra -Werror \
  /tmp/kvs_hnsw_random_heap_test.c \
  /tmp/kvs_hnsw_test_memory.c \
  src/vector/core/kvs_vector_id_slot_map.c \
  src/vector/core/kvs_vector_heap.c \
  src/vector/core/hnsw/kvs_hnsw_index.c \
  src/vector/core/hnsw/kvs_hnsw_graph.c \
  src/vector/core/hnsw/kvs_hnsw_context.c \
  src/vector/core/hnsw/kvs_hnsw_info.c \
  src/vector/core/hnsw/kvs_hnsw_random.c \
  -lm \
  -o /tmp/kvs_hnsw_random_heap_test
```

说明：

- `/tmp/kvs_hnsw_test_memory.c` 是 standalone 测试使用的 `mem_alloc / mem_free` shim。
- 生产代码仍统一使用 `kvs_malloc / kvs_free`。

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

结论：

- 业务字段只出现在边界注释中。
- core_hnsw 逻辑代码没有处理这些业务语义。
- core_hnsw 没有访问 `g_vector_state`。

额外检查：

```bash
rg -n "kvs_hnsw_index_add|kvs_hnsw_index_search|neighbor selection|VINDEX|rand\\(" \
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
src/vector/core/hnsw/kvs_hnsw_random.o
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
?? include/vector/kvs_vector_hnsw.h
?? src/vector/core/hnsw/
```

说明：

- `docs/vector_core_reset/00...07...` 的删除状态不是本阶段改动内容，本阶段未恢复或修改这些文件。
