# Stage 9G3: scale/stability gate and final consolidation

## AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。本阶段与 `AGENTS.md` 无冲突。

本阶段只做 scale/stability gate、tombstone policy、default backend final regression 和最终文档收口。未做 SIMD / reverse_prune / layout / cache 微优化，未删除 legacy current `core_hnsw`，未改 VSEARCH response，未改 RDB/AOF 格式，未改 `kvs_vector_index_ops_t` 签名，未引入 background thread，未让 v2 core 访问 manager-owned state。

## Modified / added files

新增：

- `tests/vector_core_hnsw_v2_scale_stability_test.c`
- `tests/vector_core_hnsw_v2_scale_stability_test.sh`
- `tests/vector_core_hnsw_v2_tombstone_policy_test.c`
- `tests/vector_core_hnsw_v2_tombstone_policy_test.sh`
- `docs/vector_core_reset/stage9g3_scale_stability_final_consolidation.md`
- `docs/vector_core_reset/core_hnsw_v2_final_architecture.md`
- `docs/vector_core_reset/core_hnsw_v2_interview_review.md`
- `docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md`

修改：

- `Makefile`

工作树仍保留 Stage 9G0/9G1/9G2 未提交改动；本阶段未回退或整理这些既有改动。

## Production code changed

未修改生产代码。`Makefile` 仅新增两个测试二进制接线。

## Current call chain

默认路径保持 Stage 9G2 结果：

```text
unset vector_index_type
  -> kvs_config_init_default()
  -> vector_index_type = core_hnsw_v2
  -> kvs_vector_index_type_from_config()
  -> KVS_VECTOR_INDEX_CORE_HNSW_V2
  -> g_kvs_vector_core_hnsw_v2_ops
  -> core_hnsw_v2 record_id/vector-only API
  -> manager resolve/filter/response assembly
```

Explicit fallback paths remain:

```text
vector_index_type=flat      -> core_flat
vector_index_type=core_flat -> core_flat
vector_index_type=core_hnsw -> legacy current core_hnsw
```

## Before behavior

Stage 9G2 已完成 default backend switch hardening，但缺少最终 10K/50K/100K scale gate、tombstone ratio policy 测试、cold start/bulk import 风险收口，以及最终架构/面试/默认后端文档。

## After design

新增两个 index-only 测试：

- `vector_core_hnsw_v2_scale_stability_test`
  - 10K full gate: 50 queries, ef=128/256/512
  - 50K sampled gate: 20 queries, ef=128/256/512
  - 100K smoke gate: 5 queries, ef=128/256/512
  - 输出 build/search/recall/memory/degree/duplicate/overflow/ef sensitivity
- `vector_core_hnsw_v2_tombstone_policy_test`
  - 验证 30% recommend rebuild policy
  - 验证 50% strong recommend policy
  - 验证 reset/re-add rebuild simulation 后 deleted_count 清零
  - 验证 active records still searchable，deleted records not returned

新增最终文档：

- `core_hnsw_v2_final_architecture.md`
- `core_hnsw_v2_interview_review.md`
- `core_hnsw_v2_default_backend_summary.md`

## Scale/stability profile

Scale gate uses dim=512, topK=10, ef_search=128/256/512, default ef_construction=256, AVX2 distance backend.

| count | mode | queries | build_us | memory_bytes | deleted | active | level0_degree_avg | duplicate | overflow |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 10K | full | 50 | 61,308,701 | 64,104,384 | 0 | 10,000 | 64.00 | 0 | 0 |
| 50K | sampled | 20 | 464,624,378 | 319,997,248 | 0 | 50,000 | 64.00 | 0 | 0 |
| 100K | smoke | 5 | 1,034,482,127 | 639,994,400 | 0 | 100,000 | 64.00 | 0 | 0 |

Recall/search:

