# HNSW-S5: VINFO 暴露 snapshot status 报告

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

冲突说明：`AGENTS.md` 的历史 stage 模板要求阶段报告写入 `docs/vector_core_reset/`；当前 HNSW-S 路线和本任务明确要求报告写入 `docs/hnsw_index_snapshot/`。本报告按 HNSW-S 路线放在 `docs/hnsw_index_snapshot/`，同时继续遵守 AGENTS 的核心边界：不修改 RDB/AOF 主格式、不修改 VSEARCH response、不修改 `kvs_vector_index_ops_t`、不让 `core_hnsw_v2` 访问 `g_vector_state` / manager `record_id_map` / scope / TTL / metadata / question / answer。

## 是否修改生产代码

是。生产代码修改仅用于把 S3 / S4 已有 manager-owned snapshot status 拷贝到 `kvs_vector_info_t` 并组装进 `VINFO`。

未修改：

- `VSEARCH` response
- RDB/AOF 主格式
- `kvs_vector_index_ops_t`
- 默认 backend
- 默认 HNSW 参数
- snapshot load / fallback 语义
- snapshot writer 语义
- `core_hnsw_v2` graph/search 算法
- background rebuild / snapshot GC / mmap / admin command

## 新增 / 修改文件

S5 相关新增：

- `tests/vector_index_snapshot_vinfo_status_test.c`
- `tests/vector_index_snapshot_vinfo_status_test.sh`
- `docs/hnsw_index_snapshot/hnsw_s5_vinfo_snapshot_status_report.md`

S5 相关修改：

- `Makefile`
- `include/kvs_vector.h`
- `src/vector/kvs_vector.c`
- `src/core/kvs_operation.c`
- `tests/vector_vinfo_backend_stats_test.sh`
- `docs/hnsw_index_snapshot/README.md`
- `docs/hnsw_index_snapshot/index.md`
- `docs/hnsw_index_snapshot/snapshot_design.md`
- `docs/hnsw_index_snapshot/implementation_plan.md`

说明：当前 worktree 还包含大量前序 CODE-HYGIENE / HNSW 产品化 / HNSW-S1-S4 未提交改动，本报告只说明 S5 直接涉及的文件。

## 当前调用链

```text
VINFO
  -> handle_vinfo_command()
  -> kvs_vector_info()
  -> read manager-owned g_vector_state.snapshot_status
  -> handle_vector_info_response()
  -> RESP key/value array
```

snapshot status 仍由 KVManager 持有。`core_hnsw_v2` 不读取 manager 全局状态，也不参与 response assembly。

## Before 行为

S3 / S4 已经维护 internal snapshot status：

```text
load attempted / loaded / valid / loaded_from_snapshot / fallback count / last error
write attempted / written / last write error / write time / bytes / checksum
```

但这些状态只存在于 manager 内部和测试 helper，线上只能通过日志间接观察，无法通过 `VINFO` 做 machine-checkable 诊断。

## After 设计

`kvs_vector_info_t` 新增 snapshot status 字段，`kvs_vector_info()` 从 manager-owned `g_vector_state.snapshot_status` 拷贝，`VINFO` 统一输出固定 key/value 字段。

第一版不暴露完整 manifest / snapshot 文件路径，避免测试环境路径泄漏和响应膨胀。错误文本在 `VINFO` 中限制为 `%.90s`。

验证过程中发现 `VINFO` 组装处存在重复追加 `last_rebuild_ms` 的代码。S5 修正为只输出一次，并同步调整 RESP array count，避免 VINFO 后续命令读取被多余 RESP bulk item 污染。

## VINFO 新增字段

```text
snapshot_supported
snapshot_load_attempted
snapshot_loaded
snapshot_valid
loaded_from_snapshot
snapshot_fallback_rebuild_count
snapshot_last_error

snapshot_write_attempted
snapshot_written
snapshot_last_write_error
snapshot_last_write_started_ms
snapshot_last_write_finished_ms
snapshot_last_bytes
snapshot_last_checksum
```

## non-v2 backend 语义

```text
core_hnsw_v2:
  snapshot_supported = 1
  其余字段来自 manager snapshot_status

core_flat / non-v2:
  snapshot_supported = 0
  snapshot_load_attempted = 0
  snapshot_write_attempted = 0
  loaded / valid / loaded_from_snapshot / written = 0
  error 字段为空
```

