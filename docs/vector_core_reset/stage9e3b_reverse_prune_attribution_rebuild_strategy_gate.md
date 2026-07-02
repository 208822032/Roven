# Stage 9E3b: reverse_prune attribution and rebuild strategy decision gate

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只做 `core_hnsw_v2` index-only reverse_prune attribution 与 rebuild strategy decision gate。未做正式 reverse_prune cache 优化，未改 graph 语义，未接入 VINDEX/VSEARCH，未注册 v2 backend，未修改 current `core_hnsw` 行为，未改 RDB/AOF，未改 `kvs_vector_index_ops_t`，未改 VSEARCH response，未让 v2 访问 `g_vector_state`、manager-owned `record_id_map` 或 scope / TTL / metadata / question / answer 等 manager 语义字段。

本 prompt 与 `AGENTS.md` 无冲突。

## 2. Modified / added files

修改：

- `Makefile`
- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c`
- `tests/vector_core_hnsw_v2_performance_profile_test.c`

新增：

- `tests/vector_core_hnsw_v2_reverse_prune_profile_test.c`
- `tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh`
- `docs/vector_core_reset/stage9e3b_reverse_prune_attribution_rebuild_strategy_gate.md`

## 3. Production code changed

v2 index-only production code只增加低成本 counters 和默认关闭的 diagnostic hooks。

未改变：

- forward link / reverse link / reverse prune 决策语义
- neighbor_select 排序、score polarity、visited/deleted 语义
- degree 上限、duplicate edge 处理
- search 行为和输出顺序
- RDB/AOF、V command response、manager/core boundary

新增 pair attribution hash table 不由 index 正常路径分配；只在 `tests/vector_core_hnsw_v2_reverse_prune_profile_test.c` 中显式分配并挂到 `index.reverse_prune_diag`。默认 `reverse_prune_diag.enabled=0`。

## 4. Current call chain

Build:

```text
kvs_hnsw_v2_index_add()
  -> kvs_hnsw_v2_connect_new_node()
     -> construction greedy / layer search
     -> kvs_hnsw_v2_neighbor_select()
     -> kvs_hnsw_v2_connect_layer()
        -> forward A -> B append
        -> kvs_hnsw_v2_add_reverse_edge(B, A)
           -> if B has room: append
           -> else reverse_prune:
              -> rescore B -> existing neighbors
              -> rescore B -> A
              -> kvs_hnsw_v2_neighbor_select(B, candidates)
                 -> candidate-to-selected diversity scores
              -> rewrite B row with selected neighbors
