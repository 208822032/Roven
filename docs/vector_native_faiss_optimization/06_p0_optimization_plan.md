# P0 Optimization Plan

P0 target: make the `hnswlib_hnsw` main path measurable and remove the largest server-path bottlenecks found in the formal hnswlib-vs-Faiss run.

## Scope

- `hnswlib_hnsw` is the KVLite primary vector engine.
- Faiss remains a baseline and fallback, not the product main path.
- `native_flat` and legacy `native_hnsw` are no longer mainline comparison targets.
- No hnswlib algorithm rewrite in P0; focus on correctness, profiling, allocation, and server-path overhead.

## Evidence

Formal run: `hnswlib_faiss_20260624T000000Z`

Result roots:

- `results/vector_native_faiss/hnswlib_vs_faiss/index_only/hnswlib_faiss_20260624T000000Z/`
- `results/vector_native_faiss/hnswlib_vs_faiss/server_path/hnswlib_faiss_20260624T000000Z/`

Key observations:

1. Build is now healthy: `make clean && make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j$(nproc)` passed.
2. Faiss build is `FAISS_OPT_LEVEL=generic`, `FAISS_ENABLE_MKL=ON`, `FAISS_ENABLE_GPU=OFF`.
3. Index-only 10K: hnswlib search avg 113us vs Faiss HNSW 142us; hnswlib recall@10 0.994100 vs Faiss HNSW 0.996700.
4. Index-only 100K: hnswlib search avg 342us vs Faiss HNSW 315us; hnswlib recall@10 0.899700 vs Faiss HNSW 0.927300.
5. Server-path VSET is the largest gap: hnswlib avg 1190us at both 10K/100K, while Faiss HNSW is 260us at 10K and 530us at 100K.
6. Server logs show hnswlib starts with `max_elements=16` and resizes repeatedly up to 131072 on the 100K run.
7. VSEARCH server-path gap is modest: hnswlib avg 150us/170us vs Faiss HNSW 110us/150us at 10K/100K.
8. Current VSEARCH index/non-index profiling is incomplete: server logs report `vsearch_search_count=0`, so `index_avg_us` is unavailable for VSEARCH.

## Work Items

1. Confirm `efSearch` actually takes effect
   - Verify hnswlib runtime `setEf(128)` path for both standalone and server.
   - Add a low-noise backend stat or VINFO field showing effective `M/efConstruction/efSearch`.
   - Re-run recall@10 after any instrumentation change.

2. Fix VSEARCH profiling visibility
   - Record index search count/total/max for the normal RESP `VSEARCH` path.
   - Split search into index search, candidate export, metadata lookup, response serialization, and network push where practical.
   - Ensure raw.csv never treats missing profile values as real zero-cost work.

3. Split VSET add cost
   - Keep current `vset_hnsw_add_avg_us`, but separate resize/reserve time from ordinary add time.
   - Record backend type in the profile line so hnswlib/Faiss add timing cannot be misread.
   - Add max/p95-oriented evidence for add tail latency.

4. Check hnswlib capacity and resize behavior
   - Avoid starting production hnswlib indexes at `max_elements=16` for bulk import.
   - Add a reserve or initial-capacity path driven by known import size or config.
   - Confirm whether resize copies graph/vector state and quantify its impact.

5. Reduce temporary allocation in add/search
   - Audit per-request vector/string/topK containers in `VSET` and `VSEARCH`.
   - Add buffer pooling where lifetime is executor-thread local and simple.
   - Avoid repeated malloc/free in hot paths before touching algorithm logic.

6. Preallocate response buffers
   - Pre-size common `VSEARCH` response buffers for topK=10.
   - Keep RESP formatting identical.
   - Track response build/push counters after the profiling fix.

7. Stabilize VINFO/backend stats
   - Expose backend name, effective HNSW params, active capacity, index size, and last resize count.
   - Keep Faiss fields available as baseline/fallback diagnostics.

## Exit Criteria

1. VSEARCH raw.csv contains reliable `index_avg_us` and `non_index_avg_us`.
2. VSET profile shows hnswlib ordinary add vs resize/reserve impact.
3. hnswlib 100K server VSET average is materially closer to Faiss HNSW, or the remaining gap has quantified root cause.
4. hnswlib 100K recall@10 is reported with confirmed effective `efSearch=128`.
5. A P0 rerun updates `09_hnswlib_vs_faiss_benchmark_report.md` or adds a follow-up report.

## P0 Profiling Update

Follow-up report:

`docs/vector_native_faiss_optimization/11_full_e2e_p0_profile_report.md`

Run id:

`full_e2e_p0_profile_20260624T003000Z`

Status changes:

1. VSEARCH profiling visibility is fixed for the L3 full e2e path. `raw.csv` now contains VSEARCH `index_avg_us`, full `non_index_avg_us`, and VSEARCH `response_avg_us`.
2. Hash embedding profiling is fixed for the current `hash` provider. The final run reports `runtime_encode_count=2000` for 1K and `11000` for 10K per backend, with `embedding_encode_avg_us=4-8`.
3. VSEARCH response serialization profiling is fixed. `vsearch_response_build_count=1000` and `vsearch_response_build_avg_us=11` in all final run cases.
4. Network push and metadata lookup are still not split into standalone fields.

