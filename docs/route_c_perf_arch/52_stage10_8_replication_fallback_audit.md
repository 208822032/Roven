# Stage 10.8 / 10.8A: Reactor/Proactor Replication Fallback Audit and Convergence

## 背景

Stage 10.2 (`45_stage10_2_lazy_expire_and_sync_fallback_audit.md`) 已经确认 reactor/proactor
在正常 client command 路径下会构造 `kvs_command_task_t` 并提交给 single command executor。
Stage 10.3 (`46_stage10_3_replicated_command_task.md`) 又把 TCP slave apply 收敛为
`kvs_command_executor_submit_replicated()`。

本阶段先审计剩余的 reactor/proactor replication fallback 和 proactor async fallback。
Stage 10.8A 在同一提交内收敛最小风险项：proactor async build/submit failure fallback。

- 不删除或弱化 `g_kv_data_rwlock`
- 不做 rwlock removal
- 不改变 RESP/store/AOF/RDB/replication/HNSW 语义
- 不引入多 executor、commit ordering 或 response reorder
- 不大改网络栈

## 结论

`reactor.c` 和 `proactor.c` 中的 eBPF `REPLSYNC` / `is_replication` 路径仍绕过
single command executor，直接进入同步 session handler，再调用 `kvs_handler`。启动时
`kvs_handler` 注册为 `kvs_filter_protocol`，因此最终进入：

```text
kvs_filter_protocol
  -> kvs_command_task_build
  -> kvs_command_task_prepare
  -> kvs_command_task_execute_sync
  -> kvs_command_task_post_commit
```

`kvs_command_task_execute_sync()` 会在当前调用线程中获取 `g_kv_data_rwlock`，并在
`KVS_CMD_RETRY_WITH_WRITE_LOCK` 时从读锁升级到写锁。因此这些路径目前仍依赖全局
rwlock 来保护 store/TTL/vector side effects。

Stage 10.8A 已处理 proactor normal async command 的 build/submit failure fallback：

```text
alloc/build/copy/submit failure
  -> proactor_fail_async_command(...) or direct fail-fast submit failure path
  -> release task/response
  -> resp_parser_reset(parser)
  -> return -1
  -> existing caller closes connection
```

该异常路径不再回退到 `p_process_received_data(ext)`，因此不再通过 proactor io_uring loop
同步执行 `kvs_handler` / `kvs_filter_protocol` / `kvs_command_task_execute_sync`。

**Stage 10.8A 结论：PARTIAL_CONVERGED.** Proactor async failure fallback 已收敛；reactor/proactor
eBPF replication fallback 仍是 legacy no-rwlock-removal path。

## Reactor Replication Fallback 调用链

代码证据：

- `src/network/reactor.c:301-316`: eBPF `REPLSYNC` 在 slave role 下把连接标记为 `ext->is_replication = 1`
- `src/network/reactor.c:326-337`: replication 连接设置 `sync_context = 2` 后调用 `kvs_handler(...)`
- `src/network/reactor.c:635-640`: `ext->is_replication || reactor_command_is_replsync(session)` 时直接调用 `net_session_process_resp(...)`
- `src/core/kvstore.c:52-83`: `kvs_filter_protocol(...)` 同步 build/prepare/execute/post_commit
- `src/core/kvs_command_task.c:526-563`: `kvs_command_task_execute_sync(...)` 在当前线程持有 `g_kv_data_rwlock`

实际链路：

```text
reactor recv_cb
  -> reactor_submit_async_command(fd, ext)
    -> net_session_parse_one_resp(fd, session, &cmd_len)
    -> if ext->is_replication || reactor_command_is_replsync(session)
      -> net_session_process_resp(fd, session, reactor_session_handler, ext)
        -> reactor_session_handler(...)
          -> REPLSYNC: ext->is_replication = 1, return +OK
          -> replication command: set_sync_context(2)
          -> kvs_handler(parser->tokens, parser->arg_index, parser->token_lens, response_buf)
            -> kvs_filter_protocol(...)
              -> kvs_command_task_execute_sync(...)
```

