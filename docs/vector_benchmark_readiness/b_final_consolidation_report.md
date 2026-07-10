# B-final: real embedding benchmark readiness consolidation

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`，并读取本阶段要求的：

- `docs/vector_benchmark_readiness/README.md`
- `docs/vector_benchmark_readiness/index.md`
- `docs/vector_benchmark_readiness/benchmark_plan_real_embedding.md`
- `docs/vector_benchmark_readiness/b0_preflight_report.md`
- `docs/vector_benchmark_readiness/b1_fixture_cache_generation_report.md`
- `docs/vector_benchmark_readiness/b2_ground_truth_generation_report.md`
- `docs/vector_benchmark_readiness/b3_core_hnsw_v2_index_only_benchmark_report.md`
- `docs/vector_benchmark_readiness/b4_faiss_index_only_baseline_report.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；当前 benchmark readiness 路线明确要求写入 `docs/vector_benchmark_readiness/`。本报告按任务指定目录输出，同时继续遵守 AGENTS 的核心边界：不修改生产 V 命令行为、不修改 RDB/AOF、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不恢复 FAISS product backend。

本阶段只做 B0-B4 收口、结果审计和后续 scale benchmark 决策。未修改 `src/`、`include/`、`scripts/benchmark/`、`tests/` 或 `Makefile`。

## 2. B0-B4 能力矩阵

| stage | 能力 | 产物 | 当前结论 |
|---|---|---|---|
| B0 preflight | 环境/依赖检查 | `b0_preflight_report.md` | model/ONNX Runtime/FAISS index-only/product backend name/artifact policy 均已确认 |
| B1 fixture/cache | 真实 ONNX embedding cache | `manifest.json`, `embeddings.f32`, `query_embeddings.f32`, `records.jsonl`, `queries.jsonl` | 1000 records / 200 queries / dim=512 / L2-normalized / IP，validate 通过 |
| B2 ground truth | core_flat exact IP ground truth | `groundtruth.jsonl`, `groundtruth.manifest.json` | core_flat exact 与 independent double brute-force 交叉验证通过 |
| B3 core_hnsw_v2 | core_hnsw_v2 index-only benchmark | `hnsw_v2_benchmark_result.json`, `hnsw_v2_per_query.jsonl` | recall/latency/build/memory/stats 均可机器校验 |
| B4 FAISS baseline | FAISS IndexFlatIP / IndexHNSWFlat index-only baseline | `faiss_benchmark_result.json`, `faiss_flat_per_query.jsonl`, `faiss_hnsw_per_query.jsonl` | IndexFlatIP exact parity 通过；IndexHNSWFlat 指标可机器校验 |

正式 artifact 位于仓库外：

```text
/tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/
```

## 3. 最终同口径结果表

Dataset 统一口径：

```text
dataset_id = qa_zh_1k_200_bge_small_zh_v1_5
records = 1000
queries = 200
dim = 512
metric = ip_normalized
topK = 10
embedding model = bge-small-zh-v1.5
```

| backend | records | dim | topK | M | M0 / level0 cap | efC | efS | build_us | search_avg_us | p95_us | p99_us | QPS | recall@1 | recall@10 | 指标来源 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| core_flat | 1000 | 512 | 10 | - | - | - | - | 1,124 | 397 | not_recorded | not_recorded | 2,516.58 | 1.000000 | 1.000000 | B2 `groundtruth.manifest.json`; exact ground truth by definition |
| core_hnsw_v2 | 1000 | 512 | 10 | 48 | 96 | 512 | 1024 | 2,059,178 | 705.440 | 724 | 761 | 1,417.555 | 1.000000 | 1.000000 | B3 `hnsw_v2_benchmark_result.json` |
| FAISS IndexFlatIP | 1000 | 512 | 10 | - | - | - | - | 1,074 | 88.325 | 92 | 95 | 11,321.823 | 1.000000 | 1.000000 | B4 `faiss_benchmark_result.json`; exact parity with B2 |
| FAISS IndexHNSWFlat | 1000 | 512 | 10 | 48 | 96 actual | 512 | 1024 | 255,683 | 920.485 | 947 | 972 | 1,086.384 | 1.000000 | 1.000000 | B4 `faiss_benchmark_result.json` |

