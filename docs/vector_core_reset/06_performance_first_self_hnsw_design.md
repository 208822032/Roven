# Performance-first Self HNSW Design

Date: 2026-06-24

References:

- `docs/vector_core_reset/02_third_party_vector_engine_audit.md`
- `docs/vector_core_reset/03_kvlite_vector_manager_kernel_design.md`
- `docs/vector_core_reset/04_vector_compute_kernel_design.md`
- `docs/vector_core_reset/05_vector_backend_reselection_plan.md`

## 1. Design Goal

KVLite should own a native Vector Compute Kernel that can compete with production HNSW engines, not a demonstration implementation that later needs a rewrite.

The first self-owned kernels are:

- `self_flat_exact`: exact flat search used as ground truth, recall baseline, and simple production fallback.
- `self_hnsw`: HNSW approximate search designed from the first implementation around cache locality, explicit capacity, reusable scratch memory, and SIMD-ready distance computation.

Target boundary:

```text
KVLite Manager:
  VSET / VSEARCH / VDEL / VINFO
  record / key-id map / metadata / scope / TTL
  AOF / RDB / replication
  backend lifecycle
  candidate filtering

Vector Compute Kernel:
  reserve / capacity
  add(record_id, vector)
  search(query) -> record_id + score
  mark_delete, optional
  reset
  info
```

Core rule:

```text
vector record is source of truth
index graph is rebuildable derived state
```

## 2. Why Not Demo First

A demo HNSW usually starts with:

- `vector<vector<int>>` neighbor lists;
- per-add and per-search heap allocations;
- STL priority queues on hot paths;
- scattered node objects;
- full `memset` of visited arrays;
- graph links mixed with product metadata;
- no explicit reserve/capacity model.

That approach can validate functional recall, but it bakes in the wrong memory shape. The expensive parts of HNSW are exactly the loops that touch candidates, neighbors, visited state, and distances. If those loops begin with pointer-heavy containers and allocator traffic, later optimization becomes a rewrite of the graph layout, API, and benchmark interpretation.

The first `self_hnsw` implementation must therefore start with the target memory layout:

- contiguous vector cache;
- fixed-capacity level-0 neighbor arrays;
- compact upper-layer neighbor storage;
- reusable candidate/top heaps;
- visited tags instead of clearing;
- per-thread search contexts;
- backend-owned distance facade with scalar and AVX2 slots.

## 3. self_flat_exact Design

Purpose:

- ground truth for recall@K;
- baseline for score correctness;
- fallback backend for small datasets or debug;
- benchmark companion for `self_hnsw`.

Ops:

```text
reserve(capacity)
add(record_id, vector)
search(query, topK)
reset()
info()
```

Metrics:

- IP;
- L2;
- cosine through normalized vectors plus IP.

Data structure:

```text
self_flat_exact {
  dim
  metric
  capacity
  size
  vectors[capacity * dim]        // float32 row-major backend-owned cache
  record_ids[capacity]           // slot -> KVLite record_id
  slot_by_record_id              // record_id -> slot, dense map or manager-provided slot
  deleted_tags[capacity]         // optional logical skip bit
  distance_facade
  search_context_pool
}
```

The flat kernel owns a vector cache. The Manager record remains authoritative; the backend cache is derived state and can be rebuilt from records. Keeping a backend-owned contiguous cache is required for exact search performance and avoids chasing manager record pointers in the inner distance loop.

Search flow:

```text
ctx = acquire_search_context()
top_heap.reset(topK)
for slot in 0..size-1:
  if deleted_tags[slot]: continue
  score = distance_facade.score(query, vectors + slot * dim)
  top_heap.push_bounded(record_ids[slot], score)
top_heap.export_best_first(out)
release(ctx)
```

TopK heap:

- fixed-capacity min-heap for larger-is-better scores;
- heap root is the current worst accepted score;
- push if heap not full or score is better than root;
- export sorts the final heap best-first;
- no allocation after context initialization.

Benchmark relationship:

- L1 recall for `self_hnsw` is measured against `self_flat_exact`.
- `self_flat_exact` also cross-checks Faiss flat for distance convention and score ordering.
- HNSW benchmark reports recall@10, add latency, search latency, memory, and distance computations where available.

## 4. self_hnsw Data Structures

`self_hnsw` uses HNSW, but its memory layout is KVLite-specific and performance-first.

Top-level state:

```text
self_hnsw {
  dim
  metric
  M
  M0
  ef_construction
  ef_search
  max_level
  entry_node
  size
  capacity

  vectors[capacity * dim]
  nodes[capacity]

  level0_neighbors[capacity * M0]
  level0_counts[capacity]

  upper_offsets[capacity + 1]        // fixed phase or compact phase
  upper_neighbors[upper_capacity]
  upper_counts[upper_list_count]

  visited_tags[capacity]
  current_visit_tag

  record_to_node
  free_nodes
  rng
  distance_facade
  search_context_pool
}
```

