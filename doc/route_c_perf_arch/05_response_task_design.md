# Response Task Design

## 1. 本阶段目标

阶段 3 的目标是引入 `response_task` 抽象，但仍同步回写。该阶段为异步 executor 和多 poller 的 response 回投做准备。

## 2. 当前代码现状

`net_session_process_resp()` 目前为每条命令预留 `KVS_MAX_RESPONSE_LENGTH`，把 `response_buf` 传给 handler。handler 同步写入 RESP，返回长度。`net_session_process_resp()` 将长度追加到 `send_buf.size`。网络层之后通过 `send_cb()` 或 proactor write CQE 写回 socket。

## 3. 设计方案

`response_task` 字段：

- session ref
- seq
- response buffer
- response length
- status
- error code
- destroy callback

buffer 策略：

- 不要每个 task 固定分配 2MB。
- 默认小 buffer，例如 4KB 或 16KB。
- 按需增长到 `KVS_MAX_RESPONSE_LENGTH`。
- VSEARCH topK 大响应使用增长 buffer。
- 如果 response 超过上限，生成错误 RESP。

同步阶段流程：

```text
execute command_task
  -> build response_task
  -> append response_task buffer to session send_buf
  -> destroy response_task
```

异步化后不能依赖：

- 栈 buffer。
- `net_session.send_buf` 内部直接预留指针。
- parser token 指向的 recv_buf 生命周期。

## 4. 不修改范围

阶段 3 不做：

- 不引入异步回写。
- 不引入 executor。
- 不引入多 poller。
- 不改变 socket send 逻辑。
- 不改变 RESP 协议。

## 5. 涉及模块和文件

- `include/net_session.h`
- `src/network/net_session.c`
- `src/network/reactor.c`
- `src/network/proactor.c`
- `src/network/ntyco.c`
- `src/core/kvstore.c`
- `src/core/kvs_operation.c`

## 6. 正确性边界

- response_task 的 buffer owner 必须明确。
- 同步回写后必须释放 response_task。
- future async 模式中 session 已关闭时 response_task 应安全丢弃。
- response seq 必须能支持后续同 session 保序。

## 7. 性能影响预期

阶段 3 可能带来少量额外分配成本。可通过小 buffer 和复用策略控制。阶段目标不是提升性能，而是解耦 response 生命周期。

## 8. 风险点

- 小 buffer 增长路径错误导致响应截断。
- 大响应分配过多导致内存峰值高。
- response_task 和 send_buf 双重复制。

## 9. 验证方式

- GET/SET/DEL RESP 字节一致。
- VSEARCH topK 大响应。
- 错误响应。
- pipeline 多响应顺序。
- 大 value GET。

## 10. 回滚方式

回滚到 handler 直接写 `net_session.send_buf` 的模型。

## 11. 下一阶段前置条件

进入 single executor 前，必须完成 command_task 和 response_task 的同步路径，并证明功能输出不变。
