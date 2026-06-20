# 阶段 6A：proactor 生命周期硬化

## 1. 阶段 6A 背景

阶段 6 设计结论是：当前 proactor/io_uring 不能直接接 executor。原因不是 adapter 抽象不足，而是 proactor connection 生命周期还不能安全承载跨线程 response 回投和 late CQE。

阶段 6A 只补 proactor 生命周期边界，不接 executor，不新增 proactor async ops，不接 response eventfd。proactor 命令执行仍保持同步 fallback：

```text
read CQE
  -> net_session_process_resp()
  -> proactor_session_handler()
  -> kvs_handler() 同步执行
  -> append session.send_buf
  -> post write SQE
```

## 2. 当前 proactor 生命周期风险

阶段 6A 前的主要风险：

- 连接只有裸 `conn_extension_t *`，没有 fd owner table。
- `net_session_t` 未通过 `net_session_set_identity()` 初始化 session_id、generation 和 owner_poller_id。
- read/write SQE 的 `io_uring_op_t` 只保存裸 ctx，不持有 connection ref。
- close/error 路径直接 `net_session_free()`、`close(fd)`、`kvs_free(ext)`。
- pending CQE 可能在连接释放后访问已释放 ctx。
- fd reuse 后旧 CQE 没有 owner/generation 防护。
- shutdown 时 pending read/write SQE 没有 drain 边界。

这些问题在同步 handler 模型下也可能被 high accept/disconnect 触发；后续接 executor 前必须先收口。

## 3. fd owner table 设计

阶段 6A 在 `proactor.c` 内部增加 proactor 私有 owner table：

- `proactor_connections[fd]`
- `proactor_fd_generation[fd]`

accept 初始化成功后：

```text
generation++
net_session_set_identity(session, next_session_id++, generation, 0)
proactor_connections[fd] = ext
```

close/error 早期：

```text
net_session_mark_closing(session)
if proactor_connections[fd] == ext:
    proactor_connections[fd] = NULL
close(fd)
ext->fd = -1
```

这样 late CQE 即使持有旧 ctx，也会看到 owner table mismatch 或 closing，不能继续处理命令，也不能误操作 fd reuse 后的新连接。

## 4. session identity / generation 设计

proactor accept 时调用 `net_session_set_identity()`：

- `session_id` 使用 proactor 私有递增计数。
- `generation` 来自 fd generation table。
- `owner_poller_id` 当前固定为 0。

阶段 6A 不设置 async adapter owner type，也不提交 command_task。这个 identity 只用于后续 current 校验和 fd reuse 防护的前置准备。

current 校验要求：

- ext 非 NULL。
- fd 在 proactor table 范围内。
- `proactor_connections[fd] == ext`。
- session 未 closing。
- session generation 与 fd generation table 一致。

## 5. pending SQE ref 设计

read/write SQE 创建时：

```text
allocate io_uring_op_t
conn ref++
op owns pending ref
set read_in_flight/write_in_flight
submit SQE
```

read/write CQE 完成时：

```text
if current and success:
    process read/write
else:
    only clear in_flight when ctx still exists
release op pending ref
free io_uring_op_t
```

accept op 不绑定 connection，不持有 connection ref。read/write op 必须持有 ref，因为 CQE 可能在 close/error/shutdown 后到达。

## 6. close/drain 设计

close 被拆成三个阶段：

1. request close
   - 幂等判断 owner ref 是否已经释放。
   - mark closing。
   - owner table 失效。
   - close fd once。
   - release owner ref。

2. pending op release
   - 每个 read/write CQE 归还自己的 pending ref。
   - 不要求连接仍 current。

3. final free
   - 只有 `net_session_ref_dec()` 返回 0 且 session closing 时才执行。
   - `net_session_free()` 和 `kvs_free(ext)` 只在 final free 中执行。

shutdown 时先关闭所有 current owner，再尽量 drain pending CQE，使 pending op ref 有机会归还后再退出 ring。

## 7. late CQE 处理规则

late read/write CQE 的规则：

- 可以读取 `op->ctx`，因为 op 持有 pending ref。
- 如果 owner table mismatch、generation mismatch 或 closing，则不访问 recv_buf/send_buf，不处理命令，不投递新的 SQE。
- error CQE 对 current 连接触发 request close。
- 所有 CQE 路径最终都必须 release pending op ref。

这保证 fd reuse 后旧 CQE 不会操作新连接，也不会访问已释放的旧连接。

## 8. 本阶段不做事项

阶段 6A 不做：

- 不接 executor。
- 不接 proactor async_net_ops。
- 不接 response eventfd。
- 不做 executor response drain。
- 不做多 executor。
- 不做 embedding worker pool。
- 不做 pipeline 并行。
- 不做 response reorder。
- 不改变 `g_kv_data_rwlock`。
- 不改变 RESP/RDB/AOF/主从协议。
- 不改变 command_task / response_task 模型。
- 不修改 reactor executor 行为。
- 不修改 ntyco。
- 不修 MOD 清 TTL 既有语义。
- 不修 `kvs_vector_rebuild_after_load` 既有 warning。

## 9. 验证方案

阶段 6A 重点验证 proactor 模式：

- `make clean && make embedding`
- proactor KV SET/GET
- TTL EXPIRE/PERSIST
- VSET/VGET/VSEARCH/VDEL
- pipeline 顺序
- disconnect/fd reuse
- high accept 200 或 500 连接

可选：

- ASAN/UBSAN 构建。
- ASAN/UBSAN 下重复 proactor socket 回归。

已知不归入 6A 的既有问题：

- `kvs_vector_rebuild_after_load` 隐式声明 warning。
- MOD 不清 TTL 语义问题。
- store/vector/HNSW 退出清理 LSAN 泄漏。

## 10. 后续 6B eventfd wake/drain 前置条件

6A 完成后，6B 才适合把 executor eventfd 纳入 io_uring owner loop。前置条件是：

- connection 有 fd owner table 和 generation。
- session identity 已初始化。
- close 不直接 free connection。
- read/write SQE 持有 pending ref。
- late CQE 可以安全 drop/release。
- shutdown 能关闭 owner 并 drain pending SQE。

6B 只应接 eventfd wake/drain 能力，不应同时接 async command submit。proactor async_net_ops 和 executor submit 应留到 6C。
