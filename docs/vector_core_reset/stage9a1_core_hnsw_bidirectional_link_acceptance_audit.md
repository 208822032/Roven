# Stage 9A1: core_hnsw bidirectional link acceptance root-cause audit

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只做 current `core_hnsw` graph-construction 语义审计、profiling counters 和默认关闭的实验开关。未做 `core_hnsw_v2` skeleton，未做 broad refactor，未做 layout prototype，未做 SIMD/heap/visited 性能优化，未调整默认 `ef_construction` / `ef_search`，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未让 vector_core 访问 manager 语义字段，未处理既有无关 `README.md` 修改。

本 prompt 要求先验证 `accepted_ba` 是否是低召回根因，这与 Stage 9A “旧 core 冻结、v2 设计边界”存在路线调整，但不冲突：本阶段是用户明确要求的窄范围 root-cause audit，不突破 `AGENTS.md` 硬边界。未发现需要停止执行的冲突。

## 2. Modified / added files

本阶段修改：

- `include/vector/kvs_vector_hnsw.h`
- `include/kvs_vector.h`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/kvs_vector.c`
- `src/core/kvs_operation.c`
- `tests/vcmd_profile_bench.c`
- `tests/vector_core_hnsw_rebuild_profile_test.sh`

本阶段新增：

- `docs/vector_core_reset/stage9a1_core_hnsw_bidirectional_link_acceptance_audit.md`

工作树中仍有此前 Stage 7/8/9 报告和代码改动，以及既有无关 `README.md` 修改；本阶段未回退或处理这些无关项。

## 3. Whether production code changed

是，但范围很窄：

- 增加 bidirectional link acceptance audit counters。
- 增加默认关闭的实验开关 `KVS_HNSW_FORWARD_INDEPENDENT_LINK=1`。
- 默认不设置该环境变量时保持原有 graph construction 行为。
- VSEARCH response 未变；仅 VINFO/profile/benchmark 输出增加观测字段。

## 4. Current call chain

Rebuild / add call chain:

```text
vcmd_profile_bench ann_core
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_hnsw_index_add()
  -> kvs_hnsw_search_layer_for_add()
  -> kvs_hnsw_connect_layer_from_context()
  -> kvs_hnsw_add_new_slot_bidirectional_edge()
```

当新节点当前层还有容量时，核心路径位于 `src/vector/core/hnsw/kvs_hnsw_add.c`:

```text
kvs_hnsw_add_new_slot_bidirectional_edge(slot=A, neighbor=B)
  -> plan B->A
  -> if !accepted_ba return OK
  -> append A->B
  -> apply B->A
```

当新节点当前层已满时，会回退到 `kvs_hnsw_add_bidirectional_edge()`，该函数原来也要求 `accepted_ab && accepted_ba` 才同时写入两侧。

## 5. Before behavior

审计确认：当前 baseline 逻辑确实让新节点 `A->B` 正向边依赖旧邻居 `B->A` 反向接受。

关键点：

- `kvs_hnsw_layer_plan_new_slot_append_or_replace(index, neighbor, slot, ...)` 只规划旧邻居 `B->A`。
- 如果 `accepted_ba == 0`，baseline 直接返回。
- 返回前没有写入 `slot_neighbors[slot_count] = neighbor`，因此新节点已选中的 `A->B` 也被跳过。
- 该逻辑不只影响 level0；`kvs_hnsw_connect_layer_from_context()` 对每层调用同一 helper。

## 6. Experimental design

新增 counters：

- `bidirectional_selected_neighbor_count`
- `bidirectional_forward_edge_attempt_count`
- `bidirectional_forward_edge_added_count`
- `bidirectional_reverse_edge_attempt_count`
- `bidirectional_reverse_edge_accepted_count`
- `bidirectional_reverse_edge_rejected_count`
- `bidirectional_forward_edge_skipped_due_to_reverse_reject_count`

实验开关：

```bash
KVS_HNSW_FORWARD_INDEPENDENT_LINK=1
```

实验语义：

- baseline：保持当前逻辑，`B->A` 被拒绝时跳过 `A->B`。
- experiment：只要新节点 `A` 还有层容量，就保留 `A->B`；旧邻居 `B->A` 仍按原逻辑尽力添加/修剪，拒绝时不取消 `A->B`。

实验不改变：

- `M` / `M0` 上限。
- 默认 `ef_construction` / `ef_search`。
- distance kernel。
- manager resolve / filtering / response。
- RDB/AOF。

## 7. Benchmark result

命令：

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
./build/tests/bin/vcmd_profile_bench ann_core 5000 512 10 50
./build/tests/bin/vcmd_profile_bench ann_core 10000 512 10 50

KVS_HNSW_FORWARD_INDEPENDENT_LINK=1 ./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
KVS_HNSW_FORWARD_INDEPENDENT_LINK=1 ./build/tests/bin/vcmd_profile_bench ann_core 5000 512 10 50
KVS_HNSW_FORWARD_INDEPENDENT_LINK=1 ./build/tests/bin/vcmd_profile_bench ann_core 10000 512 10 50
```

