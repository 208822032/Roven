# 阶段 5：async network adapter 抽象设计

## 1. 阶段 5 背景

阶段 4B/4C 已经完成 reactor-only single command executor 闭环：

```text
reactor recv/parse
  -> heap command_task + owned argv
  -> owned response_task
  -> single_inflight
  -> executor command queue
single executor
  -> prepare
  -> execute_sync
  -> post_commit
  -> response queue
  -> eventfd wake reactor
reactor
  -> drain eventfd
  -> poll response
  -> validate fd/session
  -> append/drop
  -> clear inflight
  -> release session
```

阶段 5 的目标是把 response 回投中的通用语义从 `reactor.c` 抽象出来，让 executor 不关心 reactor/proactor/ntyco 的网络细节。阶段 5 只做抽象，不新增网络模式，不接 io_uring，不接 ntyco，不改变命令执行模型。

## 2. 4C 中 reactor.c 的耦合

阶段 4C 的 `reactor_handle_executor_response()` 同时承担了这些职责：

- 从 `kvs_response_task_t` 读取 fd、session owner、session_id、generation、request_seq。
- 访问 reactor 的 `conn_list[]` 和 `conn_extensions[]`，校验 fd slot owner。
- 校验 `session_id`、`generation` 和 `closing`。
- 判断 response length，失败时记录 drop stats 并关闭连接。
- 直接 reserve/append `session.send_buf`。
- 设置 `EPOLLIN | EPOLLOUT`。
- 清理 `single_inflight`。
- 继续尝试处理同 session 中已缓存的下一条请求。
- release session ref 并销毁 response。

这些逻辑让 executor response 回投流程与 reactor 内部结构强耦合。后续阶段 6 如果接 proactor/io_uring adapter，不能复用这段高层 dispatch 语义。

## 3. 新增 async network adapter 抽象

阶段 5 新增：

- `include/kvs_async_net_adapter.h`
- `src/network/kvs_async_net_adapter.c`

抽象层定义：

- owner type：`REACTOR` / `PROACTOR` / `NTYCO`。
- drop reason：`SESSION_MISMATCH` / `SESSION_CLOSING` / `APPEND_FAILED`。
- response target：executor response 中与网络 owner 相关的稳定快照。
- ops：由具体网络运行时实现的 validate/append/drop/release/event/wake 操作。
- dispatch：通用 response 回投流程。

阶段 5 只实现 reactor ops。`PROACTOR` 和 `NTYCO` 仅作为 owner type 占位，不接 executor。

## 4. 新增数据结构和接口

核心接口：

```c
typedef enum kvs_async_net_owner_type {
    KVS_ASYNC_NET_OWNER_REACTOR = 1,
    KVS_ASYNC_NET_OWNER_PROACTOR = 2,
    KVS_ASYNC_NET_OWNER_NTYCO = 3
} kvs_async_net_owner_type_t;

typedef struct kvs_async_net_response_target {
    int fd;
    kvs_async_net_owner_type_t owner_type;
    void *owner;
    int owner_poller_id;
    net_session_t *session;
    uint64_t session_id;
    uint32_t generation;
    uint64_t request_seq;
    char *response_buf;
    size_t response_len;
    size_t response_cap;
    void (*release_session)(void *owner);
} kvs_async_net_response_target_t;

typedef struct kvs_async_net_ops {
    int  (*validate_owner)(const kvs_async_net_response_target_t *target,
                           kvs_async_net_drop_reason_t *reason);
    int  (*append_response)(const kvs_async_net_response_target_t *target);
    void (*drop_response)(const kvs_async_net_response_target_t *target,
                          kvs_async_net_drop_reason_t reason);
    void (*release_session)(kvs_async_net_response_target_t *target);
    int  (*restore_read)(const kvs_async_net_response_target_t *target);
    int  (*enable_write)(const kvs_async_net_response_target_t *target);
    int  (*wake_owner)(int owner_poller_id);
} kvs_async_net_ops_t;
```

通用入口：

- `kvs_async_net_response_target_from_task()`
- `kvs_async_net_dispatch_response()`

`command_task` 和 `response_task` 新增 `owner_type` 字段。reactor async submit 时设置为 `KVS_ASYNC_NET_OWNER_REACTOR`，executor 只复制该元数据，不解释 owner 内部结构。

