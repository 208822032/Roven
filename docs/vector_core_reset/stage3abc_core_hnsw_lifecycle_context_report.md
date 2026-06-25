# Stage 3A/3B/3C core_hnsw lifecycle and context report

## Scope

本阶段只建立 core_hnsw 的目录、基础结构、生命周期和 search context pool。

本阶段明确未做：

- 未实现 HNSW add
- 未实现 HNSW search
- 未实现 random level
- 未实现 neighbor selection
- 未实现 mark_delete / rebuild
- 未接入 VINDEX core_hnsw
- 未修改 manager 搜索路径
- 未修改 RDB/AOF 格式
- 未修改 `kvs_vector_index_ops_t` 签名
- 未替换 hnswlib / faiss / old hnsw
- 未做 benchmark

## Modified / Added Files

新增：

```text
include/vector/kvs_vector_hnsw.h
src/vector/core/hnsw/kvs_hnsw_index.c
src/vector/core/hnsw/kvs_hnsw_graph.c
src/vector/core/hnsw/kvs_hnsw_context.c
src/vector/core/hnsw/kvs_hnsw_info.c
docs/vector_core_reset/stage3abc_core_hnsw_lifecycle_context_report.md
```

修改：

```text
Makefile
```

Makefile 只新增 core_hnsw 源文件编译项：

```text
src/vector/core/hnsw/kvs_hnsw_index.c
src/vector/core/hnsw/kvs_hnsw_graph.c
src/vector/core/hnsw/kvs_hnsw_context.c
src/vector/core/hnsw/kvs_hnsw_info.c
```

## Production Code Changes

本阶段有生产代码新增：

- 新增 core_hnsw header 和 source skeleton。
- 新增 Makefile 编译入口。

本阶段没有改现有 V 族命令行为，没有接入 VINDEX，也没有修改 manager 搜索路径。

## core_hnsw Structure

`kvs_hnsw_index_t` 定义在：

```text
include/vector/kvs_vector_hnsw.h
```

当前字段：

```text
dim
metric
M
M0
ef_construction
ef_search
capacity
size
vectors
levels
deleted_flags
slot_to_record_id
record_id_to_slot
level0_neighbors
level0_counts
upper_offsets
upper_neighbors
upper_counts
entry_point
has_entry
max_level
visited_tags
current_visit_tag
search_context_pool
distance
ready
needs_rebuild
deleted_count
```

默认参数：

```text
M = 16
M0 = 32
ef_construction = 128
ef_search = 128
```

边界语义：

- core_hnsw 只保存 record_id、向量、图结构和搜索临时缓冲。
- core_hnsw 不保存、不理解 question / answer / scope / TTL / metadata。
- record store 仍是唯一事实来源。

## Lifecycle Implementation

### init

实现文件：

```text
src/vector/core/hnsw/kvs_hnsw_index.c
```

行为：

- 校验 `index != NULL` 且 `dim > 0`。
- 清零结构体。
- 设置 `dim / metric`。
- 设置默认参数 `M / M0 / ef_construction / ef_search`。
- 初始化 entry point / max level / visit tag 状态。
- 设置 `ready = 1`，`needs_rebuild = 0`。

### reserve

实现文件：

```text
src/vector/core/hnsw/kvs_hnsw_index.c
```

预分配：

```text
vectors
levels
deleted_flags
slot_to_record_id
record_id_to_slot
level0_neighbors
level0_counts
upper_offsets
upper_counts
visited_tags
search_context_pool
```

说明：

- `upper_neighbors` 当前只保留字段，不分配实际上层邻接存储。
- 本阶段不实现 add/search，因此不构造图边。
- reserve 支持扩容并复制已有 lifecycle 字段。
- 所有内存使用 `kvs_malloc / kvs_free`。

### reset

实现文件：

```text
src/vector/core/hnsw/kvs_hnsw_index.c
```

行为：

- 清空 vectors / levels / deleted_flags / slot_to_record_id。
- 清空 level0 neighbor counts 和 level0 neighbor buffer。
- 清空 upper offsets / upper counts。
- 清空 visited tags。
- reset `record_id_to_slot`。
- 设置：

```text
size = 0
deleted_count = 0
entry_point = 0
has_entry = 0
max_level = 0
current_visit_tag = 1
ready = 1
needs_rebuild = 0
```

