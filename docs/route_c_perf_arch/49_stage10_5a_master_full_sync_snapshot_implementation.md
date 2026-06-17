# Stage 10.5A: Master Full Sync Snapshot Implementation

## 1. 本阶段目标

在不改变 replication/store 语义的前提下，把 master full sync 从：

- `slave_sync_thread` 直接持 `g_kv_data_rwlock` 读锁遍历 live store

改为：

- `slave_sync_thread` 提交 `full_sync_snapshot_task`
- executor 持 `g_kv_data_rwlock` 读锁构造普通 KV snapshot
- `slave_sync_thread` 仅发送 snapshot
- 发送完成后由 `slave_sync_thread` 释放 snapshot

## 2. 代码变更概览

### 2.1 新增 snapshot 类型和 API

在 `include/kvs_command_executor.h` 中新增：

- `kvs_full_sync_snapshot_backend_t`
- `kvs_full_sync_snapshot_entry_t`
- `kvs_full_sync_snapshot_t`
- `kvs_command_executor_build_full_sync_snapshot()`
- `kvs_full_sync_snapshot_destroy()`

snapshot 仅覆盖当前 full sync 已支持的普通 KV：

- array
- rbtree
- hash
- skiptable

不包含：

- vector store
- HNSW / flat / sq8
- id_map / scope map

### 2.2 executor 内新增 full sync snapshot task

`src/core/kvs_command_executor.c` 新增 `kvs_full_sync_snapshot_task_t`：

- 使用独立 `snapshot_head/snapshot_tail` 队列
- 由请求线程同步等待
- task 本身由请求线程分配和释放
- executor 只负责填充 `status + snapshot` 并 `signal`

这是一个最小化实现，不扩成通用 persistence snapshot framework。

## 3. executor build 流程

当前实现流程：

```text
slave_sync_thread
  → kvs_command_executor_build_full_sync_snapshot(&snapshot)
    → 分配 snapshot task
    → 入 executor snapshot queue
    → 阻塞等待 done

executor thread
  → pop snapshot task
  → pthread_rwlock_rdlock(&g_kv_data_rwlock)
  → count live full-sync entries
  → 分配 snapshot / entries
  → copy key/value/expire_at_ms
  → pthread_rwlock_unlock(&g_kv_data_rwlock)
  → signal waiter
```

构造时保持两阶段：

1. count 当前活跃普通 KV 条目
2. 分配并复制 entry 副本

每条 snapshot entry 保存：

- backend kind
- key/key_len
- value/value_len
- expire_at_ms

## 4. sync thread send 流程

`src/distributed/distributed.c::slave_sync_thread()` 现在变为：

```text
build snapshot
→ iterate snapshot entries
→ 根据 backend 发送 SET/RSET/HSET/SSET
→ 如果 expire_at_ms >= 0，补发 PEXPIREAT
→ destroy snapshot
→ 进入原有 recv loop 等待断开
```

因此 `slave_sync_thread` 不再：

- 持 `g_kv_data_rwlock`
- 遍历 `global_array/global_rbtree/global_hash/global_skiptable`
- 直接读取 live node 指针

## 5. Ownership

本轮 ownership 明确如下：

- snapshot task：调用者分配，等待完成后调用者释放
- snapshot：executor build 成功后转交给 `slave_sync_thread`
- snapshot entries/key/value：作为 snapshot 子对象，由 `kvs_full_sync_snapshot_destroy()` 统一释放

失败路径：

- build 失败：executor 不发布 snapshot，请求线程拿到 `status != 0`
- send 失败：`slave_sync_thread` 销毁 snapshot 并关闭当前 slave 连接
- stop/drain 失败：`kvs_command_executor_stop()` 会把排队中的 snapshot task 标记失败并唤醒等待者

## 6. Shutdown 安全性

当前 shutdown 相关事实：

- `kvs_command_executor_stop()` 先于 `dest_kvengine()`
- `dest_kvengine()` 内先 `distributed_shutdown()`
- `distributed_shutdown()` join 所有 slave sync thread
- 之后才销毁 live store/vector

配合本轮实现，可得到以下安全性：

### 6.1 task 等待安全

若 snapshot task 仍在队列中，`kvs_command_executor_stop()` 会：

- 取下 queued snapshot tasks
- `status=-1`
- `signal` waiter

不会遗留永久等待。

### 6.2 snapshot 对象安全

若 snapshot 已构造完成并交给 sync thread：

- sync thread 独占它
- 在发送完成或失败时本线程销毁
- `distributed_shutdown()` join 该线程后，store 才会 destroy

因此没有：

- snapshot 泄漏
- UAF
- double free

## 7. replication 语义保持

本轮明确未改动：

- 增量复制编码格式
- `master_sync_command()` 的 op 映射
- slave apply 的 `replicated_command_task`
- vector replication 语义

变化仅在 full sync 输入来源：

```text
之前: live store traversal
之后: executor-built snapshot traversal
```

发送内容仍然是：

- 普通 KV：`SET/RSET/HSET/SSET`
- TTL metadata：`PEXPIREAT`

## 8. 本轮未做的事

本轮明确未做：

- RDB save snapshot 化
- AOF rewrite snapshot 化
- vector/HNSW full sync
- reactor/proactor replication fallback 收敛
- rwlock removal

## 9. 结果

Stage 10.5A 完成后：

- master full sync 不再直接遍历 live store
- full sync live traversal blocker 从 sync thread 收敛到 executor snapshot build
- snapshot ownership / failure path / shutdown path 明确
- 后续 Stage 10.6/10.7 可复用这套 ownership 和 drain discipline