```

Search path is unchanged from Stage 9E2/9E3.

## 5. Before behavior

Stage 9E3 showed build residual bottleneck:

- 10K/512d build_us about 49.9s in that run.
- `reverse_prune_us` about 42.8s.
- reverse_prune contained about 41.6M direct rescoring calls and about 961.2M neighbor_select distance calls.

But Stage 9E3 did not answer whether these reverse_prune distances are reusable enough to justify a formal Stage 9E3c optimization, nor whether rebuild can be made acceptable by background shadow rebuild.

## 6. After design

新增 full-path counters:

- `reverse_prune_call_count`
- `reverse_prune_direct_score_count`
- `reverse_prune_existing_score_count`
- `reverse_prune_new_score_count`
- `reverse_prune_neighbor_select_call_count`
- `reverse_prune_candidate_count_total`
- `reverse_prune_existing_degree_total`
- `reverse_prune_candidate_count_hist[]`
- `reverse_prune_existing_degree_hist[]`
- `reverse_prune_rewrite_count`
- `reverse_prune_reject_count`
- `reverse_prune_candidate_score_known_recompute_count`

新增 diagnostic-only reverse_prune pair attribution:

- `sampled_reverse_prune_call_count`
- `sampled_score_pair_observation_count`
- `sampled_unique_pair_score_count`
- `sampled_repeated_pair_score_count`
- `sampled_direct_existing_score_count`
- `sampled_direct_existing_repeated_count`
- `sampled_neighbor_select_pairwise_score_count`
- `sampled_neighbor_select_pairwise_repeated_count`
- `pair_table_used`
- `pair_table_saturated_count`

Interpretation rule:

- normal profile counters are full-path counters.
- pair attribution is sampling + direct-mapped fingerprint table.
- direct-mapped collision is counted as `pair_table_saturated`; repeated-pair estimates are therefore conservative when collision count is high.
- attribution-enabled build_us is diagnostic cost and must not be interpreted as formal path regression.

## 7. Normal build/search profile

Command: `tests/vector_core_hnsw_v2_performance_profile_test.sh`.

This run executed while other profile tests were also running, so wall-clock timing is noisier than Stage 9E3. Counts and graph stats are the stable evidence.

| count | build_us | reverse_prune_us | neighbor_select_us | build_distance | reverse_direct | reverse_ns_distance | ef=256 recall | ef=256 search_avg_us | ef=512 recall | ef=512 search_avg_us | duplicate | overflow |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 1,974,659 | 1,793,937 | 1,663,360 | 35,683,398 | 3,889,600 | 30,223,169 | 1.0000 | 210.74 | 1.0000 | 355.04 | 0 | 0 |
| 5K | 21,526,637 | 18,677,790 | 17,998,694 | 398,817,470 | 20,633,088 | 350,424,196 | 1.0000 | 576.76 | 1.0000 | 903.68 | 0 | 0 |
| 10K | 59,967,970 | 49,854,925 | 47,444,097 | 1,081,028,852 | 41,584,096 | 961,155,196 | 0.9860 | 1,243.60 | 1.0000 | 2,170.24 | 0 | 0 |

10K observations:

- `reverse_prune_us / build_us = 83.1%`.
- reverse_prune score work = `41.58M + 961.16M = 1.003B` distance-related calls.
- `reverse_prune_neighbor_select_distance_compute_count` dominates reverse_prune score work.
- graph health is unchanged: duplicate edge = 0, degree overflow = 0.

## 8. Reverse_prune call breakdown

Command: `tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh`.

This test enables diagnostic sampling. Build wall time includes attribution overhead; use it only as an attribution-enabled run.

| count | reverse_prune_call | reverse_prune_us | direct_score | ns_call | ns_distance | candidate avg/p50/p95 | existing degree avg/p50/p95 | rewrite | reject |
|---:|---:|---:|---:|---:|---:|---|---|---:|---:|
| 1K | 59,840 | 3,267,704 | 3,889,600 | 59,840 | 30,223,169 | 65.00 / 65 / 65 | 64.00 / 64 / 64 | 56,062 | 3,778 |
| 5K | 318,976 | 20,804,325 | 20,633,088 | 318,976 | 350,424,196 | 64.69 / 65 / 65 | 63.69 / 64 / 64 | 281,461 | 37,517 |
| 10K | 643,552 | 52,060,829 | 41,584,096 | 643,552 | 961,155,196 | 64.62 / 65 / 65 | 63.62 / 64 / 64 | 561,028 | 82,526 |

The reverse_prune candidate set is nearly always a full row plus the new candidate:

```text
candidate_count ~= 65
existing_degree ~= 64
```

So most reverse_prune calls run a full M0-sized neighbor_select over B's row.

## 9. Repeated distance attribution

| count | sample interval | sampled pair observations | sampled unique pairs | sampled repeated pairs | pair collisions | estimated reusable | estimated reusable ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 1 | 34,112,769 | 364,367 | 30,619,219 | 3,129,183 | 30,638,408 | 0.8982 |
| 5K | 32 | 11,567,522 | 2,718,930 | 5,696,462 | 3,152,130 | 182,596,864 | 0.4921 |
| 10K | 128 | 7,805,077 | 4,094,699 | 1,860,224 | 1,850,154 | 238,752,224 | 0.2381 |

10K estimate details:

- reverse_prune score work total: `1,002,739,292`.
- sampled repeated pairs scaled by sample interval: at least `238,752,224` reusable score opportunities.
- direct-mapped collisions were `1,850,154` in the 10K sample, so the 23.81% reusable ratio is a conservative lower-bound style estimate.
- The estimated reverse_prune wall-time upper-bound reduction from this conservative ratio is about `12.4s` in the attribution-enabled run.

## 10. Candidate cache opportunity

10K/512d:

| score class | count | cache/reuse note |
|---|---:|---|
| `new_slot -> old_slot` direct score | 643,552 | Already known from forward selected candidate score; small but trivial to pass into reverse edge. |
| `old_slot -> existing_neighbor` direct rescoring | 40,940,544 | Row-local existing edge scores are repeatedly recomputed across reverse_prune calls. |
| `query/new_slot -> candidate` score already known but recomputed | 643,552 | Same as `new_slot -> old_slot`; low absolute impact. |
| neighbor_select pairwise diversity score | 961,155,196 | Dominant score class; sampled repeated-pair evidence shows substantial reuse opportunity. |
| estimated cacheable direct score | 16,020,448 | Conservative sampled estimate plus known new-slot score recompute. |
| estimated cacheable neighbor_select score | 222,731,776 | Conservative sampled estimate. |
| estimated non-cacheable score | 763,987,068 | Remaining score calls under this diagnostic model. |

If Stage 9E3c proceeds, the priority should be:

1. Pass known `new_slot -> old_slot` score into reverse edge to remove the trivial recompute.
2. Add reverse_prune-local score reuse for old row candidates and neighbor_select diversity pairs.
3. Keep the cache local to a prune/add path; do not add graph edge score storage or global pairwise cache in 9E3c unless a new stage explicitly allows it.

## 11. Recall and graph safety

Reverse-prune attribution profile:

| count | ef=256 recall | ef=256 search_avg_us | ef=512 recall | ef=512 search_avg_us | level0 degree avg/max | upper degree avg/p95 | duplicate | overflow |
|---:|---:|---:|---:|---:|---|---|---:|---:|
| 1K | 1.0000 | 219.48 | 1.0000 | 351.88 | 64.00 / 64 | 19.00 / 19 | 0 | 0 |
| 5K | 1.0000 | 595.60 | 1.0000 | 852.04 | 64.00 / 64 | 31.53 / 32 | 0 | 0 |
| 10K | 0.9860 | 1,241.68 | 1.0000 | 1,811.50 | 64.00 / 64 | 31.58 / 32 | 0 | 0 |

Required gates:

- 1K/512d ef=256 recall@10 >= 0.99: pass.
- 10K/512d ef=256 recall@10 >= 0.98: pass.
- 10K/512d ef=512 recall@10 >= 0.99: pass.
- ef_search sensitivity monotonic non-decreasing: pass in `tests/vector_core_hnsw_v2_search_recall_test.sh`.
- duplicate edge = 0: pass.
- degree overflow = 0: pass.

## 12. Background rebuild / shadow swap assessment

Current rebuild cost:

- If rebuild is synchronous user path, 10K/512d ~50-60s is not acceptable for product VINDEX create, online rebuild, bulk import, or crash recovery.
- If rebuild is background shadow build, 10K/512d can be stage-wise acceptable for query hot path because active index continues serving reads.
- The cost still matters for cold start, VINDEX completion latency, tombstone compaction, bulk import, crash recovery, and any workload that requires deterministic rebuild completion.
- For 100K / 1M, this cost almost certainly becomes unacceptable without more build-side optimization or a different rebuild strategy.

Memory:

- Shadow rebuild temporarily requires active index + shadow index memory.
- At 10K/512d v2 memory is about 64.1MB, so shadow rebuild implies roughly 128MB plus context/delta overhead.
- If double memory is not acceptable, build optimization becomes mandatory before integration.

## 13. Shadow rebuild state machine

Minimum state machine:

```text
REBUILD_IDLE
  -> REBUILD_BUILDING
  -> REBUILD_REPLAYING_DELTA
  -> REBUILD_READY_TO_SWAP
  -> REBUILD_SWAPPING
  -> REBUILD_IDLE

