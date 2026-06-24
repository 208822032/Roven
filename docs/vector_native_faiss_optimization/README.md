# KVLite Native vs Faiss Optimization

This directory is the home for KVLite native vs Faiss benchmark design, smoke validation, formal benchmark reports, and follow-up optimization plans.

## Document Map

| File | Purpose |
|---|---|
| `00_benchmark_plan_review.md` | Audit of the previous benchmark assets and the `NOT_APPROVED` fairness decision. |
| `01_benchmark_methodology.md` | Reproducible dataset manifest, index-only benchmark, server-path benchmark, output formats, and formal test matrix. |
| `02_smoke_benchmark_report.md` | 1K/64d smoke validation result and current backend availability. |
| `03_optimization_roadmap.md` | P0/P1/P2 optimization entry criteria after formal benchmark evidence exists. |
| `04_smoke_benchmark_report.md` | Dependency audit and index-only/server-path runner readiness smoke results. |
| `05_formal_benchmark_report.md` | Formal 10K/64d and 100K/64d index-only/server-path benchmark report. |
| `06_p0_optimization_plan.md` | P0 plan for Faiss baseline recovery, efSearch correctness, profiling, and VSET add bottlenecks. |
| `07_p1_optimization_plan.md` | P1 plan for SIMD, layout, batch APIs, scope-aware search, metadata lookup, and HNSW tuning. |
| `08_p2_architecture_plan.md` | P2 plan for persistence, sharding, multi-executor, quantization, and Faiss fallback architecture. |

## Rules

Do not mix standalone Faiss API results with KVLite server-path latency or QPS results. They answer different questions and must be reported separately.

Benchmark work is split into two layers:

1. **Index-only benchmark**: compares backend index implementations directly. It does not use RESP, the KVLite server, network I/O, executor queues, or response serialization.
2. **Server-path benchmark**: compares the real user path through VSET and VSEARCH. Backends must be switched only through controlled configuration, while the RESP, handler, executor, metadata, and response path stay the same.

Optimization work should be driven by formal benchmark reports and grouped as P0, P1, and P2 tasks.
