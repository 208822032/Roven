# Stage 8.4: VSET commit / index / AOF / response breakdown

## 背景

Stage 8.3 的审计结论为 `CONDITIONALLY_APPROVED`。batch collector 架构、
ONNX true batch、动态 `[batch, max_seq_len_in_batch]` shape、VSET/VSEARCH
mixed batch、task/response_task/session ref 生命周期、single executor 串行
commit/search/finalize 语义都通过审计。

Stage 8.3 性能结论是：

- VSEARCH batch 有明确收益。
- VSET batch 没有稳定收益。
- `prepared_wait` 仍是几十微秒。
- `execute_sync` 约 115-153us，明显低于 OrtRun。
- 当前不建议直接做 commit ordering 或多 executor。

Stage 8.4 因此只做低风险观测增强，目标是拆清 VSET 从 prepared task 回到
single executor 后的串行路径，回答 VSET batch 无稳定收益的真实原因。

## 边界

本阶段没有做：

- 多 command executor / 多 commit executor。
- commit ordering / pipeline 并行 / response reorder。
- `g_kv_data_rwlock` 语义变更。
- RESP/RDB/AOF/主从协议变更。
- VSET/VSEARCH 结果语义变更。
- vector record ownership 变更。
- replication / REPLSYNC worker 化。
- HNSW cleanup、TTL cleanup、TSAN shutdown 专项。
- batch embedding 语义变更。

所有新增逻辑都是 atomic stats 和 shutdown log 输出。

## 新增 Stats

executor prepared task 侧：

```text
vset_prepared_tasks
vset_prepared_wait_total_us / max_us / avg_us
vset_execute_sync_count / total_us / max_us / avg_us
vset_post_commit_count / total_us / max_us / avg_us
vset_finalize_response_count / total_us / max_us / avg_us
vset_push_response_count / total_us / max_us / avg_us
vsearch_execute_sync_count / total_us / max_us / avg_us
```

VSET commit / index 侧：

```text
vset_commit_count / total_us / max_us / avg_us
vset_record_lookup_count / total_us / max_us / avg_us
vset_record_insert_or_update_count / total_us / max_us / avg_us
vset_vector_copy_count / total_us / max_us / avg_us
vset_flat_index_update_count / total_us / max_us / avg_us
vset_hnsw_add_count / total_us / max_us / avg_us
vset_id_map_update_count / total_us / max_us / avg_us
vset_scope_answer_question_update_count / total_us / max_us / avg_us
```

side-effect / response 侧：

```text
vset_lock_wait_count / total_us / max_us / avg_us
vset_lock_hold_count / total_us / max_us / avg_us
vset_aof_append_count / total_us / max_us / avg_us
vset_replication_count / total_us / max_us / avg_us
vset_dirty_mark_count / total_us / max_us / avg_us
vset_response_build_count / total_us / max_us / avg_us
vset_response_push_count / total_us / max_us / avg_us
vsearch_search_count / total_us / max_us / avg_us
```

说明：

- lock wait/hold 在 command task lock/unlock 边界统计，不改变锁获取顺序。
- `vset_dirty_mark_*` 当前对应 vector store metadata update，持久化 dirty flag
  没有独立低风险 hook。
- AOF 和 replication 在 `kvs_command_task_post_commit()` 内分开计时。
- response build 对应 `kvs_response_task_finalize()`，response push 对应
  executor response queue push + eventfd wakeup。

## Commit 路径

VSET prepared commit 当前拆为：

```text
kvs_vector_set_commit_prepared
  -> validate prepared
  -> record lookup
  -> repeat update: answer update + prepared destroy
  -> insert: free slot / capacity
  -> scope/question/answer/embedding ownership transfer
  -> id_map put
  -> flat index add
  -> current index add (HNSW/SQ8)
  -> store active/total/deleted metadata update
```

失败路径按实际走到的阶段计数，`vset_commit_*` 覆盖整次 commit。

