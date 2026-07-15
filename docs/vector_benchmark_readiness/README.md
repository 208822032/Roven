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
- [b_scale_3b_core_hnsw_v2_build_optimization_report.md](b_scale_3b_core_hnsw_v2_build_optimization_report.md)：T2Ranking 10K core_hnsw_v2 build path audit 与局部优化 gate，记录 reverse-prune/neighbor-select 热点、三次重复结果、graph/search 完全一致性和未达到 5% 的候选结论。
- [b_scale_4a_100k_embedding_parallel_throughput_preflight_report.md](b_scale_4a_100k_embedding_parallel_throughput_preflight_report.md)：T2Ranking 100K embedding 连续行分片/resume/merge 能力与 2K real-record 并行吞吐预检，记录当前 4 vCPU VM 未通过 28 records/s gate。
- [b_scale_4_t2ranking_100k_embedding_cache_report.md](b_scale_4_t2ranking_100k_embedding_cache_report.md)：T2Ranking 100K real ONNX embedding cache 正式生成，记录 90K 分片推理、资源、原子发布、checksum 和 B-scale-5 输入契约。
- [b_scale_5_t2ranking_100k_exact_groundtruth_report.md](b_scale_5_t2ranking_100k_exact_groundtruth_report.md)：T2Ranking 100K core_flat exact normalized-IP ground truth，记录 double brute-force 全量 cross-check、score diff 和 B-scale-6 输入契约。
- [b_scale_6_t2ranking_100k_hnsw_faiss_index_only_report.md](b_scale_6_t2ranking_100k_hnsw_faiss_index_only_report.md)：T2Ranking 100K core_hnsw_v2 / FAISS index-only 完整 efSearch sweep，记录 backend checkpoint/resume、build/search/recall、独立审计和原子发布。
- [b_scale_7_t2ranking_100k_product_e2e_report.md](b_scale_7_t2ranking_100k_product_e2e_report.md)：T2Ranking 100K core_hnsw_v2 产品 E2E，记录 prepared VSET/VSEARCH、公开文本 ONNX VSEARCH、产品开销、correctness defect 修复和完整 sweep。
- [index.md](index.md)：文档索引和后续建议。

## 当前结论

B0 preflight、B1 fixture/cache generation、B2 ground truth generation、B3 core_hnsw_v2 index-only benchmark、B4 FAISS index-only baseline、B-final consolidation 均已完成。真实 embedding cache（1000 records / 200 queries，`bge-small-zh-v1.5`，dim=512，L2-normalized，IP metric）、core_flat exact ground truth（topk=10）、core_hnsw_v2 index-only benchmark 和 FAISS IndexFlatIP / IndexHNSWFlat baseline 结果已生成并通过校验，位于仓库外 `/tmp/kvlite_vector_benchmark_artifacts/real_embedding/qa_zh_1k_200_bge_small_zh_v1_5/`。当前仍只能视为 1K smoke/correctness 规模结论。

B-scale-0 已完成：基于固定 T2Ranking revision `2a369a430a70979223f1b9a41b1919774d46b432` 生成 deterministic 10K / 100K 子集，输出位于仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/`。本阶段选择 200 queries，保留 1131 positive pairs / 1131 unique positive pids；10K 是 100K 的前缀子集，全部 positive 保留，重复运行 checksum 不变。

B-scale-1 已完成：复用并扩展 `scripts/benchmark/generate_real_embedding_cache.c`，从 B-scale-0 的 `collection.10k.tsv` / `queries.dev.200.tsv` 生成 T2Ranking 10K real ONNX embedding cache，输出位于仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding/`。本阶段生成 10000 record embeddings 和 200 query embeddings（dim=512，float32，L2 normalized，IP metric），核心文件重复生成 byte-identical。

B-scale-2 已完成：复用并扩展 `scripts/benchmark/generate_real_embedding_groundtruth.c`，读取 B-scale-1 cache 并使用 core_flat exact normalized IP 生成 T2Ranking 10K exact ground truth，输出位于仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth/`。本阶段生成 200 query 的 topK=10 ground truth，并用独立 double precision brute-force 对全部 query / 10000 documents 复算一致；T2Ranking qrels 未参与 ANN exact topK 生成。

B-scale-3 已完成：新增 `scripts/benchmark/generate_real_embedding_hnsw_faiss_sweep_benchmark.cpp` 和 `scripts/benchmark/real_embedding_core_hnsw_v2_sweep_helper.c`，读取 B-scale-1 cache 与 B-scale-2 exact ground truth，使用同一 qrels-free deterministic insertion order 对 core_hnsw_v2 和 FAISS IndexHNSWFlat 做 `efSearch=32,64,128,256,512,1024` index-only sweep，并用 FAISS IndexFlatIP 做 exact parity。输出位于仓库外 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_3_10k_benchmark/`。本阶段未调用 ONNX、未重新生成 ground truth、未运行 product e2e benchmark；qrels 未参与 ANN recall。

