# Embedding Prepare Worker Pool Design

## 1. 本阶段目标

阶段 7 的目标是引入 embedding prepare worker pool，让 VSET/VSEARCH 的 ONNX OrtRun 并发执行，同时保持 store/HNSW/TTL/AOF 提交由 single commit executor 串行完成。

## 2. 当前代码现状

当前 `kvs_vector_embedding_generate()` 最终调用 `embedding_encode()`。已有 profile 显示 tokenizer 微秒级，OrtRun 毫秒级，是主要耗时。三种 network_mode 下 `max_inflight=1`。

## 3. 设计方案

worker pool：

- 第一版 worker_count=1 验证流程。
- 第二版扩到 2/4/8，观察 `max_inflight` 和 QPS。
- prepare worker 不访问 store/HNSW/TTL。
- prepare worker 只调用 embedding。

prepare queue：

- command_task 需要 prepare 时进入 prepare queue。
- worker 输出 prepared_task。
- prepared_task 回到 commit executor。

VSET：

```text
parse VSET
  -> copy question/answer/scope
  -> prepare embedding
  -> executor re-check question+scope
  -> insert/update record/id_map/HNSW
  -> AOF/replication after commit
```

VSEARCH：

```text
parse VSEARCH
  -> copy query/scope/topk/threshold
  -> prepare query embedding
  -> executor HNSW search
  -> build response
```

顺序策略第一版：

- 每 session 单 in-flight。
- 不同 session 允许 ready commit。
- commit executor 的执行顺序定义全局状态顺序。
- 不做全局 reorder buffer。

OrtSession 分析：

- ONNX Runtime 通常允许同一 session 并发 Run，但实际性能受 intra_threads、allocator、CPU 竞争影响。
- 如果 `max_inflight > 1` 但 QPS 不提升，再考虑 per-thread OrtSession。
- per-thread OrtSession 会增加模型内存和初始化成本。
- worker_count 与 `embedding_num_threads` 必须配合，避免 `worker_count * intra_threads` 过度抢占 CPU。
- 多 session + `intra_threads=1` 可能更适合吞吐，但不是第一步。

## 4. 不修改范围

阶段 7 不做：

- 不访问 store/HNSW/TTL 于 worker。
- 不改 `include/embedding_c_api.h`。
- 不做 batch embedding。
- 不直接引入 per-thread OrtSession。
- 不改变 VSET/VSEARCH RESP 语义。

## 5. 涉及模块和文件

- `src/vector/kvs_vector_embedding.c`
- `src/embedding/embedding_runtime_onnx.c`
- command_task 模块
- executor 模块
- response_task 模块

## 6. 正确性边界

- prepare 阶段不可见。
- commit 阶段重新检查当前 store 状态。
- 同 session 命令保序。
- prepared embedding owner 必须明确。
- commit 失败释放 prepared data。

## 7. 性能影响预期

目标指标：

- `max_inflight > 1`。
- VSET/VSEARCH c4/c8 QPS 提升。
- `prepare_queue_depth` 不持续增长。
- `commit_latency_p99` 不恶化。
- CPU 利用率更均衡。

## 8. 风险点

- prepare 乱序导致语义变化。
- 慢 embedding 阻塞同 session 后续命令。
- worker 过多导致 ORT intra threads 竞争。
- prepared data 泄漏。
- executor 被 HNSW search 阻塞。

## 9. 验证方式

- 单 worker 流程验证。
- 多 worker c4/c8/c16。
- `KVLITE_EMBEDDING_PROFILE=1` 观察 `max_inflight`。
- 记录 prepare queue depth、commit latency p99。
- 同 key VSET/VDEL/VSEARCH 混合正确性测试。

## 10. 回滚方式

保留同步 embedding prepare 路径。worker pool 出问题时切回 executor 内或阶段 1 的锁外同步 prepare。

## 11. 下一阶段前置条件

只有当 command_task、response_task、executor、session refcount 稳定后，才能引入 worker pool。