## Correctness

命令：

```bash
git diff --check
bash -n tests/vset_commit_breakdown_stage8_4.sh
bash -n tests/vset_commit_breakdown_perf_stage8_4.sh
make embedding ENABLE_EBPF=0
tests/vset_commit_breakdown_stage8_4.sh
```

结果：

```text
reactor batch=0 aof=no: vset stats ok
reactor batch=0 aof=yes: vset stats ok
reactor batch=1 aof=no: vset stats ok
reactor batch=1 aof=yes: vset stats ok
proactor batch=0 aof=no: vset stats ok
proactor batch=0 aof=yes: vset stats ok
proactor batch=1 aof=no: vset stats ok
proactor batch=1 aof=yes: vset stats ok
stage8.4 VSET commit breakdown correctness: ok
```

覆盖：

- reactor / proactor。
- batch disabled / enabled。
- AOF disabled / enabled。
- VSET insert。
- repeat VSET update。
- mixed VSET/VSEARCH。
- pipeline 10 / 50。
- multi-session concurrent VSET。
- batch stats。
- VSET commit / HNSW / flat index / response push stats。

## Performance

命令：

```bash
RESULT_DIR=results/stage8_4_vset_commit_breakdown \
tests/vset_commit_breakdown_perf_stage8_4.sh
```

默认配置：

- worker_threads=4
- embedding_num_threads=1
- per-thread runtime enabled
- shared OrtEnv enabled
- `REQUESTS=80`
- `PREPARE_COUNT=80`

关键 throughput：

| mode | config | VSET c16 qps | repeat VSET c16 qps | VSEARCH c16 qps | VSET latency avg/p95 ms |
|---|---:|---:|---:|---:|---:|
| reactor | no batch 4x1 | 351.57 | 342.40 | 715.24 | 12.53 / 17.78 |
| reactor | batch2 wait0 | 368.17 | 343.22 | 1087.86 | 10.90 / 16.59 |
| reactor | batch4 wait0 | 364.93 | 310.20 | 1047.46 | 9.54 / 10.19 |
| reactor | batch4 wait500 | 357.31 | 360.29 | 1099.08 | 11.80 / 18.17 |
| reactor | batch4 wait0 AOF | 323.96 | 320.29 | 1119.21 | 10.50 / 11.11 |
| proactor | no batch 4x1 | 352.95 | 325.97 | 741.28 | 9.56 / 10.20 |
| proactor | batch2 wait0 | 356.88 | 354.99 | 975.72 | 10.63 / 16.49 |
| proactor | batch4 wait0 | 367.10 | 336.84 | 1069.98 | 9.74 / 10.33 |
| proactor | batch4 wait500 | 356.36 | 353.01 | 1126.52 | 11.57 / 18.18 |
| proactor | batch4 wait0 AOF | 326.38 | 330.90 | 1084.04 | 12.14 / 18.40 |

关键平均耗时：

| mode | config | prepared wait us | VSET execute us | VSET commit us | HNSW add us | AOF us | response push us | OrtRun us |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| reactor | no batch | 19 | 167 | 165 | 156 | 0 | 6 | 10463 |
| reactor | batch2 wait0 | 38 | 158 | 155 | 148 | 0 | 6 | 9765 |
| reactor | batch4 wait0 | 45 | 150 | 148 | 142 | 0 | 5 | 9750 |
| reactor | batch4 wait500 | 74 | 150 | 149 | 143 | 0 | 5 | 9717 |
| reactor | batch4 wait0 AOF | 36 | 154 | 153 | 146 | 3 | 5 | 10521 |
| proactor | no batch | 16 | 149 | 148 | 141 | 0 | 6 | 10243 |
| proactor | batch2 wait0 | 27 | 148 | 148 | 141 | 0 | 5 | 9775 |
| proactor | batch4 wait0 | 45 | 155 | 152 | 145 | 0 | 6 | 9458 |
| proactor | batch4 wait500 | 63 | 143 | 140 | 134 | 0 | 5 | 9556 |
| proactor | batch4 wait0 AOF | 38 | 142 | 141 | 134 | 2 | 6 | 10421 |

