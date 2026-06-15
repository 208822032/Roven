# 阶段 6C：proactor executor 最小异步闭环

## 1. 背景

阶段 6C 在 reactor 已有 executor 闭环、proactor 生命周期硬化和 executor eventfd wake 接入之后，把 proactor 普通客户端命令接入 single command executor。目标是形成可验证的最小异步闭环，同时保持单 executor、单 proactor owner、每 session 单 in-flight、pipeline 保序和 replication 同步 fallback。

本阶段不改变 RESP/RDB/AOF/主从协议，不改变 `g_kv_data_rwlock`，不改变 vector/embedding 语义。

## 2. 阶段 6A/6B 已具备能力

阶段 6A 已经提供 fd owner table、`session_id`、`generation`、`owner_poller_id`、pending SQE ref、`request_close/mark_closing/final_free` 拆分，以及 late CQE 不访问失效 buffer 的生命周期边界。

阶段 6B 已经把 executor response eventfd 纳入 proactor io_uring owner loop。proactor 能 arm/cancel eventfd poll SQE，并在 CQE 到达后 drain eventfd counter。

## 3. 为什么必须先做 owner-aware response polling

原 `kvs_command_executor_poll_response()` 是全局 FIFO pop。若 proactor 直接调用它，可能弹出 reactor response，导致错误 owner append、错误 session ref release，或者 fd reuse 后把 response 投到错误连接。

因此 proactor 接入 executor 前必须先提供 owner-aware poll：只摘取匹配 `owner_type + owner_poller_id` 的 response，不匹配的 response 留在队列中。

## 4. executor response owner routing 设计

新增接口：

```c
int kvs_command_executor_poll_response_for_owner(
    int owner_type,
    int owner_poller_id,
    kvs_response_task_t **out
);
```

实现采用 response queue 链表扫描 conditional pop。命中匹配 owner 时摘链、维护 `resp_head/resp_tail`、递减 `response_queue_depth`、递增 `polled_responses`。未命中的 response 不移动、不释放、不改变统计。

保留 `kvs_command_executor_poll_response()` 的全局 FIFO 行为，避免破坏旧调用语义。当前代码也保留 `kvs_command_executor_poll_response_for()` 作为同等实现入口，`poll_response_for_owner()` 调用它。

## 5. proactor async_net_ops 设计

proactor 在 `src/network/proactor.c` 内实现 `kvs_async_net_ops_t`：

- `validate_owner`：校验 `PROACTOR` owner、fd 范围、`proactor_connections[fd] == owner`、`ext->fd`、`session_id`、`generation` 和 closing 状态。
- `append_response`：只在 proactor owner loop 复制 response bytes 到 `session.send_buf`。
- `drop_response`：记录 executor drop stats；`APPEND_FAILED` 且 target 仍 current 时 request close。
- `release_session`：调用 target 携带的 release callback，归还 executor target 持有的 session ref。
- `enable_write`：只在 owner loop 通过 `io_uring` 投递 write SQE。
- `restore_read`：在安全条件满足时继续提交下一条 buffered command。
- `wake_owner`：当前单 owner `owner_poller_id=0`，实际唤醒依赖 executor eventfd，该 op 保留占位。

executor 线程只复制 owner metadata，不解引用 proactor connection，不访问 io_uring ring。

## 6. proactor async command submit 路径

普通连接 read CQE 后：

```text
read CQE
  -> recv_buf.size += n
  -> net_session_parse_one_resp()
  -> replication/REPLSYNC 走同步 fallback
  -> heap command_task
  -> owned argv
  -> owned response_task
  -> session ref++
  -> single_inflight=1
  -> owner_type=PROACTOR, owner_poller_id=0
  -> submit executor
  -> consume parsed command bytes
```

构建 task、argv、response 或 submit 失败时，不消费 `recv_buf`，清理临时对象，并回到同步 `net_session_process_resp()` fallback。若已经设置 `single_inflight` 或增加 session ref，失败路径先 rollback 再 fallback。

## 7. eventfd CQE response drain 路径

proactor eventfd CQE：

