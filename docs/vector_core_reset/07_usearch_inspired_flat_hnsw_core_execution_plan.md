# KVLite USearch-inspired Flat/HNSW Vector Core Execution Plan

## 1. Executive Summary

This document records a design audit and execution plan for resetting the KVLite vector module boundary around a KVmanager layer and a USearch-inspired `vector_core`.

The target direction is:

- KVLite remains the local vector storage layer for industrial software Agents.
- KVmanager owns product semantics: V-family commands, records, key/scope, TTL-facing fields, metadata, persistence, replication, rebuild orchestration, embedding pipeline, and Agent/RAG integration.
- `vector_core` owns high-performance vector computation: exact flat, HNSW, distance kernels, id/slot mapping, search context, heaps, visited state, and memory accounting.
- Record store is the source of truth.
- Flat storage and HNSW graph are derived state and must be rebuildable from records.
- The near-term index set is only `exact_flat` and `hnsw`.

This plan does not propose making KVLite a Faiss, hnswlib, or USearch wrapper. Faiss is used as an abstraction reference, hnswlib as the current minimum viable / legacy backend reference, and USearch as a design reference for data layout and hot-path discipline.

## 2. Current Code Findings

Audited paths:

- `third_party/faiss/`
- `third_party/hnswlib/`
- `third_party/USearch-2.25.3/`
- `src/vector/`
- `include/`
- `src/core/`
- `src/embedding/`
- `src/persistence/`

Current vector-related files are flat under `src/vector/`:

```text
src/vector/kvs_vector.c
src/vector/kvs_vector_distance.c
src/vector/kvs_vector_distance_avx2.c
src/vector/kvs_vector_embedding.c
src/vector/kvs_vector_faiss_backend.cpp
src/vector/kvs_vector_faiss_ops.c
src/vector/kvs_vector_hnswlib_backend.cpp
src/vector/kvs_vector_hnswlib_ops.c
src/vector/kvs_vector_index.c
```

Findings against the requested boundary questions:

1. Current index ops still return manager results. Confirmed in `include/kvs_vector.h`: `kvs_vector_index_ops_t.search` returns `kvs_vector_search_result_t *results`, and `kvs_vector_index_search_params_t` contains `scope`, `scope_len`, `topk`, and `threshold`.
2. Current backend/adapter directly accesses `g_vector_state.store`. Confirmed in `src/vector/kvs_vector_hnswlib_ops.c`, where search checks `g_vector_state.store.capacity`, reads `g_vector_state.store.records[record_index]`, and exports results through `kvs_vector_topk_heap_export_desc(..., &g_vector_state.store)`. Faiss ops follow the same pattern in `src/vector/kvs_vector_faiss_ops.c`.
3. Current backend label uses `record_index`, not stable `record_id`. Confirmed in `include/kvs_vector_faiss_backend.h`, where `record_index` is documented as the Faiss id, and in `include/kvs_vector_hnswlib_backend.h`, where add takes `size_t record_index`. The hnswlib adapter calls `kvs_hnswlib_index_add(..., record_index, ...)`.
4. Current flat / hnsw / faiss / hnswlib boundaries are mixed in the flat `src/vector/` directory. Confirmed by file layout above.
5. Current `include/kvs_vector.h` exposes public API, record/store/index/backend-like internals, and internal prototypes. Confirmed by the same header containing public `kvs_vector_set/search/info`, `kvs_vector_record_t`, `kvs_vector_store_t`, `kvs_vector_flat_index_t`, `kvs_vector_ann_index_t`, `kvs_vector_index_ops_t`, `g_vector_state`, and many internal function prototypes under `KVS_VECTOR_INTERNAL`.
6. Current scope/deleted filtering appears in backend adapter code. Confirmed in `src/vector/kvs_vector_hnswlib_ops.c`: search filters by score threshold, store capacity, record active/deleted state, and `kvs_vector_record_scope_match`. TTL filtering was not confirmed in the backend adapter code. Metadata filtering was not confirmed in the backend adapter code.
7. Current exact flat and HNSW data structures partially support a future USearch-style core, but not cleanly. Confirmed in `include/kvs_vector.h`: flat has contiguous `vectors`, `record_indices`, and `record_ids`; ANN node has `record_index`, `record_id`, per-node neighbor arrays, and index-level entry/max level fields. Missing or insufficient for the target core: stable `record_id -> slot` as the primary label boundary, standalone search context pool, visited tags, no-malloc search contract, compact HNSW tape layout, and backend search returning only `record_id + score`.

