# Stage 10.3: Slave Apply replicated_command_task

## 背景

Stage 10.2 (`45_stage10_2_lazy_expire_and_sync_fallback_audit.md`) 完成了 lazy expire ownership
审计和同步 fallback 路径清单。剩余 rwlock removal blocker 之一是 slave apply 仍由
replication thread 直接修改 store/TTL。

本阶段把 slave apply 收敛为 `replicated_command_task`，由 single command executor 串行 apply。

## Stage 10.2 后剩余 Blocker

| # | Blocker | 状态 |
|---|---------|------|
| 1 | TTL active expire | ✅ Stage 10.1 |
| 2 | Master full sync 直接遍历 live store | ❌ |
| 3 | Slave apply 直接修改 KV/TTL | → Stage 10.3 |
| 4 | AOF rewrite 直接遍历 live store | ❌ |
| 5 | RDB save 直接遍历 live store | ❌ |
| 6 | Ntyco 同步 handler | ❌ |
| 7 | Reactor/proactor replication fallback | ❌ |
| 8 | Proactor OOM fallback | ❌ |

## 当前 Slave Apply 直接写 Store/TTL 的路径（改造前）

```text
slave_connect_master (replication thread)
  → recv master sync message
  → ms_decode_message → op, key, val
  → pthead_rwlock_wrlock(&g_kv_data_rwlock)
  → kvs_array_set/del/mod 或 kvs_rbtree_set/del/mod 等
  → ttl_set_any_key_expire_at_ms (TTL 变更)
  → slave_append_aof_from_op (AOF)
  → pthead_rwlock_unlock(&g_kv_data_rwlock)
```

问题：
- Replication thread 拥有 store 写权限
- Replication thread 绕过 command executor
- Slave apply 与 client command 只能依赖 `g_kv_data_rwlock` 互斥

## 新 replicated_command_task 架构

```text
slave_connect_master (replication thread)
  → recv master sync message
  → ms_decode_message → op, key, val
  → kvs_command_executor_submit_replicated(op, key, klen, val, vlen, expire_at)
    → heap alloc task + deep copy key/value
    → enqueue to replicated queue under cmd_mu
  → kvs_free(key), kvs_free(val)  ← replication thread 释放自己的副本

single command executor
  → pop queue: prepared → replicated → TTL → command
  → kvs_command_executor_execute_repl()
    → wrlock(&g_kv_data_rwlock)
    → set_sync_context(1)   ← 防止 master_sync_command 回环
    → kvs_command_executor_apply_repl_store()
    → unlock(&g_kv_data_rwlock)
    → AOF append (if enabled)
    → set_sync_context(old)
    → destroy task
```

核心原则：
- Replication thread 只负责接收、解析、构造 task
- Executor 才能修改 store/TTL
- Replicated command apply 保持 master 消息顺序（replicated queue FIFO）
- `g_kv_data_rwlock` 完整保留

## Task Ownership

`kvs_replicated_command_task_t` 定义（`include/kvs_command_executor.h:18-27`）：

```c
typedef struct kvs_replicated_command_task_s {
    struct kvs_replicated_command_task_s *queue_next;
    ms_op_t op;
    char *key;
    size_t key_len;
    char *value;
    size_t value_len;
    int64_t expire_at_ms;
    uint64_t enqueue_us;
} kvs_replicated_command_task_t;
```

**字段说明**：
- `op`: master-slave 协议操作类型（MS_OP_ARRAY_SET/DEL/MOD/TTL_PEXPIREAT/TTL_PERSIST 等）
- `key` / `key_len`: deep copy 的 key
- `value` / `value_len`: deep copy 的 value（DEL 操作时为 NULL/0）
- `expire_at_ms`: TTL 操作的过期时间戳（非 TTL 操作时为 0）
- `enqueue_us`: 入队时间戳（用于 wait time stats）
- `queue_next`: 链表指针

**支持的操作类型**：
- `MS_OP_ARRAY_SET` / `MS_OP_RBTREE_RSET` / `MS_OP_HASH_HSET` / `MS_OP_SKIPTABLE_SSET` / `MS_OP_PAPERSET`
- `MS_OP_ARRAY_DEL` / `MS_OP_RBTREE_RDEL` / `MS_OP_HASH_HDEL` / `MS_OP_SKIPTABLE_SDEL` / `MS_OP_PAPERDEL`
- `MS_OP_ARRAY_MOD` / `MS_OP_RBTREE_RMOD` / `MS_OP_HASH_HMOD` / `MS_OP_SKIPTABLE_SMOD` / `MS_OP_PAPERMOD`
- `MS_OP_TTL_PEXPIREAT` / `MS_OP_TTL_PERSIST`

