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
- [index.md](index.md)：文档索引和后续建议。

## 当前结论

B0 preflight、B1 fixture/cache generation、B2 ground truth generation、B3 core_hnsw_v2 index-only benchmark、B4 FAISS index-only baseline、B-final consolidation 均已完成。真实 embedding cache（1000 records / 200 queries，`bge-small-zh-v1.5`，dim=512，L2-normalized，IP metric）、core_flat exact ground truth（topk=10）、core_hnsw_v2 index-only benchmark 和 FAISS IndexFlatIP / IndexHNSWFlat baseline 结果已生成并通过校验，位于仓库外 `/tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/`。当前仍只能视为 1K smoke/correctness 规模结论。

原因：

- `core_hnsw_v2` product 主路径、snapshot、async rebuild、VINFO observability 已具备。
- 真实 embedding cache + ground truth 生成链路已验证：B1 复用生产 ONNX runtime/tokenizer（无 hash fallback），B2 复用生产 core_flat exact IP（无 ONNX），全部 checksum/determinism 通过。
- B3 复用现有 core_hnsw_v2 API 完成 index-only benchmark：recall@10=1.000000，search_avg_us=705.440，p95=724，build_us=2,059,178，memory_bytes=8,502,864（estimated）。
- B4 直接使用 FAISS C++ index-only API：IndexFlatIP 与 B2 strict rank/score 对齐，IndexHNSWFlat recall@10=1.000000，search_avg_us=920.485，p95=947，build_us=255,683，actual level-0 neighbor capacity=96。
- 当前产品 backend 只支持 `core_flat` 与 `core_hnsw_v2`；FAISS product backend 已删除并映射到 `core_flat` compatibility fallback。
- 新 B4 runner 已替代旧 FAISS benchmark 脚本缺失的 `tests/vector_index_only_formal.cpp` 依赖；它仍是 index-only 外部 baseline，不是 product backend。
- `config/kvstore-embedding.conf` 仍配置旧 `hnswlib_hnsw` 名称，会被 resolver 映射到 `core_flat`，不适合作为 v2 真实 embedding benchmark 默认配置。

推荐下一步先提交 B0-B4 benchmark readiness 工具/测试/文档；之后进入 `B-scale-0`，冻结 10K/100K real corpus 来源和生成策略。当前 1K 结果不能作为大规模 SLA 或 product e2e 性能证明。
