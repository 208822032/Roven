# B-scale-2: T2Ranking 10K exact ground truth report

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`。本阶段只改 benchmark 工具、smoke test 和 `docs/vector_benchmark_readiness/` 文档；未修改生产 V 命令代码，未修改 RDB/AOF，未修改 `VSEARCH` response，未修改 `kvs_vector_index_ops_t`，未修改默认 backend 或默认 HNSW 参数。

冲突说明：`AGENTS.md` 的历史阶段模板要求报告写入 `docs/vector_core_reset/`；当前 benchmark readiness 路线要求所有 benchmark 文档集中在 `docs/vector_benchmark_readiness/`。本报告按任务指定目录输出，同时保持 AGENTS 的生产边界。

## 2. 阶段目标和边界

目标：复用并最小扩展现有 B2 `scripts/benchmark/generate_real_embedding_groundtruth.c`，读取 B-scale-1 冻结的 T2Ranking 10K real embedding cache，生成 200 query 的 exact topK=10 ground truth。

本阶段未执行：

- B-scale-0 子集重新生成。
- T2Ranking 真实 ONNX embedding 重新生成。
- 使用 qrels 生成或修正 ANN exact ground truth。
- core_hnsw_v2 / FAISS benchmark。
- 产品 V 命令压测。

## 3. 只读审计摘要

现有 B2 工具已经通过 `kvs_flat_index_*` 复用项目 core_flat exact IP 路径，并写出 `groundtruth.jsonl` / `groundtruth.manifest.json`。B3/B4 下游依赖的关键字段是：

```text
groundtruth.jsonl:
  query_id
  query_row
  topk
  metric
  neighbors[].rank
  neighbors[].record_id
  neighbors[].score

groundtruth.manifest.json:
  params.record_count
  params.query_count
  params.dim
  params.topk
  params.metric
  outputs.groundtruth_jsonl.sha256 / size_bytes / rows
```

本阶段保留上述 schema 和 numeric `record_id == embeddings.f32 row == records.jsonl.id` 语义。

## 4. 实际修改内容

修改：

```text
scripts/benchmark/generate_real_embedding_groundtruth.c
tests/vector_real_embedding_groundtruth_smoke_test.sh
docs/vector_benchmark_readiness/README.md
docs/vector_benchmark_readiness/index.md
```

新增：

```text
docs/vector_benchmark_readiness/b_scale_2_t2ranking_10k_ground_truth_report.md
```

工具改造：

- 新增外部输入/输出路径：`--cache-dir`、`--output-dir`。
- 新增 `--top-k` 别名，保留旧 `--topk`。
- `generate --cache-dir ... --output-dir ...` 允许完整既有输出重复运行，若新生成 `groundtruth.jsonl` checksum 一致则返回 `status=unchanged core_outputs_identical=true`。
- `validate --output-dir ...` 可从 ground truth manifest 的 `source.cache_dir` 回读 provenance；也支持显式 `--cache-dir`。
- 继续保留旧 B2 `--dataset-id/--out-root` 调用方式和已存在输出 fail-fast 语义。
- 对 cache 文件、row/id 映射、finite、normalized、topK 范围做运行前校验。
- `core_flat` hits 在写入前按 `score desc, numeric record_id asc` 排序，避免 tie 时依赖 heap/扫描偶然顺序。
- 独立 double precision brute-force 对全部 query 和全部 document 复算 topK，并逐 rank 校验 record_id 与 score tolerance。
- 扩大路径缓冲，修复长外部路径触发的 `snprintf` truncation 编译告警。

Smoke test 改造：

- 保留旧 B2 1K disposable copy smoke。
- 移除旧脚本中缺失 1K B1 fixture 时自动调用 ONNX 的 fallback；现在缺失即失败，避免 B2 smoke 隐式重跑 embedding。
- 新增小型 synthetic external cache/output 覆盖：完全相同 score tie、重复运行 unchanged、duplicate id、wrong dim、count mismatch、non-finite vector、topK 大于 record_count、失败不留下半成品。

## 5. 输入 cache 和 qrels

Embedding cache：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding
```

输入参数：

```text
record_count = 10000
query_count = 200
dim = 512
dtype = float32
normalized = true
metric = ip_normalized
top_k = 10
```

输入 checksum：

