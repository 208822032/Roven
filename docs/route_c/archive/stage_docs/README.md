# Route C Performance Architecture Documentation

## 1. 本阶段目标

本文档集定义 KVLite ONNX embedding 语义缓存的路线 C 架构优化方案：

```text
多网络队列 + embedding prepare pool + 串行 commit executor
```

核心判断：

```text
锁不是被直接删除的，而是被新的状态所有权模型替代。
```

路线 C 的目标是把网络收发、ONNX embedding 计算、store/HNSW/TTL/AOF/RDB/replication 状态提交分层，先保证一致性边界清晰，再逐步验证性能收益。

## 2. 当前代码现状

当前 `src/network/reactor.c`、`src/network/proactor.c`、`src/network/ntyco.c` 都通过 `net_session_process_resp()` 同步调用网络 session handler，再调用 `kvs_filter_protocol()`。`kvs_filter_protocol()` 根据 `src/core/kvs_operation.c` 中的 `command_table` 获取 `lock_mode`，持有 `g_kv_data_rwlock` 后同步执行 handler。

已知性能结论：

- `reactor` 是单 epoll loop，同步执行 handler。
- `proactor` 是单 io_uring completion loop，handler 仍同步执行。
- `ntyco` 当前连接协程中使用阻塞 `poll/recv/send`，并发测试会卡住。
- 三种模式下 `max_inflight=1`，没有多个请求并发进入 `embedding_encode`。
- `VSET` 持写锁，`VSEARCH` 持读锁，embedding 生成发生在锁内。

## 3. 设计方案

阅读顺序：

1. `00_route_c_overview.md`
2. `01_lock_and_shared_state_audit.md`
3. `02_existing_lock_bypass_fix_design.md`
4. `03_vector_embedding_prepare_outside_lock.md`
5. `04_command_task_design.md`
6. `05_response_task_design.md`
7. `06_single_command_executor_design.md`
8. `07_background_tasks_executor_design.md`
9. `08_multi_poller_queue_design.md`
10. `09_embedding_prepare_worker_pool_design.md`
11. `10_rwlock_removal_criteria.md`
12. `11_persistence_replication_under_route_c.md`
13. `12_test_and_benchmark_plan.md`
14. `13_iteration_roadmap.md`

阶段总表：

| 阶段 | 文档 | 目标 | 当前建议 |
|---|---|---|---|
| 0 | `01_lock_and_shared_state_audit.md` | 锁点和共享状态地图 | 先做 |
| 0.5 | `02_existing_lock_bypass_fix_design.md` | 修复现有绕锁风险 | 先做 |
| 1 | `03_vector_embedding_prepare_outside_lock.md` | VSET/VSEARCH embedding 锁外 prepare | 第一项性能改造 |
| 2 | `04_command_task_design.md` | 抽象 command_task，仍同步执行 | 阶段 1 后做 |
| 3 | `05_response_task_design.md` | 抽象 response_task，仍同步回写 | 阶段 2 后做 |
| 4 | `06_single_command_executor_design.md` | reactor-only 单 executor | 不直接跳到此阶段 |
| 5 | `07_background_tasks_executor_design.md` | 后台任务 executor 化 | 删除锁前硬前置 |
| 6 | `08_multi_poller_queue_design.md` | 多 poller / 多队列 | 中后期 |
| 7 | `09_embedding_prepare_worker_pool_design.md` | embedding worker pool | 路线 C 性能核心 |
| 8 | `10_rwlock_removal_criteria.md` | 删除或弱化 rwlock 的判定标准 | 最后 |

推荐最小下一步：

```text
先做阶段 0 和阶段 0.5，确认现有绕锁风险；
随后做阶段 1：VSET/VSEARCH embedding 移出锁外。
```

## 4. 不修改范围

当前不要做：

