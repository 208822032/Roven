# 阶段 7：embedding prepare 工作线程池最小闭环

## 1. 背景

阶段 6D 已经压稳 reactor/proactor 共享 single command executor 的异步闭环，并验证 owner-aware response polling 下 reactor 只消费 `REACTOR` response、proactor 只消费 `PROACTOR` response。

6D 性能结果显示，继续扩大网络侧收益有限。VSET/VSEARCH 的主要耗时仍在 embedding prepare：RESP 解析后的 command task 进入 single executor 后，`OrtRun` / embedding encode 会占用唯一 executor 线程，导致普通 KV、TTL、其他连接的 vector task 都被 prepare 阶段串行阻塞。

阶段 7 的目标不是多 executor，而是拆出耗时 prepare：

```text
embedding prepare 并发，状态 commit 串行。
```

## 2. 为什么先做 worker pool

没有先做多 command executor，原因是：

- KV store、HNSW、AOF、replication 的提交语义仍依赖单点串行化。
- 多 executor 会立刻引入 commit ordering、AOF ordering、replication ordering 和跨连接可见性问题。
- 当前瓶颈集中在 VSET/VSEARCH prepare，而 prepare 可以在不访问 store、不持锁、不生成最终 response 的前提下拆出。

因此本阶段只把 embedding prepare 从 single executor 线程搬到 worker pool。commit/search/finalize 仍回到 single command executor。

## 3. 总体架构

新增模块：

- `include/kvs_embedding_worker_pool.h`
- `src/core/kvs_embedding_worker_pool.c`

执行路径：

```text
network owner loop
  -> build command_task + response_task + session ref
  -> submit command_queue

single command executor
  -> normal command: prepare/execute/post_commit/finalize
  -> VSET/VSEARCH with worker enabled:
       submit task to embedding worker prepare_queue
       continue processing other tasks

embedding worker
  -> kvs_command_task_prepare(task)
  -> push task to command executor prepared_queue

single command executor
  -> pop prepared_queue first
  -> execute_sync / commit / search under existing lock
  -> post_commit
  -> finalize response
  -> push owner-aware response queue
  -> eventfd wake reactor/proactor owner loop
```

## 4. 队列设计

本阶段有三类队列：

- `command_queue`：网络线程提交的新 task。
- `prepare_queue`：worker pool 内部队列，仅保存需要 embedding prepare 的 VSET/VSEARCH task。
- `prepared_queue`：worker 完成 prepare 后回投给 command executor 的 task。

executor 主循环优先级：

```text
prepared_queue first
command_queue second
```

这样已完成 prepare 的 task 不会长时间等待 commit。`prepared_queue` 的 commit/search/finalize 仍由唯一 executor 线程执行。

## 5. task 判定

新增：

```c
int kvs_command_task_requires_embedding_prepare(const kvs_command_task_t *task);
```

仅允许 build 成功的以下命令进入 worker pool：

- `VSET`
- `VSEARCH`

以下命令不进入 worker pool：

- `SET` / `GET` / `DEL` / `EXPIRE` / `TTL` / `PERSIST`
- `VGET` / `VDEL` / `VINFO` / `VINDEX`
- replication / `REPLSYNC`
- unknown command、arity error、invalid argument

如果 worker pool 未启用，或者 submit 失败，executor 回退到 inline prepare，保持阶段 6D 行为。

## 6. task 生命周期和 ownership

正常 async prepare 路径：

```text
network owner loop
  -> heap task + owned argv + response_task + session ref
  -> command executor queue

command executor
  -> submit worker pool
  -> 不 destroy task
  -> 不 release session ref

embedding worker
  -> prepare
  -> set prepare_status / prepare_completed
  -> submit prepared_queue

command executor
  -> transfer session ref from task to response_task
  -> execute_sync / post_commit / finalize
  -> destroy task exactly once
  -> push response_task

network owner loop
  -> dispatch response
  -> release response_task session ref
  -> destroy response_task exactly once
```

prepare 失败路径：

- worker 记录 `prepare_status` 为 error response 长度。
- task 回到 prepared queue。
- executor 不 commit store，不做 post_commit。
- executor finalize error response 并按原 owner-aware response queue 回投。

submit prepared queue 失败路径：

- worker 记录 `prepare_dispatch_failed`。
- worker 安全销毁 task/response_task/session ref。

shutdown 路径：

- command executor stop 先停止 worker pool。
- worker pool drain prepare queue，完成已提交 prepare，并把 prepared task 回投 executor。
- 然后 executor 设置 `stopping`，drain command/prepared queue，再 join。
- remaining task/response 会按现有 destroy helper 释放 session ref。

## 7. commit 串行和锁语义

`kvs_command_task_prepare()` 只负责 VSET/VSEARCH 的 embedding prepare。

`kvs_command_task_execute_sync()` 仍在 single command executor 中执行：

