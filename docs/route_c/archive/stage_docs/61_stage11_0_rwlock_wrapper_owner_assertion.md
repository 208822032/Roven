# Stage 11.0 RWLock Wrapper and Owner Assertion Foundation

## 1. Goal

Stage 11.0 introduces infrastructure for future owner assertions around `g_kv_data_rwlock` without changing lock behavior.

This stage does not remove `g_kv_data_rwlock`, does not weaken it, and does not turn it into a no-op.

## 2. API

New header:

```text
include/kvs_owner_lock.h
```

New source:

```text
src/core/kvs_owner_lock.c
```

Wrapper API:

```c
int kvs_owner_rwlock_rdlock(pthread_rwlock_t *lock, const char *site);
int kvs_owner_rwlock_wrlock(pthread_rwlock_t *lock, const char *site);
int kvs_owner_rwlock_unlock(pthread_rwlock_t *lock, const char *site);
```

Owner thread API:

```c
void kvs_owner_lock_set_executor_thread(void);
int kvs_owner_lock_is_executor_thread(void);
```

## 3. Runtime Semantics

The wrappers currently call real pthread locks:

```text
kvs_owner_rwlock_rdlock -> pthread_rwlock_rdlock
kvs_owner_rwlock_wrlock -> pthread_rwlock_wrlock
kvs_owner_rwlock_unlock -> pthread_rwlock_unlock
```

The `site` argument is reserved for assertion/logging diagnostics. It does not change behavior.

## 4. Executor Owner Recording

`kvs_command_executor_thread_main()` calls `kvs_owner_lock_set_executor_thread()` when the executor thread starts. This records the executor owner thread id for future assertions.

`kvs_owner_lock_is_executor_thread()` compares the recorded owner id with `pthread_self()`.

## 5. Demonstration Replacement

Stage 11.0 replaces only the TTL scanner executor lock site:

```text
kvs_command_executor_execute_ttl_scan()
  -> kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "executor.ttl_scan")
  -> kvs_command_executor_run_ttl_scan()
  -> kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.ttl_scan")
```

This is a mechanical wrapper replacement. The wrapper still calls real pthread rwlock functions, so the lock remains fully effective.

## 6. Non-Goals

- No lock removal.
- No no-op wrapper.
- No lock weakening.
- No multi-executor.
- No commit ordering or response ordering change.
- No mass replacement of all lock sites in this stage.

## 7. Next Step

Stage 11.1 should replace remaining executor-internal `g_kv_data_rwlock` sites with the wrapper and decide assertion behavior:

- debug-only assert when expected owner is executor
- warning-only mode for init/destroy boundary
- explicit allowlist for current-thread snapshot fast paths

Only after a later full assertion pass should KVLite consider lock weakening/removal.
