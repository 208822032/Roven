# KVLite Vector Manager Kernel Design

Date: 2026-06-24

Reference:

- `docs/vector_core_reset/02_third_party_vector_engine_audit.md`
- `docs/vector_native_faiss_optimization/14_p0d_hnswlib_internal_profile_report.md`

## 1. Audited KVLite Files

Core vector module:

- `include/kvs_vector.h`
- `include/kvs_vector_faiss_backend.h`
- `include/kvs_vector_hnswlib_backend.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_index.c`
- `src/vector/kvs_vector_distance.c`
- `src/vector/kvs_vector_distance_avx2.c`
- `src/vector/kvs_vector_embedding.c`
- `src/vector/kvs_vector_faiss_ops.c`
- `src/vector/kvs_vector_faiss_backend.cpp`
- `src/vector/kvs_vector_hnswlib_ops.c`
- `src/vector/kvs_vector_hnswlib_backend.cpp`

Command/config/benchmark/docs:

- `src/core/kvs_operation.c`
- `src/core/kvs_command_executor.c`
- `src/core/kvs_config.c`
- `include/kvs_config.h`
- `config/kvstore.conf`
- `config/kvstore-embedding.conf`
- `scripts/benchmark/`
- `docs/vector_native_faiss_optimization/`

## 2. Current Module Classification

Current Vector Manager general logic:

- Public V-family APIs: `kvs_vector_set`, `kvs_vector_get`, `kvs_vector_search`, `kvs_vector_del`, `kvs_vector_index_rebuild`, `kvs_vector_info`.
- Prepared VSET/VSEARCH flow: prepare embedding outside record mutation, then commit/search.
- Record table: `kvs_vector_store_t`, `kvs_vector_record_t`, active/deleted/empty state, free list, record ids, generation, metadata fields.
- Key mapping: `kvs_vector_id_map_t` maps question/scope hash to record index with content recheck.
- Metadata ownership: question, answer, normalized question, scope, embedding, TTL-facing `expire_at_ms`, model version.
- Backend dispatch: `kvs_vector_index_t` and `kvs_vector_index_ops_t`.
- Basic result filtering: active record check, deleted check, scope filter, threshold, topK export.
- VINFO logical counters: total/active/deleted/capacity/index size/dimension/index type/metric.
- AOF/RDB/replication boundaries in `kvs_command_executor.c`, which snapshot or replay records rather than backend graph internals.

hnswlib/Faiss-specific residuals:

- Public index enum values expose `KVS_VECTOR_INDEX_FAISS_HNSW`, `KVS_VECTOR_INDEX_FAISS_FLAT`, and `KVS_VECTOR_INDEX_HNSWLIB_HNSW` directly.
- `kvs_vector_info_t` has backend-specific fields for Faiss and hnswlib.
- `kvs_vector.c` uses weak function pointers for Faiss/hnswlib create/get-ops hooks.
- `kvs_vector_hnswlib_backend.cpp` prints directly to `stderr`.
- `kvs_vector_faiss_backend.cpp` prints directly to `stderr`.
- `src/core/kvs_command_executor.c` logs backend-specific fields in the generic shutdown/profile log path.
- `vector_index_type` accepts product names that are backend names, such as `hnswlib_hnsw` and `faiss_hnsw`.

Legacy/native residuals:

- Native flat, native HNSW, and SQ8 flat are still present in `kvs_vector_index.c`.
- `native_hnsw` is rejected in config selection, but native HNSW structs and functions still exist.
- `CLEANUP_CANDIDATE` comments mark old text-scoring and stale-index paths.
- VINFO has both logical info and physical legacy fields such as `flat_physical_size`, `ann_*`, and `sq8_*`.

Profiling/logging residuals:

- `kvs_vector_vset_commit_stats_t` is large and includes detailed P0-era timing fields.
- `kvs_command_executor_log_embedding_worker_stats` emits long vector/backend/profile lines.
- `vset_backend_add_*` is useful as a generic high-level counter, but the fine-grained VSET phase matrix should not be the long-term Vector Manager API.
- Direct backend `fprintf(stderr, ...)` statements should move behind KVLite logging or be removed.
- Historical benchmark scripts and docs under `docs/vector_native_faiss_optimization/` should remain archival, not active design input.

## 3. Manager Kernel Boundary

The Vector Manager Kernel is KVLite's vector storage and command-semantics owner.

It owns:

- VSET / VSEARCH / VDEL / VINFO integration.
- Vector record allocation, update, deletion, and free-slot reuse.
- External key semantics: question + scope.
- Stable record identity.
- Metadata: question, answer, normalized question, scope.
- Embedding as record data.
- TTL-facing fields and future TTL expiry hooks.
- AOF/RDB/replication record serialization boundaries.
- Backend lifecycle and rebuild orchestration.
- Backend-neutral stats and info.

It does not own:

- HNSW internal graph search loops.
- Neighbor selection heuristics.
- Backend-specific visited set implementation.
- Backend-specific save/load format as authoritative state.
- Faiss training, clustering, GPU, or quantization flows.
- USearch bindings or key ownership semantics.

Core rule:

```text
KVLite vector record is source of truth.
Vector index graph is derived state.
Third-party engines are compute components.
```

## 4. Record Model

The manager record should remain the authoritative unit:

```text
record_id
question
answer
normalized_question
scope
embedding
embedding_dim
state: empty | active | deleted
created_at_ms
updated_at_ms
expire_at_ms
generation
embedding_model_version
```

Record identity rules:

- VSET upsert key is `question + scope`.
- The manager maps that key to a stable record slot/id.
- Backends receive only record id/slot and embedding.
- Search returns candidate record ids.
- Manager filters candidates against active/deleted/scope/TTL state before exporting results.

## 5. Backend Ops Boundary

The manager should converge on a backend-neutral ops table:

```text
create(config) -> handle
destroy(handle)
reserve(handle, capacity) -> status
capacity(handle) -> n
size(handle) -> n
add(handle, record_id, embedding, dim) -> status
search(handle, query, dim, topk, params, out_candidates) -> status
delete(handle, record_id) -> unsupported | status
mark_deleted(handle, record_id) -> unsupported | status
rebuild_begin/end or reset(handle) -> status
info(handle, out_info) -> status
save_snapshot(handle, writer) -> optional
load_snapshot(handle, reader) -> optional
```

Current `kvs_vector_index_ops_t` already covers:

- `add`
- `search`
- `rebuild`
- `info`
- `destroy`

It should be extended later with:

- `reserve`
- `capacity`
- `delete` or `mark_delete`
- `reset`
- optional `save_snapshot` / `load_snapshot`

Backend output should be candidate-only:

```text
record_id
score
backend_optional_flags
```

The backend must not return question/answer/scope or perform final metadata filtering.

## 6. VSET Flow

Target VSET flow:

```text
parse command
prepare embedding
manager lock
find or allocate record
write record fields
update key/id map
reserve backend if needed
backend add/update record_id + embedding
mark dirty for AOF/RDB/replication
unlock
build response
```

Backend failure handling:

- If record write succeeds but backend add fails, manager marks `needs_rebuild`.
- The source-of-truth record remains valid unless the user-visible write must be rolled back by explicit policy.
- VINFO should expose `needs_rebuild`, not backend internals.

## 7. VSEARCH Flow

Target VSEARCH flow:

```text
parse command
prepare query embedding
manager read lock
backend search(query, topk * overfetch)
manager filters candidates:
  active
  not deleted
  ttl-valid
  scope match
  threshold
manager fills results from records
unlock
build response
```

Overfetch is manager policy:

- Backends should support `topk`.
- Manager may request a larger candidate count to compensate for scope/TTL/deleted filtering.
- Backend-specific `efSearch` remains backend config, not command semantics.

## 8. VDEL Flow

Target VDEL flow:

```text
parse command
manager lock
find question + scope
mark record deleted
remove key/id map entry
call backend delete/mark_delete if supported
if unsupported, mark index needs_rebuild
mark dirty for AOF/RDB/replication
unlock
build response
```

Deletion semantics:

- Logical delete is mandatory in manager.
- Physical graph delete is optional in compute kernels.
- Rebuild remains the compatibility path for backends without delete.

## 9. VINFO Flow

VINFO should report two classes of information.

Stable manager info:

- record counts
- record capacity
- dimension
- metric
- backend name
- backend size
- backend capacity
- `needs_rebuild`
- last rebuild status/time
- vector memory estimate
- index memory estimate

Backend-neutral compute info:

- kind: `flat`, `hnsw`, `sq8_flat`
- implementation: `self_hnsw`, `hnswlib_hnsw`, `usearch_hnsw`, `faiss_hnsw`, `faiss_flat`
- M/connectivity
- ef/add expansion
- ef/search expansion
- add/search count
- optional memory estimate

Avoid long-term VINFO fields for:

- hnswlib internal add phases
- Faiss-specific `ntotal` names
- direct engine layout details
- benchmark-only counters

## 10. AOF/RDB/Replication Boundary

Persistence and replication must be record-based:

```text
VSET question answer scope embedding metadata ttl
VDEL question scope
record generation/model version when needed
```

RDB/AOF should not depend on:

- hnswlib `saveIndex`
- Faiss `write_index`
- USearch `save/view`
- self_hnsw graph memory

Backend snapshot support can be added later as an optimization:

- It must be validated against record metadata.
- It must be discardable.
- On mismatch, KVLite rebuilds graph from records.

## 11. Cleanup Candidates

Safe later cleanup categories:

- Remove backend `fprintf(stderr, ...)` from Faiss/hnswlib adapters or replace with `KVS_LOG_*`.
- Collapse backend-specific VINFO fields into a generic backend info block.
- Remove `CLEANUP_CANDIDATE` legacy text scoring when no active caller remains.
- Remove native HNSW product selector remnants if self_hnsw replaces it cleanly.
- Keep `vset_backend_add_*` as high-level timing, but remove obsolete P0 phase counters from public headers.
- Archive benchmark scripts that emit hnswlib/Faiss-specific P0 breakdowns.

Do not delete yet:

- hnswlib/Faiss backends.
- Existing record/AOF/RDB snapshot code.
- Existing benchmark runners needed for backend comparison.
- Existing embedding worker stats while embedding runtime tuning remains active.

## 12. Mounting Future Backends

`self_hnsw`:

- New backend module under `src/vector/`.
- Implements compute-kernel ops directly.
- Uses KVLite record id as graph label.
- Does not own metadata or persistence semantics.

`usearch_hnsw`:

- New adapter around `third_party/USearch-2.25.3`.
- Maps KVLite record id to USearch key.
- Uses USearch reserve/add/search/remove/info.
- Does not enable USearch multi-key or binding layers.

`faiss_hnsw`:

- Remains baseline/fallback.
- Uses Faiss ids as KVLite record ids.
- Does not become the product storage model.

The manager should select all three through the same backend ops table.
