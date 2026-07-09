# HNSW-R 文档索引

## 当前文档

- [README.md](README.md): 目录目的、HNSW-S / HNSW-R 边界和当前路线结论。
- [hnsw_r_execution_plan.md](hnsw_r_execution_plan.md): HNSW-R0 / product background rebuild execution plan。
- [hnsw_r_final_product_rebuild_consolidation_report.md](hnsw_r_final_product_rebuild_consolidation_report.md): HNSW-R1 到 HNSW-R7 的最终收口报告。

## HNSW-R 阶段报告

- [HNSW-R1 state machine hardening](hnsw_r1_sync_rebuild_state_machine_hardening_report.md)
- [HNSW-R2 reader safety primitive](hnsw_r2_reader_safety_primitive_report.md)
- [HNSW-R3 active / shadow / retired publish](hnsw_r3_active_shadow_retired_publish_report.md)
- [HNSW-R4 delta generation boundary](hnsw_r4_delta_generation_boundary_report.md)
- [HNSW-R5 shadow replay final validation](hnsw_r5_shadow_replay_final_validation_report.md)
- [HNSW-R6 background worker async rebuild](hnsw_r6_background_worker_async_rebuild_report.md)
- [HNSW-R7 post-publish snapshot writer hook](hnsw_r7_post_publish_snapshot_writer_hook_report.md)
- [HNSW-R-final product rebuild consolidation](hnsw_r_final_product_rebuild_consolidation_report.md)

## 关联旧文档

- [HNSW-S final consolidation](../hnsw_index_snapshot/hnsw_s_final_consolidation_report.md)
- [HNSW-S cold-start benchmark](../hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md)
- [HNSW-S snapshot design](../hnsw_index_snapshot/snapshot_design.md)
- [Stage 9F1 rebuild state machine skeleton](../vector_core_reset/stage9f1_manager_owned_rebuild_state_machine_skeleton.md)
- [Stage 9F3 delta capture skeleton](../vector_core_reset/stage9f3_manager_write_path_delta_capture_skeleton.md)
- [Stage 9F4 shadow replay harness](../vector_core_reset/stage9f4_shadow_rebuild_replay_harness.md)
- [Stage 9F5 active/shadow swap skeleton](../vector_core_reset/stage9f5_active_shadow_swap_skeleton.md)

## 阶段报告命名

```text
hnsw_r1_sync_rebuild_state_machine_hardening_report.md
hnsw_r2_reader_safety_primitive_report.md
hnsw_r3_active_shadow_retired_publish_report.md
hnsw_r4_delta_generation_boundary_report.md
hnsw_r5_shadow_replay_final_validation_report.md
hnsw_r6_background_worker_async_rebuild_report.md
hnsw_r7_post_publish_snapshot_writer_hook_report.md
hnsw_r_final_product_rebuild_consolidation_report.md
```

这些报告应继续放在 `docs/hnsw_rebuild_productization/`，除非后续任务明确改变路线。