B-scale-3B 已完成：在不改变图语义、参数或搜索输出的前提下，对 core_hnsw_v2 10K build path 做三次重复 profiling 与两个局部候选验证。实际热点是 reverse-prune / neighbor-select；关闭 build wall-clock profiling 的 median 变化为 `-0.48%`，build-only metric dispatch prototype 为 `+2.46%`，均未达到约 5% 接受线，prototype 已回退。九次运行 graph SHA-256、六档 returned IDs/顺序/score/recall 和 exact counters 完全一致。下一阶段仍为 B-scale-4。

B-scale-4A preflight 已完成：real embedding cache 工具已支持确定性连续行 shard、checksum resume、B-scale-1 前 10K/query cache 复用和全局 row 顺序原子 merge。当前 4 vCPU / 3.8 GiB VM 的最佳完整配置为 `2p x 2t / batch32 = 18.058 records/s`，未达到 28 records/s；batch64 产生严重内存/换页压力，4p/5p 不适合在该 VM 启动。建议在至少 6 vCPU / 12-14 GiB RAM 上复测 `5p x 1t / batch32`，通过后再生成完整 90K。

B-scale-4 已完成：升级后的 8 vCPU / 15 GiB VM 使用 `4 processes x 2 threads / batch32`，复用 B-scale-1 前 10K records 和全部 query cache，生成新增 90K records 并原子发布 100K cache 到 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_4_100k_embedding/`。正式生成吞吐 `35.374 records/s`，无 swap/OOM；100K finite/norm、全局 row/id/pid、shard range、前缀和 query byte-identity、最终 checksum/validate 均通过。

B-scale-5 已完成：复用 B-scale-2 runner 和 B-scale-4 100K cache，通过 core_flat exact IP 与独立 double precision brute-force 生成 200 queries / topK=10 ground truth。200/200 query 的 topK ID 和 strict order 完全一致，`nonfinite_score_count=0`；输出位于 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_5_100k_groundtruth/`。

B-scale-6 已完成：复用 B-scale-3 冻结参数和口径，对 B-scale-4/B-scale-5 100K 输入执行 core_hnsw_v2 / FAISS HNSW 完整 sweep。core 与 FAISS HNSW build 分别为 `678,326,503us` / `215,897,392us`；两者均在 efSearch=512 首次达到 recall@10=1.0。结果已通过 backend checkpoint、runner validator 和独立 per-query/checksum 审计，并原子发布到 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_6_100k_benchmark/`。

B-scale-7 已完成：100K records 通过公开 RESP VSET、正常 record commit/ID mapping/current index ops 建立 core_hnsw_v2 产品索引；B-scale-7A 仅绕过 tokenizer/ONNX，B-scale-7B 使用公开文本 VSEARCH 完成 7200 次真实 ONNX encode。prepared/public returned IDs 和顺序完全一致；recall@10 首次在 ef512 达到 1.0。阶段同时修复已有 `vector_hnsw_initial_capacity` 未生效导致 destructive reserve 丢图的 correctness defect。FAISS 产品 backend 不存在，未用 standalone API 冒充产品 E2E。正式结果位于 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_7_100k_product_e2e/`。

原因：

- `core_hnsw_v2` product 主路径、snapshot、async rebuild、VINFO observability 已具备。
- 真实 embedding cache + ground truth 生成链路已验证：B1 复用生产 ONNX runtime/tokenizer（无 hash fallback），B2/B-scale-2 复用生产 core_flat exact IP（真实 ground truth 生成不调用 ONNX），全部 checksum/determinism 通过。
- B3 复用现有 core_hnsw_v2 API 完成 index-only benchmark：recall@10=1.000000，search_avg_us=705.440，p95=724，build_us=2,059,178，memory_bytes=8,502,864（estimated）。
- B4 直接使用 FAISS C++ index-only API：IndexFlatIP 与 B2 strict rank/score 对齐，IndexHNSWFlat recall@10=1.000000，search_avg_us=920.485，p95=947，build_us=255,683，actual level-0 neighbor capacity=96。
- B-scale-3 证明 10K T2Ranking real embedding index-only harness 可以机器校验：core_hnsw_v2 在 efSearch=128 起 recall@10=1.0000，FAISS HNSW 在 efSearch=256 起 recall@10=1.0000；FAISS Flat 与 B-scale-2 exact ground truth strict parity。
- 当前产品 backend 只支持 `core_flat` 与 `core_hnsw_v2`；FAISS product backend 已删除并映射到 `core_flat` compatibility fallback。
- 新 B4 runner 已替代旧 FAISS benchmark 脚本缺失的 `tests/vector_index_only_formal.cpp` 依赖；它仍是 index-only 外部 baseline，不是 product backend。
- `config/kvstore-embedding.conf` 仍配置旧 `hnswlib_hnsw` 名称，会被 resolver 映射到 `core_flat`，不适合作为 v2 真实 embedding benchmark 默认配置。

`ready_for_B_scale_8=yes`。B-scale-7 是单实例、单客户端产品 E2E，不代表并发 SLA；FAISS product parity 仍需独立立项。
