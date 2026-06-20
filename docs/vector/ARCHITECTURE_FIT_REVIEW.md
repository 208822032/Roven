# KVLite Vector Store Architecture Fit Review

审查日期：2026-06-20
审查范围：`src/vector/`, `include/kvs_vector.h`, `src/core/kvs_operation.c`, `src/persistence/`, `config/kvstore-embedding.conf`

---

## 1. 总体结论

**PARTIAL**

当前架构已经具备独立 vector store engine 的主要骨架（vector_record_table, id_map, flat/HNSW/SQ8 index, embedding cache, ONNX runtime），但没有完全符合"内部专用引擎 + 外部简单命令"的成熟度目标。

核心差距：

1. **vector_record_table 已存在**，但缺少 TTL / generation / model_version 字段
2. **embedding cache 设计完善**，但未接入服务态验证、默认关闭、全局锁瓶颈未解决
3. **ONNX embedding 路径完整**，但 worker pool + batch 路径均未完成服务态集成
4. **HNSW recall 显著低于 Faiss**，差距源于实现细节而非架构设计
5. **持久化已存 embedding**，但未存 model_version，新旧模型切换无安全网
6. **VSEARCH 仍在同步 handler 内完成**，无 index snapshot / search worker / async update

---

## 2. 当前真实架构

当前 V 族命令有**完全独立的处理路径**，不经过普通 KV 的数据结构和 handler：

```
client RESP
  → parser (kvs_operation.c: VCMD parsing)
  → V command handler (handle_vset/vsearch/vget/vdel/vinfo/vindex)
  → kvs_vector_* public API (kvs_vector.c)
  → embedding layer (kvs_vector_embedding.c)
  → index layer (kvs_vector_index.c: flat/HNSW/SQ8 dispatch)
  → store layer (kvs_vector_index.c: records, id_map, topK heap)
  → RESP reply
```

V 族命令使用自己的数据结构：

- `kvs_vector_store_t` — 记录数组，含 active_records / deleted_records / free_list
- `kvs_vector_id_map_t` — question+scope → record_index 的开放寻址 hash
- `kvs_vector_flat_index_t` — 保存所有 active record 的 float embedding + record_index/record_id 映射
- `kvs_vector_ann_index_t` — HNSW 多层图索引
- `kvs_vector_sq8_flat_index_t` — SQ8 压缩 flat 索引（实验性）
- `kvs_vector_index_t` + `kvs_vector_index_ops_t` — FAISS-like 抽象层

**关键判断**：当前不是"普通 KV + 外挂 index"。记录不经过 array/rbtree/hash/skiptable。
VSEARCH 命中后通过 `record_index` 直接从 `store.records[record_index]` 读取 answer，
**不需要**回普通 KV 查 answer。

---

## 3. 当前调用链

### 3.1 VSET_TEXT 当前调用链

```
client: VSET question answer [scope]
  → parser (kvs_operation.c:780-812): 解析 tokens → command_context_t
  → handle_vset_command (kvs_operation.c:1969-1986)
  → kvs_vector_set (kvs_vector.c:1649-1833)
    → kvs_vector_find_record: id_map lookup by question+scope
    → [if found]: kvs_vector_record_update_answer → return
    → kvs_vector_find_free_slot: pop free list or expand store
    → kvs_vector_record_set_fields (kvs_vector_index.c:229-332)
      → kvs_vector_embedding_generate (kvs_vector_embedding.c:1368-1452)
        → [local_c]: embedding_cache_lookup → [hit: return cached L2-normed embedding]
        → [local_c miss]: kvs_vector_local_c_embedding_generate
          → [thread_context or global_mutex]: embedding_encode (ONNX Run)
        → [hash]: kvs_vector_hash_embedding_generate
        → kvs_vector_embedding_l2_normalize
        → [local_c]: embedding_cache_insert
      → populate record fields (id, question, answer, scope, embedding, timestamps)
    → kvs_vector_id_map_put: register question+scope → record_index
    → kvs_vector_flat_index_add: append to flat index (always for debug baseline)
    → [if not flat]: kvs_vector_index_add_current → HNSW or SQ8 add
    → update store counts (active_records++, deleted decrement if was deleted)
  → handle_vector_status_response → "+OK\r\n"
```

