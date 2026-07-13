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
- [b_scale_0_t2ranking_subset_generation_report.md](b_scale_0_t2ranking_subset_generation_report.md)：B-scale-0 T2Ranking deterministic 10K / 100K subset generator，真实输出 checksum、嵌套/positive 保留验证和 B-scale-1 输入契约。
- [b_scale_1_t2ranking_10k_embedding_cache_report.md](b_scale_1_t2ranking_10k_embedding_cache_report.md)：B-scale-1 T2Ranking 10K real ONNX embedding cache，外部 TSV 输入、pid/qid 对齐、finite/norm 验证、确定性和 B-scale-2 输入契约。
- [b_scale_2_t2ranking_10k_ground_truth_report.md](b_scale_2_t2ranking_10k_ground_truth_report.md)：B-scale-2 T2Ranking 10K core_flat exact IP ground truth，外部 cache/output、全量 double brute-force 核对、tie-break 和 B-scale-3 输入契约。
- [b_scale_3_t2ranking_10k_hnsw_faiss_benchmark_report.md](b_scale_3_t2ranking_10k_hnsw_faiss_benchmark_report.md)：B-scale-3 T2Ranking 10K core_hnsw_v2 / FAISS index-only benchmark，共享 deterministic insertion order、FAISS Flat parity、efSearch sweep 和 B-scale-4 建议。

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

B-scale-0 已完成：`scripts/benchmark/generate_t2ranking_subsets.py` 基于固定 T2Ranking revision `2a369a430a70979223f1b9a41b1919774d46b432`，生成了仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/` 下的 200 query、1131 positive pair、10K / 100K deterministic corpus 子集。10K 是 100K 的前缀子集，全部 selected-query positive pid 保留，重复运行 checksum 不变，详见 [b_scale_0_t2ranking_subset_generation_report.md](b_scale_0_t2ranking_subset_generation_report.md)。

B-scale-1 已完成：`scripts/benchmark/generate_real_embedding_cache.c` 已支持外部 id/text TSV 和直接 `--output-dir` 发布，基于 B-scale-0 的 `collection.10k.tsv` / `queries.dev.200.tsv` 生成了仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding/` 下的 10000 records / 200 queries / dim=512 / float32 / L2-normalized / IP metric embedding cache。核心文件重复生成 byte-identical，详见 [b_scale_1_t2ranking_10k_embedding_cache_report.md](b_scale_1_t2ranking_10k_embedding_cache_report.md)。

B-scale-2 已完成：`scripts/benchmark/generate_real_embedding_groundtruth.c` 已支持外部 `--cache-dir` / `--output-dir`，基于 B-scale-1 cache 生成仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth/` 下的 `groundtruth.jsonl` / `groundtruth.manifest.json`。本阶段使用 core_flat exact normalized IP，topK=10，并用独立 double precision brute-force 对全部 200 queries / 10000 documents 复算一致；T2Ranking qrels 未参与 exact topK 生成，详见 [b_scale_2_t2ranking_10k_ground_truth_report.md](b_scale_2_t2ranking_10k_ground_truth_report.md)。

B-scale-3 已完成：新增 combined index-only sweep runner `scripts/benchmark/generate_real_embedding_hnsw_faiss_sweep_benchmark.cpp` 和 core_hnsw_v2 C helper，读取 B-scale-1 cache 与 B-scale-2 exact ground truth，使用共享 qrels-free deterministic insertion order 执行 core_hnsw_v2 / FAISS HNSW `efSearch=32,64,128,256,512,1024` sweep，并用 FAISS IndexFlatIP 验证 strict parity。输出位于仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_3_10k_benchmark/`；本阶段未调用 ONNX、未重新生成 ground truth、未运行 product e2e benchmark，详见 [b_scale_3_t2ranking_10k_hnsw_faiss_benchmark_report.md](b_scale_3_t2ranking_10k_hnsw_faiss_benchmark_report.md)。

建议后续使用独立阶段完成：

```text
B-scale-4:
  读取 B-scale-0 的 T2Ranking 100K subset 和 200 queries，
  生成一次可复用 100K real embedding cache，不重新生成 B-scale-0 子集。

implement-benchmark-harness (B6+):
  生成或读取 embedding cache，构建 core_hnsw_v2 与 FAISS index-only 对比，
  并执行 end-to-end VSET / VSEARCH / VINDEX / VINFO 压测。
```