VSEARCH 对照：

- no batch c16: reactor 715.24 qps, proactor 741.28 qps。
- batch c16: reactor 1047-1099 qps, proactor 975-1126 qps。
- `vsearch_search_avg_us` 约 61-107us，低于 VSET commit，且 VSEARCH
  不走 HNSW insert / AOF / write lock。

## 判断

1. VSET 主要串行成本在 HNSW add / current index add。
   `vset_commit_avg_us` 约 140-165us，其中 `vset_hnsw_add_avg_us`
   约 134-156us，占大头。

2. AOF 不是本轮主瓶颈。
   no-fsync AOF enabled 时 `vset_aof_append_avg_us` 约 2-3us，但 VSET
   c16 throughput 有下降，说明 AOF 对端到端仍有扰动，单次 append 本身不是主成本。

3. response finalize / push 不是主瓶颈。
   response build 平均 0us，push 平均约 5-6us。

4. lock wait 不是主瓶颈，lock hold 基本等于 VSET commit。
   `vset_lock_wait_avg_us` 约 0-2us，`vset_lock_hold_avg_us` 约 141-165us。

5. single executor prepared queue 不是当前主瓶颈。
   `vset_prepared_wait_avg_us` 约 16-74us，远低于 OrtRun，也低于端到端 latency。
   batch wait=500 会抬高 prepared wait，但仍不是主要成本。

6. VSET batch 无稳定收益不是因为 executor 串行段阻塞，而是因为 VSET 仍要支付
   HNSW insert/write path 成本，batch encode 的收益被写路径和 workload 波动抵消。

7. VSEARCH batch 有收益，因为它只走 query embedding + search + response，
   没有 VSET 的 HNSW insert、record ownership transfer、AOF 写入副作用。

## 配置建议

默认策略建议：

```text
VSEARCH batch enabled
VSET batch disabled
```

如果短期仍只有全局 batch 开关：

```conf
embedding_batch_enabled = 1
embedding_batch_max_size = 2
embedding_batch_max_wait_us = 0
```

这是较保守的折中。更合理的下一阶段是引入命令级 batch policy：

```conf
embedding_batch_vset_enabled = 0
embedding_batch_vsearch_enabled = 1
embedding_batch_vset_max_size = 2
embedding_batch_vsearch_max_size = 4
embedding_batch_vset_max_wait_us = 0
embedding_batch_vsearch_max_wait_us = 500
```

本阶段未实现该 policy。

## 是否进入 commit ordering

当前不建议直接进入 commit ordering 实现。

原因：

- `prepared_wait_avg_us` 仍低。
- `vset_lock_wait_avg_us` 低。
- `vset_execute_sync_avg_us` 主要由 HNSW insert 构成。
- 多 executor / commit ordering 不能消除单条 HNSW add 成本，还会引入
  AOF ordering、replication ordering、id_map/HNSW 可见性和 response reorder 复杂度。

更合适的下一步：

- HNSW insert / current index add 优化。
- HNSW 参数调优。
- async / batch index 设计文档。
- AOF batch/async append 仅在 fsync 或真实部署数据证明后再进入。
- 命令级 batch policy，使 VSEARCH 默认 batch、VSET 默认 no batch。

## Sanitizer

本阶段 normal build 和 correctness 已通过。ASAN/UBSAN/TSAN 计划按同一
correctness 脚本运行：

```bash
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
tests/vset_commit_breakdown_stage8_4.sh

TSAN_OPTIONS="halt_on_error=0 exitcode=0" \
tests/vset_commit_breakdown_stage8_4.sh
```

若 TSAN 仅报告既有 shutdown/background race，继续归类为既有问题；新增 stats
使用 atomic 计数，不应引入 data race。
