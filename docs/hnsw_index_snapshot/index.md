# HNSW index snapshot 文档索引

## 本目录文档

- [README.md](README.md)：HNSW-S 路线定位、原则和与 R / buildperf 的边界。
- [snapshot_design.md](snapshot_design.md)：manifest / snapshot 文件模型、校验规则、startup load、写入和失败策略。
- [implementation_plan.md](implementation_plan.md)：HNSW-S0 到 HNSW-S6 的阶段拆分、允许修改、禁止修改、测试和成功标准。
- [hnsw_s0_project_structure_analysis_report.md](hnsw_s0_project_structure_analysis_report.md)：本阶段项目结构分析和 S0 决策。
- [codebase_hygiene_before_s1_report.md](codebase_hygiene_before_s1_report.md)：进入 HNSW-S1 前的 vector / HNSW 代码结构、测试分类、边界和 cleanup 审计。
- [hnsw_s1_minimal_manifest_report.md](hnsw_s1_minimal_manifest_report.md)：S1 minimal manifest schema、读写、校验、checksum helper 和测试结果。
- [hnsw_s2_core_hnsw_v2_snapshot_roundtrip_report.md](hnsw_s2_core_hnsw_v2_snapshot_roundtrip_report.md)：S2 core_hnsw_v2 graph cache serialize / deserialize、目录组织、roundtrip 测试和边界检查。
- [hnsw_s3_snapshot_load_fallback_report.md](hnsw_s3_snapshot_load_fallback_report.md)：S3 records load 后 snapshot validate / load / fallback rebuild 接入、测试覆盖和决策。
- [hnsw_s4_snapshot_write_after_rebuild_report.md](hnsw_s4_snapshot_write_after_rebuild_report.md)：S4 rebuild 成功后 best-effort 写 manifest + snapshot、失败不阻塞 rebuild、测试覆盖和决策。
- [hnsw_s5_vinfo_snapshot_status_report.md](hnsw_s5_vinfo_snapshot_status_report.md)：S5 VINFO 暴露 snapshot load / fallback / write status、non-v2 语义和测试结果。
- [hnsw_s6_shadow_publish_snapshot_report.md](hnsw_s6_shadow_publish_snapshot_report.md)：S6 shadow publish skeleton 成功后 best-effort 写 snapshot、失败不回滚 publish、测试覆盖和决策。
- [hnsw_s_final_consolidation_report.md](hnsw_s_final_consolidation_report.md)：S-final 路线收口、能力矩阵、边界审计、风险清单、后续路线分流和回归验证结果。

## 关联历史文档

- [HNSW 产品化路线](../hnsw_productization/README.md)
- [HNSW-P2 100K balanced default report](../hnsw_productization/hnsw_p2_100k_balanced_default_report.md)
- [HNSW-P5 rebuild / cold-start strategy](../hnsw_productization/hnsw_p5_rebuild_cold_start_strategy.md)
- [HNSW-P5 rebuild / cold-start report](../hnsw_productization/hnsw_p5_rebuild_cold_start_report.md)
- [HNSW-P5 build / rebuild bottleneck analysis](../hnsw_productization/hnsw_p5_build_rebuild_bottleneck_analysis.md)
- [core_hnsw_v2 final architecture](../vector_core_reset/core_hnsw_v2_final_architecture.md)
- [core_hnsw_v2 default backend summary](../vector_core_reset/core_hnsw_v2_default_backend_summary.md)

## 命名规则

后续 snapshot / manifest 冷启动加速路线统一使用 `HNSW-S*` 命名，不继续占用 HNSW-P rebuild 产品化编号，也不恢复 Stage 9G / 9H 命名。

HNSW-S-final 后不继续扩展当前 S 线功能。新的工作应按问题域进入 `HNSW-R`、`HNSW-buildperf`、real/proxy embedding benchmark，或 future `HNSW-S` 的 GC / mmap / crash-consistency 子路线。
