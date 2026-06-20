# Stage 10.6: RDB Save Snapshot Task

## 1. 目标

把 RDB save 从：

- `kvs_rdb_save()` 外层持 `g_kv_data_rwlock` 读锁
- 直接遍历 live store 和 `g_vector_state.store`
- 在读锁内执行文件 IO

改为：

- 调用方提交 `rdb_snapshot_task`
- executor 持 `g_kv_data_rwlock` 读锁构造 RDB snapshot
- `kvs_rdb_save()` 只负责把 snapshot 写到文件
- 写完后由保存调用方释放 snapshot

这一步不改变：

- RDB 文件格式
- RDB load 逻辑
- RESP/store/replication/HNSW 语义

## 2. 当前实现

### 2.1 新增 snapshot API

在 `include/kvs_command_executor.h` 新增：

- `kvs_rdb_snapshot_kv_entry_t`
- `kvs_rdb_snapshot_vector_record_t`
- `kvs_rdb_snapshot_vector_section_t`
- `kvs_rdb_snapshot_t`
- `kvs_command_executor_build_rdb_snapshot()`
- `kvs_rdb_snapshot_destroy()`

snapshot 包含：

- 普通 KV：array / rbtree / hash / skiptable
- vector section metadata：`initialized/index_type/metric_type/next_id`
- vector records：按现有 RDB vector section 需要的字段逐条复制

不包含：

- HNSW graph 内部结构
- flat / sq8 index 内部缓存
- id_map / scope map

这些内容本来就不写入 RDB，load 后仍走现有 rebuild 逻辑。

### 2.2 executor 内新增 rdb snapshot task

`src/core/kvs_command_executor.c` 新增：

- `kvs_rdb_snapshot_task_t`
- `rdb_snapshot_head/rdb_snapshot_tail`
- `kvs_command_executor_execute_rdb_snapshot()`

流程与 Stage 10.5A full sync snapshot 一致：

```text
kvs_rdb_save()
  → kvs_command_executor_build_rdb_snapshot(&snapshot)
    → enqueue task
    → wait

executor thread
  → pop rdb snapshot task
  → pthread_rwlock_rdlock(&g_kv_data_rwlock)
  → count ordinary KV + vector records
  → allocate snapshot
  → copy ordinary KV payload
  → copy vector payload
  → pthread_rwlock_unlock(&g_kv_data_rwlock)
  → signal waiter
```

## 3. save 侧改造

`src/persistence/kvs_rdb.c` 现在不再：

- 直接遍历 `global_array/global_rbtree/global_hash/global_skiptable`
- 直接遍历 `g_vector_state.store`
- 在 save 期间外层持 `g_kv_data_rwlock`

而是改为：

```text
kvs_rdb_save(filename)
  → build snapshot via executor
  → kvs_rdb_save_snapshot_file(filename, snapshot)
  → kvs_rdb_snapshot_destroy(snapshot)
```

序列化函数改为 snapshot writer：

- `rdb_save_array_snapshot()`
- `rdb_save_rbtree_snapshot()`
- `rdb_save_hash_snapshot()`
- `rdb_save_skiptable_snapshot()`
- `rdb_save_vector_section_snapshot()`

这些函数保持原有 section 顺序和字段布局，所以 RDB 文件格式不变。

## 4. Ownership

ownership 明确如下：

- snapshot task：调用线程分配、等待、销毁
- snapshot：executor build 成功后转交给 `kvs_rdb_save()`
- snapshot 子对象：由 `kvs_rdb_snapshot_destroy()` 统一释放

因此 save 线程不再引用 live store node 指针。

## 5. Failure Path

### 5.1 build 失败

如果：

- executor 未运行
- task 入队失败
- 读锁获取失败
- snapshot 分配/复制失败

则：

- 返回 `RDB_ERR`
- 不写 tmp 文件
- 不发布半成品 snapshot

### 5.2 写文件失败

如果：

- tmp 文件打开失败
- section write 失败
- `fflush/fsync/rename` 失败

则：

- 删除 tmp 文件
- 销毁 snapshot
- 返回 `RDB_ERR`

### 5.3 shutdown 期间

当前 shutdown 顺序仍是：

```text
kvs_rdb_save()
→ kvs_ttl_background_stop()
→ kvs_command_executor_stop()
→ dest_kvengine()
```

因此正常关机路径里：

- RDB snapshot build 发生在 executor stop 之前
- save writer 在 executor stop 之前拿到自己的私有 snapshot
- 后续就算 executor 停止，writer 也只在锁外写私有内存

若队列中仍有未处理 `rdb_snapshot_task`，`kvs_command_executor_stop()` 会把它们标记失败并唤醒 waiter，不会永久阻塞。

## 6. 语义保持

本轮保持不变：

- RDB magic/version/section layout
- 普通 KV TTL 持久化规则
- vector section 字段布局
- RDB load / rebuild 逻辑

变化仅在数据来源：

```text
之前: live traversal + lock-held IO
现在: executor snapshot + lock-free IO
```

## 7. 结果

Stage 10.6 完成后：

- RDB save 不再直接遍历 live store/vector
- RDB save 不再在 `g_kv_data_rwlock` 读锁内执行文件 IO
- ordinary KV/vector 的 snapshot ownership、failure path、shutdown path 已明确
- 路线 C 剩余 blocker 缩减为：
  - AOF rewrite
  - reactor/proactor replication fallback
  - proactor OOM fallback
