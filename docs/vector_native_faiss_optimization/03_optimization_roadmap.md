# Optimization Roadmap

Optimization work starts only after a formal native vs Faiss benchmark report exists. The 1K/64d smoke is not enough evidence for any performance change.

## Entry Criteria

Before opening P0/P1/P2 optimization work, the formal report must include:

1. index-only 10K/64d and 100K/64d results
2. server-path 10K/64d and 100K/64d results
3. recall@10 for ANN backends
4. backend availability and skipped-case reasons
5. HNSW `M`, `efConstruction`, and `efSearch`
6. profiling split between index and non-index overhead
7. raw CSV, summary, and run manifest for each benchmark layer

## P0

P0 is for the largest measured blocker that prevents a fair native vs Faiss comparison or dominates the formal result.

Possible P0 directions, depending on evidence:

1. backend selection and config correctness
2. recall parity and HNSW parameter alignment
3. severe native index add/search bottleneck
4. missing end-to-end profiling split
5. server-path overhead that hides index behavior

Do not start P0 from smoke results.

## P1

P1 is for follow-up improvements after P0 bottlenecks are measured and addressed.

Possible P1 directions:

1. secondary server-path overhead
2. memory reporting quality
3. backend parameter tuning after recall parity is established
4. expanded 512d experiments
5. benchmark runner ergonomics

## P2

P2 is for architecture-level changes that should not be mixed into benchmark setup or early optimization.

Possible P2 topics:

1. index snapshotting
2. async search workers
3. backend lifecycle redesign
4. larger benchmark framework cleanup
5. storage/layout changes that affect compatibility

P2 requires formal benchmark evidence, P0/P1 outcomes, a cost-benefit argument, and compatibility impact review.

## Diagnosis Rules

Use the formal report to choose work:

1. If index-only gap is large, focus on native index implementation.
2. If index-only is close but server-path gap is large, focus on KVLite server path.
3. If VSEARCH gap is larger than VSET, inspect search buffers, topK handling, distance/cache behavior, metadata lookup, and response serialization.
4. If 512d gap expands, inspect SIMD and memory layout.
5. If 100K gap expands, inspect HNSW graph layout, neighbor expansion, and candidate-set management.
6. If recall differs, do not compare latency until HNSW parameters and recall level are aligned.