**Ownership 生命周期**：

| 阶段 | 操作 | Owner |
|------|------|-------|
| Replication thread 解析 | `malloc` key + val 副本 | Replication thread |
| Submit API | `kvs_malloc` task + deep copy key/value | Submit API（临时） |
| Submit 成功 | task 入队 replicated queue | **Executor** |
| Submit 失败（malloc/stopped） | `destroy_repl_task` | 无泄漏 |
| Replication thread 清理 | `kvs_free` 自己的 key/val 副本 | 无泄漏 |
| Executor pop | 从 replicated queue 取出 | Executor |
| Executor execute | `kvs_command_executor_apply_repl_store` | Executor |
| Executor done | `kvs_command_executor_destroy_repl_task` | 释放 key + value + task |
| Executor drain | stop() 遍历 replicated queue | 递增 dropped，释放所有 |

- ✅ Task 不持有 response_task
- ✅ Task 不持有 session ref
- ✅ Task 不触发 client response
- ✅ Task 不与 command_task 共享内存布局

## Queue 设计

### 消费顺序

```
prepared queue  (Stage 9 - embedding worker pool 结果)
↓
replicated queue (Stage 10.3 - slave apply)
↓
TTL queue       (Stage 10.1 - active expire)
↓
command queue   (normal client commands)
```

**顺序理由**：
1. **Prepared 优先**：embedding 计算已经完成，延迟投递会增大 response latency
2. **Replicated 优先于 TTL**：slave apply 需要尽快跟上 master 状态；TTL 是 best-effort 清理
3. **TTL 优先于 Normal**：TTL task 极少（最多 32/秒），执行极快，不会阻塞 normal command
4. **Replicated 不优先于 Prepared**：避免 slave apply 高峰延迟 embedding response

### 线程安全

- Replicated queue 与其他三个队列共享同一个 `cmd_mu` 互斥锁
- 所有 enqueue/dequeue/drain 操作在锁内
- `cmd_cv` 条件变量覆盖所有四个队列
- `pthread_cond_signal` 在每次 submit 后调用

### FIFO 保证

- Replicated queue 是独立链表（repl_head/repl_tail），submit 在尾部，pop 在头部
- Single executor 保证串行 apply
- Replication thread 按 master 消息顺序 submit → executor 按 submit 顺序 apply
- **不保证** replicated task 之间不会插入 prepared/TTL task，但这不影响 slave apply 的最终一致性：prepared task 只修改 vector（与 KV store 正交），TTL task 只删除过期 key

## Executor Apply 流程

### kvs_command_executor_execute_repl (line 958-1001)

```c
static void kvs_command_executor_execute_repl(kvs_replicated_command_task_t *task) {
    // 1. 记录 wait time
    // 2. 获取 g_kv_data_rwlock 写锁
    pthread_rwlock_wrlock(&g_kv_data_rwlock);
    // 3. 设置 sync_context=1（防止 master_sync_command 回环）
    set_sync_context(1);
    // 4. 执行 store apply
    ret = kvs_command_executor_apply_repl_store(task);
    // 5. 释放写锁
    pthread_rwlock_unlock(&g_kv_data_rwlock);
    // 6. AOF append（如果 apply 成功）
    if (ret == 0) kvs_command_executor_repl_append_aof(task);
    // 7. 恢复 sync_context 并释放 task
    set_sync_context(old_sync_context);
    kvs_command_executor_destroy_repl_task(task);
}
```

### kvs_command_executor_apply_repl_store (line 788-896)

直接调用底层 store API：
- `kvs_array_set_len` / `kvs_rbtree_set_len` / `kvs_hash_set_len` / `kvs_skiptable_set_len`
- `kvs_array_del_len` / `kvs_rbtree_del_len` / `kvs_hash_del_len` / `kvs_skiptable_del_len`
- `kvs_array_mod_len` / `kvs_rbtree_mod_len` / `kvs_hash_mod_len` / `kvs_skiptable_mod_len`
- `ttl_set_any_key_expire_at_ms`（TTL 操作）

不经过任何 command handler 函数，不触发 lazy expire。

### AOF 写入

