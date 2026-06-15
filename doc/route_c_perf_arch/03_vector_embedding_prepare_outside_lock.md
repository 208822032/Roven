# Vector Embedding Prepare Outside Lock

## 1. 本阶段目标

阶段 1 的目标是把 `VSET` 和 `VSEARCH` 的 ONNX embedding 生成从 `g_kv_data_rwlock` 临界区中移出，先缩短锁持有时间，不改网络模型，不引入 worker pool。

## 2. 当前代码现状

当前调用链：

```text
net_session_process_resp
  -> *_session_handler
  -> kvs_filter_protocol
  -> g_kv_data_rwlock
  -> handle_vset_command / handle_vsearch_command
  -> kvs_vector_set / kvs_vector_search
  -> kvs_vector_embedding_generate
  -> embedding_encode / OrtRun
```

`VSET` 持写锁，`VSEARCH` 持读锁，OrtRun 在锁内执行。已有 profile 显示 OrtRun 是毫秒级主耗时，tokenizer 是微秒级。

## 3. 设计方案

### VSET

锁外准备：

- `question`
- `answer`
- `scope`
- normalized question
- embedding buffer

当前同步 handler 内 tokens 指向 `recv_buf`，在函数返回前可用。但为了向路线 C 兼容，设计上应倾向深拷贝跨阶段字段：

- `question`：后续异步 prepare 必须深拷贝。
- `answer`：后续 commit 需要，必须明确 owner。
- `scope`：可为空，非空时复制。
- embedding buffer：prepare 分配，commit 成功后转移给 vector record；失败或无需使用时释放。

进入写锁后重新检查：

- `question+scope` 是否已存在。
- 如果已存在，保持当前语义：只更新 answer，不重建 embedding。
- 如果被 `VDEL` 删除，则按当前 live store 状态插入新 record。

并发语义：

- 同 key 并发 VSET 以进入写锁 commit 的顺序为准。
- prepare 不产生可见状态。
- HNSW insert 仍在写锁内。

失败路径：

- embedding 失败：直接返回错误，不进入写锁。
- commit 插入/id_map/HNSW 失败：释放 prepared embedding 和复制字段。
- 覆盖已存在 record：释放未使用的 prepared embedding。

### VSEARCH

锁外准备：

- query embedding
- scope 参数

query embedding 只依赖请求文本和 embedding runtime，不访问 store，锁外安全。进入读锁后执行 HNSW search。搜索看到的是 search 时刻的数据，而不是 prepare 开始时刻的数据。这符合 KV 风格：命令的可见状态由执行/commit 时刻定义。

失败路径：

- embedding 失败：返回 vector error。
- HNSW search 失败：释放 query embedding 后返回错误。

## 4. 不修改范围

阶段 1 不做：

- 不引入 command_task。
- 不引入 worker pool。
- 不引入 executor。
- 不删除或弱化 `g_kv_data_rwlock`。
- 不修改 RESP 协议。
- 不修改 `include/embedding_c_api.h`。
- 不改 HNSW insert/search 算法。

## 5. 涉及模块和文件

- `src/core/kvs_operation.c`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_embedding.c`
- `src/vector/kvs_vector_index.c`
- `src/embedding/embedding_runtime_onnx.c`

## 6. 正确性边界

- prepare 阶段不得访问 store/HNSW。
- commit/search 阶段仍持现有锁。
- prepared embedding 不得在失败路径泄漏。
- VSET 重复 key 语义不变。
- VDEL 后 VSEARCH 不返回 deleted record，仍依赖 vector record active/deleted 过滤。

## 7. 性能影响预期

预期：

- 锁持有时间显著下降。
- 多线程或后续 executor 模型下 `embedding_encode max_inflight` 有机会大于 1。
- 单线程 reactor 下吞吐不一定提升，但锁外 prepare 是后续 worker pool 的前置拆分。

指标：

- VSET latency/QPS。
- VSEARCH latency/QPS。
- c4/c8 QPS。
- `max_inflight`。
- 锁持有时间，如果增加 profile。

## 8. 风险点

- VSET prepare 后发现 record 已存在，prepared embedding 需要释放。
- question/answer/scope 内存 owner 不清晰导致 use-after-free。
- commit 失败路径释放不完整。
- 同 key 并发语义变化。

## 9. 验证方式

功能：

- VSET/VGET/VSEARCH/VDEL。
- 重复 VSET 覆盖 answer。
- VDEL 后 VSEARCH 不返回旧数据。
- scoped VSET/VSEARCH。

性能：

- `tests/vector_benchmark --mode latency -t vset/vsearch`
- `tests/vector_benchmark --mode throughput -c 4/-c 8`
- `KVLITE_EMBEDDING_PROFILE=1` 观察 `max_inflight`

稳定：

- 大量 VSET + VDEL + VSEARCH 混合。
- embedding 错误路径。

## 10. 回滚方式

回滚到当前 `kvs_vector_set()` 和 `kvs_vector_search()` 内部生成 embedding 的同步模型。该阶段不改变外部协议，回滚风险低。

## 11. 下一阶段前置条件

阶段 1 进入阶段 2 前必须满足：

- 功能测试通过。
- 没有 embedding buffer 泄漏。
- 重复 VSET、VDEL、scoped search 语义不变。
- profile 能证明锁外 prepare 没有引入性能明显回退。
