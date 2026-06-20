# Test And Benchmark Plan

## 1. 本阶段目标

定义路线 C 每个阶段需要的功能、性能、稳定性和正确性验证体系。

## 2. 当前代码现状

已有测试和工具包括：

- `tests/vector_testcase`
- `tests/vector/semantic_onnx_test.sh`
- `tests/vector_benchmark`
- TTL shell 测试
- RDB/AOF/replication shell 测试

当前性能观测包括：

- `tests/vector_benchmark --mode latency`
- `tests/vector_benchmark --mode throughput`
- `KVLITE_EMBEDDING_PROFILE=1`

## 3. 设计方案

### 功能测试

- GET/SET/DEL。
- EXPIRE/TTL/PERSIST。
- lazy expire。
- active expire。
- VSET/VGET/VSEARCH/VDEL。
- RDB save/load。
- AOF append/rewrite/load。
- master full sync。
- slave apply。

### 性能测试

- VSET latency/QPS。
- VSEARCH latency/QPS。
- GET/SET latency/QPS。
- c4/c8/c16。
- pipeline。
- `max_inflight`。
- `prepare_queue_depth`。
- `command_queue_depth`。
- `commit_latency_p99`。
- `response_latency`。

### 稳定性测试

- 长时间压测。
- 客户端断开。
- pipeline。
- 大 value。
- 大量过期 key。
- AOF rewrite 同时写入。
- full sync 同时写入。
- slave apply 同时写入。

### 正确性测试

- 同 session 顺序。
- 不同 session commit 顺序可解释。
- VDEL 后 VSEARCH 不返回脏数据。
- TTL 到期后不返回。
- AOF/RDB 恢复后一致。
- 主从一致。

## 4. 不修改范围

本计划不新增测试代码，只定义后续阶段的验收标准。

## 5. 涉及模块和文件

- `tests/vector_benchmark.c`
- `tests/vector_testcase.c`
- `tests/vector/semantic_onnx_test.sh`
- TTL/RDB/AOF/replication shell tests
- future executor/queue/profile metrics

## 6. 正确性边界

每个阶段必须先通过功能和正确性测试，再看性能数据。性能提升不能以 VSET/VSEARCH/TTL/RDB/AOF/replication 语义退化为代价。

## 7. 性能影响预期

阶段期望：

- 阶段 1：锁持有时间下降，`max_inflight` 有机会提升。
- 阶段 4：网络线程阻塞减少，但 store 吞吐不一定提升。
- 阶段 6：网络 c4/c8/c16 收发能力提升。
- 阶段 7：ONNX 并发，VSET/VSEARCH QPS 提升。

## 8. 风险点

- benchmark 未 warmup 导致数据误判。
- RDB/AOF 残留文件污染结果。
- VSET 重复 key 导致没有真正触发 embedding。
- c4/c8 结果未结合 `max_inflight` 解读。

## 9. 验证方式

每次性能测试应记录：

- 配置文件。
- network_mode。
- embedding provider 和 ORT 参数。
- 是否清理 RDB/AOF。
- latency/QPS。
- `max_inflight`。
- queue depth。
- p95/p99。

## 10. 回滚方式

测试计划无代码修改。若某阶段指标退化，回滚该阶段实现并保留测试记录用于对比。

## 11. 下一阶段前置条件

进入任何性能优化阶段前，必须有可重复 benchmark 脚本和干净数据集初始化方式。
