# Stage 9C: core_hnsw_v2 graph construction prototype

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只实现独立 `core_hnsw_v2` 的 index-only graph construction prototype。未实现 search，未接入 VINDEX/VSEARCH，未接入 manager rebuild path，未修改 current `core_hnsw` 算法，未做 SIMD / heap / visited / layout 性能优化，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未让 v2 访问 `g_vector_state` 或 manager 语义字段，未处理既有无关 `README.md` 修改。

本 prompt 与 `AGENTS.md` 无冲突。

## 2. Modified / added files

本阶段修改：

- `Makefile`
- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_info.c`
- `tests/vector_core_hnsw_v2_skeleton_test.c`

本阶段新增：

- `src/vector/core/hnsw_v2/kvs_hnsw_v2_random.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_graph.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c`
- `tests/vector_core_hnsw_v2_graph_construction_test.c`
- `tests/vector_core_hnsw_v2_graph_construction_test.sh`
- `docs/vector_core_reset/stage9c_core_hnsw_v2_graph_construction.md`

工作树中仍有此前 Stage 7/8/9 报告和代码改动，以及既有无关 `README.md` 修改；本阶段未回退或处理这些无关项。

## 3. Whether production code changed

新增 v2 graph construction source，但仍未接入生产 backend：

- 未新增 public backend index type。
- 未注册 `kvs_vector_index_ops_t`。
- 未修改 VINDEX/VSEARCH/rebuild 生产路径。
- `kvstore` 主二进制不选择、不调用 v2。
- v2 只由 `vector_core_hnsw_v2_skeleton_test` 和 `vector_core_hnsw_v2_graph_construction_test` 直接链接验证。

## 4. Current call chain

Stage 9C index-only harness:

```text
tests/vector_core_hnsw_v2_graph_construction_test.sh
  -> build/tests/bin/vector_core_hnsw_v2_graph_construction_test
  -> kvs_hnsw_v2_index_init()
  -> kvs_hnsw_v2_index_set_seed()
  -> kvs_hnsw_v2_index_reserve()
  -> kvs_hnsw_v2_index_add()
     -> random level
     -> vector/id-slot write
     -> layer candidate collection
     -> neighbor_select
     -> forward A->B add
     -> best-effort reverse B->A add/prune
     -> entry_point/max_level update
  -> graph stats / invariants checks
  -> reset / destroy
```

current `core_hnsw` call chain 未变。

## 5. Before behavior

Stage 9B 只有 v2 lifecycle/storage skeleton：

- init / reserve / reset / destroy / info
- aligned vector storage
- id-slot map
- levels / deleted_flags / visited_tags
- graph placeholder
- memory accounting

没有 add、random level、graph adjacency 语义、neighbor selection、bidirectional link 或 graph quality stats。

## 6. After design

Stage 9C 新增：

- deterministic random level generator
- `kvs_hnsw_v2_index_add()`
- level0 graph construction
- upper graph construction
- neighbor selection
- bidirectional link + reverse pruning
- graph quality stats
- graph construction test

仍未实现：

- search
- recall gate
- VINDEX/VSEARCH integration
- manager rebuild integration
- performance optimization

## 7. Random level design

`kvs_hnsw_v2_random_t` 使用 splitmix64-style deterministic generator。

API:

```text
kvs_hnsw_v2_random_init()
kvs_hnsw_v2_random_next_u32()
kvs_hnsw_v2_random_next_unit()
kvs_hnsw_v2_random_level()
kvs_hnsw_v2_index_set_seed()
```

Level policy:

```text
threshold = 1.0 / M
while random_unit < threshold:
  level++