Persistence findings:

- `src/persistence/kvs_rdb.c` saves/loads a vector section with metric, index type, next id, records, embeddings, TTL-facing fields, generation, and `embedding_model_version`, then calls `kvs_vector_rebuild_after_load()`. This supports the source-of-truth model.
- `src/persistence/kvs_aof_rewrite.c` rewrites active vector records as `VSET question answer [scope]`, so AOF replay follows command semantics and may regenerate embeddings.

Command and execution findings:

- `src/core/kvs_operation.c` registers `VSET`, `VGET`, `VSEARCH`, `VDEL`, `VINDEX`, and `VINFO`.
- `src/core/kvs_command_task.c` contains prepared VSET/VSEARCH paths.
- `src/core/kvs_command_executor.c` contains vector stats, snapshot builder paths, and replication-related execution timing hooks. A dedicated `src/replication/` directory was not found in the audited tree.

## 3. Design Goal

The design goal is to make `vector_core` a narrow compute/storage component that can be swapped without changing product semantics.

The non-negotiable boundary is:

```text
KVmanager input: VSET/VSEARCH/VDEL/VINFO protocol and records.
KVmanager output to core: record_id + vector.
vector_core output to manager: record_id + score.
Manager then resolves record_id to record and applies product filtering.
```

This avoids current leakage where backend adapters see `question`, `answer`, `scope`, `deleted`, and store internals.

## 4. KVmanager / vector_core Boundary

KVmanager is responsible for:

- `VSET / VGET / VSEARCH / VDEL / VINDEX / VINFO`
- record lifecycle
- `key/scope -> record_id`
- `record_id -> record`
- `question`
- `answer`
- `normalized_question`
- `embedding`
- metadata
- `scope / project / collection`
- TTL-facing fields
- `embedding_model_version`
- `generation`
- candidate filtering: scope, TTL, deleted, threshold
- AOF/RDB record boundary
- replication record boundary
- startup rebuild index
- backend lifecycle orchestration
- Agent/RAG integration boundary

`vector_core` is responsible for:

- capacity
- reserve
- size
- dim
- metric
- `add(record_id, vector)`
- `search(query) -> record_id + score`
- `mark_delete(record_id)`
- reset
- info
- destroy
- `record_id -> slot`
- `slot -> record_id`
- `slot -> vector`
- HNSW graph
- visited list / visited tag
- candidate heap
- topK heap
- search context / scratch buffer
- distance hot path
- memory usage

`vector_core` must not own or interpret:

- question / answer
- scope
- TTL
- metadata
- AOF/RDB record encoding
- replication command encoding
- Agent semantics
- VSET/VSEARCH text protocol

## 5. Faiss-inspired Unified Index Ops

Faiss design references:

- `faiss::Index` provides a common lifecycle with dimension, `ntotal`, metric, add, search, reset, and optional remove.
- `IndexFlat` stores full vectors and provides the exact baseline.
- `IndexHNSW` separates graph (`HNSW hnsw`) from sequential storage (`Index *storage`).
- `IndexIDMap` and `IndexIDMap2` show the value of external id mapping on top of internal storage.

KVLite should not import Faiss complexity in the near term:

- IVF
- PQ
- SQ
- OPQ
- GPU
- training
- `index_io` as KVLite persistence format

Recommended KVLite index ops:

```c
typedef struct kvs_vector_hit_s {
    uint64_t record_id;
    float score;
} kvs_vector_hit_t;

typedef struct kvs_vector_index_info_s {
    size_t capacity;
    size_t size;
    size_t dim;
    int metric;
    size_t memory_bytes;
    int ready;
    int needs_rebuild;
} kvs_vector_index_info_t;

typedef struct kvs_vector_index_ops_s {
    int (*reserve)(void *index, size_t capacity);
    int (*add)(void *index, uint64_t record_id, const float *vector, size_t dim);
    int (*search)(void *index, const float *query, size_t dim,
                  size_t topk, kvs_vector_hit_t *out, size_t *out_count);
    int (*mark_delete)(void *index, uint64_t record_id);
    int (*reset)(void *index);
    int (*info)(void *index, kvs_vector_index_info_t *out);
    void (*destroy)(void *index);
} kvs_vector_index_ops_t;
```

