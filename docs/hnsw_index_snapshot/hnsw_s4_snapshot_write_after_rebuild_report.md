# HNSW-S4: rebuild 成功后写 snapshot 报告

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

冲突说明：`AGENTS.md` 的历史 stage 模板要求阶段报告写入 `docs/vector_core_reset/`；当前 HNSW-S 路线和本任务明确要求报告写入 `docs/hnsw_index_snapshot/`。本报告按 HNSW-S 路线放在 `docs/hnsw_index_snapshot/`，同时继续遵守 AGENTS 的核心边界：不修改 RDB/AOF 主格式、不修改 VSEARCH response、不修改 `kvs_vector_index_ops_t`、不让 `core_hnsw_v2` 访问 `g_vector_state` / manager `record_id_map` / scope / TTL / metadata / question / answer。

## 是否修改生产代码

是。修改范围是 manager-owned snapshot lifecycle glue：

- `src/vector/kvs_vector.c`
- `include/vector/kvs_vector_manager.h`

未修改 RDB/AOF、VSEARCH response、`kvs_vector_index_ops_t`、默认 backend、默认 HNSW 参数或 `core_hnsw_v2` graph/search implementation。

## 新增 / 修改文件

S4 新增：

- `tests/vector_index_snapshot_write_after_rebuild_test.c`
- `tests/vector_index_snapshot_write_after_rebuild_test.sh`
- `docs/hnsw_index_snapshot/hnsw_s4_snapshot_write_after_rebuild_report.md`

S4 修改：

- `Makefile`
- `include/vector/kvs_vector_manager.h`
- `src/vector/kvs_vector.c`
- `tests/vector_index_snapshot_load_after_records_test.c`
- `docs/hnsw_index_snapshot/README.md`
- `docs/hnsw_index_snapshot/index.md`
- `docs/hnsw_index_snapshot/snapshot_design.md`
- `docs/hnsw_index_snapshot/implementation_plan.md`

## Snapshot writer 接入点

新增 manager-owned writer：

```c
int kvs_vector_index_snapshot_write_current(
    kvs_vector_state_t *state,
    char *reason,
    size_t reason_size
);
```

接入两个 rebuild 成功路径：

```text
kvs_vector_rebuild_after_load()
  -> snapshot load failed
  -> records-only rebuild success
  -> best-effort write snapshot

kvs_vector_index_rebuild()
  -> explicit VINDEX REBUILD success
  -> best-effort write snapshot
```

snapshot load 成功路径直接返回，不重复写 snapshot。非 `core_hnsw_v2` backend 不写 snapshot。

## Atomic write / temp rename 策略

S4 写入流程：

```text
1. validate active core_hnsw_v2 graph
2. serialize active index to vector_index.snapshot.tmp
3. compute tmp snapshot size/checksum
4. build manifest from current manager records/config
5. write vector_index.manifest.tmp
6. backup existing vector_index.snapshot / vector_index.manifest to .bak
7. rename temp snapshot -> vector_index.snapshot
8. rename temp manifest -> vector_index.manifest
9. cleanup .bak on success
10. on normal failure, cleanup tmp and rollback .bak
```

第一版没有强制 fsync。若进程或机器在两文件发布窗口崩溃，可能留下 tmp / bak / 不匹配的正式文件；startup validation 会拒绝不完整或不匹配的 manifest/snapshot，并 fallback records-only rebuild。records 仍是 source of truth。

## Manifest 构造逻辑

复用 S3 的 expected manifest 构造逻辑：

- active_count / deleted_count 来自 manager record store 扫描。
- record_generation 使用当前 records 的最大 generation。
- record_id_checksum 覆盖 active record id。
- vector_checksum 覆盖 active record id、embedding_dim 和 embedding bytes。
- dim / metric / M / M0 / efConstruction / efSearch 来自当前 active v2 index/config。
- snapshot_size / snapshot_checksum 来自刚写出的 `vector_index.snapshot.tmp`。
- created_at_ms 由 manager 写入当前时间。

manifest 不包含 question、answer、scope、TTL、metadata 或 response 信息，也不进入 RDB/AOF。

## Snapshot write failure 策略

最高优先级原则已保持：

```text
snapshot write is best-effort cache write
snapshot write failure must not fail rebuild
```

写失败行为：

