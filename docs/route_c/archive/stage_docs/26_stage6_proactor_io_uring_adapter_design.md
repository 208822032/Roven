# 阶段 6：proactor / io_uring adapter 接入设计

## 1. 阶段 6 背景

阶段 5 已经把 reactor executor response 回投流程抽象成 async network adapter。executor 只负责执行 `command_task` 并产出 `response_task`，网络 owner 负责校验 session、append response、恢复读写事件和释放 session 引用。

阶段 6 的目标不是直接实现 proactor executor，而是先分析当前 `src/network/proactor.c` 的 io_uring 生命周期是否已经满足 adapter 接入条件，并给出后续最小实现路径。

结论：当前 proactor 不适合直接接 executor。它仍是同步 handler fallback 模型，缺少足够的连接生命周期边界、pending SQE 引用和 owner loop 唤醒消费点。直接接 executor 会放大 fd reuse、CQE use-after-free、跨线程操作 send buffer 和 shutdown drain 风险。

## 2. 阶段 5/5.1 已具备的 adapter 基础

阶段 5/5.1 已具备：

- `kvs_async_net_owner_type_t`：已预留 `REACTOR` / `PROACTOR` / `NTYCO`。
- `kvs_async_net_response_target_t`：保存 fd、owner type、opaque owner pointer、owner poller id、session、session_id、generation、request_seq 和 owned response buffer。
- `kvs_async_net_ops_t`：抽象 validate、append、drop、release、restore read、enable write、wake owner。
- `kvs_async_net_dispatch_response()`：统一处理 validate、append/drop、clear single_inflight、restore read、enable write 和 release target。
- owner pointer 契约：executor 只能复制 opaque pointer，不能解引用；只有对应 ops 可以解释。
- 非法 ops cleanup、APPEND_FAILED unregister 边界和 restore_read refcount 注释已经补强。

这些抽象足够表达 proactor response 回投，但 proactor runtime 本身还没有提供安全实现这些 ops 所需的 owner 生命周期能力。

## 3. 当前 proactor/io_uring 网络模型梳理

当前 `src/network/proactor.c` 的连接对象是：

```c
typedef struct conn_info {
    int fd;
    net_session_t session;
    size_t send_offset;
    int read_in_flight;
    int write_in_flight;
    int is_replication;
} conn_extension_t;
```

io_uring user data 使用：

```c
typedef struct {
    int op;
    conn_extension_t *ctx;
} io_uring_op_t;
```

当前路径：

```text
accept CQE
  -> malloc conn_extension_t
  -> p_init_conn_extension()
  -> set_event_recv()

read CQE
  -> ctx->read_in_flight = 0
  -> recv_buf.size += cqe->res
  -> net_session_process_resp()
  -> proactor_session_handler()
  -> kvs_handler() 同步执行命令并写 send_buf
  -> post_recv_if_needed()
  -> post_send_if_needed()

write CQE
  -> ctx->write_in_flight = 0
  -> send_offset += cqe->res
  -> flush 完成后清空 send_buf
  -> post_send_if_needed()
  -> post_recv_if_needed()
```

当前 proactor 特点：

- 有 per-connection `conn_extension_t`，但没有 reactor 那样的 fd slot 表和 owner 校验表。
- `net_session_t` 已经包含 session_id、generation、closing、refcount、owner_poller_id、single_inflight 字段，但 proactor 初始化路径未调用 `net_session_set_identity()`，因此 identity/generation/owner_poller_id 没有被赋予有效运行时语义。
- read/write SQE 的 `io_uring_op_t` 只保存裸 `conn_extension_t *`，没有对 connection/session refcount 加引用。
- `p_free_conn_extension()` 会直接 `net_session_free()`、`close(fd)`、`kvs_free(ext)`。
- close/error 路径没有先标记 `closing` 并等待 pending read/write CQE drain。
- owner loop 当前通过 `io_uring_wait_cqe_timeout()` 轮询等待 CQE，没有注册 executor response eventfd，也没有 response queue drain 分支。
- `set_event_send()` 只能在 proactor loop 所在线程安全调用，因为它直接操作 io_uring ring 和 `send_buf`。

## 4. 与 reactor executor 路径的差异

reactor executor 路径已经具备：

- `conn_extensions[fd]` fd slot owner 表。
- 每次 fd accept 增加 generation，并调用 `net_session_set_identity()`。
- close 路径 `net_session_mark_closing()`。
- executor submit 前对 session ref++。
- response 回来后通过 fd slot owner + session_id + generation + closing 校验。
- executor eventfd 注册到 epoll，response 只在 reactor loop 线程 append 到 send_buf。
- append 成功后由 reactor loop enable write。
- drop/append/error 路径统一 clear single_inflight 并 release executor target ref。

