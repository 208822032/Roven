# Stage 9F4: shadow rebuild replay harness

## AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。

本阶段目标是 manager-owned shadow rebuild replay harness。实现仍保持 record store 为 source of truth，shadow `core_hnsw_v2` 只是 runtime derived state。未注册 `core_hnsw_v2` 为产品 backend，未改变 VSEARCH/VINDEX runtime behavior，未修改 VSEARCH response，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 current `core_hnsw` 算法行为，未实现 active/shadow swap，未实现 background thread。

本 prompt 与 `AGENTS.md` 无硬边界冲突。`AGENTS.md` 中早期 Stage 5 推荐路线是历史基线；本阶段按用户提供的 Stage 9F4 路线执行，符合 v2 stage discipline。

## Modified / added files

本阶段修改：

- `Makefile`
- `include/vector/kvs_vector_rebuild.h`
- `src/vector/kvs_vector_rebuild.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`
- `tests/vector_core_hnsw_v2_tombstone_test.c`

本阶段新增：

- `include/vector/kvs_vector_rebuild_shadow.h`
- `src/vector/kvs_vector_rebuild_shadow.c`
- `tests/vector_rebuild_shadow_replay_test.c`
- `tests/vector_rebuild_shadow_replay_test.sh`
- `docs/vector_core_reset/stage9f4_shadow_rebuild_replay_harness.md`

生产代码有变更，但范围只限 manager-owned rebuild shadow harness、rebuild context shadow cleanup hook、以及 v2 index-only tombstone 后 same-record re-add 语义。

## Runtime behavior guard

确认保持不变：

- 未注册 `core_hnsw_v2` backend。
- 未改变 VSEARCH runtime behavior。
- 未改变 VINDEX runtime behavior。
- 未修改 VSEARCH response。
- 未修改 RDB/AOF。
- 未修改 `kvs_vector_index_ops_t`。
- 未修改 current `core_hnsw` 算法代码。
- VSEARCH 不读取 shadow index。
- VINDEX 不消费 shadow index。
- 未实现 active/shadow pointer publication。
- 未实现 background thread。
- vector_core/v2 未访问 `g_vector_state`、manager `record_id_map`、scope/TTL/metadata/question/answer。

## Current call chain

产品路径仍是：

```text
VSET / VDEL / VSEARCH / VINDEX
  -> existing manager/current_index path
  -> current product backend
```

9F4 新增的 test/internal harness 是：

```text
kvs_vector_rebuild_shadow_v2_init_for_test()
  -> allocate manager-owned shadow kvs_hnsw_v2_index_t
  -> kvs_vector_rebuild_begin(..., shadow)
  -> BUILDING

kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test()
  -> scan manager active/non-deleted records
  -> kvs_hnsw_v2_index_add(record_id, embedding, dim)
  -> REPLAYING_DELTA

kvs_vector_rebuild_shadow_v2_replay_deltas_for_test()
  -> replay delta_log in generation order
  -> UPSERT: mark_delete existing record_id if present, then add latest vector
  -> DELETE: kvs_hnsw_v2_index_mark_delete(record_id), missing delete is counted no-op
  -> READY_TO_SWAP
```

## Shadow v2 lifecycle

`kvs_vector_rebuild_context_t` now has:

```c
void (*shadow_destroy_fn)(void *shadow_index);
```

The callback lets the generic rebuild context release an opaque shadow index without exposing v2 internals to the state machine.

Cleanup behavior:

- `kvs_vector_rebuild_context_destroy()` releases shadow before clearing deltas.
- `kvs_vector_rebuild_fail()` releases shadow and preserves FAILED state/error.
- `kvs_vector_rebuild_cancel()` releases shadow and preserves CANCELLED state.
- `kvs_vector_rebuild_reset_idle()` releases shadow and clears delta log.
- `kvs_vector_reset_for_load()` and `kvs_vector_destroy()` continue to destroy the rebuild context, so shadow and delta log are released.

## Baseline build from active records

`src/vector/kvs_vector_rebuild_shadow.c` scans manager active embeddings through `kvs_vector_manager_for_each_active_embedding()`.

