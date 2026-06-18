# Stage 10.9 Shutdown Save/Stop/Destroy Owner Boundary Audit

## Scope

Stage 10.9 audits the shutdown save/stop/destroy boundary after the Stage 10.5A,
10.6, and 10.7 snapshot conversions. It does not remove or weaken
`g_kv_data_rwlock`, does not change RESP/store/AOF/RDB/replication/HNSW
semantics, and does not rework the eBPF replication legacy path.

## Current shutdown chain

Main process shutdown currently follows this owner chain:

```text
network loop exits after g_shutdown_flag
  -> kvs_ttl_background_stop()
  -> aof_fsync(1)
  -> kvs_rdb_save(...)
       -> kvs_command_executor_build_rdb_snapshot(...)
       -> executor-owned snapshot of KV + vector state
       -> file write from immutable snapshot
  -> kvs_command_executor_stop()
       -> kvs_embedding_worker_pool_stop()
       -> drain executor queues until empty
       -> destroy leftover command/prepared/replicated/TTL/snapshot/response tasks
  -> dest_kvengine()
       -> distributed_shutdown() when TCP sync is enabled
       -> aof_free()
       -> write-lock global store/vector destroy
       -> mem_destroy()
  -> pthread_rwlock_destroy(&g_kv_data_rwlock)
  -> kvs_log_shutdown()
```

Stage 10.9 moved `kvs_ttl_background_stop() before final save`. The final RDB
snapshot still requires the command executor to be running, so executor stop
must remain after `kvs_rdb_save(...)`.

## Save ownership

### RDB final save

`kvs_rdb_save(...)` no longer traverses the live store directly from the
shutdown thread. It calls `kvs_command_executor_build_rdb_snapshot(...)`; the
executor builds an immutable KV/vector snapshot and the persistence layer writes
that snapshot to disk.

This path is snapshot-owned, but still protected by `g_kv_data_rwlock` while the
snapshot is built. That lock remains required for legacy paths.

### AOF final save and rewrite

Normal AOF shutdown uses `aof_fsync(1)` to flush the append-only stream. It does
not need to traverse live store/vector state.

AOF rewrite uses `kvs_command_executor_build_aof_rewrite_snapshot(...)` and then
writes an immutable snapshot to the temp file. Stage 10.9 does not rewrite the
already-converged Stage 10.7 logic.

## Stop and destroy ownership

### TTL background scanner

Before this stage, final RDB save happened while the TTL background scanner was
still alive. The scanner reads live store keys under `g_kv_data_rwlock` and
submits TTL expire tasks into the executor. That was safe only because the
rwlock was still present, but it was not a clean shutdown owner boundary.

Stage 10.9 stops the TTL scanner before final save. After that point it cannot
submit new TTL tasks or read live store during final RDB snapshot construction.

### Embedding worker pool

The embedding worker pool is owned by the command executor. Executor stop first
calls `kvs_embedding_worker_pool_stop()`, joins workers, and only then marks the
executor stopping and drains remaining queues. A worker that finishes prepare
before the stop boundary may submit a prepared task back to the executor; a
worker that cannot submit records failure and does not fall back to synchronous
store execution.

Prepared task cleanup is still owned by executor stop. This prevents vector
prepared tasks from running concurrently with vector/HNSW destroy after the
executor has joined.

### Command executor

`kvs_command_executor_stop()` joins the executor thread and then clears leftover
normal, prepared, replicated, TTL, full-sync snapshot, RDB snapshot, AOF
snapshot, and response queues. Submit APIs reject new normal, replicated, TTL,
and snapshot tasks when the executor is not running or is stopping. Proactor
submit failure is fail-fast after Stage 10.8A and does not call the synchronous
handler fallback.

### Replication threads

TCP master replication threads are stopped by `distributed_shutdown()` inside
`dest_kvengine()`. Master-side full sync uses an executor snapshot. Slave-side
apply submits `kvs_command_executor_submit_replicated(...)` and the submit API
rejects work after executor stop.

There is still an owner-boundary limitation: slave connect thread ownership is
not fully joined by `distributed_shutdown()`. This is not changed in Stage 10.9
and remains a follow-up item.

## Destroy boundary

`dest_kvengine()` calls `distributed_shutdown()` and `aof_free()` before taking
the global write lock and destroying array/rbtree/hash/skiptable/vector state.
After Stage 10.9, the TTL scanner and executor-owned tasks have already exited
before store/vector destroy. This means vector/HNSW destroy should not race with
normal command tasks, prepared VSET tasks, replicated tasks, TTL tasks, or
snapshot tasks in the reactor/proactor mainline.

The eBPF REPLSYNC/is_replication synchronous handler path and ntyco legacy mode
still bypass the executor by design. They remain no-rwlock-removal paths.

## Remaining executor bypasses

Active shutdown owner boundary blockers:

1. TCP slave connect thread owner is not fully joined in `distributed_shutdown()`.
2. eBPF REPLSYNC/is_replication remains a legacy synchronous replication path.
3. ntyco remains a legacy synchronous network mode.
4. TTL scanner still reads live store under `g_kv_data_rwlock` during normal
   runtime, although it is now stopped before final save/destroy.

Not active blockers after prior stages:

1. Master full sync uses executor snapshot.
2. RDB save uses executor snapshot.
3. AOF rewrite uses executor snapshot.
4. TCP slave apply uses `kvs_command_executor_submit_replicated(...)`.
5. Proactor async build/submit failure no longer falls back to synchronous
   handler execution.

## Readiness conclusion

Conclusion: `CONDITIONALLY_READY`.

The reactor/proactor shutdown save/stop/destroy order no longer has an obvious
final-save race between the TTL scanner and executor snapshot save. Final RDB
save and AOF rewrite are snapshot-owned. Executor stop drains or releases queued
tasks and joins embedding workers before store/vector destroy.

`g_kv_data_rwlock` must remain. Future rwlock removal still requires closing the
legacy eBPF replication path, ntyco legacy mode, normal-runtime TTL scanner live
store reads, and the TCP slave connect thread ownership gap.

## Validation

Static and smoke coverage is in:

```text
tests/shutdown_owner_stage10_9.sh
```

The script checks shutdown ordering, snapshot ownership, executor submit stop
guards, Stage 10.8A proactor fail-fast behavior, and a reactor RDB/AOF shutdown
smoke with KV, TTL, and optional vector commands.
