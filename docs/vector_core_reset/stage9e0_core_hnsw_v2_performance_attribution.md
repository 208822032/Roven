# Stage 9E0: core_hnsw_v2 performance attribution

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只做 `core_hnsw_v2` index-only performance profiling / attribution / optimization plan。未接入 VINDEX/VSEARCH，未注册 v2 为产品 backend，未接入 manager rebuild，未修改 current `core_hnsw` 算法，未改 RDB/AOF，未改 `kvs_vector_index_ops_t`，未改 VSEARCH response，未让 v2 访问 `g_vector_state`、`record_id_map` 或 scope / TTL / metadata / question / answer 等 manager 语义字段。

本 prompt 与 `AGENTS.md` 无冲突。`AGENTS.md` 中“core_hnsw_v2 performance stages 先做 attribution，再做 targeted optimization”的要求与本阶段一致。

## 2. Modified / added files

本阶段修改：

- `Makefile`
- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c`

本阶段新增：

- `tests/vector_core_hnsw_v2_performance_profile_test.c`
- `tests/vector_core_hnsw_v2_performance_profile_test.sh`
- `docs/vector_core_reset/stage9e0_core_hnsw_v2_performance_attribution.md`

工作树中仍有此前 Stage 7/8/9 报告和代码改动，以及既有无关 `README.md` 修改；本阶段未处理无关 `README.md`。

## 3. Whether production code changed

v2 index-only source 增加 runtime profiling counters。v2 源码已由 Stage 9D/本阶段 Makefile 纳入正常编译对象，并链接进 `kvstore` / `libkvstore.a`，但 v2 仍不是产品 backend：

- 未新增 public backend type。
- 未注册 `kvs_vector_index_ops_t`。
- 未接入 VINDEX/VSEARCH。
- 未接入 manager rebuild。
- current `core_hnsw` 行为未变。

Makefile 额外补充 `$(HNSW_V2_SRCS:.c=.o): include/vector/kvs_vector_hnsw_v2.h`，保证 v2 header 字段布局变化会触发 v2 object 重编。`ar t build/lib/libkvstore.a | grep hnsw_v2` 确认库内包含 9 个 v2 object。

## 4. Current call chain

Stage 9E0 index-only profiling harness:

```text
tests/vector_core_hnsw_v2_performance_profile_test.sh
  -> build/tests/bin/vector_core_hnsw_v2_performance_profile_test
  -> kvs_hnsw_v2_index_init()
  -> kvs_hnsw_v2_index_reserve()
  -> kvs_hnsw_v2_index_add()
     -> random level
     -> vector / record_id slot write
     -> full candidate collect over existing slots
     -> neighbor_select
     -> forward A->B append
     -> reverse B->A append/prune
     -> per-add graph_stats / memory refresh
  -> exact scan oracle
  -> kvs_hnsw_v2_index_search()
     -> context ensure / visit tag
     -> upper greedy
     -> level0 array candidate/top search
     -> result sort/export
