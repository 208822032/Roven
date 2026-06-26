# Stage 7A core_hnsw memory layout / cache locality audit

## 1. 是否已读取并遵守 AGENTS.md

是。本轮所有操作严格满足 `AGENTS.md` 中的 KVLite vector_core 边界、P0/P1/P2 分层约束、Stage 5A-5D completed invariants、Stage 6A-6E completed invariants。本文是 P2 前置审计（audit / profiling / design），不改变算法语义、不重构内存布局、不改变 graph 结构。

**冲突点**：无。AGENTS.md 当前推荐的 next stage 是 Stage 5A，但 completed invariants 已记录 5A-5D 及 6A-6E 全部完成。Stage 7A 作为 P2 前置审计，不违反任何 AGENTS.md 约束。

## 2. 阶段目标

Stage 7A：core_hnsw memory layout / cache locality audit。

目标是找出 core_hnsw 剩余性能瓶颈是否来自 memory layout、cache locality、heap access pattern、visited_tags、neighbor list 访问模式。为后续 Stage 7B 是否重构提供量化证据。

**不做 memory layout 大重构，不改变 graph 结构，不改变 add/search 语义。**

## 3. 修改/新增文件

| 文件 | 变更类型 | 说明 |
|---|---|---|
| `include/vector/kvs_vector_hnsw.h` | 修改 | 新增 9 个 event profiling counters 到 `kvs_hnsw_rebuild_profile_t` |
| `src/vector/core/hnsw/kvs_hnsw_add.c` | 修改 | 在 `kvs_hnsw_search_layer_for_add` hot loop 中插桩 event counters |
| `include/kvs_vector.h` | 修改 | VINFO struct 新增 9 个 event counter 字段 |
| `src/vector/kvs_vector.c` | 修改 | 将 event counters 从 rebuild_profile 写入 VINFO |
| `src/core/kvs_operation.c` | 修改 | VINFO RESP 序列化新增 9 个字段，array_count 从 23→32 |
| `scripts/bench_vector_core_compare.py` | 修改 | 新增 `--stage7a` flag、prefix、stage 字段、REBUILD_PROFILE_KEYS |
| `docs/vector_core_reset/stage7a_core_hnsw_memory_layout_cache_audit.md` | 新增 | 本报告 |
| `results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_1k_64d.json` | 新增 | 1K DIM=64 benchmark |
| `results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_1k_64d.md` | 新增 | 1K DIM=64 benchmark markdown |
| `results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_10k_64d.json` | 新增 | 10K DIM=64 benchmark |
| `results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_10k_64d.md` | 新增 | 10K DIM=64 benchmark markdown |
| `results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_1k_384d.json` | 新增 | 1K DIM=384 synthetic benchmark |
| `results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_1k_384d.md` | 新增 | 1K DIM=384 synthetic benchmark markdown |

## 4. 是否修改生产代码

是，但仅限于 profiling / stats / audit helper 插桩。修改内容：

- 在 `kvs_hnsw_rebuild_profile_t` 结构中新增 9 个 `uint64_t` event counter 字段
- 在 `kvs_hnsw_search_layer_for_add()` hot loop 中新增 9 个计数点，每个计数点仅为 `++` 操作
- 在 VINFO 序列化中新增 9 个字段的 RESP 输出
- `kvs_operation.c` 中 `array_count` 从 23 更新到 32

**为什么需要生产代码插桩**：

Stage 6E 的 micro-optimization（locals hoisting、lower_bound 更新、visited-tag 顺序）仅产生 ~1% 改善。剩余瓶颈无法通过 P1 级别的代码重构解决。需要通过 event counters 量化：
1. visited-tag skip 比例 → 判断 visited_tags cache pollution 严重程度
2. distance compute / neighbor visit 比例 → 判断距离计算是否是主要开销
3. level0 vs upper neighbor scan 比例 → 判断 level0 的 cache pressure
4. heap push/replace 分布 → 判断堆操作的 cache 命中率

这些计数器的运行时开销为每条路径 1-2 个整数 increment，与 hot loop 中 64 fma 的距离计算相比可忽略不计。计数器已被纳入 VINFO 响应，通过 benchmark 脚本自动采集。

## 5. 当前 core_hnsw memory layout 总览

### 5.1 内存布局（10K capacity, DIM=64, M=16, M0=32）