这样 VINFO 消费方可以稳定检查同一组 key，不需要按 backend 猜测字段是否存在。

## snapshot load status 映射

```text
snapshot_load_attempted <- status.snapshot_load_attempted
snapshot_loaded <- status.snapshot_loaded
snapshot_valid <- status.snapshot_valid
loaded_from_snapshot <- status.loaded_from_snapshot
snapshot_fallback_rebuild_count <- status.fallback_rebuild_count
snapshot_last_error <- status.last_snapshot_error
```

这些字段只描述最近一次 startup / rebuild-after-load 的 snapshot load 尝试结果。valid snapshot load 时 `snapshot_loaded=1`、`snapshot_valid=1`、`loaded_from_snapshot=1`。missing / mismatch / corrupt / deserialize failure 时 `snapshot_loaded=0`、`snapshot_valid=0`，并通过 fallback rebuild count 和 error 文本观测。

## snapshot write status 映射

```text
snapshot_write_attempted <- status.snapshot_write_attempted
snapshot_written <- status.snapshot_written
snapshot_last_write_error <- status.last_snapshot_write_error
snapshot_last_write_started_ms <- status.last_snapshot_write_started_ms
snapshot_last_write_finished_ms <- status.last_snapshot_write_finished_ms
snapshot_last_bytes <- status.last_snapshot_bytes
snapshot_last_checksum <- status.last_snapshot_checksum
```

这些字段只描述最近一次 best-effort writer 尝试。写成功时 `snapshot_written=1` 且 bytes/checksum 非 0。写失败时 rebuild 仍成功，active index 不变，`snapshot_written=0` 且 `snapshot_last_write_error` 非空。

## Safety invariants

- snapshot status 只用于 `VINFO` / diagnostics，不参与数据正确性判断。
- records 仍是 source of truth。
- snapshot 仍是可删除、可过期、可损坏的 derived cache。
- load / fallback / writer 返回路径不因 VINFO 字段改变。
- `core_hnsw_v2` 仍只处理 record_id / vector / score / graph cache。

## 为什么不改 VSEARCH response

S5 是观测阶段，只给 `VINFO` 增加状态字段。`VSEARCH` 仍保持：

```text
core_hnsw_v2 search -> manager resolve/filter -> response assembly
```

用户查询响应不暴露 snapshot、slot、record_id 或 graph 状态。

## 为什么不改 RDB/AOF

snapshot 是独立 cache 文件，不是 records source of truth。RDB/AOF 仍只保存 records。S5 只读 runtime status 并输出到 VINFO，不增加任何持久化字段。

## 为什么 snapshot 仍不是 source of truth

启动时必须先恢复 records，再用 manager records/config 生成 expected manifest 并校验 snapshot。任何 mismatch、missing、checksum error 或 deserialize failure 都 fallback records-only rebuild。VINFO 只反映这个过程的结果，不改变信任模型。

## 为什么不做 background rebuild / GC / mmap

这些属于后续独立路线：

- background shadow rebuild 属于 HNSW-R / HNSW-S6 orchestration glue。
- snapshot GC / 多版本 snapshot 需要单独设计文件生命周期。
- mmap serving 会改变 snapshot IO / memory 模型。

S5 只暴露已有状态，不扩展 snapshot 功能。

## 与 S1 / S2 / S3 / S4 的关系

- S1 提供 manifest schema、read/write/validate/checksum helper。
- S2 提供 `core_hnsw_v2` graph cache serialize / deserialize / validate。
- S3 在 records load 后 validate/load snapshot，失败 fallback rebuild，并维护 load status。
- S4 在 fallback rebuild 或 explicit VINDEX REBUILD 成功后 best-effort 写 snapshot，并维护 write status。
- S5 把 S3 / S4 已有 status 暴露到 `VINFO`。

## 测试结果

已运行：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS

tests/vector_index_snapshot_manifest_test.sh PASS
tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh PASS
tests/vector_index_snapshot_load_after_records_test.sh PASS
tests/vector_index_snapshot_write_after_rebuild_test.sh PASS
tests/vector_index_snapshot_vinfo_status_test.sh PASS

tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
tests/vector_vinfo_backend_stats_test.sh PASS
tests/vector_core_flat_rdb_reload_test.sh PASS
```

测试覆盖：

- fresh start / no snapshot 的 VINFO 字段存在性和 `snapshot_supported=1`。
- valid snapshot load 后 `snapshot_loaded=1`、`snapshot_valid=1`、`loaded_from_snapshot=1`。
- missing / corrupt snapshot fallback rebuild 后 fallback count / error 可观测。
- rebuild 后 writer success 可观测，bytes/checksum 非 0。
- writer failure 不阻塞 rebuild，error 可观测，search 仍正常。
- `core_flat` 下 `snapshot_supported=0`，不尝试 load/write。
- 产品路径 `VINFO` RESP 包含所有 snapshot keys。

测试生成的仓库根目录 `vector_index.manifest` / `vector_index.snapshot` 已清理。

## Boundary grep

```text
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 src/vector/core/hnsw_v2_snapshot include/vector/kvs_vector_hnsw_v2.h include/vector/kvs_vector_hnsw_v2_snapshot.h || true
no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 src/vector/core/hnsw_v2_snapshot include/vector/kvs_vector_hnsw_v2.h include/vector/kvs_vector_hnsw_v2_snapshot.h || true
no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 src/vector/core/hnsw_v2_snapshot include/vector/kvs_vector_hnsw_v2.h include/vector/kvs_vector_hnsw_v2_snapshot.h || true
no output
```

## git diff --check

```text
PASS
```

## git status --short

当前 worktree 包含大量前序 CODE-HYGIENE / HNSW 产品化 / HNSW-S1-S4 未提交改动。S5 相关新增/修改已列在上方。

```text
 M Makefile
 M docs/hnsw_productization/README.md
 M docs/hnsw_productization/hnsw_p0_recall_attribution_report.md
 M docs/hnsw_productization/hnsw_p2_100k_balanced_default_report.md
 M docs/hnsw_productization/hnsw_p5_rebuild_cold_start_report.md
 M docs/hnsw_productization/hnsw_p5_rebuild_cold_start_strategy.md
 M docs/hnsw_productization/index.md
 M docs/hnsw_productization/interview_narrative.md
 M docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md
 M docs/vector_core_reset/core_hnsw_v2_final_architecture.md
 M docs/vector_core_reset/core_hnsw_v2_interview_review.md
 M docs/vector_core_reset/stage9e1a_core_hnsw_v2_candidate_quality_attribution.md
 M docs/vector_core_reset/stage9e2_core_hnsw_v2_bounded_heap_optimization.md
 M docs/vector_core_reset/stage9e3c_core_hnsw_v2_reverse_prune_score_reuse.md
 M docs/vector_core_reset/stage9g2_default_backend_switch_hardening.md
 M docs/vector_core_reset/stage9g3_scale_stability_final_consolidation.md
 M include/kvs_command_task.h
 M include/kvs_config.h
 M include/kvs_vector.h
 D include/kvs_vector_faiss_backend.h
 D include/kvs_vector_hnswlib_backend.h
 D include/vector/kvs_vector_hnsw.h
