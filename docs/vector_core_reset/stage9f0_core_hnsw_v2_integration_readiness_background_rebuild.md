# Stage 9F0: core_hnsw_v2 integration readiness and background rebuild design

## 1. AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。

本阶段只做 integration readiness / background rebuild design。未注册 `core_hnsw_v2` backend，未接入 VINDEX / VSEARCH，未改变 runtime behavior，未修改 current `core_hnsw`，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未继续 reverse_prune / SIMD / layout / cache 优化。

本 prompt 与 `AGENTS.md` 无硬边界冲突。`AGENTS.md` 早期的 Stage 5A/5B 推荐路线是历史推荐；后续 v2 stage discipline 要求遵循用户提供的最新 roadmap，因此本阶段按 Stage 9F0 执行。

## 2. Modified / added files

新增：

- `docs/vector_core_reset/stage9f0_core_hnsw_v2_integration_readiness_background_rebuild.md`

生产代码变更：无。

## 3. Audited files

已审计：

- `include/kvs_vector.h`
- `include/vector/kvs_vector_core.h`
- `include/vector/kvs_vector_hnsw.h`
- `include/vector/kvs_vector_hnsw_v2.h`
- `include/vector/kvs_vector_manager.h`
- `include/vector/kvs_vector_index_internal.h`
- `include/vector/kvs_vector_record.h`
- `src/vector/kvs_vector.c`
- `src/core/kvs_operation.c`
- `src/vector/core/hnsw/`
- `src/vector/core/hnsw_v2/`
- `tests/vector_core_hnsw_rebuild_test.sh`
- `tests/vector_core_hnsw_rdb_reload_test.sh`
- `tests/vector_core_hnsw_aof_replay_test.sh`
- `tests/vector_core_hnsw_v2_*.sh`

## 4. Scope result

只做设计、代码路径审计、接口边界梳理和报告。

没有做：

- 产品 backend 注册
- VINDEX / VSEARCH runtime 行为修改
- current `core_hnsw` 修改
- RDB/AOF 修改
- `kvs_vector_index_ops_t` 修改
- VSEARCH response 修改
- reverse_prune cache 扩大
- SIMD / layout 优化

## 5. Current call chain

当前产品路径：

```text
VSET
  -> kvs_vector_set_commit_prepared()
     -> record store insert/update
     -> manager id_map / record_id_map update
     -> flat baseline add
     -> kvs_vector_index_add_current()
        -> current_index.ops->add()

VSEARCH
  -> handle_vsearch_command()
     -> kvs_vector_search_with_prepared()
        -> kvs_vector_index_search_current()
           -> current_index.ops->search()
           -> kvs_vector_manager_resolve_hits()
           -> response assembly in command layer

VINDEX REBUILD
  -> handle_vindex_command()
     -> kvs_vector_index_rebuild()
        -> compact record store
        -> rebuild manager id_map
        -> rebuild current product index synchronously
```

`core_hnsw_v2` 当前仍是 index-only 测试路径：

```text
kvs_hnsw_v2_index_init()
  -> kvs_hnsw_v2_index_reserve()
  -> kvs_hnsw_v2_index_add(record_id, vector)
  -> kvs_hnsw_v2_index_search(query) -> record_id + score
  -> kvs_hnsw_v2_index_info()
  -> kvs_hnsw_v2_index_destroy()
```

## 6. v2 backend readiness matrix

| Capability | Current status | Integration note |
|---|---|---|
| init | Ready: `kvs_hnsw_v2_index_init()` | v2 uses dim + metric only. |
| reserve | Ready: `kvs_hnsw_v2_index_reserve()` | Capacity-based allocation is index-local. |
| add | Ready: `kvs_hnsw_v2_index_add(record_id, vector, dim)` | Uses stable `record_id`, not manager `record_index`. |
| search | Ready: `kvs_hnsw_v2_index_search()` | Returns `kvs_vector_hit_t` only. Manager must resolve/filter. |
| reset | Ready: `kvs_hnsw_v2_index_reset()` | Keeps capacity, clears graph/state. |
| destroy | Ready: `kvs_hnsw_v2_index_destroy()` | Index-local lifecycle only. |
| info / memory | Ready index-only | Not yet exposed through product VINFO. |
| build/search stats | Ready index-only | Profile tests print detailed stats. |
| deleted_flags | Partial | Search skips `deleted_flags`; tests can set it directly. |
| tombstone API | Missing | No `kvs_hnsw_v2_index_mark_delete()` yet. Required before product registration. |
| rebuild from records | Feasible | Manager can scan active records and call `add(record_id, embedding)`. |
| graph persistence | Not needed | Graph remains derived runtime state. RDB/AOF should still save records only. |
| manager state access | Clean | Boundary grep for `g_vector_state`, `record_id_map`, scope/TTL/metadata/question/answer produced no output. |

