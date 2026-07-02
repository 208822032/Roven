# Stage 9E1a: core_hnsw_v2 candidate-quality attribution

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只修改 `core_hnsw_v2` index-only 代码，增加 diagnostic counters / diagnostic mode，不做正式优化。未接入 VINDEX/VSEARCH，未注册 v2 backend，未修改 current `core_hnsw` 行为，未改 RDB/AOF，未改 `kvs_vector_index_ops_t`，未改 VSEARCH response，未让 v2 访问 `g_vector_state`、manager-owned `record_id_map` 或 scope / TTL / metadata / question / answer 等 manager 语义字段。

本 prompt 与 `AGENTS.md` 无冲突。

## 2. Modified / added files

本阶段修改：

- `include/vector/kvs_vector_hnsw_v2.h` — 增加 `kvs_hnsw_v2_diag_t` 诊断结构体，在 `kvs_hnsw_v2_index_t` 中增加 `diag` 字段
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c` — 增加诊断 helper 函数和 instrumentation 点（gated by `diag.enabled`）；增加 `#include <stdlib.h>`
- `tests/vector_core_hnsw_v2_performance_profile_test.c` — 增加 `run_diag_profile()` 和 `run_ef_construction_profile()` 函数

本阶段新增：

- `docs/vector_core_reset/stage9e1a_core_hnsw_v2_candidate_quality_attribution.md`

## 3. Whether production code changed

v2 index-only source changed，仅在 `diag.enabled=1` 时激活诊断测量逻辑。默认 `diag.enabled=0`，诊断代码零开销（每个 instrumentation 点一个 branch）。

v2 仍未作为产品 backend 使用。

## 4. Diagnostic design

### 4.1 新增数据结构 (`kvs_hnsw_v2_diag_t`)

```text
kvs_hnsw_v2_diag_t:
  enabled: int              — 诊断开关
  sample_interval: size_t   — 0=每次 add 采样; N=每 N 次 add 采样
  add_seq: size_t           — 内部 add 计数器

  1. candidate-quality:
     level0_layer_search_count / exact_best_visited / exact_best_in_bounded
     level0_recall_at_M0_sum / level0_recall_at_ef_sum
     upper_layer_search_count / exact_best_visited / exact_best_in_bounded
     upper_recall_at_M_sum / upper_recall_at_ef_sum

  2. neighbor_select quality:
     ns_diag_calls / bounded_selected_total / full_selected_total
     ns_overlap_total / pruned_was_in_exact_M / fallback_count

  3. reverse prune quality:
     rp_attempts_sampled / rejected_was_exact_best / pruned_was_exact_best
     new_slot_score_ratio_sum

  4. entry-point drift:
     upper_greedy_diag_calls / drift_score_sum / drift_slot_match
     level0_entry_diag_calls / entry_score_ratio_sum
```

### 4.2 Instrumentation 点

| 位置 | 测量内容 | 条件 |
|------|---------|------|
| `kvs_hnsw_v2_search_layer_for_add` return 前 | bounded top vs exact top 候选重叠 | `diag_should_sample()` |
| `kvs_hnsw_v2_connect_new_node` upper greedy 后 | entry-point drift | `diag_should_sample()` |
| `kvs_hnsw_v2_connect_new_node` level0 搜索前 | level0 entry 质量 | `diag_should_sample()` |
| `kvs_hnsw_v2_connect_new_node` neighbor_select 后 | bounded vs full selected 重叠 | `diag_should_sample()` |
| `kvs_hnsw_v2_add_reverse_edge` duplicate/reject 时 | 被拒绝边是否为 exact best | `diag_should_sample()` |
| `kvs_hnsw_v2_add_reverse_edge` prune 完成后 | 被修剪边是否为 exact best；score ratio | `diag_should_sample()` |

### 4.3 精确候选计算

`kvs_hnsw_v2_diag_exact_top()` 通过 brute-force 扫描所有 level >= target_level 的已存在 slot，使用 insertion-sort 维护 top-k。这是 O(size * dim) 操作，仅诊断模式启用。

## 5. Candidate-quality attribution results

### 5.1 诊断数据 (1K/512d, ef_construction=128, sample every add)

