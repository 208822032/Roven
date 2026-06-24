# Vector Compute Kernel Design

Date: 2026-06-24

## 1. Purpose

The Vector Compute Kernel is the backend-facing vector computation layer below the Vector Manager Kernel.

It owns only derived compute structures:

- flat arrays
- HNSW graphs
- visited/search scratch buffers
- candidate queues
- optional backend snapshots

It does not own KVLite storage semantics.

Core rule:

```text
Compute kernels receive record ids and vectors.
Compute kernels return record ids and scores.
KVLite Manager owns records, metadata, TTL, AOF/RDB, and replication.
```

## 2. Minimal Contract

Required ops:

```text
init(config, allocator, logger) -> handle
destroy(handle)
reserve(handle, capacity) -> status
capacity(handle) -> size
size(handle) -> size
add(handle, record_id, vector, dim) -> status
search(handle, query, dim, topk, params, out) -> status
info(handle, out_info) -> status
reset(handle) -> status
```

Optional ops:

```text
mark_delete(handle, record_id) -> status
delete(handle, record_id) -> status
replace(handle, record_id, vector, dim) -> status
save_snapshot(handle, writer) -> status
load_snapshot(handle, reader) -> status
compact(handle) -> status
```

Unsupported optional ops must return `KVS_VECTOR_ERR_NOT_IMPLEMENTED`, not silently fake success.

## 3. Data Types

Backend config:

```text
name
kind: flat | hnsw | sq8_flat
implementation: self_hnsw | hnswlib_hnsw | usearch_hnsw | faiss_hnsw | faiss_flat
dimension
metric
initial_capacity
M/connectivity
ef_construction/expansion_add
ef_search/expansion_search
allow_replace_deleted
thread limits
```

Search params:

```text
topk
overfetch
threshold
ef_search_override, optional
filter_mode, optional
```

Search result:

```text
record_id
score
```

Info:

```text
size
capacity
dimension
metric
kind
implementation
M/connectivity
ef_construction/expansion_add
ef_search/expansion_search
deleted_count
memory_bytes
add_count
search_count
last_error
```

## 4. ID Semantics

The compute kernel treats ids as opaque `uint64_t` or `size_t` record ids.

It must not:

- parse question/scope keys;
- keep question/answer strings;
- decide TTL visibility;
- decide scope visibility;
- write AOF/RDB;
- replicate operations.

For backends with internal ids:

- The adapter owns internal-id translation.
- The manager-visible id remains the KVLite record id.
- The adapter must prevent stale internal ids from leaking upward.

## 5. Memory and Capacity

The kernel must expose reserve/capacity explicitly.

Rationale from audited engines:

- hnswlib has `max_elements_` and `resizeIndex`.
- USearch has `try_reserve`, `capacity`, and `index_limits_t`.
- Faiss is less uniform, but KVLite should keep the contract stable above Faiss.

Rules:

- Manager may call `reserve` before bulk rebuild or before add when capacity is insufficient.
- Backend may grow automatically, but must update `capacity`.
- Failed reserve must leave the old index usable.
- `capacity >= size` must hold.

## 6. Graph Neighbor Layout

For future `self_hnsw`, prefer a compact layout:

```text
nodes[]:
  record_id
  level
  vector_ref or vector_offset
  neighbor_offset

neighbors[]:
  per-node/per-level packed neighbor ids
```

Candidate layouts:

- Faiss-style: `levels`, `offsets`, one `neighbors` array.
- USearch-style: per-node tape with count plus neighbor slots.
- hnswlib-style: level-0 inline with vector data plus upper-level pointer arrays.

Recommendation:

- Do not mix KVLite vector record memory with graph link memory.
- Store graph links as derived compute state.
- Keep vectors in KVLite records or in a backend-owned copy/cache that can be rebuilt.
- Prefer compact contiguous neighbor storage for cache locality and easier snapshotting.

## 7. Visited Set and Search Context

Self-owned HNSW should include reusable search contexts:

```text
visited_set
candidate_queue
topk_heap
scratch_neighbors
stats counters
```

Borrowed ideas:

- hnswlib `VisitedListPool`: tag arrays avoid clearing `O(n)` memory per search.
- USearch contexts: reusable candidate buffers avoid repeated allocations.
- Faiss `VisitedTable` and result handlers: isolate traversal bookkeeping from output formatting.