Recommended adjustments based on current code:

- Add `metric` and `dim` to index create/config, not every hot-path call, but keep `dim` validation in `add/search`.
- Keep `threshold` outside core. Core returns top candidates; manager applies threshold after resolving record state.
- Keep `scope` outside core. Scope filtering belongs to manager.
- Add optional `kvs_vector_search_context_t *ctx` later if per-thread/pool ownership needs explicit API control. Stage 1 can keep context internal to index.
- Add `deleted_count` or `tombstone_count` to info if it is needed for VINFO and rebuild heuristics.

## 6. USearch-inspired Core Principles

USearch references observed in `third_party/USearch-2.25.3/`:

- C API exposes `usearch_init`, `usearch_reserve`, `usearch_size`, `usearch_capacity`, `usearch_add`, `usearch_search`, `usearch_remove`, `usearch_save/load/view`, and `usearch_memory_usage`.
- Dense index internals keep key/slot structures: `slot_lookup_`, `vectors_lookup_`, `free_keys_`, and typed graph slots.
- Add/search paths require reserved capacity and thread resources; failures explicitly say to reserve capacity ahead of insertions/searches.
- Search uses context buffers, candidate/top heaps, graph slots, and metric proxies.
- Graph internals use slots, node levels, tape allocator, compact neighbor storage, entry slot, max level, and memory usage reporting.

Designs to migrate into KVLite-owned `vector_core`:

1. `record_id -> slot` mapping.
2. `slot -> record_id` mapping.
3. Contiguous vector storage.
4. `reserve/capacity` before high-throughput add/search.
5. Search context pooling.
6. `visited_tags` to avoid full memset.
7. Custom lightweight candidate heap and top heap.
8. `memory_usage/info`.
9. Distance function abstraction.
10. Graph/tape compact layout principles.

Designs not to introduce in Stage 1:

1. quantization.
2. multi-key.
3. mmap view as source of truth.
4. save/load graph as KVLite persistence truth.
5. language bindings.
6. physical compact.
7. online graph compaction.
8. cluster/join/high-level vector database features.

## 7. core/flat Design

`exact_flat` is the ground truth index and recall baseline. It must return `record_id + score`; manager performs record lookup and filtering.

Minimal data structure:

```c
typedef struct kvs_flat_index_s {
    size_t dim;
    int metric;
    size_t capacity;
    size_t size;

    float *vectors;              /* capacity * dim */
    uint64_t *slot_to_record_id;  /* capacity */
    uint8_t *deleted_flags;       /* capacity */

    kvs_vector_id_slot_map_t id_to_slot;
    kvs_vector_distance_fn distance;

    kvs_flat_search_context_pool_t search_contexts;

    int ready;
    int needs_rebuild;
} kvs_flat_index_t;
```

Search context:

```c
typedef struct kvs_flat_search_context_s {
    kvs_vector_hit_t *hits;
    size_t hits_capacity;
    kvs_vector_heap_t top_heap;
} kvs_flat_search_context_t;
```

Requirements:

- `vectors` is a contiguous `float` array.
- `record_id` is the external label; slot is internal.
- Deleted entries are skipped by core only for index-level deletion; product-level deleted/TTL/scope filtering remains in manager.
- No result should contain `question`, `answer`, or `scope`.
- `memory_usage` includes vectors, id maps, flags, and search context buffers.
- Faiss flat remains a benchmark comparison only, not source of truth.

Flat operation behavior:

- `reserve(capacity)` allocates vectors, slot map, flags, and any fixed context buffers.
- `add(record_id, vector)` inserts or updates `id_to_slot`; update overwrites vector in-place.
- `mark_delete(record_id)` marks the slot deleted and removes or tombstones id mapping.
- `reset()` clears size and mappings but may retain capacity if the implementation chooses.
- `search(query, topk)` scans active slots, computes metric, maintains a top heap, and exports hits sorted by score.