- 按 command lock mode 获取 `g_kv_data_rwlock`。
- VSET 调用 `kvs_vector_set_commit_prepared()` 写入 record / flat index / HNSW / id map。
- VSEARCH 调用 `kvs_vector_search_with_prepared()` 读取 HNSW、scope filter、threshold 并生成 response。
- AOF 和 replication 仍在 post_commit 串行路径。

本阶段没有改变 `g_kv_data_rwlock`、RESP、RDB、AOF、主从协议、vector/embedding 语义。

## 8. pipeline 保序

同 session 保序仍依赖阶段 6C/6D 的 `single_inflight + send_buf gating`：

- 同一连接一次只允许一个 in-flight task。
- response append/drop 完成后才 `restore_read` 继续读取下一条。
- worker pool 不会让同一 session 的多个 VSET/VSEARCH 并行 prepare。
- 不做 response reorder。

不同 session 的 VSET/VSEARCH 会因为 prepare 完成顺序不同而以不同顺序进入 serial commit。这符合当前 KVLite 语义：不承诺跨连接到达顺序的全局严格线性化。AOF/replication 顺序以后续 single executor 实际 commit 顺序为准。

## 9. embedding runtime 线程安全审计

VSET prepare 当前做：

- 解析后的 question/scope/answer 已在 task owned argv 中。
- 调用 embedding encode。
- 保存 prepared vector 和 prepared strings 到 task prepared data。
- 不访问全局 store。
- 不持有 `g_kv_data_rwlock`。

VSEARCH prepare 当前做：

- 解析后的 query/scope/topk/threshold 已在 task ctx 中。
- 调用 embedding encode。
- 保存 prepared query vector 到 task prepared data。
- 不访问全局 store。
- 不持有 `g_kv_data_rwlock`。

ONNX/local_c runtime 审计：

- `EmbeddingHandle` 内有共享 `last_error` buffer。
- vocab/tokenizer 数据主要只读。
- profiling 字段已有 mutex，但不覆盖 `last_error` 和完整 encode 调用。
- 当前没有 per-thread `OrtSession` 或 per-thread tokenizer context。

保守处理：

- 在 `src/vector/kvs_vector_embedding.c` 增加 provider-level mutex。
- local_c single encode 和 batch encode 都在 `g_embedding_provider_encode_mu` 下调用 `embedding_encode()`。
- error log 中读取 `embedding_last_error()` 也在同一锁内。
- hash provider 不加锁，仍保持纯本地 deterministic path。

结论：

- Stage 7 已把 embedding prepare 从 command executor 线程拆出。
- ONNX/local_c encode 并行度受 provider mutex 限制，主要收益是 executor 不再被 `OrtRun` 长时间占用。
- 真正并行 ONNX encode 需要后续 per-thread session 或 provider context 审计，不在本阶段做。

## 10. 配置项

新增配置：

```conf
embedding_worker_pool_enabled = 1
embedding_worker_threads = 4
embedding_worker_queue_capacity = 1024
```

默认值：

- `embedding_worker_pool_enabled = 0`
- `embedding_worker_threads = 4`
- `embedding_worker_queue_capacity = 1024`

`config/kvstore-embedding.conf` 显式开启 worker pool，普通未配置场景默认保持阶段 6D 行为。

## 11. stats

executor 新增 stats：

- `prepared_queue_depth`
- `prepared_queue_max_depth`
- `prepared_tasks`
- `embedding_prepare_dispatched`
- `embedding_prepare_inline`
- `embedding_prepare_submit_failed`

worker pool stats：

- `submitted_prepare_tasks`
- `completed_prepare_tasks`
- `failed_prepare_tasks`
- `prepare_queue_depth`
- `prepare_queue_max_depth`
- `worker_threads`
- `submit_failed`
- `prepare_dispatch_failed`
- `prepare_count`
- `prepare_total_us`
- `prepare_max_us`

当前统一 stats endpoint 尚未接入，worker pool stats 在 command executor shutdown 时输出。

样例：

```text
embedding worker pool stats: submitted=1560 completed=1560 failed=0 queue_depth=0 queue_max=16 worker_threads=2 submit_failed=0 dispatch_failed=0 prepare_count=1560 prepare_avg_us=3038 prepare_max_us=27526
embedding worker pool stats: submitted=1560 completed=1560 failed=0 queue_depth=0 queue_max=14 worker_threads=4 submit_failed=0 dispatch_failed=0 prepare_count=1560 prepare_avg_us=5943 prepare_max_us=108838
```

## 12. 测试结果

构建：

```bash
make clean && make embedding
```

结果：通过。

Stage 7 稳定性：

```bash
tests/embedding_worker_pool_stage7.sh
```

结果：

```text
reactor worker_enabled=0: ok
reactor worker_enabled=1: ok
proactor worker_enabled=0: ok
proactor worker_enabled=1: ok
stage7 embedding worker pool: ok
```

覆盖：

