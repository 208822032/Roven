# 阶段 4C：reactor executor 稳定化与边界收口

## 1. 目标

阶段 4C 只收口阶段 4B 审查中的非阻塞风险，为阶段 5 async network adapter 抽象准备更稳定的边界。4C 不引入新执行模型，不进入阶段 5。

本阶段目标：

- 修复 reactor EPOLLET 下 `accept_cb` 单次 accept 的高并发连接积压风险。
- 将 session `refcount` 的增减和读取收口到 atomic helper，覆盖 executor error path 的理论竞态。
- 增加 single command executor 轻量观测指标，观察队列深度、eventfd wakeup 和 response drop。
- 保持单 executor 队头阻塞为已知限制，只观测，不解决。

## 2. 背景：阶段 4B 非阻塞风险

阶段 4B 已完成 reactor-only single command executor MVP，并通过 ASAN/UBSAN/TSAN、semantic、pipeline、断连、fd reuse 和 benchmark。审查结论为 CONDITIONALLY_APPROVED，非阻塞建议集中在：

1. `set_event` 全局加入 EPOLLET 后，listen fd 的 `accept_cb` 必须循环 accept 至 EAGAIN。
2. `refcount` 是普通 int，正常路径安全，但 executor error path 理论上可能跨线程 release。
3. executor 缺少轻量 stats，不利于判断队头阻塞、queue 积压和 response drop。
4. 单 executor 队头阻塞是 MVP 限制，本阶段不做多 executor。

## 3. accept_cb EPOLLET 修复

`accept_cb` 已从单次 `accept()` 改为循环：

- 成功 accept 后继续处理下一个连接。
- 每个新连接仍检查 `CONNECTION_SIZE`。
- 每个新连接仍调用原 `event_register`，完成 conn extension 初始化、session identity/generation 设置、nonblocking、TCP_NODELAY 和 socket buffer 设置。
- `EINTR` 继续循环。
- `EAGAIN/EWOULDBLOCK` 表示本轮 accept drain 完成。
- 其他错误按当前日志风格记录并返回。

listen fd 仍保留 EPOLLET。

## 4. session refcount 原子化 / 边界收口

`net_session_t.refcount` 字段仍保留在 session 内，调用方不裸读写。阶段 4C 将 helper 内部改为 GCC atomic builtin：

- 初始化：`__atomic_store_n(..., 1, __ATOMIC_RELAXED)`
- inc：`__atomic_add_fetch(..., __ATOMIC_RELAXED)`
- dec：CAS 循环，成功时使用 `__ATOMIC_ACQ_REL`
- read：`__atomic_load_n(..., __ATOMIC_ACQUIRE)`

`closing` 和 `single_inflight` 未 atomic 化，它们仍由 reactor 线程维护。response 回投仍按 fd slot owner、session_id、generation、closing 校验后 append/drop。

## 5. executor 观测指标

新增 `kvs_command_executor_stats_t` 和代码层 API：

- `kvs_command_executor_get_stats`
- `kvs_command_executor_record_response_drop`

当前未新增 RESP 命令，也未改变 response 格式。stats 仅用于代码层读取和 shutdown info 日志，后续可在阶段 5 或独立 debug/info 命令中接出。

核心统计：

- submitted_tasks
- completed_tasks
- submit_failed
- command_queue_depth
- command_queue_max_depth
- response_queue_depth
- response_queue_max_depth
- pushed_responses
- polled_responses
- eventfd_wakeup_count
- eventfd_write_fail_count
- dropped_responses
- session_mismatch_drop
- session_closing_drop
- response_append_failed

## 6. 队列深度与 drop 统计

command queue：

- submit 成功时 depth++，更新 max depth。
- executor pop 时 depth--。
- stop drain 时按释放数量递减。
- stop 后 submit 失败并统计 submit_failed。
- 入队前清空 `queue_next`。

response queue：

- push response 时 depth++，更新 max depth。
- reactor poll response 时 depth--。
- stop drain 时按释放数量递减。
- push 后通过 eventfd 唤醒 reactor，记录 wakeup/write fail。

response drop：