## 5. reactor adapter 实现方式

reactor-specific ops 仍保留在 `src/network/reactor.c`，原因是它们需要访问 `conn_list[]`、`conn_extensions[]` 和 `conn_extension_t`：

- `validate_owner`：校验 fd 范围、`conn_list[fd].fd`、fd slot owner、`session_id`、`generation`、`closing`。
- `append_response`：reserve `session.send_buf` 并复制 owned response buffer。
- `drop_response`：把 adapter drop reason 映射到 executor stats；append failed 且 owner 仍有效时关闭 fd。
- `release_session`：调用 response target 中保存的 session release callback。
- `enable_write`：设置 `EPOLLIN | EPOLLOUT`。
- `restore_read`：在 append 成功后，如果 `recv_buf` 仍有完整请求，继续按每 session 单 in-flight 规则提交下一条命令。
- `wake_owner`：阶段 5 reactor 下保留占位；实际 executor wake 仍由 command executor 的 eventfd 完成。

## 6. executor 与网络层解耦后的边界

executor 仍只负责：

- 从 command queue 取 `command_task`。
- 执行 `prepare -> execute_sync -> post_commit`。
- finalize owned `response_task`。
- 把 response push 到 response queue。
- 通过 eventfd wake owner poller。

executor 不访问：

- reactor fd 表。
- reactor `conn_extension_t`。
- `session.send_buf`。
- epoll event。
- fd reuse 校验逻辑。

网络 response 如何 validate、append、drop、restore read 和 release session，由 adapter dispatch + 具体 owner ops 完成。

## 7. 本阶段不做什么

阶段 5 不做：

- proactor executor。
- ntyco executor。
- io_uring adapter。
- 多 poller。
- 多 executor。
- embedding worker pool。
- pipeline 多 in-flight。
- response reorder。
- dynamic response buffer。
- store 锁模型调整。
- RESP/RDB/AOF/主从协议调整。

`REPLSYNC` 和已标记 replication connection 继续走 reactor 同步 fallback。proactor/ntyco 继续走旧同步 fallback。

## 8. 生命周期与 drop 路径

保持不变的生命周期：

1. async submit 前 command_task deep copy argv。
2. async response_task 使用 owned buffer。
3. submit 前 session ref++，response append/drop 后 release。
4. 每 session 单 in-flight，response append/drop 后清理。
5. fd reuse 通过 fd slot owner + `session_id` + `generation` 防护。
6. closing session response 必须 drop。
7. response_task 被 reactor/adapter 消费后销毁。

drop 路径：

- fd slot owner、session_id、generation 不匹配：`SESSION_MISMATCH`。
- identity 匹配但 session closing：`SESSION_CLOSING`。
- response length 非法或 send_buf reserve 失败：`APPEND_FAILED`。

`APPEND_FAILED` 在 owner 仍有效时关闭连接，随后统一 clear in-flight、release session、destroy response。executor drop stats 仍保留 4C 的分类计数。

## 9. 验证方案

阶段 5 验证必须覆盖：

- `make clean && make embedding`
- ONNX semantic test
- KV SET/GET 基础测试
- TTL 测试
- VSET/VGET/VSEARCH/VDEL 测试
- scope 测试
- repeat VSET 测试
- pipeline 顺序测试
- disconnect/fd reuse 测试
- high accept 测试

可选继续运行 ASAN/UBSAN/TSAN。TSAN 中 ONNX Runtime、signal、distributed shutdown、TTL stop 的既有报告不应误判为阶段 5 新问题。

## 10. 后续阶段 6 如何接 proactor/io_uring adapter

阶段 6 可以新增 proactor owner ops：

- response target 的 owner 指向 proactor heap connection context。
- validate_owner 校验 connection generation、session_id、closing 和 outstanding io_uring op 生命周期。
- append_response 写入 proactor session send buffer。
- enable_write 投递或更新 WRITE SQE。
- wake_owner 唤醒所属 io_uring poller 或 eventfd。
- release_session 对接 proactor context refcount。

阶段 6 的前提是先补齐 proactor connection context 的 refcount/closing/generation，并明确 read/write SQE 与 executor response 的共同生命周期。