| count | ef_search | recall@10 | search_avg_us | search_p50_us | search_p95_us | visited_avg | distance_avg |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 10K | 128 | 0.8840 | 1,453.92 | 1,448 | 1,522 | 5,519.50 | 5,602.82 |
| 10K | 256 | 0.9860 | 2,049.00 | 2,040 | 2,138 | 7,896.16 | 7,979.48 |
| 10K | 512 | 1.0000 | 2,703.78 | 2,686 | 2,849 | 9,516.82 | 9,600.14 |
| 50K | 128 | 0.4600 | 2,381.60 | 2,384 | 2,706 | 7,896.40 | 8,025.80 |
| 50K | 256 | 0.6650 | 3,943.10 | 3,974 | 4,100 | 13,773.40 | 13,902.80 |
| 50K | 512 | 0.8600 | 6,612.15 | 6,584 | 6,833 | 23,241.25 | 23,370.65 |
| 100K | 128 | 0.3800 | 2,946.40 | 2,968 | 3,051 | 8,416.60 | 8,557.20 |
| 100K | 256 | 0.5400 | 5,120.60 | 5,119 | 5,156 | 15,143.60 | 15,284.20 |
| 100K | 512 | 0.7200 | 8,926.40 | 8,950 | 8,980 | 27,200.20 | 27,340.80 |

ef sensitivity was monotonic for all three counts.

Interpretation:

- 10K default gate passes: ef=256 recall@10 = 0.9860, ef=512 recall@10 = 1.0000.
- 50K/100K sampled/smoke gates are structurally healthy: no duplicate edge, no degree overflow, memory accounting works.
- 50K/100K recall is not production-ready at ef<=512. This is a scale limitation, not a reason to hide risk by continuing Stage 9G optimization.
- 50K/100K synchronous build time is high, confirming cold start and bulk import risk.

## Tombstone ratio policy

Policy:

```text
deleted_count / max(size, 1) >= 0.30
  -> recommend VINDEX REBUILD

deleted_count / max(size, 1) >= 0.50
  -> strong recommend rebuild / warn search degradation risk

after VINDEX REBUILD
  -> deleted_count should reset to 0
```

Test output:

| stage | size | deleted | active | ratio | recommend | strong |
|---|---:|---:|---:|---:|---:|---:|
| recommend | 1000 | 300 | 700 | 0.3000 | 1 | 0 |
| strong | 1000 | 500 | 500 | 0.5000 | 1 | 1 |
| after_rebuild | 500 | 0 | 500 | 0.0000 | 0 | 0 |

This policy is documented/tested only. It does not add a user command and does not automatically trigger rebuild.

## Cold start risk

RDB/AOF still save records, not v2 graph. After cold start, v2 graph must be rebuilt from active records.

Current synchronous rebuild is acceptable for correctness and small gates, but 50K/100K timing shows it should not be presented as the final production cold-start path.

## Bulk import risk

Recommended operational guidance:

1. Import records first.
2. Avoid frequent rebuilds during import.
3. Run one `VINDEX REBUILD` after import.
4. If future product requirements need online rebuild, use the Stage 9F shadow rebuild/replay/swap design as the starting point.

## Sync rebuild risk

Synchronous rebuild blocks completion until graph construction finishes. Stage 9G3 measured about 465s for 50K and about 1034s for 100K smoke on this machine. This must be treated as a known limitation.

## Default backend final regression

Confirmed by Stage 9G2/9G3 tests:

- default unset config selects `core_hnsw_v2`
- `vector_index_type=flat` selects `core_flat`
- `vector_index_type=core_hnsw` selects legacy current `core_hnsw`
- VSEARCH response does not expose internal slot/record_id fields
- RDB/AOF do not save v2 graph
- current core_hnsw regressions still pass
- core_flat baseline regressions still pass

## Final architecture docs

Added:

- `docs/vector_core_reset/core_hnsw_v2_final_architecture.md`

It documents backend roles, manager/vector_core boundary, record-store source of truth, runtime graph semantics, RDB/AOF boundary, VSET/VDEL/VSEARCH/VINDEX call paths, tombstone semantics, rebuild strategy, scale results, known limits, and directions not recommended now.

## Interview review docs

Added:

- `docs/vector_core_reset/core_hnsw_v2_interview_review.md`

It summarizes the project narrative, why old core_hnsw was replaced, why core_flat was required, HNSW insertion/search flow, tombstone deletion, rebuild/shadow replay design, graph persistence rationale, optimization path, latest numbers, likely interview follow-ups, and limitations.

## Default backend docs

Added:

- `docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md`

