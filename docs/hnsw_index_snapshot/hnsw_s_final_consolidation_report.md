# HNSW-S-final: index snapshot 路线收口与风险审计

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

冲突说明：`AGENTS.md` 的历史模板要求阶段报告写入 `docs/vector_core_reset/`；当前任务明确属于 `docs/hnsw_index_snapshot/` 的 HNSW-S 路线，并要求新增本报告。因此本报告按任务指定目录输出，同时继续遵守 AGENTS 的核心边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不让 `core_hnsw_v2` 访问 `g_vector_state` / manager `record_id_map` / scope / TTL / metadata / question / answer。

## 是否修改生产代码

否。

HNSW-S-final 只做文档收口和回归验证。本阶段未修改：

- `src/`
- `include/`
- `tests/`
- Makefile
- RDB/AOF 主格式
- `VSEARCH` response
- `kvs_vector_index_ops_t`
- 默认 backend
- 默认 HNSW 参数

当前 worktree 仍包含 HNSW-S1 到 S6 以及前序 hygiene / productization 的生产代码改动，本报告不把这些前序改动回退或重新归属为 S-final 改动。

## 新增 / 修改文件

S-final 新增：

- `docs/hnsw_index_snapshot/hnsw_s_final_consolidation_report.md`

S-final 修改：

- `docs/hnsw_index_snapshot/README.md`
- `docs/hnsw_index_snapshot/index.md`
- `docs/hnsw_index_snapshot/implementation_plan.md`
- `docs/hnsw_index_snapshot/snapshot_design.md`

## HNSW-S1 到 S6 总结

```text
HNSW-S1:
  minimal manifest schema / read / write / validate / checksum

HNSW-S2:
  core_hnsw_v2 graph cache serialize / deserialize roundtrip

HNSW-S3:
  startup records load 后尝试 load snapshot，失败 fallback records-only rebuild

HNSW-S4:
  records-only rebuild / VINDEX REBUILD 成功后 best-effort 写 snapshot

HNSW-S5:
  VINFO 暴露 snapshot load / fallback / write status

HNSW-S6:
  shadow publish skeleton 成功后 best-effort 写 snapshot
```

当前最小闭环：

```text
records load / replay
  -> manager rebuild id map
  -> validate vector_index.manifest + vector_index.snapshot
  -> valid: load core_hnsw_v2 graph cache
  -> invalid / missing / corrupt: records-only rebuild
  -> rebuild / VINDEX REBUILD / shadow publish skeleton success: best-effort write next snapshot
  -> VINFO exposes load / fallback / write status
```

## 当前能力矩阵

```text
manifest read/write/validate:
  yes

anncore snapshot serialize/deserialize:
  yes

startup load snapshot:
  yes

invalid snapshot fallback rebuild:
  yes

rebuild success write snapshot:
  yes

VINDEX REBUILD write snapshot:
  yes

VINFO snapshot status:
  yes

shadow publish skeleton write snapshot:
  yes

snapshot GC:
  no

mmap serving:
  no

background worker:
  no

graph persistence in RDB/AOF:
  no
```

## 当前不变量审计

```text
records are source of truth:
  kept

snapshot is cache:
  kept

snapshot may be deleted:
  kept

snapshot may be invalid:
  kept

snapshot load failure does not fail records load:
  kept

snapshot write failure does not fail rebuild:
  kept

snapshot does not enter RDB/AOF:
  kept

VSEARCH response unchanged:
  kept

kvs_vector_index_ops_t unchanged:
  kept

core_hnsw_v2 does not access manager semantic state:
  kept by boundary grep
```

## 当前 snapshot 能解决的问题

- valid snapshot 存在且 manifest / checksum / records 对照全部通过时，可以在 records 恢复后加载 `core_hnsw_v2` graph cache。
- missing / mismatch / corrupt snapshot 不影响 records load，会回到 records-only rebuild。
- rebuild、VINDEX REBUILD 和 shadow publish skeleton 成功后，会 best-effort 生成下一次 cold-start 可用的 snapshot。
- `VINFO` 可以观测最近一次 snapshot load / fallback / write 状态。

## 当前 snapshot 不能解决的问题

- 不能降低 anncore build CPU 成本。
- 不能替代真正 background rebuild worker。
- 不能提供 reader safety、active / shadow / retired 产品级发布。
- 不能提供 snapshot GC / 多版本管理。
- 不能提供 mmap / view serving。
- 不能保证完整 crash-consistency / fsync 语义。
- 不能证明真实 embedding 下 recall / QPS / latency SLA。
- 不能证明 100K snapshot load time 或 cold-start SLA。

## 当前风险清单

1. snapshot 文件路径仍是第一版默认策略。
2. 第一版 atomic rename 没有完整 crash-consistency / fsync 保证。
3. shadow publish hook 仍基于 9F skeleton / test internal，不等于完整 background rebuild。
4. 没有 snapshot GC / 多版本管理。
5. 没有 mmap / view。
6. 没有真实 embedding 下 cold-start benchmark。
7. 没有 100K snapshot load time benchmark。
8. 50 分钟 build CPU 成本仍然存在，snapshot 只是在 valid cache 存在时绕过 rebuild。
9. worktree 存在大量前序未提交改动，需要提交前人工确认。

## 不继续做 GC / mmap / admin command 的理由

- snapshot GC / 多版本管理是文件生命周期问题，会引入保留策略、并发发布、清理时机和 crash recovery 语义，不属于当前最小闭环收口。
- mmap / view 会改变 snapshot IO 和 memory model，需要独立性能、兼容性和生命周期审计。
- admin command 会扩大用户可见 API surface，需要命令权限、响应格式和运维语义设计。
- 当前任务目标是路线收口和风险审计，继续扩 S 线会混入新的产品功能风险。

## HNSW-S 与后续路线边界

```text
HNSW-S:
  index snapshot / cold-start cache
  snapshot 是 derived cache，不是 source of truth

HNSW-R:
  真正 background shadow rebuild
  reader safety
  active / shadow / retired 产品化
  delta replay / safe swap

HNSW-buildperf:
  bulk-build API
  parallel build
  reverse prune / candidate selection 优化
  allocator / memory layout

real/proxy embedding benchmark:
  recall@10
  QPS
  p50 / p95 / p99 latency
  snapshot load time
  cold-start no-snapshot vs with-snapshot 对比

future HNSW-S:
  snapshot GC
  multi-version snapshot
  mmap/view
  stronger fsync/crash consistency
```

下一步不建议继续扩当前 HNSW-S 功能。应先提交当前已验证的最小闭环，再按问题域进入 HNSW-R、HNSW-buildperf 或真实 benchmark。

## 是否建议提交当前阶段

建议提交当前 HNSW-S 收口阶段，但提交前需要人工确认 worktree 中大量前序未提交改动均属于预期范围。

建议 commit message：

```text
docs: close hnsw snapshot route with final risk audit
```

如果把 S1 到 S6 的代码和测试一并提交，建议使用更宽的 commit message：

```text
vector: add hnsw v2 snapshot cache lifecycle
```

## 测试结果

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
```

Required tests：

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

## HNSW-S-final decision

```text
HNSW-S-final decision:
  production_code_changed = no
  snapshot_minimal_loop_complete = yes
  rdb_aof_changed = no
  vsearch_response_changed = no
  index_ops_changed = no
  snapshot_is_source_of_truth = no
  background_worker_added = no
  ready_to_commit = yes
  recommended_next = commit
```

提交前注意：`ready_to_commit = yes` 表示当前 S-final 验证通过、边界未破坏；由于 worktree 含大量前序未提交改动，实际提交前仍应人工确认 diff 范围。
