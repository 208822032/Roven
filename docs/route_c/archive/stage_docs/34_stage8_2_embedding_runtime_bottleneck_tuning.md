# Stage 8.2: embedding runtime 性能调优与瓶颈定位

## 背景

Stage 8.1 已完成 shared OrtEnv + per-worker OrtSession，并把
worker_threads * embedding_num_threads 从 Stage 8 baseline 的 16
降到 2/4。性能相对 Stage 8 baseline 明显改善，但 VSET/VSEARCH
整体吞吐仍低于并行 prepare 的理想收益。

本阶段继续停留在 embedding runtime 层做瓶颈定位和低风险调优：

- 不做多 command executor。
- 不做 commit ordering 实现。
- 不做 pipeline 并行或 response reorder。
- 不改变 RESP/RDB/AOF/主从协议。
- 不改变 VSET/VSEARCH commit/search 语义。

核心原则仍是：

```text
embedding encode 并发，状态 commit/search/finalize 串行。
```

## Stage 8.1 审计结论

Stage 8.1 审计结论为 CONDITIONALLY_APPROVED：

- shared OrtEnv + per-worker OrtSession 生命周期闭合。
- per-worker EmbeddingHandle/OrtSession 隔离保留。
- global mutex fallback 保留。
- TSAN/LSAN 归类合理，未发现 worker/TLS/shared OrtEnv 新 race。
- Stage 8 baseline 资源乘积 16 的性能退化已通过 2x1/4x1 调优明显缓解。

本阶段先修复审计指出的 CLI 缺口：

```text
--embedding_shared_ort_env_enabled <yes|no>
```

该参数与配置文件字段 `embedding_shared_ort_env_enabled` 使用相同 bool parser。

## 新增 Stats

### Worker pool 侧

```text
prepare_queue_wait_avg_us
prepare_queue_wait_max_us
prepare_avg_us
prepare_max_us
prepared_queue_submit_avg_us
prepared_queue_submit_max_us
queue_max
```

含义：

- `prepare_queue_wait_*`：task 从进入 worker prepare queue 到被 worker 取出的等待时间。
- `prepare_*`：worker 内执行 `kvs_command_task_prepare()` 的耗时。
- `prepared_queue_submit_*`：worker 把 prepared task 回投 single executor prepared queue 的提交耗时。

### Embedding runtime 侧

```text
runtime_encode_count
embedding_encode_avg_us
embedding_encode_max_us
tokenizer_avg_us
tokenizer_max_us
ort_run_avg_us
ort_run_max_us
encode_max_inflight
```

ONNX runtime 现在默认记录 tokenizer 和 OrtRun 分段耗时，不再依赖
`KVLITE_EMBEDDING_PROFILE`。

### Executor prepared task 侧

```text
prepared_queue_max
prepared_wait_avg_us
prepared_wait_max_us
prepared_task_execute_avg_us
prepared_task_execute_max_us
execute_sync_avg_us
execute_sync_max_us
commit_or_search_avg_us
commit_or_search_max_us
finalize_response_avg_us
finalize_response_max_us
```

说明：

- `prepared_wait_*`：prepared task 从回投 executor 到被 executor 取出执行的等待时间。
- `execute_sync_*`：prepared task 回到 executor 后执行 `kvs_command_task_execute_sync()` 的耗时。
- `commit_or_search_*`：当前低风险统计口径为 `kvs_command_task_post_commit()` 耗时。VSET/VSEARCH
  的主要同步执行成本仍包含在 `execute_sync_*` 中。
- `prepared_task_execute_*`：prepared task 从 executor 开始处理到 response push 前后的总耗时。

本阶段未深入拆 `vset_commit_total_us`、`vsearch_search_total_us`、`hnsw_search_total_us`，
避免触碰 vector/HNSW 内部路径。若 `execute_sync_*` 成为主瓶颈，下一阶段再拆 vector 内部。

## ONNX Runtime Session Options 审计

当前 ONNX provider 已支持：

- `embedding_num_threads` -> `SetIntraOpNumThreads`
- `embedding_inter_threads > 0` -> `SetInterOpNumThreads`
- `embedding_ort_execution_mode` -> `sequential|parallel`
- `embedding_ort_graph_opt_level` -> `disable|basic|extended|all`

Stage 8.2 代码层没有强行提高默认线程数。benchmark matrix 继续以
`graph_opt=basic`、`inter_threads=0`、`execution_mode=sequential` 为基线，
避免在未确认收益前引入额外 ORT 线程竞争。后续可单独追加
`graph_opt=all` 与 `inter_threads=1` 的对比。

## 配置矩阵

新增脚本：

```bash
tests/embedding_runtime_bottleneck_stage8_2.sh
```