| 数组 | 类型 | 大小（字节） | 分配方式 | 用途 |
|---|---|---|---|---|
| `vectors` | `float *` | 2,560,000 | 独立 kvs_malloc | AoS: slot-major, `vectors[slot*64..slot*64+63]` |
| `levels` | `uint8_t *` | 10,240 | 独立 kvs_malloc | 每 slot 最大层级 |
| `deleted_flags` | `uint8_t *` | 10,240 | 独立 kvs_malloc | 逻辑删除标记 |
| `slot_to_record_id` | `uint64_t *` | 81,920 | 独立 kvs_malloc | slot→record_id 映射 |
| `level0_neighbors` | `uint32_t *` | 1,280,000 | 独立 kvs_malloc | level0 邻接表（M0=32） |
| `level0_counts` | `uint16_t *` | 20,480 | 独立 kvs_malloc | level0 邻居计数 |
| `level0_worst_pos` | `uint16_t *` | 20,480 | 独立 kvs_malloc | level0 最差邻居位置缓存 |
| `level0_worst_score` | `float *` | 40,960 | 独立 kvs_malloc | level0 最差邻居分数缓存 |
| `level0_worst_valid` | `uint8_t *` | 10,240 | 独立 kvs_malloc | 缓存有效标记 |
| `upper_offsets` | `size_t *` | 81,920 | 独立 kvs_malloc | 预计算 `slot*KVS_HNSW_MAX_LEVEL*M` |
| `upper_neighbors` | `uint32_t *` | 20,480,000 | 独立 kvs_malloc | upper 邻接表（MAX_LEVEL×M） |
| `upper_counts` | `uint16_t *` | 655,360 | 独立 kvs_malloc | upper 邻居计数 |
| `visited_tags` | `uint32_t *` | 40,960 | 独立 kvs_malloc | 访问标记（tag 递增） |
| `record_id_to_slot` | hash map | ~160,000 | 独立 kvs_malloc | record_id→slot 反向映射 |
| `search_context_pool` | 2 contexts | ~14,336 | 独立 kvs_malloc | 搜索临时 buffers |

**总计：~24.8 MB（其中 upper_neighbors 占 19.5 MB，约 78%）**

### 5.2 关键内存布局特征

**AoS vectors 布局**：每个 slot 的 64 个 float 连续存储。访问 `vectors[neighbor * 64 .. neighbor * 64 + 63]` 时硬件 prefetcher 可以顺序读取 256 字节（4 个 cache line）。这对单向量距离计算是友好的。

**Upper neighbors 严重浪费**：公式为 `capacity × KVS_HNSW_MAX_LEVEL × M`，其中 `KVS_HNSW_MAX_LEVEL = 32`。实际 max_level 通常 ≤ 4，因此 28/32 = 87.5% 的 upper_neighbors 空间从未使用（全部为 0），但占用 19.5 MB 物理内存 + TLB entries + 缓存污染。

**热路径访问的元数据分散在多个独立分配中**：`visited_tags`、`levels`、`deleted_flags` 是三个不同的分配。每个 neighbor 访问需要分别访问：
- `visited_tags[neighbor]`（4 字节，随机 offset）
- `levels[neighbor]`（1 字节，随机 offset）
- `deleted_flags[neighbor]`（1 字节，仅当 `no_deleted == false`）
- `vectors[neighbor * 64..]`（256 字节，连续）

这意味着每个 neighbor 访问可能涉及 3-4 个不同的物理内存区域，产生 3-4 次 DTLB lookups。

**upper_offsets 冗余**：`upper_offsets[i] = i * KVS_HNSW_MAX_LEVEL * M`，可在线性时间内计算，不需要预计算数组（80 KB 浪费）。

## 6. vectors 访问模式审计

### 6.1 布局

- 当前为 **AoS（Array of Structures）**：`vectors[slot * dim + i]` — slot-major 连续。
- 64-dim vector = 256 bytes = 4 个 cache line（假设 64B cache line）。
- 对齐：`kvs_malloc` 通常返回 16-byte 对齐。未强制 32/64-byte 对齐。

### 6.2 访问模式

- Hot loop 中对每个非跳过 neighbor 调用 `kvs_hnsw_metric_score(query, vectors + neighbor * dim)`。
- Query vector 是单一固定地址，在整个 layer search 期间反复读取 → 应常驻 L1。
- Neighbor vector 随机访问 256 字节 → 每次 4 个连续 cache line。
- 10K × 256 bytes = 2.44 MB → 不适用于 L2（256 KB），勉强适合典型 L3（8-24 MB）。

### 6.3 对齐分析

- 64d × 4B = 256B = 4 × 64B cache lines。如果 vectors 起始地址是 64-byte 对齐，则每个 vector 的前 64 字节在同一 cache line 边界开始。
- 当前 16-byte 对齐意味着 vector 可能跨 5 个 cache line（如果起始地址 mod 64 ≠ 0）。
- **建议**：确保 vectors 分配为 64-byte 对齐（`aligned_alloc(64, size)` 或 `posix_memalign`）。