```text
eventfd CQE
  -> drain eventfd counter
  -> poll_response_for_owner(PROACTOR, 0)
  -> response_target_from_task()
  -> kvs_async_net_dispatch_response(proactor_ops, target)
  -> destroy response_task
  -> rearm eventfd poll SQE
```

proactor 只消费 `PROACTOR/0` response，不消费 reactor response。dispatch 统一负责 append/drop、clear `single_inflight`、enable write、restore read 和 release session ref。

## 8. pipeline 保序策略

阶段 6C 继续使用每 session 单 in-flight：

- submit 成功前设置 `single_inflight=1`。
- response append/drop 后清理 `single_inflight`。
- 同一 session 下一条命令只能在上一条 response 完成 append/drop 后提交。
- 不做并行 pipeline，不做 response reorder。

proactor 额外要求 write SQE 正在引用 `send_buf` 时不提交会 append response 的下一条命令，避免 buffer realloc 破坏 io_uring send 指针。该策略保守但保持顺序和内存安全。

## 9. replication fallback

以下路径不进入 executor：

- `ext->is_replication` 已标记的连接。
- `REPLSYNC` 命令。
- 主从同步相关同步上下文。

这些命令继续通过 `proactor_session_handler()` 调用现有同步处理，复制上下文语义不变。

## 10. 生命周期路径

连接 owner ref、pending SQE ref 和 executor target ref 分离：

- accept 初始化 `session_id/generation/owner_poller_id`。
- read/write SQE 持有 pending op ref。
- async submit 增加 executor target ref。
- close 先 remove owner table 并 mark closing，再等待 refs 归零 final free。
- late CQE 只释放 pending op ref，不访问 stale buffer。
- response dispatch release executor target ref exactly once。

fd reuse 由 owner pointer、fd、`session_id` 和 `generation` 共同防护。

## 11. drop reason 与 stats

adapter drop reason 映射到 executor stats：

- `SESSION_MISMATCH` -> `session_mismatch_drop`
- `SESSION_CLOSING` -> `session_closing_drop`
- `APPEND_FAILED` -> `response_append_failed`

`SESSION_MISMATCH` 和 `SESSION_CLOSING` 不重复关闭失效连接。`APPEND_FAILED` 只在 target 仍 current 时 request close。

## 12. shutdown drain

shutdown 时停止 rearm eventfd，关闭 current proactor connections，drain eventfd，并使用 owner-aware poll 尽量处理 `PROACTOR/0` response。随后 cancel eventfd poll、drain pending CQE，并调用 `kvs_command_executor_stop()` join executor。

executor stop 会释放剩余 command/response queue，包含 owned argv、owned response buffer 和 response 持有的 session ref，避免不匹配 owner response 被遗漏。

## 13. 不做事项

本阶段不做多 executor、embedding worker pool、多 proactor poller、ntyco executor、pipeline 并行、response reorder、`g_kv_data_rwlock` 调整、协议调整、vector/embedding 语义调整、MOD TTL 修复、`kvs_vector_rebuild_after_load` warning 修复，也不把 replication connection 接入 executor。

## 14. 验证方案

构建：

```bash
make clean && make embedding
```

proactor 验证：KV SET/GET、TTL EXPIRE/PERSIST、VSET/VGET/VSEARCH/VDEL、scope、repeat VSET、pipeline 顺序、disconnect/fd reuse、high accept 200 或 500、`tests/semantic_onnx_test.sh`。

reactor 验证：`tests/semantic_onnx_test.sh`、基础 KV SET/GET、pipeline 顺序。

可选 sanitizer：

```bash
EXTRA_CFLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=address,undefined" \
make embedding ENABLE_EBPF=0
```

## 15. 风险清单

- `send_buf` 与 io_uring write SQE 指针生命周期必须保持稳定。
- conditional pop 扫描必须正确维护 tail 和 depth。
- submit failure rollback 必须清理 `single_inflight` 和 session ref。
- append failed 必须只关闭 current connection。
- shutdown 时 owner response drain 与 executor stop 不能 double release。
- reactor/proactor 不能互相消费 response。
- ignored `src/core/kvs_command_executor.c` 需要 `git add -f`。
