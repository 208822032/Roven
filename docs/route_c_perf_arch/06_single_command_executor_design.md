# Single Command Executor Design

## 1. 本阶段目标

阶段 4 的目标是实现 reactor-only 的单 command executor。第一版只把网络 handler 和 store execution 解耦，不做多 poller、不做 embedding worker pool、不删除锁。

## 2. 当前代码现状

`src/network/reactor.c::recv_cb()` 当前在 reactor event loop 中调用 `net_session_process_resp()`，后者同步执行 handler。handler 执行期间 reactor 无法处理其他连接事件。store 访问通过 `kvs_filter_protocol()` 统一加锁。

## 3. 设计方案

executor 生命周期：

- 在 `main()` 初始化 store、RDB/AOF load 后启动。
- reactor 收到完整命令后 build `command_task`。
- task 投递到 command queue。
- executor thread 串行取 task，调用现有 `kvs_filter_protocol()`。
- executor 生成 `response_task`。
- response 回到 reactor 线程，追加到 session send queue。
- shutdown 时 stop accept，drain queue，flush response，停止 executor。

queue：

- 第一版单 reactor 生产者 + 单 executor 消费者，SPSC 足够。
- 接口上可预留 MPSC 形态，便于阶段 6 多 poller。

session 生命周期：

- session 增加 refcount 和 closing 状态。
- task 创建时持有 session ref。
- 客户端断开时标记 closing，不立即释放有 in-flight task 的 session。
- response_task 回来时若 session closing，则丢弃 response 并 release。

每 session 单 in-flight：

- 第一版如果 session 有 in-flight task，暂停继续 parse 或停止 EPOLLIN。
- response 回写完成后恢复 EPOLLIN。
- 这样避免 pipeline response reorder。

executor 内仍保留：

- `kvs_filter_protocol()`。
- `g_kv_data_rwlock`。
- GET lazy expire read-then-write retry。

## 4. 不修改范围

第一版不支持：

- proactor。
- ntyco。
- 多 poller。
- embedding worker pool。
- 删除或弱化 `g_kv_data_rwlock`。
- RESP 协议变化。

## 5. 涉及模块和文件

- `src/network/reactor.c`
- `include/net_session.h`
- `src/network/net_session.c`
- `src/core/kvstore.c`
- `src/core/kvs_operation.c`
- 后续新增 executor 模块文件时需单独设计

## 6. 正确性边界

- executor 是唯一执行 command_task 的线程。
- 第一版仍可通过锁保护现有后台路径。
- 同 session 单 in-flight 保证 response 顺序。
- replication context 必须随 task 传递。

## 7. 性能影响预期

收益：

- reactor event loop 不再被 store handler 长时间阻塞。
- 网络接收和命令执行开始解耦。

限制：

- 单 executor 仍串行执行 store 和 embedding。
- VSET/VSEARCH QPS 不会因 executor 本身明显提升。

## 8. 风险点

- reactor 等待 response 的设计不当会退化为阻塞。
- session refcount 错误导致 use-after-free。
- queue drain 和 shutdown 死锁。
- pipeline 被单 in-flight 限制后吞吐变化。

## 9. 验证方式

- 功能：普通 KV、TTL、VSET/VSEARCH/VDEL、AOF/RDB。
- 网络：多客户端连接、断连、pipeline。
- 指标：command queue depth、executor task latency、response latency。
- 压测：c4/c8 下 event loop 是否仍可接受新连接。

## 10. 回滚方式

保留原 reactor 同步路径作为回滚点。若 executor 出现问题，可切回 `net_session_process_resp()` 直接同步调用 handler。

## 11. 下一阶段前置条件

进入后台任务 executor 化前，必须证明 reactor-only executor 下功能不变，session 生命周期稳定，shutdown drain 无死锁。

## 实现状态：阶段 4B reactor-only single command executor MVP

- 当前状态：已接入 reactor-only single command executor。
- 支持范围：reactor 模式、single executor、每 session 单 in-flight。
- command_task：async 路径使用 heap task + owned argv。
- response_task：async 路径使用 owned response buffer。
- response 回投：executor 通过 response queue + eventfd 唤醒 reactor，由 owner reactor append 到 send_buf。
- session 安全：reactor 连接扩展改为每连接 heap owner，response 回投使用 fd 槽位、session_id、generation、closing 和 owner 指针校验；断连或 fd 复用时 drop response。
- 保持不变：g_kv_data_rwlock、命令锁模型、RESP 格式、RDB/AOF 格式、主从协议。
- 同步 fallback：REPLSYNC 和已标记 replication connection 继续走 reactor 同步旧路径。
- 不支持范围：proactor/ntyco、多 poller、多 executor、pipeline 多 in-flight、response reorder、embedding worker pool。
- 已知限制：单 executor 串行执行所有 async 命令，慢 VSET/VSEARCH 会造成 executor 队头阻塞。

## 实现状态：阶段 4C reactor executor 稳定化与边界收口

- 当前状态：已完成阶段 4B 后的稳定化收口，不引入新的执行模型。
- accept 边界：reactor listen fd 继续使用 EPOLLET，`accept_cb` 已改为循环 accept 至 `EAGAIN/EWOULDBLOCK`，避免高并发短连接在 listen backlog 中积压。
- session refcount：`refcount` 仍通过 `net_session_ref_inc/ref_dec/ref_count` 访问，内部改为 GCC atomic builtin，收口 executor error path 理论竞态。
- executor stats：新增代码层 stats API，统计 submitted/completed、command/response queue depth 和 max depth、eventfd wakeup/write fail、response drop。
- response drop 观测：reactor response 回投 drop/append failed 时记录 executor drop stats；不新增 RESP 命令，不改变 response 格式。
- 保持不变：reactor-only、single executor、每 session 单 in-flight、REPLSYNC/replication 同步 fallback、g_kv_data_rwlock、RESP/RDB/AOF/主从协议。
- 不支持范围：proactor/ntyco executor、多 poller、多 executor、pipeline 多 in-flight、response reorder、dynamic response buffer、embedding worker pool。
- 阶段 5 前置：4C 只提供稳定边界和观测数据，async network adapter 抽象留到阶段 5。
