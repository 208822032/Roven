# Background Tasks Executor Design

## 1. 本阶段目标

阶段 5 的目标是把所有后台 store 访问路径 executor 化。这是删除或弱化 `g_kv_data_rwlock` 前的硬前置。

## 2. 当前代码现状

当前后台路径：

- TTL active expire：`src/core/kvs_ttl_background.c` 线程直接拿写锁扫描并删除。
- AOF append：handler 后同步调用 `aof_append()`，AOF IO thread 刷盘。
- AOF rewrite：`src/persistence/kvs_aof_rewrite.c` 遍历 live store。
- RDB save：`src/persistence/kvs_rdb.c::kvs_rdb_save` 遍历 live store。
- master full sync：`src/distributed/distributed.c::slave_sync_thread` 遍历 live store。
- slave apply：`slave_connect_master` 直接修改底层 store。
- shutdown：停止部分后台线程后销毁 store。

## 3. 设计方案

### TTL

- timer thread 不再直接访问 store。
- timer thread 只投递 `expire_scan_task`。
- executor 内执行 active expire。
- lazy expire 继续在 executor 内完成。

### AOF

- AOF append 由 executor 在 commit 成功后按 commit 顺序生成 AOF record。
- AOF IO thread 只写盘，不读 store。
- AOF rewrite 通过 `snapshot_task`，不直接遍历 live store。

### RDB

- RDB save 通过 `snapshot_task`。
- RDB load 启动期可以保留单线程，因为网络未启动。
- 后续如果支持在线 replay，可封装成 `replay_task`。

### 主从同步

- master 增量复制事件由 executor commit 后生成。
- slave apply 封装为 `replicated_command_task`，由 executor 串行 apply。
- full sync 封装为 `snapshot_task`，sync thread 只负责发送 snapshot。

### shutdown

```text
stop accept
drain prepare queue
drain command queue
flush AOF
save snapshot if needed
destroy store
stop executor
```

## 4. 不修改范围

阶段 5 不做：

- 不做多 poller。
- 不做 embedding worker pool。
- 不删除锁。
- 不改变 AOF/RDB 文件格式。
- 不改变主从协议。

## 5. 涉及模块和文件

- `src/core/kvs_ttl_background.c`
- `src/persistence/kvs_aof.c`
- `src/persistence/kvs_aof_rewrite.c`
- `src/persistence/kvs_rdb.c`
- `src/distributed/distributed.c`
- `src/core/kvstore.c`

## 6. 正确性边界

- 所有修改 store 的后台任务必须进入 executor。
- 所有读取 live store 的后台任务必须通过 executor 或 snapshot。
- AOF 和 replication 顺序必须等于 commit 顺序。
- RDB/AOF snapshot 必须对应某个明确状态点。

## 7. 性能影响预期

收益：

- 消除绕锁路径。
- 为最终去锁建立状态所有权。

成本：

- TTL scan、snapshot、replication 可能占用 executor 时间。
- 需要控制 snapshot 粒度，避免长时间阻塞 commit。

## 8. 风险点

- snapshot 过大导致 executor pause。
- AOF append 从 handler 后置到 executor 后，错误处理路径变化。
- full sync snapshot 内存占用高。
- slave apply 走 executor 后复制延迟上升。

## 9. 验证方式

- 大量过期 key active expire。
- AOF append/rewrite/load 同时写入。
- RDB save/load 同时写入。
- master full sync 同时写入。
- slave apply 同时本地读。
- shutdown drain。

## 10. 回滚方式

每类后台任务单独迁移、单独提交。若某类 executor 化失败，可回滚该类任务，其余已稳定任务保留。

## 11. 下一阶段前置条件

进入多 poller 或删除锁前，必须确认后台任务全部不再绕过 executor 访问 live store。
