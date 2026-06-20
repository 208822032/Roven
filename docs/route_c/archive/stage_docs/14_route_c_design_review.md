# 路线 C 设计文档审批报告

## 1. 总体审批结论

CONDITIONALLY_APPROVED：方向正确，但必须先修正若干前置问题。

当前文档与代码主结构基本一致，路线 C 的长期所有权模型合理。但代码中确实存在 master full sync、slave apply、RDB save、AOF rewrite 等绕过 `g_kv_data_rwlock` 的 live store 访问路径。进入性能改造前，必须先做阶段 0.5 的绕锁路径修复，不能直接进入阶段 1。

## 2. 审批摘要

1. 文档对当前网络调用链判断准确：reactor/proactor/ntyco 均通过 `net_session_process_resp()` 同步调用 handler，再进入 `kvs_filter_protocol()`。
2. 文档对锁模型判断基本准确：主入口由 `command_table.lock_mode` 驱动读写锁，`GET/EXIST` 存在 lazy expire 读锁升级写锁重试。
3. TTL 理解准确：`expire_at_ms` 内嵌在普通 KV 节点中，TTL 是锁存在的重要原因，但不是唯一原因。
4. V 族命令理解准确：VSET 写锁、VSEARCH 读锁，embedding 当前在锁内，vector store/id_map/HNSW 依赖外层锁保护。
5. 持久化和复制审计准确：RDB save、AOF rewrite、master full sync、slave apply 均存在或包含不经过全局锁的 live state 访问。
6. 阶段 1 的 VSET/VSEARCH embedding 锁外 prepare 方向可行，但必须要求深拷贝输入、明确 embedding owner、进入写锁后重新检查 question+scope。
7. response 异步化风险没有低估：当前 response buffer 是 `send_buf` 尾部预留区，不能跨线程复用栈指针或 recv buffer token。
8. pipeline 顺序风险识别充分：第一版 single in-flight 是保守且合理的约束。
9. 性能收益表述克制：文档没有把 single executor 或多 poller 夸大为直接解决 OrtRun 瓶颈。
10. 推荐顺序应调整为硬性顺序：阶段 0.5 bug fix 完成并验证后，再进入阶段 1。

## 3. 当前文档与代码一致性检查

- 网络调用链是否一致：一致。`src/network/net_session.c` 中 `net_session_process_resp()` 解析完整 RESP 命令后同步调用 handler，并将 handler 响应追加到 `session->send_buf`。reactor、proactor、ntyco 的 session handler 都调用 `kvs_handler()`，即 `kvs_filter_protocol()`。
- 锁模型是否一致：基本一致。`src/core/kvstore.c` 定义 `g_kv_data_rwlock`，`kvs_filter_protocol()` 根据 `command_table.lock_mode` 加读锁或写锁，并处理 `KVS_CMD_RETRY_WITH_WRITE_LOCK`。需要注意 `TTL` 也是写锁，因为它会惰性删除过期 key。
- TTL 设计理解是否一致：一致。普通 KV 各后端节点包含 `expire_at_ms`，lazy expire 会删除节点，active expire 线程在 `ttl_scan_once()` 中持写锁扫描和删除。TTL 需要 executor 化，但删除锁的原因不仅是 TTL，还包括普通 KV、vector、HNSW、持久化、复制、shutdown 等共享状态。
- V 族命令理解是否一致：一致。`VSET/VDEL/VINDEX` 写锁，`VGET/VSEARCH/VINFO` 读锁；`kvs_vector_set()` 修改 store/id_map/index；`kvs_vector_search()` 读取 index 和 record。第一版 HNSW insert/search 应继续在锁内或 executor 内。
- 持久化理解是否一致：一致。`kvs_filter_protocol()` 在写命令成功后做 AOF append 和 TCP replication；`kvs_rdb_save()` 内部遍历普通 KV 和 vector，不持全局锁；AOF rewrite 使用 `aof_state.data_rwlock`，不是 `g_kv_data_rwlock`。
- 主从同步理解是否一致：一致。master full sync 线程直接遍历普通 KV；slave apply 线程直接调用底层 set/del/mod 和 TTL apply。长期应改为 snapshot_task 和 replicated_command_task。
- session 生命周期理解是否一致：一致。当前 session 没有 refcount/closing 状态，async response 前必须先补 session 引用计数、fd owner、关闭竞态和 response 回投策略。

## 4. 各阶段审批结果表