```

## 5. Before behavior

Stage 9D exposed coarse v2 recall and search stats:

- recall gate passed: 1K/512d = 1.0000, 5K/512d = 1.0000, 10K/512d ef=256 = 0.9840.
- build was known slow: 10K/512d around 130s.
- search candidate/top implementation was known array-based, but no time split showed how much it cost.
- build path did not split candidate collect, neighbor_select, reverse prune, graph stats refresh, memory refresh.

## 6. After design

新增 v2-only profiling fields:

Build path:

- `add_count`
- `add_total_us`
- `random_level_us`
- `vector_write_us`
- `id_slot_map_us`
- `candidate_collect_us`
- `construction_layer_search_us`
- `neighbor_select_us`
- `forward_link_us`
- `reverse_link_us`
- `reverse_prune_us`
- `graph_stats_refresh_us`
- `memory_refresh_us`
- `distance_compute_count`
- `candidate_collect_scan_count`
- `candidate_collect_kept_count`

Search path:

- `search_avg_us`
- per-query p50 / p95 / p99 in the profile test
- `upper_greedy_us`
- `level0_search_us`
- `candidate_select_us`
- `top_maintenance_us`
- `result_sort_us`
- `context_acquire_release_us`
- visited / distance / level0 scan avg, p50, p95, p99
- candidate push / pop / skip
- top push / replace
- early stop count

`construction_layer_search_us` is intentionally 0 in current Stage 9C/9D graph construction because v2 add path does not yet implement HNSW-style add-time layer search. Current construction still uses full candidate collection.

## 7. Build attribution

Parameters:

```text
metric=IP
dim=512
topK=10
queries=50
M=32
M0=64
ef_construction=128
distance backend=avx2
```

| count | build_us | add_total_us | candidate_collect_us | neighbor_select_us | reverse_link_us | reverse_prune_us | graph_stats_refresh_us | memory_refresh_us | build_distance_compute_count | candidate_collect_scan_count |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 1,778,645 | 1,775,309 | 19,365 | 1,573,548 | 1,577,360 | 1,571,351 | 2,005 | 40 | 36,114,438 | 509,058 |
| 5,000 | 29,059,053 | 29,037,533 | 489,724 | 27,361,894 | 15,980,360 | 15,923,763 | 49,137 | 257 | 424,757,066 | 12,841,998 |
| 10,000 | 129,107,928 | 129,057,009 | 2,115,210 | 123,419,126 | 43,146,873 | 42,999,099 | 213,105 | 643 | 1,118,475,876 | 51,423,369 |

Graph counters:

| count | selected | pruned | reverse_attempt | reverse_accepted | reverse_rejected | reverse_prune | duplicate | overflow | level0_avg/p50/p95/max | upper_avg/p95 | memory_bytes |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---:|
| 1,000 | 62,110 | 3,085,715 | 62,110 | 58,332 | 3,778 | 59,840 | 0 | 0 | 64.00 / 64 / 64 / 64 | 19.00 / 19 | 6,390,864 |
| 5,000 | 321,585 | 19,301,273 | 321,585 | 283,609 | 37,976 | 318,976 | 0 | 0 | 64.00 / 64 / 64 / 64 | 31.53 / 32 | 32,052,240 |
| 10,000 | 646,166 | 22,830,293 | 646,166 | 566,860 | 79,306 | 643,552 | 0 | 0 | 64.00 / 64 / 64 / 64 | 31.58 / 32 | 64,104,384 |

Attribution:

- Build 主瓶颈是 `neighbor_select_us` / reverse prune path，而不是 vector write、id-slot map、random level、graph stats refresh 或 memory refresh。
- `candidate_collect_scan_count` 随 N 二次增长，10K 已达 51.4M 次扫描；但当前 `candidate_collect_us` 仅约 2.1s，不是 10K build 的最大 wall-time 项。
- full-candidate construction 是根因之一，因为它把大量候选交给 `neighbor_select`，进而触发海量 diversity / reverse prune 距离计算。10K `build_distance_compute_count` 已达 1.118B。
- `graph_stats_refresh_us` 10K 约 0.214s，`memory_refresh_us` 约 0.001s，不污染 129s build timing。

## 8. Search attribution matrix

| count | ef | recall@10 | search_avg_us | p50_us | p95_us | visited_avg | distance_avg | level0_scan_avg | upper_us_avg | level0_us_avg | candidate_select_us_avg | top_maintenance_us_avg | result_sort_us_avg |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 128 | 1.0000 | 285.94 | 284 | 295 | 999.76 | 1,038.38 | 8,200.96 | 2.16 | 280.02 | 32.88 | 116.80 | 3.40 |
| 1,000 | 256 | 1.0000 | 478.48 | 479 | 487 | 1,000.00 | 1,038.62 | 16,384.00 | 1.96 | 464.88 | 79.80 | 234.06 | 11.06 |
| 1,000 | 512 | 1.0000 | 900.18 | 900 | 917 | 1,000.00 | 1,038.62 | 32,768.00 | 2.00 | 858.28 | 189.02 | 468.72 | 39.26 |
| 5,000 | 128 | 0.9580 | 929.38 | 908 | 961 | 3,968.84 | 4,049.18 | 8,211.20 | 5.42 | 919.92 | 45.72 | 380.20 | 3.30 |
| 5,000 | 256 | 1.0000 | 1,631.66 | 1,619 | 1,767 | 4,772.44 | 4,852.78 | 16,390.40 | 5.14 | 1,614.92 | 131.62 | 875.02 | 11.04 |
| 5,000 | 512 | 1.0000 | 2,985.42 | 2,984 | 3,030 | 4,987.60 | 5,067.94 | 32,768.00 | 5.56 | 2,939.20 | 381.46 | 1,828.70 | 40.06 |
| 10,000 | 128 | 0.8800 | 1,401.72 | 1,342 | 1,886 | 5,509.42 | 5,592.74 | 8,277.76 | 7.40 | 1,390.14 | 49.28 | 520.88 | 3.38 |
| 10,000 | 256 | 0.9840 | 2,724.66 | 2,709 | 2,886 | 7,894.30 | 7,977.62 | 16,399.36 | 7.02 | 2,705.96 | 149.68 | 1,368.86 | 11.02 |
| 10,000 | 512 | 1.0000 | 5,322.24 | 5,317 | 5,409 | 9,519.92 | 9,603.24 | 32,773.12 | 7.60 | 5,273.38 | 451.10 | 3,253.90 | 40.54 |

Search operation counters:

| count | ef | candidate_push_avg | candidate_pop_avg | candidate_skip_avg | top_push_avg | top_replace_avg | early_stop_total |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 128 | 356.78 | 129.14 | 7,845.18 | 128.00 | 228.78 | 50 |
| 1,000 | 256 | 559.78 | 257.00 | 15,825.22 | 256.00 | 303.78 | 50 |
| 1,000 | 512 | 812.50 | 513.00 | 31,956.50 | 512.00 | 300.50 | 50 |
| 5,000 | 128 | 522.46 | 129.30 | 7,689.74 | 128.00 | 394.46 | 50 |
| 5,000 | 256 | 896.28 | 257.10 | 15,495.12 | 256.00 | 640.28 | 50 |
| 5,000 | 512 | 1,486.58 | 513.00 | 31,282.42 | 512.00 | 974.58 | 50 |
| 10,000 | 128 | 574.52 | 130.34 | 7,704.24 | 128.00 | 446.52 | 50 |
| 10,000 | 256 | 1,035.24 | 257.24 | 15,365.12 | 256.00 | 779.24 | 50 |
| 10,000 | 512 | 1,772.44 | 513.08 | 31,001.68 | 512.00 | 1,260.44 | 50 |

Search attribution:

- Search 主耗时几乎都在 `level0_search_us`。
- `top_maintenance_us_avg` 是 level0 内最大可分离子项：10K/ef=256 约 1.37ms，占 search avg 约 50%；10K/ef=512 约 3.25ms，占 search avg 约 61%。
- `candidate_select_us_avg` 也随 ef 增长，但显著小于 top maintenance。
- `upper_greedy_us_avg`、`result_sort_us_avg`、`context_us_avg` 很小，不是当前主瓶颈。
- distance compute 随 N 增长，但在 ef=256 下 10K 平均约 7,978 次；从 timing 看，array-based top maintenance 已是比纯 distance 更清楚的 search 瓶颈。

## 9. Required judgments

1. Build 主瓶颈是否来自 full-candidate construction：
   是，但更精确地说，full-candidate collection 本身的扫描时间不是最大项；它产生的全量候选和反向 prune 工作把成本放大到 `neighbor_select_us` / `reverse_prune_us` / `build_distance_compute_count`。当前 add path 没有 HNSW-style layer search，`construction_layer_search_us=0`。

2. Search 主瓶颈是否来自 array-based candidate/top maintenance：
   是。10K/ef=256 `top_maintenance_us_avg=1368.86us`，`candidate_select_us_avg=149.68us`，合计占 `search_avg_us=2724.66us` 的约 56%。ef=512 时 top maintenance 更明显。

3. Distance compute 是否已是主瓶颈：
   Build 侧是主要成本来源之一，因为 neighbor_select / reverse prune 中产生 1.118B 次距离计算。Search 侧暂不应先做 SIMD/distance backend，因为 `top_maintenance_us` 已经暴露为更直接的 array-maintenance 瓶颈。

4. graph_stats_refresh / memory_refresh 是否污染 build timing：
   否。10K `graph_stats_refresh_us=213,105`，`memory_refresh_us=643`，合计约 0.17% build wall time。

5. 下一步优先级：
   - 首选 Stage 9E1: HNSW-style add-time layer search。理由：当前 build 仍是 full-candidate construction，10K build 129s，1.118B build distance compute，`neighbor_select_us=123s`。
   - 次选 Stage 9E2: bounded candidate/top heap。理由：search 侧 10K/ef=256 top maintenance 已占约半数 search time；但 build 目前比 search 更不适合作为生产 rebuild path。
   - 暂缓 Stage 9E3: distance backend/SIMD。理由：search 侧更明确瓶颈是 array top maintenance；build 侧应先减少候选规模和 reverse prune 压力。
   - 暂缓 Stage 9E4: graph layout/compact memory。理由：本阶段未发现 memory layout 是当前最优先瓶颈，且 AGENTS.md 要求 compact/layout 在 recall 和 primary bottleneck 清楚后再做。

## 10. Correctness tests

通过：

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

Final recall gate after this stage:

```text
1K / 512d / ef=256:  recall@10 = 1.0000
5K / 512d / ef=256:  recall@10 = 1.0000
10K / 512d / ef=256: recall@10 = 0.9840
10K / 512d / ef=512: recall@10 = 1.0000
```

Recall 未回退。

## 11. Benchmark result

新增 `tests/vector_core_hnsw_v2_performance_profile_test.sh` 覆盖：

```text
1K / 512d / topK=10 / queries=50 / ef=128,256,512
5K / 512d / topK=10 / queries=50 / ef=128,256,512
10K / 512d / topK=10 / queries=50 / ef=128,256,512
```

主要结果见第 7、8 节。

## 12. Static boundary check result

通过，无输出：

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
git diff --check
```

