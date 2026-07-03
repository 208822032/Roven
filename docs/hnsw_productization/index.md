# HNSW 产品化文档索引

## 当前目录

- [README.md](README.md)：HNSW 产品化路线总览，包含 HNSW-P0 到 HNSW-P5。
- [hnsw_p0_recall_attribution_review.md](hnsw_p0_recall_attribution_review.md)：HNSW-P0 审阅任务说明，用于 100K recall 归因和产品化风险判定。
- [hnsw_p0_recall_attribution_report.md](hnsw_p0_recall_attribution_report.md)：HNSW-P0 审阅报告，记录 9G3 数据复盘、benchmark 可信度、参数/图质量风险、parity baseline 方案和下一步路线。
- [hnsw_p1_parity_baseline_report.md](hnsw_p1_parity_baseline_report.md)：HNSW-P1 quick parity baseline 与参数敏感性补测报告，记录 10K/50K shared oracle、baseline/balanced 参数结果、mature library parity 缺口和下一步判断。
- [hnsw_p1b_faiss_parity_report.md](hnsw_p1b_faiss_parity_report.md)：HNSW-P1b FAISS direct parity baseline 报告，记录 test-only FAISS C++ direct harness、10K/50K 同参 baseline 对照、recall gap、build/search 成本差距和 redo-P1b 判断。
- [interview_narrative.md](interview_narrative.md)：面试复盘讲法，覆盖 manager/vector_core 边界、v2 backend、scale gate 和下一步归因路径。

## 相关 reset 文档

- [Stage 9G3 scale/stability final consolidation](../vector_core_reset/stage9g3_scale_stability_final_consolidation.md)
- [core_hnsw_v2 final architecture](../vector_core_reset/core_hnsw_v2_final_architecture.md)
- [core_hnsw_v2 interview review](../vector_core_reset/core_hnsw_v2_interview_review.md)
- [core_hnsw_v2 default backend summary](../vector_core_reset/core_hnsw_v2_default_backend_summary.md)
