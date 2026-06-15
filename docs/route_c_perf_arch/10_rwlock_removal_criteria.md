# RWLock Removal Criteria

## 1. 本阶段目标

定义删除或弱化 `g_kv_data_rwlock` 的硬性判定标准。

## 2. 当前代码现状

当前 `g_kv_data_rwlock` 是多个并发上下文访问 store/vector/HNSW/TTL 的主要保护手段。部分路径当前还绕过该锁，因此不能直接删除。

## 3. 设计方案

硬条件：

- 普通 KV handler 不再被网络线程直接调用。
- Vector handler 不再被网络线程直接调用。
- TTL active expire 不再直接访问 store。
- lazy expire 只在 executor 内完成。
- RDB save 不直接遍历 live store。
- AOF rewrite 不直接遍历 live store。
- master full sync 不直接遍历 live store。
- AOF append 顺序由 executor 产生。
- slave apply 通过 `replicated_command_task`。
- shutdown/destroy 在 executor drain 后执行。
- vector store / id_map / HNSW 所有访问有明确 owner。
- session 生命周期安全。
- 普通 KV / TTL / V族 / 持久化 / 主从同步测试覆盖。
- 长时间压测通过。
- TSAN 或等价竞态检测通过。

原则：

```text
删除锁是最后一步，不是优化起点。
```

## 4. 不修改范围

本阶段不实现删除锁，不修改源码，只定义判定标准和验收门槛。

## 5. 涉及模块和文件

- `src/core/kvstore.c`
- `src/core/kvs_operation.c`
- `src/core/kvs_ttl_background.c`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_index.c`
- `src/persistence/*`
- `src/distributed/distributed.c`
- network/executor/session 模块

## 6. 正确性边界

只有当所有 live state 访问都能归属到 executor 或明确 snapshot owner，才能移除全局读写锁。

## 7. 性能影响预期

删除锁本身不是主要性能收益来源。真正收益来自：

- 网络不被 handler 阻塞。
- embedding 并发 prepare。
- 状态提交无需读写锁开销。

## 8. 风险点

- 隐性绕过 executor 的路径残留。
- RDB/AOF/replication snapshot 不一致。
- vector HNSW search/insert 并发访问。
- shutdown 时未 drain 完 task。

## 9. 验证方式

- 全量功能测试。
- RDB/AOF/主从同步混合压测。
- TTL active/lazy expire 压测。
- VSET/VDEL/VSEARCH 混合压测。
- 长时间 c4/c8/c16。
- TSAN 或同等竞态检测。

## 10. 回滚方式

保留编译开关或配置开关让 executor 内继续持有 `g_kv_data_rwlock`。发现竞态后立即恢复锁路径。

## 11. 下一阶段前置条件

进入本阶段前必须完成阶段 5 后台任务 executor 化，并证明所有 store/HNSW 访问路径已经收敛。