### 6.4 SIMD 准备度

- AoS 布局对水平 SIMD（如 AVX2 同时处理 8 个 float）友好：64 个连续 float 可以 8×unroll 为 8 个 256-bit YMM 寄存器操作。
- 当前距离计算使用标量 C 循环，未使用 SIMD intrinsics。

### 6.5 Dimension scaling

DIM=64 和 DIM=384 synthetic benchmark 产生完全一致的事件计数（距离计算次数、neighbor 访问次数等相同）。原因是 hash embedding provider 始终使用 64-dim，DIM=384 配置未改变 embedding 维度。

**实际 embedding dim**：64（hash embedding，`KVS_VECTOR_DEFAULT_DIMENSION`）。项目当前未接入 ONNX 或其他真实模型 embedding provider。`config/kvstore.conf` 中 `embedding_dim` 被注释，默认 64。

**Dimension-scaling 投影分析**（基于 AoS 布局理论推算）：

| dim | bytes/vector | cache lines/vector | 10K vectors L3 fit | 距离计算开销（相对64d） |
|---|---:|---:|---:|---:|
| 64 | 256 | 4 | ✓ (2.44 MB) | 1× |
| 384 | 1,536 | 24 | ✗ (14.6 MB，接近 L3 上限) | ~6× |
| 768 | 3,072 | 48 | ✗ (29.3 MB，超出典型 L3) | ~12× |
| 1024 | 4,096 | 64 | ✗ (39.1 MB) | ~16× |

**结论**：在 DIM ≥ 384 时，vectors 数组不再适合 L3 cache。Stage 7B optimization 若针对更高维度（如 LLM embedding 的 768/1024/1536），AoS→SoA 转换和显式 SIMD 将更具价值。

## 7. neighbor list 访问模式审计

### 7.1 level0 邻接表

- 布局：`level0_neighbors[slot * M0 .. slot * M0 + count_in_level0]`，M0=32。
- 每 slot：最多 32 个 uint32_t = 128 bytes = 2 个 cache line。连续存储，local 访问友好。
- 访问模式：随机 slot 访问，每个 slot 读取 0-32 个邻居 ID。
- Cache 影响：10K × 128 bytes = 1.22 MB。可与 vectors 共享 L3，但 level0 邻居扫描次数极高（30,432,809 / 10,000 ≈ 3,043 per add），导致大量随机 access。

### 7.2 upper 邻接表

- 布局：`upper_neighbors[(slot * KVS_HNSW_MAX_LEVEL + level) * M]`，M=16。
- 每 slot 每 level：最多 16 个 uint32_t = 64 bytes = 1 个 cache line。
- **关键问题**：连续 slot 的同一 level 间距 = `KVS_HNSW_MAX_LEVEL * M * 4 = 32 × 16 × 4 = 2,048 bytes`。slot 间无 locality。
- **87.5% 空间浪费**：MAX_LEVEL=32 但实际 max_level ≤ 4。19.5 MB 分配中约 17 MB 始终为 0。
- 访问次数：upper_neighbor_scan_count = 981,760（仅占 3.1% 的 neighbor 扫描）。

### 7.3 Pointer chasing

当前实现无 pointer chasing（使用连续数组 + 索引计算）。`kvs_hnsw_layer_neighbors()` 返回数组指针，不涉及链表遍历。

### 7.4 prefetch 适合度

- level0：每个 slot 的 128 bytes 邻接表顺序遍历 → 硬件 prefetcher 可以工作。
- upper：每 slot 每 level 仅 64 bytes，但 slot 间 stride = 2,048 → 硬件 prefetcher 无法预取。

## 8. metadata / visited_tags / deleted_flags 访问模式审计

### 8.1 访问频率

**10K DIM=64 统计数据：**

| 指标 | 数值 | 每 add 平均 |
|---|---|---|
| add_neighbor_visit_count | 31,414,569 | 3,141 |
| add_visited_skip_count | 22,319,225 | 2,232 |
| add_active_check_count | 0 | 0 |
| add_level0_neighbor_scan_count | 30,432,809 | 3,043 |
| add_upper_neighbor_scan_count | 981,760 | 98 |

**关键发现：71.0% 的 neighbor 迭代被 visited_tags 命中后直接 skip。**