Only these fields cross into v2:

- `record_id`
- embedding vector
- embedding dimension

The shadow index does not receive scope, TTL, metadata, question, answer, manager `record_id_map`, or response state.

The harness reserves conservative shadow capacity from current store capacity and delta slack. Real background rebuild still needs a formal capacity policy before product integration.

## Delta replay semantics

Replay order is the delta log append order, validated by strictly increasing `generation`. If generation order is violated, replay fails, rebuild state enters `FAILED`, and shadow index is destroyed.

UPSERT replay:

```text
if record_id exists in shadow:
  mark_delete(record_id)
add(record_id, latest_vector)
```

This makes latest generation win. Old slots remain tombstoned routing nodes and are filtered from results.

DELETE replay:

```text
mark_delete(record_id)
if not found:
  count missing_delete and continue
```

Missing DELETE is non-fatal because final semantics are "record is absent from visible active result". This covers repeated delete and delete for a record absent from the baseline snapshot.

## v2 same-record UPSERT support

9F4 found that existing v2 rejected any same-record `kvs_hnsw_v2_index_add()` because `record_id_to_slot` retained tombstoned ids. To make UPSERT replay possible, this stage added a narrow v2 index-only rule:

- active duplicate record_id add is still rejected;
- if the existing mapped slot is tombstoned, same record_id may be added into a new slot;
- `record_id_to_slot` is updated to the newest slot;
- old slot remains tombstoned and can still be used for graph routing;
- deleted slot is not returned from search results.

`tests/vector_core_hnsw_v2_tombstone_test.c` now covers active duplicate rejection, tombstone re-add success, id map pointing to the new slot, old slot remaining tombstoned, and search returning the latest active record.

## Shadow consistency validation

新增 `tests/vector_rebuild_shadow_replay_test.c` 覆盖：

- shadow harness init/destroy;
- `IDLE -> BUILDING -> REPLAYING_DELTA -> READY_TO_SWAP`;
- baseline build from active records;
- baseline shadow search finds initial active records;
- capture active after build:
  - new record UPSERT is replayed into shadow;
  - existing record UPSERT uses latest vector;
  - repeated UPSERT same record_id latest generation wins;
  - DELETE removes visible record from shadow results;
  - missing DELETE is counted and treated as no-op;
- shadow visible count equals manager active record count after tombstones:
  `shadow.size - shadow.deleted_count == manager.active_records`;
- active-only exact oracle top1 matches shadow top1 for deterministic vectors;
- all-deleted scenario returns empty shadow search result;
- replay generation-order failure enters FAILED, destroys shadow, and leaves active product index unchanged;
- cleanup returns IDLE and clears shadow;
- reset-for-load / destroy cleanup clears shadow and delta state;
- product VSEARCH still reads current product index, not shadow;
- VINDEX still uses existing runtime behavior and does not consume shadow.

The test uses a manager-internal harness to update a record embedding and append UPSERT delta for latest-vector replay. Current command-level repeated `VSET` for an existing question/scope still updates answer only and does not recompute embedding; 9F4 does not change that product behavior.

## READY_TO_SWAP meaning

`READY_TO_SWAP` only means:

- shadow v2 index was built from active records;
- delta log replay completed in generation order;
- deterministic consistency checks passed.

It does not publish the shadow index, does not move `current_index`, and does not retire/destroy the active product index.

## Failure cleanup

On replay failure:

- state becomes `KVS_VECTOR_REBUILD_FAILED`;
- error code/message are preserved;
- shadow index is destroyed immediately through `shadow_destroy_fn`;
- active product index remains unchanged;
- `kvs_vector_rebuild_reset_idle()` clears the failed context and delta log.

## Why no swap / thread / backend registration

No active/shadow swap:

- pointer publication, reader safety, retired-index lifetime and rollback are a separate risk surface.

No background thread:

- concurrent scan/capture/replay needs locking and ordering policy. 9F4 first validates the serial correctness loop.

No v2 backend registration:

- product integration still needs swap lifecycle, VINFO status, disabled feature flag, fallback behavior, and command-level integration tests.