正常 reactor client command 路径不同：

```text
reactor_submit_async_command
  -> kvs_response_task_init_owned
  -> kvs_command_task_build
  -> kvs_command_task_copy_argv
  -> kvs_command_executor_submit(task)
```

也就是说，只有 normal client command 已经 executor-owned；replication/REPLSYNC 分支仍是同步
handler 路径。Stage 10.8A 在 `REPLSYNC` 被 slave 接受时新增 warning：

```text
reactor eBPF REPLSYNC uses legacy synchronous replication path; rwlock removal is not supported for this path
```

该 warning 不改变 eBPF replication 状态机，只把该路径明确标记为 legacy/no-rwlock-removal。

## Proactor Replication Fallback 调用链

代码证据：

- `src/network/proactor.c:472-485`: eBPF `REPLSYNC` 在 slave role 下把连接标记为 `ext->is_replication = 1`
- `src/network/proactor.c:494-504`: replication 连接设置 `sync_context = 2` 后调用 `kvs_handler(...)`
- `src/network/proactor.c:522-530`: `p_process_received_data(...)` 包装 `net_session_process_resp(...)`
- `src/network/proactor.c:778-780`: `ext->is_replication || proactor_command_is_replsync(session)` 时直接调用 `p_process_received_data(ext)`
- `src/core/kvstore.c:52-83`: `kvs_filter_protocol(...)` 同步 build/prepare/execute/post_commit

实际链路：

```text
proactor CQE read path
  -> proactor_submit_async_command(ring, ext)
    -> net_session_parse_one_resp(ext->fd, session, &cmd_len)
    -> if ext->is_replication || proactor_command_is_replsync(session)
      -> p_process_received_data(ext)
        -> net_session_process_resp(ext->fd, &ext->session, proactor_session_handler, ext)
          -> proactor_session_handler(...)
            -> REPLSYNC: ext->is_replication = 1, return +OK
            -> replication command: set_sync_context(2)
            -> kvs_handler(parser->tokens, parser->arg_index, parser->token_lens, response_buf)
              -> kvs_filter_protocol(...)
                -> kvs_command_task_execute_sync(...)
```

正常 proactor client command 路径不同：

```text
proactor_submit_async_command
  -> kvs_response_task_init_owned
  -> kvs_command_task_build
  -> kvs_command_task_copy_argv
  -> kvs_command_executor_submit(task)
```

Stage 10.8A 在 `REPLSYNC` 被 slave 接受时新增 warning：

```text
proactor eBPF REPLSYNC uses legacy synchronous replication path; rwlock removal is not supported for this path
```

该 warning 不改变 eBPF replication 状态机，只把该路径明确标记为 legacy/no-rwlock-removal。

## Proactor Async Failure Fallback 收敛

Stage 10.8 审计时，`proactor_submit_async_command()` 的异常降级路径会递增
`async_fallback_count` 并回到同步 `p_process_received_data(ext)`。
Stage 10.8A 已移除这些同步回退。

当前失败点处理方式：

| 失败点 | 当前行为 | 是否同步执行 handler |
|---|---:|---|
| `resp` 或 `task` 分配失败 | `proactor_fail_async_command(..., "alloc")` -> return `-1` | 否 |
| `kvs_response_task_init_owned` 失败 | `proactor_fail_async_command(..., "response_init")` -> return `-1` | 否 |
| `kvs_command_task_build` 失败且无响应状态 | `proactor_fail_async_command(..., "task_build")` -> return `-1` | 否 |
| `kvs_command_task_copy_argv` 失败 | `proactor_fail_async_command(..., "copy_argv")` -> return `-1` | 否 |
| `kvs_command_executor_submit` 失败 | release task/response, record stats, return `-1` | 否 |

当前链路：

```text
proactor_submit_async_command
  -> async task/response build or submit failure
  -> release owned allocations
  -> resp_parser_reset(parser) where applicable
  -> return -1
  -> existing caller proactor_conn_request_close(ext)
```