Updated 10K L3 result:

| Operation | hnswlib avg us | Faiss avg us | Ratio | hnswlib index avg us | Faiss index avg us | Attribution |
|---|---:|---:|---:|---:|---:|---|
| VSET | 1190 | 270 | 4.41x | 1103 | 190 | dominated by backend add/index cost |
| VSEARCH | 160 | 160 | 1.00x | 86 | 99 | no material hnswlib search deficit in this run |

Next P0 focus:

1. Split hnswlib VSET add into ordinary add, resize/reserve, label/id mapping, and temporary allocation cost.
2. Expose backend stats for backend name, size, capacity/max_elements, resize count, M, efConstruction, and efSearch.
3. Add a minimal reserve/initial-capacity path before bulk insert/import.
4. Keep L3 full e2e and L1 index-only results separated in reports.

## P0-B Add/Reserve Update

Follow-up report:

`docs/vector_native_faiss_optimization/12_p0b_hnswlib_add_reserve_report.md`

Run id:

`full_e2e_p0b_add_reserve_20260624T012000Z`

Implemented:

1. Added `vector_hnsw_initial_capacity`.
2. The L3 full e2e runner now sets `vector_hnsw_initial_capacity=dataset_size`.
3. Added hnswlib add profiling split: total backend add, ordinary add, resize/reserve, id-map, and vector-copy/prepare.
4. Added backend stats for hnswlib capacity/max_elements, resize count, last resize range, M, efConstruction, efSearch, metric, and current provider normalization.
5. Fixed hnswlib `efSearch` handling so configured `efSearch=128` actually reaches hnswlib instead of silently using `topK=10`.

Result:

| Dataset | Metric | Before | P0-B |
|---:|---|---:|---:|
| 1K | hnswlib resize count | 1 | 0 |
| 10K | hnswlib resize count | 5 | 0 |
| 10K | hnswlib VSET avg | 1190us | 1230us |
| 10K | Faiss VSET avg | 270us | 270us |
| 10K | VSET ratio | 4.41x | 4.56x |
| 10K | hnswlib ordinary add avg | N/A | 1151us |

Interpretation:

- Initial capacity/reserve removes resize churn, but VSET does not materially improve.
- The remaining VSET bottleneck is hnswlib ordinary `addPoint()` cost.
- VSEARCH is now a fairer `efSearch=128` comparison and should not be compared directly to the earlier topK-effective hnswlib search numbers.

Next P0 focus:

1. Continue inside ordinary hnswlib add cost attribution.
2. Add efSearch latency/recall sweep for 32/64/128.
3. Keep Faiss as baseline/fallback and hnswlib as main path.

## P0-C AddPoint / efSearch Update

Follow-up report:

`docs/vector_native_faiss_optimization/13_p0c_hnswlib_add_efsearch_report.md`

Run id:

`full_e2e_p0c_add_efsearch_20260624T023000Z`

Implemented:

1. Added hnswlib wrapper add profiling fields: `before_add_prepare_us`, `hnswlib_add_point_us`, `after_add_bookkeeping_us`, and `stats_update_us`.
2. Extended the full e2e runner to support `--ef-search-list` and backend selection.
3. Re-ran L3 full e2e for 1K/10K, `efSearch=32/64/128`, `hnswlib_hnsw` and `faiss_hnsw`.
4. Added a separate L1 index-only recall table for the same efSearch values. L1 recall is not mixed into the L3 latency table.

Key result:

| Dataset | efSearch | hnswlib VSET avg | Faiss VSET avg | hnswlib addPoint avg | VSET ratio |
|---:|---:|---:|---:|---:|---:|
| 10000 | 32 | 1270us | 270us | 1186us | 4.70x |
| 10000 | 64 | 1260us | 270us | 1181us | 4.67x |
| 10000 | 128 | 1260us | 270us | 1179us | 4.67x |

Attribution:

- `vector_hnsw_initial_capacity` remains effective and `resize_count=0`.
- Wrapper prepare/bookkeeping/stats update costs are near zero.
- Vector copy is about `1us`.
- The remaining VSET bottleneck is black-box hnswlib `addPoint()` internals, not KVLite wrapper overhead.

efSearch recommendation:

- Default/quality profile: keep `efSearch=128` because 10K L1 hnswlib recall@10 is `0.994100`.
- Latency profile: `efSearch=64` is available, but 10K L1 recall@10 drops to `0.949400`.
- `efSearch=32` is only suitable for smoke/low-recall diagnostics at 10K.

Next P0-D focus:

1. Add optional source-level hnswlib `addPoint()` profiling behind a compile-time flag.
2. Sweep `efConstruction=64/128/256` for add latency and L1 recall.
3. Decide whether the next production optimization should be parameter tuning, batch add, or hnswlib internal memory-layout work.