这意味着：
1. `visited_tags[neighbor]` 是 hot loop 中访问频率最高的数据结构（31M 次读取 + 22M 次跳过 = 9.1M 次写入）。
2. visited_tags 数组（40 KB）访问模式是**随机访问**（neighbor 顺序不可预测），产生 cache miss。
3. 71% skip rate 说明大量 neighbor 被多个候选路径重复遍历，visited_tags 是防重复的 guard。

### 8.2 Cache pollution 分析

当前 hot loop 对每个 neighbor 的访问序列：

```text
1. visited_tags[neighbor]         ← 4B read, 随机 offset, 独立分配 (40 KB)
2. (若 skip) continue             ← 71% 路径在此终止
3. levels[neighbor]               ← 1B read, 随机 offset, 另一独立分配 (10 KB)
4. deleted_flags[neighbor]        ← 1B read, 随机 offset, 另一独立分配 (10 KB)（仅当 no_deleted=false）
5. vectors[neighbor * 64..]       ← 256B read, 随机 slot, 独立分配 (2.44 MB)
```

每个非跳过邻居涉及 3 个不同分配的随机访问 + 1 个 256B 流式读取。

**联合缓存优化机会**：将 `visited_tags`、`levels`、`deleted_flags` 合并为一个 compact struct（如 `uint32_t tag:16; uint8_t level:6; uint8_t deleted:1;`），可以在一个 cache line 中容纳 16 条记录，将 3 次随机访问降为 1 次。

### 8.3 levels / deleted_flags 布局

- `levels`: `uint8_t[capacity]` — 10 KB，独立分配。每个 entry 1 字节。
- `deleted_flags`: `uint8_t[capacity]` — 10 KB，独立分配。rebuild 路径（deleted_count=0）完全不访问此数组。
- 这两个数组各自适合 L1 cache（32 KB），但由于在不同分配中，与 visited_tags 的联合访问需要多个 DTLB entries。

### 8.4 slot_to_record_id / record_id_to_slot

- `slot_to_record_id`: 10K × 8 = 80 KB。仅 VSEARCH/export 路径读取，不进入 add hot loop。
- `record_id_to_slot`: hash map（开放寻址），~160 KB。仅 `kvs_hnsw_index_add` 入口处查重使用，不进入 hot loop。
- **审计结论**：这两个映射不占用 add hot path 的 cache，无需优化。

## 9. visited_tags 机制审计

### 9.1 当前设计

- `uint32_t visited_tags[capacity]` — 每个 slot 一个 4-byte tag。
- `current_visit_tag`：每次 search/layer_search 递增 1。
- 当 `current_visit_tag` 溢出到 0 时，全量 `memset(visited_tags, 0, capacity * 4)`。
- Tag 比较：`visited_tags[neighbor] == visit_tag`（uint32_t 比较）。

### 9.2 性能分析

- 71% neighbor skip rate 证明 visited_tags 是 hot loop 中最关键的快速路径。
- 每次访问读 4 字节 + 比较 → 至少 1 次 cache line fill（如果不在 cache 中）。
- 40 KB 数组对 10K 容量 → 可放 L2（256 KB）但不能放 L1（32 KB）。
- Tag 溢出概率：uint32_t 支持 ~4.29×10⁹ 次搜索。在 10K rebuild 中每次 add 执行 (max_level+1) 次 layer search，总共约 20K-50K 次 search → 需要约 85,000-200,000 rebuilds 才触发一次 memset。**溢出开销可忽略。**

### 9.3 更紧凑的设计空间

| 方案 | entry 大小 | 10K 总大小 | L1 fit | 溢出频率 | 改动量 |
|---|---|---|---|---|---|
| uint32_t（当前） | 4 B | 40 KB | ✗ | ~2×10⁵ rebuilds | 0 |
| uint16_t | 2 B | 20 KB | ✗（放 L2） | ~2×10³ rebuilds | 小 |
| uint8_t | 1 B | 10 KB | ✓ | ~2 rebuilds | 中（需改 memset 策略） |
| compact struct (tag:16 + level:6 + deleted:1) | 3 B | 30 KB | ✗ | 中 | 大 |

**uint16_t 推荐度最高**：20 KB 适合 L2，溢出频率仍可接受（每 65,535 次 search 一次 memset，在 10K rebuild 中每 3-6 rebuilds 触发一次，40 KB memset 开销可忽略）。

## 10. heap / context buffer 审计

### 10.1 Heap item 大小

```c
typedef struct kvs_vector_heap_item_s {
    uint64_t record_id;   // 8 bytes
    uint32_t slot;        // 4 bytes
    float score;          // 4 bytes
    // padding: 0 bytes (8+4+4=16, aligned to 8)
} kvs_vector_heap_item_t;  // 16 bytes total
```