| file | bytes | sha256 |
|---|---:|---|
| `manifest.json` | 4010 | `ff349b42dc6ed07fc492b0163df536c89783c0998cbc61be0ec974702953ac9b` |
| `embeddings.f32` | 20480000 | `aeead23e186bc73da2d92ce52885bf5e1763fbed5cb430059ba65abf21c77fc1` |
| `query_embeddings.f32` | 409600 | `2b1d3e5952051dcf4eee938b6edf3454fef1660f16aa0e319d9e3d53e8dca318` |
| `records.jsonl` | 34409206 | `7372ebe3fd264dd2bf5276cc07952c68d1bc2b9500f4d19a8350b7adfbfdf981` |
| `queries.jsonl` | 28094 | `d4ced627ace77b2f806a7206944be98c0b7d9b170c84038dc9a4ca1cfaad45b4` |

T2Ranking qrels：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/qrels.retrieval.dev.200.tsv
rows = 1132 including header, positive pairs = 1131
sha256 = 97221f7ca305c817e41da2e1cee3e136b38e3fb563586b2ed5e06cba7563af1f
```

qrels 没有参与 ANN exact topK 生成、排序、过滤或校验。本阶段 exact ground truth 只表示 query embedding 对全部 10000 document embeddings 的 exact normalized inner-product topK；qrels 只作为后续 semantic metrics 输入。

## 6. exact ground truth 实现路径

调用链：

```text
kvs_config_init_default(&g_cfg)
  -> g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM
  -> mem_set_backend(KVS_MEM_BACKEND_SYSTEM)
  -> kvs_flat_index_init(dim=512, KVS_VECTOR_METRIC_IP)
  -> kvs_flat_index_reserve(record_count=10000)
  -> kvs_flat_index_add(record_id=row, embeddings[row])
  -> per query: kvs_flat_index_search(query_embeddings[row], topK=10)
  -> qsort hits by score desc, record_id asc
  -> write groundtruth.jsonl
  -> independent double brute-force crosscheck over all 10000 documents
```

`record_id` 继续是 numeric row id；T2Ranking 原始 pid 通过 `records.jsonl` 的 `pid` / `external_id` 回表，不改变下游 B3/B4 的 numeric id 契约。

Tie-break 冻结：

```text
primary = score descending
tie     = numeric record_id ascending
```

真实 10K 输出中没有精确 score tie：

```text
tie_query_count = 0
tie_result_count = 0
```

Synthetic smoke fixture 主动构造了 tie，并验证 topK 顺序为 `score desc, record_id asc`。

## 7. 生成命令和结果

命令：

```bash
build/tools/bin/generate_real_embedding_groundtruth generate \
  --cache-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding \
  --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth \
  --top-k 10
```

输出：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth/groundtruth.jsonl
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth/groundtruth.manifest.json
```

生成日志摘要：

```text
[b1-cache] record_count=10000 query_count=200 dim=512 metric=ip_normalized (checksums + row/id mapping verified)
[search] queries=200 topk=10 search_total_us=789999 search_avg_us=3949
[crosscheck] queries_checked=200 strict_order_checked=200 max_score_abs_diff=9.32e-07 mean_score_abs_diff=1.79e-07 tie_query_count=0 tie_result_count=0 (set/score/order all consistent)
[publish] dataset_id=b_scale_1_10k_embedding groundtruth=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth/groundtruth.jsonl rows=200
```

Timing：

```text
cache_load_us = 10955
index_build_us = 13343
search_total_us = 789999
search_avg_us = 3949
crosscheck_us = 1026163
total_us = 1897998
exact search throughput = 253.165 queries/s
end-to-end generator throughput including crosscheck = 105.374 queries/s
```

## 8. 输出文件

