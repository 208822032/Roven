# Vector benchmark readiness

本目录记录 KVLite V 族进入真实 embedding + backend 对比压测前的 read-only audit。

本路线关注：

```text
models/ real embedding
core_flat / core_hnsw_v2 / FAISS core comparison
end-to-end VSET / VSEARCH / VINDEX / VINFO comparison
snapshot cold-start and async rebuild benchmark readiness
```

本目录不承载生产功能实现，不修改 `VSEARCH` response，不修改 RDB/AOF 主格式，不修改 `kvs_vector_index_ops_t`。

## 当前文档

- [v_family_implementation_audit_report.md](v_family_implementation_audit_report.md)：V-family implementation audit 与真实 embedding benchmark readiness 判断。
- [benchmark_plan_real_embedding.md](benchmark_plan_real_embedding.md)：真实 embedding + core/e2e benchmark 设计冻结，定义 fixture/cache/result schema 和 B0-Bfinal 阶段路线。
- [b0_preflight_report.md](b0_preflight_report.md)：真实 embedding benchmark 环境与依赖 preflight，确认模型、ONNX Runtime、FAISS index-only、backend name 和 artifact 策略。
- [b1_fixture_cache_generation_report.md](b1_fixture_cache_generation_report.md)：真实 embedding fixture/cache 生成器实现、sanity/校验结果、manifest schema 和 B2 输入契约。
- [b2_ground_truth_generation_report.md](b2_ground_truth_generation_report.md)：core_flat exact IP ground truth 生成器、tie-break 冻结、独立 brute-force 交叉验证和 B3 输入契约。
- [b3_core_hnsw_v2_index_only_benchmark_report.md](b3_core_hnsw_v2_index_only_benchmark_report.md)：core_hnsw_v2 index-only benchmark runner、recall/latency/build/memory 结果和 B4 输入契约。
- [b4_faiss_index_only_baseline_report.md](b4_faiss_index_only_baseline_report.md)：FAISS IndexFlatIP / IndexHNSWFlat index-only baseline、flat parity、HNSW recall/latency 和 B2/B3/B4 同口径对比。
- [b_final_consolidation_report.md](b_final_consolidation_report.md)：B0-B4 real embedding benchmark readiness 收口，冻结 1K 同口径表、已证明/未证明结论和 scale benchmark 后续路线。
- [b_scale_0_t2ranking_subset_generation_report.md](b_scale_0_t2ranking_subset_generation_report.md)：T2Ranking 10K / 100K deterministic subset generator、真实输出 checksum、嵌套/positive 保留验证和 B-scale-1 输入契约。
- [b_scale_1_t2ranking_10k_embedding_cache_report.md](b_scale_1_t2ranking_10k_embedding_cache_report.md)：T2Ranking 10K real ONNX embedding cache，外部 TSV 支持、pid/qid 对齐、finite/norm 验证和 B-scale-2 输入契约。
- [b_scale_2_t2ranking_10k_ground_truth_report.md](b_scale_2_t2ranking_10k_ground_truth_report.md)：T2Ranking 10K core_flat exact IP ground truth，外部 cache/output 支持、double brute-force 全量核对、tie-break 和 B-scale-3 输入契约。
- [b_scale_3_t2ranking_10k_hnsw_faiss_benchmark_report.md](b_scale_3_t2ranking_10k_hnsw_faiss_benchmark_report.md)：T2Ranking 10K core_hnsw_v2 / FAISS index-only sweep benchmark，共享确定性插入顺序、FAISS Flat parity、recall-latency 曲线和 B-scale-4 输入建议。
- [index.md](index.md)：文档索引和后续建议。

## 当前结论

B0 preflight、B1 fixture/cache generation、B2 ground truth generation、B3 core_hnsw_v2 index-only benchmark、B4 FAISS index-only baseline、B-final consolidation 均已完成。真实 embedding cache（1000 records / 200 queries，`bge-small-zh-v1.5`，dim=512，L2-normalized，IP metric）、core_flat exact ground truth（topk=10）、core_hnsw_v2 index-only benchmark 和 FAISS IndexFlatIP / IndexHNSWFlat baseline 结果已生成并通过校验，位于仓库外 `/tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/`。当前仍只能视为 1K smoke/correctness 规模结论。

