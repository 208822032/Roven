# Third-party Vector Engine Audit

Date: 2026-06-24

Scope:

- `third_party/faiss/`
- `third_party/hnswlib/`
- `third_party/USearch-2.25.3/`

Note: the requested `third_party/usearch/` path is not present in this workspace. The available USearch source is `third_party/USearch-2.25.3/`.

Reference:

- `docs/vector_native_faiss_optimization/14_p0d_hnswlib_internal_profile_report.md`

## 1. Faiss Design Summary

Audited core files:

- `third_party/faiss/faiss/Index.h`
- `third_party/faiss/faiss/Index.cpp`
- `third_party/faiss/faiss/IndexFlat.h`
- `third_party/faiss/faiss/IndexFlat.cpp`
- `third_party/faiss/faiss/IndexFlatCodes.h`
- `third_party/faiss/faiss/IndexHNSW.h`
- `third_party/faiss/faiss/IndexHNSW.cpp`
- `third_party/faiss/faiss/impl/HNSW.h`
- `third_party/faiss/faiss/impl/HNSW.cpp`
- `third_party/faiss/faiss/IndexIDMap.h`
- `third_party/faiss/faiss/IndexIDMap.cpp`
- `third_party/faiss/faiss/index_io.h`
- `third_party/faiss/faiss/utils/distances.h`

Faiss is organized around a broad `Index` base abstraction:

- `Index` owns common metadata: dimension `d`, `ntotal`, `is_trained`, `metric_type`, `metric_arg`.
- `train`, `add`, `add_with_ids`, `search`, `range_search`, `remove_ids`, `reconstruct`, `reset`, `merge_from`, and codec hooks form the generic index contract.
- Batch input is first-class. Vectors are passed as compact row-major matrices of shape `n * d`.
- `SearchParameters` carries optional per-search controls such as `IDSelector`.

`IndexFlat`:

- Inherits through `IndexFlatCodes`.
- Stores encoded vectors in a contiguous `codes` buffer.
- For float flat indexes, `get_xb()` returns the raw float storage.
- Search is exhaustive and uses distance helper paths.
- `IndexFlatL2` adds an optional L2 norm cache.
- `remove_ids` is supported, but flat removal can shift internal sequential ids.

`IndexHNSW`:

- Combines an `HNSW` graph object with a separate random-access `storage` index.
- `IndexHNSWFlat` is the common form: full float storage plus HNSW graph.
- The HNSW object stores graph links, not the vector payload.
- Search delegates distance access to a `DistanceComputer`.
- Parameters include `M`, `efConstruction`, `efSearch`, `check_relative_distance`, bounded queue mode, and optional visited-table mode.
- Graph layout in `faiss::HNSW` is compact: `levels`, `offsets`, and one `neighbors` array. `offsets[i]..offsets[i+1]` contains all neighbor slots for node `i` across levels.

`IndexIDMap`:

- A wrapper/decorator around another `Index`.
- `IndexIDMap` stores external ids in `id_map`.
- `IndexIDMap2` adds `rev_map` for efficient reconstruction by external id.
- This is useful as an architectural pattern: id translation is isolated from the compute index.

Training, quantization, and index zoo:

- Faiss supports many trainable and compressed indexes: IVF, PQ, SQ, additive quantizers, refine indexes, shards, replicas, GPU variants, and more.
- The base `Index` carries `train` and codec APIs even when a specific index does not need training.
- This is powerful, but far wider than KVLite needs for the Vector Core reset.

Persistence:

- Faiss has generic index I/O under `index_io.h` and related implementations.
- The serialized index is engine-specific derived state. For KVLite, it should not become the source of truth.

## 2. hnswlib Design Summary

Audited core files:

- `third_party/hnswlib/hnswlib/hnswlib.h`
- `third_party/hnswlib/hnswlib/hnswalg.h`
- `third_party/hnswlib/hnswlib/visited_list_pool.h`
- `third_party/hnswlib/hnswlib/space_l2.h`
- `third_party/hnswlib/hnswlib/space_ip.h`
- `third_party/hnswlib/examples/cpp/example_replace_deleted.cpp`
- `third_party/hnswlib/tests/cpp/updates_test.cpp`

hnswlib has a narrow public interface:

- `AlgorithmInterface::addPoint`
- `AlgorithmInterface::searchKnn`
- `AlgorithmInterface::searchKnnCloserFirst`
- `AlgorithmInterface::saveIndex`

The main implementation is `HierarchicalNSW`.

Core state:

- `max_elements_` and `cur_element_count` represent capacity and used slots.
- `data_level0_memory_` is a contiguous level-0 allocation. Each element stores level-0 links, vector payload, and external label in one record-sized block.
- `linkLists_` is an array of pointers for upper-level neighbor lists.
- `element_levels_` stores the top level of each internal id.
- `label_lookup_` maps external labels to internal table ids.
- `visited_list_pool_` reuses `VisitedList` arrays by incrementing a visit tag instead of clearing the whole array on every search.
- `deleted_elements` tracks reusable deleted internal ids when replacement is enabled.