## 7. current core_hnsw vs core_hnsw_v2

| Area | current core_hnsw | core_hnsw_v2 |
|---|---|---|
| Product registration | Registered as `KVS_VECTOR_INDEX_CORE_HNSW` | Not registered. |
| Manager wrapper | Has `kvs_vector_index_ops_t` wrapper in `src/vector/kvs_vector.c` | No product wrapper yet. |
| Core API shape | Mostly record_id + vector internally, but product wrapper handles manager resolve | Pure index-only record_id + vector -> record_id + score. |
| Rebuild | Synchronous product rebuild through `VINDEX REBUILD` | Index-only tests build directly; no manager rebuild path. |
| Delete/tombstone | `kvs_hnsw_index_mark_delete()` exists | No public mark_delete function yet. |
| VINFO | Product VINFO exposes extensive current core_hnsw stats | v2 stats exist but are not VINFO fields. |
| Recall | Legacy fallback/reference; earlier low-recall root cause led to v2 route | 10K/512d ef=256 recall@10 0.9860; ef=512 1.0000 in latest tests. |
| Build | Product rebuild path exists | 10K/512d build still about 56s index-only. |
| Persistence | Derived graph; RDB/AOF store records | Same intended boundary; no graph persistence needed. |

## 8. Product integration file map

Future registration, not done in Stage 9F0:

| File | Required future work |
|---|---|
| `include/kvs_vector.h` | Add `KVS_VECTOR_INDEX_CORE_HNSW_V2` enum value and VINFO fields only after deciding product exposure. |
| `include/vector/kvs_vector_manager.h` | Add manager-owned active/shadow/retired v2 index state only in the rebuild state-machine stage. |
| `include/vector/kvs_vector_hnsw_v2.h` | Add v2 mark_delete API and any stable product info fields. |
| `src/vector/core/hnsw_v2/` | Implement missing tombstone API and keep core free of manager semantics. |
| `src/vector/kvs_vector.c` | Add disabled/flagged wrapper, bind logic, add/search/rebuild/info/destroy paths, manager resolve integration. |
| `src/core/kvs_config.c` | Accept `core_hnsw_v2` config only when backend registration is intentionally enabled. |
| `src/core/kvs_operation.c` | No VSEARCH response change; optionally expose rebuild status in VINFO in a later observability stage. |
| `Makefile` | v2 sources are already compiled; future tests may need new manager-level harness targets. |
| `tests` | Add product integration tests for VSET/VSEARCH/VDEL/VINDEX, RDB reload, AOF replay, shadow rebuild. |
| `docs` | Add Stage 9F1+ design and product integration reports. |

## 9. Integration gates before product registration

Required gates:

- Recall: keep 1K/512d ef=256 >= 0.99, 10K/512d ef=256 >= 0.98, 10K/512d ef=512 >= 0.99.
- Search latency: keep bounded heap search profile stable; no regression from Stage 9E2/9E3c.
- Build strategy: do not put 56s 10K/512d build on synchronous hot path as the only production path.
- Delete/tombstone: implement and test `mark_delete(record_id)` before VDEL integration.
- VINFO: expose only manager-owned/product-safe v2 status; keep response format rules explicit.
- Fallback: current `core_hnsw` remains fallback until v2 product hardening passes.
- RDB/AOF: replay and reload must rebuild from active records; graph remains non-persistent.
- Boundary: v2 must keep grep-clean for `g_vector_state`, `record_id_map`, scope / TTL / metadata / question / answer.

## 10. Background rebuild objects

Minimum manager-owned design:

```text
active_index:
  current query-serving backend wrapper/index

shadow_index:
  newly built backend, not visible to VSEARCH

retired_index:
  old active index after swap; destroyed after no reader can hold it

record store:
  source of truth for vectors and manager semantics

rebuild_generation:
  manager generation captured when build starts

rebuild_delta_log:
  ordered manager-owned vector deltas after rebuild_generation

rebuild state:
  IDLE / BUILDING / REPLAYING_DELTA / READY_TO_SWAP / SWAPPING / FAILED / CANCELLED

progress / error / cancel:
  manager-owned observability and control fields
```

No vector_core object should know scope, TTL, metadata, question, answer, VSEARCH response format, AOF/RDB format, or `g_vector_state`.

## 11. Background rebuild state machine

```text
REBUILD_IDLE
  -> REBUILD_BUILDING
     capture rebuild_generation
     allocate shadow_index
     scan active records and add record_id + embedding

REBUILD_BUILDING
  -> REBUILD_REPLAYING_DELTA
     initial scan complete
     replay ordered deltas newer than rebuild_generation

REBUILD_REPLAYING_DELTA
  -> REBUILD_READY_TO_SWAP
     delta log drained to a bounded tail

REBUILD_READY_TO_SWAP
  -> REBUILD_SWAPPING
     acquire short manager write lock
     pause new vector writes briefly
     replay final delta tail
     validate generation / active count / index health

REBUILD_SWAPPING
  -> REBUILD_IDLE
     publish shadow_index as active_index
     move old active_index to retired_index
     release lock
     delayed destroy retired_index

any state
  -> REBUILD_FAILED
     keep active_index unchanged
     destroy shadow_index
     preserve error/status

REBUILD_FAILED
  -> REBUILD_IDLE
     explicit cleanup / retry

any state
  -> REBUILD_CANCELLED
     cancel requested
     keep active_index unchanged
     destroy shadow_index
```

## 12. Query path

- VSEARCH reads only `active_index`.
- `shadow_index` never serves queries while building.
- Before swap, old `active_index` continues serving all queries.
- Swap publishes new `active_index` in a short critical section.
- After swap, old `active_index` becomes `retired_index`.
- `retired_index` is destroyed only after no in-flight reader can still reference it.

Minimum reader safety choices:

- Start with a coarse manager RW lock if current command executor already serializes write/read adequately.
- If concurrency increases, add active-index refcount or epoch-based delayed destroy.

## 13. Write consistency design

Compared options:

| Option | Pros | Cons | Decision |
|---|---|---|---|
| Pause writes during rebuild | Simple correctness | 56s 10K build makes write unavailability unacceptable | Not recommended. |
| Generation + delta log + final short lock | Keeps reads/writes mostly live, avoids dual-write complexity | Needs manager-owned delta log and replay ordering | Recommended. |
| Active/shadow dual write | Shadow remains fresh | Makes every write pay two backend updates and complicates failures | Not recommended first. |

Recommended: generation + delta log + final short lock.

Delta entries are manager-owned and contain only vector-core payload plus operation type:

```text
delta_generation
op = UPSERT | DELETE
record_id
embedding pointer/copy for UPSERT
embedding_dim
```

VSET:

- Commit record store and manager maps first under existing write semantics.
- Assign/increment manager generation.
- Append `UPSERT(record_id, embedding, dim, generation)` if rebuild state is BUILDING or REPLAYING_DELTA.
- Shadow replay calls `v2_add(record_id, embedding, dim)`.

VDEL:

- Mark record deleted and remove manager maps under existing write semantics.
- Append `DELETE(record_id, generation)` if rebuild is active.
- Shadow replay calls backend mark_delete once v2 tombstone API exists; until then, v2 cannot be product-integrated.

TTL expiry:

- TTL belongs to manager.
- When TTL expiry makes a vector record inactive/deleted, append the same `DELETE(record_id, generation)` delta.
- If TTL only changes expiration timestamp but vector remains active and visible, no vector-core delta is needed.

Metadata/scope changes:

- Current vector_core must not see metadata/scope.
- If a future scope/metadata update does not change record_id, vector, active/deleted status, no core delta is needed.
- If it changes visibility by delete/reinsert semantics, manager emits DELETE / UPSERT as appropriate.

Replay order:

- Replay deltas in generation order.
- For repeated updates to the same record_id, later generation wins.
- Final short lock replays the remaining tail and verifies no newer delta is pending before swap.

