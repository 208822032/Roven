# Existing Lock Bypass Fix Design

## 1. 本阶段目标

阶段 0.5 的目标是在路线 C 大改前，先修复当前已知绕过 `g_kv_data_rwlock` 的数据竞争风险。该阶段是稳定性 bug fix，不是性能优化，也不是 executor 重构。

## 2. 当前代码现状

已知绕锁路径：

- `src/distributed/distributed.c::slave_sync_thread` 作为 master full sync 线程，直接遍历 `global_array/global_rbtree/global_hash/global_skiptable`。
- `src/distributed/distributed.c::slave_connect_master` 作为 slave apply 线程，直接调用底层 `kvs_array_set/kvs_hash_set/...`。
- `src/persistence/kvs_rdb.c::kvs_rdb_save` 函数内部直接遍历普通 KV 和 vector section，函数内部不持 `g_kv_data_rwlock`。
- `src/persistence/kvs_aof_rewrite.c` 有 `aof_state.data_rwlock`，但该锁不是全局 store 锁，不能完整保护普通 KV 和 vector live store。

## 3. 设计方案

最小修复建议：

1. master full sync 遍历 store 时应持有 `g_kv_data_rwlock` 读锁。
2. slave apply 每条 set/del/mod/TTL apply 应持有 `g_kv_data_rwlock` 写锁，或者统一改为调用 `kvs_filter_protocol()` 路径。
3. RDB save 应由调用方明确持读锁，或在 `kvs_rdb_save()` 内部持读锁。需要避免 shutdown 已持锁时递归死锁。
4. AOF rewrite 如果读取 live store，应持有全局读锁或改为在持锁下复制 snapshot，再锁外写文件。

为什么这是 bug fix：

- 当前这些路径已经可能和网络 handler、TTL 后台线程并发访问同一 store。
- 修复不改变命令语义，不引入新架构。
- 修复后路线 C 后续性能数据更可信。

## 4. 不修改范围

阶段 0.5 不做：

- 不引入 executor。
- 不引入 command_task。
- 不引入 response_task。
- 不改 RESP。
- 不删除 `g_kv_data_rwlock`。
- 不改 vector/HNSW 主链路。

## 5. 涉及模块和文件

- `src/distributed/distributed.c`
- `src/persistence/kvs_rdb.c`
- `src/persistence/kvs_aof_rewrite.c`
- `src/core/kvstore.c`
- `include/kvs_globals.h`

## 6. 正确性边界

修复后应满足：

- full sync 遍历期间不会和写命令并发释放节点。
- slave apply 不会和网络 handler/TTL scan 并发修改 store。
- RDB save/AOF rewrite 不会遍历被并发修改的 live store。

## 7. 性能影响预期

这是稳定性修复，可能轻微增加 full sync、RDB save、AOF rewrite 与写命令之间的阻塞。该影响可接受，因为这些路径不是主请求热路径。

## 8. 风险点

- RDB save 如果在已有写锁上下文中调用，内部再加读锁可能死锁。
- AOF rewrite 持读锁时间过长会阻塞写命令。
- slave apply 每条命令加锁可能降低同步吞吐。

规避：

- 明确锁由调用方还是函数内部持有，避免双重加锁。
- AOF rewrite 第一版可复制小 snapshot 后锁外写。
- slave apply 可以先用粗粒度写锁验证正确性，再优化。

## 9. 验证方式

- 主从 full sync 同时持续写入。
- slave apply 同时本地读命令。
- RDB save 同时 SET/DEL/EXPIRE。
- AOF rewrite 同时写入。
- TTL 大量过期 key 同时 full sync。
- 长时间压测确认无崩溃、无脏读、无段错误。

## 10. 回滚方式

每个修复点独立提交。若某个路径出现死锁或性能严重退化，回滚该路径的加锁改动，并保留其他路径修复。

## 11. 下一阶段前置条件

阶段 0.5 完成后，再做阶段 1 的 VSET/VSEARCH embedding 锁外 prepare。否则后续性能测试可能混入已有数据竞争导致的不稳定结果。

## 实现状态：master full sync

- 当前状态：已通过 `g_kv_data_rwlock` 读锁保护 live store 遍历。
- 锁范围：覆盖 `global_array/global_rbtree/global_hash/global_skiptable` 遍历。
- 已知限制：当前实现边遍历边发送，读锁持有时间可能较长，会阻塞写命令。
- 后续路线 C：改为 executor `snapshot_task`，sync thread 只发送快照。

## 实现状态：slave apply

- 当前状态：已在 slave apply 修改 live store 时通过 `g_kv_data_rwlock` 写锁保护。
- 锁范围：覆盖 replicated set/del/mod 以及同一复制消息内的 TTL/expire_at 修改。
- 已知限制：当前仍是阶段 0.5 的临时锁修复，长期路线 C 会改为 `replicated_command_task` 进入 single commit executor。
- 边界：socket recv 和协议解析在锁外完成，不在写锁内阻塞网络 IO。
