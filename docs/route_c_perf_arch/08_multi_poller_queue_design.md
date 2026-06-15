# Multi Poller And Queue Design

## 1. 本阶段目标

阶段 6 的目标是引入多 network poller / 多队列，让网络收发和 RESP parse 能并发扩展。该阶段不直接解决 ONNX OrtRun 串行瓶颈。

## 2. 当前代码现状

当前 `reactor` 是单 epoll loop。`proactor` 是单 io_uring completion loop。`ntyco` 当前实现中每连接协程使用阻塞 `poll/recv/send`，不适合作为路线 C 基础。

## 3. 设计方案

多 poller 模型：

- 主 acceptor 接收连接后分配给某个 poller。
- 每个 poller 拥有自己的 epoll fd 和连接集合。
- fd ownership 固定到 poller，跨 poller 不直接操作 fd。
- 每个 session 记录所属 poller id。
- poller parse RESP 后生成 command_task。
- command_task 投递到 global MPSC command queue。
- executor 生成 response_task 后投递回 session 所属 poller 的 response queue。
- poller 通过 eventfd 或 pipe 被唤醒，注册 EPOLLOUT 并发送。

queue：

- poller -> executor：MPSC command queue。
- executor -> poller：每 poller 一个 SPSC 或 MPSC response queue。

## 4. 不修改范围

阶段 6 不做：

- 不引入 embedding worker pool。
- 不删除 `g_kv_data_rwlock`。
- 不改 RESP 协议。
- 不以当前 ntyco 为基础。
- 不改变 commit executor 单 owner 模型。

## 5. 涉及模块和文件

- `src/network/reactor.c`
- `include/net_session.h`
- `src/network/net_session.c`
- executor 模块
- response_task 模块

## 6. 正确性边界

- response_task 必须回到原 poller。
- fd 只能由 owner poller 读写关闭。
- session refcount 跨 poller/executor 安全。
- 同 session response 顺序必须保持。

## 7. 性能影响预期

多 poller提升：

- 连接数。
- 网络 recv/send 能力。
- RESP parse 并发。
- pipeline 接收能力。

多 poller不提升：

- 单 executor store 执行吞吐。
- executor 内同步 OrtRun。
- HNSW 串行 search/insert。

因此多 poller应和 embedding worker pool 联合验证。

## 8. 风险点

- response 投错 poller。
- fd 关闭和 response 回投竞态。
- 多 poller 下 session refcount 错误。
- command queue 堆积。
- poller 唤醒丢事件。

## 9. 验证方式

- 多连接 c4/c8/c16。
- 慢客户端写回。
- 客户端断开时 executor 仍返回 response。
- pipeline 保序。
- poller response queue depth。
- command queue depth。

## 10. 回滚方式

保留单 poller 配置。多 poller 出问题时切回单 reactor poller + executor。

## 11. 下一阶段前置条件

进入 embedding prepare pool 前，多 poller 不是硬前置。若当前主要瓶颈仍是 OrtRun，可先做阶段 7，再回头扩展 poller。