proactor 当前差异：

- 没有 fd slot owner 表，无法验证 fd 是否仍属于同一连接对象。
- 没有 per-fd generation 递增，fd reuse 防护不成立。
- 没有 pending SQE ref，CQE 可能在连接被释放后拿到悬空 `ctx`。
- 没有 owner wake event 注册到 io_uring loop，executor response 不能可靠唤醒 owner loop。
- 没有 response drain 分支，executor response 即使入队也不会被 proactor 消费。
- 读写 SQE 和 response append 共享 `send_buf`，必须保证都在 owner loop 线程执行。

## 5. proactor 接 executor 的最小可行路径

最小可行路径应保持 executor 模型不变，只让 proactor 像 reactor 一样在 owner loop 中消费 executor response：

```text
proactor read CQE
  -> parse one command
  -> heap command_task + owned argv
  -> owned response_task
  -> session single_inflight
  -> session ref++
  -> submit executor command queue

executor
  -> execute_sync
  -> push response queue
  -> eventfd wake

proactor owner loop
  -> observe executor eventfd completion
  -> drain eventfd
  -> poll executor response
  -> kvs_async_net_dispatch_response(proactor_ops, target)
  -> append/drop/release in proactor loop thread
  -> submit write SQE if needed
```

关键约束：

- executor 线程不能直接操作 io_uring ring。
- executor 线程不能直接 append proactor `send_buf`。
- response 必须回投到 connection 所属 proactor owner loop 后再 append。
- `enable_write` 必须由 owner loop 投递 write SQE。
- 每 session 仍然只允许一个 executor command in-flight。
- pipeline 通过 append 成功后的 `restore_read` 再提交下一条请求，不能并行执行。
- replication connection 继续同步 fallback。

## 6. io_uring owner loop wakeup 设计

推荐使用 executor 现有 response eventfd，但 proactor 必须把它纳入 io_uring owner loop：

1. `kvs_command_executor_start()` 仍创建单个 eventfd。
2. proactor 初始化 ring 后，为 executor eventfd 投递一个 poll/read 型 SQE。
3. executor push response 后写 eventfd。
4. proactor loop 收到 eventfd CQE 后 drain eventfd。
5. proactor loop 调用 `kvs_command_executor_poll_response()`，逐个 dispatch。
6. dispatch 完成后重新 arm eventfd SQE。

可选实现方向：

- `IORING_OP_POLL_ADD` 监听 eventfd readable，再在 owner loop 中 `read(eventfd)` drain。
- 或投递 eventfd read SQE，收到 8 字节 counter 后重新投递。

阶段 6A/6B 建议先实现单 proactor loop + 单 executor eventfd，不设计多 poller。`owner_poller_id` 当前可固定为 0，但必须通过 `net_session_set_identity()` 写入，避免后续多 poller 时缺少字段语义。

如果阶段 6 无法安全把 executor eventfd 纳入 io_uring loop，应保持 proactor 同步 fallback，不应让 executor response 依赖 100ms timeout 轮询间接消费。

## 7. pending SQE/CQE 生命周期设计

当前 `io_uring_op_t` 只保存裸 `conn_extension_t *`，不能直接用于 executor 模式。需要引入 pending op 引用规则：

```text
submit read SQE
  -> conn ref++
  -> op owns one pending ref

read CQE
  -> use ctx only after current/generation/closing 校验
  -> clear read_in_flight
  -> op release ref

submit write SQE
  -> conn ref++
  -> op owns one pending ref

write CQE
  -> use ctx only after current/generation/closing 校验
  -> clear write_in_flight
  -> op release ref
```

close/error 路径应变为：

```text
mark closing
close fd or shutdown fd
remove fd owner mapping
do not free conn immediately
wait until:
  reactor/proactor owner ref released
  executor target refs released
  pending SQE refs released
then net_session_free + free conn
```

这要求 proactor 拥有与 reactor 类似的 `proactor_conn_ref_inc()` / `proactor_conn_release()`，并且所有 SQE user_data 都持有显式 ref。否则 read/write CQE 仍可能在 `p_free_conn_extension()` 后访问已释放内存。

## 8. session ref / closing / generation 设计

proactor accept 时应建立 session identity：

- 分配 heap `conn_extension_t`。
- fd 范围合法时更新 `g_proactor_fd_generation[fd]`。
- 调用 `net_session_set_identity(&ext->session, next_session_id, generation, owner_poller_id)`。
- 把 fd 映射到当前 `conn_extension_t *`，例如 `proactor_connections[fd] = ext`。

