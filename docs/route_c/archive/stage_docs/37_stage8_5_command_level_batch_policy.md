# Stage 8.5 Command-Level Batch Policy

## 背景

Stage 8.3 已经证明 embedding worker 的 true batch encode 可以降低 VSEARCH 的 embedding prepare 成本。Stage 8.4 继续拆分 VSET 写路径后确认：VSET batch 没有稳定收益，主要不是 embedding runtime 或 prepared queue，而是回到 single executor 后仍要串行支付 HNSW/current index insert 成本。

本阶段把这个结论落到配置策略：batch 能力保留，但由命令级 policy 控制。

## Stage 8.3 / 8.4 结论

- VSEARCH batch 有稳定收益。
- VSET batch 没有稳定收益。
- VSET commit 中 `vset_hnsw_add_avg_us` 约占 `vset_commit_avg_us` 大头。
- AOF no-fsync、response push、lock wait、prepared queue 都不是当前 VSET 主瓶颈。
- 当前不进入 commit ordering，也不做多 executor。

## 为什么需要命令级 policy

全局 batch 会把 VSET 和 VSEARCH 放在同一个策略下调参。Stage 8.4 的数据说明两类命令的收益模型不同：

- VSEARCH 没有写路径 side effects，embedding batch 收益可以直接体现到吞吐。
- VSET 即使 prepare 被 batch，后续仍要串行 commit HNSW/current index，因此 batch wait 容易抵消 prepare 收益。

命令级 policy 允许默认只让 VSEARCH 进入 batch，同时保留用户手动开启 VSET batch 的能力。

## 配置项

默认值：

```conf
embedding_batch_enabled = 0
embedding_batch_vset_enabled = 0
embedding_batch_vsearch_enabled = 1
embedding_batch_vset_max_size = 2
embedding_batch_vset_max_wait_us = 0
embedding_batch_vsearch_max_size = 4
embedding_batch_vsearch_max_wait_us = 500
```

语义：

- `embedding_batch_enabled=0`：全局关闭 batch，回到单 task prepare。
- `embedding_batch_enabled=1` 且 `embedding_batch_vsearch_enabled=1`：VSEARCH 可以 batch。
- `embedding_batch_enabled=1` 且 `embedding_batch_vset_enabled=0`：VSET 不进入 batch，走 single prepare。
- 用户显式开启 VSET batch 时使用 VSET 自己的 `max_size/max_wait_us`。
- 保留旧全局 `embedding_batch_max_size/max_wait_us` 作为兼容字段；Stage 8.5 新路径优先使用命令级字段。

## CLI

新增命令行覆盖项：

```text
--embedding_batch_vset_enabled
--embedding_batch_vsearch_enabled
--embedding_batch_vset_max_size
--embedding_batch_vset_max_wait_us
--embedding_batch_vsearch_max_size
--embedding_batch_vsearch_max_wait_us
```

解析方式沿用现有 bool/int range parser，不改变配置文件优先级和命令行覆盖逻辑。

## Collector Policy

worker pop 出第一个 task 后，根据命令类型读取 policy：

```text
global batch disabled -> single prepare
command batch disabled -> single prepare
command batch enabled -> only collect same command type
```

Stage 8.5 禁用 mixed VSET/VSEARCH batch。collector 只收集队头连续同类命令，遇到不同命令立即停止，不跨越队头任务，因此不引入 prepared queue 重排。

## Stats

保留全局 batch stats：

```text
batch_count
batch_items
batch_avg_size
batch_max_observed_size
batch_wait_avg_us
batch_encode_avg_us
batch_fallback_single_count
```

新增命令级 stats：

```text
embedding_batch_vset_enabled
embedding_batch_vsearch_enabled
embedding_batch_vset_count
embedding_batch_vset_items
embedding_batch_vset_avg_size
embedding_batch_vset_max_observed_size
embedding_batch_vset_wait_avg_us
embedding_batch_vset_encode_avg_us
embedding_batch_vset_fallback_single_count
embedding_batch_vsearch_count
embedding_batch_vsearch_items
embedding_batch_vsearch_avg_size
embedding_batch_vsearch_max_observed_size
embedding_batch_vsearch_wait_avg_us
embedding_batch_vsearch_encode_avg_us
embedding_batch_vsearch_fallback_single_count
```

全局 stats 是 VSET + VSEARCH 汇总；命令级 stats 用于判断 policy 是否生效。

## Correctness

覆盖脚本：

```bash
tests/embedding_batch_policy_stage8_5.sh
```

覆盖：

- reactor / proactor。
- 全局 batch disabled。
- VSET disabled + VSEARCH enabled。
- VSET enabled + VSEARCH enabled。
- VSET insert / repeat update。
- VSEARCH correctness。
- pipeline 10/50。
- multi-session concurrent VSET/VSEARCH。
- invalid VSET / invalid VSEARCH。
- VSET/VSEARCH command-level batch stats。

## Performance

覆盖脚本：

```bash
RESULT_DIR=results/stage8_5_batch_policy \
tests/embedding_batch_policy_perf_stage8_5.sh
```

对比：

- no batch baseline。
- old global mixed equivalent：VSET/VSEARCH 均启用同样 size/wait，但 Stage 8.5 collector 仍按命令隔离。
- policy：VSET off, VSEARCH on。
- policy：VSET on size2 wait0, VSEARCH on size4 wait500。

输出：

- VSET throughput c=4/8/16。
- repeat VSET throughput c=4/8/16。
- VSEARCH throughput c=4/8/16。
- VSET/VSEARCH latency p95/p99。
- VSET/VSEARCH batch count / avg size。
- prepared_wait / execute_sync。
- vset_commit / vsearch_search。

## 推荐配置

推荐生产默认仍保持全局 batch 关闭：

```conf
embedding_batch_enabled = 0
```

推荐需要查询吞吐时显式打开全局 batch，并使用：

```conf
embedding_batch_vset_enabled = 0
embedding_batch_vsearch_enabled = 1
embedding_batch_vsearch_max_size = 4
embedding_batch_vsearch_max_wait_us = 500
```

VSET batch 只建议在特定 workload 下手动打开：

```conf
embedding_batch_vset_enabled = 1
embedding_batch_vset_max_size = 2
embedding_batch_vset_max_wait_us = 0
```

## 已知限制

- Stage 8.5 不改变 single executor commit/search/finalize 串行语义。
- 不做 commit ordering。
- 不做多 executor。
- 不改变 HNSW insert、AOF append、replication broadcast。
- 禁用 mixed VSET/VSEARCH batch 后，旧全局 mixed batch 不再作为运行时行为保留。

## 下一阶段判断

优先级：

1. 继续观察 policy 模式在真实 workload 下是否让 VSEARCH 保持 batch 收益，同时避免 VSET batch wait。
2. 进入 HNSW/current index insert 优化或更细 VSET insert/update 拆分。
3. HNSW 参数调优。
4. commit ordering 只先写设计文档，不直接实现。
5. 多 executor、AOF batch、response batching、TSAN shutdown、vector/HNSW cleanup、TTL MOD 语义修复后置。