## 8. core/hnsw Design

The HNSW core should be USearch-inspired but KVLite-owned. It should use slots as graph node ids and expose only `record_id + score`.

Minimal data structure:

```c
typedef struct kvs_hnsw_index_s {
    size_t dim;
    int metric;

    size_t M;
    size_t M0;
    size_t ef_construction;
    size_t ef_search;

    size_t capacity;
    size_t size;

    float *vectors;              /* capacity * dim */
    uint8_t *levels;             /* capacity */
    uint8_t *deleted_flags;      /* capacity */

    uint64_t *slot_to_record_id; /* capacity */
    kvs_vector_id_slot_map_t record_id_to_slot;

    uint32_t *level0_neighbors;  /* capacity * M0 */
    uint16_t *level0_counts;     /* capacity */

    size_t *upper_offsets;
    uint32_t *upper_neighbors;
    uint16_t *upper_counts;

    uint32_t entry_point;
    int has_entry;
    uint8_t max_level;
    kvs_hnsw_random_t rng;

    uint32_t *visited_tags;      /* capacity */
    uint32_t current_visit_tag;

    kvs_hnsw_search_context_pool_t search_context_pool;
    kvs_vector_distance_fn distance;

    int ready;
    int needs_rebuild;
} kvs_hnsw_index_t;
```

Search context:

```c
typedef struct kvs_hnsw_search_context_s {
    kvs_hnsw_heap_t candidate_heap;
    kvs_hnsw_heap_t top_heap;
    uint32_t *neighbor_buffer;
    float *distance_buffer;
    kvs_vector_hit_t *hits;
    size_t hits_capacity;
} kvs_hnsw_search_context_t;
```

Hard hot-path rules:

- Search hot path must not call `malloc/free`.
- Search must not full-memset visited state.
- Search should use `visited_tags[slot] == current_visit_tag`, with tag rollover handled by a rare full reset.
- Search must not use `std::priority_queue` as the long-term hot-path structure.
- Graph search must not call back into manager.
- HNSW search returns only `record_id + score`.
- Scope, TTL, product-level deleted state, metadata, and threshold filtering are manager responsibilities.
- Delete in the first phase is `mark_delete` only.
- Rebuild is triggered by manager from record store.

Graph layout recommendation:

- Level 0 uses fixed-width arrays: `level0_neighbors[slot * M0 + i]`.
- Upper levels use a compact tape: `upper_offsets`, `upper_neighbors`, and `upper_counts`, or an equivalent compact contiguous layout.
- Use 32-bit slots internally if capacity limits permit; keep `record_id` 64-bit externally.
- Keep graph node slot stable during normal add/delete; physical compaction is not a Stage 1 feature.

Add behavior:

- `reserve` must preallocate vectors, levels, id maps, visited tags, level0 neighbors, and context pool capacity.
- `add(record_id, vector)` chooses/reuses a slot, stores vector, records id maps, generates random level, and links neighbors.
- Neighbor selection stays fully inside HNSW core.
- `ef_construction` affects graph construction only.

Search behavior:

- Start from `entry_point` at `max_level`.
- Greedy-search upper layers.
- Search level 0 with `ef_search`.
- Use candidate heap and top heap from search context.
- Export top hits as `record_id + score`.

## 9. Recommended Directory Reorganization

Target structure:

```text
src/vector/
  manager/
    kvs_vector_manager.c
    kvs_vector_record_store.c
    kvs_vector_key_map.c
    kvs_vector_scope.c
    kvs_vector_prepare.c
    kvs_vector_rebuild.c
    kvs_vector_info.c
    kvs_vector_persistence_bridge.c
    kvs_vector_replication_bridge.c

  core/
    kvs_vector_core.c
    kvs_vector_index_ops.c
    kvs_vector_distance.c
    kvs_vector_distance_avx2.c
    kvs_vector_id_slot_map.c
    kvs_vector_search_context.c
    kvs_vector_heap.c

  core/flat/
    kvs_flat_index.c
    kvs_flat_storage.c
    kvs_flat_search.c
    kvs_flat_info.c

  core/hnsw/
    kvs_hnsw_index.c
    kvs_hnsw_graph.c
    kvs_hnsw_add.c
    kvs_hnsw_search.c
    kvs_hnsw_neighbors.c
    kvs_hnsw_context.c
    kvs_hnsw_id_slot_map.c
    kvs_hnsw_heap.c
    kvs_hnsw_random.c
    kvs_hnsw_info.c

  backends/
    faiss/
      kvs_vector_faiss_backend.cpp
      kvs_vector_faiss_ops.c
    hnswlib/
      kvs_vector_hnswlib_backend.cpp
      kvs_vector_hnswlib_ops.c
    usearch/
      optional_adapter_only.md

  embedding/
    kvs_vector_embedding.c

  protocol/
    kvs_vector_command_handler.c
    kvs_vector_response.c
```

