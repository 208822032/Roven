# 阶段 6B：proactor executor eventfd wake 接入

## 1. 阶段 6B 背景

阶段 6A 已经补齐 proactor connection 的基础生命周期边界：fd owner table、generation、session identity、closing/refcount、pending SQE ref 和 late CQE 防护。

阶段 6B 只把 executor eventfd 接入 proactor/io_uring owner loop，让 proactor loop 能被 executor eventfd 唤醒并 drain eventfd counter。本阶段仍不把 proactor 普通命令提交到 executor。

## 2. 为什么只接 eventfd，不接 proactor executor submit

proactor 接 executor 的完整路径还缺少两块能力：

- proactor async_net_ops：validate、append、drop、release、restore_read、enable_write。
- executor response routing：当前 response queue 是全局 FIFO，`kvs_command_executor_poll_response()` 会直接 pop 任意 response。

如果阶段 6B 在 proactor 中调用 `poll_response()`，未来 reactor/proactor 同时演进时可能抢走属于 reactor owner 的 response。因此 6B 只建立 wake 能力，不启用 response drain。

## 3. executor eventfd 接入方式

proactor 启动时调用现有 executor 接口：

- `kvs_command_executor_start()`
- `kvs_command_executor_response_event_fd()`

没有新增 eventfd accessor。eventfd 仍由 executor 生命周期管理，proactor 只拿 fd 注册 io_uring poll。

接入方式：

```text
proactor_start
  -> kvs_command_executor_start()
  -> io_uring_queue_init_params()
  -> set_event_accept()
  -> proactor_arm_executor_eventfd()
```

eventfd SQE 使用 `io_uring_prep_poll_add(fd, POLLIN)`。poll CQE 到达后，proactor loop 使用 `read(eventfd, &counter, sizeof(counter))` 循环 drain 到 `EAGAIN/EWOULDBLOCK`。

## 4. eventfd op 生命周期

新增 op 类型：

- `EVENT_EXECUTOR_EVENTFD`

eventfd op 规则：

- 不绑定 `conn_extension_t`。
- 不持有 connection ref。
- 不计入 `proactor_pending_conn_ops`。
- `ctx == NULL`。
- SQE get 失败时释放 op。
- CQE 到达、cancel 到达或 shutdown drain 时释放 op。

read/write op 的 pending ref 规则不变。eventfd op 与 connection 生命周期隔离。

## 5. eventfd CQE 处理流程

eventfd CQE 流程：

```text
EVENT_EXECUTOR_EVENTFD CQE
  -> clear eventfd pending marker
  -> if cqe->res >= 0:
       drain executor eventfd counter
       do not poll executor response queue in 6B
  -> if not shutting down:
       rearm eventfd poll SQE
  -> release eventfd op
```

错误或 cancel CQE 也会 release op。非 shutdown 下只有正常 readable CQE 才 drain counter；错误路径不访问 response queue。

## 6. 是否消费 executor response queue

阶段 6B 不调用 `kvs_command_executor_poll_response()`。

代码审查结论：

- `kvs_command_executor_poll_response()` 从 `g_executor.resp_head` pop 全局 response。
- response queue 当前没有按 `owner_type` 或 `owner_poller_id` 路由。
- poll 后无法 peek 再归还，因此 proactor 不能安全判断并跳过 reactor response。

因此 6B 只 drain eventfd counter，保留 response queue 给当前拥有 executor response 路径的 reactor。

## 7. 全局队列下为什么不能在 proactor poll

如果 proactor 在 6B 中 poll response queue，会出现这些问题：

- 可能 pop 到 `KVS_ASYNC_NET_OWNER_REACTOR` response。
- proactor 没有 reactor fd slot 和 reactor ops，不能 append/drop/release 该 response。
- 即使检测 owner_type 后再 drop，也会破坏 reactor response exactly-once 回投。
- 当前接口没有把 response 放回队列的能力。

所以在有 owner routing 或 per-owner queue 之前，proactor 不能调用 `poll_response()`。

## 8. shutdown/rearm 规则

shutdown 规则：

- `g_shutdown_flag` 已设置时不再 rearm eventfd SQE。
- 退出 loop 后先关闭所有 proactor connection。
- 对 pending eventfd poll SQE 投递 cancel。
- drain pending connection op 和 eventfd op。
- `io_uring_queue_exit()` 后调用 `kvs_command_executor_stop()`。

如果 bounded drain 后 eventfd op 仍 pending，proactor 释放本地 op 元数据；ring exit 会清理内核侧 poll。eventfd fd 本身仍由 executor stop 关闭。

## 9. 本阶段不做事项

阶段 6B 不做：

- 不提交 proactor command 到 executor。
- 不实现 proactor async_net_ops。
- 不调用 `kvs_async_net_dispatch_response(proactor_ops, ...)`。
- 不调用 `kvs_command_executor_poll_response()`。
- 不改变 proactor read CQE 同步 fallback。
- 不改变 command_task / response_task 模型。
- 不做多 executor。
- 不做 embedding worker pool。
- 不做 pipeline 并行。
- 不做 response reorder。
- 不改变 `g_kv_data_rwlock`。
- 不改变 RESP/RDB/AOF/主从协议。
- 不修改 reactor executor 行为。
- 不修改 ntyco。
- 不修 MOD 清 TTL 既有语义。
- 不修 `kvs_vector_rebuild_after_load` warning。

## 10. 验证方案

阶段 6B 需要验证：

- `make clean && make embedding`
- proactor 模式：
  - KV SET/GET
  - TTL EXPIRE/PERSIST
  - VSET/VGET/VSEARCH/VDEL
  - pipeline 顺序
  - disconnect/fd reuse
  - high accept 200 或 500
- reactor 模式：
  - `tests/semantic_onnx_test.sh`
- ASAN/UBSAN proactor 回归。

重点确认：

- proactor 仍同步 fallback。
- eventfd CQE 不影响 read/write CQE。
- eventfd op 不泄漏。
- shutdown 不 hang。
- reactor executor response 路径不变。

## 11. 后续 6C 前置条件

进入 6C 前必须先解决 response routing：

- 要么 executor response queue 按 `owner_poller_id` / owner type 路由。
- 要么提供 peek + conditional pop。
- 要么保证同一进程内只有当前 owner loop 消费对应 response，并在接口层明确约束。

6C 还需要新增 proactor async_net_ops，并把 proactor read CQE 从同步 `net_session_process_resp()` 切到 parse-one-command + owned argv + single_inflight + executor submit。