`kvs_command_executor_repl_append_aof` (line 899-955) 将操作转为 RESP 格式写入 AOF。
**行为与改造前一致**：slave 本地 AOF 记录复制的写操作。

## Ordering 语义

### Slave apply 顺序保证

- Replication thread 按 TCP 流顺序接收消息
- 解析后按顺序 submit 到 replicated queue
- Replicated queue 是 FIFO
- Single executor 串行 apply → master 消息顺序不被破坏

### 是否允许 Normal Command 插入 Replicated Task 之间

**允许。** 当前设计：
- Prepared tasks 优先于 replicated tasks
- Normal commands 在 replicated queue 清空后处理
- 一个 replicated task 不会在它的前一个 replicated task 之后被另一个 replicated task 超越（同一 queue 内 FIFO）

**语义分析**：
- 如果 slave 同时接受本地写，本地写（normal command）可能与复制写（replicated task）交错
- 当前 slave role 下，slave 是只读的（客户端的 SET/DEL 等写命令被拒绝）
- 因此正常路径下 client command 只有 GET/EXIST 等读命令
- GET 命令的执行在 replicated task 之间不会影响最终一致性

### 如果 Slave 允许本地写

当前 slave role 拒绝所有写命令（在 `kvs_command_task_build` 中检查）。即使未来放宽，executor 的串行执行保证了最终顺序一致性。

## Failure Path

| Failure 点 | 行为 | 资源释放 |
|---|---|---|
| Submit 参数无效（key=NULL） | 递增 `repl_task_submit_failed`，返回 -1 | 无分配 |
| Task malloc 失败 | 递增 `repl_task_submit_failed`，返回 -1 | 无泄漏 |
| Key malloc 失败 | 递增 `repl_task_submit_failed`，释放 task，返回 -1 | ✅ task 已释放 |
| Value malloc 失败 | 递增 `repl_task_submit_failed`，`destroy_repl_task`，返回 -1 | ✅ task+key 已释放 |
| Executor stopped | 递增 `repl_task_submit_failed`，`destroy_repl_task`，返回 -1 | ✅ task+key+value 已释放 |
| Executor execute wrlock 失败 | 递增 `repl_task_failed`，`destroy_repl_task` | ✅ 全部释放 |
| Store apply 失败 | 递增 `repl_task_failed`，`destroy_repl_task` | ✅ 全部释放 |
| Shutdown drain | 递增 `repl_task_dropped`，`destroy_repl_task` | ✅ 全部释放 |
| Replication socket 断开 | 已提交 task 仍由 executor apply 或被 drain | 无泄漏 |

## Shutdown 顺序

```text
network exit
RDB save
kvs_ttl_background_stop()       ← 先停 TTL scanner
kvs_command_executor_stop()
  → kvs_embedding_worker_pool_stop()
  → 设置 stopping, broadcast cv
  → pthread_join(executor_thread)   ← executor 处理完当前 task 后退出
  → drain command queue
  → drain prepared queue
  → drain replicated queue          ← 释放所有未执行的 replicated task
  → drain TTL queue
  → drain response queue
  → close eventfd
destroy store
```

## Stats

新增 14 个 replicated executor stats（`include/kvs_command_executor.h:103-116`）：

| Stat | 说明 | 更新位置 |
|------|------|---------|
| `repl_task_submitted` | 成功入队数 | submit |
| `repl_task_executed` | 成功 apply 数 | execute_repl (ret==0) |
| `repl_task_failed` | apply 失败数 | execute_repl (ret!=0 or wrlock fail) |
| `repl_task_dropped` | shutdown drain 丢弃数 | stop |
| `repl_task_submit_failed` | submit 失败数 | submit (入队前) |
| `repl_queue_depth` | 当前队列深度 | submit +1, pop/drain -1 |
| `repl_queue_max_depth` | 历史最大深度 | submit |
| `repl_wait_count/avg/max` | 排队等待时间 | execute_repl |
| `repl_set_count` | SET 操作数 | execute_repl |
| `repl_del_count` | DEL 操作数 | execute_repl |
| `repl_mod_count` | MOD 操作数 | execute_repl |
| `repl_ttl_count` | TTL 操作数 | execute_repl |

**会计等式**：`submitted == executed + failed + dropped`

所有 stats 使用 `__atomic_*` (RELAXED) 更新。

## g_kv_data_rwlock 是否仍保留

