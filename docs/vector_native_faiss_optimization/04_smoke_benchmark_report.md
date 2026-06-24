# Dependency and Runner Readiness

## 1. Goal

This stage checks native vs Faiss smoke-runner readiness. It does not run formal 10K/100K benchmarks, does not optimize native HNSW, and does not fabricate unavailable backend results.

## 2. Dependency Audit

| Item | Status | Evidence / Reason |
|---|---|---|
| `third_party/hnswlib` | unavailable | Directory exists but contains no `hnswlib/hnswlib.h` file. |
| `third_party/faiss` | unavailable | Directory exists but contains no Faiss headers and no `build/faiss/libfaiss.a`. |
| `ENABLE_HNSWLIB` | default-on in Makefile | `ENABLE_HNSWLIB ?= 1`, but build requires populated `third_party/hnswlib`. |
| `ENABLE_FAISS` | default-off in Makefile | `ENABLE_FAISS ?= 0`; when enabled, build expects Faiss headers and static library under `third_party/faiss`. |
| Config parser | config file supports Faiss names | `vector_index_type=faiss_hnsw/faiss_flat` is accepted in config files. |
| CLI parser | incomplete for Faiss names | `--vector_index_type` accepts only `flat`, `hnsw`, `ivf_flat`, `sq8_flat`; smoke runners use config files instead. |

No dependency was downloaded or initialized in this stage.

## 3. Backend Availability

| Backend | Build / Smoke Status | Reason |
|---|---|---|
| `native_flat` | available for server-path smoke | Built with `ENABLE_HNSWLIB=0 ENABLE_FAISS=0`; selected by temporary config `vector_index_type=flat`. |
| `hnswlib_hnsw` | unavailable | `third_party/hnswlib` is empty or missing `hnswlib/hnswlib.h`. |
| `faiss_flat` | unavailable | `third_party/faiss` is empty or missing headers and `build/faiss/libfaiss.a`; default `ENABLE_FAISS=0`. |
| `faiss_hnsw` | unavailable | Same Faiss dependency/build limitation. |
| `native_hnsw` | unavailable by design | Current code reports `native_hnsw` removed; use `hnswlib_hnsw` when hnswlib is available. |

## 4. Runner Files

New runner scripts:

```text
scripts/benchmark/run_vector_index_only_smoke.sh
scripts/benchmark/run_vector_server_path_smoke.sh
```

Smoke output directories:

```text
results/vector_native_faiss/index_only_smoke/
results/vector_native_faiss/server_path_smoke/
```

## 5. Index-only Smoke Result

Output:

```text
results/vector_native_faiss/index_only_smoke/raw.csv
results/vector_native_faiss/index_only_smoke/summary.md
```

| Backend | Status | Reason |
|---|---|---|
| `native_flat` | unavailable | No KVLite index-only binary currently consumes the fixed `dataset.bin` / `queries.bin` / `manifest.json` directly. |
| `hnswlib_hnsw` | unavailable | hnswlib dependency missing; no manifest-compatible index-only runner. |
| `faiss_flat` | unavailable | Faiss dependency missing; no manifest-compatible index-only runner. |
| `faiss_hnsw` | unavailable | Faiss dependency missing; no manifest-compatible index-only runner. |

Existing `tests/vector_search_bench.c` generates its own mock vectors. It was not used as a pass result because doing so would violate the fixed dataset/manifest requirement.

## 6. Server-path Smoke Result

Output:

```text
results/vector_native_faiss/server_path_smoke/raw.csv
results/vector_native_faiss/server_path_smoke/summary.md
results/vector_native_faiss/server_path_smoke/logs/server_*.log
```

| Backend | Operation | Requests | Success | Failed | Errors | Status |
|---|---|---:|---:|---:|---:|---|
| `native_flat` | VSET | 1 plus 1000 prepare | 1 | 0 | 0 | pass |
| `native_flat` | VSEARCH | 100 | 100 | 0 | 0 | pass |
| `hnswlib_hnsw` | VSET/VSEARCH | 0 | 0 | 0 | 0 | unavailable |
| `faiss_flat` | VSET/VSEARCH | 0 | 0 | 0 | 0 | unavailable |
| `faiss_hnsw` | VSET/VSEARCH | 0 | 0 | 0 | 0 | unavailable |

The server-path runner uses a temporary config file and real `kvstore` server. The smoke uses `smoke_scope` to avoid ambiguity in the current `VSEARCH query [scope] [topk] [threshold]` command form when scope is empty.

The runner had to be executed outside the sandbox for local socket bind/connect. The sandboxed attempt failed with `PermissionError: [Errno 1] Operation not permitted` during socket creation.

## 7. Current Non-conclusions

This stage still cannot conclude:

1. native vs Faiss latency or QPS parity.
2. hnswlib_hnsw vs faiss_hnsw performance.
3. recall@10 for any ANN backend.
4. 10K/100K scaling behavior.
5. index-only performance gap, because the manifest-compatible index-only runner is still missing.
6. formal server-path fairness, because the current server-path smoke still uses deterministic text replay rather than exact `dataset.bin` / `queries.bin` command replay.

## 8. Formal Benchmark Preconditions

Before entering 10K/64d formal benchmark:

1. Initialize or explicitly skip `third_party/hnswlib`.
2. Initialize or explicitly skip `third_party/faiss`, and build with `ENABLE_FAISS=1` only after headers/library are present.
3. Add a manifest-compatible index-only runner that consumes `dataset.bin` and `queries.bin` directly.
4. Add server-path replay that maps dataset rows and query rows one-to-one into VSET/VSEARCH commands.
5. Keep index-only and server-path raw/summary reports separate.
6. Add recall@10 ground truth, ideally exact flat.
7. Keep backend switching through temporary config files until the CLI parser accepts all backend names.

## 9. Suggested Next Commands

Re-run readiness smoke:

```bash
scripts/benchmark/run_vector_index_only_smoke.sh
scripts/benchmark/run_vector_server_path_smoke.sh
```

After dependencies and manifest-compatible runners are ready, generate formal 10K/64d data:

```bash
python3 scripts/benchmark/gen_vector_benchmark_dataset.py \
  --count 10000 --query-count 1000 --dim 64 --seed 42 \
  --metric ip --normalize --topk 10
```
