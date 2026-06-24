# Full E2E Text+Embedding Benchmark Report

Run id: `full_e2e_20260624T000000Z`

## 1. Current Faiss Path Audit

Reviewed files:

- `scripts/benchmark/run_hnswlib_vs_faiss_benchmark.py`
- `scripts/benchmark/run_hnswlib_vs_faiss_full_e2e.py`
- `tests/vector_benchmark.c`
- `tests/vector_index_only_formal.cpp`
- `results/vector_native_faiss/hnswlib_vs_faiss/`
- `docs/vector_native_faiss_optimization/09_hnswlib_vs_faiss_benchmark_report.md`

Audit conclusion:

| Question | Current `faiss_hnsw` answer |
|---|---|
| Does it use network? | Yes for the previous `server_path` run and the new `full_e2e` run; no for `index_only`. |
| Does it use RESP? | Yes for `server_path` and `full_e2e`; no for `index_only`. |
| Does it use real `VSET`/`VSEARCH` handlers? | Yes for `server_path` and `full_e2e`; no for `index_only`. |
| Does it use an embedding provider? | Yes. Server logs show `hash embedding provider ready dimension=64`. Current provider is `hash`, not ONNX. |
| Is it separated from hnswlib only by backend config? | Yes in server/full-e2e runs. The generated config changes `vector_index_type` between `hnswlib_hnsw` and `faiss_hnsw`; text data, query text, provider, topK, metric, and HNSW params are otherwise the same. |

The previous `server_path` benchmark was not a raw-vector-only path. It used `build/tests/bin/vector_benchmark`, which sends textual RESP `VSET` and `VSEARCH`; the server then generates vectors internally with `embedding_provider=hash`. The name `server_path` was underspecified, so this report fixes the layer naming.

## 2. L1/L2/L3 Benchmark Layers

| Layer | Name | Path | Current coverage |
|---|---|---|---|
| L1 | index-only benchmark | direct hnswlib/Faiss index API; no server, no RESP, no embedding | `tests/vector_index_only_formal.cpp` and `results/.../index_only/` |
| L2 | server-path vector benchmark | network/RESP/server path with pre-generated vectors and no server-side embedding | Not currently implemented; KVLite RESP API used here accepts text, not raw float vectors. |
| L3 | full e2e text+embedding benchmark | network -> RESP -> text VSET/VSEARCH -> server embedding -> backend -> metadata -> response -> network | previous `server_path` run and new `full_e2e` run |

Do not compare L1 and L3 in one primary performance table. L1 isolates index implementation. L3 is closer to user-visible semantic cache latency.

## 3. Full E2E Chain

The new full e2e runner exercises:

```text
vector_benchmark client
-> TCP network
-> RESP command
-> VSET/VSEARCH text command
-> kvstore command parser/handler
-> hash embedding provider inside server
-> executor / vector operation
-> vector_index_type backend: hnswlib_hnsw or faiss_hnsw
-> metadata lookup / result assembly
-> RESP response serialization
-> TCP network response
```

The benchmark uses `--text-mode semantic`, so request payloads are text questions/queries, not pre-generated float vectors.

## 4. Backend Difference Control

For both backends:

- `embedding_provider=hash`
- `embedding_dim=64`
- `vector_metric=ip`
- `vector_hnsw_m=32`
- `vector_hnsw_ef_construction=256`
- `vector_hnsw_ef_search=128`
- `topK=10`
- `scope=empty`
- same `vector_benchmark` client
- same TCP/RESP path
- same text generation mode

Only `vector_index_type` changes:

- `hnswlib_hnsw`
- `faiss_hnsw`

## 5. 1K Smoke Result

Result directory:

`results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_20260624T000000Z/`

| Dataset | Backend | Operation | QPS | Avg us | P95 us | Success | Errors | Index avg us | Status |
|---:|---|---|---:|---:|---:|---:|---:|---:|---|
| 1K | `hnswlib_hnsw` | VSET | 1420.36 | 700 | 960 | 1000 | 0 | 634 | pass |
| 1K | `hnswlib_hnsw` | VSEARCH | 7562.35 | 130 | 150 | 1000 | 0 | N/A | pass |
| 1K | `faiss_hnsw` | VSET | 6806.24 | 150 | 200 | 1000 | 0 | 85 | pass |
| 1K | `faiss_hnsw` | VSEARCH | 7743.29 | 130 | 70 | 1000 | 0 | N/A | pass |

1K smoke proves both backends can run through the full text+embedding path.

## 6. 10K Full E2E Result

| Dataset | Backend | Operation | QPS | Avg us | P95 us | Success | Errors | Index avg us | Status |
|---:|---|---|---:|---:|---:|---:|---:|---:|---|
| 10K | `hnswlib_hnsw` | VSET | 817.95 | 1220 | 1440 | 10000 | 0 | 1143 | pass |
| 10K | `hnswlib_hnsw` | VSEARCH | 5611.11 | 180 | 200 | 1000 | 0 | N/A | pass |
| 10K | `faiss_hnsw` | VSET | 3849.90 | 260 | 320 | 10000 | 0 | 191 | pass |
| 10K | `faiss_hnsw` | VSEARCH | 10540.74 | 90 | 90 | 1000 | 0 | N/A | pass |