Swap rollback:

- Do not mutate `active_index` until shadow is fully built, replayed, and validated.
- If final replay or validation fails, keep active unchanged, destroy shadow, set REBUILD_FAILED.
- Swap itself should be pointer/handle publication only; old active becomes retired after publication succeeds.

## 14. Rebuild slowness decision

Stage 9E3c / latest validation show:

- 10K/512d build is about 56s.
- 10K/512d ef=256 recall@10 remains 0.9860.
- 10K/512d ef=512 recall@10 remains 1.0000.
- Search latency is stable for index-only use.
- Duplicate edge and degree overflow remain 0.

Conclusion:

- Synchronous v2 rebuild is not acceptable as the only production rebuild path.
- With shadow rebuild, rebuild slowness can be downgraded from query-path P0 blocker to P1/P2 background task for moderate datasets.
- Cold start and replay after process restart still remain issues, because there is no existing active index before rebuild completes.
- Bulk import remains an issue if every write updates active index and background rebuild is triggered too early; bulk import should prefer deferred rebuild.
- 100K/1M scale remains a build-time and memory-risk area. Current 10K/512d memory is about 64 MB for one v2 index, so active + shadow roughly doubles backend memory during rebuild.
- High tombstone ratio needs manager rebuild policy, for example threshold by deleted_count / active_count or backend_deleted_count.

Reverse_prune broader cache / graph edge score sidecar remains technical debt, not next route. Stage 9E3c showed local pair cache is safe but low hit-rate and larger cache variants worsened wall time.

## 15. Stage 9F1 recommendation

Recommended next stage:

```text
Stage 9F1: manager-owned rebuild state machine skeleton
```

Why this route:

- It addresses the real product blocker: rebuild must not block query availability.
- It can be implemented without registering v2 backend and without changing VSEARCH behavior.
- It creates manager-owned state boundaries for active/shadow/retired, generation, delta log, progress, error, and cancellation.
- It prepares a testable foundation for later v2 backend registration behind a disabled feature flag.

Do not choose next:

- v2 backend registration first: premature because shadow rebuild state and v2 mark_delete gate are not ready.
- VINFO rebuild status first: useful, but should follow a manager-owned state skeleton.
- shadow rebuild test harness first: useful after the skeleton exists.

Minimum Stage 9F1 boundary:

- Add manager-owned rebuild state enum and opaque slots for active/shadow/retired handles.
- Add generation and delta-log data structures, initially disabled / not wired to VSEARCH.
- Add unit or harness tests for state transitions and failure cleanup.
- Do not register `core_hnsw_v2`.
- Do not change VSEARCH response or runtime behavior.

## 16. Validation

Build:

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
```

v2 tests:

```text
tests/vector_core_hnsw_v2_skeleton_test.sh PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh PASS
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_performance_profile_test.sh PASS
tests/vector_core_hnsw_v2_distance_profile_test.sh PASS
tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh PASS
```

current core_hnsw regression:

```text
tests/vector_core_hnsw_correctness_test.sh PASS
tests/vector_core_hnsw_rebuild_test.sh PASS
tests/vector_core_hnsw_rdb_reload_test.sh PASS
tests/vector_core_hnsw_aof_replay_test.sh PASS
```

Latest key profile results:

| Test | Result |
|---|---:|
| v2 10K/512d build_us | 56,057,890 |
| v2 10K/512d ef=256 recall@10 | 0.9860 |
| v2 10K/512d ef=256 search_avg_us | 856.22 |
| v2 10K/512d ef=512 recall@10 | 1.0000 |
| v2 10K/512d ef=512 search_avg_us | 1,256.32 |
| v2 duplicate edge | 0 |
| v2 degree overflow | 0 |
| v2 memory_bytes 10K/512d | 64,104,384 |

Distance profile 10K dim sensitivity:

| Dim | ef=256 recall | ef=256 search_avg_us | ef=512 recall | ef=512 search_avg_us | build_us |
|---:|---:|---:|---:|---:|---:|
| 384 | 0.9720 | 721.32 | 0.9980 | 1,059.64 | 42,749,809 |
| 512 | 0.9860 | 936.94 | 1.0000 | 1,344.80 | 56,046,652 |
| 768 | 0.9420 | 1,703.56 | 0.9940 | 2,246.38 | 89,670,081 |
| 1024 | 0.9500 | 2,582.02 | 1.0000 | 3,253.04 | 122,465,324 |

Backend compare:

```text
10K/512d fallback build_us=448,034,636
10K/512d avx2 build_us=56,046,652
fallback ef=256 search_avg_us=3,817.32
avx2 ef=256 search_avg_us=936.94
fallback recall ef=256=0.9860
avx2 recall ef=256=0.9860
```

## 17. Static boundary checks

All produced no output:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
```