- reactor + worker disabled/enabled。
- proactor + worker disabled/enabled。
- `semantic_onnx_test` correctness。
- KV SET/GET。
- TTL EXPIRE/TTL/PERSIST。
- VSET/VGET/VSEARCH/VDEL。
- repeat VSET 覆盖更新语义。
- invalid VSET、invalid VSEARCH、unknown vector command。
- vector pipeline 10/50。
- high accept 200。
- fd reuse 120。
- 多连接并发 VSET。
- enabled=1 时检查 worker stats：submitted > 0 且 completed == submitted。

ASAN/UBSAN：

```bash
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=address,undefined" \
make embedding ENABLE_EBPF=0

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
tests/embedding_worker_pool_stage7.sh
```

结果：

- ASAN/UBSAN 构建通过。
- 功能回归通过。
- graceful shutdown 时 LSAN 报既有退出清理泄漏，进程 abort，但测试脚本完成。

LSAN 分类：

- worker disabled 也存在泄漏报告。
- 栈主要位于 `kvs_vector_*` / HNSW / flat index / RESP parser / proactor/reactor op 清理。
- worker enabled 场景中出现 `kvs_vector_set_prepare()` 分配栈，但对应 prepared fields 在 commit 后被 vector record 接管，最终仍归类到既有 vector/HNSW 退出清理。
- 未发现 worker pool queue、prepared queue、pthread lifecycle 的新增独立泄漏栈。

TSAN：

- 本阶段未跑 TSAN。
- 当前 ONNX Runtime、signal、TTL/distributed shutdown 预计有既有噪音。
- 为降低明显 race 风险，`kvs_embedding_worker_pool_enabled()` 改为在 pool mutex 下读取 `running/thread_count`，避免 atomic 读取普通 `int`。

## 13. 性能对比

脚本：

```bash
RESULT_DIR=results/stage7_worker_pool_rerun tests/embedding_worker_pool_perf_stage7.sh
```

参数：

- requests=120。
- prepare_count=120。
- provider=local_c / ONNX model。
- worker disabled baseline。
- worker enabled threads=2。
- worker enabled threads=4。
- reactor/proactor 分别测试。

核心结果摘要：

| mode | worker | VSET latency avg/p95/p99 ms | VSEARCH latency avg/p95/p99 ms | VSET c16 qps | VSEARCH c16 qps | KV SET/GET c4 qps |
| --- | --- | --- | --- | ---: | ---: | ---: |
| reactor | disabled | 2.95 / 4.09 / 4.41 | 1.50 / 1.82 / 1.97 | 349.14 | 626.50 | 11638.32 |
| reactor | threads=2 | 3.07 / 4.93 / 5.43 | 1.58 / 2.57 / 3.39 | 335.41 | 607.08 | 10746.50 |
| reactor | threads=4 | 3.08 / 4.83 / 5.21 | 1.63 / 2.48 / 3.78 | 231.14 | 617.88 | 6989.11 |
| proactor | disabled | 2.98 / 4.65 / 4.91 | 1.57 / 2.30 / 3.55 | 356.88 | 627.90 | 6884.76 |
| proactor | threads=2 | 3.05 / 4.94 / 5.49 | 1.60 / 2.31 / 3.17 | 336.52 | 621.74 | 7299.79 |
| proactor | threads=4 | 4.03 / 8.31 / 8.76 | 2.18 / 3.44 / 7.99 | 296.44 | 429.19 | 8752.08 |

结论：

- worker pool 处理闭环正确，threads=2/4 均有 `submitted=1560 completed=1560 failed=0`。
- 由于 local_c/ONNX encode 被 provider mutex 串行化，VSET/VSEARCH 吞吐没有稳定提升。
- threads=4 在本环境下部分尾延迟更差，主要来自 worker 调度和 provider mutex 竞争。
- 这不代表阶段失败；本阶段目标是安全拆分 executor blocking prepare，并保留 commit 串行语义。
- 后续若要获得 embedding 吞吐提升，应优先做 per-thread embedding runtime / per-thread OrtSession 审计和实现。

## 14. 已知限制

- 仍是 single command executor。
- commit/search/finalize 仍串行。
- ONNX/local_c encode 受 provider mutex 限制，不能真实并行 `OrtRun`。
- 不保证跨连接到达顺序的全局严格线性化。
- worker pool stats 暂时只在 shutdown log 输出。
- LSAN 仍有既有 vector/HNSW/RESP/proactor/reactor 退出清理噪音。
- 未做 TSAN 专项。

## 15. 是否建议进入下一阶段

建议进入代码审查。

不建议立刻做多 command executor。Stage 7 已证明 prepare 可以安全脱离 executor，但性能数据表明 provider mutex 是下一瓶颈。

下一阶段优先级建议：

1. per-thread embedding runtime / per-thread OrtSession 审计与最小实现。
2. commit ordering 设计与 commit 串行化边界文档化。
3. 多 executor 放在更后面，必须等 commit ordering、AOF/replication ordering 明确后再做。