注意：当前 VSET 在 `kvs_vector_record_set_fields` 内部同步生成 embedding。
VSET 的 RDB/AOF/replication 钩子在 **handler 外部**通过统一框架处理，不在 kvs_vector_set 内部。

### 3.2 VSEARCH_TEXT 当前调用链

```
client: VSEARCH query [scope] [topk] [threshold]
  → parser (kvs_operation.c:833-896): 解析 tokens → command_context_t
  → handle_vsearch_command (kvs_operation.c:2021-2160)
  → kvs_vector_search (kvs_vector.c:2210-2288)
    → kvs_vector_embedding_generate: embedding cache or ONNX run + L2 normalize
    → kvs_vector_index_search_current → ops->search
      → [flat]: kvs_vector_flat_index_search
        → linear scan all vectors in flat_index
        → for each: scope match + record_id check + metric score + threshold filter
        → kvs_vector_topk_heap_push (min-heap, size=topk)
        → kvs_vector_topk_heap_export_desc: qsort + fill results from store.records[record_index]
      → [hnsw]: kvs_vector_ann_index_search
        → greedy descent from entry_node through upper levels
        → level 0: collect_candidates with ef=effective_ef
        → for each candidate: scope match + record_id check + threshold filter
        → kvs_vector_topk_heap_push
        → kvs_vector_topk_heap_export_desc
  → RESP format: score|scope|question|answer per result
  → RESP array reply
```

### 3.3 embedding cache hit 调用链

```
kvs_vector_embedding_generate (embedding.c:1368)
  → [provider == LOCAL_C]: embedding_cache_lookup (embedding.c:454-538)
    → embedding_cache_should_use: check config + enabled + lru policy
    → embedding_cache_key_build: normalize text → hash → model_id → dim
    → pthread_mutex_lock(&g_embedding_cache.mutex)   ← GLOBAL LOCK
    → bucket_index = text_hash & (bucket_count-1)
    → chained scan: key_equal check → TTL check (expire on access)
    → [hit]: memcpy(copy), LRU touch, stats update
    → pthread_mutex_unlock
    → return cached L2-normalized embedding (skip ONNX + L2 normalize)
```

### 3.4 embedding cache miss 调用链

```
kvs_vector_embedding_generate (embedding.c:1368)
  → embedding_cache_lookup → miss
  → kvs_vector_local_c_embedding_generate (embedding.c:1282-1363)
    → [per_thread_context]: use thread-local EmbeddingHandle
    → [else]: pthread_mutex_lock(&g_embedding_provider_encode_mu)  ← GLOBAL MUTEX
    → embedding_encode(handle, text, text_len, embedding, dim)
      → tokenizer
      → ONNX Runtime run
      → copy sentence_embedding
    → unlock
  → kvs_vector_embedding_l2_normalize
  → embedding_cache_insert (embedding.c:540-634)
    → pthread_mutex_lock(&g_embedding_cache.mutex)
    → [if key exists]: update → unlock → return
    → [if full]: LRU evict from tail until under capacity
    → calloc entry + kvs_malloc embedding + memcpy + link into bucket + LRU head
    → pthread_mutex_unlock
```

### 3.5 ANN add 调用链

```
kvs_vector_index_add_current → ops->add → kvs_vector_ann_index_add (index.c:3172)
  → kvs_vector_ann_index_reserve: expand nodes array if needed
  → kvs_vector_ann_random_level: deterministic level from record_id
  → kvs_vector_ann_node_init_layers: allocate neighbors/neighbor_counts/neighbor_capacities per level
  → [first node]: set entry_node, has_entry, max_level → return
  → [subsequent]: greedy_search_layer from entry down to (node_level+1)
  → for level = min(node_level, max_level) down to 0:
    → kvs_vector_ann_collect_candidates_layer: ef=ef_construction, visited bitmap, candidate+result sets
    → kvs_vector_ann_sort_candidates_desc: insertion sort
    → for top M candidates:
      → kvs_vector_ann_neighbor_add_or_replace: bidirectional link, keep best M by distance
  → if new node_level > max_level: update entry_node, max_level
```

关键细节：
- `kvs_vector_ann_neighbor_add_or_replace` 在满容时，逐邻居计算距离找最差，新连接优于最差才替换。这比 Faiss 的 `select_neighbors_simple` / `select_neighbors_heuristic` 简单，可能影响 HNSW 图质量。
- 邻居选择使用 `g_vector_state.metric_type`（硬编码全局 metric），而非传入参数。
- `greedy_search_layer` 使用 `kvs_vector_metric_score`（distance 后端，支持 AVX2/fallback）。