Node descriptor:

```text
node {
  record_id
  level
  flags                  // active/deleted
  vector_offset          // node_id * dim
  upper_offset           // first upper neighbor-list slot
  upper_list_count       // level count above 0
}
```

Required arrays:

```text
vectors[capacity * dim]
nodes[capacity]
level0_neighbors[capacity * M0]
level0_counts[capacity]
upper_offsets / upper_neighbors
visited_tags[capacity]
candidate_heap
top_heap
search_context_pool
distance_facade
```

### Record and Node Mapping

The Manager owns stable KVLite `record_id`. The kernel owns dense `node_id`.

Mapping:

```text
record_id -> node_id
node_id -> record_id
```

Rules:

- `record_id` is the only id exposed to Manager.
- `node_id` is internal and dense for array indexing.
- search returns `record_id + score`;
- rebuild can assign new `node_id` values because records are source of truth.

The first implementation can use a hash map for `record_id -> node_id` outside search hot loops. Search only uses `node_id` and converts to `record_id` at result export.

### Vector Cache Ownership

`vectors` is backend-owned derived cache:

- row-major float32;
- aligned for AVX2 loads where possible;
- normalized at add time when metric is cosine;
- rebuilt from Manager records after reset/rebuild.

This decouples graph traversal from Manager record memory and keeps distance computation contiguous.

### Graph and Manager Decoupling

The graph stores only:

- internal node ids;
- record ids;
- vector cache offset;
- level;
- neighbor links;
- delete bit.

It must not store:

- question;
- answer;
- scope;
- TTL;
- AOF/RDB state;
- replication metadata.

Manager filtering remains outside the kernel. If Manager overfetches candidates to handle scope/TTL/deleted filtering, it passes a larger `topK` or `overfetch` search param.

### Level 0 Layout

Level 0 is the hottest layer and should use fixed arrays:

```text
level0_neighbors[node_id * M0 + i]
level0_counts[node_id]
```

Rationale:

- no pointer chasing;
- predictable capacity;
- fast pruning rewrite;
- easy prefetch;
- no per-node allocation.

`M0` should default to `2 * M`, following common HNSW practice.

### Upper Layer Layout

First phase:

```text
upper_neighbors[capacity * max_upper_levels * M]
upper_counts[capacity * max_upper_levels]
```

This is simple and allocation-free but can waste memory if maximum random level is high.

Target compact phase:

```text
upper_offsets[node_id] -> offset in upper_neighbors
upper_counts[offset_per_node_level]
upper_neighbors[packed slots]
```

Each node reserves exactly `level * M` upper slots during add, from a pre-reserved arena. Because the random level is known before insertion, the kernel can allocate the node's upper tape once from contiguous capacity. Rebuild can compact offsets perfectly.

Evolution path:

1. Implement level 0 as fixed arrays.
2. Implement upper layers with a pre-reserved arena and per-node offsets.
3. Add rebuild-time compacting of `upper_offsets/upper_neighbors`.
4. Keep the public ops unchanged.

### Capacity and Reserve

`reserve(capacity)` must be a first-class operation.

Rules:

- Manager calls reserve before bulk rebuild or when nearing capacity.
- Growth policy should be geometric, for example `new_capacity = max(requested, capacity * 2)`.
- `reserve` allocates all hot arrays together.
- failed reserve leaves the existing index usable.
- add path must not trigger frequent reallocation.

## 5. Add Flow

High-level add:

```text
ensure capacity
node_id = allocate node
copy/normalize vector into vectors[node_id * dim]
level = random_level()
nodes[node_id] = descriptor(record_id, level)
record_to_node[record_id] = node_id

if size == 0:
  entry_node = node_id
  max_level = level
  return

ep = entry_node
for layer = max_level downto level + 1:
  ep = greedy_search_layer(query, ep, layer)

for layer = min(level, max_level) downto 0:
  candidates = search_layer(query, ep, layer, ef_construction)
  selected = select_neighbors(query, candidates, M_layer)
  connect_bidirectional(node_id, selected, layer)
  ep = best candidate from candidates

if level > max_level:
  entry_node = node_id
  max_level = level
```

Required HNSW pieces:

- random level;
- entry point;
- upper-layer greedy search;
- `search_layer` / `searchBaseLayer` equivalent;
- neighbor selection heuristic;
- bidirectional link update;
- neighbor list pruning.

### Avoiding malloc on Add

Add must avoid allocator traffic in graph hot loops:

- vectors copied into pre-reserved `vectors`;
- node descriptors written into `nodes`;
- level-0 neighbors written into fixed slots;
- upper neighbors written into pre-reserved upper arena;
- candidate heap and top heap come from an add/search context;
- scratch candidate arrays are fixed-capacity based on `ef_construction`;
- no `vector`, `priority_queue`, or per-neighbor allocation in traversal.