Graph construction:

- `addPoint` assigns an internal id and random level.
- Upper layers perform greedy descent from the entry point.
- `searchBaseLayer` expands base-layer candidates using a candidate queue and a top-candidate queue.
- `getNeighborsByHeuristic2` prunes candidates using the HNSW neighbor selection heuristic.
- `mutuallyConnectNewElement` writes bidirectional links and prunes full neighbor lists.

Search:

- `searchKnn` performs greedy descent through upper layers, then calls `searchBaseLayerST` at level 0.
- `searchBaseLayerST` supports a fast bare-bone path when there are no deletes or filters.
- Results are returned as a priority queue in farther-first order; `searchKnnCloserFirst` reverses it.

Delete/update:

- `markDelete` sets a delete bit inside the level-0 link-list metadata.
- `unmarkDelete` clears it.
- `allow_replace_deleted` allows new inserts to reuse deleted internal ids.
- Updating an existing label rewrites the vector and repairs local connections.
- Physical graph deletion is not immediate.

Resize and persistence:

- `resizeIndex` reallocates level-0 memory, link pointer array, locks, and visited-list pool.
- `saveIndex` writes parameters, level-0 memory, and upper-level link lists.
- `loadIndex` reconstructs memory, label lookup, deleted count, and upper link lists.

Distance and SIMD:

- `SpaceInterface` provides data size, distance function pointer, and distance parameter.
- `L2Space` and `InnerProductSpace` choose scalar/SSE/AVX/AVX512 implementations based on dimension and CPU capability.

P0-D relevance:

- The measured VSET/add bottleneck is inside `addPoint`.
- The dominant work is `searchBaseLayer` plus `getNeighborsByHeuristic2`.
- Wrapper overhead, vector copy, visited-list pool acquire/release, allocation, and resize are not the primary problem for the profiled 1K/10K cases.

## 3. USearch Design Summary

Audited core files:

- `third_party/USearch-2.25.3/include/usearch/index.hpp`
- `third_party/USearch-2.25.3/include/usearch/index_dense.hpp`
- `third_party/USearch-2.25.3/include/usearch/index_plugins.hpp`
- `third_party/USearch-2.25.3/c/usearch.h`
- `third_party/USearch-2.25.3/c/lib.cpp`
- `third_party/USearch-2.25.3/cpp/README.md`

USearch splits a generic graph core from dense-vector convenience APIs:

- `index_gt` is the lower-level graph index.
- `index_dense_gt` wraps it for fixed-dimensional dense vectors, key lookup, scalar conversion, and serialization.
- The C API exposes a compact engine boundary: init/free/reserve/add/search/remove/rename/save/load/view/info.

Core graph layout in `index_gt`:

- Nodes are addressed by compressed slots.
- `nodes_` is a buffer of `node_t` descriptors.
- Each `node_t` points to a tape allocation.
- The tape starts with key and level metadata, then neighbor tapes per level.
- A neighbor list stores a count followed by compressed slot ids.
- Contexts hold reusable `top_candidates` and `next_candidates` buffers.
- Visited state can use compact bitsets/hash sets.

Dense wrapper state:

- `metric_punned_t` abstracts metric kind, scalar kind, dimensions, and hardware dispatch.
- `slot_lookup_` maps keys to graph slots when key lookup is enabled.
- `vectors_lookup_` maps slots to vector memory.
- `free_keys_` tracks logically removed entries.
- `multi` allows multiple vectors per key.

Capacity:

- `index_limits_t` separates member capacity from add/search thread limits.
- `try_reserve` grows graph node buffers, lookup tables, vector lookup arrays, contexts, and lock stripes.
- The C API exposes `usearch_reserve`, `usearch_size`, `usearch_capacity`, `usearch_connectivity`, `usearch_expansion_add`, `usearch_expansion_search`, and `usearch_memory_usage`.

Add/search/remove:

- `add(key, vector)` accepts external keys directly.
- `search(query, wanted)` returns keys and distances.
- `filtered_search` supports key predicates.
- `remove(key)` removes entries from key lookup and marks slots as free at the dense wrapper level.
- `rename(from, to)` remaps keys without changing vector content.

Persistence:

- Dense serialized files start with a fixed 64-byte header containing magic, version, metric/scalar/key kinds, present/deleted counts, dimensions, and multi flag.
- USearch supports file save/load, in-memory buffer save/load, and zero-copy view/mmap-style load.
- Serialization may include or exclude vectors.

Non-core surface:

- USearch ships many bindings: C, C++, Python, JavaScript, Java, Go, Rust, C#, Swift, ObjC, Wolfram, SQLite, and WASM.
- These bindings are not relevant to KVLite's internal Vector Core design.