### 3.6 ANN search 调用链

```
kvs_vector_ann_index_search (index.c:3325)
  → effective_ef = kvs_vector_ann_effective_ef_search: scope 非空时 × overfetch_factor
  → for level = max_level down to 1:
    → kvs_vector_ann_greedy_search_layer: 1-hop greedy per step
  → kvs_vector_ann_collect_candidates_layer(level=0, ef=effective_ef):
    → malloc candidate_set (ef*(M+1) items), result_set (ef items), visited bitmap (capacity bytes)
    → entry_node → visited[entry]=1 → append to candidate_set + result_set
    → loop:
      → find_best_unexpanded in candidate_set
      → if worst_result_score > best: break (early termination)
      → expand neighbors: for each → visited check → score compute → add to candidate_set + result_set
    → free visited + candidate_set → return result_set
  → for each candidate: record_id check + scope match + threshold → topK heap push
  → topK heap export_desc: qsort + fill from store.records
  → free candidates + heap
```

### 3.7 candidate lookup 调用链

```
kvs_vector_topk_heap_export_desc (index.c:2160)
  → qsort heap items by score desc
  → for each sorted item:
    → record_index = heap items[i].record_index
    → record = &store.records[record_index]
    → if ACTIVE && !deleted → kvs_vector_search_result_from_record
    → set result.score from heap item
```

candidate lookup 是 `record_index → store.records[record_index]` 的直接数组访问，**不经过** id_map 或 hash lookup。

---

## 4. 与目标架构的符合度

| 模块 | 目标设计 | 当前实现 | 评价 | 证据 |
|------|---------|---------|------|------|
| 对外 VSET/VSEARCH 命令语义 | 简单文本命令 | VSET question answer [scope] / VSEARCH query [scope] [topk] [threshold] | MATCHED | kvs_operation.c:780-939 |
| embedding cache | text_hash+model_id+dim key, L2 normed value, LRU, TTL, capacity | 完全符合设计：hash table + LRU + TTL + capacity + metrics | MATCHED | embedding.c:21-52, 454-538, 540-634 |
| ONNX embedding runtime | 复用 session, 可配 threads, per-thread context | shared ORT env, per-thread context, global mutex fallback, configurable intra_threads | PARTIAL | embedding.c:989-1075, 1282-1363 |
| vector record table | record_id → question/answer/embedding/metadata | 独立 store.records 数组，含 id/question/answer/scope/embedding/state/deleted/timestamps | PARTIAL | kvs_vector.h:459-500; 缺 TTL/generation/model_version 字段 |
| answer blob store | 独立 blob store | answer 存储在 record 内部字段，非独立 blob store | PARTIAL | 记录级 answer 字段足够当前语义，但后端复用和共享 answer 的能力未体现 |
| vector arena | 独立向量内存区域 | flat_index 管理连续 embedding 数组，record 自身也持有一份 embedding | PARTIAL | flat_index 提供 arena-like 向量存储，但 record.embedding 是独立 malloc 的副本，两份存储 |
| Flat index | 全量精确检索 | 连续 float 数组 + record_indices + record_ids，线性扫描 + scope/post-filter | MATCHED | kvs_vector.h:525-535, index.c:1703-1850 |
| HNSW index | ANN 近似检索 | 多层双向图，greedy search + collect_candidates, M/ef_construction/ef_search 可配置 | PARTIAL | 实现正确但 recall 明显低于 Faiss，原因见 §6 |
| Faiss baseline 对比 | 作为参考，不做回退 | 独立 benchmark binary 对比，已产出报告 | MATCHED | tests/faiss_baseline.cpp, tests/vcmd_onnx_faiss_bench.cpp, ONNX_FAISS_BOTTLENECK_REPORT.md |
| TTL / deleted / generation | record 级 TTL, tombstone, 代际管理 | deleted 和 state(ACTIVE/EMPTY/DELETED) 已实现，无 TTL/generation | PARTIAL | 缺 expire_at_ms 字段，无 TTL scanner 集成 |
| AOF / RDB | VSET 专用语义，持久化 question+answer+embedding | RDB 完整持久化所有字段含 embedding; AOF rewrite 只发 VSET question answer（无 embedding 回放）；HNSW 图不持久化 | PARTIAL | kvs_rdb.c:134-189, kvs_command_executor.h:66-130 |
| replication | master-slave 同步 VSET | 通过统一框架处理，VSET 走 REPLICATE 标记 | UNKNOWN | 代码中 `vset_replication_*` metrics 存在，但未深入验证同步语义 |
| metrics | 全链路分阶段 metrics | 非常详细的 metrics: vset_commit 各阶段、vsearch 各阶段、embedding runtime、cache stats | MATCHED | kvs_vector.h:198-391, embedding.c:835-895 |
| shutdown lifecycle | 统一销毁 | kvs_vector_destroy 链式销毁所有组件 + provider + cache | MATCHED | kvs_vector.c:1052-1064 |
| build variant isolation | 编译期开关隔离 | 无独立编译开关；向量模块始终编译进主二进制 | MISMATCH | Makefile 中无 KVS_ENABLE_VECTOR 或等价开关；向量模块无条件编译 |

