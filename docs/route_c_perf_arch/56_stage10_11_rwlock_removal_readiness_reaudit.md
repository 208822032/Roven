# Stage 10.11 rwlock removal readiness re-audit

## 当前结论

**CONDITIONALLY_READY** for the reactor/proactor normal async command path.

`g_kv_data_rwlock` 仍然必须保留。当前主线已经把多数 store/vector/TTL/replication/snapshot 运行期访问收敛到 single command executor，但仍存在明确的 legacy synchronous paths：ntyco 同步模式、reactor/proactor eBPF REPLSYNC/is_replication fallback。这些路径绕过 executor，删除或弱化 `g_kv_data_rwlock` 仍不安全。

本阶段不删除、不弱化 `g_kv_data_rwlock`。

## g_kv_data_rwlock 调用点分类

### A. 已收敛到 executor-owned，当前不是 blocker

- Reactor normal client command:
  `reactor_submit_async_command()` 构造 `kvs_command_task_t`，调用 `kvs_command_executor_submit(task)`。
- Proactor normal client command:
  `proactor_submit_async_command()` 构造 `kvs_command_task_t`，调用 `kvs_command_executor_submit(task)`。
- Embedding prepare:
  worker 只执行 prepare，prepared task 回到 single command executor 后 commit/search/finalize。
- TCP slave apply:
  `slave_connect_master()` 解码复制消息后调用 `kvs_command_executor_submit_replicated(...)`。
- Master full sync:
  `slave_sync_thread()` 调用 `kvs_command_executor_build_full_sync_snapshot(...)`。
- RDB save:
  `kvs_rdb_save()` 调用 `kvs_command_executor_build_rdb_snapshot(...)`。
- AOF rewrite:
  rewrite 线程调用 `kvs_command_executor_build_aof_rewrite_snapshot(...)`。
- TTL active scanner:
  `kvs_ttl_background.c` 只 submit `ttl_scan_task`；scan/read/check/delete 在 executor 内完成。

### B. lock-retained-but-owner-safe

这些路径仍持有 `g_kv_data_rwlock`，但访问发生在 executor 内，当前不再是 owner blocker：

- Normal command commit/search:
  `kvs_command_task_execute_sync()` 中按 command lock mode 获取 read/write lock，并支持 lazy expire read-to-write retry。
- Replicated command task:
  `kvs_command_executor_execute_repl()` 持 write lock apply store/TTL。
- Legacy TTL expire task:
  `kvs_command_executor_execute_ttl()` 持 write lock 调用 `ttl_expire_observed_key()`。
- TTL scan task:
  `kvs_command_executor_execute_ttl_scan()` 持 write lock 扫描 TTL metadata 并 re-check/delete。
- Full sync snapshot:
  `kvs_command_executor_execute_full_sync_snapshot()` / current-thread fast path 持 read lock 构造 snapshot。
- RDB snapshot:
  `kvs_command_executor_execute_rdb_snapshot()` / current-thread fast path 持 read lock 构造 snapshot。
- AOF rewrite snapshot:
  `kvs_command_executor_execute_aof_snapshot()` / current-thread fast path 持 read lock 构造 snapshot。

这些 lock 点是未来弱化锁时需要替换的 mechanical sites，但不代表仍绕过 executor。

### C. active blocker

仍绕过 executor 的路径：

1. ntyco legacy sync mode
   - `main()` 明确日志：
     `network_mode=ntyco runs in legacy sync mode. Commands bypass command executor. rwlock removal is NOT supported in this mode.`
   - ntyco 仍直接以 `kvs_filter_protocol` 为 handler，同步执行命令。
   - 结论：若保留 ntyco legacy mode，则 ntyco 必须永久依赖 `g_kv_data_rwlock`，或者明确从 rwlock removal 支持范围排除。

2. Reactor eBPF REPLSYNC/is_replication fallback
   - `reactor_submit_async_command()` 在 `ext->is_replication || reactor_command_is_replsync(session)` 分支直接调用 `net_session_process_resp(...)`。
   - `reactor_session_handler()` 对 REPLSYNC 标记 `ext->is_replication = 1`，设置 `sync_context=2`，并明确日志标记 legacy synchronous replication path。
   - 结论：该路径仍通过同步 `kvs_filter_protocol` 执行完整命令语义，绕过 executor。

3. Proactor eBPF REPLSYNC/is_replication fallback
   - `proactor_submit_async_command()` 在 `ext->is_replication || proactor_command_is_replsync(session)` 分支直接调用 `p_process_received_data(ext)`。
   - `p_process_received_data()` 调用 `net_session_process_resp(...)` 和 `proactor_session_handler()`。
   - `proactor_session_handler()` 对 REPLSYNC 标记 `ext->is_replication = 1`，设置 `sync_context=2`，并明确日志标记 legacy synchronous replication path。
   - 结论：该路径仍通过同步 `kvs_filter_protocol` 执行完整命令语义，绕过 executor。

## 已经不再是 blocker 的路径

- TTL scanner 不再直接访问 live store / TTL metadata。
- TTL scan task 已由 executor 消费。
- TCP slave apply 已走 `replicated_command_task`。
- Master full sync 已走 executor snapshot。
- RDB save 已走 executor snapshot。
- AOF rewrite 已走 executor snapshot。
- Proactor normal async build/submit failure 已 fail-fast，不再 fallback 到 `p_process_received_data()`。
- Shutdown owner boundary 已收敛到：stop TTL scanner -> final fsync/save -> stop executor -> destroy。

## shutdown/stop/destroy 边界

当前 shutdown 顺序：

1. network loop 返回后进入 shutdown。
2. `kvs_ttl_background_stop()` 停止 TTL trigger thread。
3. final `aof_fsync(1)`。
4. final `kvs_rdb_save(...)`，通过 executor snapshot。
5. `kvs_command_executor_stop()` 停止 worker pool、join executor、drain queues。
6. `dest_kvengine()` 调用 `distributed_shutdown()` / `aof_free()`，随后持 write lock destroy store/vector。
7. `pthread_rwlock_destroy(&g_kv_data_rwlock)`。

这是 shutdown/stop/destroy 边界增强项，不是当前 rwlock removal blocker。未来可增强项：

- 为所有 snapshot task 增加更明确的 stop-time error propagation 日志。
- 将 destroy 阶段的 write lock 替换为 executor-stopped invariant assert。
- 清理旧文档中“TTL scanner 直接读 live store”的过期描述，避免误判。

## readiness 判定

- 全局项目：**NOT_READY** for unconditional rwlock removal。
- reactor/proactor normal async command path：**CONDITIONALLY_READY**，前提是排除 ntyco 和 eBPF REPLSYNC/is_replication fallback。
- ntyco：**NOT_READY / legacy no-rwlock-removal mode**。
- eBPF replication fallback：**NOT_READY**，仍是 active blocker。

## 下一阶段建议

1. Stage 10.12: eBPF REPLSYNC/is_replication fallback policy
   - 选择 executor 化 replication fallback，或明确禁用/隔离该模式。
2. Stage 10.13: ntyco no-rwlock-removal policy hardening
   - 配置/文档/启动日志上明确 ntyco 不参与 rwlock removal。
3. Stage 10.14: executor-only build profile audit
   - 在 reactor/proactor + TCP replication + snapshots + TTL scanner 组合下，审计是否可以引入 compile/runtime guard，只在 owner-safe profile 中弱化锁。
4. Stage 10.15: lock-retained-but-owner-safe mechanical replacement plan
   - 将 executor 内 lock sites 改为 owner assertions 或 no-op guard，但仅在 active blocker 关闭之后执行。
