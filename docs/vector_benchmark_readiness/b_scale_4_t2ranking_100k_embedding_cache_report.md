# B-scale-4: T2Ranking 100K Real Embedding Cache

## 1. 范围与约束

本阶段直接复用 B-scale-4A 已完成的连续行分片、resume、checksum、全局顺序 merge 和 validate 能力，生成一次可复用的 T2Ranking 100K real embedding cache。未修改 benchmark/生产代码、模型、512 token cap、float32、L2 normalization 或输入顺序；未执行 ground truth、HNSW、FAISS 或 product e2e。

AGENTS.md 的 manager/vector_core 边界、RDB/AOF、VSEARCH response 和 index ops 禁止项均未触及。本阶段代码和测试修改为零，只新增本报告并更新本目录索引。

## 2. 输入与配置

输入继承 B-scale-0/B-scale-1 provenance：

```text
records TSV:
  /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/collection.100k.tsv
  rows=100000
  sha256=738f03d1a1b063a1957fb627669c9520b75b4745e1475d2740eabcc1b630736c

base cache:
  /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding
  reused record range=[0,10000)
  query_count=200, query cache reused

generated range=[10000,100000)
processes=4
threads_per_process=2
batch_size=32
dim=512
dtype=float32
normalization=L2
metric=ip_normalized
model=bge-small-zh-v1.5 / ONNX Runtime 1.24.4
```

四个连续 shard 均为 22,500 records：

```text
[10000,32500)
[32500,55000)
[55000,77500)
[77500,100000)
```

## 3. 生成与资源结果

正式 90K ONNX 生成结果：

```text
generated_record_count=90000
worker/shard_count=4
generation_wall_seconds=2544.259596
end_to_end_wall_seconds=2550.651653
aggregate_records_per_second=35.373749
aggregate_cpu_utilization_percent=756.907
```

资源监控：

```text
worker peak RSS KiB:
  1982632
  1962712
  1983260
  1982012
sum_worker_peak_rss_kb=7910616
sampled_aggregate_peak_rss_kb=7883236
minimum_available_memory_kb=6386420
maximum_swap_used_kb=0
pswpin_delta_pages=0
pswpout_delta_pages=0
worker_failure_or_oom=no
```

运行期间未触发 3 GiB available-memory 停止线，没有 swap I/O、OOM 或 worker 异常。四个 shard 完成后在 work root 内确定性合并，再通过同文件系统原子 rename 发布到：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_4_100k_embedding
```

## 4. 输出与 SHA-256

```text
manifest.json
  size=8690
  sha256=f817924c115eef731d7f1e7d0ca94661a4e753715a4b025639fa3746393e1ce1

embeddings.f32
  size=204800000
  shape=[100000,512]
  sha256=970ce35b4fd60e82f9ed06a81ef97da5fa02ba97cd4ae848f76ad10293ea0994

records.jsonl
  size=330571480
  rows=100000
  sha256=a8ed28ef3cb81735a7d3aab8f87ff5201a3d935daff8de4554b1270bd02a158e

query_embeddings.f32
  size=409600
  shape=[200,512]
  sha256=2b1d3e5952051dcf4eee938b6edf3454fef1660f16aa0e319d9e3d53e8dca318

queries.jsonl
  size=28094
  rows=200
  sha256=d4ced627ace77b2f806a7206944be98c0b7d9b170c84038dc9a4ca1cfaad45b4
```

最终 artifact 磁盘占用约 512 MiB；shard、日志和资源采样保留在同一 artifact 根目录下的隐藏 work root，不进入 git。

## 5. 完整性验证

以下门禁全部通过：

- `record_count=100000`、`query_count=200`、`dim=512`。
- `embeddings.f32=100000 * 512 * 4=204800000 bytes`。
- 全量 f32 finite / L2 norm validate PASS。
- `records.jsonl` 的 global row、numeric id、pid、external_id 和 source line 与 `collection.100k.tsv` 逐行一致。
- 四个 shard range 连续，无 gap / overlap；每个 shard checksum validate PASS。
- merged validate 在 work path 和最终发布 path 均 PASS。
- 前 10K `embeddings.f32` bytes 与 B-scale-1 完全一致，prefix SHA-256 为 `aeead23e186bc73da2d92ce52885bf5e1763fbed5cb430059ba65abf21c77fc1`。
- 前 10K `records.jsonl` 与 B-scale-1 完全一致，prefix SHA-256 为 `7372ebe3fd264dd2bf5276cc07952c68d1bc2b9500f4d19a8350b7adfbfdf981`。
- `query_embeddings.f32` 和 `queries.jsonl` 与 B-scale-1 byte-identical。
- 原子发布前不存在最终目录；发布后最终路径 validate PASS，没有暴露半成品。

## 6. Shard 重跑决策

固定 `[10000,32500)` 完整 shard 重跑曾按相同参数启动，但单 shard 需要再次执行 22,500 条 ONNX 推理，成本接近 42 分钟。运行约 `1769.2s` 后，基于以下已有证据判定该验证重复且成本过高，按用户决策终止并跳过完整 rerun：

- B-scale-4A 多组 process/thread/batch 输出已经 byte-identical。
- 正式四个 shard 均完成 checksum/finite/norm validate。
- 100K merged、前 10K prefix、query cache 和全局 row/id/pid 均独立通过校验。

终止只作用于 rerun ONNX 进程；对应 `.tmp` 已清理。原四个 shard、merged cache、正式资源采样、rerun 日志和采样均保留且未改写。本阶段不把部分 rerun 作为确定性通过证据，也不再启动 ONNX 重跑。

## 7. 测试结果

```text
make gen-embedding-cache
PASS

tests/vector_real_embedding_cache_parallel_shard_smoke_test.sh
PASS

B-scale-1 base cache validate
PASS

4 formal shards validate
PASS

100K merged validate before/after atomic publish
PASS

prefix/query/global-order independent checks
PASS
```

## 8. 下一阶段

100K real embedding cache 已冻结并可作为 exact ground truth 输入。下一阶段：

```text
B-scale-5 - T2Ranking 100K Exact Ground Truth
```

```text
B-scale-4 decision:
  production_code_changed = no
  benchmark_code_changed = no
  tests_changed = no
  docs_changed = yes
  base_10k_reused = yes
  query_cache_reused = yes
  onnx_generated_record_count = 90000
  formal_shard_count = 4
  record_count = 100000
  query_count = 200
  dim = 512
  finite_norm_validation_passed = yes
  global_row_id_pid_order_passed = yes
  shard_gap_overlap_check_passed = yes
  base_prefix_byte_identical = yes
  query_cache_byte_identical = yes
  merged_validate_passed = yes
  atomic_publish_passed = yes
  full_shard_rerun_completed = no
  full_shard_rerun_skipped_as_redundant = yes
  groundtruth_or_ann_executed = no
  ready_for_B_scale_5 = yes
  recommended_next = B-scale-5
```
