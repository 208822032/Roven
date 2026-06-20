# KVLite Vector Store Stage 1-2: Vector record 语义补齐与服务态 prepare 路径闭环

日期：2026-06-20

---

## 1. 背景

目标是将 KVLite 内置向量模块从实验路径推进到端到端闭环：

- **保留原生 vector engine**：不引入外部向量库作为生产回退
- **外部命令不变**：`VSET` / `VSEARCH` / `VGET` / `VDEL` / `VINFO` / `VINDEX`
- **范围边界**：不引入 `data_entry`、不引入 `arena` / `blob store` / `index snapshot`
- **Faiss 只作为 baseline 参考**，不作为生产回退路径

---

## 2. 阶段一改动：Vector Record 语义补齐

### 2.1 `kvs_vector_record_t` 新增字段

```c
typedef struct kvs_vector_record_s {
    // ... existing fields ...
    int64_t created_at_ms;
    int64_t updated_at_ms;

    int64_t expire_at_ms;              // NEW  KV_TTL_NO_EXPIRE (-1)
    uint64_t generation;               // NEW  store epoch on last write
    uint32_t embedding_model_version;  // NEW  model identity from embedding_provider_model_id()
} kvs_vector_record_t;
```

### 2.2 `kvs_vector_state_t` 新增字段

```c
typedef struct kvs_vector_state_s {
    // ... existing fields ...
    uint64_t generation;               // NEW  current store epoch
} kvs_vector_state_t;
```

### 2.3 Generation 语义

- `g_vector_state.generation` 初始化为 0
- 在以下操作成功后递增：
  - `kvs_vector_set` (VSET 新写入)
  - `kvs_vector_set_commit_prepared` (prepare/commit 路径)
  - `kvs_vector_del` (VDEL)
  - `kvs_vector_index_rebuild` (VINDEX REBUILD)

### 2.4 Embedding model version getter

```c
// src/vector/kvs_vector_embedding.c
uint32_t kvs_vector_embedding_current_model_version(void);
```

从 `embedding_provider_model_id()` 静态函数提取为 public wrapper，绑定 `provider_name | model_path | embedding_dim`。

### 2.5 RDB Vector Section v2

- `RDB_VECTOR_SECTION_VERSION` 从 1 升级到 2
- 每个 record 新增写入字段：`expire_at_ms (i64)` / `generation (u64)` / `embedding_model_version (u32)`
- 不支持 v1 格式回读（加载时报 "unsupported section version 1"）
- 加载路径通过 `kvs_vector_restore_record_with_embedding` 新参数恢复

### 2.6 Embedding cache 默认开启

```conf
# config/kvstore-embedding.conf
embedding_cache_enabled = 1
```

仅对 `local_c` (ONNX) provider 生效，hash provider 不启用。cache key 绑定 `text_hash + model_id + dim`，模型切换后不会误命中。

### 2.7 HNSW Level 0 容量 = 2*M

```c
// kvs_vector_ann_node_init_layers
// Before: capacity = m (all levels same)
// After:
size_t capacity = (l == 0) ? m * 2 : m;
```

对齐 HNSW paper 标准做法。需要 VINDEX REBUILD 后对新节点完整生效。

### 2.8 AOF 保持命令流语义

AOF rewrite 继续输出 `VSET question answer [scope]`，不持久化 embedding / generation / model_version。RDB 负责完整数据快照。

---

## 3. 阶段二改动：服务态 prepare 路径闭环

### 3.1 VSET handler 切换为 prepare / commit

```text
Before: handle_vset_command → kvs_vector_set() (monolithic)
After:  handle_vset_command → kvs_vector_set_prepare() → kvs_vector_set_commit_prepared()
```

### 3.2 VSEARCH handler 切换为 prepare / search

```text
Before: handle_vsearch_command → kvs_vector_search() (monolithic)
After:  handle_vsearch_command → kvs_vector_search_prepare() → kvs_vector_search_with_prepared()
```

### 3.3 Worker pool 最小安全接入

- `embedding_worker_pool_enabled = 1`（默认 1 worker，queue_capacity=1024）
- `per_thread_runtime_enabled = 0`（保守，不开启）
- `embedding_batch_enabled = 0`（保守，不开启）
- Worker 只执行 embedding prepare，不写 `g_vector_state` / `id_map` / `flat_index` / `ann_index`

### 3.4 Prepared 所有权防御性修复

```c
// kvs_command_task_execute_handler, VSET branch
int status = kvs_vector_set_commit_prepared(&task->prepared_set);
task->prepared_kind = KVS_COMMAND_TASK_PREPARED_NONE;  // NEW defensive line
```

防止 VSET commit 消费 prepared 后 `kvs_command_task_destroy` 二次释放。

### 3.5 VINFO 接入 worker pool / executor prepared stats

新增 10 个 VINFO 字段：