**是。** `g_kv_data_rwlock` 完整保留。Replicated task apply 在 `kvs_command_executor_execute_repl()` 中持写锁（line 978），与正常命令、TTL task、lazy expire 互斥。

## 语义变化检查

| 检查项 | 是否改变 | 说明 |
|--------|---------|------|
| RESP 协议 | ❌ 不改变 | |
| Store 语义 | ❌ 不改变 | 直接调用相同的底层 API |
| AOF | ❌ 不改变 | 仍写 AOF（在 executor 中） |
| Replication 语义 | ❌ 不改变 | 仍按 master 消息顺序 apply |
| HNSW | ❌ 不改变 | Replicated task 不涉及 vector |
| TTL | ❌ 不改变 | `ttl_set_any_key_expire_at_ms` 相同 |
| sync_context | ⚠️ 改变执行线程 | 从 replication thread 移到 executor thread |
| master_sync_command | ❌ 不改变 | sync_context=1 防止回环 |
| SET/DEL/MOD/EXPIRE/TTL | ❌ 不改变 | |
| VSET/VSEARCH | ❌ 不改变 | |

## Tests 结果

测试脚本 `tests/replicated_command_task_stage10_3.sh`：

```
bash -n tests/replicated_command_task_stage10_3.sh:    ✅ passed
tests/replicated_command_task_stage10_3.sh:             ✅ passed
  - stage10_3 replication suite: ok
  - repl stats: ok
```

| 测试用例 | 结果 |
|----------|------|
| SET 复制 (master → slave GET) | ✅ PASSED |
| DEL 复制 | ✅ PASSED |
| TTL 复制 (EXPIRE → TTL) | ✅ PASSED |
| PERSIST 复制 | ✅ PASSED |
| MOD 复制 | ✅ PASSED |
| 10 keys 顺序验证 | ✅ PASSED |
| 交错操作 SET/DEL/SET | ✅ PASSED |
| Replicated stats (submitted/executed/failed/dropped/depth) | ✅ PASSED |
| Crash 签名检测 | ✅ PASSED |

## Regression 结果

```
tests/ttl_executor_stage10_1.sh:          ✅ passed
tests/lazy_expire_stage10_2.sh:           ✅ passed
tests/embedding_worker_pool_stage9.sh:    ✅ passed
tests/embedding_worker_pool_stage9_1.sh:  ✅ passed
```

## Sanitizer 结果

待运行：
```bash
# ASAN/UBSAN
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=address,undefined" \
make embedding ENABLE_EBPF=0

ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
tests/replicated_command_task_stage10_3.sh

# TSAN
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=thread -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=thread" \
make embedding ENABLE_EBPF=0

TSAN_OPTIONS="halt_on_error=0 exitcode=0" \
tests/replicated_command_task_stage10_3.sh
```

## 剩余 Blocker（Stage 10.3 后）

| # | Blocker | 状态 |
|---|---------|------|
| 1 | TTL active expire | ✅ Stage 10.1 |
| 2 | Master full sync 直接遍历 live store | ❌ |
| 3 | Slave apply 直接修改 KV/TTL | ✅ Stage 10.3 |
| 4 | AOF rewrite 直接遍历 live store | ❌ |
| 5 | RDB save 直接遍历 live store | ❌ |
| 6 | Ntyco 同步 handler | ❌ |
| 7 | Reactor/proactor replication fallback | ❌ |
| 8 | Proactor OOM fallback | ❌ |

## 下一阶段建议

建议进入 **Stage 10.4：Ntyco Async Executor 改造（或 Ntyco no-rwlock-removal 标记）**。

Ntyco 是最大的绕过 executor 的路径——所有 ntyco 命令（包括 GET/EXIST/TTL）都在 coroutine 中同步执行 `kvs_filter_protocol`。如果完整改造 ntyco 成本过高，可以将 ntyco 标记为 "不支持 rwlock removal 的网络模式"，优先处理更实际的目标（AOF/RDB snapshot_task）。

后续优先级：
1. Stage 10.4: Ntyco async executor 改造 或 no-rwlock-removal 标记
2. Stage 10.5: Master full sync snapshot_task
3. Stage 10.6: RDB save snapshot_task
4. Stage 10.7: AOF rewrite snapshot_task
5. Stage 10.8: Reactor/proactor replication fallback 收敛
6. Stage 10.9: Shutdown/destroy 收敛 + 最终 rwlock removal 评估
