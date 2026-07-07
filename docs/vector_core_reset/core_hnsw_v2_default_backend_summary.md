# core_hnsw_v2 default backend summary

## Current default

`core_hnsw_v2` is the default vector ANN backend when `vector_index_type` is unset.

`vector_hnsw_v2_experimental_enabled` remains visible as a compatibility/telemetry field, but it no longer gates v2 selection.

After HNSW-P2-final, the `core_hnsw_v2` default ANN profile is the balanced profile:

```text
M = 48
M0 = 96
efConstruction = 512
efSearch = 1024
```

The switch is based on the 100K balanced gate:

```text
count = 100000
dim = 512
queries = 100
topK = 10
metric = IP normalized
recall@10 = 0.9740
oracle_mismatch = 0
duplicate_edge = 0
degree_overflow = 0
```

This is a default-parameter hardening result, not a full production SLA. The measured build cost was about 3,011,810,795us, so rebuild / cold-start remains a high-risk productization item and should be handled by the HNSW-P5 rebuild/cold-start route.

## Explicit backend selection

Use exact baseline / fallback:

```text
vector_index_type=flat
```

or:

```text
vector_index_type=core_flat
```

Compatibility request for removed legacy core_hnsw:

```text
vector_index_type=core_hnsw
```

Current behavior: accepted for old configuration compatibility and resolved to `core_flat`. It does not create a legacy core_hnsw index.

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
- `core_hnsw` maps to `core_flat` fallback

## Required gate list

Baseline:

```bash
tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh
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
- legacy `core_hnsw` source is removed; explicit `core_hnsw` config remains only as `core_flat` fallback compatibility.
- High tombstone ratio should trigger operational rebuild recommendation.
- Large cold start / bulk import should not be represented as final production behavior until background/shadow rebuild is hardened.
- The balanced default improves 100K recall, but increases build/search/memory cost versus the old lower-parameter default.