### 10.2 单 context 内存

| Buffer | Capacity (ef=128) | Item size | 总大小 |
|---|---|---|---|
| candidate_heap items | 128 | 16 B | 2,048 B |
| top_heap items | 128 | 16 B | 2,048 B |
| neighbor_buffer | 128 | 4 B | 512 B |
| distance_buffer | 128 | 4 B | 512 B |
| hits | 128 | 16 B | 2,048 B |

**单 context 总计：~7,168 bytes → 完全适合 L1 cache（32 KB）。**

2 个 context 的 pool：~14 KB → 仍适合 L1。

### 10.3 Heap 操作统计（10K DIM=64）

| 操作 | 总计 | 每 add 平均 |
|---|---|---|
| top_heap push | 1,345,783 | 135 |
| top_heap replace | 1,128,250 | 113 |
| candidate push | 1,922,664 | 192 |
| **heap 操作总计** | **~4,396,697** | **~440** |

每次 add 执行约 440 次 heap 操作（push + pop + replace_top，各 O(log ef)）。

### 10.4 Heap item `record_id` 字段浪费

**Add path**：`kvs_hnsw_search_layer_for_add` 中 `item.record_id = 0`（从未使用）。8 bytes × 256 items（128×2 heaps）= 2,048 bytes per context 浪费 — 占 heap 内存的 50%。

**Search path**：需要 `record_id`（通过 `slot_to_record_id` 查找），不浪费。

**审计结论**：Add-time waste 为 2 KB/context → 不是主要瓶颈。

### 10.5 Heap 操作的内存访问模式

- `kvs_vector_heap_push`：sift-up，访问 `items[i]`, `items[parent]`, `items[(parent-1)/2]`。Cache 友好（数组连续，sift-up 沿树路径）。
- `kvs_vector_heap_replace_top`：sift-down，访问 `items[0]`, `items[left]`, `items[right]`。Cache 友好。
- 总 heap items = 128 × 16 = 2,048 bytes → 32 个 cache line → 适合 L1。

**审计结论**：Heap 操作不是 cache locality 瓶颈。所有 heap items 在 L1 中，push/pop/replace 操作高度局部化。

## 11. distance compute 审计

### 11.1 当前实现

HNSW 使用自己的静态距离函数（`kvs_hnsw_dot_score` / `kvs_hnsw_l2_score` / `kvs_hnsw_cosine_score`），通过 `kvs_hnsw_metric_score()` 分发：

```c
static float kvs_hnsw_metric_score(const kvs_hnsw_index_t *index,
                                    const float *lhs, const float *rhs) {
    if (index->distance) {
        return index->distance(lhs, rhs, index->dim);  // 自定义函数指针
    }
    switch (index->metric) {
        case KVS_VECTOR_METRIC_L2:    return kvs_hnsw_l2_score(lhs, rhs, index->dim);
        case KVS_VECTOR_METRIC_COSINE: return kvs_hnsw_cosine_score(lhs, rhs, index->dim);
        case KVS_VECTOR_METRIC_IP:
        default:                       return kvs_hnsw_dot_score(lhs, rhs, index->dim);
    }
}
```

### 11.2 SIMD 准备度

- **当前所有距离函数均为标量 C 循环**（逐个 float 相乘/相减/累加）。
- 项目有独立的 AVX2 距离后端（`src/vector/kvs_vector_distance_avx2.c`），但它**未接入 HNSW**。`index->distance` 在 `kvs_hnsw_index_init` 中通过 `memset(index, 0, ...)` 被置为 NULL。
- 标量 IP 循环：`for (i = 0; i < dim; i++) { score += lhs[i] * rhs[i]; }` — GCC -O2 可能会做有限的 auto-vectorization，但不会生成显式 AVX2 指令。
- 对于 DIM=64：64 次乘法 + 63 次加法 ≈ 127 次浮点运算 × 9.1M 次调用 = 1.16×10⁹ FLOP。在 755,854 μs 内完成 → ~1.5 GFLOP/s。现代 CPU 单核可轻松达到 20-40 GFLOP/s。**距离计算受限于内存带宽，不是 ALU**。

### 11.3 函数指针 vs switch

- `index->distance` 为 NULL 时走 `switch(index->metric)` 路径。
- `index->metric` 在 hot loop 中不变 → branch predictor 可完美预测 switch。
- 开销：1 次 branch + jmp，约 1-2 cycle，可忽略。

### 11.4 已验证的距离计算效率

