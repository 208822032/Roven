# P0-D hnswlib Internal Profile and efConstruction Sweep

Run id: `full_e2e_p0d_internal_efconstruction_20260624T000000Z`

Result directory:

`results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0d_internal_efconstruction_20260624T000000Z/`

## 1. Scope

P0-D keeps the P0-C direction:

- `hnswlib_hnsw` is the KVLite primary vector engine.
- `faiss_hnsw` is a baseline/fallback only.
- No RESP protocol change.
- No batch add/search, SIMD, quantization, sharding, native_hnsw/native_flat revival, or HNSW algorithm rewrite.
- L3 full e2e and L1 index-only recall are reported separately.

Benchmark parameters:

- `M=32`
- `efSearch=128`
- `efConstruction=64/128/256`
- `dim=64`
- `topK=10`
- `hnswlib_hnsw` vs `faiss_hnsw`

## 2. Modified Files

Production/profiling code:

- `third_party/hnswlib/hnswlib/hnswalg.h`
- `include/kvs_vector_hnswlib_backend.h`
- `src/vector/kvs_vector_hnswlib_backend.cpp`
- `src/vector/kvs_vector_hnswlib_ops.c`
- `include/kvs_vector.h`
- `src/vector/kvs_vector.c`
- `src/core/kvs_command_executor.c`

Benchmark/report files:

- `scripts/benchmark/run_hnswlib_vs_faiss_full_e2e.py`
- `docs/vector_native_faiss_optimization/14_p0d_hnswlib_internal_profile_report.md`
- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0d_internal_efconstruction_20260624T000000Z/`

Production code was modified only to add opt-in profiling and logging. The runtime switch is:

```bash
KVLITE_HNSWLIB_INTERNAL_PROFILE=1
```

Default behavior is unchanged when the variable is unset.

## 3. Build

Build command:

```bash
make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j 4
```

Build status: passed.

L1 helper build:

```bash
g++ -O3 -std=c++17 -I third_party/hnswlib -I third_party/faiss \
  -o build/tests/bin/vector_index_only_formal \
  tests/vector_index_only_formal.cpp \
  third_party/faiss/build/faiss/libfaiss.a \
  -fopenmp -lblas -llapack -lpthread -lm