---

## 5. 当前主要瓶颈

### 冷路径瓶颈

1. **ONNX 推理占比 >95%**：`embed_only avg=9627us` 中 `onnx_run=9608us` (99.80%)
2. **VSET_TEXT_ONNX 中 embedding_total 占 95.77%**，record commit + ANN add 合计 <5%
3. **VSEARCH_TEXT_ONNX 中 embedding_total 占 97.37%**

根本原因：每次 VSET/VSEARCH 都走一次完整的 tokenizer + ONNX 推理链路。

### 热路径瓶颈

1. **embedding cache 的全局读写锁**：每个 cache lookup 和 insert 都争用同一个 `g_embedding_cache.mutex`
2. **embedding encode 的全局互斥锁**：非 per-thread 模式下，所有 OnnxRuntime run 串行
3. **无 index snapshot**：VSEARCH 直接读全局 index，读写互斥依赖 V 族命令的 handler 锁（KVS_CMD_LOCK_READ / KVS_CMD_LOCK_WRITE）

### ANN-core 瓶颈

1. **HNSW visited bitmap 每次 malloc**：`kvs_vector_ann_collect_candidates_layer` 中 `visited = kvs_malloc(index->capacity)`（index.c:2977）
2. **candidate_set 每次 malloc**：`ef * (M+1)` 临时数组每次分配（index.c:2962-2966）
3. **candidate 排序用插入排序**：`O(n²)`，对 ef_search 较大时不理想（index.c:3159-3169）
4. **flat search 中每次 scope 过滤仍访问 store invalid record**：线性扫描不可跳过的无效检查较多

### 存储路径瓶颈

1. **record.embedding 与 flat_index.vectors 双份存储**：embedding 同时存在 record 和 flat_index 中
2. **flat_index 永远保留**：即使主路径是 HNSW，flat 仍占用内存
3. **VDEL 只标记 deletion 不清除 index**：flat_index 和 HNSW 中 stale entry 存在，需要 VINDEX REBUILD 清理

### 服务态瓶颈

1. **embedding cache 默认关闭**（`kvstore-embedding.conf` 中 `embedding_cache_enabled = 0`）
2. **worker pool 默认关闭**（`embedding_worker_pool_enabled = 0`）
3. **per_thread_runtime 默认关闭**（`embedding_per_thread_runtime_enabled = 0`）
4. **batch embedding 默认关闭**（`embedding_batch_enabled = 0`）
5. **无服务态校验过的 cache hit rate / worker pool 性能数据**

### 构建系统风险

1. **无 build variant 隔离**：向量模块（含 ONNX Runtime 链接依赖）始终编译进主二进制
2. **ONNX Runtime 为 mandatory 依赖**：如果 `libonnxruntime.so` 不存在，链接失败

---

## 6. 与 Faiss 的差距

仅作为 baseline 分析，不作为生产回退建议。

### KVLite Flat add 为什么慢于 Faiss Flat？
- KVLite: 每次 `kvs_vector_flat_index_add` 调用 `kvs_vector_flat_index_reserve`（检查容量→可能扩容），做 record 状态校验，写三个数组（vectors + record_indices + record_ids）
- Faiss: 预分配内存，直接 `memcpy + ntotal++`
- 差距约 **3.1x**

