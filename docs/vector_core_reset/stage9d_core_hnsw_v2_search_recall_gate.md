# Stage 9D: core_hnsw_v2 search and recall gate

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只实现 `core_hnsw_v2` index-only search / recall gate。未接入 VINDEX/VSEARCH，未接入 manager rebuild path，未修改 current `core_hnsw` 算法，未改 RDB/AOF，未改 `kvs_vector_index_ops_t`，未改 VSEARCH response，未让 v2 访问 `g_vector_state`、`record_id_map` 或 scope / TTL / metadata / question / answer 等 manager 语义字段。

本 prompt 与 `AGENTS.md` 无冲突。

## 2. Modified / added files

本阶段修改：

- `Makefile`
- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_context.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_info.c`
- `tests/vector_core_hnsw_v2_skeleton_test.c`

本阶段新增：

- `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c`
- `tests/vector_core_hnsw_v2_search_recall_test.c`
- `tests/vector_core_hnsw_v2_search_recall_test.sh`
- `docs/vector_core_reset/stage9d_core_hnsw_v2_search_recall_gate.md`

工作树中仍有此前 Stage 7/8/9 报告和代码改动，以及既有无关 `README.md` 修改；本阶段未处理无关 README 修改。

## 3. Whether production code changed

新增 v2 search production source，并将 `src/vector/core/hnsw_v2/*.c` 纳入 `Makefile` 正常对象列表，完整 `make` 会编译 v2 `.o` 并链接进 `kvstore` / `libkvstore.a`。

但 v2 仍未注册为产品 backend：

- 未新增 public index type。
- 未修改 `kvs_vector_index_ops_t`。
- 未接入 VINDEX/VSEARCH。
- 未接入 manager rebuild。
- VSEARCH response 格式未变。

## 4. Current call chain

Stage 9D index-only harness:

```text
tests/vector_core_hnsw_v2_search_recall_test.sh
  -> build/tests/bin/vector_core_hnsw_v2_search_recall_test
  -> kvs_hnsw_v2_index_init()
  -> kvs_hnsw_v2_index_reserve()
  -> kvs_hnsw_v2_index_add()
  -> kvs_hnsw_v2_index_search()
     -> upper greedy search
     -> level0 ef_search
     -> record_id + score export
  -> independent exact scan recall oracle
```

current `core_hnsw` production call chain 未变。

## 5. Before behavior

Stage 9C 已有 v2 graph construction prototype：

- random level
- add path
- graph adjacency
- neighbor selection
- bidirectional link / reverse pruning
- graph quality stats

但没有 search API、search context、recall gate 或 ef_search sensitivity。

## 6. After design

新增 API：

```c
int kvs_hnsw_v2_index_search(
    kvs_hnsw_v2_index_t *index,
    const float *query,
    size_t dim,
    size_t topk,
    size_t ef_search,
    kvs_vector_hit_t *hits,
    size_t *out_count
);
```

输出只包含：

- `record_id`
- `score`

不返回、不读取 question / answer / scope / metadata，不调用 manager，不处理 TTL / threshold。

## 7. Search context design

`kvs_hnsw_v2_context_pool_t` 从 Stage 9B placeholder 改为 search context pool：

```text
kvs_hnsw_v2_search_context_t:
  candidates buffer
  top buffer
  scratch buffer
  capacity fields
```

当前 index-only search 使用 pool 中第一个 context。buffer 首次按需分配，后续复用；query hot path 不在每次查询 malloc/free。`kvs_hnsw_v2_memory_refresh()` 已将 context struct 和已分配 buffer 计入 `context_bytes`。

## 8. Candidate heap / top heap semantics

本阶段实现清晰数组式 heap-equivalent 语义，未做 heap 性能优化：

- candidate set: 保存待扩展候选，每次取 score 最大项。
- top set: 保存已发现的 ef_search 个最好候选，lower_bound 为 top set 最差 score。
- early stop: 当 top set 已满且当前 candidate score 小于 lower_bound 时停止。
- result export: top set 按 score 降序排序后输出 topK。
- `ef_search == 0` 时使用 index 默认 `index->ef_search`。
- `effective_ef_search < topK` 时 clamp 到 topK。
- `effective_ef_search > size` 时 clamp 到 size。

## 9. Score polarity

v2 search、neighbor selection、early stop、result export、exact oracle 使用统一 larger-is-better：

- IP: dot，越大越好。
- COSINE: cosine score，越大越好。
- L2: `-squared_l2`，越大表示距离越小。

本阶段 recall gate 使用 metric=IP，base/query vectors 使用同一生成器与 normalization。

## 10. Exact ground truth

测试中实现独立 exact scan helper：

- 直接扫描同一批 base vectors。
- 使用同一 metric / dim / topK。
- 跳过 deleted slot。
- 按 score 降序生成 exact topK。

未引入 manager 依赖，也未通过 VSEARCH。

## 11. Recall gate results

参数：

```text
metric=IP
dim=512
topK=10
queries=50
M=32
M0=64
ef_construction=128
ef_search=256
distance backend=avx2
```

| count | recall@10 | gate | build_us | search_avg_us | visited_avg | distance_avg | level0 degree avg | memory_bytes |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 1.0000 | >=0.99 pass | 1,753,730 | 366 | 1000.00 | 1038.62 | 64.00 | 6,390,864 |
| 5,000 | 1.0000 | >=0.95 pass | 29,593,877 | 1,344 | 4772.44 | 4852.78 | 64.00 | 32,052,240 |
| 10,000 | 0.9840 | >=0.95 pass, target >=0.98 pass | 130,253,565 | 2,905 | n/a in ef table | n/a in ef table | 64.00 | not printed in ef table |

10K result comes from the same 10K graph used by ef_search sensitivity at `ef=256`.

## 12. ef_search sensitivity

Same 10K / 512d graph:

| ef_search | recall@10 | search_avg_us |
|---:|---:|---:|
| 64 | 0.6640 | 948 |
| 128 | 0.8800 | 1,542 |
| 256 | 0.9840 | 2,905 |
| 512 | 1.0000 | 5,408 |
| 1024 | 1.0000 | 10,971 |

Recall is monotonic non-decreasing as ef_search increases.

## 13. Search stats

新增 runtime stats fields:

- `search_query_count`
- `search_ef_search`
- `search_visited_count_total`
- `search_distance_compute_count_total`
- `search_candidate_push_count`
- `search_candidate_pop_count`
- `search_candidate_skip_count`
- `search_top_push_count`
- `search_top_replace_count`
- `search_early_stop_count`
- `search_deleted_skip_count`
- `search_upper_greedy_step_count`
- `search_level0_neighbor_scan_count`
- `search_total_us`
- `search_avg_us`
- last-query variants for visited / distance / candidate / top / early_stop / deleted_skip

1K/5K printed stats:

| count | candidate_push | candidate_pop | candidate_skip | top_push | top_replace | early_stop | deleted_skip |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 27,989 | 12,850 | 791,261 | 12,800 | 15,189 | 50 | 0 |
| 5,000 | 44,814 | 12,855 | 774,756 | 12,800 | 32,014 | 50 | 0 |

## 14. Miss-case analysis

Recall gate passed:

- 1K/512d: 1.0000
- 5K/512d: 1.0000
- 10K/512d ef=256: 0.9840
- 10K/512d ef=512/1024: 1.0000

因此本阶段未触发 required miss-case dump。若后续 recall gate 下降，应优先审计 search heap semantics、entry point / upper greedy path 和 graph construction candidate collection。

## 15. Graph stats and recall

Stage 9D 10K graph 在 level0 上保持满度 `M0=64`，无 isolated node。1K/5K 搜索在 ef=256 下分别达到 1.0000 recall；10K 在 ef=256 达到 0.9840，ef=512 后达到 1.0000。

这说明 Stage 9C 的 independent forward edge / best-effort reverse pruning graph semantics 足以通过当前 index-only recall gate。剩余主要问题不是 correctness，而是 Stage 9C/9D prototype 的 build/search performance 尚未优化。

## 16. Correctness tests

通过：

```text
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_v2_graph_construction_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
```

`vector_core_hnsw_v2_search_recall_test` 覆盖：

- empty index search
- one vector search
- topK larger than size
- dim mismatch reject
- deleted flag skip via test helper
- result score descending
- exact scan recall for 1K/512d, 5K/512d, 10K/512d
- ef_search sensitivity

## 17. Build result

通过：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

构建输出确认 v2 source 已编译：

```text
src/vector/core/hnsw_v2/kvs_hnsw_v2_index.o
src/vector/core/hnsw_v2/kvs_hnsw_v2_storage.o
src/vector/core/hnsw_v2/kvs_hnsw_v2_context.o
src/vector/core/hnsw_v2/kvs_hnsw_v2_info.o
src/vector/core/hnsw_v2/kvs_hnsw_v2_random.o
src/vector/core/hnsw_v2/kvs_hnsw_v2_graph.o
src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.o
src/vector/core/hnsw_v2/kvs_hnsw_v2_add.o
src/vector/core/hnsw_v2/kvs_hnsw_v2_search.o
```

## 18. Static boundary checks

通过，无输出：

```text
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
```

通过：

```text
git diff --check
```

## 19. git status --short

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
?? tests/vector_core_hnsw_v2_search_recall_test.c
?? tests/vector_core_hnsw_v2_search_recall_test.sh
?? tests/vector_core_hnsw_v2_skeleton_test.c
?? tests/vector_core_hnsw_v2_skeleton_test.sh
```

## 20. Next suggested step

Recall gate passed. 下一步可进入 Stage 9E，但只能做 performance profiling / optimization planning，优先基于本阶段数据审计：

1. Stage 9C graph construction 仍是全量候选 prototype，10K/512d build_us 约 130s，不应作为生产 rebuild path。
2. Stage 9D search candidate/top set 目前是清晰数组语义，不是 heap-optimized implementation。
3. 进入任何 VINDEX/VSEARCH integration 前，应先完成 v2 build/search performance audit，并明确是否替换 Stage 9C 全量候选 construction 为 HNSW-style add-time layer search。
