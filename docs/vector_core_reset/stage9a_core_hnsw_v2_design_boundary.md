# Stage 9A: core_hnsw_v2 design boundary and old-core retirement plan

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段只做 design / architecture audit / retirement plan，未实现 `core_hnsw_v2`，未优化旧 `core_hnsw`，未修改生产代码，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未删除 current `core_hnsw`，未删除 FAISS / hnswlib adapter，未处理既有无关 `README.md` 修改。

本 prompt 明确允许重新设计 `core_hnsw_v2` 路线。Stage 8A 曾建议先做 Stage 8B fairness / FAISS adapter audit，但本阶段用户目标明确改变为自研 `core_hnsw_v2` 设计边界与旧 core 退役计划。该路线调整不突破 `AGENTS.md` 硬边界。未发现需要停止执行的冲突。

约束影响：

- `core_hnsw_v2` 设计必须继续保持 manager / vector_core 分离。
- v2 初期不能要求 RDB/AOF 保存 graph。
- v2 不能要求修改 `kvs_vector_index_ops_t`。
- v2 未通过 recall gate 前不能接管默认 backend。
- 如果未来发现 v2 必须突破这些边界，应先修改路线 / `AGENTS.md`，不能在实现阶段擅自突破。

## 2. Modified / added files

新增文件：

- `docs/vector_core_reset/stage9a_core_hnsw_v2_design_boundary.md`

本阶段未修改生产代码。

工作树中已有 Stage 7E-7L 代码/文档改动、Stage 8A 报告和无关 `README.md` 修改，本阶段未回退或处理。

## 3. Whether production code changed

否。

本阶段只新增设计报告。没有 implementation、optimization、refactor、layout prototype 或测试代码改动。

## 4. User goals restated

本阶段采用以下产品目标：

- KVmanager 与 Vector_core 分离。
- KVmanager 负责 record store、key/scope、TTL、metadata、AOF/RDB、replication、rebuild、response assembly。
- Vector_core 只负责 `record_id + vector -> record_id + score`。
- `core_flat` 作为 exact recall baseline / correctness oracle，不再追求 FAISS flat 性能。
- current `core_hnsw` 作为 legacy backend / regression reference，不再作为主要优化对象。
- `core_hnsw_v2` 作为新的主生产候选 ANN backend。
- FAISS / hnswlib 只作为性能、稳定性、参数语义和 graph quality baseline，不作为产品架构替代。
- record store 是 source of truth；HNSW graph 是 runtime derived state。

## 5. Current architecture boundary

当前 manager / vector_core 边界仍适用于 v2，不需要改变。

Manager 当前职责：

- VSET / VSEARCH / VDEL / VINDEX / VINFO 命令语义。
- record store、key、scope、TTL、metadata、question、answer。
- manager-owned `record_id_map` fast resolve。
- AOF / RDB / replication。
- 从 active records rebuild derived indexes。
- threshold、scope、active/deleted、TTL 等业务过滤。
- VSEARCH response assembly。

Vector_core 当前职责：

- `record_id`、vector、score。
- core-local `record_id -> slot` map。
- core-local deleted/tombstone state。
- graph / search state。
- search context / heap / visited tags。
- core-local memory and profiling stats。

Static boundary expected for v2:

```text
src/vector/core/hnsw_v2/*
  no g_vector_state
  no manager record store access
  no scope / ttl / metadata / question / answer
  no response assembly
  no RDB/AOF serialization format ownership
```

Graph persistence policy remains unchanged:

```text
RDB/AOF save records, not HNSW graph.
load / replay / VINDEX rebuild reconstruct v2 graph from manager active records.
```

## 6. Current call chain

Current core_hnsw rebuild:

```text
VINDEX REBUILD / ann_core direct-vector benchmark
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_vector_manager_for_each_active_embedding()
  -> kvs_hnsw_index_add(record_id, vector)
  -> greedy upper search
  -> kvs_hnsw_search_layer_for_add()
  -> neighbor_select / bidirectional link / reverse pruning
```

Current core_hnsw VSEARCH:

```text
VSEARCH
  -> kvs_vector_index_search_current()
  -> kvs_vector_core_hnsw_ops_search()
  -> kvs_hnsw_index_search(query, raw_topk)
  -> returns kvs_vector_hit_t(record_id, score)
  -> kvs_vector_manager_resolve_hits()
  -> response assembly
```

Current core_flat:

