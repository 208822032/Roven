# Stage 9E1: core_hnsw_v2 add-time layer search

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只修改 `core_hnsw_v2` index-only add-time construction path 和 v2 profile 输出字段。未接入 VINDEX/VSEARCH，未注册 v2 backend，未接入 manager rebuild，未修改 current `core_hnsw` 行为，未改 RDB/AOF，未改 `kvs_vector_index_ops_t`，未改 VSEARCH response，未让 v2 访问 `g_vector_state`、manager-owned `record_id_map` 或 scope / TTL / metadata / question / answer 等 manager 语义字段。

本 prompt 与 `AGENTS.md` 无冲突。`AGENTS.md` 要求 v2 性能阶段按“add-time layer search -> bounded heap -> distance backend -> layout”的顺序推进，本阶段符合该路线。

## 2. Modified / added files

本阶段修改：

- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`
- `tests/vector_core_hnsw_v2_performance_profile_test.c`

本阶段新增：

- `docs/vector_core_reset/stage9e1_core_hnsw_v2_add_time_layer_search.md`

工作树中仍有此前 Stage 7/8/9 的未提交文件和既有无关 `README.md` 修改；本阶段未处理无关 `README.md`。

## 3. Whether production code changed

v2 index-only source changed. v2 仍未作为产品 backend 使用：

- 未新增 public backend type。
- 未注册 `kvs_vector_index_ops_t`。
- 未接入 VINDEX/VSEARCH。
- 未接入 manager rebuild。
- current `core_hnsw` 仅跑回归，未改行为。

## 4. Current call chain

```text
kvs_hnsw_v2_index_add()
  -> random level
  -> vector / record_id / level write
  -> if first node: set entry point
  -> kvs_hnsw_v2_connect_new_node()
     -> upper layers above new level: greedy search from entry point
     -> levels min(old_max_level, new_level)..0:
        -> kvs_hnsw_v2_search_layer_for_add()
           -> visited_tags based bounded graph exploration
           -> array candidate queue pop-best
           -> array top set bounded by ef_construction
        -> kvs_hnsw_v2_neighbor_select()
        -> forward A->B append
        -> reverse B->A best-effort append/prune
  -> update entry point if new level is higher
```

## 5. Before behavior

Stage 9E0 v2 add path still used full-candidate construction:

```text
for each new node and each connected layer:
  scan existing slots at that layer
  compute query-to-slot score
  pass full candidate list to neighbor_select
  connect selected neighbors
  reverse-link old neighbors and prune
