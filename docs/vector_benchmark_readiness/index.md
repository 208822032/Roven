# Vector benchmark readiness 文档索引

## 当前文档

- [README.md](README.md)：目录定位、边界和当前 readiness 结论。
- [v_family_implementation_audit_report.md](v_family_implementation_audit_report.md)：V 族实现逐文件审计、真实 embedding / FAISS 对比可行性、缺口和 decision block。
- [benchmark_plan_real_embedding.md](benchmark_plan_real_embedding.md)：真实 embedding benchmark 设计冻结，覆盖 fixture/cache/result schema、FAISS index-only 边界、e2e 指标和 B0-Bfinal 阶段路线。
- [b0_preflight_report.md](b0_preflight_report.md)：B0 环境与依赖 preflight，确认模型、ONNX Runtime、FAISS index-only link、backend name 和 artifact 策略。
- [b1_fixture_cache_generation_report.md](b1_fixture_cache_generation_report.md)：B1 真实 embedding fixture/cache 生成器实现，manifest schema、sanity/校验结果和 B2 输入契约。
- [b2_ground_truth_generation_report.md](b2_ground_truth_generation_report.md)：B2 core_flat exact IP ground truth 生成器，tie-break 冻结、独立 brute-force 交叉验证、groundtruth schema 和 B3 输入契约。
- [b3_core_hnsw_v2_index_only_benchmark_report.md](b3_core_hnsw_v2_index_only_benchmark_report.md)：B3 core_hnsw_v2 index-only benchmark runner，recall/latency/build/memory 结果、validate-only 和 B4 输入契约。
- [b4_faiss_index_only_baseline_report.md](b4_faiss_index_only_baseline_report.md)：B4 FAISS IndexFlatIP / IndexHNSWFlat index-only baseline，flat parity、HNSW recall/latency、storage 口径和 B2/B3/B4 同口径对比。
- [b_final_consolidation_report.md](b_final_consolidation_report.md)：B-final real embedding benchmark readiness 收口，B0-B4 能力矩阵、1K 同口径结果表、已证明/未证明结论和后续 scale 路线。

## 关联文档

- [HNSW-S README](../hnsw_index_snapshot/README.md)
- [HNSW-S index](../hnsw_index_snapshot/index.md)
- [HNSW-S final consolidation](../hnsw_index_snapshot/hnsw_s_final_consolidation_report.md)
- [HNSW-S cold-start benchmark](../hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md)
- [HNSW-R README](../hnsw_rebuild_productization/README.md)
- [HNSW-R index](../hnsw_rebuild_productization/index.md)
- [HNSW-R execution plan](../hnsw_rebuild_productization/hnsw_r_execution_plan.md)
- [HNSW-R final consolidation](../hnsw_rebuild_productization/hnsw_r_final_product_rebuild_consolidation_report.md)
- [core_hnsw_v2 final architecture](../vector_core_reset/core_hnsw_v2_final_architecture.md)
- [Stage 5D VINFO / benchmark stats hardening](../vector_core_reset/stage5d_vinfo_benchmark_stats_hardening_report.md)

## 后续建议

B1-fixture-cache-generation 已完成：`scripts/benchmark/generate_real_embedding_cache.c` 复用生产 ONNX runtime/tokenizer，从 `tests/fixtures/qa_zh_1000.tsv` / `qa_zh_queries.tsv` 生成了 `records.jsonl` / `queries.jsonl` / `embeddings.f32` / `query_embeddings.f32` / `manifest.json`（1000 records / 200 queries / dim=512 / L2-normalized / IP metric），详见 [b1_fixture_cache_generation_report.md](b1_fixture_cache_generation_report.md)。

B2-ground-truth-generation 已完成：`scripts/benchmark/generate_real_embedding_groundtruth.c` 复用生产 core_flat exact IP 检索，对 B1 缓存的 200 条 query 生成了 `groundtruth.jsonl`（topk=10，tie-break 已冻结）与 `groundtruth.manifest.json`，并用独立 double 精度 brute-force 对全部 200 条 query 交叉验证一致，详见 [b2_ground_truth_generation_report.md](b2_ground_truth_generation_report.md)。

B3-core-hnsw-v2-index-only-benchmark 已完成：`scripts/benchmark/generate_real_embedding_hnsw_v2_benchmark.c` 复用现有 core_hnsw_v2 API，读取 B1 cache 与 B2 ground truth，输出 `hnsw_v2_benchmark_result.json` 和 `hnsw_v2_per_query.jsonl`。1K smoke 结果为 recall@10=1.000000，search_avg_us=705.440，p95=724，build_us=2,059,178，memory_bytes=8,502,864（estimated），详见 [b3_core_hnsw_v2_index_only_benchmark_report.md](b3_core_hnsw_v2_index_only_benchmark_report.md)。

B4-FAISS-index-only-baseline 已完成：`scripts/benchmark/generate_real_embedding_faiss_benchmark.cpp` 直接使用 FAISS C++ API，读取同一份 B1 cache 与 B2 ground truth，输出 `faiss_benchmark_result.json`、`faiss_hnsw_per_query.jsonl` 和 `faiss_flat_per_query.jsonl`。IndexFlatIP 与 B2 strict rank/score 对齐，IndexHNSWFlat 1K smoke 结果为 recall@10=1.000000，search_avg_us=920.485，p95=947，build_us=255,683，actual level-0 neighbor capacity=96，详见 [b4_faiss_index_only_baseline_report.md](b4_faiss_index_only_baseline_report.md)。

B-final 已完成：B0-B4 已收口为 1K real embedding smoke benchmark。最终结论是 provenance/checksum、real ONNX cache、exact ground truth、core_hnsw_v2 harness、FAISS Flat/HNSW harness 和 1K correctness/parity 均已证明；10K/100K real embedding recall、大规模 SLA、参数最优性、product VSEARCH e2e、runtime memory parity 和多线程扩展仍未证明，详见 [b_final_consolidation_report.md](b_final_consolidation_report.md)。

建议后续使用独立阶段完成：

```text
B-scale-0:
  冻结 10K/100K real corpus 来源和生成策略；在进入 scale 前先提交 B0-B4
  benchmark readiness 工具/测试/文档。

implement-benchmark-harness (B6+):
  生成或读取 embedding cache，构建 core_hnsw_v2 与 FAISS index-only 对比，
  并执行 end-to-end VSET / VSEARCH / VINDEX / VINFO 压测。
```
