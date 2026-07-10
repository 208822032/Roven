# B3-core-hnsw-v2-index-only-benchmark report

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`，并读取本阶段要求的：

- `docs/vector_benchmark_readiness/benchmark_plan_real_embedding.md`
- `docs/vector_benchmark_readiness/b1_fixture_cache_generation_report.md`
- `docs/vector_benchmark_readiness/b2_ground_truth_generation_report.md`
- `docs/vector_benchmark_readiness/README.md`
- `docs/vector_benchmark_readiness/index.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；本任务明确要求写入 `docs/vector_benchmark_readiness/`。本报告按任务指定目录输出，同时继续遵守 AGENTS 的核心边界：不修改生产 V 命令行为、不修改 RDB/AOF、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不让 vector core 访问 manager-owned 状态。

## 2. B3 范围

本阶段只实现 core_hnsw_v2 index-only benchmark runner，消费 B1 冻结的真实 embedding cache 和 B2 冻结的 core_flat exact ground truth。

未实现：

- FAISS benchmark。
- product e2e benchmark。
- ONNX 推理。
- ground truth regeneration。
- 参数 sweep。
- build/rebuild 优化。
- HNSW-R。
- 100K scale benchmark。

本阶段只对 1K records / 200 queries 的 smoke 数据集给出 correctness / harness 结论，不宣称大规模性能 SLA。

## 3. 新增/修改文件

```text
scripts/benchmark/generate_real_embedding_hnsw_v2_benchmark.c
tests/vector_real_embedding_hnsw_v2_benchmark_smoke_test.sh
docs/vector_benchmark_readiness/b3_core_hnsw_v2_index_only_benchmark_report.md
docs/vector_benchmark_readiness/README.md
docs/vector_benchmark_readiness/index.md
Makefile
```

`Makefile` 仅新增显式 opt-in target：

```text
gen-embedding-hnsw-v2-benchmark
smoke-vector-real-embedding-hnsw-v2-benchmark
```

生产 `src/` / `include/` 未修改，product behavior 未改变。

## 4. 输入 provenance

Dataset：

```text
/tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/
```

B3 运行前先执行并通过：

```text
build/tools/bin/generate_real_embedding_cache validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
build/tools/bin/generate_real_embedding_groundtruth validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
```

B3 runner 使用并校验：

```text
manifest.json
embeddings.f32
query_embeddings.f32
records.jsonl
queries.jsonl
groundtruth.jsonl
groundtruth.manifest.json
```

绑定 checksum：

```text
manifest.json            0176cdcf05070fe96db39ac40897704a284af7c2f7a040a901539517b3ca6f2b
embeddings.f32           57dc5611ee5d30a6da11a7efe2e0393a19da106bad5dae4f63d5cd7a5a4506ce
query_embeddings.f32     fc3117ef25dfd8e2137b0df836d776e697c39c3934e8b4018b0b9f2c5c5058c6
records.jsonl            720bf45ae6b543106bacd26e728697e5ab4a28c5d30b06a997dff575baa80df2
queries.jsonl            e8c8dbbd7fb4a9c82f720f67d99f6893d97f15b601bc08444eea81639eea1a9c
groundtruth.jsonl        b53ddf15773402072c2e27deeba00af5dc837648bddd4e64091cd4426603f3c9
groundtruth.manifest.json 512b0e02eb4bddda8b9cf1aaa4c12c499cd883e4ae1f311d11e473b2d086f4f8
```

B3 runner 不链接 `libonnxruntime`，不调用 ONNX，不重新生成 ground truth。

## 5. core_hnsw_v2 调用链

runner 使用项目现有 core_hnsw_v2 API：

```text
kvs_hnsw_v2_index_init(&index, dim=512, KVS_VECTOR_METRIC_IP)
  -> kvs_hnsw_v2_index_set_seed(&index, deterministic_seed)
  -> kvs_hnsw_v2_index_reserve(&index, record_count)
  -> kvs_hnsw_v2_index_add(&index, record_id=row, vector, dim)
  -> kvs_hnsw_v2_graph_stats_refresh(&index)
  -> kvs_hnsw_v2_memory_refresh(&index)
  -> kvs_hnsw_v2_index_info(&index, &core_info)
  -> per query: kvs_hnsw_v2_index_search(&index, query, dim, topK, index.ef_search, hits, &hit_count)
  -> kvs_hnsw_v2_index_destroy(&index)
```

只处理 `record_id` / `vector` / `score` / index stats，不经过 manager、RESP、record store、scope/TTL/metadata、embedding runtime 或 RDB/AOF。

## 6. 参数

使用当前冻结默认值，不重新调参：

```text
M              = 48
M0             = 96
efConstruction = 512
efSearch       = 1024
metric         = ip_normalized
dim            = 512
topK           = 10
record_count   = 1000
query_count    = 200
```

`params_source = core_hnsw_v2_frozen_defaults`。runner 仅设置 deterministic seed 以稳定 smoke 输出，不改变默认 HNSW 参数。

## 7. 输出 artifact

B3 输出写入同一 `/tmp` dataset 目录：

```text
hnsw_v2_benchmark_result.json
hnsw_v2_per_query.jsonl
```

写入方式：

- 默认 fail-fast：结果文件已存在时拒绝覆盖。
- 使用临时文件 `.tmp.*.<pid>`。
- per-query 文件先 rename，result manifest 最后 rename；result manifest 是发布完成标志。
- 失败路径清理临时文件。

`validate` 子命令只做 schema / provenance / checksum / query count / metric range 校验，不构建 v2，不调用 ONNX。

