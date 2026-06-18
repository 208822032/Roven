# Stage 11: rwlock wrapper takeover and weakening gate

## Goal

This stage completes the Route C / Stage 11 `g_kv_data_rwlock` controllability loop. It is not unconditional lock deletion, 不是无条件删除锁. It moves executor-owned `g_kv_data_rwlock` sites from naked `pthread_rwlock_*` calls into owner-aware wrappers, keeps the default real pthread rwlock behavior, and adds an explicit rollback-safe weak-lock gate.

Stage 10.14 closed Full Executor Coverage as `FULL_EXECUTOR_COVERAGE_READY`. Stage 11.0 introduced the wrapper primitives. This stage makes the wrapper the production owner for executor-owned sites.

## Non-goals

- Do not delete `g_kv_data_rwlock`.
- Do not delete `pthread_rwlock_destroy(&g_kv_data_rwlock)`.
- Do not weaken init, destroy, or shutdown boundaries by default.
- Do not touch `aof_state.data_rwlock`, session locks, queue mutexes, condition variables, or NtyCo internals.
- Do not introduce multiple executors, response reorder, commit ordering changes, or replication/RDB/AOF/full-sync semantic changes.

## Lock Site Audit

### A. Executor-owned sites: wrapper takeover

These sites run on the single command executor owner thread and now call `kvs_owner_rwlock_*`:

| Site | Path | Wrapper site string |
| --- | --- | --- |
| Normal command read | `kvs_command_task_lock()` | `command_task.execute.read` |
| Normal command write | `kvs_command_task_lock()` | `command_task.execute.write` |
| Lazy-expire read to write retry | `kvs_command_task_execute_sync()` | `command_task.lazy_expire.retry` |
| Command task destroy safety unlock | `kvs_command_task_destroy()` | `command_task.destroy` |
| Replicated task apply | `kvs_command_executor_execute_repl()` | `executor.replicated.write` |
| TTL expire task | `kvs_command_executor_execute_ttl()` | `executor.ttl_expire.write` |
| TTL scan task | `kvs_command_executor_execute_ttl_scan()` | `executor.ttl_scan.write` |
| Master full sync snapshot | executor snapshot task and executor reentrant build | `executor.full_sync_snapshot.read` |
| RDB snapshot | executor snapshot task and executor reentrant build | `executor.rdb_snapshot.read` |
| AOF rewrite snapshot | executor snapshot task and executor reentrant build | `executor.aof_rewrite_snapshot.read` |
| Vector VSET/VSEARCH/VDEL/VINDEX | normal command task read/write path | `command_task.execute.read` / `command_task.execute.write` |

The vector paths are covered by the command task wrapper because vector commit/search is executed by `kvs_command_task_execute_sync()` inside the executor. Embedding preparation remains outside the data lock as before.

### B. Init, destroy, shutdown boundaries: real pthread lock retained

These are intentionally not weak-lock candidates:

- `pthread_rwlock_init(&g_kv_data_rwlock, NULL)` in startup.
- `dest_kvengine()` keeps naked `pthread_rwlock_wrlock(&g_kv_data_rwlock)` and `pthread_rwlock_unlock(&g_kv_data_rwlock)`.
- `pthread_rwlock_destroy(&g_kv_data_rwlock)` remains in startup failure and final shutdown.

Reason: these are lifecycle boundaries, not concurrent command commit sites. Final destroy happens after background shutdown, final save, executor stop/drain, and before process exit. Keeping the real lock at destroy also preserves rollback safety.

### C. Non-target internal locks

No replacement was made for:

- `aof_state.data_rwlock`.
- session locks and queue mutexes.
- condition variables.
- NtyCo internal synchronization.

These locks protect different ownership domains and are outside this stage.

### D. Documentation and tests

Older Stage 10 tests that asserted naked `pthread_rwlock_*(&g_kv_data_rwlock)` were updated to assert retained lock semantics through `kvs_owner_rwlock_*`.

### E. Executor bypass blockers

No new E-class production path was found in this stage. Normal reactor/proactor/ntyco command paths submit command tasks. eBPF/TCP replicated apply submits replicated tasks. TTL scan, TTL expire, full sync, RDB snapshot, and AOF rewrite snapshot route through executor-owned tasks. Therefore this stage implements the explicit no-op candidate gate rather than DRY_RUN-only mode.

## Wrapper Default Behavior

Default mode still performs real pthread rwlock calls: 默认仍真实调用 pthread_rwlock.

```c
kvs_owner_rwlock_rdlock -> pthread_rwlock_rdlock
kvs_owner_rwlock_wrlock -> pthread_rwlock_wrlock
kvs_owner_rwlock_unlock -> pthread_rwlock_unlock
```

No weak behavior is enabled unless the operator explicitly sets `KVLITE_ROUTE_C_WEAK_RWLOCK=1` or test/control code calls `kvs_owner_lock_set_weak_mode(1)`.

## Owner Assertion, Warning, and Stats

The wrapper records the executor owner thread through `kvs_owner_lock_set_executor_thread()` at executor thread startup. `kvs_owner_lock_is_executor_thread()` uses `pthread_equal()` against the recorded owner.

The exported stats structure tracks:

- total wrapper calls.
- rdlock, wrlock, and unlock calls.
- executor-thread hits.
- non-executor calls.
- strict assertion failures.
- weak no-op calls.
- weak fallback calls.

In weak mode, non-executor calls are not allowed to silently no-op. They emit a warning and fall back to the real pthread rwlock path.

## Weak-Lock Mode

Weak-lock mode is an explicit no-op candidate:

- Enable with `KVLITE_ROUTE_C_WEAK_RWLOCK=1` or `kvs_owner_lock_set_weak_mode(1)`.
- Executor-owned wrapper calls verify owner-thread identity and then skip the real pthread rwlock call.
- Unlock pairing is protected by a thread-local weak no-op depth, so only locks actually no-op'd by this thread are no-op unlocked.
- Non-executor calls warn, increment assertion/fallback stats, and use real pthread rwlock.
- Init, destroy, and shutdown boundaries do not use the wrapper and remain real locks.

This is rollback-safe because removing the environment variable or calling `kvs_owner_lock_set_weak_mode(0)` returns the wrapper to real pthread rwlock behavior.

## Rollback

Rollback requires no code change:

1. Do not set `KVLITE_ROUTE_C_WEAK_RWLOCK`.
2. Or explicitly set `KVLITE_ROUTE_C_WEAK_RWLOCK=0`.
3. Or call `kvs_owner_lock_set_weak_mode(0)` in tests/control code.

The default binary path remains real `pthread_rwlock`.

## Shutdown, Stop, Destroy Safety

Shutdown still stops TTL background work, performs final AOF/RDB actions, stops/drains the command executor, then calls `dest_kvengine()`. `dest_kvengine()` retains the real naked write lock while destroying global KV/vector state. `pthread_rwlock_destroy(&g_kv_data_rwlock)` remains present and runs after data structure teardown. Weak mode does not affect these lifecycle calls.

## Stage 12 Gate

Result: `STAGE12_READY`.

The code is ready for a Stage 12 validation phase focused on runtime stress, TSAN/equivalent race checks, and possibly measuring weak-lock performance. Stage 12 should not delete `g_kv_data_rwlock` until weak mode has been exercised under mixed workload and replication/persistence stress.

## TSAN Note

TSAN was not required by this stage. If the current environment cannot support TSAN, static audit plus function/socket regression is acceptable for this gate, but Stage 12/13 should add TSAN or equivalent race detection before any permanent lock removal decision.