默认覆盖 reactor/proactor 下的七组组合：

| label | worker_pool | worker_threads | per_thread_runtime | shared_ort_env | embedding_num_threads | resource_product |
| --- | --- | ---: | --- | --- | ---: | ---: |
| worker_disabled | 0 | 1 | 0 | 0 | 1 | 1 |
| stage7_mutex_2x1 | 1 | 2 | 0 | 0 | 1 | 2 |
| tuned_2x1 | 1 | 2 | 1 | 1 | 1 | 2 |
| tuned_4x1 | 1 | 4 | 1 | 1 | 1 | 4 |
| tuned_2x2 | 1 | 2 | 1 | 1 | 2 | 4 |
| tuned_4x2 | 1 | 4 | 1 | 1 | 2 | 8 |
| tuned_1x1 | 1 | 1 | 1 | 1 | 1 | 1 |

每组运行：

- KV SET/GET smoke
- VSET latency
- VSEARCH latency
- VSET throughput c=4/8/16
- VSEARCH throughput c=4/8/16

默认 `REQUESTS=80 PREPARE_COUNT=80`，可通过环境变量放大。

## Correctness 测试结果

运行命令：

```bash
git diff --check
make clean
make embedding ENABLE_EBPF=0
tests/embedding_runtime_tuning_stage8_1.sh
```

结果：

- `git diff --check` 通过。
- normal clean build 通过。
- Stage 8.1 correctness 通过 reactor/proactor、semantic ONNX、VSET/VGET/VSEARCH/VDEL、
  repeat VSET、pipeline、high accept、fd reuse、多连接并发。
- Stage 8.2 benchmark matrix 的 KV SET/GET smoke 全部通过。
- per-thread tuned case 中 `context_created == context_destroyed`、
  `ort_session_created == ort_session_destroyed`、
  `shared_ort_env_created == shared_ort_env_destroyed`。

Sanitizer：

- ASAN/UBSAN 构建通过。
- ASAN/UBSAN 功能路径通过；进程退出时 LSAN 报既有 shutdown/vector/argv/index 残留，
  未指向 shared OrtEnv、per-worker OrtSession、TLS context 或 Stage 8.2 stats。
- TSAN 构建通过。
- TSAN correctness 通过；报告仍为既有 shutdown/background race：
  `g_shutdown_flag`、TTL stop、distributed shutdown、sync_backend shutdown。
  未发现 `prepare_enqueue_us`、`prepared_enqueue_us`、worker queue stats、
  executor bottleneck stats、shared OrtEnv 或 ONNX split stats 相关 race。

## Performance 测试结果

运行命令：

```bash
RESULT_DIR=results/stage8_2_embedding_runtime_bottleneck \
tests/embedding_runtime_bottleneck_stage8_2.sh
```

默认 `REQUESTS=80 PREPARE_COUNT=80`。关键 c=16 throughput 结果：

| mode | label | resource_product | VSET qps | VSEARCH qps |
| --- | --- | ---: | ---: | ---: |
| reactor | worker_disabled | 0 | 100.18 | 287.61 |
| reactor | stage7_mutex_2x1 | 2 | 94.84 | 293.09 |
| reactor | tuned_1x1 | 1 | 101.79 | 290.11 |
| reactor | tuned_2x1 | 2 | 201.54 | 534.73 |
| reactor | tuned_4x1 | 4 | 353.48 | 680.84 |
| reactor | tuned_2x2 | 4 | 324.07 | 460.28 |
| reactor | tuned_4x2 | 8 | 202.57 | 414.00 |
| proactor | worker_disabled | 0 | 101.38 | 284.63 |
| proactor | stage7_mutex_2x1 | 2 | 94.97 | 288.49 |
| proactor | tuned_1x1 | 1 | 103.43 | 296.88 |
| proactor | tuned_2x1 | 2 | 203.35 | 541.36 |
| proactor | tuned_4x1 | 4 | 354.76 | 712.89 |
| proactor | tuned_2x2 | 4 | 332.20 | 716.25 |
| proactor | tuned_4x2 | 8 | 222.12 | 415.12 |

结论：

- 4x1 是当前最稳的整体 throughput 配置。
- 2x2 的 single-request latency 和 OrtRun 平均耗时更好，但 VSET c=16 吞吐低于 4x1。
- 4x2/resource_product=8 明显退化，说明 ORT/CPU/memory 资源竞争已经超过收益。
- proactor 与 reactor 结论一致，proactor VSEARCH 在 4x1/2x2 上略高。

## 分段耗时分析

短矩阵 `REQUESTS=40 PREPARE_COUNT=40` 用于验证拆分后的 executor stats。
关键平均耗时：