More conservative migration for this repository:

- First split headers and logical ownership without immediately moving every `.c/.cpp` file.
- Keep `src/core/kvs_operation.c` command registration in place during Stage 1 to avoid broad command-layer churn.
- Move backend adapter files only after the new index ops boundary is stable.
- Treat `protocol/` as optional until V command handlers are small enough to extract safely from `src/core/kvs_operation.c`.

Recommended headers:

```text
include/kvs_vector.h                 public API only
include/kvs_vector_manager.h         manager internal API
include/kvs_vector_record.h          record/store/key map
include/kvs_vector_core.h            core index ops and hit/info types
include/kvs_vector_flat.h            flat core internals
include/kvs_vector_hnsw.h            hnsw core internals
include/kvs_vector_backend_faiss.h   benchmark adapter ABI
include/kvs_vector_backend_hnswlib.h legacy adapter ABI
```

## 10. Migration Plan

### Stage 1: Boundary Refactor, No Behavior Change

Scope:

1. Split `include/kvs_vector.h` into public and internal headers.
2. Introduce manager/core headers.
3. Narrow index ops to `record_id + vector` input and `record_id + score` output.
4. Keep adapter compatibility layers so existing VSET/VSEARCH/VDEL/VINFO behavior remains unchanged.
5. Keep current hnswlib/Faiss backend behavior while hiding manager records from backend ops.
6. Do not implement a new HNSW.
7. Do not change persistence format.

Required compatibility work:

- Map current `record_index` labels to stable `record_id`.
- Manager owns `record_id -> record`.
- Adapter outputs hits; manager converts hits to `kvs_vector_search_result_t`.
- Keep existing test expectations for scope/deleted filtering, but move filtering out of backend adapters.

### Stage 2: Implement `core/flat`

Scope:

1. Create `src/vector/core/flat`.
2. Implement contiguous vector storage.
3. Implement `record_id -> slot` and `slot -> record_id`.
4. Implement exact search with manager-side filtering.
5. Use exact flat as recall ground truth.
6. Use Faiss flat only as benchmark comparison.
7. Add VSEARCH selection for flat core.
8. Add VINFO memory/core stats.

Acceptance for Stage 2:

- exact flat returns the same visible search results as the old flat path after manager filtering.
- exact flat can be used as recall baseline for HNSW.
- No backend adapter sees `question`, `answer`, `scope`, TTL, or metadata.

### Stage 3: Implement `core/hnsw`

Scope:

1. Create `src/vector/core/hnsw`.
2. Implement USearch-inspired HNSW data structures.
3. Implement `reserve/add/search/mark_delete/reset/info`.
4. Ensure manager only calls through index ops.
5. Rebuild graph from record store on startup/load/rebuild.
6. Benchmark against hnswlib, Faiss, and optionally USearch adapter.

Acceptance for Stage 3:

- Search hot path has no allocation.
- Visited handling avoids full memset.
- HNSW search returns only `record_id + score`.
- Recall@10 is measurable against exact flat.
- VINFO exposes core memory and graph stats.

## 11. Benchmark and Validation Plan

Dataset tiers:

```text
10K / 64d
100K / 64d
Optional: production-like embedding dimension from current embedding provider
```

Performance goals:

```text
10K / 64d:
  exact_flat search is the ground truth.
  hnsw search is clearly faster than exact_flat.
  hnsw add/search should not show order-of-magnitude regressions versus current baselines.

100K / 64d:
  hnsw search should not timeout.
  hnsw latency should be stable.
  memory_usage should be observable.
  recall@10 should be compared with hnswlib/Faiss baselines.

USearch comparison:
  Stage 1 target is not to beat USearch.
  Target is to remove obvious structural inefficiencies:
    search malloc
    full memset visited
    vector<vector<int>>-style graph storage
    std::priority_queue hot path
    manager callbacks during graph search
```

Benchmark items:

- exact_flat add/search
- hnsw add/search
- hnsw recall@10 against exact_flat
- hnsw vs hnswlib_hnsw
- hnsw vs faiss_hnsw
- optional hnsw vs usearch_hnsw adapter
- VSET/VSEARCH end-to-end
- VINFO memory_usage/core stats

Validation items:

- VSET inserts new record.
- VSET updates existing key/scope without duplicate visible records.
- VSEARCH returns same manager-visible fields as before.
- VDEL hides records from search.
- VINDEX rebuild reconstructs derived index from record store.
- RDB load restores records and rebuilds index.
- AOF rewrite still emits valid VSET sequence.
- VINFO distinguishes manager stats, core stats, and backend stats.

## 12. Risks

- Migrating backend label from `record_index` to `record_id` can expose assumptions in RDB restore, free slot reuse, and current adapter search result paths.
- Moving filtering from adapter to manager can change result ordering if overfetch is insufficient. Manager may need `topk * overfetch_factor` or two-phase filtering.
- AOF rewrite currently persists VSET command semantics, while RDB persists embeddings. This split must remain explicit.
- Existing SQ8 flat code is present in `include/kvs_vector.h` and `src/vector/kvs_vector_index.c`; near-term work should not expand it while the target index set is exact flat + HNSW.
- Search context pooling introduces lifecycle and concurrency requirements that need focused tests.
- Compact HNSW layout is harder to debug than pointer-heavy node arrays; Stage 3 should start with readable structures and tighten only after correctness is stable.
- `src/core/kvs_command_executor.c` currently reaches into vector state for snapshot building; extracting persistence/replication bridges must be done carefully.

## 13. Non-goals

- Do not modify production code in this planning round.
- Do not move source files in this planning round.
- Do not implement new features in this planning round.
- Do not make KVLite a USearch wrapper.
- Do not make KVLite a Faiss wrapper.
- Do not continue hnswlib internal performance attribution.
- Do not introduce IVF, PQ, SQ, OPQ, GPU, training, or index_io persistence.
- Do not use mmap view as source of truth.
- Do not introduce language bindings, cluster, join, or vector database feature expansion.
- Do not make graph persistence the source of truth; record store remains the source of truth.

## 14. Acceptance Criteria

For this execution plan:

- The current code findings identify concrete files and functions/structures.
- The target boundary states that index ops only process `record_id`, vector, and score.
- The document clearly separates KVmanager and vector_core responsibilities.
- The document gives minimal `core/flat` and `core/hnsw` data structures.
- The document keeps near-term scope to `exact_flat` and `hnsw`.
- The document includes a three-stage migration plan.
- The document includes benchmark and validation plans.

For future implementation:

- `include/kvs_vector.h` is public-only.
- Manager owns product semantics and record store.
- Core owns vector compute and index state only.
- Backend adapters do not directly access `g_vector_state.store`.
- Backend/core labels are stable `record_id`, not `record_index`.
- Search hot path avoids malloc/free.
- HNSW visited state avoids full memset.
- VSET/VSEARCH/VDEL/VINDEX/VINFO behavior remains compatible unless deliberately changed and documented.

## 15. Git Status and File-change Confirmation

Executed after writing this document:

```bash
git status --short
?? docs/vector_core_reset/06_performance_first_self_hnsw_design.md
?? docs/vector_core_reset/07_usearch_inspired_flat_hnsw_core_execution_plan.md
```

本轮只新增/更新 docs/vector_core_reset/07_usearch_inspired_flat_hnsw_core_execution_plan.md。
没有修改生产代码。

The untracked `docs/vector_core_reset/06_performance_first_self_hnsw_design.md` entry was already present before this round's file write and was not produced or modified by this round.
