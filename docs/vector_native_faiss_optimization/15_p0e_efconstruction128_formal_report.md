# P0-E efConstruction=128 Formal Report

Run id: `full_e2e_p0e_efconstruction128_20260624T000000Z`

Result directory:

`results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0e_efconstruction128_20260624T000000Z/`

## 1. Scope

P0-E changes only the default HNSW construction parameter:

- `vector_hnsw_ef_construction = 128`
- Keep config file / command line override support.
- No RESP protocol change.
- No HNSW algorithm rewrite.
- No batch, SIMD, quantization, or sharding work.

Benchmark parameters:

- L3 full e2e: `1K / 10K / 100K`
- L1 index-only recall: `10K / 100K`
- backend: `hnswlib_hnsw` vs `faiss_hnsw`
- `M=32`
- `efConstruction=128`
- `efSearch=128`
- `dim=64`
- `topK=10`

L1 and L3 are reported separately.

## 2. Modified Files

Production code:

- `src/core/kvs_config.c`
- `include/kvs_vector.h`

Report / result files:

- `docs/vector_native_faiss_optimization/15_p0e_efconstruction128_formal_report.md`
- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0e_efconstruction128_20260624T000000Z/`

Production behavior was modified: the default `vector_hnsw_ef_construction` is now `128`.

Override behavior remains unchanged:

- config file key: `vector_hnsw_ef_construction=<N>`
- command line key: `--vector_hnsw_ef_construction <N>`

## 3. Build

Build command:

```bash
make clean && make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j$(nproc)
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

## 4. Default Parameter Confirmation

The production defaults now resolve to:

| File | Value |
|---|---:|
| `src/core/kvs_config.c` | `cfg->vector_hnsw_ef_construction = 128` |
| `include/kvs_vector.h` | `KVS_VECTOR_HNSW_EF_CONSTRUCTION 128` |

The L3 run configs and backend stats also confirm `backend_ef_construction=128`.

## 5. L3 Full E2E Results

L3 path: real `kvstore` server, TCP/RESP text `VSET` / `VSEARCH`, hash embedding provider, executor/handler, vector backend, metadata/response. Initial capacity equals dataset size, so `resize_count=0`.

| Dataset | Backend | VSET avg/p95/index us | VSEARCH avg/p95/index us | Status |
|---:|---|---:|---:|---|
| 1K | hnswlib_hnsw | 560 / 770 / 483 | 540 / 720 / 405 | pass |
| 1K | faiss_hnsw | 140 / 190 / 64 | 100 / 110 / 37 | pass |
| 10K | hnswlib_hnsw | 720 / 900 / 623 | 680 / 760 / 588 | pass |
| 10K | faiss_hnsw | 220 / 290 / 130 | 340 / 220 / 301 | pass |
| 100K | hnswlib_hnsw | 900 / 1180 / 794 | 870 / 1060 / 762 | pass |
| 100K | faiss_hnsw | 540 / 830 / 440 | 160 / 150 / 90 | pass |

Raw data:

- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0e_efconstruction128_20260624T000000Z/raw.csv`
- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0e_efconstruction128_20260624T000000Z/summary.md`

## 6. hnswlib vs Faiss Gap

Current P0-E `hnswlib_hnsw / faiss_hnsw` L3 avg latency ratios:

| Dataset | VSET avg ratio | VSET index ratio | VSEARCH avg ratio | VSEARCH index ratio |
|---:|---:|---:|---:|---:|
| 1K | 4.00x | 7.55x | 5.40x | 10.95x |
| 10K | 3.27x | 4.79x | 2.00x | 1.95x |
| 100K | 1.67x | 1.80x | 5.44x | 8.47x |

Compared with P0-D `efConstruction=256` on the shared 1K/10K L3 matrix:

| Dataset | Operation | P0-D 256 hnsw/faiss avg ratio | P0-E 128 hnsw/faiss avg ratio | hnswlib avg change |
|---:|---|---:|---:|---:|
| 1K | VSET | 4.50x | 4.00x | -22.2% |
| 10K | VSET | 4.50x | 3.27x | -42.9% |
| 1K | VSEARCH | 5.38x | 5.40x | +25.6% |
| 10K | VSEARCH | 7.44x | 2.00x | +1.5% |

The gap clearly shrinks for the target bottleneck, L3 `VSET`. `VSEARCH` is not the primary effect of `efConstruction` and shows run-to-run/backend variability, especially for Faiss.

## 7. L1 Index-Only Recall

L1 path: direct index API on fixed binary float datasets with exact flat ground truth. This is separate from L3.

Formal P0-E `efConstruction=128` results:

| Dataset | Backend | Add avg/p95 us | Search avg/p95 us | recall@10 | Status |
|---:|---|---:|---:|---:|---|
| 10K | hnswlib_hnsw | 122 / 166 | 120 / 126 | 0.994300 | acceptable |
| 10K | faiss_hnsw | 181 / 268 | 172 / 182 | 0.996300 | acceptable |
| 100K | hnswlib_hnsw | 291 / 457 | 371 / 398 | 0.891500 | quality risk |
| 100K | faiss_hnsw | 652 / 1065 | 458 / 507 | 0.922700 | quality risk |

Raw data:

- `results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0e_efconstruction128_20260624T000000Z/l1_recall_efconstruction128.csv`

Reference check, not part of the formal 128 matrix:

- `100K hnswlib_hnsw efConstruction=256, efSearch=128`: recall@10 `0.899700`, add avg `530us`, search avg `375us`

Interpretation:

- At 10K, recall remains acceptable and matches the P0-D conclusion.
- At 100K, recall is not acceptable for a high-recall quality gate, but the efConstruction=256 reference only improves hnswlib recall from `0.891500` to `0.899700`. The 100K quality issue is therefore not primarily caused by lowering `efConstruction` from 256 to 128.
- If 100K recall must be near the 10K level, tune `efSearch` / `M` or add a separate high-quality profile before treating the 100K quality gate as closed.

## 8. Recommendation

Adopt `efConstruction=128` as the default.

Reasoning:

- It directly reduces the measured P0 bottleneck: hnswlib L3 `VSET`.
- On the shared 10K matrix, hnswlib `VSET` avg latency improves from `1260us` at 256 to `720us` at 128.
- 10K L1 recall remains `0.994300`, matching the previous acceptable range.
- 100K recall is a real quality risk, but raising only `efConstruction` back to 256 does not materially fix it and roughly doubles 100K index-only add cost (`291us` -> `530us` in the reference run).

Recommended operational stance:

| Profile | efConstruction | efSearch | Use |
|---|---:|---:|---|
| Default | 128 | 128 | Balanced ingest cost and 10K recall. |
| High-build-quality diagnostic | 256 | 128 | Diagnostic only; not enough to fix 100K recall alone. |
| Future high-recall profile | 128 or 256 | >128 | Needs a separate P1/P2 quality sweep. |

## 9. Next Stage

Enter P1 batch add for ingest performance.

Rationale:

- P0-D showed the VSET bottleneck is hnswlib `addPoint()` internals, mainly `searchBaseLayer()` and `getNeighborsByHeuristic2()`.
- P0-E reduces the default construction work without changing the protocol or algorithm.
- Further major ingest improvement should come from batching/amortization before attempting hnswlib internals rewrites.

Parallel follow-up: keep a separate 100K recall quality track. The 100K recall result should not block adopting `efConstruction=128`, but it should block claiming the 100K high-recall target is solved.
