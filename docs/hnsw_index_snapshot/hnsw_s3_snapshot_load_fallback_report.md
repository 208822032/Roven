# HNSW-S3: snapshot load / fallback rebuild 报告

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

冲突说明：`AGENTS.md` 的历史模板要求阶段报告写入 `docs/vector_core_reset/`，但当前任务明确进入 `docs/hnsw_index_snapshot/` 的 HNSW-S 路线；本报告按任务指定目录输出。其余边界继续遵守：不修改 VSEARCH response、不修改 RDB/AOF 主格式、不修改 `kvs_vector_index_ops_t`、不让 `core_hnsw_v2` 访问 `g_vector_state` / manager `record_id_map` / scope / TTL / metadata / question / answer。

## 是否修改生产代码

是。修改范围只在 manager / snapshot load glue：

- records load 后的 `kvs_vector_rebuild_after_load()` 接入 optional snapshot load。
- 新增 manager-owned expected manifest 构造和 snapshot status。
- 新增默认路径 helper。

未实现 snapshot writer、rebuild success hook、background rebuild、snapshot GC、RDB/AOF graph persistence 或 VINFO response 扩展。

## 新增 / 修改文件

S3 新增：

- `tests/vector_index_snapshot_load_after_records_test.c`
- `tests/vector_index_snapshot_load_after_records_test.sh`
- `docs/hnsw_index_snapshot/hnsw_s3_snapshot_load_fallback_report.md`

S3 修改：

- `Makefile`
- `include/vector/kvs_vector_index_snapshot.h`
- `include/vector/kvs_vector_manager.h`
- `src/vector/kvs_vector_index_snapshot.c`
- `src/vector/kvs_vector.c`
- `docs/hnsw_index_snapshot/README.md`
- `docs/hnsw_index_snapshot/index.md`
- `docs/hnsw_index_snapshot/snapshot_design.md`
- `docs/hnsw_index_snapshot/implementation_plan.md`

## 接入点

当前 call chain：

```text
RDB vector load / replay-like restore
  -> kvs_vector_reset_for_load()
  -> kvs_vector_restore_record_with_embedding()
  -> kvs_vector_rebuild_after_load()
       -> kvs_vector_id_map_rebuild_from_store()
       -> kvs_vector_index_bind_current()
       -> if backend == core_hnsw_v2:
            kvs_vector_index_snapshot_try_load_after_records()
            if loaded: return OK
            else: fallback kvs_vector_index_rebuild_current()
          else:
            kvs_vector_index_rebuild_current()
```

Snapshot load 成功后不重复 full rebuild。测试用 `core_hnsw_v2_index.build_stats.add_count == 0` 验证。

## Expected manifest 构造方式

`kvs_vector_index_snapshot_build_expected_manifest_after_records()` 由 manager 扫描当前 record store 构造 expected manifest：

- backend 固定为 `core_hnsw_v2`
- dim / metric 来自当前 manager 配置和 v2 index
- M / M0 / efConstruction / efSearch 来自当前 v2 index
- active_count / deleted_count 来自 manager records 扫描，并校验与 store counters 一致
- record_id_checksum 覆盖 active record_id
- vector_checksum 覆盖 active record_id、embedding_dim 和 embedding bytes
- snapshot_size / snapshot_checksum 来自 `vector_index.snapshot` 文件

`record_generation` 使用当前 records 的最大 `record->generation`。当前 `g_vector_state.generation` 在 load/replay 后不是唯一可靠判据，因此真正防线是 `record_id_checksum` + `vector_checksum` + active/deleted count。

## 路径策略

第一版默认路径：

```text
vector_index.manifest
vector_index.snapshot
```

路径 helper 位于 manager/snapshot layer：

```c
kvs_vector_index_snapshot_default_manifest_path()
kvs_vector_index_snapshot_default_snapshot_path()
```

anncore 不知道文件名。后续可接 data dir / persistence dir / config path helper。

## 成功路径

```text
read manifest
build expected manifest from current records/config
validate manifest
validate snapshot file size/checksum
deserialize core_hnsw_v2 snapshot
validate graph basic
validate loaded active record_id/vector bytes against manager records
bind current index
mark snapshot status loaded
```

额外的 manager-side record 校验会逐个检查 loaded graph active slot：

- active slot 数量等于 manager active_records
- record_id 能通过 manager record_id_map 回表
- record 仍 ACTIVE 且未 deleted
- embedding_dim 匹配
- snapshot vector bytes 与 record embedding bytes 一致

## 失败 fallback 路径

以下情况都不影响 records load，直接 fallback 到原 records-only rebuild：

- manifest missing
- manifest unreadable / malformed
- manifest backend / dim / metric / HNSW 参数 mismatch
- active/deleted count mismatch
- record_generation / record_id_checksum / vector_checksum mismatch
- snapshot_size / snapshot_checksum mismatch
- snapshot corrupt
- deserialize failed
- loaded graph 与 manager records 不一致

non-v2 backend 不尝试 snapshot load，保持原路径。

## Snapshot status

新增 manager internal status：

```text
snapshot_load_attempted
snapshot_loaded
snapshot_valid
loaded_from_snapshot
fallback_rebuild_count
last_snapshot_error
last_snapshot_manifest_path
last_snapshot_path
```

本阶段只提供 internal copy helper，不改 VINFO response。S5 如需要再做观测面。

## 为什么本阶段不写 snapshot

S3 只接 load / validate / fallback。写 snapshot 需要 rebuild success hook、temp file、fsync、atomic rename、manifest/snapshot 发布顺序和 GC，属于 HNSW-S4。测试中手工调用 S1/S2 helper 生成 fixture，不代表产品路径会写 snapshot。

## 为什么不改 RDB/AOF

records 仍是 source of truth。`vector_index.manifest` 和 `vector_index.snapshot` 是独立 cache 文件，不进入 RDB/AOF 主格式。snapshot 缺失、损坏或过期不会影响 records load。

## 与 S1 / S2 的关系

- S1 提供 manifest schema、read/write/validate/checksum helper。
- S2 提供 `core_hnsw_v2` graph cache serialize / deserialize / validate。
- S3 把 S1/S2 的读取能力接到 KVManager records load 后路径，并定义 fallback。

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
tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
tests/vector_core_flat_rdb_reload_test.sh PASS
```

S3 新增测试覆盖：

- valid manifest + valid snapshot：load 后 search 正确，且不重复 full rebuild
- missing manifest：fallback rebuild 后 search 正确
- manifest param mismatch：fallback rebuild 后 search 正确
- snapshot checksum mismatch：fallback rebuild 后 search 正确
- corrupted snapshot：manifest 可通过但 deserialize 失败，fallback rebuild 后 search 正确
- truncated snapshot deserialize failure：fallback rebuild 后 search 正确
- text snapshot deserialize failure：fallback rebuild 后 search 正确
- non-core_hnsw_v2 backend：不尝试 snapshot load，原路径 search 正确

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

当前 worktree 包含大量前序 CODE-HYGIENE / HNSW 产品化 / HNSW-S1 / S2 未提交改动。S3 相关新增/修改已列在上方。

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
```

## HNSW-S3 decision

```text
production_code_changed = yes
startup_load_changed = yes
snapshot_writer_added = no
rdb_aof_changed = no
vsearch_response_changed = no
index_ops_changed = no
snapshot_is_source_of_truth = no
fallback_rebuild_verified = yes
ready_for_HNSW_S4 = yes
next_stage = HNSW-S4
```