Rules:

- No heap allocation in the hot search loop after context initialization.
- Contexts should be per-thread or pooled.
- Context acquisition/release must be outside graph traversal where possible.

## 8. Candidate Queue and TopK Heap

HNSW needs two logical queues:

- exploration candidates ordered by current best distance;
- accepted top candidates bounded by `ef` or `topk`.

KVLite should standardize:

- score convention: larger score is better at the manager boundary;
- backend-internal distance convention may differ;
- conversion happens inside the adapter/kernel;
- output is sorted best-first or explicitly documented.

Existing `kvs_vector_topk_heap_t` is useful at the manager/export layer. A compute kernel may use its own optimized internal heap.

## 9. Metric and SIMD Boundary

Metric contract:

```text
ip
l2
cosine
```

Manager score convention:

- IP: higher dot product is better.
- L2: convert to a larger-is-better score at the manager boundary.
- Cosine: higher similarity is better.

Compute kernels may use:

- scalar C distance functions;
- AVX2/SSE/AVX512 functions;
- hnswlib `SpaceInterface`;
- Faiss `DistanceComputer`;
- USearch `metric_punned_t`.

KVLite should keep one backend-neutral distance facade for native/self kernels:

```text
distance_backend = auto | scalar | avx2
metric = ip | l2 | cosine
normalize = manager policy for cosine/ip embeddings
```

Do not expose third-party SIMD names as command semantics.

## 10. Add/Search/Delete

Add:

- Validate dimension.
- Ensure capacity.
- Insert or replace record id.
- Return backend status.
- Increment backend add counter.

Search:

- Validate dimension/topk.
- Use backend search params.
- Return candidates only.
- Increment backend search counter.

Delete:

- Mandatory manager logical delete.
- Optional backend physical delete.
- If backend delete is unsupported, manager marks `needs_rebuild`.

Replace:

- Optional.
- If unsupported, manager can perform delete + add + rebuild policy.

## 11. Snapshot Boundary

Compute snapshot is optional derived state.

If implemented:

- Snapshot header must include backend implementation, version, dimension, metric, M/connectivity, ef/expansion, size, capacity, record generation/checksum.
- Load must validate against manager records.
- Any mismatch forces rebuild from records.
- Snapshot format must not replace AOF/RDB record persistence.

Borrow:

- USearch fixed metadata header idea.
- hnswlib full graph save/load as a backend-local implementation technique.
- Faiss index I/O only as adapter-local baseline behavior.

Do not borrow:

- A third-party snapshot as KVLite's only durable vector state.

## 12. Stats and Profiling

Keep stable stats:

- size
- capacity
- memory estimate
- add count
- search count
- delete count
- rebuild count
- last rebuild status/time
- basic HNSW params

Keep diagnostic stats behind opt-in debug only:

- visited nodes
- distance computations
- graph hops
- average candidate count

Do not keep as stable API:

- hnswlib `searchBaseLayer` timing
- hnswlib `getNeighborsByHeuristic2` timing
- backend-private allocation timing
- benchmark-only phase fields

The P0-D conclusion is already sufficient: hnswlib VSET bottleneck is core HNSW construction, not KVLite wrapper overhead.

## 13. Backend Adapter Mapping

`hnswlib_hnsw`:

- `reserve` maps to `resizeIndex`.
- `add` maps to `addPoint(record_id)`.
- `search` maps to `searchKnn` and converts distance to KVLite score.
- `mark_delete` can map to `markDelete` later.
- `save/load` remains optional.

`usearch_hnsw`:

- `reserve` maps to `usearch_reserve`.
- `add` maps to `usearch_add(record_id)`.
- `search` maps to `usearch_search`.
- `delete` maps to `usearch_remove`.
- `info` maps to size/capacity/connectivity/expansion/memory.
- Do not enable multi-key by default.

`faiss_hnsw`:

- `add` maps to `add_with_ids` where possible.
- `search` maps to Faiss search and id output.
- `reserve/delete` may be unsupported or adapter-specific.
- Keep as baseline/fallback.

`self_hnsw`:

- Implements the full contract directly.
- Uses KVLite metric/distance facade.
- Uses compact graph layout and reusable search contexts.
- Rebuilds from manager records.