```text
VSEARCH / exact recall baseline
  -> kvs_flat_index_search()
  -> exact scan over core-local vectors
  -> returns kvs_vector_hit_t(record_id, score)
  -> manager resolve / response assembly
```

Stage 9B index-only v2 should initially avoid VSEARCH integration:

```text
unit / index-only harness
  -> kvs_hnsw_v2_index_init()
  -> reserve / reset / destroy / info
  -> no add/search in 9B
```

## 7. Before behavior and current core_hnsw freeze

current `core_hnsw` remains available as:

- legacy backend;
- regression comparison;
- fallback during v2 development;
- old test coverage target;
- profiling/reference source only when comparing v2 against legacy behavior.

Freeze rules:

- Do not delete current `core_hnsw`.
- Do not make broad in-place refactors to current `core_hnsw`.
- Do not continue Stage 7-style local layout / visited / heap micro-optimization.
- Do not add more profiling fields to current `core_hnsw` unless directly needed for v2 comparison.
- Keep existing correctness, rebuild, RDB reload, AOF replay, and rebuild profile tests passing.

## 8. Why current core_hnsw should not continue as the main optimization target

Stage 7/8A evidence:

- Stage 7E solved level0 worst-neighbor distance recompute through score reuse.
- Stage 7G reduced high-dim distance / SIMD / call overhead.
- Stage 7I B packed-row mirror and Stage 7J A slot-hot-state mirror were valid prototypes but did not reduce `level0_us`.
- Stage 7K removed both failed layout prototypes.
- Stage 7L showed remaining add-time `layer_search` cost is mostly dense level0 row expansion with high repeated visited skip pressure plus unique distance compute.
- Stage 8A direct-vector 10K / 512d current `core_hnsw` result was `recall@10 = 0.6240`, `layer_search_us = 2,398,373`, `level0_us = 2,383,517`.

Conclusion:

The remaining risk is no longer one local wrapper, cache, or recompute bug. The larger issue is graph construction/search semantics, parameter scaling, dense level0 expansion behavior, graph quality, and maintainability. Continuing to patch current `core_hnsw` in place risks more production structure pollution without proving recall or graph quality at scale.

## 9. Old design items not to carry into v2

Do not carry these forward as v2 defaults:

- Stage 7I packed level0 row mirror.
- Stage 7J slot_hot_state mirror.
- Broad layout refactor before recall / graph quality gates pass.
- More production profiling fields as a substitute for design clarity.
- Optimizing `level0_us` before recall / graph quality is acceptable.
- Reducing `ef_construction` or `ef_search` to claim speed.
- Treating current `core_hnsw` parameter semantics as equivalent to FAISS / hnswlib without tests.
- In-place rewrite of current `core_hnsw`.
- Any vector_core access to manager fields such as scope, TTL, metadata, question, answer, or `g_vector_state`.
- Any FAISS/hnswlib adapter manager/global-state pattern copied into v2.

## 10. core_hnsw_v2 minimum architecture

Proposed independent implementation boundary:

```text
include/vector/kvs_vector_hnsw_v2.h
src/vector/core/hnsw_v2/
  kvs_hnsw_v2_index.c
  kvs_hnsw_v2_storage.c
  kvs_hnsw_v2_graph.c
  kvs_hnsw_v2_add.c
  kvs_hnsw_v2_search.c
  kvs_hnsw_v2_neighbor_select.c
  kvs_hnsw_v2_context.c
  kvs_hnsw_v2_random.c
  kvs_hnsw_v2_info.c
```

Do not place v2 into current `src/vector/core/hnsw/` during early stages. Keeping a separate directory prevents broad edits to legacy code and makes rollback simple.

### 10.1 Index object

Minimum index fields:

- `dim`
- `metric`
- `M`
- `M0`
- `ef_construction`
- `ef_search`
- `capacity`
- `size`
- aligned vector storage
- `slot_to_record_id`
- core-local `record_id_to_slot`
- graph adjacency storage
- per-slot level distribution
- `entry_point`
- `has_entry`
- `max_level`
- deleted/tombstone state
- visited tags
- search context pool
- memory accounting
- graph quality / correctness stats for index-only gates

The v2 index object must not contain:

- question / answer / scope / TTL / metadata;
- manager record index as externally meaningful label;
- manager-owned `record_id_map`;
- response buffer or `kvs_vector_search_result_t`.

### 10.2 Storage policy

Vector row layout:

- Contiguous slot-major float rows.
- 32-byte or 64-byte alignment for high-dimensional SIMD kernels.
- Explicit stride if padding is introduced.
- `dim` remains logical dimension; padded stride must not leak to manager.