```

This made 10K/512d build unusable:

- `build_us=129,107,928`
- `neighbor_select_us=123,419,126`
- `build_distance_compute_count=1,118,475,876`
- `candidate_collect_scan_count=51,423,369`
- `construction_layer_search_us=0`

## 6. After design

Stage 9E1 replaces full-candidate collection with HNSW-style add-time layer search:

- Start from current `entry_point`.
- For layers above `new_node_level`, run greedy search.
- For layers `min(old_max_level, new_node_level)` down to level0, run bounded layer search with effective `ef_construction`.
- Layer search returns bounded top candidates only.
- `neighbor_select` runs on bounded candidates.
- Existing v2 link semantics are preserved: A->B forward edge is independently appended; B->A reverse edge is best-effort and may be pruned/rejected without canceling A->B.

Candidate/top structures remain intentionally simple for this stage:

- Candidate queue: array, pop-best by scan.
- Top set: array, replace worst by scan.
- This stage does not implement bounded heap optimization; that remains a later candidate only after build quality is acceptable.

Visited tag behavior:

- Reuses index-local `visited_tags` and `current_visit_tag`.
- `current_visit_tag` increments per construction layer search.
- On uint32 overflow, `visited_tags` is cleared and tag resets to 1.
- No manager state is introduced.

New / corrected build profiling fields:

- `construction_candidate_push_count`
- `construction_candidate_pop_count`
- `construction_candidate_skip_count`
- `construction_visited_count`
- `construction_neighbor_scan_count`
- `construction_distance_compute_count`

`candidate_collect_scan_count` is now 0 because the old full-candidate slot scan is removed. `construction_neighbor_scan_count` records bounded layer-search neighbor-entry scans.

## 7. Before / after build matrix

Baseline is Stage 9E0. After values are from the final Stage 9E1 `tests/vector_core_hnsw_v2_performance_profile_test.sh` run.

| count | build_us before | build_us after | add_total_us after | candidate_collect_us before -> after | construction_layer_search_us before -> after | neighbor_select_us before -> after | reverse_prune_us before -> after |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 1,778,645 | 1,745,952 | 1,742,053 | 19,365 -> 0 | 0 -> 104,989 | 1,573,548 -> 1,452,409 | 1,571,351 -> 1,582,154 |
| 5K | 29,059,053 | 18,116,041 | 18,094,147 | 489,724 -> 0 | 0 -> 1,812,487 | 27,361,894 -> 15,090,166 | 15,923,763 -> 15,731,452 |
| 10K | 129,107,928 | 48,071,962 | 48,025,622 | 2,115,210 -> 0 | 0 -> 5,960,430 | 123,419,126 -> 39,219,755 | 42,999,099 -> 40,630,870 |

Build changed in the intended direction:

- 10K `build_us` down from 129.1s to 48.1s.
- 10K `candidate_collect_scan_count` down from 51,423,369 to 0.
- 10K `neighbor_select_us` down from 123.4s to 39.2s.
- 10K `reverse_prune_us` only modestly down, so reverse prune remains the main residual build cost.

## 8. Build counters

| count | build_distance before | build_distance after | candidate_collect_scan before | candidate_collect_scan after | construction_neighbor_scan | construction_visited | construction_push / pop / skip |
|---:|---:|---:|---:|---:|---:|---:|---|
| 1K | 36,114,438 | 35,376,008 | 509,058 | 0 | 7,640,781 | 499,656 | 254,566 / 120,851 / 7,369,532 |
| 5K | 424,757,066 | 384,720,242 | 12,841,998 | 0 | 41,007,437 | 11,212,146 | 2,107,872 / 645,914 / 38,599,690 |
| 10K | 1,118,475,876 | 1,016,660,811 | 51,423,369 | 0 | 83,232,782 | 35,447,416 | 4,876,939 / 1,313,633 / 77,654,496 |

Distance compute decreased, but not proportionally to build_us because reverse prune still recomputes many pairwise scores.

## 9. Search / recall matrix

Search implementation was not optimized in this stage.

| count | ef | recall@10 | search_avg_us | p50_us | p95_us | visited_avg | distance_avg | level0_scan_avg |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 128 | 1.0000 | 289.04 | 287 | 300 | 999.76 | 1,038.38 | 8,200.96 |
| 1K | 256 | 1.0000 | 507.36 | 485 | 652 | 1,000.00 | 1,038.62 | 16,384.00 |
| 1K | 512 | 1.0000 | 953.16 | 901 | 1,412 | 1,000.00 | 1,038.62 | 32,768.00 |
| 5K | 128 | 0.9700 | 942.90 | 926 | 966 | 3,965.36 | 4,045.70 | 8,226.56 |
| 5K | 256 | 0.9960 | 1,651.82 | 1,650 | 1,689 | 4,769.02 | 4,849.36 | 16,387.84 |
| 5K | 512 | 1.0000 | 3,092.36 | 3,088 | 3,207 | 4,987.12 | 5,067.46 | 32,768.00 |
| 10K | 128 | 0.8640 | 1,477.10 | 1,421 | 1,982 | 5,501.30 | 5,584.62 | 8,298.24 |
| 10K | 256 | 0.9780 | 2,771.68 | 2,715 | 3,109 | 7,865.40 | 7,948.72 | 16,408.32 |
| 10K | 512 | 1.0000 | 5,478.10 | 5,272 | 6,552 | 9,495.82 | 9,579.14 | 32,773.12 |

Recall status:

- 1K/512d ef=256 recall@10 = 1.0000, passes required >= 0.99.
- 10K/512d ef=256 recall@10 = 0.9780, passes hard minimum >= 0.95.
- 10K/512d ef=512 recall@10 = 1.0000.
- ef sensitivity is monotonic in the recall test: 0.6500 -> 0.8640 -> 0.9780 -> 1.0000 -> 1.0000.
- Compared with Stage 9E0 10K/ef=256 recall@10 = 0.9840, this is a small regression of 0.0060 and misses the target >= 0.98 by 0.0020.

Therefore Stage 9E1 is not a clean recall acceptance pass, even though the hard gate passes and build time is much lower.

## 10. Graph stats

| count | selected | reverse_attempt | reverse_accepted | reverse_rejected | reverse_prune | duplicate | overflow | level0 avg/p50/p95/max | upper avg/p95 | memory_bytes |
|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---:|
| 1K | 62,110 | 62,110 | 58,349 | 3,761 | 59,840 | 0 | 0 | 64.00 / 64 / 64 / 64 | 19.00 / 19 | 6,390,864 |
| 5K | 321,585 | 321,585 | 287,414 | 34,171 | 318,976 | 0 | 0 | 64.00 / 64 / 64 / 64 | 31.53 / 32 | 32,052,240 |
| 10K | 646,166 | 646,166 | 552,833 | 93,333 | 643,552 | 0 | 0 | 64.00 / 64 / 64 / 64 | 31.58 / 32 | 64,104,384 |

Graph health:

- Degree does not exceed M/M0.
- Duplicate edge count is 0.
- Degree overflow count is 0.
- Memory bytes are unchanged from Stage 9E0 for the same capacity/layout.

## 11. Safety invariants

Preserved:

- v2 owns only record_id, vector, score, index-local graph/search state, slot map, deleted flags, and memory/stat info.
- No scope / TTL / metadata / question / answer handling in vector_core.
- No access to `g_vector_state`.
- No access to manager `record_id_map`.
- No RDB/AOF format change.
- No `kvs_vector_index_ops_t` signature change.
- No VSEARCH response change.
- No current `core_hnsw` production behavior change.
- No ef_construction / ef_search lowering.

## 12. Correctness tests

Final code passed:

```text
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_v2_graph_construction_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_hnsw_v2_performance_profile_test.sh
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
```

Build passed:

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

## 13. Static boundary check result

Passed with no output:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
git diff --check
```