RM src/vector/core/hnsw_v2/kvs_hnsw_v2_heap.h -> include/vector/kvs_vector_hnsw_v2_heap.h
RM src/vector/core/hnsw_v2/kvs_hnsw_v2_score_cache.h -> include/vector/kvs_vector_hnsw_v2_score_cache.h
 M include/vector/kvs_vector_index_internal.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_command_executor.c
 M src/core/kvs_command_task.c
 M src/core/kvs_config.c
 M src/core/kvs_operation.c
 D src/vector/core/hnsw/kvs_hnsw_add.c
 D src/vector/core/hnsw/kvs_hnsw_context.c
 D src/vector/core/hnsw/kvs_hnsw_delete.c
 D src/vector/core/hnsw/kvs_hnsw_graph.c
 D src/vector/core/hnsw/kvs_hnsw_index.c
 D src/vector/core/hnsw/kvs_hnsw_info.c
 D src/vector/core/hnsw/kvs_hnsw_random.c
 D src/vector/core/hnsw/kvs_hnsw_search.c
 M src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c
 M src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c
 M src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c
 M src/vector/kvs_vector.c
 D src/vector/kvs_vector_faiss_backend.cpp
 D src/vector/kvs_vector_faiss_ops.c
 D src/vector/kvs_vector_hnswlib_backend.cpp
 D src/vector/kvs_vector_hnswlib_ops.c
 M src/vector/kvs_vector_index.c
 M tests/README.md
 D tests/faiss_baseline.cpp
 D tests/hnswlib_smoke/hnswlib_smoke.cpp
 D tests/vcmd_onnx_faiss_bench.cpp
 M tests/vector_core_benchmark_compare.sh
 M tests/vector_core_flat_correctness_test.sh
 M tests/vector_core_flat_rdb_reload_test.sh
 M tests/vector_core_flat_rebuild_test.sh
 D tests/vector_core_hnsw_aof_replay_test.sh
 D tests/vector_core_hnsw_correctness_test.sh
 D tests/vector_core_hnsw_faiss_parity_bridge.c
 D tests/vector_core_hnsw_faiss_parity_bridge.h
 D tests/vector_core_hnsw_faiss_parity_test.cpp
 D tests/vector_core_hnsw_faiss_parity_test.sh
 D tests/vector_core_hnsw_rdb_reload_test.sh
 D tests/vector_core_hnsw_rebuild_test.sh
 M tests/vector_core_hnsw_v2_100k_balanced_gate_test.sh
 M tests/vector_core_hnsw_v2_default_backend_test.c
 M tests/vector_core_hnsw_v2_default_backend_test.sh
 M tests/vector_core_hnsw_v2_distance_profile_test.sh
 M tests/vector_core_hnsw_v2_graph_construction_test.sh
 M tests/vector_core_hnsw_v2_parity_baseline_test.c
 M tests/vector_core_hnsw_v2_parity_baseline_test.sh
 M tests/vector_core_hnsw_v2_performance_profile_test.sh
 M tests/vector_core_hnsw_v2_product_integration_test.sh
 M tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh
 M tests/vector_core_hnsw_v2_scale_stability_test.sh
 M tests/vector_core_hnsw_v2_search_recall_test.sh
 M tests/vector_core_hnsw_v2_skeleton_test.sh
 M tests/vector_core_hnsw_v2_tombstone_policy_test.sh
 M tests/vector_core_hnsw_v2_tombstone_test.sh
 D tests/vector_index_only_formal.cpp
 M tests/vector_manager_resolve_fastpath_test.sh
 M tests/vector_rebuild_delta_capture_test.sh
 M tests/vector_rebuild_shadow_replay_test.sh
 M tests/vector_rebuild_shadow_swap_test.sh
 M tests/vector_rebuild_state_machine_test.sh
 D tests/vector_search_bench.c
 M tests/vector_testcase.c
 M tests/vector_vinfo_backend_stats_test.sh
 M tests/vector_vsearch_profile_breakdown_test.sh
?? docs/hnsw_index_snapshot/
?? docs/hnsw_productization/hnsw_p5_build_rebuild_bottleneck_analysis.md
?? include/vector/kvs_vector_backend_resolve.h
?? include/vector/kvs_vector_hnsw_v2_snapshot.h
?? include/vector/kvs_vector_index_snapshot.h
?? src/vector/core/hnsw_v2_snapshot/
?? src/vector/kvs_vector_backend_resolve.c
?? src/vector/kvs_vector_index_snapshot.c
?? tests/vector_core_hnsw_v2_snapshot_roundtrip_test.c
?? tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh
?? tests/vector_index_snapshot_load_after_records_test.c
?? tests/vector_index_snapshot_load_after_records_test.sh
?? tests/vector_index_snapshot_manifest_test.c
?? tests/vector_index_snapshot_manifest_test.sh
?? tests/vector_index_snapshot_vinfo_status_test.c
?? tests/vector_index_snapshot_vinfo_status_test.sh
?? tests/vector_index_snapshot_write_after_rebuild_test.c
?? tests/vector_index_snapshot_write_after_rebuild_test.sh
```

## 下一步建议

进入 HNSW-S6：把 background shadow rebuild publish 成功后的 snapshot writer 与 active / shadow / retired 生命周期做 manager-owned glue。不要在 S6 混入 graph construction、SIMD/layout、snapshot GC、多版本 snapshot 或 RDB/AOF graph persistence。

```text
HNSW-S5 decision:
  production_code_changed = yes
  vinfo_snapshot_status_added = yes
  vsearch_response_changed = no
  rdb_aof_changed = no
  index_ops_changed = no
  snapshot_is_source_of_truth = no
  background_rebuild_added = no
  snapshot_gc_added = no
  ready_for_HNSW_S6 = yes
  next_stage = HNSW-S6
```