口径说明：

- core_flat 的 recall 是 exact ground truth 自身口径；B2 未记录 p95/p99。
- FAISS IndexFlatIP 与 B2 strict rank / score tolerance 对齐，因此 recall@1/10 记为 1.000000。
- FAISS HNSW 的 `M0 / level0 cap` 来自运行时 `hnsw.nb_neighbors(0)`，不是硬编码假设。
- 1K 是 smoke/correctness 规模；`efSearch=1024 >= record_count=1000`，perfect recall 不能外推为大规模 ANN 质量。

## 4. 关键计算结论

FAISS HNSW build 相比 core_hnsw_v2 约快 8.05x：

```text
core_hnsw_v2 build_us / FAISS HNSW build_us
= 2,059,178 / 255,683
= 8.05x
```

1K 下 core_hnsw_v2 search latency 低于 FAISS HNSW：

```text
core_hnsw_v2 search_avg_us = 705.440
FAISS HNSW search_avg_us   = 920.485
core / FAISS               = 0.766x
```

该结果不能外推到大规模：1K 的 `efSearch=1024` 已覆盖全部或近全部候选空间，且 FAISS / v2 的实现成本可能随规模、cache locality、线程和参数变化。

FAISS IndexFlatIP 比当前 core_flat exact scan 更快，但 exact 结果一致：

```text
core_flat search_avg_us       = 397
FAISS IndexFlatIP avg_us      = 88.325
core_flat / FAISS flat search = 4.49x
IndexFlatIP strict_rank_match = true
max_score_abs_diff            = 1.01327896e-06 <= 1e-4
```

memory / storage 指标禁止直接比较：

```text
core_hnsw_v2 memory_bytes = 8,502,864
core_hnsw_v2 memory source = estimated

FAISS HNSW serialized_index_bytes = 2,447,298
FAISS memory_bytes = null
FAISS memory source = unsupported
FAISS storage source = faiss_serialized_index_size
```

`memory_bytes` 与 `serialized_index_bytes` 不是同一口径，不得放入同一 memory ranking。

## 5. 指标口径审计

已确认：

- B1 manifest 绑定 records / queries / embeddings / query_embeddings checksum。
- B2 groundtruth manifest 绑定 B1 manifest checksum。
- B3 result manifest 绑定 B1 + B2 artifact checksum。
- B4 result manifest 绑定 B1 + B2 artifact checksum。
- B3/B4 validate-only 不重建索引、不调用 ONNX、不重新生成 ground truth。
- B3/B4 per-query 输出支持独立复算 recall。
- B4 fixed `thread_count=1`，结果中记录 `thread_count=1`。
- FAISS product backend 未恢复，B4 只直接使用 FAISS C++ index-only API。

限制：

- manifest/checksum 是完整性校验，不是防篡改签名。
- 当前 benchmark 是单机单次 smoke run，未做多轮统计稳定性。
- current artifacts 位于 `/tmp`，是 benchmark artifact，不应提交到 git。

## 6. 已证明 / 未证明

已证明：

- 输入 provenance / checksum 链路完整。
- 真实 ONNX embedding cache 可复用，且 validate-only 可阻止损坏输入。
- core_flat exact ground truth 可信，并经过 independent double brute-force 交叉验证。
- core_hnsw_v2 benchmark harness 可机器校验。
- FAISS IndexFlatIP / IndexHNSWFlat benchmark harness 可机器校验。
- 1K correctness / parity 通过。

未证明：

- 10K / 100K real embedding recall。
- 大规模 build/search SLA。
- 参数最优性。
- product `VSEARCH` end-to-end 性能。
- runtime memory parity。
- 多线程扩展能力。

## 7. synthetic 与 real benchmark 边界

本报告不把 synthetic 10K/50K/100K 与 real 1K 混成同一统计表。原因：

- dataset 不同：历史 synthetic 是生成向量；B1-B4 是真实 ONNX embedding。
- runner / 层级不同：部分历史结果来自 product path 或旧 direct parity harness；B3/B4 是新的 index-only artifact runner。
- 参数可能不同：Stage 9G3 scale gate 使用当时 default profile；B3/B4 使用当前 balanced default `M=48 / M0=96 / efConstruction=512 / efSearch=1024`。