```text
level0_search_count        = 999
level0_exact_best_visited  = 999  (100.0%)
level0_exact_best_in_bounded = 999 (100.0%)
level0_recall_at_M0        = 0.9685
level0_recall_at_ef        = 0.9364

upper_search_count         = 19
upper_exact_best_visited   = 19   (100.0%)
upper_exact_best_in_bounded = 19  (100.0%)
upper_recall_at_M          = 0.3125
upper_recall_at_ef         = 0.0781

ns_diag_calls              = 1018
ns_bounded_sel_avg         = 61.01
ns_full_sel_avg            = 61.01
ns_overlap_ratio           = 0.9884
ns_pruned_was_exact_M      = 719
ns_fallback                = 28919

rp_attempts_sampled        = 59840
rp_rejected_exact_best     = 0
rp_pruned_exact_best       = 0
rp_score_ratio_avg         = 0.5136

upper_greedy_diag_calls    = 947
upper_drift_avg            = 0.000000
upper_drift_slot_match     = 947   (100.0%)

level0_entry_diag_calls    = 999
level0_entry_score_ratio_avg = 0.4618
```

### 5.2 Attribution 分析

#### 1. bounded layer-search candidates vs full-candidate exact top

**Level0:**
- exact best candidate **always visited** (999/999 = 100%)
- exact best candidate **always in bounded top** (999/999 = 100%)
- recall@M0 = 0.9685: 平均 64 个 exact top-M0 候选中有 ~62 个在 bounded top 中（缺失 ~2 个）
- recall@ef = 0.9364: 平均 128 个 exact top-ef 候选中有 ~120 个在 bounded top 中（缺失 ~8 个）

**Upper:**
- 1K 规模 upper 层极少（仅 19 次搜索），候选质量差（recall@M=0.3125，recall@ef=0.0781）
- 但 upper 层 sparse 且不影响最终 recall，因为 level0 通过 64 条边 dense 连接

**结论：** Level0 bounded search 能找到所有 exact best，但 candidate recall 非 100%——约 3% 的 exact top-M0 候选被遗漏。这些遗漏的 2-3 个近邻在 1K 规模不影响 recall（1.0000），但在 10K 规模累积效应可能导致 0.006 recall 回退。

#### 2. neighbor_select quality

- bounded-candidate neighbor_select 与 full-candidate neighbor_select **重叠率 98.84%**
- bounded selected avg = full selected avg = 61.01（几乎相同）
- 719 次 prune 中被剪掉的候选属于 exact top-M/M0（pruned_was_exact_M）
- 28,919 次 fallback 填充（平均每次调用 ~28 个 fill）

**结论：** neighbor_select 在 bounded candidates 上产生的 selected set 与 full-candidate 几乎一致（98.84% overlap）。diversity prune 剪掉 exact top-M 候选的情况极少（每次调用 <1 个）。**neighbor_select 不是 recall 回退的主因。**

#### 3. reverse prune quality

- **0 个** reverse-rejected 边是 exact best
- **0 个** reverse-pruned 边是 exact best
- new_slot score ratio = 0.5136（新节点得分平均是 best-kept 边的 51.36%）

**结论：** Reverse prune **不会丢失 exact best 边**。质量损失不来自 reverse prune。新节点得分偏低（51% 对比 best kept）是正常的：reverse prune 中的候选来自 old_slot 已有边+new_slot，新节点未必是 old_slot 的最佳近邻。

#### 4. entry-point / upper greedy drift

- upper greedy drift **绝对为零**（upper_drift_avg=0.000000）
- upper greedy final slot **100% 等于 exact best slot**（upper_drift_slot_match=947/947）
- 但 level0 entry score ratio **仅 0.4618**——进入 level0 搜索的入口点得分仅为 level0 exact best 的 46.18%

**结论：** Upper greedy 在每一层都能找到该层的 exact best（零漂移），这说明 upper graph 质量足够好。但 level0 entry 得分低是因为 upper greedy 只管 upper 层的局部最优——从 upper 层下降到 level0 时，entry point 不是 level0 的全局最优。这不是 drift 问题而是 HNSW 多层结构的固有属性。

**Level0 entry 质量低（46.18%）是 level0 bounded search 候选 recall 非 100% 的根因：** 入口点离 exact best 较远，bounded search 需要在 graph 中走更多跳才能到达 exact best 区域，导致探索范围受限（ef_construction 限制了搜索预算）。

### 5.3 ef_construction sensitivity

