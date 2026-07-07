# HNSW-S1: minimal manifest 数据结构和读写工具报告

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

本阶段目标是新增 `vector_index.manifest` 的最小 schema、读写、校验和 checksum helper。未修改 VSEARCH response、RDB/AOF 主格式、`kvs_vector_index_ops_t`、默认 backend、默认 HNSW 参数、`core_flat`、`core_hnsw_v2` graph/search 实现，也未接入 startup load、rebuild success hook、background thread 或 graph serialize / deserialize。

冲突说明：`AGENTS.md` 中历史模板仍提到旧阶段和 `docs/vector_core_reset/` 报告目录；本任务明确进入 `docs/hnsw_index_snapshot/` 的 HNSW-S 路线，因此本报告按任务指定目录输出，同时继续遵守 manager/vector_core、RDB/AOF、response 和 index ops 边界。当前仓库中 legacy `core_hnsw` 源码已在前序 CODE-HYGIENE 阶段删除；本阶段没有恢复或修改 legacy 路径。

## 是否修改生产代码

是，但只新增 snapshot-local manifest helper，并接入构建：

- `include/vector/kvs_vector_index_snapshot.h`
- `src/vector/kvs_vector_index_snapshot.c`

该 helper 不接产品启动路径，不读写 graph snapshot，不访问 manager semantic state，不改变用户命令行为。

## 新增/修改文件

新增：

- `include/vector/kvs_vector_index_snapshot.h`
- `src/vector/kvs_vector_index_snapshot.c`
- `tests/vector_index_snapshot_manifest_test.c`
- `tests/vector_index_snapshot_manifest_test.sh`
- `docs/hnsw_index_snapshot/hnsw_s1_minimal_manifest_report.md`

修改：

- `Makefile`
- `docs/hnsw_index_snapshot/implementation_plan.md`
- `docs/hnsw_index_snapshot/index.md`

## Manifest schema

新增结构：

```c
typedef struct kvs_vector_index_snapshot_manifest_s {
    char magic[32];
    uint32_t manifest_version;
    uint32_t snapshot_format_version;
    uint32_t backend;
    uint32_t dim;
    uint32_t metric;
    uint32_t hnsw_m;
    uint32_t hnsw_m0;
    uint32_t hnsw_ef_construction;
    uint32_t hnsw_ef_search;
    uint64_t active_count;
    uint64_t deleted_count;
    uint64_t record_generation;
    uint64_t record_id_checksum;
    uint64_t vector_checksum;
    uint64_t snapshot_size;
    uint64_t snapshot_checksum;
    uint64_t created_at_ms;
} kvs_vector_index_snapshot_manifest_t;
```

常量：

```text
magic = KVLITE_HNSW_SNAPSHOT
manifest_version = 1
snapshot_format_version = 1
```

## 文件格式选择

第一版 manifest 使用稳定文本格式：

```text
key=value
```

原因：

- manifest 是小文件，读写性能不是瓶颈。
- 易调试、易 diff、易人工审阅。
- 第一版字段有限，文本 parser 足够简单。
- 真正的大文件是后续 `vector_index.snapshot` graph cache，不在 S1 实现。

## Checksum 选择

新增：

```c
uint64_t kvs_vector_index_snapshot_checksum_file(const char *path);
```

算法使用 FNV-1a 64-bit。它 deterministic、无外部依赖、适合小文件测试和 cache corruption detection。它不是 cryptographic checksum，不用于安全防篡改语义。

## Validate mismatch 列表

`kvs_vector_index_snapshot_manifest_validate()` 覆盖：

- `magic mismatch`
- `manifest_version mismatch`
- `snapshot_format_version mismatch`
- `backend mismatch`
- `dim mismatch`
- `metric mismatch`
- `HNSW parameter mismatch`
- `active_count mismatch`
- `deleted_count mismatch`
- `record_generation mismatch`
- `record_id_checksum mismatch`
- `vector_checksum mismatch`
- `snapshot_size mismatch`
- `snapshot_checksum mismatch`

mismatch 只表示 snapshot cache 不可用，应 fallback rebuild；不代表 records 损坏。

## 本阶段未接 anncore serialize / deserialize

S1 只建立 manifest 元信息能力。`core_hnsw_v2` graph serialize / deserialize 属于 HNSW-S2。这样可以先稳定参数、版本、count、generation、size 和 checksum 的校验边界，避免在同一阶段混入 graph binary format、slot/level/neighbor 存储和 search roundtrip 语义。

## 本阶段未修改 RDB/AOF

records 仍是 source of truth。manifest 是独立 snapshot cache metadata，不进入 RDB/AOF 主格式。缺失、过期或损坏的 manifest 只会让 snapshot 不可用，未来应回到 records-only rebuild，不影响 records load / replay。

## Snapshot 仍不是 source of truth

`vector_index.manifest` 和 future `vector_index.snapshot` 只描述可丢弃 cache。真实数据仍在 record store / RDB / AOF 中。manifest validate 不访问 anncore graph，不访问 RDB/AOF，不处理 question、answer、scope、TTL、metadata 或 response assembly。

## 测试结果

已运行：

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_index_snapshot_manifest_test.sh
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
```

结果：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
tests/vector_index_snapshot_manifest_test.sh PASS
tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
```

覆盖：

- init default manifest
- write + read roundtrip
- magic mismatch
- manifest version mismatch
- snapshot format version mismatch
- backend mismatch
- dim mismatch
- metric mismatch
- HNSW parameter mismatch
- active_count mismatch
- deleted_count mismatch
- record_generation mismatch
- snapshot_size mismatch
- snapshot_checksum mismatch
- record_id_checksum / vector_checksum mismatch
- checksum helper deterministic
- missing manifest file failure
- malformed / incomplete manifest failure

## Boundary grep

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
```

结果：

```text
no output
```

## git diff --check

```text
PASS
```

## git status --short

当前 worktree 包含大量前序 CODE-HYGIENE / HNSW 产品化未提交改动。本阶段新增/修改项包括：

```text
 M Makefile
 M docs/hnsw_index_snapshot/implementation_plan.md
 M docs/hnsw_index_snapshot/index.md
?? docs/hnsw_index_snapshot/hnsw_s1_minimal_manifest_report.md
?? include/vector/kvs_vector_index_snapshot.h
?? src/vector/kvs_vector_index_snapshot.c
?? tests/vector_index_snapshot_manifest_test.c
?? tests/vector_index_snapshot_manifest_test.sh
```

完整 `git status --short`：

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
?? include/vector/kvs_vector_index_snapshot.h
?? src/vector/kvs_vector_backend_resolve.c
?? src/vector/kvs_vector_index_snapshot.c
?? tests/vector_index_snapshot_manifest_test.c
?? tests/vector_index_snapshot_manifest_test.sh
```

## HNSW-S1 decision

```text
production_code_changed = yes
manifest_schema_added = yes
manifest_read_write_added = yes
manifest_validate_added = yes
anncore_serialize_added = no
rdb_aof_changed = no
snapshot_is_source_of_truth = no
ready_for_HNSW_S2 = yes
next_stage = HNSW-S2
```