| 阶段 | 文档 | 审批状态 | 是否可实施 | 阻塞问题 | 建议 |
|---|---|---|---|---|---|
| 0 | `01_lock_and_shared_state_audit.md` | APPROVED | 是 | 无 | 可作为实现前审计基线 |
| 0.5 | `02_existing_lock_bypass_fix_design.md` | APPROVED | 是 | 无 | 必须先做，按路径独立提交 |
| 1 | `03_vector_embedding_prepare_outside_lock.md` | CONDITIONAL | 暂缓 | 阶段 0.5 未完成 | 0.5 验证后再做 |
| 2 | `04_command_task_design.md` | CONDITIONAL | 后续可做 | 需要先稳定 argv 生命周期规则 | 先同步 wrapper，零拷贝不得逃逸 |
| 3 | `05_response_task_design.md` | CONDITIONAL | 后续可做 | session 生命周期和 buffer owner 未实现 | 先同步 response_task，再异步 |
| 4 | `06_single_command_executor_design.md` | CONDITIONAL | 后续可做 | session refcount、queue drain、pipeline 顺序 | reactor-only 原型合理 |
| 5 | `07_background_tasks_executor_design.md` | CONDITIONAL | 后续可做 | snapshot 粒度和 executor pause 风险 | 分 TTL/AOF/RDB/replication 迁移 |
| 6 | `08_multi_poller_queue_design.md` | NEEDS_MORE_ANALYSIS | 中后期 | fd ownership、response 回投、关闭竞态 | 不作为 embedding 性能第一步 |
| 7 | `09_embedding_prepare_worker_pool_design.md` | CONDITIONAL | 后续可做 | OrtSession 并发、CPU oversubscription、prepared data owner | command/response/executor 稳定后做 |
| 8 | `10_rwlock_removal_criteria.md` | APPROVED | 最后做 | 无 | 标准完整，必须作为硬门槛 |
| 持久化/复制 | `11_persistence_replication_under_route_c.md` | CONDITIONAL | 后续可做 | snapshot 内存和复制延迟 | 纳入同一 commit 状态流是必要条件 |
| 测试 | `12_test_and_benchmark_plan.md` | CONDITIONAL | 可执行 | 需要补 queue/response 内存指标落地 | 作为每阶段验收清单 |
| 路线图 | `13_iteration_roadmap.md` | CONDITIONAL | 可执行 | 0.5 应升级为硬前置 | 推荐顺序正确，但措辞应更硬 |

## 5. 必须修正或补充的问题

### 问题 1：master full sync 无锁遍历 live store

- 问题描述：`slave_sync_thread()` 直接遍历 `global_array/global_rbtree/global_hash/global_skiptable` 并发送全量同步数据。
- 相关代码位置：`src/distributed/distributed.c:112`、`src/distributed/distributed.c:125`、`src/distributed/distributed.c:140`、`src/distributed/distributed.c:156`、`src/distributed/distributed.c:168`。
- 为什么影响路线 C：该线程可与网络 handler、TTL active expire、RDB/AOF 路径并发访问同一节点，删除或弱化全局锁前必须收敛。
- 建议如何修正：短期阶段 0.5 持 `g_kv_data_rwlock` 读锁遍历；长期改为 executor `snapshot_task` 生成一致快照后发送。
- 是否阻塞下一阶段：是，阻塞阶段 1。

### 问题 2：slave apply 直接修改底层 store

- 问题描述：`slave_connect_master()` 解码主节点消息后直接调用各后端 set/del/mod，并调用 TTL apply。
- 相关代码位置：`src/distributed/distributed.c:493`、`src/distributed/distributed.c:498` 到 `src/distributed/distributed.c:560`。
- 为什么影响路线 C：slave apply 绕过 `kvs_filter_protocol()` 和全局锁，可能与本地读、TTL 扫描、持久化并发冲突。
- 建议如何修正：短期阶段 0.5 对每条 apply 持写锁，或改走统一命令入口并设置复制上下文；长期封装为 `replicated_command_task` 进入 executor。
- 是否阻塞下一阶段：是，阻塞阶段 1。

### 问题 3：RDB save 函数内部不持全局锁