这条路径不是 replication 专属；它是 proactor normal async command 的异常 fallback。
Stage 10.8A 后，它不再能执行普通写命令、TTL 命令或 vector 命令，因此不再是 rwlock removal
blocker。这里选择关闭连接而不是生成 RESP error，是为了避免在半构造状态下引入新的 response ordering
或 recv_buf 消费语义。

## 是否仍直接调用 kvs_filter_protocol/kvs_handler

| 路径 | 是否直接调用 `kvs_handler` | 是否进入 `kvs_filter_protocol` | 是否提交 executor |
|---|---:|---:|---:|
| Reactor normal client command | 否 | 否 | 是，`kvs_command_executor_submit` |
| Reactor eBPF `REPLSYNC` / replication | 是 | 是 | 否 |
| Proactor normal client command | 否 | 否 | 是，`kvs_command_executor_submit` |
| Proactor eBPF `REPLSYNC` / replication | 是 | 是 | 否 |
| Proactor async build/submit failure | 否 | 否 | 否，fail-fast close |
| TCP slave apply after Stage 10.3 | 否 | 否 | 是，`kvs_command_executor_submit_replicated` |

## 是否可能写 Store/TTL/Vector

对 eBPF replication fallback：是。只要进入 `kvs_filter_protocol`，命令表和 handler 仍可执行完整命令语义：

- Store 写：`SET/RSET/HSET/SSET/PAPERSET`、`DEL/RDEL/HDEL/SDEL/PAPERDEL`、`MOD/...`
- TTL 写：`EXPIRE`、`PEXPIREAT`、`PERSIST`、`TTL/PTTL` 的过期副作用，以及 `GET/EXIST` 的 lazy expire 写锁重试
- Vector 写：`VSET`、`VDEL` 及其 HNSW/vector store 副作用
- Post-commit side effects：AOF append；TCP master replication 在 `sync_context != 2` 且 master role 下触发

Replication handler 会设置 `sync_context = 2`，这会跳过从节点只读检查和避免重复复制广播，但不会让
handler 变成只读，也不会把 store/TTL/vector mutation 转交给 executor。

对 proactor async build/submit failure：否。Stage 10.8A 后该路径直接 fail-fast 并关闭连接，不再执行
`kvs_handler`。

## 是否仍是 rwlock removal blocker

部分是。

原因不是这些路径当前不安全；它们当前依赖 `g_kv_data_rwlock` 才安全。问题在于 rwlock removal 的前提是
所有可能访问 live store/TTL/vector 的并发 owner 都收敛到 single command executor 或明确的 snapshot/task
边界。Stage 10.8A 后当前仍有以下非 executor owner：

| Owner | 路径 | 影响 |
|---|---|---|
| Reactor epoll thread | eBPF `REPLSYNC` / replication sync handler | 可同步执行写命令、TTL lazy expire、vector 写 |
| Proactor io_uring loop thread | eBPF `REPLSYNC` / replication sync handler | 可同步执行写命令、TTL lazy expire、vector 写 |

因此在 eBPF replication 路径改造或隔离前，`g_kv_data_rwlock` 不能移除或弱化。Proactor async
build/submit failure fallback 已不再是 rwlock removal blocker。

## 与 Stage 10.3 的边界

Stage 10.3 解决的是 TCP slave apply：

```text
distributed.c slave_connect_master
  -> ms_decode_message
  -> kvs_command_executor_submit_replicated(...)
  -> executor replicated queue
```

这条路径已经不再由 replication thread 直接修改 store/TTL。

Stage 10.8 发现的 blocker 是另一条 eBPF/REPLSYNC 路径：

```text
syncd -> slave client port -> REPLSYNC -> ext->is_replication
  -> network session handler
  -> kvs_handler / kvs_filter_protocol
```

两者不能混为同一个 blocker。TCP slave apply 已收敛；eBPF network replication fallback 未收敛。

## 推荐方案

### 方案 A：将 eBPF replication 命令封装为 executor-owned command task

推荐优先级最高。

核心思路：