v2 object 编译/链接确认：

```text
kvs_hnsw_v2_index.o
kvs_hnsw_v2_storage.o
kvs_hnsw_v2_context.o
kvs_hnsw_v2_info.o
kvs_hnsw_v2_random.o
kvs_hnsw_v2_graph.o
kvs_hnsw_v2_neighbor_select.o
kvs_hnsw_v2_add.o
kvs_hnsw_v2_search.o
```

## 13. Build result

通过：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

构建输出确认 v2 object 已链接进 `kvstore`，且 `libkvstore.a` 包含 v2 object。

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

`git status --short` 中未列出本报告文件，是因为该 status 在写报告前采集；报告写入后会新增：

```text
?? docs/vector_core_reset/stage9e0_core_hnsw_v2_performance_attribution.md
```

## 15. Next suggested step

进入 Stage 9E1: HNSW-style add-time layer search。

最小边界：

- 只替换 v2 add-time full candidate construction。
- 保持 index-only，不接入 VINDEX/VSEARCH。
- 不改 current `core_hnsw`。
- 不降低 `ef_construction` / `ef_search`。
- 不牺牲 recall。
- 保留本阶段 profiling 字段，用 1K/5K/10K build/search/recall 对比验证。

若 Stage 9E1 后 build 成本下降但 search 仍由 array top maintenance 主导，再进入 Stage 9E2 bounded candidate/top heap。不要直接进入 SIMD 或 layout。
