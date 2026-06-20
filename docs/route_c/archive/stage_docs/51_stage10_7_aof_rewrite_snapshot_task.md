# Stage 10.7: AOF Rewrite Snapshot Task

## 1. 目标

把 AOF rewrite 从直接遍历 live store/vector，改为基于 executor 构造的 snapshot 重写 AOF。

改造前：
- 旧方案外层持 `g_kv_data_rwlock` 读锁遍历 live store
- 在读锁内执行文件 IO

改造后：
- 调用方通过 `kvs_command_executor_build_aof_rewrite_snapshot()` 获取 snapshot
- executor 持 `g_kv_data_rwlock` 读锁构造 AOF snapshot
- `write_snapshot_to_tempfile()` 只负责把 snapshot 写到文件
- 写完后由调用方释放 snapshot

## 2. 代码变更

### 2.1 新增 snapshot API

在 `include/kvs_command_executor.h` 新增：

- `kvs_aof_rewrite_snapshot_vector_entry_t` — 仅包含 question/answer/scope（AOF 只写 VSET 命令）
- `kvs_aof_rewrite_snapshot_t` — KV entries 复用 `kvs_rdb_snapshot_kv_entry_t`，vector 使用独立的精简类型
- `kvs_command_executor_build_aof_rewrite_snapshot()`
- `kvs_aof_rewrite_snapshot_destroy()`

### 2.2 executor 内新增 aof rewrite snapshot task

`src/core/kvs_command_executor.c` 新增：

- `kvs_aof_rewrite_snapshot_task_t` — task 结构体（mu/cv/done/status/snapshot）
- `aof_snapshot_head/aof_snapshot_tail` — 独立队列
- `kvs_aof_rewrite_snapshot_builder_t` — 构造器状态
- `kvs_command_executor_execute_aof_snapshot()` — executor 执行函数
- `kvs_command_executor_create_aof_snapshot()` — count + alloc + fill
- `kvs_command_executor_count_aof_snapshot()` / `kvs_command_executor_fill_aof_snapshot()` — 两阶段构造

流程：

```text
aof_rewrite_background() / aof_rewrite_buffer_write()
  → kvs_command_executor_build_aof_rewrite_snapshot(&snapshot)
    → 如果调用者是 executor 线程：直接持 rdlock 构造（快速路径）
    → 否则：enqueue task → wait

executor thread
  → pop aof snapshot task
  → pthread_rwlock_rdlock(&g_kv_data_rwlock)
  → count ordinary KV + vector records
  → allocate snapshot
  → copy ordinary KV payload
  → copy vector payload (question/answer/scope only)
  → pthread_rwlock_unlock(&g_kv_data_rwlock)
  → signal waiter
```

### 2.3 rewrite 侧改造

`src/persistence/kvs_aof_rewrite.c` 现在不再：

- 直接遍历 `global_array/global_rbtree/global_hash/global_skiptable`
- 直接遍历 `g_vector_state.store`
- 在 rewrite 期间外层持 `g_kv_data_rwlock`

而是改为：

```text
aof_rewrite_background()
  → build snapshot via executor
  → fork+COW (reactor) 或 thread (proactor)
  → write_snapshot_to_tempfile(fd, snapshot)
  → finish_rewrite / atomic_replace_file
  → kvs_aof_rewrite_snapshot_destroy(snapshot)
```

序列化函数改为 snapshot writer：

- `write_kv_snapshot_entries()` — 写 SET/RSET/HSET/SSET + PEXPIREAT 命令
- `write_vector_snapshot_to_tempfile()` — 写 VSET 命令
- `write_ttl_command_if_needed()` — 写 PEXPIREAT 命令
- `write_vset_command_snapshot()` — 单条 VSET RESP 编码

这些函数保持 AOF 命令格式不变（RESP 数组格式，与增量 AOF append 一致），所以 AOF replay 逻辑不变。

### 2.4 移除死代码

移除 `kvs_aof_rewrite.c` 中的死代码：

- `aof_rewrite_acquire_store_read_lock()` — 从未被调用
- `aof_rewrite_release_store_read_lock()` — 从未被调用

这些函数是旧方案残留（直接持 g_kv_data_rwlock 遍历 live store 的 lock/unlock wrapper）。

## 3. Snapshot 结构

`kvs_aof_rewrite_snapshot_t` 包含：

- 普通 KV：array/rbtree/hash/skiptable（每种带 key/value/expire_at_ms，复用 `kvs_rdb_snapshot_kv_entry_t`）
- Vector：仅 question/answer/scope（`kvs_aof_rewrite_snapshot_vector_entry_t`）

AOF rewrite 只需要写出 VSET 命令重放 vector 数据，不需要 embedding/id/state/deleted 等内部字段。

不包含：
- HNSW graph 内部结构
- flat / sq8 index 内部缓存
- id_map / scope map
- vector embedding 向量

## 4. Ownership

- snapshot task：调用线程分配、等待、销毁
- snapshot：executor build 成功后转交给调用方
- snapshot 子对象：由 `kvs_aof_rewrite_snapshot_destroy()` 统一释放

fork+COW 路径中，子进程拥有 COW 副本，子进程退出后由父进程释放原始 snapshot。

## 5. Failure Path

### 5.1 build 失败

如果：
- executor 未运行 / stopping
- task 入队失败
- 读锁获取失败
- snapshot 分配/复制失败

则：
- 返回 -1
- 不写 tmp 文件
- 不发布半成品 snapshot

### 5.2 写文件失败

如果：
- tmp 文件打开失败
- section write 失败
- fsync 失败

则：
- 清理 tmp 文件
- 销毁 snapshot
- 重置 `rewrite_in_progress` 标志

### 5.3 shutdown 期间

当前 shutdown 顺序：
```text
kvs_rdb_save()
→ kvs_ttl_background_stop()
→ kvs_command_executor_stop()     ← drain pending aof snapshot tasks
→ dest_kvengine()
```

正常路径里：
- AOF rewrite 在 executor 运行期间触发
- snapshot 在 executor stop 之前已完成
- 若队列中仍有未处理 task，stop() 标记失败并唤醒 waiter

## 6. 语义保持

本轮保持不变：
- AOF 命令格式（RESP 数组：SET/RSET/HSET/SSET/VSET/PEXPIREAT）
- AOF replay/load 逻辑
- 增量 AOF append 语义
- fork+COW 子进程机制
- Proactor 线程模式

变化仅在数据来源：
```
之前: live traversal + lock-held IO
现在: executor snapshot + lock-free IO
```

## 7. 结果

Stage 10.7 完成后：
- AOF rewrite 不再直接遍历 live store/vector
- AOF rewrite 不再在 `g_kv_data_rwlock` 读锁内执行文件 IO
- 普通 KV + vector 的 snapshot ownership、failure path、shutdown path 已明确
- 路线 C 剩余 blocker 缩减为：
  - reactor/proactor replication fallback
  - proactor OOM fallback