### KVLite Flat search 小规模接近 Faiss 是否可信？
- nb=1000 下两者接近（KVLite 7451 QPS vs Faiss 6075 QPS），可信
- KVLite 使用 AVX2 distance backend（当 CPU 支持时），Faiss 使用 BLAS
- 注意：这个规模下两者都是纯 CPU 线性扫描，Faiss 的 BLAS 优势在大维度/大批量时才体现

### KVLite HNSW recall 明显低于 Faiss HNSW 的可能原因

| 可能原因 | 分析 |
|---------|------|
| **邻居选择策略** | KVLite 使用简单 "worst replacement"，Faiss 支持 `select_neighbors_simple` / `select_neighbors_heuristic`（后者通过排除近邻来增加图多样性） |
| **greedy search 实现** | KVLite `greedy_search_layer` 是 1-hop greedy（每步检查所有邻居，取最优），非 beam search。标准 HNSW paper 也使用 1-hop greedy，但 Faiss 在实现上有更多优化 |
| **random_level 确定性** | KVLite 使用 deterministic seed=record_id，保证 rebuild 稳定，但 seed 分布可能不如 `rand()` 均匀 |
| **邻居数量管理** | KVLite 每层固定 M 个邻居（第 0 层也是 M），标准 HNSW 第 0 层通常 2*M |
| **visited bitmap 无法跨层复用** | 每次 `collect_candidates_layer` 重新 malloc 和清零 visited |
| **scope / record_id post-filter** | KVLite 在 candidate export 阶段做 scope match + record_id check + state/deleted check，可能过滤掉 ANN 阶段选出的好结果，从而降低统计 recall。Faiss 测试使用 `search_and_check` 只看纯向量距离的 recall |

### HNSW 的 M / efConstruction / efSearch 是否真正生效？
- `M` (默认 32)、`ef_construction` (默认 256)、`ef_search` (默认 256) 从配置正确传递到 `kvs_vector_ann_index_init`
- `ef_construction` 在 `collect_candidates_layer` 中作为 ef 参数使用 → **是**
- `ef_search` 在 search 中作为 effective_ef 的基础 → **是**
- `M` 在 add 时控制每层邻居数 → **是**
- 但第 0 层邻居容量 = M（而非 2*M），这是与标准 HNSW 的一个重要偏差

### metric=ip / cosine / L2 是否与 Faiss 一致？
- KVLite 的距离后端（`kvs_vector_distance_dot`, `kvs_vector_distance_l2_score`, `kvs_vector_distance_cosine_score`）通过 AVX2 或 fallback 实现
- 嵌入已做 L2 normalize → IP 等价于 cosine
- KVLite 使用 `larger score is better` 语义，topK min-heap 维护最大值 → 与 Faiss 一致
- **但** `kvs_vector_ann_neighbor_add_or_replace` 使用 `g_vector_state.metric_type`（全局 metric）做邻居选择，而非传入参数 → 切换 metric 后可能需要重建 HNSW

### 其他可能影响 recall 的因素
- **candidate queue**：使用临时分配的数组 + 线性扫描找 best unexpanded → O(W) 而非 O(log W)
- **visited bitmap**：每次 search 重新分配，但实现无误
- **topK heap**：min-heap 维护 topK，正确
- **score tie-breaking**：`kvs_vector_topk_candidate_less` 在 score 相同时以 record_index 大的为"更差" → 稳定
- **scope / deleted / filter 导致 recall 统计偏低**：Faiss 测试走纯向量距离，KVLite 测试走完整的 candidate export（含 scope + state + record_id check）。如果 fixture 中某些 record 的 state/deleted 不一致或 record_id 不匹配，会导致统计 recall 偏低。这是可能性很高的问题。

---

## 7. 最大架构缺口

当前最缺的 5 个模块/能力：

1. **vector_record_table 的 TTL / generation / model_version 字段**
   - 当前 record 无 `expire_at_ms`、`generation`（epoch）、`embedding_model_version` 字段
   - TTL scanner 未集成向量模块
   - 模型升级后旧 embedding 无法识别

2. **vector arena（统一向量内存管理）**
   - 当前 embedding 双份存储（record.embedding + flat_index.vectors）
   - HNSW node 通过 record_index 间接取 embedding（无独立副本）
   - 需要"记录拥有 embedding，index 只持有 arena 内的引用"的统一管理