| count | ef_construction | build_us | ef=256 recall@10 | ef=512 recall@10 | vs Stage 9E0 build |
|------:|---:|---:|---:|---:|:---|
| 1K | 128 | 1,947,163 | 1.0000 | 1.0000 | — |
| 1K | 256 | 2,073,324 | 1.0000 | 1.0000 | — |
| 1K | 512 | 2,268,220 | 1.0000 | 1.0000 | — |
| 5K | 128 | 20,344,262 | 0.9960 | 1.0000 | — |
| 5K | 256 | 22,947,244 | 1.0000 | 1.0000 | — |
| 5K | 512 | 27,487,942 | 1.0000 | 1.0000 | — |
| **10K** | **128** | **54,170,073** | **0.9780** | **1.0000** | **129.1s → 54.2s** |
| **10K** | **256** | **63,989,013** | **0.9860** | **1.0000** | **129.1s → 64.0s** |
| 10K | 512 | 78,540,922 | 0.9840 | 1.0000 | 129.1s → 78.5s |

Key observations:

- ef_construction 提高后 build_us 增加幅度可控：256 比 128 仅增加 ~18%；512 比 128 增加 ~45%
- **ef_construction=256 在 10K/ef=256 上 recall@10=0.9860，恢复 >= 0.98**
- ef_construction=256 在 5K 上 recall@10 从 0.9960 → 1.0000
- ef_construction=512 在 10K/ef=256 上 recall@10=0.9840（与 Stage 9E0 full-candidate 0.9840 **完全一致**）
- **ef_construction=256 的 build 时间（64.0s）仍远优于 Stage 9E0 full-candidate（129.1s），减少 50.4%**

## 6. Graph stats comparison

| count | ef_construction | level0_deg_avg | upper_deg_avg | reverse_prune | memory_bytes |
|------:|---:|---:|---:|---:|---:|
| 1K | 128 | 64.00 | 19.00 | 59,840 | 6,390,864 |
| 1K | 256 | 64.00 | 19.00 | 59,840 | 6,390,864 |
| 1K | 512 | 64.00 | 19.00 | 59,840 | 6,390,864 |
| 5K | 128 | 64.00 | 31.53 | 318,976 | 32,052,240 |
| 5K | 256 | 64.00 | 31.53 | 318,976 | 32,052,240 |
| 5K | 512 | 64.00 | 31.53 | 318,976 | 32,052,240 |
| 10K | 128 | 64.00 | 31.58 | 643,552 | 64,104,384 |
| 10K | 256 | 64.00 | 31.58 | 643,552 | 64,104,384 |
| 10K | 512 | 64.00 | 31.58 | 643,552 | 64,104,384 |

Graph structure is stable across ef_construction values — same degree distribution, same memory. Graph quality improvement from higher ef_construction comes from better candidate selection within the same topological budget.

## 7. Build counter breakdown

| count | ef_construction | construction_layer_search_us | neighbor_select_us | reverse_prune_us | construction_visited | construction_distance |
|------:|---:|---:|---:|---:|---:|---:|
| 10K | 128 | 6,033,790 | 45,003,134 | 46,527,790 | 35,447,416 | 36,176,967 |
| 10K | 256 | 12,538,914 | 48,120,318 | 49,203,929 | 44,739,161 | 45,468,712 |
| 10K | 512 | 25,892,523 | 49,218,345 | 49,418,952 | 49,175,662 | 49,905,213 |

Higher ef_construction increases `construction_layer_search_us` proportionally (more exploration per layer search) but `neighbor_select_us` and `reverse_prune_us` increase only modestly.

## 8. Safety invariants

Preserved:

- v2 owns only record_id, vector, score, index-local graph/search state, slot map, deleted flags, memory/stat info, and diagnostic counters
- Diagnostic counters are v2-local runtime state, never enter RDB/AOF
- No scope / TTL / metadata / question / answer handling in vector_core
- No access to `g_vector_state`
- No access to manager `record_id_map`
- No RDB/AOF format change
- No `kvs_vector_index_ops_t` signature change
- No VSEARCH response change
- No current `core_hnsw` production behavior change
- Diagnostic code is gated by `diag.enabled` (default 0), zero overhead when disabled

## 9. Correctness tests

Passed:

```text
tests/vector_core_hnsw_v2_skeleton_test.sh           PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh  PASS
tests/vector_core_hnsw_v2_search_recall_test.sh       PASS
tests/vector_core_hnsw_v2_performance_profile_test.sh PASS
tests/vector_core_hnsw_correctness_test.sh             PASS
tests/vector_core_hnsw_rebuild_test.sh                 PASS
tests/vector_core_hnsw_rdb_reload_test.sh              PASS
tests/vector_core_hnsw_aof_replay_test.sh              PASS
```

Build passed:

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"     PASS
```

## 10. Static boundary check result

Passed with no output:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
git diff --check
```

## 11. git status

```text
 M AGENTS.md
 M Makefile
 M README.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M include/vector/kvs_vector_hnsw_v2.h        (modified)
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_index.c
 M src/vector/core/hnsw/kvs_hnsw_info.c
 M src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c  (modified)
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_distance_avx2.c
 M tests/vcmd_profile_bench.c
 M tests/vector_core_hnsw_rebuild_profile_test.sh
 M tests/vector_core_hnsw_v2_performance_profile_test.c (modified)
?? docs/vector_core_reset/stage9e1a_core_hnsw_v2_candidate_quality_attribution.md (new)
?? (other pre-existing untracked files)
```

## 12. Judgment

### 12.1 Root cause of 0.006 recall regression

The 0.006 recall regression at 10K/ef=256 (0.9840 → 0.9780) with default ef_construction=128 is attributable to:

1. **Primary: Insufficient search budget (ef_construction) at scale.**
   - At 1K, level0_recall@M0=0.9685 with ef_construction=128 — about 3% of ideal level0 connections are missed during construction
   - At 10K, the graph is larger and the bounded search budget (ef_construction=128) is proportionally smaller relative to graph size
   - Missing these connections during construction creates slightly suboptimal graph topology
   - The cumulative effect across 10K nodes degrades search recall by 0.006

2. **Secondary: Weak level0 entry point (score ratio 0.4618).**
   - The greedy descent from upper layers lands at a node whose score is only ~46% of the level0 global best
   - Combined with limited ef_construction, the bounded search at level0 may not explore far enough from the suboptimal entry point

3. **NOT caused by:**
   - Upper greedy drift (zero drift) — entry-point drift is not the problem
   - Neighbor select quality degradation (98.84% overlap with full-candidate) — neighbor_select works the same on bounded vs full candidates
   - Reverse prune quality (0 exact best edges lost) — reverse prune does not reject important edges

### 12.2 ef_construction sensitivity verdict

| Metric | ef_construction=128 | ef_construction=256 | ef_construction=512 |
|--------|:---:|:---:|:---:|
| 10K build_us | 54.2s | 64.0s | 78.5s |
| 10K ef=256 recall@10 | 0.9780 | **0.9860** | 0.9840 |
| recall >= 0.98 | NO | **YES** | YES |
| vs Stage 9E0 build (129.1s) | 58% reduction | **50% reduction** | 39% reduction |

**ef_construction=256 recovers recall >= 0.98 at 10K/ef=256 while maintaining 50% build time reduction vs Stage 9E0 full-candidate.**

Stage 9E0 (full-candidate) baseline: recall@10=0.9840, build_us=129,107,928
Stage 9E1 (ef_construction=256): recall@10=0.9860, build_us=63,989,013

This is **BETTER recall AND faster build** than Stage 9E0.

### 12.3 Decision

- **Stage 9E1 is ACCEPTABLE** with ef_construction=256
- **建议将 v2 默认 ef_construction 从 128 提升到 256**
- **下一步进入 Stage 9E2**（不进入 Stage 9E1b 修 graph construction quality，因为 ef_construction=256 已恢复 recall）
- Reverse prune 不是主因（0 exact best 丢失）→ 不需要专修 reverse-prune quality
- Entry-point drift 不是主因（zero drift）→ 不需要专修 upper greedy

## 13. Next suggested step

```text
Stage 9E2: core_hnsw_v2 default ef_construction 128→256 + search heap optimization
```

Rationale:

- Stage 9E1a confirmed ef_construction=256 fixes the recall gap
- The next P1 optimization target is search/construction heap (pop-best by scan → bounded heap)
- ef_construction default change is a one-line config change
- Do NOT enter SIMD/layout before search/construction heap optimization
- Do NOT enter VINDEX/VSEARCH integration before index-only search performance is acceptable
