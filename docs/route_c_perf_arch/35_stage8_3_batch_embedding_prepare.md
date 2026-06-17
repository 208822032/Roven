# Stage 8.3: batch embedding prepare

## 背景

Stage 8.2 的瓶颈定位结论是：并行 prepare 已经生效，`prepared_queue`
和 single executor 不是主瓶颈，`execute_sync` 约百微秒级，而
`OrtRun` 仍是毫秒级主成本。Stage 8.3 因此继续留在 embedding runtime
层，不进入多 command executor，也不实现 commit ordering。

本阶段目标是在不改变 commit/search/finalize 串行语义的前提下，把 worker
已取出的多个 VSET/VSEARCH prepare task 聚合为小 batch，用一次 OrtRun
生成多条 embedding，再把结果按输入顺序拆回每个 task。

## 边界

本阶段没有做：

- 多 command executor / 多 commit executor
- commit ordering / pipeline 并行 / response reorder
- `g_kv_data_rwlock`、RESP/RDB/AOF/主从协议、VSET/VSEARCH 语义变更
- vector record ownership、HNSW cleanup、MOD TTL cleanup
- 共享 OrtSession 或删除 global mutex fallback

保留的核心原则仍是：

```text
embedding encode 并发，状态 commit 串行。
```

## 配置

新增配置项：

```conf
embedding_batch_enabled = 0
embedding_batch_max_size = 4
embedding_batch_max_wait_us = 500
```

默认保持关闭，`config/kvstore-embedding.conf` 作为 Stage 8.3 测试示例显式开启：

```conf
embedding_worker_threads = 4
embedding_num_threads = 1
embedding_batch_enabled = 1
embedding_batch_max_size = 4
embedding_batch_max_wait_us = 500
```

推荐仍以 Stage 8.2 的 `worker_threads=4, embedding_num_threads=1` 为基线。

## Batch Collector

worker pool 流程从单条 prepare 扩展为：

```text
worker pop first task
  -> collect compatible tasks until max_size or max_wait_us
  -> batch encode
  -> split output vectors by task order
  -> push each prepared task to prepared_queue
  -> single executor commit/search/finalize
```

兼容条件：

- task build 成功
- task 是 VSET 或 VSEARCH
- 非 replication / REPLSYNC
- 未持有 store lock
- 尚未 prepare
- 使用同一个 embedding provider/dimension/runtime

VSET 和 VSEARCH 支持混合 batch，输入文本统一来自 `task->ctx.query_text`。
same-session pipeline 保序由既有“同 session 只有一个 in-flight task”机制保证；
collector 不做 response reorder。

collector 等待策略：

- 队列已有任务时立即取，直到 `embedding_batch_max_size`
- 队列为空时最多等待 `embedding_batch_max_wait_us`
- stop/shutdown 时不继续等待
- `max_wait_us=0` 表示不主动等，只收集当前队列里已经到达的任务

## Batch Encode API

新增 C API：

```c
EmbeddingErrorCode embedding_encode_batch(
    EmbeddingHandle *handle,
    const char **texts,
    const size_t *text_lens,
    size_t text_count,
    float *outputs,
    int output_dim
);
```

非 ONNX fallback 循环调用单条 `embedding_encode()`。
ONNX runtime 实现 true batch：

- tokenizer 逐条执行
- batch 输入张量 shape 为 `[batch, max_seq_len_in_batch]`
- 一次 `OrtRun` 生成 `sentence_embedding`
- 输出 shape 校验为 `[batch, embedding_dim]`
- 按输入顺序复制到连续 output buffer

重要修正：第一版曾用 `[batch, max_tokens]`，短文本场景会把 padding 放大到
512 token，导致 batch 明显退化。当前实现改为本批最大实际 token 长度，
避免短文本 batch 被固定 padding 拖垮。

## Ownership

worker 仍只做 prepare：