Gap multiples, `hnswlib_hnsw avg_us / faiss_hnsw avg_us`:

| Dataset | Operation | Ratio |
|---:|---|---:|
| 1K | VSET | 4.67x |
| 1K | VSEARCH | 1.00x |
| 10K | VSET | 4.69x |
| 10K | VSEARCH | 2.00x |

## 7. Profiling Split

| Field | Current state |
|---|---|
| `embedding_avg_us` | Unavailable for current hash provider logs. Server confirms the provider exists, but `runtime_encode_count=0` and `embedding_encode_avg_us=0`; raw.csv leaves this empty. |
| `index_avg_us` for VSET | Available through `vset_hnsw_add_avg_us`; this measures backend add cost for both hnswlib and Faiss wrapper paths. |
| `index_avg_us` for VSEARCH | Unavailable. Server logs report `vsearch_search_count=0`; raw.csv leaves this empty. |
| `non_index_avg_us` | Computed only when index avg is available. For VSEARCH it is empty. |
| `response_avg_us` | Unavailable in this run; response build counters are zero on this path. |

Therefore, VSET index attribution is usable. VSEARCH embedding/index/non-index/response attribution is not yet trustworthy.

## 8. Why L3 Is Closer To User Path

L1 index-only answers how fast the underlying index implementation can add/search vectors. It excludes parsing, network, server scheduling, metadata, embedding, and response generation.

L3 full e2e text+embedding includes the real user-facing protocol and command path. Even with the lightweight `hash` provider, it validates that Faiss and hnswlib are compared through the same KVLite semantic-cache machinery rather than through standalone Faiss APIs.

## 9. Conclusions Not Yet Supported

This run does not prove:

- ONNX embedding performance parity, because provider is `hash`.
- VSEARCH index-only attribution inside the server path, because `vsearch_search_count=0`.
- Response serialization cost, because response profile fields are unavailable.
- Recall@10 for semantic text, because the full e2e text runner does not yet compute exact semantic ground truth.
- 100K full e2e behavior, because this stage intentionally stopped at 10K.

## 10. Follow-up Recommendations

1. Keep `full_e2e` as the official L3 result directory and naming convention.
2. Fix VSEARCH profiling on the normal RESP path before deeper search optimization.
3. Add hash embedding encode counters or explicitly mark hash provider as too cheap/unprofiled.
4. Add response serialization profiling for VSEARCH.
5. Keep Faiss as baseline/fallback; optimize hnswlib VSET add path first because L3 10K VSET is 4.69x slower than Faiss HNSW.
6. Run optional 100K L3 only after profiling fields are reliable, because otherwise the result will confirm latency but not explain it.

## Next Prompt

```text
在 KVLite 仓库中继续 full e2e P0 profiling 补齐。基于
docs/vector_native_faiss_optimization/10_full_e2e_text_embedding_benchmark_report.md
和 run_id full_e2e_20260624T000000Z，不做 HNSW 算法优化，完成：
1. 修复 VSEARCH 正常 RESP 路径的 index search profiling，填充 vsearch_search_count/avg/max；
2. 为 hash embedding provider 增加 encode_count/avg_us 统计，或在 VINFO/profile 中明确 hash provider unprofiled；
3. 补齐 VSEARCH response serialization profiling；
4. 重跑 full_e2e 1K/10K hnswlib_hnsw vs faiss_hnsw，并更新 raw.csv 和报告。
```

## P0 Profiling Follow-up

Follow-up report:

`docs/vector_native_faiss_optimization/11_full_e2e_p0_profile_report.md`

Run id:

`full_e2e_p0_profile_20260624T003000Z`

Result directory:

`results/vector_native_faiss/hnswlib_vs_faiss/full_e2e/full_e2e_p0_profile_20260624T003000Z/`

This follow-up fixed the missing L3 profile fields from this report:

- `vsearch_search_count` is now 1000 for every backend/size in the final run.
- `vsearch_search_avg_us` is populated for hnswlib and Faiss.
- hash embedding now reports `runtime_encode_count` and `embedding_encode_avg_us`.
- VSEARCH response serialization now reports `vsearch_response_build_count` and `vsearch_response_build_avg_us`.

Updated 10K L3 result:

| Operation | hnswlib avg us | Faiss avg us | Ratio | hnswlib index avg us | Faiss index avg us |
|---|---:|---:|---:|---:|---:|
| VSET | 1190 | 270 | 4.41x | 1103 | 190 |
| VSEARCH | 160 | 160 | 1.00x | 86 | 99 |

The current strongest attribution is VSET/add: the 10K hnswlib VSET gap is dominated by backend add/index cost, while VSEARCH is effectively tied in the latest L3 run. Metadata lookup and network push are still not split into standalone fields.
