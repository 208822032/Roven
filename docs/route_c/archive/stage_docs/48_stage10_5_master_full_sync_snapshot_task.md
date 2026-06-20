# Stage 10.5: Master Full Sync Snapshot Task

## 1. 目标

Stage 10.5 的目标是把 master full sync 从“sync 线程直接遍历 live store”改为“executor 生成一致性 snapshot，sync 线程只发送 snapshot”。

本阶段严格保持以下边界不变：

- 不删除或弱化 `g_kv_data_rwlock`
- 不做 rwlock removal
- 不做多 executor / commit ordering / response reorder
- 不改 RESP / store / AOF / RDB / replication / HNSW 语义
- 不顺带处理 RDB save / AOF rewrite，只复用 snapshot 设计边界

## 2. 当前代码现状

当前 master full sync 调用链如下：

```text
distributed_init(role=master)
  → pthread_create(master_sync_server)
    → accept(slave)
      → pthread_create(slave_sync_thread, slave)
        → pthread_rwlock_rdlock(&g_kv_data_rwlock)
        → 遍历 global_array / global_rbtree / global_hash / global_skiptable
        → 对每个 key 调用 master_sync_command()
          → ms_encode_message()
          → sync_backend_broadcast_owned()
          → sync backend worker send()
        → pthread_rwlock_unlock(&g_kv_data_rwlock)
        → recv() 等待 slave 断开
```

关键点：

- full sync 在 `src/distributed/distributed.c::slave_sync_thread()` 中执行。
- 遍历线程是 per-slave sync thread，不是 command executor。
- 当前一致性完全依赖 `g_kv_data_rwlock` 读锁。
- 读锁覆盖整个遍历和 `master_sync_command()` 调用过程。

## 3. 审计结论

### 3.1 是否直接遍历 live store / vector

是，当前 full sync 直接遍历 live store，但仅限普通 KV store：

- `global_array`
- `global_rbtree`
- `global_hash`
- `global_skiptable`

当前 **不会** 遍历以下 vector live state：

- `g_vector_state.store`
- `g_vector_state.id_map`
- `g_vector_state.ann_index`
- `g_vector_state.flat_index`
- `g_vector_state.sq8_flat_index`
- scope/question 辅助 map

这意味着 Stage 10.5 的当前 blocker 是“普通 KV live traversal”；vector/HNSW 不是现有 full sync 的直接访问对象。

### 3.2 是否阻塞 normal command / TTL / replicated task

是。

`slave_sync_thread()` 在整个全量遍历期间持有 `g_kv_data_rwlock` 读锁，而以下路径都需要写锁：

- normal write command commit
- TTL active expire executor task
- lazy expire delete path
- replicated_command_task
- shutdown destroy path

因此只要 full sync 较慢：

- normal write 会被 full sync 长时间阻塞
- TTL 删除会被阻塞
- slave apply executor task 会被阻塞
- shutdown 需要等 full sync 线程退出或释放读锁后才能继续销毁

### 3.3 是否在锁内做网络发送

是。

当前代码在读锁内调用 `master_sync_command()`；虽然 `master_sync_command()` 自身只做 encode + broadcast enqueue，但 full sync 遍历期间仍然把编码与队列投递都放在 live traversal 锁区间内。锁区间明显大于“复制一致性所需的最小临界区”。

## 4. 为什么这轮不直接改生产代码

当前 executor 只支持 4 类任务：

- normal command task
- prepared command task
- replicated_command_task
- ttl_expire_task

还没有通用 snapshot task 基础设施：

- 没有 snapshot task 类型
- 没有同步等待 executor 结果的 API
- 没有 snapshot 对象定义
- 没有 snapshot 生命周期 / 错误传播 / shutdown drain 约束

如果直接在 Stage 10.5 临时拼一个“full sync 专用 task”，很容易把：

- snapshot ownership
- stop/shutdown 时的资源释放
- future RDB/AOF rewrite 复用边界

做成一次性实现，后面 Stage 10.6/10.7 又要返工。

因此本轮建议：

- 先完成 readiness audit + 设计收敛
- 补 regression test 固化 replication 语义
- 下一步在单独提交里引入可复用的 snapshot builder / snapshot task

## 5. Stage 10.5 建议设计

### 5.1 最小设计原则

只解决 master full sync blocker，不扩大语义范围：

- snapshot 只覆盖当前 full sync 已发送的普通 KV + TTL metadata
- 不把 vector/RDB/AOF rewrite 一起塞进本提交
- sync thread 仍然保留，只把“读 live store”替换为“发送 snapshot”

### 5.2 建议对象模型

建议新增两层对象：

```text
master_full_sync_snapshot_task
  → executor 内持读锁构造 snapshot
  → 锁外/线程外发送 snapshot

kvs_full_sync_snapshot_t
  → 普通 KV entry 数组
  → 每个 entry 保存:
      backend kind
      key/key_len
      value/value_len
      expire_at_ms
```

建议 entry 至少包含：

- store type: array / rbtree / hash / skiptable
- key bytes
- value bytes
- `expire_at_ms`

这样可以保持现有协议不变：

- array entry 发送 `SET`
- rbtree entry 发送 `RSET`
- hash entry 发送 `HSET`
- skiptable entry 发送 `SSET`
- 有 TTL 时继续补发 `PEXPIREAT`