- 不直接删除 `g_kv_data_rwlock`。
- 不直接做多 poller。
- 不直接做 per-thread OrtSession。
- 不直接做 batch embedding。
- 不直接做异步队列。
- 不修改 `include/embedding_c_api.h`。
- 不修改 RESP 协议。
- 不跳过阶段 0/0.5 直接实现 single executor。

## 5. 涉及模块和文件

- 网络入口：`src/network/reactor.c`、`src/network/proactor.c`、`src/network/ntyco.c`
- session 解析：`include/net_session.h`、`src/network/net_session.c`
- 命令入口：`src/core/kvstore.c`、`src/core/kvs_operation.c`
- 命令表：`include/kvs_command.h`、`src/core/kvs_operation.c`
- TTL：`src/core/kvs_ttl_background.c`
- vector：`src/vector/kvs_vector.c`、`src/vector/kvs_vector_index.c`、`src/vector/kvs_vector_embedding.c`
- embedding：`src/embedding/embedding_runtime_onnx.c`
- 持久化：`src/persistence/kvs_aof.c`、`src/persistence/kvs_aof_rewrite.c`、`src/persistence/kvs_rdb.c`
- 主从同步：`src/distributed/distributed.c`

## 6. 正确性边界

路线 C 的正确性边界是：

- 网络线程不访问 store。
- embedding worker 不访问 store/HNSW/TTL。
- commit executor 是 store/TTL/vector/HNSW/AOF/RDB/replication 状态的唯一 owner。
- 单连接命令和 response 必须保序。
- 不同连接之间不承诺按网络到达时间全局保序，commit executor 的执行顺序定义全局状态顺序。

## 7. 性能影响预期

预期收益：

- 阶段 1 缩短 `g_kv_data_rwlock` 持有时间，验证 `max_inflight > 1` 的可能性。
- 阶段 6 提升网络收发和 RESP parse 能力。
- 阶段 7 让 ONNX OrtRun 并发，观察 VSET/VSEARCH c4/c8 QPS 是否提升。

不应期待：

- 单 command executor 本身不会提升 store 串行吞吐。
- 多 poller 本身不会解决 OrtRun 串行瓶颈。
- per-thread OrtSession 和 batch embedding 只有在 `max_inflight > 1` 后才有判断价值。

## 8. 风险点

- session 生命周期和异步 response 释放时机。
- command_task argv 深拷贝和内存归属。
- 同 session pipeline response 乱序。
- prepare worker 堆积导致延迟放大。
- executor queue 堆积导致全局状态提交延迟。
- RDB/AOF/replication 未纳入同一状态流导致不一致。

## 9. 验证方式

基础功能：

- `tests/vector_testcase`
- `tests/vector/semantic_onnx_test.sh`
- TTL 相关 shell 测试
- RDB/AOF/replication 测试

性能：

- `tests/vector_benchmark --mode latency`
- `tests/vector_benchmark --mode throughput -c 4/-c 8/-c 16`
- `KVLITE_EMBEDDING_PROFILE=1` 的 `max_inflight`
- 后续新增 `command_queue_depth`、`prepare_queue_depth`、`commit_latency_p99`

## 10. 回滚方式

每个阶段必须能独立回滚：

- 阶段 1 可回滚到 embedding 仍在锁内。
- 阶段 2/3 可回滚到直接调用 `kvs_filter_protocol()`。
- 阶段 4 可回滚到 reactor 同步 handler。
- 阶段 5 可回滚到后台线程直接访问 store，但必须保留阶段 0.5 的锁修复。
- 阶段 6/7 可通过配置或编译开关回退到单 poller/同步 prepare。

## 11. 下一阶段前置条件

进入实现前必须完成：

- `01_lock_and_shared_state_audit.md` 中的访问路径确认。
- `02_existing_lock_bypass_fix_design.md` 中的绕锁风险修复设计评审。
- `03_vector_embedding_prepare_outside_lock.md` 的 VSET/VSEARCH 语义边界确认。