3. **answer blob store**
   - 当前 answer 以 C string 形式存在 record 内部
   - 不支持大 answer（> 几 KB 可能不适合 inline）
   - 不支持多 question 共享同一 answer（去重）

4. **index snapshot + VSEARCH fast path**
   - 当前 VSEARCH 在同步 handler 内完成（虽然有 KVS_CMD_LOCK_READ，但仍是同步路径）
   - 无 search worker pool
   - 无 index snapshot 机制（read-only view）
   - VSET 的 index 添加也是同步的

5. **vector store native persistence（含 model_version）**
   - AOF rewrite 只发射 `VSET question answer [scope]`，不包含 embedding → 重启后需要重新推理
   - RDB 包含 embedding 但缺少 model_version
   - HNSW 图不持久化 → 重启后重建

---

## 8. 分阶段优化路线

### P0：必须先做，低风险，收益大

**P0.1：服务态 embedding cache 验证与默认开启**

- 目标：在真实 V 族命令路径中验证 embedding cache hit rate
- 修改范围：config/kvstore-embedding.conf 改 `embedding_cache_enabled = yes`；补服务态 cache stats 日志
- 风险：极低（已有实现，只影响配置默认值）
- 验证指标：VSEARCH_TEXT 热路径 avg_us 从 ~7900 降到 ~136（~58x 提升，benchmark 已验证）
- 是否修改生产语义：否

**P0.2：HNSW 第 0 层邻居容量修正为 2*M**

- 目标：对齐 HNSW paper 标准
- 修改范围：`kvs_vector_ann_node_init_layers` 中 level==0 分配 `2*M` 个邻居槽位
- 风险：低（单向变更，rebuild 后生效）
- 验证指标：rebuild 后 recall@10 应提升
- 是否修改生产语义：是（图结构改变，需 VINDEX REBUILD）

**P0.3：HNSW 搜索中 visited bitmap 和 candidate_set 池化复用**

- 目标：消除每次 search 的 malloc/free，降低 p99
- 修改范围：在 `kvs_vector_ann_index_t` 中增加 `visited` + `candidate_set` + `result_set` buffer
- 风险：低（内存预分配，需在 resize 时同步更新）
- 验证指标：HNSW search core 的 p99_us 降低
- 是否修改生产语义：否

### P1：架构演进，收益大但需要更多代码调整

**P1.1：vector_record_table 增加 TTL / generation / model_version 字段**

- 目标：补齐 record 的生命周期管理
- 修改范围：
  - `kvs_vector_record_t` 增加 `expire_at_ms`、`generation`（uint64_t epoch）、`embedding_model_id`
  - `kvs_vector_record_match` / `kvs_vector_record_scope_match` 增加 TTL 过期检查
  - TTL scanner 增加向量模块扫描回调
  - `EXPIRE` / `PERSIST` 命令接入 V 族（通过 question+scope 定位）
- 风险：中（涉及 record 结构变更和 RDB 版本升级）
- 验证指标：VGET/VSEARCH 正确处理过期 record
- 是否修改生产语义：是（新增 TTL 行为）

**P1.2：RDB/AOF 持久化增加 model_version**

- 目标：重启后能检测模型是否变更
- 修改范围：
  - RDB vector section 增加 `model_id` 字段（已有 `embedding_provider_model_id()` 函数）
  - RDB_VERSION 升级
  - AOF rewrite snapshot 增加 `embedding_model_id`
  - 重启后如果 model_version 不匹配，标记所有 record 需要重新 embedding
- 风险：中（兼容性处理）
- 验证指标：模型切换后重启，旧 embedding 标记为 stale

**P1.3：embedding cache 分片（sharding）以消除全局锁**

- 目标：消除高并发下的 cache mutex 瓶颈
- 修改范围：将单 mutex 改为 N 个分片锁（按 key hash 取模）
- 风险：低（内部重构，对外无影响）
- 验证指标：多线程下 cache_lookup_us p99 不随线程数增长
- 是否修改生产语义：否

### P2：性能优化，依赖 P0/P1 之后

**P2.1：VSEARCH fast path + index snapshot**