可作为方向性参考的历史事实：

- Stage 9G3 synthetic scale gate 记录 10K/50K/100K build time 随规模快速增长，并指出 50K/100K 同步 build/cold-start 风险高。
- HNSW-P1b-B synthetic 50K balanced 同参 parity 中，core_hnsw_v2 build_us=1,198,073,777，FAISS HNSW build_us=277,934,315，v2 / FAISS build = 4.31x。
- B4 real 1K 中，core_hnsw_v2 / FAISS HNSW build = 8.05x。

因此，build gap 在 real 1K 和既有 synthetic scale 中方向一致：core_hnsw_v2 build 明显慢于 FAISS HNSW。search 相对排名可能随规模变化：real 1K 下 core_hnsw_v2 search_avg_us 低于 FAISS HNSW，但 synthetic 50K balanced 历史数据中 v2 search_avg_us 高于 FAISS HNSW 约 10%。需要 real embedding scale benchmark 才能形成最终判断。

## 8. 后续 scale 路线

本阶段不执行后续 scale benchmark，只冻结路线：

```text
B-scale-0:
  冻结 10K/100K real corpus 来源和生成策略。

B-scale-1:
  生成一次可复用 real embedding cache。

B-scale-2:
  生成 exact ground truth。

B-scale-3:
  运行 core_hnsw_v2 / FAISS 同口径 index-only benchmark。

B-e2e:
  仅在 index-only scale gate 通过后考虑产品 V 命令 benchmark。
```

进入 B-e2e 前必须先解决：

- product benchmark backend name 只能使用 `core_flat` / `core_hnsw_v2`。
- 不使用 removed backend name。
- 不把 ONNX 推理时间混入 core index-only 指标。
- 明确 response / manager resolve / embedding runtime / socket overhead 的分层指标。

## 9. 是否可提交

B1-B4 的工具、测试和文档可以作为一组 benchmark readiness 提交，原因：

- 生产代码未修改。
- 产品行为未修改。
- benchmark 工具均为显式 opt-in Makefile target，不进入默认 `all` / 默认测试路径。
- artifact 位于 `/tmp`，未加入 git。
- B1-B4 smoke / validate 链路均通过。

提交建议：

- 提交代码和文档：`Makefile`、`scripts/benchmark/*real_embedding*`、`tests/vector_real_embedding_*`、`docs/vector_benchmark_readiness/`。
- 不提交 `/tmp/kvlite_vector_benchmark_artifacts/...` 下的 artifact。
- 提交信息应明确这是 real embedding benchmark readiness / index-only harness，不是 product FAISS backend 恢复。

## 10. 验证结果

```text
$ build/tools/bin/generate_real_embedding_cache validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
[validate] PASS dataset_id=qa_zh_1k_200_bge_small_zh_v1_5 record_count=1000 query_count=200 dim=512

$ build/tools/bin/generate_real_embedding_groundtruth validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
[validate] PASS dataset_id=qa_zh_1k_200_bge_small_zh_v1_5 query_count=200 topk=10 record_count=1000 dim=512

$ build/tools/bin/generate_real_embedding_hnsw_v2_benchmark validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
[validate] PASS dataset_id=qa_zh_1k_200_bge_small_zh_v1_5 backend=core_hnsw_v2 query_count=200 topk=10 recall_at_10=1.0000

$ build/tools/bin/generate_real_embedding_faiss_benchmark validate --dataset-id qa_zh_1k_200_bge_small_zh_v1_5
[validate] PASS dataset_id=qa_zh_1k_200_bge_small_zh_v1_5 backend=faiss_index_only query_count=200 topk=10 hnsw_recall_at_10=1.0000

$ make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
  PASS，构建完成（kvstore / ebpf_syncd / eBPF object）
```

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

## 11. Decision block

```text
B-final decision:
  production_code_changed = no
  benchmark_tools_changed = no
  docs_changed = yes
  b1_cache_valid = yes
  b2_groundtruth_valid = yes
  b3_hnsw_v2_result_valid = yes
  b4_faiss_result_valid = yes
  real_embedding_smoke_loop_complete = yes
  large_scale_real_embedding_proven = no
  product_e2e_proven = no
  ready_to_commit_B0_B4 = yes
  recommended_next = commit
```
