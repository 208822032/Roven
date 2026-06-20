# Route C Iteration Roadmap

## 1. 本阶段目标

给出路线 C 的最终迭代路线图，保证每一步都可单独 commit、可验证、可回滚。

## 2. 当前代码现状

当前系统仍是同步 handler 模型：

- network poller 同步执行 handler。
- `kvs_filter_protocol()` 统一加锁。
- embedding 在锁内。
- 后台任务存在绕锁路径。
- 三种 network_mode 下 `max_inflight=1`。

## 3. 设计方案

| 阶段 | 文档 | 目标 | 是否改代码 | 是否影响性能 | 是否可独立提交 | 推荐 commit message |
|---|---|---|---|---|---|---|
| 0 | `01_lock_and_shared_state_audit.md` | 锁点和共享状态地图 | 否 | 否 | 是 | `docs: audit shared state for route C` |
| 0.5 | `02_existing_lock_bypass_fix_design.md` | 绕锁路径修复 | 是 | 可能轻微影响后台路径 | 是 | `Fix store lock bypass in background paths` |
| 1 | `03_vector_embedding_prepare_outside_lock.md` | VSET/VSEARCH embedding 锁外 prepare | 是 | 预期改善锁持有时间 | 是 | `Move vector embedding prepare outside data lock` |
| 2 | `04_command_task_design.md` | command_task 抽象 | 是 | 轻微开销 | 是 | `Introduce synchronous command task abstraction` |
| 3 | `05_response_task_design.md` | response_task 抽象 | 是 | 轻微开销 | 是 | `Introduce response task abstraction` |
| 4 | `06_single_command_executor_design.md` | reactor-only single command executor | 是 | 解耦网络和执行 | 是 | `Add reactor command executor prototype` |
| 5 | `07_background_tasks_executor_design.md` | 后台任务 executor 化 | 是 | 后台路径变化 | 分模块提交 | `Route background store access through executor` |
| 6 | `08_multi_poller_queue_design.md` | 多 poller / 多队列 | 是 | 提升网络并发 | 是 | `Add multi-poller command queue architecture` |
| 7 | `09_embedding_prepare_worker_pool_design.md` | embedding prepare worker pool | 是 | 预期提升 ONNX 吞吐 | 是 | `Add embedding prepare worker stage` |
| 8 | `10_rwlock_removal_criteria.md` | 删除或弱化 rwlock | 是 | 降低锁开销 | 是，但最后做 | `Remove global data rwlock after executor ownership` |

推荐当前第一步：

```text
先做阶段 0 和阶段 0.5，确认现有绕锁风险；
随后做阶段 1：VSET/VSEARCH embedding 移出锁外。
```

不要直接从阶段 4 或阶段 7 开始。

## 4. 不修改范围

路线图本身不修改代码。执行阶段时也必须遵守：

- 不跳阶段删除锁。
- 不跳阶段做多 poller。
- 不在未验证 `max_inflight > 1` 前做 per-thread OrtSession。
- 不在 command/response 生命周期未稳定前做异步队列。

## 5. 涉及模块和文件

所有阶段最终会涉及：

- `src/network/*`
- `include/net_session.h`
- `src/core/kvstore.c`
- `src/core/kvs_operation.c`
- `src/core/kvs_ttl_background.c`
- `src/vector/*`
- `src/embedding/embedding_runtime_onnx.c`
- `src/persistence/*`
- `src/distributed/distributed.c`

## 6. 正确性边界

每个阶段必须独立保持：

- RESP 兼容。
- 单 session response 顺序。
- TTL 语义。
- VSET/VSEARCH/VDEL 语义。
- AOF/RDB/replication 一致性。

## 7. 性能影响预期

阶段 1 和阶段 7 是主要 ONNX 性能收益点。阶段 4/6 是网络和执行模型解耦点。阶段 8 是所有权模型完成后的清理点，不是性能优化起点。

## 8. 风险点

- 过早扩大范围导致无法定位回归。
- 未修复绕锁路径就引入 executor，数据竞争仍存在。
- 未定义 session 生命周期就异步 response，容易 use-after-free。

## 9. 验证方式

每个阶段必须有：

- 功能测试。
- 性能对比。
- 稳定性压测。
- 回滚验证。
- 明确的 commit message。

## 10. 回滚方式

每个阶段单独 commit。回滚时只回滚当前阶段，不回滚前置稳定性修复，除非该修复本身引入错误。

## 11. 下一阶段前置条件

进入下一阶段前必须完成当前阶段验收指标，并更新阶段测试记录。
