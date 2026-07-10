# B4-FAISS-index-only-baseline report

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`，并读取本阶段要求的：

- `docs/vector_benchmark_readiness/benchmark_plan_real_embedding.md`
- `docs/vector_benchmark_readiness/b1_fixture_cache_generation_report.md`
- `docs/vector_benchmark_readiness/b2_ground_truth_generation_report.md`
- `docs/vector_benchmark_readiness/b3_core_hnsw_v2_index_only_benchmark_report.md`
- `docs/vector_benchmark_readiness/README.md`
- `docs/vector_benchmark_readiness/index.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；本任务明确要求写入 `docs/vector_benchmark_readiness/`。本报告按任务指定目录输出，同时继续遵守 AGENTS 的核心边界：不修改生产 V 命令行为、不修改 RDB/AOF、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不恢复 FAISS product backend。

## 2. B4 范围

本阶段只新增 FAISS index-only baseline runner，复用 B1 embedding cache 与 B2 core_flat exact ground truth。

未实现：

- FAISS product backend 恢复。
- product e2e benchmark。
- ONNX 推理。
- ground truth regeneration。
- 参数 sweep。
- build/rebuild 优化。
- HNSW-R。
- 100K scale benchmark。

本阶段只对 1K records / 200 queries 的 smoke 数据集给出 parity / harness 结论，不宣称大规模性能 SLA。

## 3. 新增/修改文件

```text
scripts/benchmark/generate_real_embedding_faiss_benchmark.cpp
tests/vector_real_embedding_faiss_benchmark_smoke_test.sh
docs/vector_benchmark_readiness/b4_faiss_index_only_baseline_report.md
docs/vector_benchmark_readiness/README.md
docs/vector_benchmark_readiness/index.md
Makefile
```

`Makefile` 仅新增显式 opt-in target：

```text
gen-embedding-faiss-benchmark
smoke-vector-real-embedding-faiss-benchmark
```

生产 `src/` / `include/` 未修改，product behavior 未改变。

## 4. FAISS 调用链和线程设置

B4 runner 直接使用 vendored FAISS C++ API：

```text
omp_set_num_threads(1)

faiss::IndexFlatIP(dim=512)
  -> add(1000, embeddings)
  -> per query: search(1, query, topK=10, distances, labels)

faiss::IndexHNSWFlat(dim=512, M=48, METRIC_INNER_PRODUCT)
  -> hnsw.efConstruction = 512
  -> hnsw.efSearch = 1024
  -> actual_level0_neighbor_capacity = hnsw.nb_neighbors(0)
  -> add(1000, embeddings)
  -> per query: search(1, query, topK=10, distances, labels)
```

线程设置：

```text
thread_count = 1
OpenMP = omp_set_num_threads(1)
```

FAISS HNSW 实际 level-0 neighbor capacity 由运行时读取：

```text
requested_M = 48
actual_level0_neighbor_capacity = 96
```

本阶段没有假设 FAISS 的 M0 一定等于 96；结果中记录的是 `hnsw.nb_neighbors(0)` 的实际值。

## 5. 输入 provenance

Dataset：

```text
/tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/
```

B4 运行前先执行并通过：

```text
build/tools/bin/generate_real_embedding_cache validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
build/tools/bin/generate_real_embedding_groundtruth validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
```