Any state
  -> REBUILD_FAILED
  -> REBUILD_IDLE after cleanup/rollback
```

Core objects:

- `active_index`: currently serving VSEARCH.
- `shadow_index`: built from record store snapshot.
- `record store`: source of truth.
- `rebuild_generation`: monotonically identifies the snapshot and delta epoch.
- `rebuild_delta_log`: manager-owned write/delete deltas since snapshot start.
- final short lock / atomic swap: block writes briefly, replay final delta, swap active/shadow pointer.
- old index retire: delayed destroy after no readers hold it.
- failure rollback: discard shadow index and delta state; active index remains serving.

## 14. Write consistency options

| option | behavior | tradeoff |
|---|---|---|
| Pause writes | Block VSET/VDEL during rebuild. | Simple but unacceptable for long 50s+ build. |
| Generation + delta log | Build shadow from snapshot, log writes/deletes, replay delta, final short lock swap. | Recommended minimum; keeps write path mostly available and avoids dual index mutation during long build. |
| Active/shadow dual write | Apply every write to both active and shadow after shadow exists. | Lower replay cost but more complex failure handling and doubles write-time index work. |

Recommended minimum for Stage 9F0 design work:

```text
generation + manager-owned delta log + final short lock swap
```

This preserves the existing source-of-truth rule: RDB/AOF still persist records, not HNSW graph.

## 15. Decision

Algorithm finding:

- `reverse_prune` contains a large amount of reusable distance work.
- 10K conservative estimate: at least `238.8M` reusable reverse_prune score opportunities, about `23.8%` of reverse_prune score work.
- Because the pair attribution table is direct-mapped and collision-heavy, this is conservative; 1K full sampling shows the repeated-score pattern is very strong, and 5K still shows about `49.2%` reusable ratio.
- The dominant opportunity is neighbor_select pairwise diversity score reuse, not the trivial `new_slot -> old_slot` recompute.

Architecture finding:

- Background shadow rebuild can remove rebuild from the query hot path.
- It does not remove cold-start, VINDEX-create, compaction, bulk-import, crash-recovery, memory-doubling, or 100K/1M scaling concerns.
- Current 10K build cost is stage-wise tolerable only if background rebuild is implemented and double memory is acceptable.

Final route judgment:

```text
Enter Stage 9E3c: reverse_prune score reuse optimization.
```

Reason:

- reverse_prune still dominates build.
- measurable score reuse exists and is large enough to justify a narrow formal optimization.
- background rebuild is feasible, but not yet implemented and does not solve build-side scaling.
- Do not enter product integration or layout work directly.

## 16. Build and tests

Passed:

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"             PASS
tests/vector_core_hnsw_v2_skeleton_test.sh                   PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh         PASS
tests/vector_core_hnsw_v2_search_recall_test.sh              PASS
tests/vector_core_hnsw_v2_performance_profile_test.sh        PASS
tests/vector_core_hnsw_v2_distance_profile_test.sh           PASS
tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh      PASS
tests/vector_core_hnsw_correctness_test.sh                   PASS
tests/vector_core_hnsw_rebuild_test.sh                       PASS
tests/vector_core_hnsw_rdb_reload_test.sh                    PASS
tests/vector_core_hnsw_aof_replay_test.sh                    PASS
```