### info

实现文件：

```text
src/vector/core/hnsw/kvs_hnsw_info.c
```

返回：

```text
capacity
size
dim
metric_type
memory_bytes
ready
needs_rebuild
deleted_count
```

`memory_bytes` 统计当前已分配的向量数组、图结构、id-slot map、visited tags 和 context pool 缓冲。

### destroy

实现文件：

```text
src/vector/core/hnsw/kvs_hnsw_index.c
```

行为：

- 释放所有由 index 持有的数组。
- destroy `record_id_to_slot`。
- destroy `search_context_pool`。
- 清零结构体。
- 支持重复 destroy，不崩溃。

## Search Context Pool

实现文件：

```text
src/vector/core/hnsw/kvs_hnsw_context.c
```

新增类型：

```text
kvs_hnsw_search_context_t
kvs_hnsw_search_context_pool_t
```

context 包含：

```text
candidate_heap
top_heap
neighbor_buffer
distance_buffer
hits
capacity
in_use
```

实现 API：

```text
kvs_hnsw_context_pool_init
kvs_hnsw_context_pool_destroy
kvs_hnsw_context_acquire
kvs_hnsw_context_release
kvs_hnsw_context_reset
```

行为：

- pool init 时一次性分配 contexts 和每个 context 的 buffer。
- acquire 返回空闲 context，并 reset 临时缓冲。
- release reset context 并标记为空闲。
- 当前默认 pool context 数为 `2`。
- 当前 context capacity 使用 `ef_search`。
- 本阶段不执行 search，但接口和资源复用路径已可用。
- 未来 search 热路径可以复用该 context，避免临时 malloc/free。

## Visit Tag

实现：

```text
kvs_hnsw_next_visit_tag()
```

行为：

- 自增 `current_visit_tag`。
- 如果 tag 溢出为 `0`，清空 `visited_tags` 并重置为 `1`。

本阶段 standalone lifecycle 测试只验证基础 tag 推进和 reset 行为。

## Standalone Lifecycle Test

临时测试文件：

```text
/tmp/kvs_hnsw_lifecycle_test.c
```

临时内存 shim：

```text
/tmp/kvs_hnsw_test_memory.c
```

说明：

- core_hnsw 生产代码使用 `kvs_malloc / kvs_free`。
- standalone 测试用 `/tmp/kvs_hnsw_test_memory.c` 提供 `mem_alloc / mem_free` shim，避免引入完整 kvstore 运行时。

编译命令：

```bash
gcc -Iinclude -Wall -Wextra -Werror \
  /tmp/kvs_hnsw_lifecycle_test.c \
  /tmp/kvs_hnsw_test_memory.c \
  src/vector/core/kvs_vector_id_slot_map.c \
  src/vector/core/kvs_vector_heap.c \
  src/vector/core/hnsw/kvs_hnsw_index.c \
  src/vector/core/hnsw/kvs_hnsw_context.c \
  src/vector/core/hnsw/kvs_hnsw_info.c \
  src/vector/core/hnsw/kvs_hnsw_graph.c \
  -lm \
  -o /tmp/kvs_hnsw_lifecycle_test
```

执行结果：

```text
kvs_hnsw_lifecycle_test PASS
```

覆盖：

- init
- reserve
- info
- context acquire/release
- pool exhausted 返回 NULL
- reset
- reset 后 info
- destroy
- 多轮 init/destroy
- 重复 destroy 不崩溃

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
- core_hnsw 逻辑代码没有访问这些业务语义。
- core_hnsw 没有访问 `g_vector_state`。

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

Makefile 已将 core_hnsw 四个源文件加入构建：

```text
src/vector/core/hnsw/kvs_hnsw_index.o
src/vector/core/hnsw/kvs_hnsw_graph.o
src/vector/core/hnsw/kvs_hnsw_context.o
src/vector/core/hnsw/kvs_hnsw_info.o
```

## Current Git Status

执行：

```bash
git status --short
```

当前输出：

```text
 M Makefile
?? docs/vector_core_reset/stage3abc_core_hnsw_lifecycle_context_report.md
?? include/vector/kvs_vector_hnsw.h
?? src/vector/core/hnsw/
```
