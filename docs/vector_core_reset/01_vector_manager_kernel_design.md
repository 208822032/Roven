# Vector Manager Kernel Design

## 1. Purpose

The Vector Manager Kernel is KVLite's stable vector data-management layer.

It must not depend on a specific graph implementation's internal layout or profiling hooks. Its job is to own KVLite records and dispatch vector compute operations through a narrow backend interface.

## 2. Boundary

KVLite Vector Manager Kernel owns:

- VSET / VSEARCH / VDEL / VINFO command integration.
- Vector record management.
- Question/scope key mapping to record id.
- Metadata binding: question, answer, scope, TTL-facing record identity.
- Embedding storage on records.
- Backend selection and lifecycle.
- Backend ops abstraction.
- Basic statistics.
- Rebuild orchestration from vector records.

Vector Compute Backend owns:

- init/create
- reserve/capacity management
- add
- search
- delete, optional
- info
- destroy

Backends do not own KVLite metadata. They receive record ids and embeddings and return candidate record ids with scores.

## 3. Backend Ops Contract

The stable backend surface is conceptually:

```text
create(config) -> handle
reserve(handle, capacity) -> status
add(handle, record_id, embedding) -> status
search(handle, query_embedding, topk, params) -> candidates
delete(handle, record_id) -> status, optional
info(handle) -> backend_info
destroy(handle)
```

Current code maps this through `kvs_vector_index_ops_t` and backend-specific create/get-ops hooks.

Required backend behavior:

- The backend treats `record_id` as opaque KVLite-owned identity.
- The backend must not require ownership of question/answer/scope strings.
- The backend may maintain graph/index state internally.
- The backend info surface must stay shallow: size, capacity, params, add/search counters.
- Internal graph profiling is not part of the Vector Manager ABI.

## 4. Current Backends

`hnswlib_hnsw`:

- Minimal HNSW compute backend.
- Kept for default-capable HNSW execution.
- Deprecated as a profiling target.

`faiss_hnsw`:

- Baseline/fallback.
- Not promoted to main path.

native flat / native HNSW / SQ8:

- Compatibility and debug paths.
- Not the target of this cleanup.

## 5. Future Backend Entry Points

Future `self_hnsw`:

- Add a backend module under `src/vector/`.
- Implement the existing backend ops.
- Keep graph construction and search internals inside the backend.
- Persist/rebuild through KVLite vector records, not by serializing private graph state as the source of truth.

Future `usearch_hnsw`:

- Add a backend adapter under `src/vector/`.
- Map KVLite record ids to USearch labels.
- Keep USearch index lifecycle inside the adapter.
- Expose only shallow info through VINFO.

Future `faiss_hnsw` replacement/upgrade:

- Keep Faiss behind the same ops boundary.
- Do not let Faiss index serialization become the Vector Manager source of truth.

## 6. Persistence and Replication Rule

Persistence and replication should be record-based:

```text
question + answer + scope + embedding + metadata + TTL identity
```

They must not be tied to one backend graph layout.

Backend graph/index state is rebuildable derived state. This allows:

- replacing hnswlib with self_hnsw or usearch_hnsw;
- changing HNSW parameters;
- rebuilding after compaction or restore;
- keeping replication format stable across backend changes.

## 7. What Is Intentionally Out of Scope

This reset does not:

- add USearch;
- switch Faiss to main path;
- implement a new HNSW algorithm;
- implement batch add/search;
- change persistence or replication;
- change RESP command semantics.

## 8. Design Direction

The next vector work should treat the Vector Manager Kernel as stable infrastructure and place compute optimization behind backend ops.

Good next steps:

- define a backend-neutral `reserve` operation if the current ops surface needs it;
- add a `delete` optional op when deletion semantics are ready;
- move new compute kernels into isolated backend modules;
- keep benchmark output backend-neutral unless explicitly running an archived investigation.
