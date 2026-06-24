# P2 Architecture Plan

P2 target: make the `hnswlib_hnsw` main path durable, scalable, and operationally predictable after P0/P1 remove local bottlenecks.

## Entry Criteria

1. P0/P1 runs prove the remaining gap is architectural rather than simple profiling, allocation, or parameter work.
2. hnswlib recall/latency tradeoffs are documented at 10K and 100K, with a follow-up 512d plan.
3. Faiss HNSW and Faiss flat remain reproducible baselines.

## Work Items

1. hnswlib graph persistence
   - Persist graph state in RDB/AOF or a versioned sidecar index snapshot.
   - Avoid restart/load paths that rebuild HNSW from all records when a valid graph snapshot exists.
   - Define versioning, checksums, rebuild fallback, and rollback behavior.

2. Index shard
   - Split large indexes by hash, record range, or scope-aware partition.
   - Bound per-shard graph size to improve memory locality and tail latency.
   - Report shard-level recall, latency, and memory.

3. Shard executor
   - Add fan-out/fan-in execution for VSEARCH across shards.
   - Merge topK deterministically.
   - Keep executor queueing and response serialization observable.

4. Multi-scope partitioned index
   - Use dedicated indexes for high-cardinality or high-traffic scopes when justified.
   - Keep global fallback for low-volume scopes.
   - Define rebuild and deletion semantics across scope partitions.

5. SQ8/PQ quantization
   - Evaluate SQ8/PQ for memory pressure and cache locality.
   - Compare against Faiss quantized baselines as a research/control path.
   - Treat quantization as an explicit recall/latency/memory product choice.

6. Faiss baseline/fallback
   - Keep Faiss build and benchmark scripts healthy.
   - Use Faiss for regression detection and emergency fallback, not as the default vector engine.
   - Maintain a separate optional AVX2 Faiss build only for extreme-performance comparison.

## Exit Criteria

1. Architecture design covers persistence format, compatibility, fallback, and rollback.
2. Shard/executor design includes recall and tail-latency measurement strategy.
3. Benchmark reports continue separating index-only from server-path results.
4. Faiss baseline remains reproducible across generic and optional AVX2 builds.