## 8. 质量结果

正式 B3 benchmark 结果：

```text
recall@1                  = 1.000000
recall@10                 = 1.000000
top1_match_rate           = 1.000000
topk_exact_match_rate     = 1.000000
minimum per-query recall  = 1.000000
queries_checked           = 200
```

smoke test 使用 Python 从 `hnsw_v2_per_query.jsonl` 和 `groundtruth.jsonl` 独立复算 recall，并检查：

- 200 条 query 均有结果。
- 每条 query 返回 topK=10。
- `record_id` 可回表到 `records.jsonl`。
- rank 从 1 连续到 10。
- score 为有限数且按降序排列。
- 无重复 `record_id`。
- runner 写入的 per-query recall 与独立复算一致。
- deterministic quality 和 per-query 输出在重生成后保持一致。

## 9. 性能结果

正式 B3 benchmark 结果：

```text
cache_load_us             = 1,305
index_build_us            = 2,059,178
build_avg_us_per_record   = 2,059.178
search_total_us           = 141,088
search_avg_us             = 705.440
search_p50_us             = 703
search_p95_us             = 724
search_p99_us             = 761
QPS                       = 1,417.555
total_us                  = 2,207,668
```

说明：这些数字来自 1K smoke dataset 和当前机器的一次运行，只用于证明 harness 可机器校验和记录完整指标，不作为大规模性能 SLA。

## 10. index stats

```text
memory_bytes              = 8,502,864
memory_bytes_source       = estimated
size                      = 1000
active_count              = 1000
deleted_count             = 0
capacity                  = 1000
duplicate_edge_count      = 0
degree_overflow_count     = 0
graph_counters_source     = core_build_event_counter
```

## 11. 测试结果

```text
$ make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
  PASS，构建完成（kvstore / ebpf_syncd / eBPF object）

$ build/tools/bin/generate_real_embedding_cache validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
  PASS，record_count=1000 query_count=200 dim=512

$ build/tools/bin/generate_real_embedding_groundtruth validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
  PASS，query_count=200 topk=10 record_count=1000 dim=512

$ tests/vector_real_embedding_hnsw_v2_benchmark_smoke_test.sh
  PASS，ALL CHECKS PASSED

$ build/tools/bin/generate_real_embedding_hnsw_v2_benchmark generate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
  PASS，生成 hnsw_v2_benchmark_result.json / hnsw_v2_per_query.jsonl

$ build/tools/bin/generate_real_embedding_hnsw_v2_benchmark validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
  PASS，recall_at_10=1.0000
```

Smoke test 覆盖：

- B1/B2 validate 通过。
- 所有 query 均返回 topK=10。
- record_id 回表。
- rank / score 合法。
- 无重复 record_id。
- 独立脚本复算 recall。
- deterministic quality / per-query 输出一致。
- corrupt cache 拒绝执行。
- corrupt ground truth 拒绝执行。
- corrupt B3 result validate 失败。
- disposable artifact copy 位于 `/tmp`，未修改 B1/B2 正式产物。

## 12. 限制

- 数据规模只有 1000 records / 200 queries；当前结论是 smoke/correctness 与 harness readiness。
- `memory_bytes` 来源为 core_hnsw_v2 当前 estimated stats，不应与 measured memory 混用。
- 真实 embedding cache 和 ground truth 仍以未签名 manifest 为信任根；checksum 能捕获意外损坏，但不能防御同时一致篡改 manifest 与 artifact。
- B3 只覆盖 core_hnsw_v2 index-only；FAISS index-only baseline 留给 B4。

## 13. B4 输入契约

B4 必须复用同一份 B1/B2 输入和 B3 已冻结的评价口径：

```text
dataset_id = qa_zh_1k_200_bge_small_zh_v1_5
dim = 512
metric = ip_normalized
topK = 10
ground truth = B2 core_flat exact IP
artifact root = /tmp/kvlite_vector_benchmark_artifacts/real_embedding/
```

B4 不应恢复 FAISS product backend，也不应使用 removed backend name；FAISS 只能作为 index-only external baseline。

## 14. 检查结果

```text
$ git diff --check
  PASS，无输出

$ git status --short
  M Makefile
  ?? docs/vector_benchmark_readiness/
  ?? scripts/benchmark/generate_real_embedding_cache.c
  ?? scripts/benchmark/generate_real_embedding_groundtruth.c
  ?? scripts/benchmark/generate_real_embedding_hnsw_v2_benchmark.c
  ?? tests/vector_real_embedding_cache_smoke_test.sh
  ?? tests/vector_real_embedding_groundtruth_smoke_test.sh
  ?? tests/vector_real_embedding_hnsw_v2_benchmark_smoke_test.sh
```

## 15. Decision block

```text
B3-core-hnsw-v2-index-only-benchmark decision:
  production_code_changed = no
  product_behavior_changed = no
  tests_changed = yes
  makefile_changed = yes
  docs_changed = yes
  b1_cache_validation_passed = yes
  b2_groundtruth_validation_passed = yes
  onnx_inference_rerun = no
  groundtruth_regenerated = no
  core_hnsw_v2_used = yes
  record_count = 1000
  query_count = 200
  dim = 512
  topk = 10
  recall_at_1 = 1.000000
  recall_at_10 = 1.000000
  top1_match_rate = 1.000000
  search_avg_us = 705.440
  search_p95_us = 724
  build_total_us = 2059178
  benchmark_schema_frozen = yes
  artifacts_outside_repo = yes
  ready_for_B4_faiss_baseline = yes
  recommended_next = B4-FAISS-index-only-baseline
```
