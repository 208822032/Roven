# core_hnsw_v2 default backend summary

## Current default

`core_hnsw_v2` is the default vector ANN backend when `vector_index_type` is unset.

`vector_hnsw_v2_experimental_enabled` remains visible as a compatibility/telemetry field, but it no longer gates v2 selection.

## Explicit backend selection

Use exact baseline / fallback:

```text
vector_index_type=flat
```

or:

```text
vector_index_type=core_flat
```

Use legacy current core_hnsw:

```text
vector_index_type=core_hnsw
```

Use default v2 explicitly:

```text
vector_index_type=core_hnsw_v2
```

Legacy hnswlib adapter names remain compatibility aliases when compiled:

```text
vector_index_type=hnsw
vector_index_type=hnswlib_hnsw
```

## How to verify default v2

Build:

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

Run:

```bash
tests/vector_core_hnsw_v2_default_backend_test.sh
```

Expected:

- default config string is `core_hnsw_v2`
- `kvs_vector_info()` reports `KVS_VECTOR_INDEX_CORE_HNSW_V2`
- `flat` maps to `core_flat`
- `core_hnsw` maps to legacy current `core_hnsw`

## Required gate list

Baseline:

```bash
tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh
```

Legacy current core_hnsw:

```bash
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
```

core_hnsw_v2:

```bash
tests/vector_core_hnsw_v2_tombstone_test.sh
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_v2_graph_construction_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_core_hnsw_v2_scale_stability_test.sh
tests/vector_core_hnsw_v2_tombstone_policy_test.sh
```

Manager rebuild harness:

```bash
tests/vector_rebuild_state_machine_test.sh
tests/vector_rebuild_delta_capture_test.sh
tests/vector_rebuild_shadow_replay_test.sh
tests/vector_rebuild_shadow_swap_test.sh
```

## Use notes

- VSEARCH response format is unchanged.
- RDB/AOF format is unchanged.
- v2 graph is runtime derived state and is rebuilt from active records.
- `core_flat` is still the exact oracle.
- legacy `core_hnsw` remains available explicitly.
- High tombstone ratio should trigger operational rebuild recommendation.
- Large cold start / bulk import should not be represented as final production behavior until background/shadow rebuild is hardened.