It records current default backend status, explicit fallback configuration, verification command, required gate list, and usage notes.

## Known limitations

- 50K/100K recall is not yet production-ready at ef<=512.
- 50K/100K synchronous build time is high.
- cold start rebuild is synchronous.
- bulk import should avoid frequent rebuild loops.
- high tombstone ratio requires rebuild policy discipline.
- no background rebuild worker is enabled.
- no 1M production-scale validation exists.

## Do not continue optimizing in this stage

This stage explicitly does not continue:

- reverse_prune cache/layout
- SIMD rewrite
- background thread
- legacy `core_hnsw` deletion
- 1M scale sprint
- VSEARCH response changes
- RDB/AOF graph persistence

Reason: Stage 9G3 is a scale/stability gate and final consolidation stage. The new data shows clear scale risks, but the requested route says not to expand Stage 9G4/9G5 unless a real bug is found.

## Build result

```text
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS

make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 optional-profile-tests -j"$(nproc)"
PASS
```

## Tests

Core flat baseline:

```text
tests/vector_core_flat_correctness_test.sh PASS
tests/vector_core_flat_rebuild_test.sh PASS
tests/vector_core_flat_rdb_reload_test.sh PASS
```

Legacy current core_hnsw:

```text
tests/vector_core_hnsw_correctness_test.sh PASS
tests/vector_core_hnsw_rebuild_test.sh PASS
tests/vector_core_hnsw_rdb_reload_test.sh PASS
tests/vector_core_hnsw_aof_replay_test.sh PASS
```

core_hnsw_v2:

```text
tests/vector_core_hnsw_v2_tombstone_test.sh PASS
tests/vector_core_hnsw_v2_skeleton_test.sh PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh PASS
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_scale_stability_test.sh PASS
tests/vector_core_hnsw_v2_tombstone_policy_test.sh PASS
```

Manager rebuild harness:

```text
tests/vector_rebuild_state_machine_test.sh PASS
tests/vector_rebuild_delta_capture_test.sh PASS
tests/vector_rebuild_shadow_replay_test.sh PASS
tests/vector_rebuild_shadow_swap_test.sh PASS
```

## Boundary grep result

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output
```

## git diff --check

```text
PASS
```

## git status --short

```text
 M Makefile
 M "docs/vector_core_reset/\344\274\230\345\214\226\346\255\245\351\252\244.md"
 M "docs/vector_core_reset/\346\211\247\350\241\214\346\255\245\351\252\244\344\271\246.md"
 M include/kvs_config.h
 M include/kvs_vector.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_config.c
 M src/core/kvs_operation.c
 M src/vector/kvs_vector.c
 M tests/README.md
 D tests/vcmd_profile_bench.c
 D tests/vector_core_hnsw_rebuild_profile_test.sh
?? docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md
?? docs/vector_core_reset/core_hnsw_v2_final_architecture.md
?? docs/vector_core_reset/core_hnsw_v2_interview_review.md
?? docs/vector_core_reset/stage9g0_experimental_core_hnsw_v2_product_integration.md
?? docs/vector_core_reset/stage9g1_cleanup_codebase_consolidation.md
?? docs/vector_core_reset/stage9g2_default_backend_switch_hardening.md
?? docs/vector_core_reset/stage9g3_scale_stability_final_consolidation.md
?? tests/vector_core_hnsw_v2_default_backend_test.c
?? tests/vector_core_hnsw_v2_default_backend_test.sh
?? tests/vector_core_hnsw_v2_product_integration_test.c
?? tests/vector_core_hnsw_v2_product_integration_test.sh
?? tests/vector_core_hnsw_v2_scale_stability_test.c
?? tests/vector_core_hnsw_v2_scale_stability_test.sh
?? tests/vector_core_hnsw_v2_tombstone_policy_test.c
?? tests/vector_core_hnsw_v2_tombstone_policy_test.sh
```

## Next suggested step

If final validation passes, stop feature development for this route. Future work should be limited to bug fixes, documentation revision, resume/interview review, and small controlled performance rechecks.

If future requirements demand 50K/100K production recall or startup guarantees, start a new profiling/audit stage from the Stage 9G3 data. Do not directly jump into layout/SIMD/reverse_prune refactor.
