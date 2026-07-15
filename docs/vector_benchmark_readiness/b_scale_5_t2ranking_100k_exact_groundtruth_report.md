# B-scale-5: T2Ranking 100K Exact Ground Truth

## 1. 范围与边界

本阶段读取 B-scale-4 冻结的 100K real embedding cache，复用 B-scale-2 exact-ground-truth runner 生成 normalized inner-product topK=10 ground truth。未调用 ONNX，未运行 HNSW、FAISS 或 product e2e，未修改生产向量代码、RDB/AOF、VSEARCH response 或 index ops。

输入和输出：

```text
cache_dir=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_4_100k_embedding
output_dir=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_5_100k_groundtruth
record_count=100000
query_count=200
dim=512
topk=10
metric=ip_normalized
total_scores=20000000
```

## 2. Runner 审计

现有 `generate_real_embedding_groundtruth.c` 已原生满足 100K 主路径：

- 支持独立 `--cache-dir` 和 `--output-dir`。
- record/query/dim/metric 从 cache `manifest.json` 读取，并以文件 checksum、shape、row/id mapping 交叉约束；没有 10K count 硬编码。
- core truth 由 `kvs_flat_index_search()` 的 float exact-IP 路径产生；oracle 由独立 double precision dot-product 对原始 cache 复算。
- core hits 和 oracle 都显式按 `score descending, numeric record_id ascending` 排序。
- oracle 只复用一个 `record_count` 长度的工作数组；逐 query 计算、排序、核对后复用，不保存 200 x 100K score matrix。
- groundtruth data 和 manifest 通过临时文件生成，data 先 rename、manifest 最后 rename；失败清理半成品。

审计发现的唯一缺口是 B-scale-2 manifest 没有记录最大相对 score difference、最大差异位置和显式 nonfinite count/topK-id alias。为满足 B-scale-5 机器审计，只添加这些 crosscheck 字段；JSONL、schema_version、排序、容差、计算和发布语义不变。

## 3. 修改文件

```text
scripts/benchmark/generate_real_embedding_groundtruth.c
  generator version 1 -> 2
  add max relative diff / locations / nonfinite count / topk_id_match_all

tests/vector_real_embedding_groundtruth_smoke_test.sh
  assert additive crosscheck fields

docs/vector_benchmark_readiness/b_scale_5_t2ranking_100k_exact_groundtruth_report.md
docs/vector_benchmark_readiness/README.md
docs/vector_benchmark_readiness/index.md
```

未修改 Makefile 或其它 benchmark/production 实现。

## 4. 正式命令

正式结果先写隐藏 work result，完整 validate 后再通过同文件系统 rename 发布到指定目录：

```bash
/usr/bin/time -v \
  build/tools/bin/generate_real_embedding_groundtruth generate \
  --cache-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_4_100k_embedding \
  --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/.b_scale_5_100k_groundtruth_work/result \
  --top-k 10
```

输入 provenance：

```text
cache manifest sha256=f817924c115eef731d7f1e7d0ca94661a4e753715a4b025639fa3746393e1ce1
embeddings sha256=970ce35b4fd60e82f9ed06a81ef97da5fa02ba97cd4ae848f76ad10293ea0994
query embeddings sha256=2b1d3e5952051dcf4eee938b6edf3454fef1660f16aa0e319d9e3d53e8dca318
records sha256=a8ed28ef3cb81735a7d3aab8f87ff5201a3d935daff8de4554b1270bd02a158e
queries sha256=d4ced627ace77b2f806a7206944be98c0b7d9b170c84038dc9a4ca1cfaad45b4
```

## 5. 性能与资源

```text
cache_load_us=98002
index_build_us=125562
core_flat_search_total_us=8046150
core_flat_search_avg_us_per_query=40230
independent_bruteforce_total_us=10822867
independent_bruteforce_avg_us_per_query=54114.335
runner_total_us=19607295
external_wall_seconds=19.63
peak_rss_kb=414084
swaps=0
```

peak RSS 包含 100K base cache、core_flat derived copy、200 query vectors 和单个 100K oracle work array；没有完整 score matrix。

## 6. Cross-check 结果

```text
verified_query_count=200
queries_checked=200
set_match_all=true
record_id_match_all=true
topk_id_match_all=true
strict_order_checked=200
strict_order_match_all=true
nonfinite_score_count=0
score_tolerance=0.0001
```

Score difference：

```text
max_score_abs_diff=8.35264581e-07
max_abs_diff_location:
  query_row=159
  record_id=952
  rank=1

max_score_rel_diff=1.23805135e-06
max_rel_diff_location:
  query_row=107
  record_id=93189
  rank=8

mean_score_abs_diff=1.85167829e-07
tie_query_count=1
tie_result_count=2
```

唯一真实 topK tie 仍通过 numeric record_id ascending 门禁。

## 7. 输出

```text
groundtruth.jsonl
  rows=200
  size_bytes=113679
  sha256=7383a3803ab3e2caea8f85e7cef28bfb8982ff7b3f3e8251e072ffb5b86e9cbb

groundtruth.manifest.json
  size_bytes=2077
  sha256=8bc82b9bb4e258eda0d8d70811059f0662c84c5b56f06197d965d8391ba1340c
```

最终目录只包含上述两个文件。正式发布前后均执行 validate；显式 `--cache-dir` 和 manifest provenance 两种 validate 模式均通过。

## 8. 测试

```text
make gen-embedding-groundtruth
PASS

tests/vector_real_embedding_groundtruth_smoke_test.sh
PASS

B-scale-4 cache validate
PASS

B-scale-5 work result validate, explicit/provenance modes
PASS

B-scale-5 final result validate
PASS

independent manifest + all-200-JSONL audit
PASS
```

VM 升级后历史 1K `/tmp` smoke cache 不存在；测试从已冻结 B-scale-1 cache 创建 disposable 1K byte-prefix fixture，未调用 ONNX，测试结束后已清理。

## 9. 限制与下一阶段

- 该 artifact 是 embedding-space exact normalized-IP topK，不是 T2Ranking qrels semantic relevance ground truth。
- 当前结果证明 exact oracle 和 runner correctness，不是 ANN recall、build/search SLA 或 product e2e 结论。
- manifest 含生成时间和 timing，正式稳定输入以 source/output checksum 和 schema 字段为准。

下一阶段：

```text
B-scale-6 - 100K core_hnsw_v2 / FAISS index-only benchmark
```

```text
B-scale-5 decision:
  production_code_changed = no
  benchmark_runner_changed = yes
  output_schema_version_changed = no
  groundtruth_jsonl_schema_changed = no
  tests_changed = yes
  docs_changed = yes
  runner_native_100k_support = yes
  core_flat_exact_used = yes
  independent_double_bruteforce_used = yes
  full_score_matrix_saved = no
  strict_order_match_all = yes
  topk_id_match_all = yes
  nonfinite_score_count = 0
  atomic_publish_passed = yes
  ready_for_B_scale_6 = yes
  recommended_next = B-scale-6
```