v2 is compiled into `build/lib/libkvstore.a`; `ar t build/lib/libkvstore.a | grep hnsw_v2` lists all v2 objects.

## 14. git status --short

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
?? include/vector/kvs_vector_hnsw_v2.h
?? src/vector/core/hnsw_v2/
?? tests/vector_core_hnsw_v2_graph_construction_test.c
?? tests/vector_core_hnsw_v2_graph_construction_test.sh
?? tests/vector_core_hnsw_v2_performance_profile_test.c
?? tests/vector_core_hnsw_v2_performance_profile_test.sh
?? tests/vector_core_hnsw_v2_search_recall_test.c
?? tests/vector_core_hnsw_v2_search_recall_test.sh
?? tests/vector_core_hnsw_v2_skeleton_test.c
?? tests/vector_core_hnsw_v2_skeleton_test.sh
```

## 15. Judgment and next suggested step

Stage 9E1 achieved the build-path goal mechanically:

- Full-candidate construction was removed.
- 10K build time dropped from about 129s to about 48s.
- `candidate_collect_scan_count` is now 0.
- `neighbor_select_us` dropped sharply.
- Graph degree / duplicate / overflow checks are healthy.

But recall acceptance is not fully clean:

- 10K/ef=256 recall@10 is 0.9780.
- This passes the hard minimum >= 0.95 but misses the target >= 0.98.
- It also regresses slightly from Stage 9E0's 0.9840.

Do not enter Stage 9E2 yet. The next step should be a narrow Stage 9E1a graph-quality / candidate-quality attribution:

- Compare bounded layer-search candidate recall against exact full-candidate top-M/M0 per layer.
- Attribute whether the 0.006 recall loss comes from level0 candidate miss, upper entry-point drift, neighbor_select diversity on bounded candidates, or reverse prune pressure.
- Keep v2 index-only and do not start search heap, SIMD, layout, or product integration until recall quality is explained.