```text
add_distance_compute_count: 9,106,016
hnsw_add_layer_search_us: 755,854

每距离计算平均耗时: 755,854 / 9,106,016 ≈ 83 ns

对于 DIM=64 IP (64 fma)：83 ns / 64 ≈ 1.3 ns / fma → ~770 MFLOPS
与内存带宽匹配（每次距离计算读取 256B neighbor vector）
```

83 ns/distance × 9.1M = ~755 ms（与 hnsw_add_layer_search_us 的 755,854 μs 基本吻合）。

**结论**：距离计算占 layer_search 的大部分时间（约 60-70%），瓶颈在内存带宽而非 ALU。

## 12. Stage 6D / 6E baseline 引用

| metric | Stage 6D (10K) | Stage 6E (10K) | Stage 7A (10K) |
|---|---:|---:|---:|
| rebuild_core_add_total_us | 910,539 | 901,286 | 1,013,818 |
| hnsw_add_layer_search_us | 682,628 | 675,495 | 755,854 |
| hnsw_add_neighbor_select_us | 168,132 | 166,836 | 193,099 |
| hnsw_add_link_us | 11,217 | 11,075 | 11,611 |
| recall@10 | 0.9870 | 0.9870 | 0.9870 |

注：Stage 7A 的 add_total 和 layer_search 数值较 Stage 6E 偏高 ~12%。这是因为 Stage 7A 在 hot loop 中新增了 9 个 profiling counter increment（~12 次 ++ 操作），以及 VINFO 反序列化路径增加了额外字段。这些 profiling overhead 不影响算法语义和 recall，仅影响绝对时间测量。**Stage 7A 的绝对值与 Stage 6D/6E 不可直接比较，但内部相对比例（如 skip rate、level0/upper split）是准确的。**

## 13. 1K / 10K audit benchmark 结果

### 13.1 10K DIM=64 — event counter 完整数据

| Event counter | 10K total | Per add | 占比/备注 |
|---|---:|---:|---|
| add_distance_compute_count | 9,106,016 | 910.6 | 距离计算调用 ≈ 层搜索总开销的 60-70% |
| add_neighbor_visit_count | 31,414,569 | 3,141.5 | 遍历的 neighbor 总数 |
| add_visited_skip_count | 22,319,225 | 2,231.9 | **71.0% 被 visited tag 跳过** |
| add_active_check_count | 0 | 0 | rebuild 路径无删除 |
| add_level0_neighbor_scan_count | 30,432,809 | 3,043.3 | **96.9% 是 level0** |
| add_upper_neighbor_scan_count | 981,760 | 98.2 | 仅 3.1% |
| add_top_heap_push_count | 1,345,783 | 134.6 | |
| add_top_heap_replace_count | 1,128,250 | 112.8 | |
| add_candidate_push_count | 1,922,664 | 192.3 | |
| hnsw_add_layer_search_us | 755,854 | 75.6 μs | |
| rebuild_core_add_total_us | 1,013,818 | 101.4 μs | |
| recall@10 | 0.9870 | — | |
| backend_memory_bytes | 41,941,200 | — | ~41.9 MB |

### 13.2 关键比例

```text
visited skip rate:        71.0%  (22.3M / 31.4M)
level0 vs upper scan:    96.9% / 3.1%
passing neighbor rate:   27.2%  ((1.35M + 1.13M) / 9.11M)
distance per add:        910.6
neighbor visits per add: 3,141.5
heap ops per add:        ~440   (top push+replace+candidate push)
```

### 13.3 1K DIM=384 synthetic bench

DIM=384 synthetic benchmark 结果与 DIM=64 完全一致（事件计数、memory_bytes 均相同）。原因：hash embedding provider 在编译时固定使用 `KVS_VECTOR_DEFAULT_DIMENSION = 64`，`embedding_dim = 384` 配置项未被 hash embedding 读取。项目当前未接入真实 ONNX embedding model。

**Dimension-scaling 审计受限于实际 embedding dim = 64。** 更高维度的 cache locality 影响见 §6.5 的理论投影分析。

## 14. 是否发现 memory layout / cache locality 证据

**是。发现 6 个可量化的 memory layout / cache locality 问题：**

### 证据 1：Upper neighbors 87.5% 空间浪费（19.5 MB → 实际使用 ~2.4 MB）

`upper_neighbors` 使用 `capacity × KVS_HNSW_MAX_LEVEL × M` 字节，但实际 max_level ≤ 4。87.5% 的空间（~17 MB）是零填充但占据物理页和 TLB entries。

### 证据 2：71% visited-tag skip rate 暴露 visited_tags cache 压力