validate_owner 需要校验：

- `target->owner_type == KVS_ASYNC_NET_OWNER_PROACTOR`。
- fd 范围合法。
- fd owner table 中的 `proactor_connections[fd] == target->owner`。
- `conn->fd == target->fd`。
- `session_id == target->session_id`。
- `generation == target->generation`。
- `closing == 0`。

fd reuse 防护必须依赖 fd + owner pointer + session_id + generation，不能只依赖 fd 或 `ctx` 指针。

close 路径必须先 `net_session_mark_closing()`，并且 fd owner table 失效后，旧 response/CQE 只能 drop/release，不能 append 或 submit write SQE。

## 9. response target 字段是否需要扩展

当前 `kvs_async_net_response_target_t` 字段对单 proactor loop 的最小接入基本足够：

- `fd`
- `owner_type`
- `owner`
- `owner_poller_id`
- `session`
- `session_id`
- `generation`
- `request_seq`
- `response_buf`
- `response_len`
- `response_cap`
- `release_session`

阶段 6A/6B 不建议扩展 target 字段。proactor-specific 信息应保留在 `conn_extension_t` 和 proactor ops 内部，例如 ring 指针、read/write in-flight、send_offset、owner table。

只有进入多 poller 或多 ring 后，才考虑扩展：

- runtime instance id。
- ring owner id。
- connection local sequence。
- response dispatch affinity。

这些不是单 proactor loop 接入 executor 的前置条件。

## 10. proactor async_net_ops 设计草案

proactor ops 可以按以下语义设计：

```c
static int proactor_async_validate_owner(
    const kvs_async_net_response_target_t *target,
    kvs_async_net_drop_reason_t *reason
);

static int proactor_async_append_response(
    const kvs_async_net_response_target_t *target
);

static void proactor_async_drop_response(
    const kvs_async_net_response_target_t *target,
    kvs_async_net_drop_reason_t reason
);

static void proactor_async_release_session(
    kvs_async_net_response_target_t *target
);

static int proactor_async_restore_read(
    const kvs_async_net_response_target_t *target
);

static int proactor_async_enable_write(
    const kvs_async_net_response_target_t *target
);

static int proactor_async_wake_owner(int owner_poller_id);
```

建议职责：

- `validate_owner`：校验 owner type、fd owner table、session_id、generation、closing。
- `append_response`：只在 owner loop 线程 reserve/copy 到 `session.send_buf`。
- `drop_response`：记录 executor drop stats；`APPEND_FAILED` 时关闭仍 current 的 fd。
- `release_session`：调用 target 中保存的 release callback，归还 executor target ref。
- `restore_read`：如果 `recv_buf` 中已有完整命令且没有 single_inflight，则继续提交下一条 async command；复制连接仍走同步 fallback。
- `enable_write`：如果 `send_buf` 有数据且没有 write_in_flight，则投递 write SQE。
- `wake_owner`：单 loop 阶段可作为占位；真正唤醒由 executor eventfd + io_uring eventfd SQE 完成。

`enable_write` 不应从 executor 线程调用。它只应在 proactor owner loop 的 response dispatch 中执行。

## 11. drop reason 与 stats 映射

继续复用阶段 5 drop reason：

- `KVS_ASYNC_NET_DROP_SESSION_MISMATCH`
  - fd owner 不匹配。
  - session_id/generation 不匹配。
  - fd 已关闭或复用。
  - 映射到 `KVS_COMMAND_EXECUTOR_DROP_SESSION_MISMATCH`。

- `KVS_ASYNC_NET_DROP_SESSION_CLOSING`
  - identity 匹配但 session 已 closing。
  - 映射到 `KVS_COMMAND_EXECUTOR_DROP_SESSION_CLOSING`。

- `KVS_ASYNC_NET_DROP_APPEND_FAILED`
  - response 长度非法。
  - send_buf reserve 失败。
  - append 后无法安全投递 write SQE。
  - 映射到 `KVS_COMMAND_EXECUTOR_DROP_APPEND_FAILED`，并关闭仍 current 的连接。

proactor 不应新增一套 executor stats。阶段 6 应保持 command_queue_depth、response_queue_depth、eventfd_wakeup_count、eventfd_write_fail_count、drop reason 计数语义不退化。

## 12. shutdown drain 设计

shutdown 时需要明确四类对象的 drain：

1. executor command queue。
2. executor response queue。
3. proactor pending read/write/eventfd SQE。
4. connection refs。

推荐顺序：