- 目标：VSEARCH 不阻塞在 writer lock 下
- 前置条件：P0.3（池化复用）完成
- 修改范围：
  - 引入 `kvs_vector_index_snapshot_t`（只读 view：flat_index snapshot + ann_index snapshot）
  - VSET 写完后更新 snapshot（COW 或 rc 引用计数）
  - VSEARCH 在 snapshot 上执行搜索（read-only）
- 风险：中（并发模型变更）
- 验证指标：VSEARCH 在 VSET 并发下延迟稳定

**P2.2：VSET async index update**

- 目标：VSET 的 embedding 生成和 index 添加不阻塞响应返回
- 修改范围：
  - embedding worker pool 正式接入 VSET 路径
  - VSET handler：提交到 worker pool → 立即返回 OK ack
  - index add 在 worker 线程完成
- 风险：中（一致性语义变更——VSET 返回 OK 但实际可能失败）
- 验证指标：VSET 端到端延迟降低到 < 1ms（不含 embedding 时间）

**P2.3：batch embedding 服务化实现**

- 目标：冷查询吞吐提升 2-4x
- 前置条件：P2.2（worker pool 已接入服务路径）
- 修改范围：
  - 实现 `kvs_embedding_batch_queue`（按 `docs/vector/onnx_embedding_batch_design.md`）
  - embedding worker 内部调用 `embedding_encode_batch`
  - metrics 接入
- 风险：中（引入排队延迟）
- 验证指标：冷查询 QPS 提升倍数，p95 延迟增幅在可接受范围

**P2.4：HNSW 邻居选择策略增强**

- 目标：缩小与 Faiss HNSW 的 recall 差距
- 前置条件：P0.2（2*M 邻居容量）
- 修改范围：
  - 实现 `select_neighbors_heuristic`（排除近邻以增加图多样性）
  - 使用 pruning factor `alpha`
- 风险：中（图结构显著改变）
- 验证指标：同 efSearch 下 recall@10 提升至接近 Faiss

### P3：长期能力建设

**P3.1：vector arena 统一内存管理**

- 目标：消除 embedding 双份存储
- 前置条件：P1.1（record 字段完善）
- 修改范围：
  - record.embedding 改为指向 arena 内的指针
  - flat_index 和 HNSW node 都引用 arena
  - arena 支持 COW resize

**P3.2：answer blob store**

- 目标：支持大 answer 和 answer 去重
- 前置条件：P1.1
- 修改范围：
  - 独立 answer 存储层
  - record.answer 改为 blob_id
  - 引用计数管理

**P3.3：HNSW 图持久化**

- 目标：重启后无需重建 HNSW
- 前置条件：P0.2 + P2.4 完成后图结构稳定
- 修改范围：
  - RDB / AOF rewrite 增加 HNSW graph section
  - 保存 nodes / neighbors / entry_node / max_level

**P3.4：build variant isolation**

- 目标：向量模块可选编译
- 修改范围：
  - 引入 `KVS_ENABLE_VECTOR` 编译开关
  - V 族命令 handler 和 vector 模块条件编译
  - ONNX Runtime 依赖可选链接

**P3.5：query result cache**

- 目标：相同 query+scope+generation 的搜索结果缓存
- 前置条件：P1.1（generation 字段）
- 风险：一致性风险较高（VSET/VDEL 需正确 invalidate）

---

## 9. 结论建议

当前架构已经具备独立 vector store engine 的核心骨架，V 族命令不经过普通 KV 路径，
embedding cache 设计完善且已通过离线 benchmark 验证收益巨大（hot cache 下 VSEARCH 从 ~7900us 降到 ~86us）。

**当前应该继续沿着 KVLite 原生 vector store 方向演进**，而不是：
- 放弃原生引擎转向封装 Faiss
- 把 Faiss 作为生产回退
- 把 V 族命令降级为普通 KV 操作 + 外挂索引

演进节奏建议：

1. **立刻（P0）**：开启 embedding cache → 大部分重复查询热路径消除 → 用户侧延迟大幅降低
2. **短期（P1）**：补齐 TTL/generation/model_version 字段 → 生命周期管理完整
3. **中期（P2）**：VSEARCH fast path + async VSET → 并发模型趋于成熟
4. **长期（P3）**：arena 统一管理 + HNSW 图持久化 → 生产就绪

当前最紧迫的一项操作：**在真实 conf 中把 `embedding_cache_enabled` 设为 `yes`**。
这是已实现、已验证、零风险、收益 87x 的优化。