B-scale-0 已完成：基于固定 T2Ranking revision `2a369a430a70979223f1b9a41b1919774d46b432` 生成 deterministic 10K / 100K 子集，输出位于仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/`。本阶段选择 200 queries，保留 1131 positive pairs / 1131 unique positive pids；10K 是 100K 的前缀子集，全部 positive 保留，重复运行 checksum 不变。

B-scale-1 已完成：复用并扩展 `scripts/benchmark/generate_real_embedding_cache.c`，从 B-scale-0 的 `collection.10k.tsv` / `queries.dev.200.tsv` 生成 T2Ranking 10K real ONNX embedding cache，输出位于仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding/`。本阶段生成 10000 record embeddings 和 200 query embeddings（dim=512，float32，L2 normalized，IP metric），核心文件重复生成 byte-identical。

B-scale-2 已完成：复用并扩展 `scripts/benchmark/generate_real_embedding_groundtruth.c`，读取 B-scale-1 cache 并使用 core_flat exact normalized IP 生成 T2Ranking 10K exact ground truth，输出位于仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth/`。本阶段生成 200 query 的 topK=10 ground truth，并用独立 double precision brute-force 对全部 query / 10000 documents 复算一致；T2Ranking qrels 未参与 ANN exact topK 生成。

B-scale-3 已完成：新增 `scripts/benchmark/generate_real_embedding_hnsw_faiss_sweep_benchmark.cpp` 和 `scripts/benchmark/real_embedding_core_hnsw_v2_sweep_helper.c`，读取 B-scale-1 cache 与 B-scale-2 exact ground truth，使用同一 qrels-free deterministic insertion order 对 core_hnsw_v2 和 FAISS IndexHNSWFlat 做 `efSearch=32,64,128,256,512,1024` index-only sweep，并用 FAISS IndexFlatIP 做 exact parity。输出位于仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_3_10k_benchmark/`。本阶段未调用 ONNX、未重新生成 ground truth、未运行 product e2e benchmark；qrels 未参与 ANN recall。

原因：

- `core_hnsw_v2` product 主路径、snapshot、async rebuild、VINFO observability 已具备。
- 真实 embedding cache + ground truth 生成链路已验证：B1 复用生产 ONNX runtime/tokenizer（无 hash fallback），B2/B-scale-2 复用生产 core_flat exact IP（真实 ground truth 生成不调用 ONNX），全部 checksum/determinism 通过。
- B3 复用现有 core_hnsw_v2 API 完成 index-only benchmark：recall@10=1.000000，search_avg_us=705.440，p95=724，build_us=2,059,178，memory_bytes=8,502,864（estimated）。
- B4 直接使用 FAISS C++ index-only API：IndexFlatIP 与 B2 strict rank/score 对齐，IndexHNSWFlat recall@10=1.000000，search_avg_us=920.485，p95=947，build_us=255,683，actual level-0 neighbor capacity=96。
- B-scale-3 证明 10K T2Ranking real embedding index-only harness 可以机器校验：core_hnsw_v2 在 efSearch=128 起 recall@10=1.0000，FAISS HNSW 在 efSearch=256 起 recall@10=1.0000；FAISS Flat 与 B-scale-2 exact ground truth strict parity。
- 当前产品 backend 只支持 `core_flat` 与 `core_hnsw_v2`；FAISS product backend 已删除并映射到 `core_flat` compatibility fallback。
- 新 B4 runner 已替代旧 FAISS benchmark 脚本缺失的 `tests/vector_index_only_formal.cpp` 依赖；它仍是 index-only 外部 baseline，不是 product backend。
- `config/kvstore-embedding.conf` 仍配置旧 `hnswlib_hnsw` 名称，会被 resolver 映射到 `core_flat`，不适合作为 v2 真实 embedding benchmark 默认配置。

推荐下一步进入 `B-scale-4 — T2Ranking 100K Real Embedding Cache`，使用 B-scale-0 的 `collection.100k.tsv` 与 `queries.dev.200.tsv` 生成 100K real embedding cache。当前 1K / 10K 结果仍不能作为 100K SLA 或 product e2e 性能证明。