| 字段 | 来源 |
|------|------|
| `embedding_worker_pool_enabled` | `kvs_embedding_worker_pool_get_stats()` |
| `embedding_worker_threads` | 同上 |
| `embedding_worker_submitted` | 同上 |
| `embedding_worker_completed` | 同上 |
| `embedding_worker_failed` | 同上 |
| `embedding_worker_queue_depth` | 同上 |
| `embedding_worker_max_queue_depth` | 同上 |
| `executor_prepare_task_submitted` | `kvs_command_executor_get_stats()` |
| `executor_prepared_task_executed` | 同上 |
| `executor_prepared_task_failed` | 同上 |

---

## 4. 当前 VSET 调用链（worker_pool_enabled=1）

```text
client: VSET question answer [scope]
  → RESP parser (kvs_operation.c)
  → command_context_t (query_text, value, scope)
  → kvs_command_executor_submit
  → kvs_command_executor_dispatch_or_execute(task)
  → kvs_command_executor_should_async_prepare(task) → YES
  → kvs_embedding_worker_pool_submit(task)
      ↓
  → [Worker Thread] worker_thread_main
    → kvs_command_task_prepare(task)
      → kvs_vector_set_prepare(question, answer, scope, &task->prepared_set)
        → kvs_vector_embedding_generate → ONNX cache or ONNX runtime
        → L2 normalize
      → task->prepare_completed = 1
      → task->prepared_kind = KVS_COMMAND_TASK_PREPARED_VSET
    → kvs_command_executor_submit_prepared(task)
      ↓
  → [Executor Thread] kvs_command_executor_pop_prepared_locked
  → kvs_command_executor_execute(task)
    → RWLock write acquire
    → kvs_command_task_execute_sync(task)
      → kvs_command_task_execute_handler(task)
        → kvs_vector_set_commit_prepared(&task->prepared_set)
          → recheck question+scope
          → [duplicate] update answer → destroy prepared
          → [new] kvs_vector_record_take_prepared_fields
            → transfer ownership (pointers NULL'd in prepared)
            → set expire_at_ms / generation / embedding_model_version
          → kvs_vector_id_map_put
          → kvs_vector_flat_index_add
          → kvs_vector_index_add_current (HNSW add)
          → store counts update
          → g_vector_state.generation++
        → task->prepared_kind = KVS_COMMAND_TASK_PREPARED_NONE
    → kvs_command_task_post_commit (AOF repl dirty)
    → kvs_response_task_finalize
    → kvs_command_executor_push_response → network layer
    → kvs_command_task_destroy(task)
  → RESP: +OK\r\n
```

---

## 5. 当前 VSEARCH 调用链（worker_pool_enabled=1）

```text
client: VSEARCH query [scope] [topk] [threshold]
  → RESP parser (kvs_operation.c)
  → command_context_t (query_text, scope, topk, threshold)
  → kvs_command_executor_dispatch_or_execute(task)
  → kvs_embedding_worker_pool_submit(task)
      ↓
  → [Worker Thread] worker_thread_main
    → kvs_command_task_prepare(task)
      → kvs_vector_search_prepare(query, scope, &task->prepared_search)
        → kvs_vector_embedding_generate → ONNX cache or ONNX runtime
        → L2 normalize
      → task->prepared_kind = KVS_COMMAND_TASK_PREPARED_VSEARCH
    → kvs_command_executor_submit_prepared(task)
      ↓
  → [Executor Thread] kvs_command_executor_pop_prepared_locked
  → kvs_command_executor_execute(task)
    → RWLock read acquire
    → kvs_command_task_execute_sync(task)
      → kvs_vector_search_prepared_to_resp(prepared_search)
        → kvs_vector_search_with_prepared
          → kvs_vector_index_search_current (HNSW / flat / SQ8 ANN)
          → topK heap export_desc → fill results from store.records
        → format RESP array: "score:...|scope:...|question:...|answer:..."
    → kvs_command_task_post_commit
    → kvs_response_task_finalize
    → kvs_command_executor_push_response → network layer
    → kvs_command_task_destroy → kvs_vector_search_prepared_destroy
  → RESP: array of bulk strings
```

---

## 6. worker_pool_enabled=0 回退路径

当 `embedding_worker_pool_enabled = 0` 时：

- `kvs_command_executor_should_async_prepare` 返回 `false`（因为 `kvs_embedding_worker_pool_enabled() == 0`）
- Task 走同步路径：`kvs_command_executor_execute` → `kvs_command_task_prepare` (inline) → `kvs_command_task_execute_sync`
- Handler 内仍使用 prepare/commit 两段式 API
- 功能语义与 enabled=1 完全一致

---

## 7. RDB v2 字段顺序

Section 头：
```
type (u8) → section_version (u8) → metric_type (i32) → index_type (i32) → next_id (u64) → record_count (u64)
```