- rebuild 返回值保持成功。
- active index 不变，仍可 search。
- internal status 记录失败原因。
- 正常错误路径尽量 rollback `.bak` 保留旧 snapshot。
- 崩溃或不完整文件由下次 startup validation 拒绝。

## Snapshot status 新字段

在 `kvs_vector_snapshot_status_t` 中新增：

```text
snapshot_write_attempted
snapshot_written
last_snapshot_write_error
last_snapshot_write_started_ms
last_snapshot_write_finished_ms
last_snapshot_bytes
last_snapshot_checksum
```

本阶段只保留 internal status 和 test copy helper，不扩展 VINFO。VINFO snapshot status 属于 HNSW-S5。

## 测试覆盖

新增 `tests/vector_index_snapshot_write_after_rebuild_test.*` 覆盖：

- records-only fallback rebuild 成功后写出 manifest + snapshot。
- 写出的 manifest + snapshot 可被 S3 load path 加载。
- 删除 manifest 后 fallback rebuild，并重新写出 snapshot。
- corrupted old snapshot 后 fallback rebuild，并写出新的 valid snapshot。
- VINDEX REBUILD 成功后写出新 snapshot。
- non-core_hnsw_v2 backend 不写 snapshot。
- snapshot write failure 不导致 rebuild failure。
- snapshot write failure 不破坏 active search。
- snapshot write failure 后旧 valid snapshot 仍可加载。
- snapshot load 成功路径不重复 rewrite snapshot。
- leftover tmp 文件被 startup validation / load path 安全忽略。

同步更新 `tests/vector_index_snapshot_load_after_records_test.c`：因为 S4 后 `kvs_vector_index_rebuild()` 会自动写 snapshot，旧 missing-manifest fallback case 需要显式删除自动生成的 manifest。

## 为什么本阶段不改 RDB/AOF

snapshot 是可丢弃 cache。records 仍是 source of truth，RDB/AOF 仍只保存 records。snapshot 缺失、过期、损坏或写失败只影响下次 cold-start 速度，不影响 records load / replay。

## 为什么本阶段不改 VSEARCH response

S4 只改变 rebuild 成功后的内部 cache 写入。VSEARCH 仍沿用：

```text
core_hnsw_v2 search -> manager resolve/filter -> response assembly
```

用户响应格式不变，也不暴露 snapshot、slot、record_id 或 graph 状态。

## 为什么 snapshot 仍不是 source of truth

snapshot 只保存 derived `record_id + vector + graph` index-local cache。启动时必须先恢复 records，再用 manager records/config 校验 manifest 和 snapshot。任何 mismatch 都 fallback records-only rebuild。

## 为什么写失败不影响 rebuild

写 snapshot 在 rebuild 成功后执行，并且返回值只用于 status / log。hook 不覆盖 rebuild 成功返回值，测试通过 blocking tmp directory 模拟写失败，验证 rebuild 仍成功且 search 正常。

## 与 S1 / S2 / S3 的关系

- S1 提供 manifest schema、read/write/validate/checksum helper。
- S2 提供 `core_hnsw_v2` graph cache serialize / deserialize / validate。
- S3 在 records load 后 validate/load snapshot，失败 fallback rebuild。
- S4 在 fallback rebuild 或 explicit VINDEX REBUILD 成功后 best-effort 生成下一次可加载的 manifest + snapshot。

## 测试结果

已运行：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
tests/vector_index_snapshot_manifest_test.sh PASS
tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh PASS
tests/vector_index_snapshot_load_after_records_test.sh PASS
tests/vector_index_snapshot_write_after_rebuild_test.sh PASS
tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
tests/vector_core_flat_rdb_reload_test.sh PASS
```

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

当前 worktree 包含大量前序 CODE-HYGIENE / HNSW 产品化 / HNSW-S1-S3 未提交改动。S4 相关新增/修改已列在上方。

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
?? tests/vector_index_snapshot_write_after_rebuild_test.c
?? tests/vector_index_snapshot_write_after_rebuild_test.sh
```

## HNSW-S4 decision

```text
production_code_changed = yes
snapshot_writer_added = yes
rebuild_success_hook_added = yes
startup_load_changed = no
rdb_aof_changed = no
vsearch_response_changed = no
index_ops_changed = no
snapshot_is_source_of_truth = no
snapshot_write_failure_blocks_rebuild = no
ready_for_HNSW_S5 = yes
next_stage = HNSW-S5
```
