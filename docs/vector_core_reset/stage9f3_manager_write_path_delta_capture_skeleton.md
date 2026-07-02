# Stage 9F3: manager write-path delta capture skeleton

## AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。

本阶段只实现 manager write-path delta capture skeleton。未注册 `core_hnsw_v2` backend，未改变 VSEARCH/VINDEX runtime behavior，未修改 VSEARCH response，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 current `core_hnsw` 算法行为，未实现 shadow replay/background thread/active-shadow swap。

本 prompt 与 `AGENTS.md` 无硬边界冲突。`AGENTS.md` 中早期 Stage 5 推荐路线属于历史基线；本阶段按用户提供的 Stage 9F3 路线执行，符合 v2 stage discipline。

## Modified / added files

本阶段修改：

- `include/vector/kvs_vector_rebuild.h`
- `src/vector/kvs_vector_rebuild.c`
- `include/vector/kvs_vector_manager.h`
- `src/vector/kvs_vector.c`
- `Makefile`

本阶段新增：

- `tests/vector_rebuild_delta_capture_test.c`
- `tests/vector_rebuild_delta_capture_test.sh`
- `docs/vector_core_reset/stage9f3_manager_write_path_delta_capture_skeleton.md`

生产代码有变更，但只限 manager-owned rebuild context capture 判断、manager write-path delta append wrapper、VSET/VDEL commit 后 capture 调用点，以及测试目标接线。

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
- 未实现 shadow replay。
- 未实现 active/shadow swap。
- 未实现 background thread。
- vector_core/v2 未访问 `g_vector_state`、manager `record_id_map`、scope/TTL/metadata/question/answer。

## Current call chain

产品主路径仍是：

```text
VSET/VDEL/VSEARCH/VINDEX
  -> existing manager/current_index path
  -> current product backend
```

新增 capture 只在 manager 写路径成功提交后发生：

```text
kvs_vector_set()
  -> record/id maps/current index commit succeeds
  -> kvs_vector_manager_capture_upsert_delta(record_id, embedding, dim)

kvs_vector_set_commit_prepared()
  -> record/id maps/current index commit succeeds
  -> kvs_vector_manager_capture_upsert_delta(record_id, embedding, dim)

kvs_vector_del()
  -> record delete/id maps/free list/current stale flags commit succeeds
  -> kvs_vector_manager_capture_delete_delta(record_id)
```

capture inactive 时 wrapper 立即返回 `KVS_VECTOR_OK`，不 append delta。

## Delta capture activation policy

新增：

```c
bool kvs_vector_rebuild_is_capture_active(const kvs_vector_rebuild_context_t *ctx);
```

capture active 状态：

- `KVS_VECTOR_REBUILD_BUILDING`
- `KVS_VECTOR_REBUILD_REPLAYING_DELTA`
- `KVS_VECTOR_REBUILD_READY_TO_SWAP`

capture inactive 状态：

- `KVS_VECTOR_REBUILD_IDLE`
- `KVS_VECTOR_REBUILD_SWAPPING`
- `KVS_VECTOR_REBUILD_FAILED`
- `KVS_VECTOR_REBUILD_CANCELLED`

`SWAPPING` 在 9F3 中不接受普通写入 capture；本阶段也没有真正 swap 逻辑。

## VSET UPSERT capture point

覆盖两个成功提交点：

- `kvs_vector_set()`：同步 VSET path。
- `kvs_vector_set_commit_prepared()`：prepared VSET commit path，当前 benchmark/async VSET 会使用该路径。

新 record 插入时，只有 record store、question/scope id_map、record_id_map、flat baseline/current index、active record counters 和 manager generation 都成功更新后，才 append UPSERT delta。

同 question/scope 已存在的 VSET 当前只更新 answer、不重算 embedding、不更新 index；9F3 在 answer 更新成功后 append UPSERT delta，使用现有 stable `record_id` 和现存 embedding copy。这样 repeated record_id 不合并，后续 replay 仍可按 generation 顺序执行，later generation wins。

UPSERT delta 保存：

- `record_id`
- `op = KVS_VECTOR_REBUILD_DELTA_UPSERT`
- `generation`
- `dim`
- vector copy

不保存 question/answer/scope/TTL/metadata。

## VDEL DELETE capture point

`kvs_vector_del()` 中只有主删除 commit 成功后 append DELETE delta。commit 成功包括：

- record 标记 deleted。
- current core backend tombstone/stale 逻辑执行完成。
- question/scope id_map remove 完成或已是 not-found。
- manager `record_id_map` remove 完成或已是 not-found。
- free list push 成功。
- active/deleted counters 更新。
- manager generation 更新。

DELETE delta 保存：

- `record_id`
- `op = KVS_VECTOR_REBUILD_DELTA_DELETE`
- `generation`

DELETE delta 不保存 vector，`dim = 0`。

命令级 repeated VDEL 现有语义是第二次找不到 active record，因此不会通过 `kvs_vector_del()` 产生第二条 delta。9F3 不改变这个用户可见语义。底层 `kvs_vector_manager_capture_delete_delta(record_id)` 和 delta log 允许重复 DELETE delta，未来 replay 依赖 9F2 `mark_delete(record_id)` 幂等语义。

## TTL expiry audit

审计结果：当前 vector manager 中 record 有 `expire_at_ms` 字段，RDB restore 会恢复该字段，但 `src/vector/kvs_vector.c` 中没有独立 TTL expiry vector delete/mark-inactive commit 点。当前 TTL expiry 代码主要在普通 KV storage 层和 command executor 层，未发现会调用 vector record delete/active 状态切换的独立路径。