Graph adjacency layout:

- Separate level0 and upper-layer storage is acceptable and likely still useful.
- Level0 should target predictable row access and simple count/header ownership.
- Upper layers should not repeat current large `capacity * KVS_HNSW_MAX_LEVEL * M` preallocation unless Stage 9B/9C memory gates justify it.
- Prefer compact per-layer allocation or offset-based adjacency for upper layers, but implement only after skeleton and add/search gates define exact requirements.

Memory ownership:

- v2 owns all vectors, graph arrays, visited tags, contexts, and id-slot map.
- manager owns record store and rebuild orchestration.
- reset may retain capacity for rebuild reuse.
- destroy releases all v2-owned memory.
- memory accounting must report vectors, graph level0, graph upper, id-slot map, deleted state, visited tags, contexts, and overhead separately enough for comparison.

### 10.3 Add/build policy

Add/build must define:

- deterministic rebuild order from manager active record iteration;
- stable duplicate `record_id` policy: either update vector in-place and mark graph stale, or reject duplicate outside rebuild; first v2 implementation should prefer explicit reject/update semantics with tests;
- random level generator with reproducible seed policy for rebuild repeatability;
- upper greedy search;
- construction layer search;
- neighbor selection heuristic matching accepted HNSW behavior, validated against FAISS/hnswlib semantics;
- bidirectional link creation;
- reverse edge pruning;
- entry point and max level update;
- failure rollback for partially-added slot;
- graph quality stats, including level distribution, degree distribution, unreachable count if available, and duplicate edge count.

Stage 9C should prioritize correctness and graph quality over speed. If recall gates fail, do not enter performance work.

### 10.4 Search policy

Search must define:

- topK semantics;
- `ef_search >= topK` enforcement;
- candidate heap semantics;
- top heap semantics;
- score polarity: larger score is always better for exported `kvs_vector_hit_t`;
- metric handling for IP / L2 / cosine;
- visited tag lifecycle and overflow reset;
- deleted/tombstone skip;
- deterministic tie-breaking where practical;
- export only `record_id + score`.

Search must not:

- call manager;
- inspect scope / TTL / metadata;
- assemble response;
- allocate in the hot path after context acquisition.

### 10.5 Delete/rebuild policy

v2 delete policy:

- `mark_delete(record_id)` only.
- Look up v2-local slot from `record_id_to_slot`.
- Set v2-local tombstone/deleted flag.
- Search must not return deleted slots.
- No online graph compaction in the first production candidate.

Rebuild policy:

- manager decides when to rebuild.
- tombstone ratio threshold is manager/rebuild policy, not graph persistence.
- rebuild calls reset/reserve/add for active records.
- RDB/AOF remain record-only.

### 10.6 Context and memory policy

Hot path constraints:

- No malloc/free in search hot path.
- Reusable search context.
- Candidate heap, top heap, visited bookkeeping, result scratch, and neighbor buffers are preallocated.
- Context pool can remain single-thread friendly initially, but the concurrency boundary must be explicit.

Future concurrency boundary:

- v2 index object is not implicitly thread-safe unless designed and documented.
- Concurrent reads need a clear context acquisition strategy and mutation exclusion.
- VSET/VDEL/rebuild concurrency with search should be treated as a later product hardening issue, not assumed.

## 11. v2 correctness / recall / performance / stability gates

### 11.1 Correctness gates

Required:

- Search results compare against `core_flat` exact ground truth on shared float32 datasets.
- `record_id -> slot -> record_id` mapping is checkable.
- Duplicate `record_id` behavior has explicit tests.
- Deleted record must never be returned.
- Rebuild from active records produces stable search behavior.
- Score ordering is descending for exported hits.
- Metric polarity is consistent with existing KVLite score convention.
- `ef_search` smaller than topK is clamped or rejected consistently.

### 11.2 Recall gates

Minimum gates:

- 1K / 512d recall@10 >= 0.99.
- 10K / 512d recall@10 >= 0.95.
- 10K / 512d target recall@10 >= 0.98.
- `ef_search` increase should be monotonic non-decreasing in recall for the same graph and dataset; any violation requires miss-case analysis and is treated as a bug until proven otherwise.
- `ef_construction` increase should improve or hold graph quality for the same dataset and search parameters; regressions require analysis.

Do not start Stage 9E performance pass until recall gates pass.

### 11.3 Performance gates

First v2 implementation does not need to beat FAISS/hnswlib.

It must report:

- index-only build time;
- index-only search latency p50/p95/p99;
- recall@10;
- memory bytes and source;
- relative build/search factor versus FAISS/hnswlib on the same dataset and parameters.

Acceptance expectation:

- v2 should not be an order of magnitude slower than baselines once recall gates pass.
- If it is, Stage 9E must attribute the gap before optimizing.

### 11.4 Stability gates

Before VSEARCH integration:

- v2 index-only create/reserve/reset/destroy/info tests pass.
- add/search/delete/rebuild tests pass without manager semantics.
- ASAN/valgrind-style memory checks should be considered if available.

Before product use:

- VSET/VSEARCH/VDEL/VINDEX/VINFO behavior remains compatible.
- RDB load rebuild works.
- AOF replay rebuild works.
- graph remains derived runtime state.
- vector_core still does not handle manager semantics.
- rollback to `core_flat` or current `core_hnsw` remains available.

## 12. v2 relationship to other backends

`core_flat`:

- Exact recall baseline.
- Correctness oracle.
- Small-N fallback.
- Not a FAISS IndexFlat parity target.

current `core_hnsw`:

- Legacy backend.
- Regression comparison.
- Development fallback.
- No longer main optimization target.

`core_hnsw_v2`:

- New primary self-contained ANN candidate.
- Starts index-only.
- Must pass recall gates before product integration.
- Must remain manager-independent inside vector_core.

FAISS / hnswlib:

- Baseline only.
- Used to compare recall, graph quality, parameters, build/search latency, and memory.
- Not copied as vector_core architecture.
- Existing adapter manager/global-state debt remains isolated adapter debt.

## 13. Coexistence and rollback strategy

Coexistence rules:

- Keep current `core_hnsw`.
- Add v2 under a separate directory and independent backend label, for example `core_hnsw_v2`, only when integration is explicitly scoped.
- Stage 9B/9C/9D should be index-only and not affect VINDEX default.
- v2 must not become default before recall and stability gates pass.
- Keep FAISS/hnswlib baseline benchmark available.
- Do not remove current tests for core_flat/current core_hnsw.

Rollback:

- If v2 fails index-only gates, stop before integration.
- If integrated but fails product hardening, switch config / VINDEX back to `core_flat` or current `core_hnsw`.
- Since graph is derived state, rollback does not require RDB/AOF migration.

## 14. Phased implementation plan

### Stage 9B: core_hnsw_v2 index-only skeleton

Goal:

- Create independent v2 index-only skeleton.

Allowed:

- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_storage.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_context.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_info.c`
- focused index-only tests

Implement:

- init / reserve / reset / destroy / info;
- vector storage allocation and alignment policy;
- id-slot map ownership;
- graph adjacency memory placeholders;
- visited tags allocation;
- search context allocation;
- memory accounting.

Do not implement add/search yet.

### Stage 9C: core_hnsw_v2 graph construction prototype

Implement:

- random level;
- add;
- graph adjacency;
- entry point / max level;
- neighbor selection;
- bidirectional link;
- reverse edge pruning;
- graph quality stats.

Gate:

- structural graph tests;
- duplicate edge tests;
- degree distribution checks;
- deterministic rebuild sanity.

### Stage 9D: core_hnsw_v2 search and recall gate

Implement:

- search;
- exact recall comparison via core_flat;
- ef_search sensitivity;
- ef_construction sensitivity;
- miss-case analysis output.

Gate:

- 1K / 512d recall@10 >= 0.99;
- 10K / 512d recall@10 >= 0.95, target >= 0.98;
- deleted slot not returned;
- monotonic ef tests.

### Stage 9E: core_hnsw_v2 performance pass

Only after recall gates pass.

Candidate areas:

- distance backend;
- vector alignment and SIMD call path;
- heap/context optimization;
- visited tags;
- memory layout;
- bulk rebuild.

Rule:

- Profile first, then optimize one bottleneck class per stage.

### Stage 9F: core_hnsw_v2 integration

Implement:

- connect v2 behind existing `kvs_vector_index_ops_t` wrapper without changing signatures;
- VINDEX selection by explicit label;
- VINFO fields;
- manager rebuild path.

Do not make v2 default in 9F.

### Stage 9G: product hardening

Validate:

- VSET / VSEARCH / VDEL / VINDEX / VINFO;
- RDB load + rebuild;
- AOF replay + rebuild;
- tombstone rebuild policy;
- stress tests;
- rollback to `core_flat` or current `core_hnsw`.

Default switch, if any, belongs after 9G data.

## 15. Stage 9B prompt candidate

Candidate next prompt:

```text
先读取并遵守项目根目录 AGENTS.md。若本 prompt 与 AGENTS.md 冲突，以 AGENTS.md 为准。