- 问题描述：`kvs_rdb_save()` 直接调用 `rdb_save_array/rbtree/hash/skiptable` 和 `rdb_save_vector_section()` 遍历 live state。
- 相关代码位置：`src/persistence/kvs_rdb.c:417`、`src/persistence/kvs_rdb.c:444` 到 `src/persistence/kvs_rdb.c:461`。
- 为什么影响路线 C：RDB 快照可能遍历被并发释放或修改的节点，且 vector section 也读取 `g_vector_state`。
- 建议如何修正：短期明确由调用方持读锁或提供 locked/unlocked API，避免 shutdown 已持锁时递归死锁；长期通过 executor snapshot_task。
- 是否阻塞下一阶段：是，阻塞阶段 1。

### 问题 4：AOF rewrite 的保护锁不是全局 store 锁

- 问题描述：AOF rewrite 使用 `aof_state.data_rwlock` 和原子读取，但写命令主路径并不以该锁保护全局 store，vector rewrite 也直接遍历 `g_vector_state.store`。
- 相关代码位置：`src/persistence/kvs_aof_rewrite.c:41` 到 `src/persistence/kvs_aof_rewrite.c:80`、`src/persistence/kvs_aof_rewrite.c:234` 到 `src/persistence/kvs_aof_rewrite.c:260`、`src/persistence/kvs_aof_rewrite.c:271` 到 `src/persistence/kvs_aof_rewrite.c:304`。
- 为什么影响路线 C：rewrite 不能作为一致性快照边界，且可能和 vector insert/delete 并发。
- 建议如何修正：短期持全局读锁生成 rewrite 输入，或先复制 snapshot 后锁外写；长期由 executor snapshot_task 产生 rewrite payload。
- 是否阻塞下一阶段：是，阻塞阶段 1。

### 问题 5：异步 response 前 session 生命周期缺失

- 问题描述：当前 `net_session_t` 只有 recv/send buffer、parser、processing、first_packet_checked，没有 refcount、closing、owner poller、seq 等字段。
- 相关代码位置：`include/net_session.h`、`src/network/net_session.c:14` 到 `src/network/net_session.c:47`、`src/network/net_session.c:157` 到 `src/network/net_session.c:180`。
- 为什么影响路线 C：executor 或 worker 异步返回 response 时，原 fd/session 可能已经关闭释放，直接回写会 use-after-free 或投错 fd。
- 建议如何修正：阶段 3/4 前补 session refcount、closing、seq、owner poller、response 丢弃规则和 drain 规则。
- 是否阻塞下一阶段：不阻塞阶段 0.5/1，阻塞阶段 4 及之后。

### 问题 6：VSET prepare 后必须二次检查并管理未使用 embedding

- 问题描述：当前 `kvs_vector_set()` 在写锁内查重，存在则只更新 answer，不重建 embedding。
- 相关代码位置：`src/vector/kvs_vector.c:1057` 到 `src/vector/kvs_vector.c:1098`。
- 为什么影响路线 C：锁外 prepare 后，到 commit 时同 question+scope 可能已被其他命令插入或删除；不二次检查会改变覆盖语义或泄漏 prepared embedding。
- 建议如何修正：阶段 1 进入写锁后重新查 `question+scope`；已存在则只更新 answer 并释放 prepared embedding；新建路径才把 prepared embedding 转移给 record。
- 是否阻塞下一阶段：不阻塞阶段 0.5，阻塞阶段 1 实现。

## 6. 当前最小下一步审批

当前不应该直接进入阶段 1：VSET/VSEARCH embedding 锁外 prepare。

必须先做阶段 0.5：绕锁路径修复。

推荐顺序：

1. 第一步：阶段 0.5，修复 master full sync、slave apply、RDB save、AOF rewrite 的全局锁或 snapshot 边界。
2. 第二步：阶段 1，做 VSET/VSEARCH embedding 锁外 prepare，保留 HNSW insert/search 在锁内。
3. 第三步：阶段 2/3，同步 command_task 和 response_task，建立 argv/response 生命周期边界。

## 7. 对阶段 0.5 的审批

- master full sync 是否应先拿读锁或改 snapshot：是。短期拿 `g_kv_data_rwlock` 读锁是最小 bug fix；长期 snapshot_task 更合理。
- slave apply 是否应先拿写锁或改 task：是。短期每条 apply 拿写锁，或通过统一命令入口；长期必须改为 replicated_command_task。
- RDB/AOF rewrite 是否应先补锁或只记录风险：应先补锁或补 snapshot 边界，不能只记录风险后继续做性能改造。
- 这些是否属于 bug fix，而不是路线 C 大重构：是。这些路径当前已经可能和网络 handler/TTL 并发访问 live store，修复属于稳定性 bug fix。

