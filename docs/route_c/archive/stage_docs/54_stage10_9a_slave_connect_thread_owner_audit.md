# Stage 10.9A TCP Slave Connect Thread Owner Boundary Audit

## Scope

Stage 10.9A closes the TCP slave connect thread owner gap identified in Stage
10.9. It does not remove or weaken `g_kv_data_rwlock`, does not change
RESP/store/AOF/RDB/replication/HNSW semantics, and does not change the eBPF
replication or ntyco legacy decisions.

## Previous risk

Before this stage, slave mode created the connect thread with a local
`pthread_t connect_thread`:

```text
distributed_init(ROLE_SLAVE)
  -> pthread_create(&connect_thread, NULL, slave_connect_thread, NULL)
```

The thread id was not stored in `global_dist_config`, and
`distributed_shutdown()` did not stop or join it for `ROLE_SLAVE`. If the thread
was blocked in `recv()` from the master, shutdown could continue into executor
stop and destroy while the connect thread still existed.

The thread did not directly mutate live store/vector state, but it decoded TCP
replication messages, allocated memory, logged, and submitted replicated tasks
through `kvs_command_executor_submit_replicated(...)`. That made the lifecycle
owner boundary incomplete.

## Current owner chain

Stage 10.9A adds slave connect thread ownership to `distributed_config_t`:

```text
slave_connect_thread
slave_connect_thread_started
slave_master_sockfd
```

Creation path:

```text
distributed_init(ROLE_SLAVE)
  -> pthread_create(&global_dist_config.slave_connect_thread, ...)
  -> global_dist_config.slave_connect_thread_started = 1
```

Runtime socket ownership:

```text
slave_connect_master()
  -> socket(...)
  -> distributed_set_slave_master_sockfd(sock)
  -> connect(...)
  -> recv/decode loop while !g_shutdown_flag
  -> distributed_clear_slave_master_sockfd(sock)
  -> close(sock)
```

Shutdown path:

```text
distributed_shutdown() on ROLE_SLAVE
  -> read slave_connect_thread_started/thread/slave_master_sockfd under slave_mutex
  -> shutdown(slave_master_sockfd, SHUT_RDWR) when present
  -> pthread_join(slave_connect_thread)
  -> clear slave_connect_thread_started and slave_master_sockfd
  -> pthread_mutex_destroy(&global_dist_config.slave_mutex)
```

This makes the connect thread joinable and owned by distributed shutdown.

## Executor and store boundary

The slave connect thread still does not directly call store, TTL, vector, AOF,
RDB, RESP handler, or synchronous network handler paths. It decodes
master-slave protocol frames and submits accepted operations through:

```text
kvs_command_executor_submit_replicated(op, key, klen, val, vlen, expire_at)
```

Stage 10.9A also guards the receive loop with `!g_shutdown_flag` and checks
`!g_shutdown_flag` before submitting replicated tasks. During process shutdown,
the thread can be unblocked by socket shutdown and joined before global
distributed state, memory, vector/HNSW, rwlock, or logging are destroyed.

`kvs_command_executor_submit_replicated(...)` already rejects work when the
executor is stopped or stopping. This remains the last-line guard, not the
primary owner boundary.

## Shutdown ordering note

The main shutdown chain still stops the command executor before
`dest_kvengine()` calls `distributed_shutdown()`. That order is acceptable after
Stage 10.9A because `g_shutdown_flag` prevents the slave connect thread from
submitting replicated tasks during shutdown, and `distributed_shutdown()` joins
the thread before global distributed state and memory are destroyed.

Future cleanup may move distributed shutdown earlier, but that is not required
for this stage and would have broader lifecycle implications for master/slave
snapshot behavior.

## Risk assessment

- Detached/unjoined thread: fixed for TCP slave connect thread.
- Destroy-after-thread risk: fixed for distributed state and memory lifetime.
- Direct live store/vector access: not present in slave connect thread.
- Executor-stopped submit: guarded by `g_shutdown_flag` plus executor submit
  rejection.
- FD leak: reduced by tracking `slave_master_sockfd`, shutdown wakeup, and
  close in the owner thread.
- Shutdown hang: reduced for connected `recv()` state. A blocking `connect()`
  to an unresponsive address can still be OS-dependent; this remains a bounded
  follow-up candidate if observed in deployment.

## Remaining blockers

1. eBPF `REPLSYNC` / `is_replication` synchronous fallback remains a legacy
   no-rwlock-removal path.
2. ntyco remains a legacy synchronous network mode.
3. TTL scanner still reads live store under `g_kv_data_rwlock` during normal
   runtime.
4. Master-side slave sync threads are join-owned, but their socket wakeup
   behavior still depends on closing the listen socket and slave connection
   lifecycle.

## Conclusion

Conclusion: `APPROVED` for TCP slave connect thread owner boundary.

This stage makes the slave connect thread joinable, gives shutdown a socket
wakeup handle, prevents shutdown-time replicated task submission, and preserves
the Stage 10.3 executor-owned slave apply semantics.