```

Status: passed.

## 4. Profiling Coverage

The hnswlib `addPoint()` profile splits these phases:

| Field | Meaning |
|---|---|
| `random_level_avg_us` | `getRandomLevel()` and explicit level override check. |
| `greedy_search_avg_us` | Upper-layer greedy descent before base-layer connection. |
| `search_base_layer_avg_us` | Existing `searchBaseLayer()` call, including candidate queues, distance calls, and graph expansion. |
| `neighbor_selection_avg_us` | Existing `getNeighborsByHeuristic2()` heuristic pruning. |
| `link_update_avg_us` | Existing `mutuallyConnectNewElement()` link writes and neighbor-list replacement. |
| `visited_list_avg_us` | `VisitedListPool` get/release only. |
| `allocation_avg_us` | Upper-layer `linkLists_[cur_c]` malloc and memset. |
| `vector_memory_write_avg_us` | Level-0 memory zero, label write, vector payload copy. |

Important limitation: `searchBaseLayer()` is not split further into distance computation vs priority queue maintenance. Splitting that loop accurately would require deeper instrumentation inside the search loop and risks changing the hot path more than this stage allows.

## 5. L3 VSET efConstruction Sweep

L3 path: TCP/RESP/text `VSET`, hash embedding, executor, backend, response. Initial capacity equals dataset size, so `resize_count=0`.

| Dataset | efConstruction | hnsw avg/index/addPoint us | hnsw searchBase/selection/link/greedy us | Faiss avg/index us |
|---:|---:|---:|---:|---:|
| 1000 | 64 | 310 / 235 / 235 | 147 / 77 / 2 / 4 | 110 / 40 |
| 1000 | 128 | 480 / 396 / 396 | 232 / 152 / 2 / 4 | 140 / 64 |
| 1000 | 256 | 720 / 634 / 634 | 336 / 286 / 2 / 4 | 160 / 87 |
| 10000 | 64 | 400 / 321 / 321 | 223 / 84 / 3 / 6 | 160 / 80 |
| 10000 | 128 | 710 / 623 / 623 | 426 / 181 / 6 / 6 | 200 / 125 |
| 10000 | 256 | 1260 / 1165 / 1164 | 767 / 380 / 10 / 6 | 280 / 198 |

Other hnswlib internal fields were below 1us average in this run:

- `visited_list_avg_us`
- `allocation_avg_us`
- `vector_memory_write_avg_us`
- wrapper prepare/bookkeeping/stats

## 6. addPoint Bottleneck

The bottleneck is inside hnswlib graph construction:

- `searchBaseLayer()` is the largest component.
- `getNeighborsByHeuristic2()` neighbor selection is the second largest component.
- Both scale strongly with `efConstruction`.
- Link update is small but grows slightly with `efConstruction`.
- Random level, upper-layer greedy descent, visited-list get/release, upper-layer allocation, and vector memory write are not material at 1K/10K.

At 10K:

| efConstruction | addPoint avg us | searchBaseLayer | neighbor selection | Combined share |
|---:|---:|---:|---:|---:|
| 64 | 321 | 223 | 84 | 95.6% |
| 128 | 623 | 426 | 181 | 97.4% |
| 256 | 1164 | 767 | 380 | 98.5% |

This supports the P0-C conclusion and narrows it: the VSET cost is not resize, wrapper, vector copy, visited-list pool, or allocation. It is the existing hnswlib base-layer candidate expansion plus neighbor-selection heuristic.

## 7. L3 VSEARCH Reference

L3 path: TCP/RESP/text `VSEARCH`, hash embedding, executor, backend, metadata/response.

| Dataset | efConstruction | hnsw avg/p95/index us | Faiss avg/p95/index us |
|---:|---:|---:|---:|
| 1000 | 64 | 420 / 450 / 346 | 80 / 90 / 15 |
| 1000 | 128 | 420 / 450 / 350 | 320 / 150 / 239 |
| 1000 | 256 | 430 / 450 / 353 | 80 / 100 / 18 |
| 10000 | 64 | 570 / 620 / 495 | 150 / 150 / 73 |
| 10000 | 128 | 620 / 680 / 544 | 80 / 100 / 20 |
| 10000 | 256 | 670 / 710 / 595 | 90 / 100 / 29 |

`efConstruction` affects hnswlib L3 VSEARCH less than VSET, but lower `efConstruction` modestly lowers hnswlib search latency at 10K.

## 8. L1 Recall@10

L1 path: direct index API on binary float dataset with exact flat ground truth. This is intentionally separate from L3.

| Dataset | efConstruction | hnsw recall/search avg/p95 us | Faiss recall/search avg/p95 us |
|---:|---:|---:|---:|
| 1000 | 64 | 0.999900 / 55 / 61 | 0.999900 / 71 / 72 |
| 1000 | 128 | 1.000000 / 55 / 58 | 1.000000 / 69 / 72 |
| 1000 | 256 | 1.000000 / 55 / 58 | 1.000000 / 69 / 72 |
| 10000 | 64 | 0.989900 / 118 / 125 | 0.990100 / 162 / 170 |
| 10000 | 128 | 0.994300 / 120 / 127 | 0.996300 / 173 / 181 |
| 10000 | 256 | 0.994100 / 122 / 131 | 0.996700 / 175 / 184 |

L1 add cost also decreases sharply with lower `efConstruction`:

| Dataset | efConstruction | hnsw add avg us | Faiss add avg us |
|---:|---:|---:|---:|
| 1000 | 64 | 60 | 42 |
| 1000 | 128 | 86 | 85 |
| 1000 | 256 | 114 | 91 |
| 10000 | 64 | 70 | 111 |
| 10000 | 128 | 122 | 182 |
| 10000 | 256 | 197 | 278 |

## 9. Default Parameter Recommendation

Recommended default:

- `efConstruction=128`
- `efSearch=128`

Reasoning:

- `efConstruction=64` gives the best VSET latency, but 10K L1 recall drops to `0.989900`.
- `efConstruction=128` recovers 10K L1 recall to `0.994300`, matching or slightly exceeding the observed hnswlib `efConstruction=256` recall (`0.994100`) in this run.
- `efConstruction=256` roughly doubles 10K L3 VSET index/addPoint cost versus 128 (`1164us` vs `623us`) without recall gain in this dataset.
- `efSearch=128` remains the quality default from P0-C. `efSearch=64` is still a latency profile only, because P0-C measured 10K recall@10 around `0.949400`.

Operational profiles:

| Profile | efConstruction | efSearch | Use |
|---|---:|---:|---|
| Default/quality | 128 | 128 | Balanced recall and add/search cost. |
| Latency/add-heavy | 64 | 128 | Only if ~0.99 recall@10 at 10K is acceptable. |
| High-build-quality diagnostic | 256 | 128 | Not recommended as default; expensive add path without measured recall gain here. |

## 10. Next Stage Decision

Do not continue with broad hnswlib internal optimization yet.

Recommended next step: keep parameter tuning as the immediate P0 action and change the default `efConstruction` from 256 to 128, then rerun the formal matrix. If the product requires much higher ingest throughput after that, move to P1 batch add, because the dominant work is per-point base-layer candidate expansion and neighbor selection.

Batch add should be preferred before rewriting hnswlib internals because:

- The measured bottleneck is core HNSW construction work, not KVLite wrapper overhead.
- A source-level hnswlib optimization would need to touch search/heuristic internals.
- Batch VSET can amortize executor/server overhead and may enable future index-side batching without changing RESP semantics immediately.

Do not prioritize:

- resize/reserve work for this case (`resize_count=0`);
- vector copy/memory-write work (below 1us);
- visited-list allocation work (below 1us);
- Faiss as product main path.