| mode | label | encode avg us | tokenizer avg us | OrtRun avg us | prepared wait avg us | execute_sync avg us |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| reactor | tuned_2x1 | 7822 | 约 10-30 | 7810 | 23 | 140 |
| reactor | tuned_2x2 | 4677 | 约 10-30 | 4663 | 14 | 124 |
| reactor | tuned_4x1 | 8851 | 约 10-30 | 8818 | 24 | 150 |
| reactor | tuned_4x2 | 9352 | 约 10-30 | 9327 | 550 | 109 |
| proactor | tuned_2x1 | 7945 | 约 10-30 | 7932 | 20 | 137 |
| proactor | tuned_2x2 | 4553 | 约 10-30 | 4539 | 45 | 109 |
| proactor | tuned_4x1 | 8593 | 约 10-30 | 8578 | 18 | 113 |
| proactor | tuned_4x2 | 8160 | 约 10-30 | 8137 | 730 | 108 |

判断：

- tokenizer 不是当前瓶颈，平均只有几十微秒以内。
- OrtRun 占据几乎全部 encode 时间，是主要瓶颈。
- prepared queue 到 single executor 的等待在 2x1/2x2/4x1 中很低，不是主瓶颈。
- `execute_sync_avg_us` 约 100-170us，远低于 OrtRun，不是当前主瓶颈。
- 4x2 的 prepared wait 明显升高，说明资源乘积过大时 ORT/CPU 竞争会外溢到队列回投和 executor 消费。

判读规则：

- `embedding_encode_avg_us` 高：ONNX encode 本身仍是主瓶颈。
- `tokenizer_avg_us` 占比高：tokenizer 是主瓶颈。
- `ort_run_avg_us` 占比高：OrtRun 是主瓶颈，优先继续调 intra_op/graph opt/batch/量化。
- `prepared_wait_avg_us` 高：single executor 的 prepared queue 消费受限。
- `execute_sync_avg_us` 高：VSET commit/index insert 或 VSEARCH/HNSW/search 路径受限。
- `queue_max` 高：worker_threads 不足或 encode 过慢。
- `encode_max_inflight` 高但 qps 不升：CPU/memory/ORT 内部竞争或后续 commit/search 瓶颈。

## Batch Embedding 评估

本阶段不实现 batch embedding，只评估方向。

VSET 多连接并发天然可以形成多个 independent prepare task。worker pool
可以在 prepare queue 中聚合若干 task 后做 batch encode，再按原 task
逐个回投 prepared queue。关键约束是：

- same session pipeline 的 response 顺序不能被破坏。
- batch 完成后仍需按 task 独立保存 prepared result。
- 单个 batch 内的慢请求会增加尾延迟。
- 对吞吐有利，对单请求 latency 不一定有利。

VSEARCH query encode 也可 batch，但 search 本身仍回到 single executor
串行执行；如果 `execute_sync_*` 已经很高，batch 只能降低 encode 成本，无法解决
search/lock/index 的瓶颈。

初步建议：若 Stage 8.2 数据显示 `ort_run_avg_us` 是主要成本且
prepared/executor 等待不高，则 batch embedding 可作为 Stage 8.3。

## 是否进入 Commit Ordering 设计

当前不建议立即进入 commit ordering 实现。进入条件应是：

- per-worker encode 已经充分并发；
- worker queue 不再明显积压；
- `prepared_wait_*` 或 `execute_sync_*` 明确成为主瓶颈；
- VSET/VSEARCH 的 single executor 串行段限制吞吐。

若主要瓶颈仍在 tokenizer/OrtRun，则应优先继续 embedding runtime、
batch embedding、模型量化或 tokenizer cache，而不是进入 commit ordering。

Stage 8.2 数据显示主要瓶颈仍是 OrtRun，而不是 prepared queue 或
`execute_sync`。因此可以进入 commit ordering 的设计讨论，但不建议现在实现；
更合适的下一步是 batch embedding prepare 或模型/ORT 推理优化。

## 下一阶段建议

- 推荐示例配置：`embedding_worker_threads=4`、`embedding_num_threads=1`、
  `embedding_per_thread_runtime_enabled=1`、`embedding_shared_ort_env_enabled=1`。
- 保持 `worker_threads * embedding_num_threads <= physical cores`，避免 4x2 这类过度竞争。
- Stage 8.3 优先评估 batch embedding prepare，尤其是 VSET/VSEARCH 多连接并发场景。
- 可补充单独 ONNX options 对比：`graph_opt=all`、`inter_threads=1`、
  `execution_mode=parallel`，但不建议默认提高 ORT 内部并发。
- commit ordering 可以开始做设计文档，但实现应等到 encode/batch 方向收益耗尽后再进入。