cap at KVS_HNSW_V2_MAX_LEVEL - 1
```

测试使用 deterministic seed，保证 graph construction stats 可复现。1K/64d 测试结果中 `max_level=2`，不是固定死 level。

## 8. Add path design

`kvs_hnsw_v2_index_add(index, record_id, vector, dim)`:

1. 验证 index/vector/dim/capacity/duplicate。
2. 分配 next slot。
3. 生成 random level。
4. 写入 aligned vector row，按 `vector_stride` 清零 padding。
5. 写入 `slot_to_record_id`。
6. 写入 v2-local `record_id_to_slot`。
7. 写入 `levels/deleted_flags/visited_tags`。
8. 第一个节点设置 `entry_point/has_entry/max_level`。
9. 后续节点逐层构建 graph。
10. 若新节点 level 高于当前 max，更新 `entry_point/max_level`。
11. 刷新 graph stats 和 memory accounting。

Duplicate / invalid behavior:

- duplicate `record_id`: reject with `KVS_VECTOR_ERR`，不增长 size。
- dim mismatch: reject with `KVS_VECTOR_ERR_INVALID_ARG`。
- capacity full: reject with `KVS_VECTOR_ERR`。
- id-slot map put 失败或 graph construction 失败：清除本 slot 已写入 vector/id/level 并删除 id-slot entry；当前测试路径未触发 partial graph failure。

## 9. Graph adjacency design

Level0:

```text
level0_neighbors: capacity * M0 uint32_t
level0_counts: capacity uint16_t
degree cap = M0
```

Upper:

```text
upper_neighbors: capacity * (KVS_HNSW_V2_MAX_LEVEL - 1) * M uint32_t
upper_counts: capacity * (KVS_HNSW_V2_MAX_LEVEL - 1) uint16_t
upper_offsets: capacity size_t
degree cap = M
```

这是 Stage 9C 的简单明确预分配策略，不是最终 compact layout。它优先验证 graph semantics 和 stats；未来是否 compact 应由后续阶段数据决定。

## 10. Neighbor selection design

Stage 9C 使用清晰可验证的 diversity heuristic：

1. 去除无效层候选和 duplicate candidate slot。
2. 按 query score 从高到低排序。
3. 依次选择候选。
4. 若候选与已选邻居的相似度高于候选对 query 的相似度，则优先 prune，保留多样性。
5. 若 diversity 后未填满容量，按 score 回填。

Stats:

- `neighbor_select_call_count`
- `neighbor_select_candidate_count`
- `neighbor_select_selected_count`
- `neighbor_select_pruned_count`
- `neighbor_select_duplicate_count`
- `neighbor_select_rejected_count`

## 11. Bidirectional link / reverse pruning design

采用 Stage 9A1 结论中的 v2 语义：

- 新节点 A 选中旧邻居 B 后，A->B 正向边独立保留。
- B->A 反向边尽力添加。
- B 未满时直接 append。
- B 已满时用“B 当前邻居 + A”构造临时候选集，运行 neighbor selection 后重写 B 的该层 row。
- B 拒绝 A 不会取消 A->B。
- 不允许 degree 超过 M/M0。
- 不允许 duplicate edge。

Stats:

- `selected_neighbor_count`
- `forward_edge_added_count`
- `reverse_edge_attempt_count`
- `reverse_edge_accepted_count`
- `reverse_edge_rejected_count`
- `reverse_prune_count`
- `duplicate_edge_count`
- `degree_overflow_count`

## 12. Score polarity

v2 内部统一使用 larger-is-better score：

- IP: dot，越大越好。
- COSINE: cosine score，越大越好。
- L2: `-squared_l2`，越大表示距离越小。

比较方向集中在 `kvs_hnsw_v2_metric_score()` 和 neighbor selection 中，避免局部反转。

## 13. Graph quality stats

`kvs_hnsw_v2_graph_stats_t` 当前包含：

- size
- max_level
- entry_point / has_entry
- level distribution
- level0 degree min/avg/p50/p95/max
- upper degree min/avg/p50/p95/max
- duplicate edge count
- degree overflow count
- selected neighbor count
- forward edge added count
- reverse attempt / accepted / rejected count
- reverse prune count
- neighbor_select counters
- isolated node count

1K / 64d deterministic test output:

```text
hnsw_v2_graph_stats size=1000 max_level=2 entry_point=21 level0_degree=min:16 avg:16.00 p50:16 p95:16 max:16 upper_degree=min:8 avg:8.00 p50:8 p95:8 max:8 selected=16960 forward=16960 reverse_attempt=16960 reverse_accepted=15056 reverse_rejected=1904 reverse_prune=16752 duplicate=0 overflow=0 isolated=0
```

Test config:

```text
dim=64
N=1000
metric=IP
M=8
M0=16
ef_construction=32
seed=0xabcdef
```

## 14. Correctness tests

Passed:

```bash
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_v2_graph_construction_test.sh
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
```

Results:

```text
vector_core_hnsw_v2_skeleton_test PASS
vector_core_hnsw_v2_graph_construction_test PASS
vector_core_hnsw_correctness_test PASS
vector_core_hnsw_rebuild_test PASS
vector_core_hnsw_rdb_reload_test PASS
vector_core_hnsw_aof_replay_test PASS
```

Graph construction test covers:

- init / reserve / add / reset / destroy
- first node entry point
- multi-node size
- duplicate record_id reject
- dim mismatch reject
- capacity full reject
- level0 degree <= M0
- upper degree <= M
- no duplicate edges
- reset clears graph while preserving capacity
- deterministic 1K / 64d graph stats

No recall gate is run in Stage 9C because search is intentionally not implemented.

## 15. Build result

Passed:

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

Result:

```text
✓ 构建完成
二进制: kvstore src/eBPF/ebpf_syncd
eBPF:   src/eBPF/ebpf_kern.o
```

## 16. Static boundary check result

Passed:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\\|ttl\\|metadata\\|question\\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
git diff --check
```

Results:

- `g_vector_state`: no output
- `record_id_map`: no output
- manager semantic grep: no output
- `git diff --check`: PASS

## 17. git status --short

```text
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
?? include/vector/kvs_vector_hnsw_v2.h
?? src/vector/core/hnsw_v2/
?? tests/vector_core_hnsw_v2_graph_construction_test.c
?? tests/vector_core_hnsw_v2_graph_construction_test.sh
?? tests/vector_core_hnsw_v2_skeleton_test.c
?? tests/vector_core_hnsw_v2_skeleton_test.sh
```

## 18. Next suggested step

Stage 9D should implement search and use `core_flat` exact ground truth for recall gate.

Suggested Stage 9D scope:

- implement v2 search only;
- do not optimize performance before recall gate passes;
- cover 1K / 5K / 10K / 512d;
- compare against core_flat exact recall;
- keep v2 disconnected from VINDEX/VSEARCH until index-only recall gate passes.
