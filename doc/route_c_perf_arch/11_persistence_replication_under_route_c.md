# Persistence And Replication Under Route C

## 1. 本阶段目标

定义路线 C 下 AOF、RDB、主从同步如何纳入同一条 commit 状态流。

## 2. 当前代码现状

当前持久化和复制分散在多个路径：

- `kvs_filter_protocol()` handler 成功后调用 `aof_append()` 和 `master_sync_command()`。
- AOF IO thread 只负责刷盘，但 AOF rewrite 会遍历 live store。
- RDB save 遍历 live store。
- master full sync 线程遍历 live store。
- slave apply 线程直接修改底层 store。

## 3. 设计方案

### AOF append

- commit executor 执行命令成功后生成 AOF record。
- AOF 顺序等于 commit 顺序。
- AOF IO thread 只负责写盘和 fsync，不读 store。

### AOF rewrite

- 使用 `snapshot_task`。
- 第一版建议 executor 暂停短时间生成一致性快照，再由 IO/rewrite 线程写临时文件。
- 不建议第一版继续依赖 fork/COW 解释在线一致性。

### RDB save/load

- RDB save 通过 `snapshot_task`。
- RDB load 启动期单线程可保留，因为网络尚未启动。
- 后续如需在线 replay，可封装为 `replay_task`。

### Master full sync

- 不允许 sync thread 直接遍历 live store。
- executor 执行 snapshot_task 生成普通 KV 和 vector snapshot。
- sync thread 只负责发送 snapshot。

### Slave apply

- 不允许直接调用底层 store set/del/mod。
- 解码复制消息后封装为 `replicated_command_task`。
- executor 串行 apply。

### TTL 复制

- `SET` / `EXPIRE` / `PERSIST` / `PEXPIREAT` 顺序必须跟 commit 顺序一致。
- 过期删除产生的 `DEL` 也应由 executor 生成。

## 4. 不修改范围

本设计不要求立即修改：

- AOF/RDB 文件格式。
- 主从同步协议。
- RESP 协议。
- vector RDB section 格式。

## 5. 涉及模块和文件

- `src/persistence/kvs_aof.c`
- `src/persistence/kvs_aof_rewrite.c`
- `src/persistence/kvs_rdb.c`
- `src/distributed/distributed.c`
- `src/core/kvstore.c`
- `src/core/kvs_operation.c`

## 6. 正确性边界

- 持久化顺序必须等于 commit 顺序。
- 复制顺序必须等于 commit 顺序。
- snapshot 必须对应明确状态点。
- slave apply 不得绕过 executor。

## 7. 性能影响预期

- AOF append 从 handler 后置到 executor 后，日志顺序更清晰。
- snapshot_task 可能造成短暂 commit pause。
- full sync 从 live 遍历变 snapshot 后，运行期数据竞争风险降低。

## 8. 风险点

- snapshot 内存占用高。
- AOF rewrite 长时间占用 executor。
- slave apply 通过 executor 后复制延迟升高。
- TTL 复制顺序错误导致主从不一致。

## 9. 验证方式

- AOF append/rewrite/load 后数据一致。
- RDB save/load 后数据一致。
- master full sync 期间持续写入，slave 最终一致。
- TTL 过期和 PERSIST/PEXPIREAT 主从一致。
- vector RDB 恢复后 VSEARCH 语义一致。

## 10. 回滚方式

逐模块迁移。AOF、RDB、full sync、slave apply 各自独立提交，出问题只回滚对应模块。

## 11. 下一阶段前置条件

持久化和主从同步必须纳入 executor 状态流后，才能考虑删除或弱化 `g_kv_data_rwlock`。

结论：

```text
持久化和主从同步不是附属模块，必须纳入同一条命令状态流。
```