每个 record：
```
record_index (u64)
state (u8)
deleted (u8)
id (u64)
question (blob)
answer (blob)
scope (blob)
normalized_question (blob)
embedding_dim (i32)
embedding (float[], conditional)
created_at_ms (i64)
updated_at_ms (i64)
expire_at_ms (i64)              ← NEW v2
generation (u64)                ← NEW v2
embedding_model_version (u32)   ← NEW v2
```

---

## 8. VINFO 新增字段

```text
embedding_worker_pool_enabled         (int)
embedding_worker_threads              (int)
embedding_worker_submitted            (uint64)
embedding_worker_completed            (uint64)
embedding_worker_failed               (uint64)
embedding_worker_queue_depth          (uint64)
embedding_worker_max_queue_depth      (uint64)
executor_prepare_task_submitted       (uint64)
executor_prepared_task_executed       (uint64)
executor_prepared_task_failed         (uint64)
```

---

## 9. HNSW 2*M 修正

| 参数 | 修改前 | 修改后 |
|------|--------|--------|
| Level 0 neighbor capacity | M | 2 × M |
| Level > 0 neighbor capacity | M | M |

- 修改位置：`kvs_vector_ann_node_init_layers` (`src/vector/kvs_vector_index.c`)
- 已有节点不受影响（已分配内存不变）
- 需要 `VINDEX REBUILD` 后对全图完整生效

---

## 10. 正确性验证矩阵

| 测试 | w/p enabled=1 | w/p enabled=0 |
|------|:---:|:---:|
| VSET → OK | ✅ | ✅ |
| VSEARCH → correct result | ✅ | ✅ |
| VGET → correct answer | ✅ | ✅ |
| VDEL → OK | ✅ | ✅ |
| VSEARCH after VDEL → ERR | ✅ | ✅ |
| Scope s1→a1, s2→a2 | ✅ | ✅ |
| Overwrite old→new | ✅ | ✅ |
| HNSW multi-insert (20 records) | ✅ | ✅ |
| VINDEX REBUILD | ✅ | ✅ |
| Search after rebuild | ✅ | ✅ |
| RDB roundtrip | ✅ | ✅ |
| VINFO worker_pool_enabled correct | ✅ | ✅ |
| VINFO submitted/completed > 0 | ✅ | N/A (0) |
| VINFO failed = 0 | ✅ | ✅ |
| No crash / no leak evidence | ✅ | ✅ |

---

## 11. 明确未做事项

| 未做项 | 说明 |
|--------|------|
| `data_entry` / `entry_meta` 统一 | 推迟到向量模块稳定后 |
| `vector arena` | 当前 embedding 双份存储可接受 |
| `answer blob store` | 当前 answer inline 在 record 中 |
| `index snapshot` | 当前依赖命令锁保护并发 |
| `HNSW graph persistence` | 重启后 rebuild 可接受 |
| `batch embedding` | 实验特性，未接入服务路径 |
| `async VSET early OK` | 未做，commit 仍在 executor 内串行 |
| `Faiss fallback` | 不作为生产回退 |
| `per_thread_runtime` | 默认关闭 |
| `select_neighbors_heuristic` | 未实现 |
| `tombstone compaction` | 未做自动触发 |
| `model_version mismatch → stale` | 下一阶段 |

---

## 12. 当前验收结论

**阶段一/二 APPROVED。向量模块端到端闭环已建立。**

- VSET / VSEARCH 从实验性一体化 API 升级为服务态 prepare/commit 路径
- Record 具备类生产语义字段：TTL / generation / model_version
- RDB v2 持久化完整 embedding 数据
- Worker pool 将 embedding prepare 从 executor 同步路径解耦
- Commit 仍由 executor 串行保证一致性
- Worker pool 可随时通过一个配置开关关闭，回退同步路径
- VINFO 可观测 worker pool 和 executor 状态

---

## 13. 下一阶段建议（阶段三边界）

阶段三建议聚焦于将向量模块从"端到端闭环"推向"可长期运行的引擎"：

- **Model version mismatch → stale**：RDB load 时比对 `embedding_model_version`，不匹配标记 `stale`
- **VSEARCH skip stale**：在 `topk_heap_export_desc` candidate export 阶段过滤 stale record
- **VINDEX REBUILD refresh stale**：rebuild 时对 stale record 重新生成 embedding
- **Tombstone ratio → rebuild_recommended**：`VINFO` 在 `deleted / active` 比例超阈值时提示
- **HNSW temp buffer pooling**：`visited` bitmap 和 `candidate_set` 池化复用，降低 p99

明确不纳入阶段三范围：
- 全局 `data_entry_table`
- `vector arena`
- `answer blob store`
- `HNSW graph persistence`
- `index snapshot`
- `batch embedding` 生产化