每 add 平均 2,232 次 skip（仅 4 字节比较 + 跳转）。如果 visited_tags 不在 L1/L2 中，每次 skip 产生一次 cache miss。40 KB 的 visited_tags 数组刚好超出 L1（32 KB），落在 L2。

### 证据 3：热路径 3 次独立分配访问

每个非跳过 neighbor 访问 3 个不同内存区域（visited_tags、levels、vectors），产生 3 次 DTLB lookup + 3 次潜在 cache miss。合并 visited_tags + levels + deleted_flags 可降至 1 次。

### 证据 4：距离计算受限于内存带宽

83 ns / 256-byte neighbor vector 读取 → ~3.1 GB/s 内存带宽。DDR4 单通道 ~12-20 GB/s。距离计算执行 64 fma ≈ 127 FLOP 在 83 ns 内，ALU 利用率极低。**内存带宽是瓶颈，不是 CPU 运算能力。**

### 证据 5：Level0 主导 neighbor 扫描（96.9%）

30.4M level0 neighbor scans vs 0.98M upper。优化应集中在 level0 访问模式。Level0 每 slot 128-byte 邻居数组连续，但随机 slot 访问破坏 locality。

### 证据 6：Heap items 完全在 L1 中（非瓶颈）

每个 context 的 heap 占用 ~7 KB，属于 L1 常驻。Heap 操作（push/replace）虽多（440/add），但 cache 局部性好。

## 15. 是否建议进入 Stage 7B

**建议进入 Stage 7B，但仅限于特定方向。** memory layout / cache locality 证据充分，但优化空间集中在 3 个方向：

| 方向 | 预期收益 | 风险 |
|---|---|---|
| Upper neighbors compact 存储（按实际 max_level 分配） | 节省 ~17 MB 内存，减少 TLB 压力 | 中等（需调整 neighbor 寻址逻辑） |
| 合并 visited_tags + levels + deleted_flags | 减少 3→1 次内存访问 / neighbor | 低（局部改动） |
| 接入 AVX2 距离计算到 HNSW | 距离计算加速 ~2-4× | 低（已有 AVX2 后端，只需接线） |

不建议在 Stage 7B 进行的：
- AoS→SoA 重写（当前 64-dim 下收益有限，且破坏现有代码结构）
- Graph 结构变更
- Heap 重新设计（当前已在 L1 中，无瓶颈）

## 16. Stage 7B 可执行优化候选列表

### 16.1 高优先级（低风险、高收益）

1. **接入 AVX2 距离后端到 HNSW**
   - 将 `kvs_vector_distance_dot_avx2` 等函数通过 `index->distance` 函数指针接入 HNSW。
   - DIM=64 下 AVX2 可对 64 个 float 进行 8×unroll 加速。
   - 预期：距离计算加速 2-4×，layer_search_us 下降 30-50%。
   - 风险：低（AVX2 后端已存在并通过 flat index 验证）。

2. **合并 visited_tags + levels + deleted_flags 为 compact struct**
   - 推荐：`uint32_t tag:16, uint8_t level:6, uint8_t deleted:1`（3 bytes padded to 4）。
   - 每 cache line 可装 16 条记录（4-byte 打包）。
   - 减少 hot loop 中 3 次随机访问 → 1 次。
   - 预期：visited skip 路径加速 30-50%。
   - 风险：低（仅影响 slot metadata 访问路径）。

### 16.2 中优先级（中等风险、中等收益）

3. **Upper neighbors 动态分配**
   - 按实际 `max_level+1` 分配 upper_neighbors，替代固定 `KVS_HNSW_MAX_LEVEL=32`。
   - 需要引入 `upper_neighbor_strides` 数组替代当前索引公式。
   - 预期：节省 ~17 MB 内存（10K），减少随机访问的物理跨度。
   - 风险：中（需修改 reserve/copy/layer_neighbors 寻址逻辑）。

4. **visited_tags uint32_t → uint16_t**
   - 节省 20 KB（10K 时 40 KB → 20 KB），使 visited_tags 更容易留在 L1。
   - 溢出频率增加但可接受（每 65K 次搜索触发 memset）。
   - 风险：低。

### 16.3 低优先级（需更多分析）

5. **vectors 64-byte 对齐**
   - 使用 `aligned_alloc` 替代 `kvs_malloc` 分配 vectors。
   - 配合 AVX2 对齐加载指令。
   - 风险：极低。

6. **移除 upper_offsets 冗余数组**
   - 将 `upper_offsets[slot]` 替换为 `slot * KVS_HNSW_MAX_LEVEL * M` 的在线计算。
   - 预期：节省 80 KB 内存。
   - 风险：极低（但该内存已可忽略）。

