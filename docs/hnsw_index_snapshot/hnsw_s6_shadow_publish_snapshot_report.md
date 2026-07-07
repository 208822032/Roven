# HNSW-S6: shadow publish 后写 snapshot 报告

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

冲突说明：`AGENTS.md` 的历史 stage 模板要求阶段报告写入 `docs/vector_core_reset/`，但当前任务明确属于 `docs/hnsw_index_snapshot/` 的 HNSW-S 路线，并要求新增 `docs/hnsw_index_snapshot/hnsw_s6_shadow_publish_snapshot_report.md`。本报告按任务指定目录输出，同时继续遵守 AGENTS 的核心边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不让 `core_hnsw_v2` 访问 `g_vector_state` / manager `record_id_map` / scope / TTL / metadata / question / answer。

## 是否修改生产代码

是。修改范围只限 manager-owned snapshot writer glue 和现有 shadow rebuild skeleton 成功点：

- 不实现 background worker。
- 不实现 reader safety。
- 不让 product `VSEARCH` 读取 rebuild context active pointer。
- 不改变 S3 load / fallback 语义。
- 不改变 S4 records-only rebuild / VINDEX REBUILD writer 语义。
- 不修改 RDB/AOF、`VSEARCH` response、`kvs_vector_index_ops_t`、默认 backend 或默认 HNSW 参数。

## 新增 / 修改文件

S6 新增：

- `tests/vector_index_snapshot_shadow_publish_test.c`
- `tests/vector_index_snapshot_shadow_publish_test.sh`
- `docs/hnsw_index_snapshot/hnsw_s6_shadow_publish_snapshot_report.md`

S6 修改：

- `Makefile`
- `include/vector/kvs_vector_manager.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_rebuild_shadow.c`
- `docs/hnsw_index_snapshot/README.md`
- `docs/hnsw_index_snapshot/snapshot_design.md`
- `docs/hnsw_index_snapshot/implementation_plan.md`
- `docs/hnsw_index_snapshot/index.md`

## 当前 shadow publish path 分析

```text
current shadow publish path:
  function:
    kvs_vector_rebuild_swap_ready_shadow_for_test()
      -> kvs_vector_rebuild_promote_shadow()
  file:
    src/vector/kvs_vector_rebuild_shadow.c
    src/vector/kvs_vector_rebuild.c
  product path or test/internal:
    test/internal harness
  whether active index is actually replaced:
    rebuild context active_index is replaced by shadow_index
    product g_vector_state.current_index is not replaced
  whether old active goes retired:
    yes, rebuild context old active_index moves to retired_index
  where success is known:
    kvs_vector_rebuild_promote_shadow() returns KVS_VECTOR_OK after no-fail pointer move
```

当前仓库仍只有 9F 系列 shadow rebuild skeleton / harness，没有完整 product background rebuild publish path。S6 因此只在已有 skeleton 成功点接入 snapshot writer glue，没有硬造 background worker。

## Snapshot writer hook 接入点

新增内部 helper：

```c
int kvs_vector_index_snapshot_write_hnsw_v2_index(
    kvs_vector_state_t *state,
    const kvs_hnsw_v2_index_t *hnsw,
    char *reason,
    size_t reason_size
);
```

该 helper 复用 S4 的流程，但允许写指定的 promoted rebuild-active v2 index：

```text
validate specified v2 graph
serialize specified v2 graph to vector_index.snapshot.tmp
build manifest from manager records/config + specified v2 params
write vector_index.manifest.tmp
publish tmp files to vector_index.snapshot / vector_index.manifest
update manager snapshot write status
```

原 `kvs_vector_index_snapshot_write_current()` 保持原语义，继续写产品 `g_vector_state.core_hnsw_v2_index`。

S6 hook：

```text
kvs_vector_rebuild_swap_ready_shadow_for_test()
  -> kvs_vector_rebuild_promote_shadow()
  -> if promote success and current backend is core_hnsw_v2:
       kvs_vector_index_snapshot_write_hnsw_v2_index(promoted_rebuild_active)
     else:
       skip snapshot write
```

## publish / write 失败语义

publish 成功：

- shadow index 被 promote 为 rebuild context active index。
- old active 进入 rebuild context retired slot。
- 如果 backend 是 `core_hnsw_v2`，best-effort 写 manifest + snapshot。
- 写成功后 S5 VINFO 字段可观测 `snapshot_write_attempted=1`、`snapshot_written=1`、bytes/checksum 非 0。

publish 失败：

- 不调用 snapshot writer。
- 不生成 manifest / snapshot。
- `snapshot_write_attempted` 保持 0。

snapshot 写失败：

- 不回滚已完成的 promote。
- 不改变 `kvs_vector_rebuild_swap_ready_shadow_for_test()` 的成功返回。
- rebuild context active search 仍正常。
- product active search 仍正常。
- S5 write status 记录 `snapshot_write_attempted=1`、`snapshot_written=0` 和错误原因。

non-core_hnsw_v2：

