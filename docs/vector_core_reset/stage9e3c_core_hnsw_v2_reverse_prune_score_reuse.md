# Stage 9E3c: core_hnsw_v2 reverse_prune local score reuse

## 1. AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。

本阶段只修改 `core_hnsw_v2` index-only reverse_prune 本地 score reuse 路径，未接入 VINDEX/VSEARCH，未注册 v2 backend，未改 current `core_hnsw` 行为，未改 RDB/AOF，未改 `kvs_vector_index_ops_t`，未改 VSEARCH response，未让 v2 访问 `g_vector_state`、manager-owned `record_id_map` 或 scope / TTL / metadata / question / answer。

本 prompt 与 `AGENTS.md` 无硬边界冲突。`AGENTS.md` 中早期 “recommended next stage” 已被后续 v2 stage discipline 和本 prompt 的显式路线覆盖。

## 2. Modified / added files

修改：

- `Makefile`
- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c`
- `tests/vector_core_hnsw_v2_performance_profile_test.c`
- `tests/vector_core_hnsw_v2_reverse_prune_profile_test.c`

新增：

- `src/vector/core/hnsw_v2/kvs_hnsw_v2_score_cache.h`
- `docs/vector_core_reset/stage9e3c_core_hnsw_v2_reverse_prune_score_reuse.md`

## 3. Scope result

只做了 local score reuse optimization。

未做：

- graph edge score storage
- global pairwise cache
- layout / compact graph / prefetch
- SIMD / AVX rewrite
- product integration
- manager rebuild / VINDEX / VSEARCH 接入
- current `core_hnsw` 修改

## 4. Current call chain

```text
kvs_hnsw_v2_index_add()
  -> kvs_hnsw_v2_connect_new_node()
     -> kvs_hnsw_v2_search_layer_for_add()
     -> kvs_hnsw_v2_neighbor_select()
     -> kvs_hnsw_v2_connect_layer()
        -> append forward edge A -> B
        -> kvs_hnsw_v2_add_reverse_edge(B, A, reverse_prune_ctx)
           -> if B row has room: append B -> A
           -> else reverse_prune:
              -> existing B -> neighbors direct scores as before
              -> new B -> A score from direct hint
              -> kvs_hnsw_v2_neighbor_select_with_cache()
                 -> optional local pairwise diversity score cache