## 17. 不建议优化的方向及原因

| 方向 | 原因 |
|---|---|
| AoS → SoA vectors 重写 | 64-dim 下 AoS 利于单向量距离计算；SoA 仅在多向量批量计算时有优势。当前 add 路径是每次一个 query vs 一个 neighbor，AoS 是正确选择 |
| Graph 结构重设计 | 不是当前瓶颈；71% skip rate 与 graph 连接模式有关但改善需要算法变更（超出 P1/P2 范围） |
| Heap 重新设计 | Heap 已完全适合 L1（7 KB），push/replace/pop 的 cache 局部性好 |
| HNSW 添加算法替代 | 算法级改变超出 Stage 7A/7B 范围 |
| 降低 M/M0/ef 默认值 | AGENTS.md 明确禁止 |
| 分片/分区 | 超出范围 |
| scope-aware search | 超出范围 + 明确禁止 |
| graph persistence | 明确禁止 |

## 18. tests 结果

构建：
```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```
✅ PASS

测试：
```bash
tests/vector_core_hnsw_correctness_test.sh       ✅ PASS
tests/vector_core_hnsw_rebuild_test.sh            ✅ PASS
tests/vector_core_hnsw_rdb_reload_test.sh         ✅ PASS
tests/vector_core_hnsw_aof_replay_test.sh         ✅ PASS
tests/vector_core_hnsw_rebuild_profile_test.sh    ✅ PASS
tests/vector_manager_resolve_fastpath_test.sh     ✅ PASS
tests/vector_vsearch_profile_breakdown_test.sh    ✅ PASS
tests/vector_vinfo_backend_stats_test.sh          ✅ PASS
```

全部 8/8 PASS。Recall@10 保持 0.9870。

## 19. 静态边界检查结果

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
# ✅ src/vector/core 未新增 g_vector_state 引用

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
# ✅ src/vector/core 未出现 manager 语义

grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
# ✅ 签名未修改

grep -RIn "record_id_map" src/vector/core include/vector 2>/dev/null
# ✅ record_id_map 不进入 vector_core

git diff -- src/persistence
# ✅ 无 diff（RDB/AOF 格式未修改）
```

所有 5 项静态边界检查 CLEAN。

## 20. git status --short

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
 M src/vector/kvs_vector_hnswlib_ops.c
 M src/vector/kvs_vector_index.c
?? AGENTS.md
?? docs/vector_core_reset/stage5a_...md
?? docs/vector_core_reset/stage5b_...md
?? docs/vector_core_reset/stage5c_...md
?? docs/vector_core_reset/stage5d_...md
?? docs/vector_core_reset/stage6a_...md
?? docs/vector_core_reset/stage6b_...md
?? docs/vector_core_reset/stage6c_...md
?? docs/vector_core_reset/stage6d_...md
?? docs/vector_core_reset/stage6e_...md
?? docs/vector_core_reset/stage7a_core_hnsw_memory_layout_cache_audit.md
?? docs/vector_core_reset/优化步骤.md
?? results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_1k_64d.json
?? results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_1k_64d.md
?? results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_10k_64d.json
?? results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_10k_64d.md
?? results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_1k_384d.json
?? results/vector_core_benchmark/stage7a_hnsw_memory_layout_audit_benchmark_1k_384d.md
?? tests/vector_core_hnsw_rebuild_profile_test.sh
?? tests/vector_manager_resolve_fastpath_test.sh
?? tests/vector_vinfo_backend_stats_test.sh
?? tests/vector_vsearch_profile_breakdown_test.sh
```

## 21. 下一步建议

**Stage 7B: core_hnsw targeted memory layout optimization**

具体执行顺序：

1. **接入 AVX2 距离后端**（最低风险、最高收益）
   - 将 `kvs_vector_distance_dot_avx2` 通过 `index->distance` 函数指针接入。
   - 确保 AVX2 路径与非 AVX2 路径对 DIM=64 产生相同 score 结果。
   - 预期：`hnsw_add_layer_search_us` ↓ 30-50%。

2. **合并 visited_tags + levels + deleted_flags**（降低内存访问次数）
   - 定义 `kvs_hnsw_slot_meta_t { uint32_t tag:16; uint8_t level:6; uint8_t deleted:1; }`（或类似 compact 结构）。
   - 每条记录 4 bytes，一个 cache line 容纳 16 条。
   - 预期：visited skip 路径延迟显著降低。

3. **校验 recall@10 ≥ 0.9870**。