### 7.1 Recall and performance

| mode | nb | recall@10 | rebuild_core_add_total_us | hnsw_add_layer_search_us | hnsw_add_neighbor_select_us | hnsw_add_link_us | search_avg_us | backend_search_avg_us |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline | 1K | 1.0000 | 90,463 | 77,244 | 4,274 | 3,049 | 148 | 147 |
| experiment | 1K | 1.0000 | 91,460 | 77,545 | 4,449 | 3,308 | 148 | 147 |
| baseline | 5K | 0.8400 | 939,706 | 850,214 | 28,298 | 18,986 | 394 | 391 |
| experiment | 5K | 0.8460 | 941,885 | 848,862 | 29,789 | 20,462 | 392 | 389 |
| baseline | 10K | 0.6240 | 2,389,232 | 2,175,881 | 67,711 | 48,438 | 752 | 749 |
| experiment | 10K | 0.6380 | 2,380,230 | 2,157,838 | 72,201 | 52,060 | 783 | 781 |

Recall 改善：

- 1K: `+0.0000`
- 5K: `+0.0060`
- 10K: `+0.0140`

结论：实验有轻微正向效果，但远不足以解释 10K/512d `recall@10 = 0.6240` 的不可用级退化。

### 7.2 Degree distribution

| mode | nb | min | avg | p50 | p95 | max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline | 1K | 48 | 62 | 64 | 64 | 64 |
| experiment | 1K | 50 | 62 | 64 | 64 | 64 |
| baseline | 5K | 35 | 61 | 64 | 64 | 64 |
| experiment | 5K | 41 | 62 | 64 | 64 | 64 |
| baseline | 10K | 26 | 60 | 63 | 64 | 64 |
| experiment | 10K | 38 | 62 | 64 | 64 | 64 |

实验模式提升了新节点 level0 final degree 下界和均值，但 max 仍为 `64 == M0`，未发现 degree 超限。

### 7.3 Forward / reverse counters

| mode | nb | selected | forward_attempt | forward_added | reverse_attempt | reverse_accepted | reverse_rejected | skipped_due_to_reverse_reject |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline | 1K | 62,704 | 62,704 | 59,982 | 62,704 | 59,982 | 2,722 | 2,722 |
| experiment | 1K | 62,704 | 62,704 | 62,704 | 62,704 | 59,919 | 2,785 | 0 |
| baseline | 5K | 322,483 | 322,483 | 299,396 | 322,483 | 299,396 | 23,087 | 23,087 |
| experiment | 5K | 322,483 | 322,483 | 322,483 | 322,483 | 298,365 | 24,118 | 0 |
| baseline | 10K | 647,293 | 647,293 | 576,812 | 647,293 | 576,812 | 70,481 | 70,481 |
| experiment | 10K | 647,293 | 647,293 | 647,293 | 647,293 | 572,336 | 74,957 | 0 |

Baseline 10K selected-to-linked ratio:

```text
forward_added / selected = 576,812 / 647,293 = 89.11%
skipped_due_to_reverse_reject / selected = 70,481 / 647,293 = 10.89%
```

Experiment 10K selected-to-linked ratio:

```text
forward_added / selected = 647,293 / 647,293 = 100.00%
skipped_due_to_reverse_reject = 0
```

## 8. Safety checks

Degree / duplicates / memory:

- degree max 未超过 `M0=64`。
- `neighbor_select_duplicate_scan_count=0`，未观察到重复边压力增加。
- `backend_memory_bytes` 相同：
  - 1K: `6,974,672`
  - 5K: `55,595,216`
  - 10K: `111,161,552`
- 实验模式只改变已有 row 中 count/slot 填充，不新增 graph arrays。

Recall:

- experiment 未回退。
- 但改善不显著，不能作为充分修复。

## 9. Correctness tests

通过：

```bash
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
tests/vector_core_hnsw_rebuild_profile_test.sh
```

结果：

```text
vector_core_hnsw_correctness_test PASS
vector_core_hnsw_rebuild_test PASS
vector_core_hnsw_rdb_reload_test PASS
vector_core_hnsw_aof_replay_test PASS
vector_core_hnsw_rebuild_profile_test PASS
```

## 10. Static boundary checks

通过：

```bash
grep -RIn "g_vector_state" src/vector/core || true
grep -RIn "record_id_map" src/vector/core || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core || true
git diff --check
```

结果：

- `g_vector_state`: no output
- `record_id_map`: no output
- manager semantic grep: no output
- `git diff --check`: PASS

## 11. Build result

通过：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

## 12. Root-cause judgment

`accepted_ba` gating 是真实 graph-construction 语义问题：

- baseline 10K 有 `70,481` 条 selected forward edges 被 reverse reject 连带跳过。
- 实验模式能把 forward_added 提升到 selected 的 100%。
- level0 degree min/avg/p50 有改善。

但它不是 10K/512d 低召回主因：

- 10K recall 仅 `0.6240 -> 0.6380`。
- 5K recall 仅 `0.8400 -> 0.8460`。
- 解除该门槛后仍远低于 FAISS HNSW 10K/512d `0.9740` 参考结果。
- search/build time 没有显著改善，10K search_avg_us 还从 `752` 增至 `783`。

因此，不能把 forward-independent link 作为 Stage 9A2 正式修复直接落地；最多作为未来 v2 或 graph semantics 设计时的参考。

## 13. Next suggested step

按本阶段数据，建议不要进入 Stage 9A2 正式修复 `accepted_ba` 语义，因为它不是低召回主因。

下一步应回到 Stage 9B：`core_hnsw_v2` index-only skeleton。v2 后续 add/search 设计应明确采用标准 HNSW 连接语义：新节点 selected neighbors 的正向连接与旧邻居反向接受解耦，但这只是 v2 设计输入之一，不应在 current core_hnsw 上继续扩大修补。

## 14. git status --short

```text
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
?? docs/vector_core_reset/stage7e_neighbor_select_score_reuse_optimization.md
?? docs/vector_core_reset/stage7f_highdim_layer_search_audit.md
?? docs/vector_core_reset/stage7g_layer_search_distance_alignment_optimization.md
?? docs/vector_core_reset/stage7h_level0_neighbor_visited_layout_audit.md
?? docs/vector_core_reset/stage7i_level0_visited_slot_locality_prototype.md
?? docs/vector_core_reset/stage7j_add_search_slot_state_locality_prototype.md
?? docs/vector_core_reset/stage7k_remove_failed_layout_prototypes.md
?? docs/vector_core_reset/stage7l_post_layout_layer_search_hotspot_audit.md
?? docs/vector_core_reset/stage8a_core_flat_hnsw_faiss_parity_architecture_audit.md
?? docs/vector_core_reset/stage9a_core_hnsw_v2_design_boundary.md
?? docs/vector_core_reset/stage9a1_core_hnsw_bidirectional_link_acceptance_audit.md
```