```

## 5. Before behavior

Stage 9E3b normal 10K/512d profile:

- `build_us=59,967,970`
- `reverse_prune_us=49,854,925`
- `neighbor_select_us=47,444,097`
- `build_distance_compute_count=1,081,028,852`
- `reverse_prune_direct_score_count=41,584,096`
- `reverse_prune_neighbor_select_distance_compute_count=961,155,196`
- `candidate_score_already_known_recomputed=643,552`
- ef=256 recall@10 `0.9860`
- ef=512 recall@10 `1.0000`
- duplicate edge `0`, degree overflow `0`

## 6. After design

Direct known score reuse:

- 新节点 `A` 连接旧邻居 `B` 时，forward selected candidate 已有 `A -> B` score。
- `kvs_hnsw_v2_connect_layer()` 创建 `kvs_hnsw_v2_reverse_prune_ctx_t`。
- reverse prune 组装 new candidate `B -> A` 时优先使用 direct hint。
- score polarity 不变：直接复用 `kvs_hnsw_v2_metric_score()` 产生的 score。
- hint key 使用 directional `from_slot=A, to_slot=B` 校验，避免误用。

Local pair score cache:

- 新增 v2-only internal helper `kvs_hnsw_v2_score_cache.h`。
- cache 生命周期局限于单次 add / reverse_prune 调用链。
- 不保存在 index graph，不跨 index 生命周期，不进入 RDB/AOF，不暴露给 manager。
- key 对称化为 `pair(min(slot_a, slot_b), max(slot_a, slot_b))`。
- direct-mapped O(1) 覆盖策略；冲突只影响命中率，不改变语义。
- `neighbor_select_with_cache()` 只缓存 candidate-to-selected diversity score。
- cache disabled 时旧 `kvs_hnsw_v2_neighbor_select()` 入口行为不变。

Tie-break / output:

- candidate sort、slot duplicate handling、fallback fill、selected output 顺序均沿用旧逻辑。
- cache hit 只替代一次 `metric_score()` 调用，比较条件 `inter_score > candidate.score` 不变。
- result score 降序和 search path 未修改。

## 7. New counters

新增 build stats：

- `reverse_prune_known_score_reuse_count`
- `reverse_prune_known_score_reuse_miss_count`
- `reverse_prune_direct_score_saved_count`
- `reverse_prune_pair_cache_lookup_count`
- `reverse_prune_pair_cache_hit_count`
- `reverse_prune_pair_cache_miss_count`
- `reverse_prune_pair_cache_insert_count`
- `reverse_prune_pair_cache_collision_count`
- `reverse_prune_pair_score_saved_count`

Profile tests now print these fields in normal and reverse_prune diagnostic profiles.

## 8. Normal profile before / after

Baseline is Stage 9E3b normal profile. After is this Stage 9E3c normal profile.

| count | build_us before | build_us after | reverse_prune_us before | reverse_prune_us after | neighbor_select_us before | neighbor_select_us after | build_distance before | build_distance after |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 1,974,659 | 1,970,440 | 1,793,937 | 1,781,889 | 1,663,360 | 1,677,254 | 35,683,398 | 34,168,569 |
| 5K | 21,526,637 | 20,760,782 | 18,677,790 | 18,419,854 | 17,998,694 | 18,083,311 | 398,817,470 | 396,154,322 |
| 10K | 59,967,970 | 56,106,579 | 49,854,925 | 48,915,490 | 47,444,097 | 47,944,384 | 1,081,028,852 | 1,077,622,766 |

10K result:

- `build_us` decreased by about 6.4%.
- `reverse_prune_us` decreased by about 1.9%.
- `neighbor_select_us` did not improve; it was slightly higher in this run.
- Distance compute reduction equals direct score reuse plus pair cache hits: `643,552 + 2,762,534 = 3,406,086`.

This stage met the narrow reuse functionality target, but the pair cache wall-time benefit is limited.

## 9. Reuse counters

| count | known reuse | known miss | direct score saved | pair lookup | pair hit | pair miss | pair score saved |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 59,840 | 0 | 59,840 | 30,223,169 | 1,454,989 | 28,768,180 | 1,454,989 |
| 5K | 318,976 | 0 | 318,976 | 350,424,196 | 2,344,172 | 348,080,024 | 2,344,172 |
| 10K | 643,552 | 0 | 643,552 | 961,155,196 | 2,762,534 | 958,392,662 | 2,762,534 |

Direct known score reuse is complete: `reverse_prune_new_score_count=0` and `candidate_score_already_known_recomputed=0`.

Pair cache is active, but direct-mapped local cache hit rate is low at 10K:

```text
2,762,534 / 961,155,196 = about 0.29%
```

An attempted larger per-add cache and direct-existing reuse saved more scores, but worsened build time due to cache overhead. It was not kept.

## 10. Recall / search sensitivity

Final `tests/vector_core_hnsw_v2_search_recall_test.sh`:

| count | ef | recall@10 | search_avg_us |
|---:|---:|---:|---:|
| 1K | 256 | 1.0000 | 203 |
| 5K | 256 | 1.0000 | 645 |
| 10K | 64 | 0.6800 | 700 |
| 10K | 128 | 0.8840 | 1,074 |
| 10K | 256 | 0.9860 | 1,514 |
| 10K | 512 | 1.0000 | 2,018 |
| 10K | 1024 | 1.0000 | 2,625 |

Recall gates:

- 1K/512d ef=256 recall@10 >= 0.99: pass.
- 10K/512d ef=256 recall@10 >= 0.98: pass.
- 10K/512d ef=512 recall@10 >= 0.99: pass.
- ef_search sensitivity monotonic non-decreasing: pass.

## 11. Graph health

Normal profile after Stage 9E3c:

| count | level0 degree avg/max | upper degree avg/p95 | reverse_prune_call | rewrite | reject | duplicate | overflow |
|---:|---|---|---:|---:|---:|---:|---:|
| 1K | 64.00 / 64 | 19.00 / 19 | 59,840 | 56,062 | 3,778 | 0 | 0 |
| 5K | 64.00 / 64 | 31.53 / 32 | 318,976 | 281,461 | 37,517 | 0 | 0 |
| 10K | 64.00 / 64 | 31.58 / 32 | 643,552 | 561,028 | 82,526 | 0 | 0 |

Graph construction semantics are unchanged by the cache:

- forward A -> B independent link remains.
- reverse B -> A best-effort prune remains.
- degree caps remain enforced.
- duplicate edge and degree overflow remain zero.

## 12. Reverse_prune diagnostic profile

`tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh` passed.

10K diagnostic output:

- `known_score_reuse_count=643,552`
- `known_score_reuse_miss_count=0`
- `direct_score_saved_count=643,552`
- `pair_cache_lookup_count=961,155,196`
- `pair_cache_hit_count=2,762,534`
- `pair_cache_miss_count=958,392,662`
- `pair_score_saved_count=2,762,534`
- ef=256 recall@10 `0.9860`
- ef=512 recall@10 `1.0000`
- duplicate `0`
- overflow `0`

## 13. Build result

Command:

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

Result: pass.

## 14. Correctness / regression tests

Passed:

- `tests/vector_core_hnsw_v2_skeleton_test.sh`
- `tests/vector_core_hnsw_v2_graph_construction_test.sh`
- `tests/vector_core_hnsw_v2_search_recall_test.sh`
- `tests/vector_core_hnsw_v2_performance_profile_test.sh`
- `tests/vector_core_hnsw_v2_distance_profile_test.sh`
- `tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh`
- `tests/vector_core_hnsw_correctness_test.sh`
- `tests/vector_core_hnsw_rebuild_test.sh`
- `tests/vector_core_hnsw_rdb_reload_test.sh`
- `tests/vector_core_hnsw_aof_replay_test.sh`

## 15. Static boundary checks

All produced no output:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
```

`git diff --check`: pass.

## 16. Decision

Stage 9E3c direct known score reuse is effective and safe.

The reverse_prune-local pair cache is also active and safe, but its final low-overhead implementation saves only about 0.29% of 10K reverse_prune neighbor_select pairwise distance calls. Larger local cache variants increased saved scores but regressed build time, so they were not kept.

Therefore:

- The narrow local score reuse optimization is worth keeping.
- Do not expand into broader reverse_prune cache, graph edge score storage, global pairwise cache, or layout work in this route.
- Because build/reverse_prune wall-time improvement is modest, do not continue deep build-side cache work without a new profiling/audit stage.

## 17. Next suggested step

Proceed to Stage 9F0 integration readiness / background rebuild design.

Keep broader reverse_prune cache / edge-score sidecar as technical debt only. It should not be implemented without a new stage that explicitly reopens graph score storage, memory cost, persistence boundary, and product integration implications.

Do not directly enter layout/product integration from this optimization alone.

## 18. git status --short

At report time the working tree still contains prior uncommitted Stage 7-9 files and unrelated `README.md` / current core_hnsw changes from earlier stages. Stage 9E3c touched only the files listed in section 2.

Final `git status --short` should be read from the command output in the task transcript.
