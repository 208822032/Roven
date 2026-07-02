# Stage 9F1: manager-owned rebuild state machine skeleton

## AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。

本阶段只实现 manager-owned rebuild state machine skeleton、generation skeleton、delta log skeleton、active/shadow/retired placeholder、init/destroy 接线和状态机测试。未注册 `core_hnsw_v2` backend，未改变 VSEARCH/VINDEX runtime behavior，未修改 VSEARCH response，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t` 签名，未修改 current `core_hnsw` 算法行为。

本 prompt 与 `AGENTS.md` 无硬边界冲突。`AGENTS.md` 中早期 Stage 5A/5B 推荐路线属于历史基线；本阶段按用户提供的 Stage 9F1 路线执行，符合 `AGENTS.md` 后续 v2 stage discipline。

## Modified / added files

本阶段新增：

- `include/vector/kvs_vector_rebuild.h`
- `src/vector/kvs_vector_rebuild.c`
- `tests/vector_rebuild_state_machine_test.c`
- `tests/vector_rebuild_state_machine_test.sh`
- `docs/vector_core_reset/stage9f1_manager_owned_rebuild_state_machine_skeleton.md`

本阶段修改：

- `include/vector/kvs_vector_manager.h`
- `src/vector/kvs_vector.c`
- `Makefile`

生产代码有变更，但仅为 manager 侧 rebuild control-plane skeleton 和 init/destroy 级别接线。

## Current call chain

产品路径保持不变：

```text
VSET/VSEARCH/VDEL/VINDEX
  -> existing manager/current_index path
  -> current product backend
```

新增 skeleton 当前只在 manager lifecycle 初始化/销毁：

```text
kvs_vector_init()
  -> kvs_vector_rebuild_context_init(&g_vector_state.rebuild)

kvs_vector_destroy()
  -> kvs_vector_rebuild_context_destroy(&g_vector_state.rebuild)

kvs_vector_reset_for_load()
  -> rebuild context reset to IDLE
```

未让 VSET/VDEL append delta，未让 VINDEX 使用 shadow rebuild，未让 VSEARCH 读取 shadow index。

## Rebuild context

新增 manager-owned `kvs_vector_rebuild_context_t`：

- `state`
- `current_generation`
- `rebuild_generation`
- `delta_count`
- `delta_capacity`
- `delta_log`
- `active_index`
- `shadow_index`
- `retired_index`
- `progress_total`
- `progress_done`
- `error_code`
- `error_message`
- `cancel_requested`

`active_index` / `shadow_index` / `retired_index` 当前是 opaque placeholder，由 manager context 持有。9F1 不分配真实 v2 index，不发布 shadow，不销毁产品 active index。

## State machine

已实现合法转移：

```text
IDLE -> BUILDING
BUILDING -> REPLAYING_DELTA
REPLAYING_DELTA -> READY_TO_SWAP
READY_TO_SWAP -> SWAPPING
SWAPPING -> IDLE
active state -> FAILED
FAILED -> IDLE cleanup
active state -> CANCELLED
CANCELLED -> IDLE cleanup
```

非法转移返回错误且不改变当前状态。测试覆盖 `idle -> swapping`、`building -> swapping`、`failed -> swapping`、`cancelled -> swapping`。

## Delta log

新增 manager-owned delta entry：

```text
generation
op = UPSERT | DELETE
record_id
vector copy pointer for UPSERT
dim
```

UPSERT delta 会复制 vector，并由 `clear` / `destroy` / cleanup 路径释放。DELETE delta 不保存 vector。append 失败不会改变 `delta_count` 或 generation。generation 单调递增；重复 record_id 本阶段不合并，后续 replay 应按 generation 顺序执行，later generation wins。

## Safety invariants

- Rebuild context 属于 manager 层，不属于 vector_core。
- Delta log 只包含 `record_id`、vector、dim、op、generation，不包含 scope/TTL/metadata/question/answer。
- 9F1 不接 VSET/VDEL delta，避免改变写路径语义。
- 9F1 不做 background thread，避免引入并发发布和生命周期风险。
- 9F1 不做 shadow swap，避免改变 current_index 选择逻辑。
- 9F1 不注册 v2 backend，因为 v2 mark_delete API 仍未正式补齐。
- RDB/AOF 仍只保存 records，不保存 graph 或 rebuild context。

## Implementation note

实现过程中发现一个 C 增量构建风险：`kvs_vector_state_t` 新增字段后，所有直接访问 `g_vector_state` 布局的对象必须按同一 header 重编。最终处理方式：

- 将 `rebuild` 放在 `kvs_vector_state_t` 末尾，减少对既有字段偏移的扰动。
- Makefile 为 `kvs_vector.o`、`kvs_vector_embedding.o`、`kvs_vector_index.o`、`kvs_vector_rebuild.o` 增加 `kvs_vector_manager.h` / `kvs_vector_rebuild.h` 依赖。

修正后 current core_hnsw correctness 通过。

## Tests

Build:

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
```

Stage 9F1 test:

```text
tests/vector_rebuild_state_machine_test.sh PASS
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

## Boundary checks

All produced no output:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
```

`git diff --check`: PASS.

## Runtime behavior

Confirmed unchanged by scope:

- `core_hnsw_v2` not registered as product backend.
- VSEARCH runtime behavior unchanged.
- VINDEX runtime behavior unchanged.
- VSEARCH response unchanged.
- RDB/AOF unchanged.
- `kvs_vector_index_ops_t` unchanged.
- current `core_hnsw` algorithm unchanged.

## Stage 9F2 recommendation

Recommended:

```text
Stage 9F2: core_hnsw_v2 mark_delete API + tombstone gate
```

Reason: 9F1 only establishes manager-owned control-plane skeleton. Before manager write-path delta capture or shadow rebuild replay can handle DELETE, v2 needs a formal `mark_delete(record_id)` API and tombstone behavior gate. Without it, DELETE deltas cannot be safely replayed into a future shadow v2 backend.

Do not proceed directly to product registration or shadow swap.

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
?? docs/vector_core_reset/stage9f1_manager_owned_rebuild_state_machine_skeleton.md
?? include/vector/kvs_vector_hnsw_v2.h
?? include/vector/kvs_vector_rebuild.h
?? src/vector/core/hnsw_v2/
?? src/vector/kvs_vector_rebuild.c
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
?? tests/vector_rebuild_state_machine_test.c
?? tests/vector_rebuild_state_machine_test.sh
```

9F1 本阶段新增/修改文件见上方 Modified / added files；其余为进入本阶段前已存在的工作区状态或前序 stage 产物，未在本阶段处理。
