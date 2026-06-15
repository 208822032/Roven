# Command Task Design

## 1. 本阶段目标

阶段 2 的目标是引入 `command_task` 抽象，但仍同步执行。该阶段只整理命令入口接口，为后续 executor 做准备。

## 2. 当前代码现状

当前 `net_session_process_resp()` 在解析出完整 RESP 命令后，直接调用网络层 handler。handler 调用 `kvs_filter_protocol(parser->tokens, parser->arg_index, parser->token_lens, response_buf)`。tokens 指向 `net_session.recv_buf` 内部数据，response 直接写入 `net_session.send_buf`。

## 3. 设计方案

`command_task` 最小字段：

- command type
- `argc`
- `argv`
- `argv_lens`
- session pointer 或 session id
- session seq
- flags：replication context、need_prepare、write/read
- prepared data 指针
- response placeholder
- destroy/free callback

argv 生命周期：

- 当前 RESP token 指向 `recv_buf`，同步执行时可用。
- task 化后，如果 task 生命周期超过 `net_session_process_resp()` 本轮解析，必须深拷贝。
- 普通同步阶段可先支持零拷贝 task wrapper，但接口必须标注“仅同步有效”。
- VSET/VSEARCH 后续异步 prepare 必须深拷贝 question/answer/scope。
- 大 value 命令需要避免重复大拷贝，后续可以引入 refcount buffer，但阶段 2 不做。

执行模型：

```text
RESP parser
  -> build command_task
  -> execute command_task synchronously
  -> write response to existing response_buf
  -> destroy command_task
```

## 4. 不修改范围

阶段 2 不做：

- 不引入线程。
- 不引入 queue。
- 不改变 response 写回方式。
- 不改变 `g_kv_data_rwlock`。
- 不改变 handler 语义。
- 不改变 RESP 协议。

## 5. 涉及模块和文件

- `include/net_session.h`
- `src/network/net_session.c`
- `src/core/kvstore.c`
- `src/core/kvs_operation.c`
- `include/kvs_command.h`

## 6. 正确性边界

- 同步模式下输出必须和当前 `kvs_filter_protocol()` 字节一致。
- task destroy 必须释放所有深拷贝字段。
- 零拷贝 task 不得逃逸出同步调用栈。
- replication context 必须保留当前 `set_sync_context()` 语义。

## 7. 性能影响预期

阶段 2 不应带来明显性能提升。目标是接口清晰和后续可演进。轻命令 latency 不应明显退化。

## 8. 风险点

- argv 生命周期误用。
- 大 value 深拷贝导致性能退化。
- command type 解析与 `command_table` 不一致。
- task 销毁路径遗漏。

## 9. 验证方式

- 普通 KV、TTL、vector、AOF load 全量回归。
- 对比改造前后 RESP 输出。
- 大 value SET/GET。
- pipeline 多命令。

## 10. 回滚方式

保留原 `kvs_filter_protocol()` 同步调用路径。若 task 抽象出问题，回滚到直接 handler 调用。

## 11. 下一阶段前置条件

进入 response_task 阶段前，必须确认所有命令都能通过 command_task 同步执行且行为不变。