If an add context is not available, the operation fails or waits at context acquisition, not inside traversal.

## 6. Search Flow

High-level search:

```text
ctx = acquire_search_context()
ctx.next_visit()

ep = entry_node
for layer = max_level downto 1:
  ep = greedy_search_layer(query, ep, layer)

candidates = search_layer(query, ep, 0, ef_search)
export topK record_id + score
release(ctx)
```

`search_layer`:

```text
visited.mark(ep)
top_heap.push(ep, score(ep))          // bounded max distance / min score policy internally
candidate_heap.push(ep, score(ep))    // best candidate first

while candidate_heap not empty:
  candidate = candidate_heap.pop_best()
  if candidate cannot improve worst(top_heap):
    break

  for neighbor in neighbors(candidate, layer):
    if visited.mark_if_new(neighbor):
      score = distance(query, vector(neighbor))
      if top_heap has room or score better than worst:
        candidate_heap.push(neighbor, score)
        top_heap.push_bounded(neighbor, score, ef)

return top_heap
```

Search hot path rules:

- no malloc;
- no Manager record access;
- no scope/TTL filtering;
- no full visited clear;
- no virtual dispatch inside distance loop after facade is selected;
- export converts internal node ids to record ids once.

## 7. Visited and Search Context Design

Visited tags:

```text
visited_tags[capacity] : uint32
current_visit_tag : uint32
```

Search starts by incrementing `current_visit_tag`.

```text
mark_if_new(node):
  if visited_tags[node] == current_visit_tag:
    return false
  visited_tags[node] = current_visit_tag
  return true
```

This avoids `memset(visited, 0, capacity)` per search. On tag overflow, reset the array once and restart tags from 1.

Context:

```text
search_context {
  visit_tag
  candidate_heap
  top_heap
  selected_neighbors
  scratch_scores
  result_buffer
}
```

Context pool:

- per-thread fast path through thread-local context;
- fallback shared pool for worker pools;
- fixed capacities derived from max `ef_construction`, `ef_search`, and `topK_limit`;
- allocation occurs at init/reserve, not inside traversal.

## 8. Heap Design

The kernel should not use STL `priority_queue` in hot loops.

Candidate heap:

- binary heap over `(score, node_id)`;
- best candidate at root;
- for larger-is-better score, candidate heap is a max-heap.

Top heap:

- bounded heap of accepted candidates;
- worst accepted candidate at root for O(1) pruning check;
- for larger-is-better score, top heap is a min-heap.

Operations:

```text
push
pop_best
push_bounded(limit)
worst_score
clear_no_free
export_sorted_best_first
```

All heap arrays live in `search_context`.

## 9. Distance and SIMD Design

Score convention at kernel boundary:

```text
larger score is always better
```

Metric handling:

- IP: score is dot product.
- L2: score is negative squared L2 distance.
- Cosine: Manager or backend normalizes vectors, then score is IP.

Distance facade:

```text
distance_facade {
  metric
  dim
  normalize_on_add
  normalize_query
  fn_score_scalar
  fn_score_avx2
  fn_score_avx512_future
  selected_fn
}
```

AVX2 is reserved from the first design:

- contiguous `float32` vectors;
- optional 32-byte alignment;
- dimension remainder handled by scalar tail;
- dimension-specialized paths can be added later for common `64d`, `128d`, `512d`.

The facade is selected at init, so the hot loop calls one function pointer or inlined backend-specific function, not a metric switch per neighbor.

## 10. Neighbor Selection and Link Update

Neighbor selection modes:

```text
simple_topM
heuristic_diversified
```

Both should be retained.

`simple_topM`:

- selects closest `M` candidates by query score;
- faster add;
- lower diversity;
- useful for controlled benchmarks and small datasets.

`heuristic_diversified`:

- HNSW-style diversity pruning;
- improves recall and graph quality;
- costs more add time;
- default for production-quality `self_hnsw`.

Selection tradeoff:

- 10K/64d can tolerate heuristic overhead to protect recall@10 >= 0.99.
- 100K/64d needs heuristic or a tuned hybrid to keep recall@10 >= 0.95 without inflating `ef_search`.
- A config may allow simple mode for experiments, but benchmark promotion must use diversified mode unless data proves otherwise.

Bidirectional link update:

```text
for neighbor in selected:
  add node_id to neighbor list of new node
  add new node to neighbor list of neighbor
  if neighbor list over capacity:
    prune neighbor list with same selection policy
```

Pruning rewrites fixed neighbor slots in place:

- read existing neighbors plus new candidate into scratch array;
- select up to `M_layer`;
- write back compactly;
- update count.