因此 9F3 未接 TTL delta，不新增用户可见 TTL 行为，不把 TTL 语义下沉到 vector_core。后续如果新增或发现 vector record TTL expiry commit 点，应在该 manager commit 成功后 append DELETE delta。

## Append failure strategy

底层 delta append 语义保持 9F1：append 失败不增加 `delta_count`，不消耗 generation。

manager write-path wrapper 策略：

- 主写入/删除已经成功后才尝试 append delta。
- 如果 append 成功，保留 rebuild capture。
- 如果 append 失败，不回滚已成功的主 VSET/VDEL。
- 如果 capture active，则将 rebuild context 标记为 `KVS_VECTOR_REBUILD_FAILED`，记录 `error_code` 和错误消息。
- VSET/VDEL 用户可见返回值不因 capture append 失败改变。

这符合 skeleton 阶段策略：未完成的 background rebuild 失败不破坏当前 active index 和现有写路径。后续真正 background rebuild 可把 delta append 纳入更严格的写锁/事务边界。

## Generation semantics

- `kvs_vector_rebuild_begin()` 会先推进 `current_generation` 并记录 `rebuild_generation`。
- capture active 后，每次 successful append 分配 `current_generation + 1`。
- UPSERT / DELETE 共用同一 generation 序列。
- append 失败不消耗 generation。
- delta log 追加顺序就是未来 replay 顺序。
- repeated record_id 不合并，后续 replay 按 generation 顺序执行，later generation wins。

## Reset/load/destroy cleanup

- `kvs_vector_reset_for_load()` destroy/init rebuild context，清理 delta log 并返回 IDLE。
- `kvs_vector_destroy()` destroy rebuild context，释放 delta vector copy，清理 delta log。
- `kvs_vector_rebuild_reset_idle()` 对 FAILED/CANCELLED cleanup 清理 delta log。

## Test-only activation hook

新增 internal/test-only helper：

```c
int kvs_vector_manager_rebuild_capture_begin_for_test(void);
int kvs_vector_manager_rebuild_capture_stop_for_test(void);
```

它们只在 internal header 中声明，不新增用户命令，不新增配置项，不改变产品默认 runtime。测试通过该 hook 将 rebuild context 进入 BUILDING/cancel cleanup，用于验证 manager write-path capture。

## Why no shadow replay / thread / swap / v2 registration

9F3 只验证 manager 能在写路径成功提交后稳定记录 UPSERT/DELETE delta。shadow index build/replay、后台线程、active/shadow swap 和 v2 产品注册是独立风险面：

- shadow replay 需要验证从 active records build baseline，再按 generation replay delta 的一致性。
- background thread 需要并发生命周期、锁边界和失败恢复策略。
- active/shadow swap 会改变 current index 发布语义。
- v2 backend registration 会改变产品 backend 暴露面。

这些都不属于 9F3。

## Tests

Build:

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
```

Stage 9F tests:

```text
tests/vector_rebuild_state_machine_test.sh PASS
tests/vector_rebuild_delta_capture_test.sh PASS
```

`tests/vector_rebuild_delta_capture_test.sh` 覆盖：

- IDLE 下 VSET 不 append delta。
- capture active 后 VSET append UPSERT delta。
- UPSERT delta 保存 `record_id` / `dim` / vector copy。
- 修改原始 input vector 后 delta vector copy 不变。
- VSEARCH 在 capture active 下不 append delta，结果仍返回 expected record。
- VINDEX rebuild 不 append delta。
- repeated VSET 同一 record_id append 多个 UPSERT，不合并。
- VDEL 成功 append DELETE delta。
- DELETE delta 保存 record_id，不保存 vector。
- 底层 duplicate DELETE capture 可 append 重复 DELETE delta。
- UPSERT/DELETE generation 单调递增。
- capture inactive 后 VSET 不 append delta。
- bottom append failure 保持 delta_count/generation 不变。
- reset-for-load 清理 delta log。
- destroy 清理 delta log。

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

v2 recall key rows from this run:

| count | dim | ef | recall@10 |
|---:|---:|---:|---:|
| 1000 | 512 | 256 | 1.0000 |
| 5000 | 512 | 256 | 1.0000 |
| 10000 | 512 | 64 | 0.6800 |
| 10000 | 512 | 128 | 0.8840 |
| 10000 | 512 | 256 | 0.9860 |
| 10000 | 512 | 512 | 1.0000 |
| 10000 | 512 | 1024 | 1.0000 |

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

## Stage 9F4 recommendation

Recommended:

```text
Stage 9F4: shadow rebuild replay harness
```

Reason:

- 9F1 established manager-owned rebuild state machine and delta log.
- 9F2 added v2 tombstone API for DELETE replay.
- 9F3 now captures manager write-path UPSERT/DELETE deltas.
- Next narrow validation should build a shadow index from active records and replay delta log into it, still index-only / manager-owned.

Stage 9F4 should still not register v2 backend, should not change VSEARCH behavior, and should not do real active/shadow swap.

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
?? docs/vector_core_reset/stage9f2_core_hnsw_v2_mark_delete_tombstone_gate.md
?? docs/vector_core_reset/stage9f3_manager_write_path_delta_capture_skeleton.md
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
?? tests/vector_core_hnsw_v2_skeleton_test.sh
?? tests/vector_core_hnsw_v2_tombstone_test.c
?? tests/vector_core_hnsw_v2_tombstone_test.sh
?? tests/vector_rebuild_delta_capture_test.c
?? tests/vector_rebuild_delta_capture_test.sh
?? tests/vector_rebuild_state_machine_test.c
?? tests/vector_rebuild_state_machine_test.sh
```

9F3 本阶段新增/修改文件见上方 Modified / added files；其余为进入本阶段前已存在的工作区状态或前序 stage 产物，未在本阶段处理。