阶段 0.5 审批状态：APPROVED，且是阶段 1 硬前置。

## 8. 对阶段 1 的审批

- VSET embedding 锁外 prepare 是否安全：有条件安全。prepare 只能依赖 request 文本和 embedding runtime，不能访问 store/HNSW。
- VSEARCH embedding 锁外 prepare 是否安全：有条件安全。query embedding 可锁外生成，HNSW search 仍在读锁内执行。
- 是否必须深拷贝 question/answer/scope：必须。当前 tokens 指向 session recv buffer，阶段 1 即使同步拆分，也应按后续异步边界设计 owner。
- 是否必须重新检查 question+scope：必须。VSET commit 前要重查当前 store。
- HNSW insert/search 是否仍在锁内：是。第一版必须保留在写锁/读锁内，长期放入 single commit executor。
- 是否能独立提交：可以，但只能在阶段 0.5 完成后提交。
- 需要哪些测试：VSET/VGET/VSEARCH/VDEL、重复 VSET 覆盖 answer、scoped search、VDEL 后搜索过滤、embedding 错误路径、泄漏检测、c4/c8 benchmark、`KVLITE_EMBEDDING_PROFILE=1`。

阶段 1 审批状态：CONDITIONAL。

## 9. 对长期路线 C 的审批

长期目标总体合理：

- 多 network poller：合理，但不应早于 session/refcount/response 回投稳定。
- embedding prepare worker pool：合理，是提升 ONNX 吞吐的核心路径。
- single commit executor：合理，能替代全局锁成为 store/TTL/vector/HNSW/AOF/replication 状态 owner。
- background task executor 化：合理且必要，是去锁硬前置。
- persistence / replication 纳入同一状态流：合理且必要。
- 最终删除或弱化 `g_kv_data_rwlock`：合理，但只能在所有 live state 访问收敛后做。

风险过高或需要调整顺序的目标：

- 多 poller 不应早于 response_task/session 生命周期。
- embedding worker pool 不应早于 command_task/response_task/executor 基础设施。
- per-thread OrtSession 不应作为第一步，只能在共享 session 并发 Run 数据证明不足后再评估。
- 删除锁必须最后做，并保留 executor 内继续持锁的回滚开关。

## 10. 性能风险审批

- GET/SET 是否可能因 executor queue 降低性能：可能。轻命令经过队列、跨线程唤醒和 response 回投后，p50/p99 都可能退化。
- VSET/VSEARCH 是否可能因 embedding pool 提升性能：可能。OrtRun 是毫秒级主耗时，worker pool 有机会提升多连接吞吐，但受 ORT intra threads、CPU 核数、HNSW search/insert 串行段影响。
- 是否应该保留轻命令快速路径：建议保留或至少用配置开关保留单线程同步路径，直到 executor 的轻命令指标达标。
- 是否应该考虑 pipeline batch：应该。pipeline 可降低 queue/wakeup 成本，但第一版可用单 session 单 in-flight 保序，后续再做同 session batch。
- response buffer 是否存在内存膨胀风险：存在。当前每条命令预留 `KVS_MAX_RESPONSE_LENGTH`，异步化后若每个 response_task 固定分配大 buffer，会造成高并发内存峰值。
- 推荐的性能北极星指标：VSEARCH/VSET c4/c8/c16 QPS 与 p99、`embedding_encode max_inflight`、GET/SET p99 退化比例、command queue depth、prepare queue depth、commit latency p99、response memory bytes。

## 11. 最终建议

- 是否建议提交当前 doc 文档：是，可以提交为架构设计基线。
- 是否建议先补充文档再提交：建议新增本审批报告后提交；原文档无需立即修改。
- 是否建议进入实现：建议进入实现，但第一项只能是阶段 0.5 绕锁路径修复。
- 如果进入实现，第一条 Codex 实现任务应该是：实现阶段 0.5 的最小锁修复，先为 master full sync 和 slave apply 补 `g_kv_data_rwlock` 读/写锁，再处理 RDB save/AOF rewrite 的 snapshot 或读锁边界，并补对应并发测试。

## 12. 建议 commit message

如果只新增审批报告，建议：

```bash
git add doc/route_c_perf_arch/14_route_c_design_review.md
git commit -m "补充路线C架构设计审批报告"
```

如果同时提交整套设计文档和审批报告，建议：

```bash
git add doc/route_c_perf_arch
git commit -m "新增路线C性能架构设计与审批报告"
```