- batch encode 返回连续 `float *embeddings`
- 每个 task 通过 `*_prepare_with_embedding()` 拷贝自己的 row
- prepared struct ownership 与单条 prepare 一致
- task 成功后逐个提交到 prepared_queue
- submit 失败时只销毁对应 task
- response_task / session ref 仍由 task destroy 或 executor 完成路径释放

错误路径：

- batch encode 失败时整批失败，并生成对应错误响应
- 第一版不做 per-item partial success
- per-task prepared 拷贝失败时整批 prepared 数据回滚，所有 task 置错误响应
- provider 不支持 true batch 时 fallback 到 single encode

## Stats

新增 batch stats：

```text
embedding_batch_enabled
embedding_batch_count
embedding_batch_items
embedding_batch_avg_size
embedding_batch_max_observed_size
embedding_batch_wait_total_us / max_us / avg_us
embedding_batch_encode_total_us / max_us / avg_us
embedding_batch_fallback_single_count
embedding_batch_failed
embedding_batch_provider_unsupported
```

保留 Stage 8.2 stats：

```text
tokenizer_avg_us
ort_run_avg_us
embedding_encode_avg_us
prepared_wait_avg_us
execute_sync_avg_us
encode_max_inflight
resource_product
```

注意：batch 路径中 runtime `encode_count` 按 item 计数，batch `OrtRun`
耗时按整批记录，因此 `embedding_encode_avg_us` / `ort_run_avg_us` 是
近似 item 摊销视角；`embedding_batch_encode_avg_us` 是整批 prepare 视角。

## Correctness

命令：

```bash
tests/embedding_batch_stage8_3.sh
```

结果：

```text
reactor batch disabled: ok
reactor batch enabled: batch_count=8 avg_size=3 max_size=4
proactor batch disabled: ok
proactor batch enabled: batch_count=9 avg_size=3 max_size=4
stage8.3 embedding batch correctness: ok
```

覆盖：

- reactor / proactor
- batch disabled / enabled
- semantic ONNX
- VSET / VGET / VSEARCH / VDEL
- repeat VSET
- invalid VSET / invalid VSEARCH
- multi-session concurrent VSET/VSEARCH
- batch stats `batch_count > 0`、`avg_size > 1`

## Performance

命令：

```bash
RESULT_DIR=results/stage8_3_embedding_batch \
tests/embedding_batch_perf_stage8_3.sh
```

测试基线：

- worker_threads=4
- embedding_num_threads=1
- per-thread runtime enabled
- shared OrtEnv enabled
- resource_product=4

关键 c16 throughput 结果：

| mode | config | VSET qps | VSEARCH qps | batch avg/max | ort avg us | exec avg us |
|---|---:|---:|---:|---:|---:|---:|
| reactor | no batch | 361.98 | 701.83 | 0/0 | 8813 | 153 |
| reactor | batch 2 wait 0 | 322.96 | 1009.49 | 2/2 | 8826 | 129 |
| reactor | batch 2 wait 500 | 344.92 | 1115.45 | 2/2 | 8787 | 134 |
| reactor | batch 4 wait 0 | 314.64 | 1016.85 | 2/4 | 9033 | 122 |
| reactor | batch 4 wait 500 | 327.72 | 1110.25 | 3/4 | 8578 | 115 |
| reactor | batch 8 wait 500 | 301.51 | 872.40 | 3/8 | 8898 | 128 |
| proactor | no batch | 353.30 | 720.81 | 0/0 | 8455 | 131 |
| proactor | batch 2 wait 0 | 353.10 | 952.53 | 2/2 | 9085 | 132 |
| proactor | batch 4 wait 0 | 345.40 | 1158.13 | 2/4 | 8809 | 122 |
| proactor | batch 4 wait 500 | 313.05 | 1017.29 | 3/4 | 8857 | 122 |
| proactor | batch 8 wait 500 | 242.44 | 828.80 | 3/8 | 8746 | 121 |

结论：

- VSEARCH 从约 700 qps 提升到约 950-1158 qps，batch 有明确收益。
- VSET 没有收益，batch size/wait 增大会略降，说明 VSET 受 commit/index insert
  与响应路径影响更明显。
