# Vector Core Slimming Report

Date: 2026-06-24

Reference:

`docs/vector_native_faiss_optimization/14_p0d_hnswlib_internal_profile_report.md`

## 1. Decision

KVLite stops the hnswlib-specific performance attribution route.

P0-D already established the useful conclusion:

- hnswlib is the current minimal usable HNSW backend.
- The VSET/add bottleneck is inside hnswlib `addPoint()`.
- The dominant internal work is `searchBaseLayer()` and `getNeighborsByHeuristic2()`.
- resize, KVLite wrapper overhead, vector copy, visited-list acquire/release, and allocation are not the main bottleneck.

Keeping fine-grained hnswlib instrumentation no longer improves product direction. It couples KVLite to one third-party graph implementation and pollutes the Vector Manager with backend-internal concepts.

## 2. Audit Classification

Keep:

- `include/kvs_vector.h` public Vector Manager APIs and data structures.
- `src/vector/kvs_vector.c` vector record store, id map, metadata/scope binding, backend lifecycle, and backend ops dispatch.
- `src/vector/kvs_vector_index.c` legacy native index implementation needed by existing modes.
- `src/vector/kvs_vector_faiss_*` minimal Faiss baseline/fallback integration.
- `src/vector/kvs_vector_hnswlib_*` minimal hnswlib backend integration.
- VSET / VSEARCH / VDEL / VINFO handlers in `src/core/kvs_operation.c`.
- Executor-to-vector handoff and generic runtime counters in `src/core/kvs_command_executor.c`.
- Full e2e benchmark runners as generic backend comparison tools.

Delete from the active code path:

- `KVLITE_HNSWLIB_INTERNAL_PROFILE`.
- hnswlib `addPoint()` internal timing hooks.
- hnswlib internal profile structs and getter APIs.
- searchBaseLayer / neighbor selection / link update / greedy / random-level / visited-list / allocation / vector-write timing fields.
- P0-B/P0-C/P0-D hnswlib add breakdown counters.
- hnswlib resize attribution fields exposed through VINFO/logging.
- benchmark output columns that only existed for hnswlib internal attribution.

Archive:

- `docs/vector_native_faiss_optimization/` remains as historical optimization context.
- Past benchmark result directories under `results/vector_native_faiss/` remain historical artifacts and are not part of the active Vector Manager API.

Temporarily keep:

- `hnswlib_hnsw` as the minimal default-capable HNSW backend.
- `faiss_hnsw` as baseline/fallback, not the primary product path.
- Existing native flat/SQ8/native HNSW code where needed for compatibility and tests.

## 3. Production Code Changed

Updated files:

- `include/kvs_vector.h`
- `include/kvs_vector_hnswlib_backend.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_hnswlib_backend.cpp`
- `src/vector/kvs_vector_hnswlib_ops.c`
- `src/core/kvs_command_executor.c`
- `src/core/kvs_operation.c`
- `third_party/hnswlib/hnswlib/hnswalg.h`

Benchmark script updated:

- `scripts/benchmark/run_hnswlib_vs_faiss_full_e2e.py`
- `scripts/benchmark/run_hnswlib_vs_faiss_benchmark.py`
- `scripts/benchmark/run_vector_native_faiss_formal.py`

Build/test support updated:

- `Makefile`
- `tests/vcmd_onnx_faiss_bench.cpp`

Documentation added:

- `docs/vector_core_reset/00_vector_core_slimming_report.md`
- `docs/vector_core_reset/01_vector_manager_kernel_design.md`

## 4. Removed Profiling and Logging Content

Removed hnswlib internal profiling:

- hnswlib `HnswlibAddPointInternalProfile`.
- hnswlib `setKvliteInternalProfileEnabled()`.
- hnswlib `getKvliteAddPointInternalProfile()`.
- hnswlib `resetKvliteAddPointInternalProfile()`.
- `KVLITE_HNSWLIB_INTERNAL_PROFILE` environment behavior.
- `kvs_hnswlib_add_internal_profile_t`.
- `kvs_hnswlib_index_get_add_internal_profile()`.
- `kvs_vector_hnswlib_add_profile_ptr`.

Removed P0 attribution counters and logs:

- `hnswlib_internal_*` VINFO fields.
- `hnswlib_resize_count`, `hnswlib_last_resize_from`, `hnswlib_last_resize_to` VINFO fields.
- `vset_hnsw_ordinary_add_*`.
- `vset_hnsw_before_add_prepare_*`.
- `vset_hnsw_add_point_*`.
- `vset_hnsw_after_add_bookkeeping_*`.
- `vset_hnsw_stats_update_*`.
- `vset_hnsw_resize_*`.
- executor log line `hnswlib internal add profile: ...`.
- benchmark columns for addPoint/searchBaseLayer/neighbor selection/link update/visited/allocation/vector write.

Renamed active generic counter:

- `vset_hnsw_add_*` -> `vset_backend_add_*`

This keeps a single high-level backend add cost while removing hnswlib-specific attribution.

## 5. Preserved Vector Manager Capabilities

The slimmed Vector Manager still owns:

- VSET / VSEARCH / VDEL / VINFO integration.
- Vector record allocation, lifecycle, deletion state, and active/deleted counts.
- Key/scope to record id mapping.
- Question/answer/scope metadata binding.
- Embedding storage on vector records.
- Backend selection and lifecycle.
- Backend ops dispatch.
- Basic backend statistics: enabled, count/ntotal, M, efConstruction, efSearch, add count, search count.
- Generic commit/search timing and executor counters.

## 6. Current Backend State

Default-capable backend:

- `hnswlib_hnsw`

Baseline/fallback:

- `faiss_hnsw`
- `faiss_flat`

Compatibility/debug:

- native flat
- legacy native HNSW/SQ8 paths where still wired

hnswlib remains usable but is no longer a performance-attribution target.

## 7. Dead Code Status

Active code no longer references:

- `KVLITE_HNSWLIB_INTERNAL_PROFILE`
- `hnswlib_internal_*`
- hnswlib add internal profile APIs
- P0 hnswlib add breakdown fields

Verification command:

```bash
rg -n "KVLITE_HNSWLIB_INTERNAL_PROFILE|internal_profile|HnswlibAddPointInternalProfile|setKvlite|getKvlite|resetKvlite|vset_hnsw_|hnswlib_resize_count|hnswlib_last_resize|vset_hnsw_ordinary|vset_hnsw_before|vset_hnsw_add_point|vset_hnsw_after|vset_hnsw_stats|vset_hnsw_resize|neighbor selection|link update|addPoint internal" include src third_party/hnswlib/hnswlib scripts/benchmark tests
```

Result: no active-code matches.

Historical documents still mention these names by design. Those reports are archival and not part of the active module boundary.

## 8. Build Verification

Required verification:

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

Fallback verification if Faiss is unavailable:

```bash
make clean
make ENABLE_HNSWLIB=1 -j"$(nproc)"
```

The final task output records the actual command status for this workspace.

Actual verification result:

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

Result: passed.

Notes:

- The full build includes the hnswlib backend, Faiss baseline/fallback objects, test binaries, and eBPF targets in this workspace.
- `Makefile` was adjusted so test binaries that link `-lntyco` explicitly depend on `$(NTYCO_LIB)`. This fixes a parallel clean-build race under `-j`.
- `make clean` now removes optional vector backend objects from `src/vector/`, including hnswlib and Faiss backend objects.