`git diff --check`: PASS.

## 18. git status --short

```text
 M AGENTS.md
 M Makefile
 M README.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_index.c
 M src/vector/core/hnsw/kvs_hnsw_info.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_distance_avx2.c
 M tests/vcmd_profile_bench.c
 M tests/vector_core_hnsw_rebuild_profile_test.sh
?? "docs/vector_core_reset/hnsw_v2\345\256\236\346\226\275\346\226\271\346\241\210.md"
?? docs/vector_core_reset/stage7e_neighbor_select_score_reuse_optimization.md
?? docs/vector_core_reset/stage7f_highdim_layer_search_audit.md
?? docs/vector_core_reset/stage7g_layer_search_distance_alignment_optimization.md
?? docs/vector_core_reset/stage7h_level0_neighbor_visited_layout_audit.md
?? docs/vector_core_reset/stage7i_level0_visited_slot_locality_prototype.md
?? docs/vector_core_reset/stage7j_add_search_slot_state_locality_prototype.md
?? docs/vector_core_reset/stage7k_remove_failed_layout_prototypes.md
?? docs/vector_core_reset/stage7l_post_layout_layer_search_hotspot_audit.md
?? docs/vector_core_reset/stage8a_core_flat_hnsw_faiss_parity_architecture_audit.md
?? docs/vector_core_reset/stage9a1_core_hnsw_bidirectional_link_acceptance_audit.md
?? docs/vector_core_reset/stage9a_core_hnsw_v2_design_boundary.md
?? docs/vector_core_reset/stage9b_core_hnsw_v2_index_only_skeleton.md
?? docs/vector_core_reset/stage9c_core_hnsw_v2_graph_construction.md
?? docs/vector_core_reset/stage9d_core_hnsw_v2_search_recall_gate.md
?? docs/vector_core_reset/stage9e0_core_hnsw_v2_performance_attribution.md
?? docs/vector_core_reset/stage9e1_core_hnsw_v2_add_time_layer_search.md
?? docs/vector_core_reset/stage9e1a_core_hnsw_v2_candidate_quality_attribution.md
?? docs/vector_core_reset/stage9e2_core_hnsw_v2_bounded_heap_optimization.md
?? docs/vector_core_reset/stage9e3_core_hnsw_v2_distance_simd_attribution.md
?? docs/vector_core_reset/stage9e3b_reverse_prune_attribution_rebuild_strategy_gate.md
?? docs/vector_core_reset/stage9e3c_core_hnsw_v2_reverse_prune_score_reuse.md
?? docs/vector_core_reset/stage9f0_core_hnsw_v2_integration_readiness_background_rebuild.md
?? include/vector/kvs_vector_hnsw_v2.h
?? src/vector/core/hnsw_v2/
?? tests/vector_core_hnsw_v2_distance_profile_test.c
?? tests/vector_core_hnsw_v2_distance_profile_test.sh
?? tests/vector_core_hnsw_v2_graph_construction_test.c
?? tests/vector_core_hnsw_v2_graph_construction_test.sh
?? tests/vector_core_hnsw_v2_performance_profile_test.c
?? tests/vector_core_hnsw_v2_performance_profile_test.sh
?? tests/vector_core_hnsw_v2_reverse_prune_profile_test.c
?? tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh
?? tests/vector_core_hnsw_v2_search_recall_test.c
?? tests/vector_core_hnsw_v2_search_recall_test.sh
?? tests/vector_core_hnsw_v2_skeleton_test.c
?? tests/vector_core_hnsw_v2_skeleton_test.sh
```

Only the Stage 9F0 report was added in this stage; the other dirty/untracked files were existing stage work or unrelated workspace state.
