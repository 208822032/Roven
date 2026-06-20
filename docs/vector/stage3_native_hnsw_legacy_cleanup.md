# Native HNSW Legacy Cleanup (Stage 3 Step 2)

Date: 2026-06-20

## Why native_hnsw Was Downgraded

- 10K benchmark: 4.8x slower than Faiss HNSW (add), 5.6x slower (search)
- 100K benchmark: **timed out** (>3 minutes)
- Scratch pooling (Stage 3 P0): provided <1% latency improvement at 10K
- hnswlib_hnsw completed 100K (259 QPS add, 634 QPS search) and was promoted to default
- Maintaining a custom HNSW implementation is not the project's goal

## What Was Removed

1. **Scratch buffer fields** from `kvs_vector_ann_index_t`:
   - `scratch_visited` / `scratch_visited_cap`
   - `scratch_candidates` / `scratch_candidates_cap`
   - `scratch_results` / `scratch_results_cap`

2. **Scratch resize logic** from `kvs_vector_ann_index_reserve` (~40 lines)

3. **Scratch free logic** from `kvs_vector_ann_index_destroy`

4. **Scratch-based collect_candidates_layer** — restored to malloc/free

5. **ef_max overfetch** calculation — no longer needed without scratch sizing

6. **Caller free suppression** — callers now free candidates return value again

## What Native HNSW Still Has

- Full add/search/rebuild capability
- `vector_index_type=native_hnsw` configuration (with deprecated warning)
- VINFO `vector_backend:native_hnsw`
- Compatible with all KVLite paths (worker pool, executor, RDB, AOF)

## Current Backend Roles

| Backend | Role |
|---------|------|
| **hnswlib_hnsw** | Default recommended HNSW backend |
| native_hnsw | Legacy / experimental / reference implementation |
| native_flat | Exact search baseline |
| faiss_hnsw | Benchmark-only baseline |
| faiss_flat | Benchmark-only baseline |

## Step 3: Should native_hnsw Be Deleted Entirely?

Recommendation: **Wait.** Observe 1-2 weeks of production use with hnswlib_hnsw.
If no issues arise, Step 3 can proceed.

When Step 3 executes:
- Remove all `kvs_vector_ann_*` functions (~600 lines)
- Remove `KVS_VECTOR_INDEX_TYPE_HNSW` enum value (or alias to hnswlib)
- `vector_index_type=hnsw` already maps to hnswlib_hnsw
- `vector_index_type=native_hnsw` would become an error
