# Stage 10.13 Ntyco Executor Convergence

## 1. Goal

Stage 10.13 closes the remaining ntyco owner blocker for Route C Full Executor Coverage. Before this stage, `network_mode=ntyco` parsed RESP in the coroutine and synchronously called `kvs_filter_protocol()`, which entered `kvs_command_task_execute_sync()` on the ntyco network thread. That meant ntyco could read/write store, TTL, vector/HNSW, AOF, and replication state outside the single command executor.

This stage keeps `g_kv_data_rwlock` intact and does not introduce multiple executors. It changes only the production ntyco path in `src/network/ntyco.c`; `src/network/ntyco_defult.c` remains a historical backup and is not compiled.

## 2. Before

```text
ntyco_conn_routine
  -> ntyco_process_received_data
  -> net_session_process_resp(..., ntyco_session_handler, ctx)
  -> ntyco_session_handler
  -> kvs_handler
  -> kvs_filter_protocol
  -> kvs_command_task_execute_sync
```

REPLSYNC and `ctx->is_replication` also stayed in the same synchronous handler model.

## 3. After

```text
ntyco_conn_routine
  -> ntyco_process_received_data
  -> net_session_parse_one_resp
  -> REPLSYNC: inline small protocol response only
  -> replicated apply: kvs_command_executor_submit_replicated
  -> normal command:
       ntyco_submit_command_task
       -> kvs_command_task_build
       -> kvs_command_task_copy_argv
       -> kvs_command_executor_submit
       -> ntyco_wait_executor_response
       -> kvs_command_executor_poll_response_for_owner(NTYCO, fd)
       -> append response to ntyco send buffer
```

The executor still owns command execution. The ntyco coroutine does not execute command handlers and does not call `kvs_filter_protocol()`.

## 4. Design Decision

This is a conservative owner-safe convergence, not a high-performance ntyco async adapter.

NtyCo does not currently expose a proven cross-thread coroutine wake/resume integration with the command executor. Instead of adding a risky callback path, the ntyco coroutine submits one command to the single executor and waits for its own owner-filtered response queue entry. This preserves correctness and closes the owner bypass with minimal scheduler changes.

Tradeoff: ntyco remains effectively one in-flight command per connection and waits synchronously for executor completion. Reactor/proactor remain the intended high-throughput network modes.

## 5. Response and Socket Ownership

- Executor never sends socket bytes.
- Normal command responses are generated in `kvs_response_task_t`, pushed to executor response queue with `owner_type=KVS_ASYNC_NET_OWNER_NTYCO`.
- The ntyco coroutine polls `kvs_command_executor_poll_response_for_owner(KVS_ASYNC_NET_OWNER_NTYCO, fd, ...)`.
- Response metadata is checked against `fd`, `session_id`, and `generation` before appending to the ntyco send buffer.
- Actual socket send remains in `ntyco_conn_routine()`.

## 6. Replication and REPLSYNC

REPLSYNC remains a small protocol-control response:

- checks `g_cfg.sync_mode == KVS_SYNC_EBPF`
- checks slave role
- marks `ctx->is_replication = 1`
- appends `+OK\r\n` or an error response

It does not access shared state and therefore does not need an executor task.

Replication apply commands use the same classifier introduced in Stage 10.12 and submit through `kvs_command_executor_submit_replicated()`. The ntyco network thread does not directly call set/del/mod/TTL apply functions.

## 7. Safety

- Disconnect while waiting: the wait loop exits on shutdown/closing; no socket send is performed by executor.
- fd reuse: ntyco response polling checks fd, session id, and generation before using a response.
- submit failure: local task/response allocations are destroyed and the connection path fails fast.
- executor stopping/stopped: `kvs_command_executor_submit()` and `kvs_command_executor_submit_replicated()` reject work; ntyco closes/fails the current connection.
- response target release: ntyco tasks do not transfer a connection owner ref to executor; the waiting coroutine owns the connection context and destroys response objects after polling.
- task ownership: normal command task/response are heap allocated and handed to executor; replicated task ownership is internal to `submit_replicated()`.

## 8. Completed Scope

- Production ntyco normal commands no longer call `net_session_process_resp()` or `kvs_filter_protocol()`.
- Production ntyco replicated apply uses `kvs_command_executor_submit_replicated()`.
- Production ntyco normal command uses `kvs_command_executor_submit()`.
- Startup log now says ntyco routes commands through the single command executor.
- Static and dynamic ntyco tests cover basic KV, TTL, lazy expire, and startup log.

## 9. Remaining Risk

- The implementation is correctness-first and may reduce ntyco throughput versus the prior in-coroutine synchronous handler.
- It polls response queue with short sleeps instead of using an eventfd/coroutine wake bridge.
- The historical `ntyco_defult.c` still contains legacy synchronous code but is not compiled.

## 10. Next Steps

1. Add a true ntyco eventfd/coroutine wake adapter only if ntyco throughput becomes important.
2. Add ntyco concurrency benchmarks to quantify the conservative wait model.
3. Continue Route C lock-retained-but-owner-safe audit toward eventual rwlock weakening/removal.