# Stage 9B: core_hnsw_v2 index-only skeleton

路线依据：docs/vector_core_reset/stage9a_core_hnsw_v2_design_boundary.md

目标：新增独立 core_hnsw_v2 index-only skeleton，只实现 init/reserve/reset/destroy/info、vector storage、id-slot map、graph adjacency memory placeholders、visited tags、search context pool 和 memory accounting。不实现 add/search，不接入 VINDEX/VSEARCH，不修改旧 core_hnsw。

范围：
* include/vector/kvs_vector_hnsw_v2.h
* src/vector/core/hnsw_v2/*
* tests/vector_core_hnsw_v2_skeleton_test.sh 或等价 index-only test
* docs/vector_core_reset/stage9b_core_hnsw_v2_index_only_skeleton.md

禁止：
* 不改 RDB/AOF
* 不改 kvs_vector_index_ops_t
* 不改 VSEARCH response
* 不接入默认 backend
* 不实现 add/search
* 不修改 current core_hnsw 算法
* 不访问 g_vector_state / scope / TTL / metadata / question / answer
* 不删除 current core_hnsw / core_flat / FAISS / hnswlib
* 不处理无关 README.md 修改

必跑：
git status --short
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_core_hnsw_v2_skeleton_test.sh
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\\|ttl\\|metadata\\|question\\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
git diff --check
git status --short
```

## 16. Safety invariants

- Manager/core boundary unchanged.
- Public V command behavior unchanged because no production code changed.
- VSEARCH response unchanged.
- RDB/AOF compatibility unchanged.
- `kvs_vector_index_ops_t` unchanged.
- current `core_hnsw` remains available.
- FAISS / hnswlib adapters remain available as baselines.
- v2 design does not require moving manager semantics into vector_core.
- v2 design does not require graph persistence.

## 17. Correctness tests

No correctness tests were required by this design-only stage. Existing Stage 8A / Stage 7 reports already record passing core_flat/core_hnsw correctness, rebuild, RDB reload, AOF replay, and rebuild profile tests for the current code state.

For this stage, the relevant validation is that no production code changed and static boundary checks remain clean.

## 18. Benchmark result

No new benchmark was required or run for Stage 9A because this is design/audit only.

Key prior data used:

| Source | Data point | Interpretation |
| --- | --- | --- |
| Stage 7E | level0 worst-neighbor recompute dropped to zero for tested high-dim runs | Local neighbor_select recompute hotspot solved |
| Stage 7G | `layer_search_level0_us` dropped 70%-80% without reducing distance count | Distance/SIMD/call overhead hotspot solved |
| Stage 7K | failed A/B layout prototype memory removed | Layout prototypes did not justify refactor |
| Stage 7L | repeated skip ratio about 92.36%-92.55% at 1K high-dim | Remaining current-core cost is graph/search behavior, not one recompute bug |
| Stage 8A | 10K / 512d direct-vector current `core_hnsw` recall@10 = 0.6240 | current `core_hnsw` graph quality / scaling risk justifies v2 route |

## 19. Static boundary check result

Commands to run for this stage:

```bash
grep -RIn "g_vector_state" src/vector/core || true
grep -RIn "record_id_map" src/vector/core || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core || true
git diff --check
```

Result:

- `g_vector_state` grep: no output.
- `record_id_map` grep: no output.
- scope / ttl / metadata / question / answer grep: no output.
- `git diff --check`: passed.

## 20. Build result

Recommended command:

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

Result: passed.

Build output:

```text
✓ 构建完成
二进制: kvstore src/eBPF/ebpf_syncd
eBPF:   src/eBPF/ebpf_kern.o
```

## 21. git status --short

Initial status before this report:

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
```

Final status after adding this report and running validation:

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
```

## 22. Next suggested step

Proceed to Stage 9B only if the product direction is to continue self-contained KVLite HNSW work.

Stage 9B should be limited to an independent `core_hnsw_v2` index-only skeleton. It must not implement add/search, must not integrate with VSEARCH/VINDEX, and must not modify current `core_hnsw`.

If the product direction changes back to default-backend selection rather than self-HNSW v2, run the Stage 8B fairness / FAISS adapter audit first. Do not mix Stage 8B backend decision work with Stage 9B v2 skeleton work in the same stage.