B4 runner 使用并校验：

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
manifest.json             0176cdcf05070fe96db39ac40897704a284af7c2f7a040a901539517b3ca6f2b
embeddings.f32            57dc5611ee5d30a6da11a7efe2e0393a19da106bad5dae4f63d5cd7a5a4506ce
query_embeddings.f32      fc3117ef25dfd8e2137b0df836d776e697c39c3934e8b4018b0b9f2c5c5058c6
records.jsonl             720bf45ae6b543106bacd26e728697e5ab4a28c5d30b06a997dff575baa80df2
queries.jsonl             e8c8dbbd7fb4a9c82f720f67d99f6893d97f15b601bc08444eea81639eea1a9c
groundtruth.jsonl         b53ddf15773402072c2e27deeba00af5dc837648bddd4e64091cd4426603f3c9
groundtruth.manifest.json 512b0e02eb4bddda8b9cf1aaa4c12c499cd883e4ae1f311d11e473b2d086f4f8
```

B4 runner 不链接 `libonnxruntime`，不调用 ONNX，不重新生成 ground truth。

## 6. 输出 artifact

B4 输出写入同一 `/tmp` dataset 目录：

```text
faiss_benchmark_result.json
faiss_hnsw_per_query.jsonl
faiss_flat_per_query.jsonl
```

写入语义：

- 默认 fail-fast：结果文件已存在时拒绝覆盖。
- 使用临时文件 `.tmp.*.<pid>`。
- per-query 文件先 rename，result manifest 最后 rename；result manifest 是发布完成标志。
- 失败路径清理临时文件。
- `validate` 子命令只做 schema / provenance / checksum / query count / metric range 校验，不重新构建 FAISS index。

## 7. IndexFlatIP parity

IndexFlatIP 与 B2 core_flat exact ground truth 对齐：

```text
record_id_set_match_all = true
strict_rank_match_all   = true
score_tolerance         = 0.0001
max_score_abs_diff      = 1.01327896e-06
queries_checked         = 200
```

Smoke test 还用 Python 独立读取 `faiss_flat_per_query.jsonl` 和 `groundtruth.jsonl`，逐 query 核对：

- record_id set match。
- strict rank match。
- score finite 且降序。
- score diff <= 1e-4。
- record_id 可回表到 `records.jsonl`。

## 8. IndexHNSWFlat 质量结果

```text
recall@1                  = 1.000000
recall@10                 = 1.000000
top1_match_rate           = 1.000000
topk_exact_match_rate     = 1.000000
minimum per-query recall  = 1.000000
queries_checked           = 200
```

注意：本数据集只有 1000 records，且 `efSearch=1024 >= record_count=1000`。因此 perfect recall 只证明 B4 harness 与 ground truth 对齐，不构成大规模 ANN 质量证明。

## 9. 性能结果

正式 B4 benchmark 结果：

```text
cache_load_us          = 1,457

IndexFlatIP:
  flat_build_us        = 1,074
  flat_search_total_us = 17,665
  flat_search_avg_us   = 88.325
  flat_search_p50_us   = 88
  flat_search_p95_us   = 92
  flat_search_p99_us   = 95
  flat_qps             = 11,321.823

IndexHNSWFlat:
  hnsw_build_us        = 255,683
  hnsw_search_total_us = 184,097
  hnsw_search_avg_us   = 920.485
  hnsw_search_p50_us   = 916
  hnsw_search_p95_us   = 947
  hnsw_search_p99_us   = 972
  hnsw_qps             = 1,086.384
```

这些数字来自 1K smoke dataset 和当前机器的一次运行，只用于证明 benchmark harness 可机器校验和记录完整指标，不作为生产 SLA。

## 10. FAISS index 状态和 storage 口径

```text
flat_ntotal                    = 1000
hnsw_ntotal                    = 1000
requested_M                    = 48
actual_level0_neighbor_capacity = 96
efConstruction                 = 512
efSearch                       = 1024
thread_count                   = 1
flat_serialized_index_bytes    = 2,048,045
hnsw_serialized_index_bytes    = 2,447,298
storage_metric_source          = faiss_serialized_index_size
storage_metric_kind            = storage_estimate
memory_bytes                   = null
memory_bytes_source            = unsupported
```

本阶段不能可靠获得 FAISS runtime memory，因此不伪造 `memory_bytes`。`serialized_index_bytes` 只是存储大小估计，不能与 B3 的 `memory_bytes=8,502,864 (estimated)` 当作同一口径直接比较。

## 11. B2/B3/B4 同口径对比

| backend | records | M | efC | efS | build_us | search_avg_us | p95_us | QPS | recall@10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| core_flat | 1000 | - | - | - | 1,124 | 397 | not_recorded | 2,516.58 | 1.000000 |
| core_hnsw_v2 | 1000 | 48 | 512 | 1024 | 2,059,178 | 705.440 | 724 | 1,417.555 | 1.000000 |
| FAISS IndexFlatIP | 1000 | - | - | - | 1,074 | 88.325 | 92 | 11,321.823 | 1.000000 |
| FAISS IndexHNSWFlat | 1000 | 48 | 512 | 1024 | 255,683 | 920.485 | 947 | 1,086.384 | 1.000000 |

说明：

- 1K 是 smoke/correctness 规模。
- `efSearch=1024 >= record_count=1000`，perfect recall 不构成大规模 ANN 质量证明。
- 该表用于实现 parity 和 benchmark harness 对齐，不用于宣称生产 SLA。
- core_flat 的 QPS 由 B2 `search_total_us=79,473` 和 `query_count=200` 计算得到；B2 未记录 p95。

## 12. 测试结果

```text
$ make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
  PASS，构建完成（kvstore / ebpf_syncd / eBPF object）