- `prepared_wait_avg_us` 仍为几十微秒级，single executor 不是主瓶颈。
- `execute_sync_avg_us` 约 115-153us，仍远低于 OrtRun。
- 主瓶颈仍是 ONNX OrtRun，只是 batch 能在 VSEARCH 高并发下摊薄调度成本。

## ORT Options

对比项：

| mode | config | VSET c16 qps | VSEARCH c16 qps | 结论 |
|---|---:|---:|---:|---|
| reactor | batch4 basic sequential inter=0 | 327.72 | 1110.25 | 基线 |
| reactor | graph_opt=all | 326.19 | 1097.85 | 无稳定收益 |
| reactor | inter_threads=1 | 291.31 | 1128.59 | VSEARCH 小升，VSET 降 |
| reactor | parallel + inter=1 | 317.06 | 889.24 | 明显不推荐 |
| proactor | batch4 basic sequential inter=0 | 313.05 | 1017.29 | 基线 |
| proactor | graph_opt=all | 323.98 | 1117.15 | VSEARCH 有收益，VSET 接近 |
| proactor | inter_threads=1 | 319.89 | 1139.58 | VSEARCH 有收益，VSET 接近 |
| proactor | parallel + inter=1 | 303.82 | 1057.82 | 不优于 sequential |

建议：

- 不在本阶段改变默认 ORT options。
- `graph_opt=all` 可作为可选 benchmark 配置继续观察。
- `execution_mode=parallel` 在小模型 + 多 worker 下不适合作默认。

## Batch Size / Wait Tradeoff

当前推荐：

```conf
embedding_batch_enabled = 1
embedding_batch_max_size = 2 或 4
embedding_batch_max_wait_us = 0 或 500
```

偏 VSEARCH throughput：

- reactor: `batch_size=2, wait=500` 或 `batch_size=4, wait=500`
- proactor: `batch_size=4, wait=0`

偏 VSET latency/throughput：

- 继续使用 no batch，或仅启用 `batch_size=2, wait=0`

`batch_size=8` 当前不推荐，平均 size 只有 3 左右，但尾延迟和 VSET 吞吐明显变差。

## Sanitizer

ASAN/UBSAN：

```bash
EXTRA_CFLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=address,undefined" make embedding ENABLE_EBPF=0

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
tests/embedding_batch_stage8_3.sh
```

功能路径通过；未发现 batch collector、batch output、OrtSession、shared OrtEnv
相关 UAF/越界/UBSAN。LSAN 在进程退出时报既有小泄漏，栈主要落在 reactor/proactor
async argv、RESP parser、HNSW/rbtree shutdown，不归类为 Stage 8.3 batch
新增问题。

TSAN：

```bash
EXTRA_CFLAGS="-O1 -fsanitize=thread -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=thread" make embedding ENABLE_EBPF=0

TSAN_OPTIONS="halt_on_error=0 exitcode=0" \
tests/embedding_batch_stage8_3.sh
```

功能路径通过。TSAN 报告仍是既有 shutdown/background race：

- `g_shutdown_flag` vs distributed master sync server
- TTL background stop flag
- distributed shutdown config
- sync backend TCP running flag

没有发现 batch collector、worker queue、stats、TLS runtime、shared OrtEnv 或
per-worker OrtSession 相关 race。

## 判断

Stage 8.3 证明 batch embedding 在 VSEARCH 高并发下能继续降低 OrtRun 调度瓶颈，
但不是 VSET 的通用收益项。当前还不建议直接进入 commit ordering 实现：

- executor/commit 仍不是主瓶颈
- VSEARCH 的收益来自 batch OrtRun
- VSET 需要进一步拆分 commit/index insert/AOF/response 的真实占比

建议下一步：

- 保留 batch 为可配置能力，默认可继续关闭
- 继续针对 VSEARCH 推荐小 batch
- 评估 tokenizer/query embedding cache 和模型量化
- 若后续 stats 显示 `prepared_wait` 或 `execute_sync` 明显升高，再进入 commit
  ordering / 多 executor 设计