## 4. Capability Comparison

| Area | Faiss | hnswlib | USearch | KVLite relevance |
|---|---|---|---|---|
| Primary abstraction | Broad `Index` hierarchy | Narrow `AlgorithmInterface` plus `HierarchicalNSW` | `index_gt` graph plus `index_dense_gt` wrapper | KVLite needs a narrow compute-kernel ops table, not a full index zoo |
| Vector input | Batched row-major `n*d` float matrix | Single-point add/search at public C++ layer | Single add/search plus exact/batch helper APIs | KVLite should define batch APIs, even if first backend implements single-item loops |
| ID model | Sequential ids by default; `IndexIDMap` for external ids | External label maps to internal table id | External key maps to graph slot | KVLite should own key/id mapping and pass opaque record ids to backends |
| Vector storage | Index-dependent; flat stores contiguous codes | Level-0 memory stores vector payload with links and label | Dense wrapper owns or references vector storage | KVLite vector records must remain source of truth |
| Graph layout | Compact `offsets` plus single neighbors array | Level-0 inline links, upper-level pointer arrays | Node tape with per-level neighbor tapes | KVLite self_hnsw can borrow compact offset/tape ideas |
| Capacity | Index-dependent; many indexes grow internally | Explicit `max_elements`, `resizeIndex` | Explicit `try_reserve`, `capacity`, `limits` | KVLite backend ops should include `reserve` and report `capacity` |
| Visited state | `VisitedTable`, optional hashset | `VisitedListPool` tag arrays | compact visited sets and reusable search contexts | KVLite self_hnsw should use reusable per-thread visited/search contexts |
| Candidate queues | Heap/result-handler abstractions | priority queues for candidates and top candidates | reusable sorted buffer and heap contexts | KVLite should standardize topK/candidate buffers |
| Metric abstraction | `MetricType`, `DistanceComputer`, SIMD utils | `SpaceInterface` function pointer | `metric_punned_t`, scalar/hardware dispatch | KVLite should keep a small metric enum and SIMD distance facade |
| Persistence | Generic index I/O | save/load raw graph memory | versioned header, save/load/view | KVLite should snapshot records first; backend snapshot is optional derived cache |
| Deletion | `remove_ids`, not universal | mark delete, optional replacement | remove/rename at dense wrapper | KVLite should define logical delete in manager; compute delete is optional |
| Training/quantization | Extensive | none | scalar quantization in dense wrapper | KVLite should not import broad training/quantization now |
| Stats/info | Rich but index-specific | basic counters plus inspectable internals | size/capacity/connectivity/memory/ISA | KVLite VINFO should stay shallow and backend-neutral |

## 5. Designs Worth Borrowing

Borrow these ideas:

- A stable index/backend ops abstraction from Faiss' `Index` idea, but much smaller.
- An isolated external-id mapping layer like Faiss `IndexIDMap`, with KVLite records remaining authoritative.
- Explicit reserve/capacity reporting from hnswlib and USearch.
- Compact graph neighbor layout ideas from Faiss offsets and USearch node tapes.
- hnswlib-style visited-list pooling or USearch-style reusable per-thread search contexts.
- Candidate/topK buffers as reusable structures, not ad hoc allocations in every call.
- A small distance metric abstraction that can dispatch to scalar/SIMD implementations.
- Backend snapshot hooks as optional acceleration, never as the storage contract.
- Batch add/search shape from Faiss row-major APIs and USearch exact-search API.
- Delete as manager-owned logical state with optional backend acceleration.
- Shallow info/stat boundaries: size, capacity, memory estimate, metric, M, ef, add/search count.

## 6. Designs Not Worth Copying

Do not copy:

- Faiss' complete index zoo.
- Faiss GPU resources, clustering, full training, and quantization hierarchy.
- Faiss serialization as KVLite persistence format.
- hnswlib's graph memory layout directly into KVLite manager code.
- hnswlib internal profiling hooks as a public or long-lived KVLite API.
- USearch's multi-language binding structure.
- USearch's quantization and mmap view model as a first Vector Core requirement.
- Any design where a third-party engine owns VSET/VSEARCH semantics.
- Any design where a backend owns question/answer/scope metadata.
- Any design that bypasses KVLite TTL, AOF/RDB, replication, or command executor boundaries.

## 7. Audit Conclusion

Faiss is the best abstraction reference, hnswlib is the clearest minimal HNSW implementation reference, and USearch is the best compute-kernel API reference.

For KVLite:

- The Vector Manager Kernel should own storage semantics.
- Third-party engines should be compute components.
- The graph/index is derived state.
- The vector record is the source of truth.
- Backend replacement should not change VSET/VSEARCH/VDEL/VINFO, metadata, TTL, AOF/RDB, or replication semantics.