| file | rows | bytes | sha256 |
|---|---:|---:|---|
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth/groundtruth.jsonl` | 200 | 112478 | `cee138ab689c73475e4775d2311026eacff41fad57309ef063b41b0c5b9f444b` |
| `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth/groundtruth.manifest.json` | - | 1824 | `0d338c6a7e19e0b5faaf726f525e208abf1c7905d58fa44c7846c39f3402d783` |

`groundtruth.jsonl` schema 与旧 B2 保持兼容：

```json
{"query_id":"q000000","query_row":0,"topk":10,"metric":"ip_normalized","neighbors":[{"rank":1,"record_id":248,"score":0.714817226}]}
```

## 9. 独立 double verification

全部 200 个 query 均用 double precision brute-force 对 10000 个 document embedding 复算 exact IP topK。

```text
verified_query_count = 200
queries_checked = 200
top_k = 10
set_match_all = true
record_id_match_all = true
strict_order_checked = 200
strict_order_match_all = true
max_score_abs_diff = 9.322285e-07
mean_score_abs_diff = 1.79409801e-07
score_tolerance = 1e-4
tie_eps = 1e-6
tie_query_count = 0
tie_result_count = 0
```

## 10. 确定性和 validate-only

Validate：

```bash
build/tools/bin/generate_real_embedding_groundtruth validate \
  --cache-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding \
  --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth
```

结果：

```text
[validate] PASS dataset_id=b_scale_1_10k_embedding query_count=200 topk=10 record_count=10000 dim=512
```

不显式传 `--cache-dir` 的 provenance validate 也通过：

```text
[validate] PASS dataset_id=b_scale_1_10k_embedding query_count=200 topk=10 record_count=10000 dim=512
```

重复运行同一 generate 命令：

```text
[publish] dataset_id=b_scale_1_10k_embedding groundtruth=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth/groundtruth.jsonl rows=200 status=unchanged core_outputs_identical=true
```

说明：真实 10K 的 `groundtruth.jsonl` byte-level checksum 保持一致；已有完整输出不会被覆盖。

## 11. 测试结果

```text
$ make gen-embedding-groundtruth
PASS（无编译告警）

$ tests/vector_real_embedding_groundtruth_smoke_test.sh
=== B2 real embedding ground truth smoke test: ALL CHECKS PASSED ===

$ build/tools/bin/generate_real_embedding_cache validate \
    --dataset-id b_scale_1_10k_embedding \
    --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding
[validate] PASS dataset_id=b_scale_1_10k_embedding record_count=10000 query_count=200 dim=512

$ build/tools/bin/generate_real_embedding_groundtruth validate \
    --cache-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding \
    --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth
[validate] PASS dataset_id=b_scale_1_10k_embedding query_count=200 topk=10 record_count=10000 dim=512

$ build/tools/bin/generate_real_embedding_groundtruth validate \
    --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth
[validate] PASS dataset_id=b_scale_1_10k_embedding query_count=200 topk=10 record_count=10000 dim=512
```

Smoke 覆盖：

- 旧 B2 1K disposable copy 调用方式无回归。
- 旧 B2 已存在输出 fail-fast。
- 外部 `--cache-dir/--output-dir`。
- deterministic tie-break。
- 重复生成 unchanged。
- cache sha mismatch。
- corrupted ground truth validate 失败。
- duplicate id、wrong dim、count mismatch、non-finite vector、topK 大于 record_count。
- 失败路径不留下 `groundtruth.jsonl` / `groundtruth.manifest.json` 半成品。

说明：第一次本地运行旧 smoke 时，旧脚本因 `/tmp` 1K B1 fixture 缺失触发了遗留的 ONNX auto-generation fallback；该行为已在本阶段移除，并重新运行 smoke 确认不再隐式调用 ONNX。T2Ranking B-scale-2 真实 ground truth 生成未调用 ONNX。

## 12. 当前限制和风险

- 本阶段只覆盖 T2Ranking 10K corpus；100K exact ground truth 尚未生成。
- `groundtruth.manifest.json` 仍包含 `created_at_utc` 和 timing，因此 manifest byte checksum 不作为重复生成的稳定指标；稳定性以 `groundtruth.jsonl` 和核心 provenance/schema 字段为准。
- qrels semantic positives 与 embedding exact topK 是两套不同口径；后续 benchmark 报告必须避免把 semantic recall 与 ANN exact recall 混用。
- B-scale-3 需要让 core_hnsw_v2 / FAISS runners 支持分离的 `cache_dir` 与 `groundtruth_dir`，或显式读取本阶段 output dir；本阶段不提前实现。

## 13. 下一阶段

推荐进入：

```text
B-scale-3 — T2Ranking 10K Core HNSW v2 and FAISS Benchmark
```

B-scale-3 输入契约：

```text
cache_dir = /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding
groundtruth_dir = /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth
record_count = 10000
query_count = 200
dim = 512
metric = ip_normalized
top_k = 10
```