```text
set shutdown flag
stop accepting new connections
mark all proactor connections closing
close or shutdown fds to force pending I/O complete
continue draining CQE until pending op refs reach zero or bounded timeout
drain executor response queue through proactor adapter where possible
stop command executor
release remaining connection owner refs
io_uring_queue_exit
```

必须保持：

- shutdown 后 command_queue_depth 和 response_queue_depth 最终归零。
- pending response drop 后仍 clear single_inflight 并 release executor target ref。
- pending SQE CQE 即使 late arrival，也只能看到 closing/current mismatch 并 release op ref。

如果阶段 6 初期不能可靠 drain pending SQE，不应开启 proactor executor 路径。

## 13. 不做事项

阶段 6 设计不做：

- 多 executor。
- embedding worker pool。
- 多 poller。
- pipeline 并行。
- response reorder。
- `g_kv_data_rwlock` 调整。
- RESP/RDB/AOF/主从协议调整。
- command_task / response_task 基础模型调整。
- MOD 清 TTL 语义修复。
- `kvs_vector_rebuild_after_load` warning 修复。
- ntyco executor 接入。

replication connection 继续同步 fallback。`REPLSYNC` 识别和已标记复制连接不进入 executor。

## 14. 风险清单

主要风险：

- **CQE UAF**：当前 pending SQE 不持有连接 ref，close 后 CQE 仍可能访问已释放 `ctx`。
- **fd reuse**：当前 proactor 没有 fd owner table 和 generation 初始化，旧 response 可能命中新 fd。
- **跨线程 send_buf 写入**：executor 线程若直接 append 会与 io_uring write SQE 共享 buffer，存在数据竞争和发送内容破坏。
- **ring 跨线程误用**：io_uring SQE submit 必须收束到 owner loop，不能从 executor 线程直接投递。
- **eventfd wake 缺失**：没有把 executor eventfd 接入 io_uring loop 时，response 不能及时消费。
- **closing drain 不完整**：close 路径当前直接 free，不能承载 executor response 和 pending SQE 的异步返回。
- **single_inflight 泄漏**：drop、append failed、submit failed、closing、shutdown 路径都必须清理。
- **replication 误入 executor**：复制连接必须继续同步 fallback，否则 sync_context 和主从协议语义可能变化。
- **shutdown hang/leak**：如果 pending SQE 或 response queue 没有统一 drain，可能出现退出挂起或泄漏。

## 15. 分阶段实现建议

### 6A：proactor 生命周期硬化，不接 executor

目标是让 proactor connection 具备 adapter 所需基本安全边界，但仍保持同步 fallback：

- 增加 proactor fd owner table。
- accept 时调用 `net_session_set_identity()`。
- 增加 proactor fd generation。
- close 路径改为 mark closing + owner table 失效 + refcount release。
- read/write SQE user_data 持有 pending op ref。
- CQE 完成后 release pending op ref。
- 保持 `net_session_process_resp()` 同步 handler 不变。

完成标准：disconnect/fd reuse、高 accept、ASAN/UBSAN 下无新增 UAF/leak。

### 6B：接入 executor eventfd 到 io_uring owner loop

目标是只建立 response wake/drain 能力，仍不提交 proactor async command：

- proactor ring 注册 executor eventfd SQE。
- eventfd CQE 分支 drain eventfd。
- 调用 `kvs_command_executor_poll_response()`，但在没有 proactor async submit 前理论上不应拿到 proactor response。
- 明确 eventfd SQE 的 rearm 和 shutdown drain。
- 保持 reactor executor 行为不变。

完成标准：proactor loop 可以安全响应 executor eventfd，不依赖 timeout 轮询，不影响现有同步命令。

### 6C：proactor async_net_ops + async command submit

目标是把非复制普通连接接入 executor：

- 新增 proactor `kvs_async_net_ops_t`。
- read CQE 中改为 parse one command + owned argv + owned response_task + single_inflight + submit executor。
- response eventfd CQE 中 dispatch proactor response。
- append 成功后投递 write SQE。
- restore_read 继续提交 recv_buf 中下一条 pipeline 命令，但保持单 in-flight。
- replication connection 继续同步 fallback。

完成标准：make embedding、semantic、KV/VSET/VSEARCH、pipeline 顺序、disconnect/fd reuse、high accept、ASAN/UBSAN 通过；response 不 reorder，queue depth shutdown 后归零。

### 6D：审查后再考虑多 poller或 ntyco

只有 6A-6C 稳定后，才考虑：

- 多 proactor ring。
- owner_poller_id routing。
- ntyco adapter。
- 更细粒度 wake owner。

这些都不是阶段 6 最小接入路径。