### 5.3 执行边界

建议拆成两个阶段：

```text
slave_sync_thread
  → request snapshot
    → executor thread:
        rdlock(g_kv_data_rwlock)
        copy current live KV + expire_at_ms into snapshot
        unlock
  → sync thread:
        iterate snapshot
        master_sync_command(...)
        free snapshot
```

注意这里 executor 仍可能在 snapshot build 期间持读锁，但区别是：

- 持锁时间只覆盖“复制内存”
- 不覆盖网络发送 / 广播排队 / slave 存活等待
- 后续 RDB/AOF rewrite 可复用同一 snapshot builder

## 6. Snapshot Ownership

建议 ownership 明确如下：

### 6.1 创建者

- executor thread 创建 `kvs_full_sync_snapshot_t`
- executor thread 负责填充所有 entry 副本

### 6.2 交付者

- executor thread 完成后把 snapshot 指针交还给请求方（slave sync thread）

### 6.3 释放者

- 默认由 slave sync thread 在发送完成后释放
- 如果 build 失败且 snapshot 未发布，由 executor thread 立即清理

### 6.4 API 语义

建议是“成功返回已拥有 snapshot，失败返回 NULL”：

```text
int kvs_command_executor_build_full_sync_snapshot(kvs_full_sync_snapshot_t **out);
void kvs_full_sync_snapshot_destroy(kvs_full_sync_snapshot_t *snapshot);
```

成功后：

- `*out` ownership 属于调用者

失败后：

- `*out == NULL`
- 调用者无需回收半成品

## 7. Failure Path

建议失败路径保持简单且显式：

### 7.1 snapshot build 失败

可能原因：

- entry 数组分配失败
- key/value copy 失败
- executor 已停止或进入 stopping

建议行为：

- 释放已复制的所有 entry
- `slave_sync_thread()` 关闭该 slave 连接
- 不发送半成品 full sync
- 不改变 master 现有增量复制语义

### 7.2 发送失败

发送失败不应反向影响 live state：

- snapshot 保持只读对象
- sync thread 发送失败后直接销毁 snapshot
- slave 重连时重新请求新的 snapshot

### 7.3 broadcast enqueue 失败

如果 `master_sync_command()` / backend enqueue 失败：

- 视为当前 slave full sync 失败
- 关闭该 slave 连接
- 销毁 snapshot

理由：full sync 不是 best-effort 局部流，而是建立 slave 初始状态的基线。

## 8. Shutdown 顺序要求

当前 shutdown 顺序是：

```text
network loop exits
→ aof_fsync
→ kvs_rdb_save
→ kvs_ttl_background_stop
→ kvs_command_executor_stop
→ dest_kvengine
  → distributed_shutdown
  → aof_free
  → wrlock(g_kv_data_rwlock)
  → destroy stores/vector
```

对 Stage 10.5 来说，需要额外保证：

- `distributed_shutdown()` join 所有 slave sync thread 之前，slave sync thread 不再持有 live snapshot builder 的内部引用
- sync thread 如果持有已发布 snapshot，必须在退出前销毁它
- executor stop 之前不应留下“未完成但无人接收”的 snapshot 任务

因此建议未来实现满足两条规则：

1. snapshot build 是同步请求-同步返回，不引入 detached snapshot completion。
2. snapshot 一旦发布，只由请求它的 sync thread 持有并在本线程销毁。

这样 shutdown 证明最简单：

- sync thread 退出前销毁 snapshot
- `distributed_shutdown()` join 完所有 sync thread
- 之后才允许 store destroy

## 9. 复制语义保持不变的方式

Stage 10.5 应严格保持以下复制语义不变：

- full sync 仍然先发普通 KV entry
- 有 TTL 的 key 仍继续发一条 `PEXPIREAT`
- 增量复制仍由 `master_sync_command()` 生成
- slave apply 仍由 Stage 10.3 的 `replicated_command_task` 执行
- master/slave 协议编码格式不变

也就是说，变化只在“full sync 输入来源”：

```text
之前: slave_sync_thread 直接读 live store
之后: slave_sync_thread 只读 snapshot
```

## 10. 与 Stage 10.6 / 10.7 的复用边界

建议把 Stage 10.5 的 snapshot builder 设计成 future-proof，但实现先只服务 full sync：

- Stage 10.5: `full_sync_snapshot`
- Stage 10.6: `rdb_snapshot`
- Stage 10.7: `aof_rewrite_snapshot`

第一版不需要一次性统一文件格式，只需要统一：

- builder ownership
- error handling
- shutdown discipline

## 11. 本轮结论

结论如下：

- 当前 master full sync **仍直接遍历 live store**
- 当前 master full sync **不读取 vector/HNSW/id_map/scope map**
- 当前 full sync **会阻塞 normal write / TTL / replicated task**
- 当前 executor **尚无通用 snapshot_task 基础设施**
- 这轮更合理的交付是 **readiness audit + regression test**
- 下一提交建议引入 **master_full_sync_snapshot_task / snapshot builder**

## 12. 下一步建议

建议后续提交顺序：

1. 为 executor 增加同步 snapshot build API 和 snapshot destroy API
2. 将 `slave_sync_thread()` 改为“build snapshot + send snapshot”
3. 保持主从协议和 TTL 复制语义不变
4. 复用同一 ownership 约束推进 RDB save / AOF rewrite