$ build/tools/bin/generate_real_embedding_cache validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
  PASS，record_count=1000 query_count=200 dim=512

$ build/tools/bin/generate_real_embedding_groundtruth validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
  PASS，query_count=200 topk=10 record_count=1000 dim=512

$ tests/vector_real_embedding_faiss_benchmark_smoke_test.sh
  PASS，ALL CHECKS PASSED

$ build/tools/bin/generate_real_embedding_faiss_benchmark generate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
  PASS，生成 faiss_benchmark_result.json / faiss_hnsw_per_query.jsonl / faiss_flat_per_query.jsonl

$ build/tools/bin/generate_real_embedding_faiss_benchmark validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
  PASS，hnsw_recall_at_10=1.0000
```

Smoke test 覆盖：

- B1/B2 validate 通过。
- IndexFlatIP strict rank / set / score tolerance parity。
- IndexHNSWFlat recall 独立复算。
- 所有 query 均返回 topK=10。
- record_id 回表。
- rank / score 合法。
- 无重复 record_id。
- 单线程、参数、actual level-0 capacity、storage 指标口径检查。
- deterministic quality / per-query 输出一致。
- corrupt cache 拒绝执行。
- corrupt ground truth 拒绝执行。
- corrupt B4 result validate 失败。
- disposable artifact copy 位于 `/tmp`，未修改 B1/B2 正式产物。

## 13. 限制

- 数据规模只有 1000 records / 200 queries；当前结论是 smoke/correctness 与 harness readiness。
- `efSearch=1024` 大于记录数，不能代表大规模 ANN 质量。
- FAISS runtime memory 未测得，报告只输出 serialized index storage estimate。
- 真实 embedding cache 和 ground truth 仍以未签名 manifest 为信任根；checksum 能捕获意外损坏，但不能防御同时一致篡改 manifest 与 artifact。
- B4 只覆盖 FAISS index-only；没有恢复或验证 product FAISS backend。

## 14. 下一阶段建议

建议进入 `B-final`：汇总 B1-B4 的 fixture/cache、ground truth、core_hnsw_v2 和 FAISS index-only baseline，冻结当前 1K smoke benchmark 结论、限制和后续大规模 benchmark 前置条件。

## 15. 检查结果

```text
$ git diff --check
  PASS，无输出

$ git status --short
  M Makefile
  ?? docs/vector_benchmark_readiness/
  ?? scripts/benchmark/generate_real_embedding_cache.c
  ?? scripts/benchmark/generate_real_embedding_faiss_benchmark.cpp
  ?? scripts/benchmark/generate_real_embedding_groundtruth.c
  ?? scripts/benchmark/generate_real_embedding_hnsw_v2_benchmark.c
  ?? tests/vector_real_embedding_cache_smoke_test.sh
  ?? tests/vector_real_embedding_faiss_benchmark_smoke_test.sh
  ?? tests/vector_real_embedding_groundtruth_smoke_test.sh
  ?? tests/vector_real_embedding_hnsw_v2_benchmark_smoke_test.sh
```

## 16. Decision block

```text
B4-FAISS-index-only-baseline decision:
  production_code_changed = no
  product_behavior_changed = no
  tests_changed = yes
  makefile_changed = yes
  docs_changed = yes
  b1_cache_validation_passed = yes
  b2_groundtruth_validation_passed = yes
  onnx_inference_rerun = no
  groundtruth_regenerated = no
  faiss_product_backend_restored = no
  faiss_index_flat_ip_used = yes
  faiss_index_hnsw_flat_used = yes
  thread_count = 1
  record_count = 1000
  query_count = 200
  dim = 512
  topk = 10
  faiss_flat_exact_match = yes
  faiss_hnsw_recall_at_10 = 1.000000
  faiss_hnsw_search_avg_us = 920.485
  faiss_hnsw_build_us = 255683
  comparison_table_complete = yes
  artifacts_outside_repo = yes
  ready_for_B_final = yes
  recommended_next = B-final
```