- fd slot owner/session_id/generation 不匹配记录 session mismatch drop。
- session 已 closing 记录 closing drop。
- response length 无效或 append reserve 失败记录 append failed。

## 7. 保持不变的边界

阶段 4C 保持以下边界不变：

- 不支持 proactor executor。
- 不支持 ntyco executor。
- 不支持多 poller。
- 不支持多 executor。
- 不支持 pipeline 多 in-flight。
- 不支持 response reorder。
- 不支持 dynamic response buffer。
- 不支持 embedding worker pool。
- 不删除或弱化 `g_kv_data_rwlock`。
- 不改变 `command_table.lock_mode`。
- 不改变 RESP 协议、response 格式、RDB/AOF 文件格式、主从协议。
- REPLSYNC 和已标记 replication connection 继续走 reactor 同步 fallback。

## 8. 验证结果

- `make clean && make embedding`：通过。仍有既有 `kvs_vector_rebuild_after_load` 隐式声明 warning，非 4C 引入。
- ONNX semantic test：通过。干净库分值保持 0.8028 / 0.7200 / 0.7751。
- 普通命令回归：通过，覆盖 SET/GET/DEL、EXPIRE/TTL/PERSIST、VSET/VGET/VSEARCH/VDEL、VINDEX REBUILD、scope 隔离、repeat VSET、unknown command、wrong args、pipeline 顺序。
- high accept：通过，200/500 并发短连接均成功；500 快速串行连接也成功。
- disconnect/fd reuse：通过，VSEARCH 后立即断连 200 次、concurrent disconnect 5x30 均无 crash、无 bad response。
- ASAN/UBSAN：功能路径通过 semantic、pipeline、high accept、disconnect/fd reuse；未见 UAF/double-free/stack-use-after-return/UBSAN。LeakSanitizer 在退出时仍报告少量既有存储/复制/vector 生命周期泄漏，并夹带 shutdown 时 async 栈的 indirect leak；本轮不修复存储/vector AOF rewrite/load 等既有清理问题。
- TSAN：功能路径通过 semantic、pipeline、high accept、disconnect/fd reuse、VSET/VSEARCH 小样本；未见 refcount/command queue/response queue/stats 相关 data race。报告项来自 ONNX Runtime 内部线程、既有 `g_shutdown_flag` 信号处理、TTL background stop、distributed shutdown。
- benchmark：通过。latency vset 200：qps 426.89 avg 2.34ms；latency vsearch 200：qps 656.99 avg 1.52ms；throughput c=4/P=8 vset 1000：qps 413.21 avg 76.04ms；throughput c=4/P=8 vsearch 1000：qps 656.75 avg 47.97ms。

executor stats 观察：

- normal 压测后 shutdown stats：submitted=7704 completed=7704 command_queue_max_depth=8 response_queue_max_depth=45 dropped_responses=28 eventfd_write_fail_count=0。
- ASAN 干净库网络压力 stats：submitted=533 completed=533 command_queue_max_depth=24 response_queue_max_depth=49 dropped_responses=0 eventfd_write_fail_count=0。
- TSAN 小样本 stats：submitted=296 completed=296 command_queue_max_depth=2 response_queue_max_depth=25 dropped_responses=0 eventfd_write_fail_count=0。

内存泄漏复盘见 `22_stage4c_lsan_leak_review.md`。

## 9. 已知限制

- 仍仅 reactor executor。
- 仍是单 executor。
- 仍每 session 单 in-flight。
- 仍不支持 pipeline 并行。
- 仍不支持 response reorder。
- 仍不支持 proactor/ntyco executor。
- 单 executor 队头阻塞仍存在，阶段 4C 只提供 queue depth/max depth 和 completed/submitted stats 辅助观察。

## 10. 下一阶段建议：阶段 5 async network adapter 抽象

阶段 5 再考虑 async network adapter 抽象。进入阶段 5 前，应继续使用阶段 4C 的 stats 验证：

- high accept 下 listen backlog 不积压。
- fd reuse/drop 无旧 response 写入新连接。
- command_queue_max_depth 能反映 VSET/VSEARCH 下的单 executor 队头阻塞。
- TSAN 不报告 refcount/queue/stats 相关 data race。