## 11. Memory Layout Summary

Required hot arrays:

```text
vectors[capacity * dim]
nodes[capacity]
level0_neighbors[capacity * M0]
level0_counts[capacity]
upper_offsets[capacity + 1]
upper_neighbors[upper_capacity]
visited_tags[capacity]
```

Estimated memory for 100K / 64d / float32 / M=32 / M0=64:

```text
vectors:           100000 * 64 * 4  ~= 25.6 MB
level0_neighbors:  100000 * 64 * 4  ~= 25.6 MB
nodes/counts/etc:  single-digit MB range
upper_neighbors:   depends on random levels, usually far below level0
```

This shape is acceptable for index-only benchmark and keeps the hottest layer contiguous.

## 12. Benchmark Targets

L1 index-only matrix:

```text
self_flat_exact
self_hnsw
faiss_hnsw
usearch_hnsw
```

L3 full e2e matrix:

```text
KVLite + self_hnsw
KVLite + faiss_hnsw
KVLite + usearch_hnsw
```

Performance gates:

```text
10K / 64d:
  recall@10 >= 0.99
  search avg <= 1.2x Faiss HNSW
  add avg <= 1.5x Faiss HNSW

100K / 64d:
  recall@10 >= 0.95
  search avg <= 1.5x Faiss HNSW
  add avg <= 2.0x Faiss HNSW
```

If `usearch_hnsw` is later integrated, the preferred target becomes USearch parity. Faiss HNSW remains the minimum baseline: `self_hnsw` must not be clearly behind Faiss on both add and search at acceptable recall.

Benchmark rules:

- recall is measured against `self_flat_exact`;
- report L1 index-only separately from L3 full e2e;
- report add/search avg, p95, recall, memory estimate, M, efConstruction, efSearch, metric, dimension, and dataset size;
- do not expose backend-internal phase timings as stable VINFO fields.

## 13. Phased Implementation Route

This is not a demo-first route. Each phase preserves the target data layout and hot-path rules.

Phase A: exact baseline

- implement `self_flat_exact`;
- add scalar distance facade and AVX2 function slots;
- implement fixed topK heap;
- validate score convention with IP/L2/cosine;
- benchmark against Faiss flat.

Phase B: HNSW memory skeleton

- implement `self_hnsw` storage: vectors, nodes, level0 arrays, upper arena, visited tags, context pool;
- implement reserve/capacity/reset/info;
- no public protocol change.

Phase C: HNSW add/search

- implement random level, entry point, greedy upper search, `search_layer`, bidirectional links, pruning;
- implement `heuristic_diversified` and `simple_topM`;
- keep all heaps and scratch buffers context-owned.

Phase D: performance gate

- run L1 10K/100K against `self_flat_exact`, Faiss HNSW, and later USearch HNSW;
- tune M/M0/efConstruction/efSearch and selection mode;
- only promote if gates are met.

Phase E: Manager integration hardening

- keep Manager filtering outside the kernel;
- add overfetch policy if scoped/TTL/deleted filtering needs it;
- add optional `mark_delete`;
- keep rebuild from records as the recovery path.

Phase F: optional derived-state features

- backend snapshot cache;
- compact rebuild;
- batch add/search API implementation;
- AVX512 distance path.

## 14. Risks and Non-goals

Risks:

- HNSW add may still lag USearch if neighbor selection and link pruning are too expensive.
- Compact upper-layer storage may require careful reserve math for failure-safe growth.
- Score conversion must be consistent across IP/L2/cosine or recall comparison becomes misleading.
- Logical deletes can lower recall unless Manager overfetch and rebuild policy are tuned.
- Thread-local contexts need clear ownership when KVLite worker scheduling changes.

First-stage non-goals:

- IVF / PQ / SQ;
- GPU;
- multi-language binding;
- complex snapshot;
- physical delete/compact;
- shard;
- formal batch implementation;
- persistence or replication redesign.

Reserved interfaces:

- batch add/search;
- snapshot cache;
- mark_delete;
- compact rebuild;
- AVX2/AVX512 distance dispatch.

## 15. Alignment With Faiss and USearch

Faiss alignment:

- compare `self_flat_exact` with Faiss flat for exact score correctness;
- compare `self_hnsw` with Faiss HNSW for recall/add/search gates;
- borrow compact offsets concept without importing Faiss index hierarchy.

USearch alignment:

- compare reserve/capacity behavior;
- compare add/search latency when `usearch_hnsw` adapter exists;
- borrow reusable context and compute-kernel boundary ideas;
- do not import bindings, multi-key behavior, or snapshot semantics into KVLite product behavior.

Promotion rule:

```text
self_hnsw can become a serious backend only if it keeps the Vector Manager boundary clean and meets L1/L3 performance gates against Faiss, then against USearch when available.
```
