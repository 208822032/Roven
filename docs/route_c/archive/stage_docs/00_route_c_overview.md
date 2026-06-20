# Route C Overall Architecture

## 1. 本阶段目标

定义路线 C 的总设计思路：将 KVLite 当前同步 handler 模型逐步演进为“网络并发、embedding 并发、状态串行提交”的分层架构。

最终目标：

```text
network poller
  -> RESP parse
  -> command_task
  -> classify/router
     -> no prepare -> commit executor
     -> prepare -> embedding worker pool -> prepared_task -> commit executor
  -> response_task
  -> original poller send
```

## 2. 当前代码现状

当前架构问题：

- `src/network/reactor.c`、`src/network/proactor.c`、`src/network/ntyco.c` 都通过 `net_session_process_resp()` 同步执行 handler。
- 网络层 handler 直接调用 `kvs_filter_protocol()`，进而直接访问 store。
- `kvs_filter_protocol()` 在 `src/core/kvstore.c` 内持有 `g_kv_data_rwlock` 后执行 `command_table` handler。
- `VSET` 和 `VSEARCH` 的 embedding 生成发生在锁内。
- `KVLITE_EMBEDDING_PROFILE=1` 观测到三种 network_mode 下 `max_inflight=1`。
- TTL 后台线程、RDB/AOF、主从同步等模块存在直接访问 store 的路径，其中部分路径不由 `kvs_filter_protocol()` 统一加锁。

## 3. 设计方案

三层所有权模型：

```text
网络层：socket / recv / send / RESP parse
计算层：embedding prepare，不访问 store
状态层：commit executor，唯一访问 store/TTL/HNSW/AOF/RDB/replication
```

职责拆分：

- network poller：负责连接和 RESP parse，不直接执行命令。
- command_task：承载完整命令、session 引用、seq、argv/lens、prepared data 指针。
- prepare worker：只做 ONNX embedding，不访问 store/HNSW/TTL。
- commit executor：串行执行状态变更和状态读取，生成 response_task。
- response_task：回到原 poller 发送，保证同 session 顺序。

## 4. 不修改范围

本阶段是设计总览，不实现任何功能。不修改：

- `g_kv_data_rwlock`
- 网络模型
- RESP 协议
- VSET/VSEARCH/HNSW/embedding 主链路
- `include/embedding_c_api.h`
- 持久化和复制逻辑

## 5. 涉及模块和文件

- 网络：`src/network/reactor.c`、`src/network/proactor.c`、`src/network/ntyco.c`
- session：`include/net_session.h`、`src/network/net_session.c`
- 命令：`src/core/kvstore.c`、`src/core/kvs_operation.c`
- vector：`src/vector/kvs_vector.c`、`src/vector/kvs_vector_index.c`
- embedding：`src/vector/kvs_vector_embedding.c`、`src/embedding/embedding_runtime_onnx.c`
- TTL：`src/core/kvs_ttl_background.c`
- 持久化：`src/persistence/kvs_aof.c`、`src/persistence/kvs_aof_rewrite.c`、`src/persistence/kvs_rdb.c`
- 同步：`src/distributed/distributed.c`

## 6. 正确性边界

- 单连接内命令顺序必须保持。
- response 必须按同 session seq 返回。
- commit executor 的执行顺序定义全局状态顺序。
- prepare 阶段不产生任何可见状态。
- TTL / RDB / AOF / 主从同步必须能用 commit executor 顺序解释。

## 7. 性能影响预期

性能目标：

- 轻命令尽量不退化。
- VSET/VSEARCH 通过 embedding 并发提升。
- `max_inflight > 1`。
- `prepare_queue_depth` 不持续增长。
- `command_queue_depth` 不持续增长。
- `commit_latency_p99` 不恶化。

## 8. 风险点

- 过早引入多 poller 会放大 session 生命周期复杂度。
- embedding prepare 乱序完成可能改变命令语义。
- commit executor 内慢命令会阻塞所有状态提交。
- 持久化和主从同步如果没有纳入 executor，会继续存在状态不一致风险。

## 9. 验证方式

- 功能：普通 KV、TTL、VSET/VGET/VSEARCH/VDEL、RDB/AOF、主从同步。
- 性能：VSET/VSEARCH latency/QPS、c4/c8/c16、`max_inflight`。
- 稳定：长时间压测、断连、pipeline、大 value、大量过期 key。
- 正确性：同 session 顺序、VDEL 后 VSEARCH 不返回脏结果、AOF/RDB 恢复一致。

## 10. 回滚方式

路线 C 必须按阶段落地。任一阶段失败时回滚该阶段改动，不影响前置阶段。例如阶段 7 失败时可回到阶段 6 的同步 embedding；阶段 4 失败时可回到 reactor 同步 handler。

## 11. 下一阶段前置条件

开始任何实现前，必须完成锁点和共享状态审计，并先修复已知绕锁路径。推荐先阅读：

- `01_lock_and_shared_state_audit.md`
- `02_existing_lock_bypass_fix_design.md`
