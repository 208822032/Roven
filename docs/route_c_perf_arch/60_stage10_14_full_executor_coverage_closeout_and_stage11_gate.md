# Stage 10.14 Full Executor Coverage Closeout and Stage 11 Gate

## 1. Overall Conclusion

**FULL_EXECUTOR_COVERAGE_READY**.

Stage 10.14 re-audited Route C Full Executor Coverage after Stage 10.12 eBPF replication fallback convergence and Stage 10.13 ntyco executor convergence. The production paths no longer have network, background, persistence, or replication threads directly executing shared state commands outside the single command executor.

`g_kv_data_rwlock` remains present and keeps its real pthread rwlock semantics. This stage does not remove, weaken, or no-op the lock.

## 2. Path Classification

| Path | Shared state access | Executor-owned | Bypasses executor | Still needs rwlock | Blocks Stage 11 | Evidence |
|---|---:|---:|---:|---:|---:|---|
| Reactor normal command | Yes | Yes | No | Yes, executor lock sites | No | `reactor_submit_command_task()` -> `kvs_command_executor_submit()` |
| Proactor normal command | Yes | Yes | No | Yes, executor lock sites | No | `proactor_submit_command_task()` -> `kvs_command_executor_submit()` |
| Ntyco normal command | Yes | Yes | No | Yes, executor lock sites | No | `ntyco_submit_command_task()` -> `kvs_command_executor_submit()` |
| eBPF REPLSYNC | No shared state | Protocol-only | No shared state bypass | No for REPLSYNC itself | No | `*_handle_replication_fallback()` inline role check/OK |
| eBPF is_replication apply | Yes | Yes | No | Yes, executor replicated task | No | `kvs_command_executor_submit_replicated()` |
| TCP slave apply | Yes | Yes | No | Yes, executor replicated task | No | `slave_connect_master()` -> `submit_replicated()` |
| Master full sync | Snapshot read | Yes | No | Yes, executor snapshot read lock | No | `kvs_command_executor_build_full_sync_snapshot()` |
| RDB save | Snapshot read | Yes | No | Yes, executor snapshot read lock | No | `kvs_command_executor_build_rdb_snapshot()` |
| AOF rewrite | Snapshot read | Yes | No | Yes, executor snapshot read lock | No | `kvs_command_executor_build_aof_rewrite_snapshot()` |
| TTL active expire | Mutation | Yes | No | Yes, executor write lock | No | `kvs_command_executor_execute_ttl()` |
| TTL scanner runtime | Scan + mutation | Yes | No | Yes, executor write lock | No | `kvs_command_executor_submit_ttl_scan()` -> `execute_ttl_scan()` |
| Lazy expire | Read-to-write mutation | Yes | No | Yes, command task lock retry | No | `kvs_command_task_execute_sync()` under executor task |
| Vector/HNSW VSET/VSEARCH/VDEL/VINDEX | Read/write/search/index | Yes | No | Yes, command task lock sites | No | command task prepare/execute/finalize path |
| Proactor async failure | No shared state | Fail-fast | No sync fallback | N/A | No | `proactor_fail_async_command()` |
| Shutdown final AOF/RDB/executor stop/destroy | Snapshot + destroy | Ordered boundary | No concurrent task bypass | Yes for destroy boundary | No | `main()` shutdown order, executor stop drain/drop |
| Executor internal lock sites | Yes | Yes | No | Yes | No | `kvs_command_task.c`, `kvs_command_executor.c` |

## 3. Executor-Owned Paths

The following are executor-owned:

- Reactor/proactor/ntyco normal commands.
- eBPF replicated apply and TCP slave apply.
- TTL active expire and TTL scanner runtime.
- Master full sync, RDB save, AOF rewrite snapshots.
- Lazy expire and vector/HNSW command execution.
- AOF append and replication post-commit from command task flow.

## 4. Non Executor-Owned Paths

No production shared-state read/write/snapshot/mutation path was found outside the executor or init/destroy boundary.

Historical documents and `src/network/ntyco_defult.c` still describe old synchronous paths, but `ntyco_defult.c` is not compiled. Tests focus on production `src/network/ntyco.c`.

## 5. Lock-Retained-But-Owner-Safe Sites

Remaining `g_kv_data_rwlock` sites are retained and owner-safe:

- `src/core/kvs_command_task.c`: command read/write lock and lazy-expire retry.
- `src/core/kvs_command_executor.c`: replicated task, TTL expire task, TTL scan task, full sync/RDB/AOF snapshot tasks.
- `src/core/kvstore.c`: init/destroy boundary and final `pthread_rwlock_destroy()`.

These sites still use real pthread rwlocks. They are candidates for Stage 11 mechanical wrapper replacement, not lock removal.

## 6. Shutdown Boundary

Shutdown remains safe for Stage 11 entry:

```text
network loop exits
  -> kvs_ttl_background_stop()
  -> final aof_fsync(1)
  -> final kvs_rdb_save() via executor snapshot
  -> kvs_command_executor_stop() drains/drops queues and joins executor
  -> dest_kvengine() destroys distributed/vector/store under retained lock boundary
  -> pthread_rwlock_destroy(&g_kv_data_rwlock)
```

Executor stop drains command, prepared, replicated, TTL expire, TTL scan, full sync snapshot, RDB snapshot, AOF snapshot, and response queues.

## 7. Stage 11.0 Gate

Stage 11.0 entry is allowed.

Reasons:

- No network thread directly accesses shared state.
- No background thread directly scans or mutates live store.
- No persistence/replication thread directly traverses live store.
- Remaining lock sites are inside executor-owned execution or init/destroy/shutdown boundary.
- `g_kv_data_rwlock` is still present and still real.

## 8. Stage 11.0 Scope Entered

This stage also introduces Stage 11.0 foundation:

- `include/kvs_owner_lock.h`
- `src/core/kvs_owner_lock.c`
- executor owner thread id recording via `kvs_owner_lock_set_executor_thread()`
- wrapper API:
  - `kvs_owner_rwlock_rdlock()`
  - `kvs_owner_rwlock_wrlock()`
  - `kvs_owner_rwlock_unlock()`

Default wrapper implementation still calls real:

- `pthread_rwlock_rdlock`
- `pthread_rwlock_wrlock`
- `pthread_rwlock_unlock`

This is not lock deletion or lock weakening.

## 9. Wrapper Replacement Scope

Only one executor-internal demonstration site was mechanically replaced:

- `kvs_command_executor_execute_ttl_scan()`
  - `pthread_rwlock_wrlock(&g_kv_data_rwlock)` -> `kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "executor.ttl_scan")`
  - `pthread_rwlock_unlock(&g_kv_data_rwlock)` -> `kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.ttl_scan")`

The wrapper still calls the real pthread lock/unlock, so lock granularity and behavior are unchanged.

## 10. Remaining Risk

- Some historical docs still describe earlier blockers; they are not production code evidence.
- Ntyco now uses a conservative wait-for-executor-response model, not a high-performance coroutine wake adapter.
- AOF has its own `aof_state.data_rwlock`; it is separate from `g_kv_data_rwlock` and remains unchanged.

## 11. Next Stage

Stage 11.1 should mechanically replace remaining executor-internal `g_kv_data_rwlock` call sites with wrappers and add owner assertions/logging policy. It still should not no-op or remove the lock until a later explicit lock weakening/removal stage.