- shadow harness 仍可测试 v2 shadow lifecycle。
- 当前 backend 不是 `core_hnsw_v2` 时不写 snapshot。
- 不更新 write attempted 状态。

## Snapshot status / VINFO

S6 不新增 VINFO 字段，复用 S5 字段：

```text
snapshot_write_attempted
snapshot_written
snapshot_last_write_error
snapshot_last_write_started_ms
snapshot_last_write_finished_ms
snapshot_last_bytes
snapshot_last_checksum
```

新增测试通过 `kvs_vector_info()` 验证 shadow publish 写成功后这些字段可被 VINFO 源数据观测。`VSEARCH` response 未修改。

## 与 S1-S5 的关系

- S1 提供 manifest schema、read/write/validate/checksum helper。
- S2 提供 `core_hnsw_v2` graph cache serialize / deserialize / validate。
- S3 在 records load 后 validate/load snapshot，失败 fallback records-only rebuild。
- S4 在 fallback rebuild 或 explicit VINDEX REBUILD 成功后 best-effort 写 snapshot。
- S5 把 load/write status 暴露到 VINFO。
- S6 把同一个 snapshot writer 接到已有 shadow publish skeleton 成功点。

## 为什么不改 VSEARCH response

S6 只改变内部 cache 写入触发点。`VSEARCH` 仍沿用：

```text
core_hnsw_v2 search -> manager resolve/filter -> response assembly
```

没有暴露 snapshot 路径、slot、record_id、graph 或 rebuild context active pointer。

## 为什么不改 RDB/AOF

snapshot 仍是独立 cache 文件。records / RDB / AOF 仍是 source of truth。snapshot 缺失、损坏、过期或写失败只影响后续 cold-start 速度，不影响 records load / replay。

## 为什么 snapshot 仍不是 source of truth

S6 写出的 snapshot 仍必须在 S3 load path 中先用 manager records/config 构造 expected manifest，再校验 manifest、snapshot size/checksum、deserialize、graph basic validate 和 loaded record bytes。任一失败都会 fallback records-only rebuild。

## 为什么不做 GC / mmap / buildperf

这些是独立风险面：

- GC / 多版本 snapshot 需要文件生命周期设计。
- mmap / view 会改变 snapshot IO 和 memory model。
- buildperf 属于 HNSW build CPU 优化路线。
- background worker / reader safety 属于 HNSW-R 产品化路线。

S6 只做 shadow publish 成功后的 writer glue。

## 测试结果

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
```

必跑测试：

```text
tests/vector_index_snapshot_manifest_test.sh PASS
tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh PASS
tests/vector_index_snapshot_load_after_records_test.sh PASS
tests/vector_index_snapshot_write_after_rebuild_test.sh PASS
tests/vector_index_snapshot_vinfo_status_test.sh PASS
tests/vector_index_snapshot_shadow_publish_test.sh PASS

tests/vector_rebuild_state_machine_test.sh PASS
tests/vector_rebuild_delta_capture_test.sh PASS
tests/vector_rebuild_shadow_replay_test.sh PASS
tests/vector_rebuild_shadow_swap_test.sh PASS

tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
tests/vector_vinfo_backend_stats_test.sh PASS
tests/vector_core_flat_rdb_reload_test.sh PASS
```

新增 S6 测试覆盖：

- shadow publish 成功后写出 manifest + snapshot。
- 写出的 snapshot 可被 S3 load path 加载。
- shadow publish 失败时不写 snapshot。
- snapshot write failure 不回滚 shadow publish。
- snapshot write failure 后 rebuild active search 和 product active search 正常。
- non-core_hnsw_v2 backend 不写 snapshot。
- VINFO 源数据能观测 shadow publish 后 write status。
- retired cleanup / rebuild active cleanup 不影响已写 snapshot 的后续 load。

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

当前 worktree 包含大量前序 CODE-HYGIENE / HNSW 产品化 / HNSW-S1-S5 未提交改动。S6 相关新增/修改已列在上方。`git status --short` 当前输出：

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
 M src/vector/kvs_vector_rebuild_shadow.c
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
?? tests/vector_index_snapshot_shadow_publish_test.c
?? tests/vector_index_snapshot_shadow_publish_test.sh
?? tests/vector_index_snapshot_vinfo_status_test.c
?? tests/vector_index_snapshot_vinfo_status_test.sh
?? tests/vector_index_snapshot_write_after_rebuild_test.c
?? tests/vector_index_snapshot_write_after_rebuild_test.sh
```

## HNSW-S6 decision

```text
HNSW-S6 decision:
  production_code_changed = yes
  shadow_publish_snapshot_hook_added = yes
  background_worker_added = no
  reader_safety_added = no
  vsearch_response_changed = no
  rdb_aof_changed = no
  index_ops_changed = no
  snapshot_is_source_of_truth = no
  snapshot_gc_added = no
  ready_for_HNSW_S_final = yes
  next_stage = HNSW-S-final
```