## Real background rebuild ordering note

The serial harness validates the intended ordering, but a real background implementation must enforce:

```text
1. enter BUILDING and enable capture under manager control
2. take a stable scan boundary / generation
3. build shadow from active records at that boundary
4. replay all deltas captured after the boundary in generation order
5. validate shadow
6. only then proceed to a later swap stage
```

Without that ordering, a write between snapshot scan and capture activation could be missed.

## Tests

Build:

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
```

Stage 9F tests:

```text
tests/vector_rebuild_state_machine_test.sh PASS
tests/vector_rebuild_delta_capture_test.sh PASS
tests/vector_rebuild_shadow_replay_test.sh PASS
```

v2 tests:

```text
tests/vector_core_hnsw_v2_tombstone_test.sh PASS
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

v2 recall gate observed during this run:

```text
1K/512d ef=256 recall@10  = 1.0000
5K/512d ef=256 recall@10  = 1.0000
10K/512d ef=64 recall@10  = 0.6800
10K/512d ef=128 recall@10 = 0.8840
10K/512d ef=256 recall@10 = 0.9860
10K/512d ef=512 recall@10 = 1.0000
10K/512d ef=1024 recall@10 = 1.0000
```

`tests/vector_core_hnsw_v2_performance_profile_test.sh` also passed with 10K/512d ef=256:

```text
recall@10 = 0.9860
search_avg_us = 984.42
level0_degree_avg = 64.00
duplicate = 0
overflow = 0
```

## Static boundary checks

```text
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
  no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
  no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
  no output
```

`git diff --check`:

```text
PASS
```

## git status --short

```text
 M AGENTS.md
 M Makefile
 M README.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_index.c
 M src/vector/core/hnsw/kvs_hnsw_info.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_distance_avx2.c
 M tests/vcmd_profile_bench.c
 M tests/vector_core_hnsw_rebuild_profile_test.sh
?? docs/vector_core_reset/hnsw_v2实施方案.md
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
?? docs/vector_core_reset/stage9f1_manager_owned_rebuild_state_machine_skeleton.md
?? docs/vector_core_reset/stage9f2_core_hnsw_v2_mark_delete_tombstone_gate.md
?? docs/vector_core_reset/stage9f3_manager_write_path_delta_capture_skeleton.md
?? docs/vector_core_reset/stage9f4_shadow_rebuild_replay_harness.md
?? include/vector/kvs_vector_hnsw_v2.h
?? include/vector/kvs_vector_rebuild.h
?? include/vector/kvs_vector_rebuild_shadow.h
?? src/vector/core/hnsw_v2/
?? src/vector/kvs_vector_rebuild.c
?? src/vector/kvs_vector_rebuild_shadow.c
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
?? tests/vector_core_hnsw_v2_tombstone_test.c
?? tests/vector_core_hnsw_v2_tombstone_test.sh
?? tests/vector_rebuild_delta_capture_test.c
?? tests/vector_rebuild_delta_capture_test.sh
?? tests/vector_rebuild_shadow_replay_test.c
?? tests/vector_rebuild_shadow_replay_test.sh
?? tests/vector_rebuild_state_machine_test.c
?? tests/vector_rebuild_state_machine_test.sh
```

The status includes existing dirty/untracked files from earlier stages and unrelated `README.md`; they were not reverted or cleaned in 9F4.

## Blockers found

No blocking issue remains for serial shadow replay harness.

The only uncovered product-integration risk is outside 9F4: real active/shadow swap still needs pointer lifecycle, reader safety, retired destroy, rollback and command-level integration tests.

## Stage 9F5 recommendation

Because 9F4 now safely supports same-record UPSERT replay through tombstone re-add semantics, the next stage should be:

```text
Stage 9F5: active/shadow swap skeleton without VSEARCH behavior change
```

Scope should stay narrow:

- validate pointer lifecycle only;
- keep VSEARCH using current product index;
- introduce retired-index cleanup/rollback skeleton;
- do not register v2 backend;
- do not start background thread;
- do not change RDB/AOF or VSEARCH response.

