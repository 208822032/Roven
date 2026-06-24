# P1 Optimization Plan

P1 target: after P0 makes hnswlib profiling and add/search overhead visible, improve throughput and latency without changing the product direction away from `hnswlib_hnsw`.

## Entry Criteria

1. P0 confirms effective `M=32`, `efConstruction=256`, and `efSearch=128`.
2. VSEARCH has trustworthy index/non-index profiling on the normal RESP path.
3. VSET add profile distinguishes hnswlib add, resize/reserve, metadata update, and response cost.
4. The P0 rerun preserves recall@10 reporting against `faiss_flat` ground truth.

## Work Items

1. Batch VSET / batch add
   - Add a batch import path that amortizes RESP parsing, embedding, metadata update, and index calls.
   - Prefer hnswlib bulk-friendly reserve/capacity setup before add.
   - Keep single VSET semantics unchanged.

2. Batch VSEARCH
   - Add optional batched search execution for multiple queries.
   - Reuse query buffers and result buffers across the batch.
   - Report batch size and tail latency separately from single-query latency.

3. Metadata lookup optimization
   - Make record-id to metadata lookup cache-friendly.
   - Keep hot fields compact for VSEARCH result assembly.
   - Avoid repeated string/key allocations during result materialization.

4. Scope-aware search
   - Avoid searching the full global graph when scope is selective.
   - Evaluate per-scope candidate filtering, scope partitions, or overfetch policies.
   - Report recall/latency by scope selectivity, not only empty scope.

5. HNSW parameter tuning
   - Sweep practical combinations of `M`, `efConstruction`, and `efSearch`.
   - Optimize for hnswlib recall/latency/memory, not Faiss absolute best.
   - Keep Faiss HNSW as a fixed baseline in every tuning table.

6. 512d SIMD / memory-layout analysis
   - Repeat representative runs at 512d after 64d is stable.
   - Quantify distance-kernel cost and memory bandwidth pressure.
   - Consider SIMD only where hnswlib/server wrapper still owns the hot computation.

## Exit Criteria

1. 10K and 100K server-path VSET/VSEARCH show measurable improvement over P0.
2. hnswlib recall@10 does not regress below the P0 baseline unless the tradeoff is explicit.
3. Batch results are reported separately from single-request server-path results.
4. Faiss remains a baseline/fallback column, not the optimization target implementation.