`tests/vector_core_hnsw_v2_search_recall_test.sh` 10K/512d ef sensitivity:

| ef | recall@10 |
|---:|---:|
| 64 | 0.6800 |
| 128 | 0.8840 |
| 256 | 0.9860 |
| 512 | 1.0000 |
| 1024 | 1.0000 |

Monotonic non-decreasing: yes.

## 17. Static boundary checks

Commands:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
git diff --check
```

Results:

```text
g_vector_state grep: no output
record_id_map grep: no output
scope/ttl/metadata/question/answer grep: no output
git diff --check: PASS
```

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

`README.md` and non-v2/current-stage dirty files were not handled in this stage.

## 19. Next suggested step

Proceed to:

```text
Stage 9E3c: core_hnsw_v2 reverse_prune score reuse optimization
```

Minimum boundary:

- reuse/pass known `new_slot -> old_slot` score in reverse edge;
- add local reverse_prune score reuse for candidate/direct and neighbor_select pairwise diversity scores;
- keep cache local to add/reverse_prune path;
- do not add graph edge score storage, global pairwise cache, layout refactor, product integration, or manager rebuild changes in 9E3c.

Do not enter product integration directly until either:

- reverse_prune build debt is reduced or explicitly accepted as background-build debt, and
- Stage 9F0 designs manager-owned generation + delta-log + shadow swap without breaking RDB/AOF or vector_core boundaries.