1. `REPLSYNC` 握手仍可在网络线程同步返回 `+OK`，只标记连接状态，不访问 store。
2. `ext->is_replication` 后续命令不要调用 `net_session_process_resp -> kvs_handler`。
3. 像 normal async path 一样 parse one RESP、构造 owned `kvs_command_task_t`、copy argv。
4. 设置 `task->sync_context = 2` 和 `task->is_replication = 1`。
5. 提交 `kvs_command_executor_submit(task)`。
6. executor 执行时临时设置 `sync_context = task->sync_context`，保持从节点复制上下文语义。
7. response 仍按现有 async response path 回到对应 session，保持单连接 inflight 与 response ordering。

优点：
- 与现有 normal async path 结构一致
- 不需要引入第二个 executor
- 不改变 command handler、AOF、TTL、vector 语义
- 为 rwlock removal 移除一个明确 blocker

风险点：
- 必须精确保持 `REPLSYNC` 握手的同步行为和后续 replication command 的 response 行为
- `sync_context` 当前是 thread-local，需要让 executor 在执行 task 时设置/恢复
- 需要确认 slave role 写命令只读检查仍按 `sync_context = 2` 跳过

### 方案 B：禁用或隔离 eBPF replication 网络 fallback

如果短期不需要 eBPF syncd，可将 rwlock removal readiness 明确限定为：

```text
sync_mode = tcp only
network_mode = reactor/proactor async normal path only
```

同时在文档和启动校验中声明：启用 eBPF `REPLSYNC` 时不支持 rwlock removal。

优点是改动小；缺点是没有真正收敛 eBPF replication owner。

### 方案 C：移除 proactor async fallback 的同步执行能力（已完成）

对 proactor build/submit failure fallback，建议改为 fail-fast response 或关闭连接，而不是同步执行命令：

```text
allocation/build/submit failure -> append -ERR busy/internal -> do not execute command
```

这避免 OOM 或 executor unavailable 时把 mutation 重新带回 io_uring loop thread。

Stage 10.8A 已采用关闭连接的最小侵入实现，避免新增错误响应与 parser/recv_buf 消费语义。

## 测试

新增静态审计测试：

```text
tests/replication_fallback_stage10_8.sh
```

覆盖点：

1. reactor `REPLSYNC` 会设置 `ext->is_replication = 1`
2. reactor replication 分支会调用 `net_session_process_resp(...)`
3. reactor session handler 会设置 `sync_context = 2` 并调用 `kvs_handler(...)`
4. proactor `REPLSYNC` 会设置 `ext->is_replication = 1`
5. proactor replication 分支会调用 `p_process_received_data(ext)`
6. `p_process_received_data(...)` 会调用 `net_session_process_resp(...)`
7. proactor session handler 会设置 `sync_context = 2` 并调用 `kvs_handler(...)`
8. proactor async failure fallback 不再回到 `p_process_received_data(ext)`
9. `kvs_filter_protocol(...)` 仍同步调用 `kvs_command_task_execute_sync(...)`
10. TCP slave apply 仍使用 `kvs_command_executor_submit_replicated(...)`
11. reactor/proactor eBPF replication fallback 被明确标记为 legacy/no-rwlock-removal path

## 剩余 Blocker

与本阶段直接相关：

1. Reactor eBPF replication fallback 仍绕过 executor。
2. Proactor eBPF replication fallback 仍绕过 executor。

已收敛：

1. Proactor async build/submit failure fallback 不再同步执行 handler，不再是 rwlock removal blocker。

路线 C 全局剩余 blocker 还包括：

1. Master full sync 仍遍历 live store。
2. AOF rewrite / RDB save 仍需要 snapshot/task ownership 边界审计和收敛。
3. Ntyco 同步 handler 仍绕过 executor。
4. Shutdown 顺序仍需在 rwlock removal 前完成最终 owner 收敛校验。

## 建议提交

建议提交本阶段文档、静态测试和最小生产代码收敛。生产代码只涉及 proactor failure fallback fail-fast
和 eBPF replication legacy warning，不改变 RESP/store/AOF/RDB/replication/HNSW 语义。

建议 commit message：

```text
network: fail fast proactor async fallback
```
