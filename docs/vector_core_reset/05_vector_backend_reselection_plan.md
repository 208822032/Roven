# Vector Backend Reselection Plan

Date: 2026-06-24

## 1. Decision Frame

KVLite should not become a Faiss, hnswlib, or USearch wrapper.

The product boundary is:

```text
KVLite owns storage semantics.
Backends own vector computation.
Graph indexes are derived and rebuildable.
```

Current P0-D conclusion:

- hnswlib add latency is dominated by `addPoint` internals.
- Main costs are `searchBaseLayer` and `getNeighborsByHeuristic2`.
- resize, wrapper, vector copy, visited-list pool, and allocation are not the main bottleneck.
- Further hnswlib internal tuning has low product leverage.

## 2. Current Backend Roles

`hnswlib_hnsw`:

- Current minimal HNSW backend.
- Good for keeping product HNSW behavior alive.
- Not a long-term profiling target.

`faiss_hnsw`:

- Baseline/fallback for comparison.
- Useful for measuring whether a change is KVLite-side or engine-side.
- Not the main product path.

`faiss_flat`:

- Exact/baseline path.
- Useful for recall checks and debugging.

Native flat/SQ8/native HNSW:

- Compatibility/debug surface.
- Native HNSW should not be revived as-is without a self_hnsw design pass.

USearch:

- Source is present at `third_party/USearch-2.25.3/`.
- This stage only audits it.
- Do not integrate it in this phase.

## 3. Backend Selection Criteria

Evaluate future backends on:

- clean fit to Vector Compute Kernel ops;
- reserve/capacity behavior;
- record-id mapping simplicity;
- add latency;
- search latency;
- recall under KVLite workloads;
- delete/mark_delete behavior;
- rebuild cost from records;
- snapshot feasibility as optional cache;
- dependency/build complexity;
- ability to keep KVLite AOF/RDB/replication backend-neutral.

Do not select based on:

- availability of language bindings;
- broad index zoo;
- GPU capability;
- third-party persistence format;
- benchmark-only internal counters;
- ability to replace KVLite command/storage semantics.

## 4. Three-phase Route

### Phase 1: Kernel Boundary Cleanup

Goal:

- Stabilize manager/compute boundaries before adding or switching engines.

Tasks:

- Keep `hnswlib_hnsw` as current default-capable HNSW backend.
- Keep `faiss_hnsw` and `faiss_flat` as baselines.
- Add or document backend-neutral ops: `reserve`, `capacity`, `delete/mark_delete`, `reset`.
- Collapse VINFO into manager info plus backend-neutral compute info.
- Remove direct backend `stderr` logging.
- Keep only high-level backend add/search timing.
- Archive hnswlib internal P0 profiling fields.

Exit criteria:

- VSET/VSEARCH/VDEL/VINFO behavior does not change.
- AOF/RDB/replication remain record-based.
- Backend selection is through the same ops table.
- No third-party graph format is required for restore.

## 5. Phase 2: self_hnsw Prototype

Goal:

- Build a KVLite-owned compute kernel to validate the target architecture.

Design:

- New backend name: `self_hnsw`.
- Use KVLite record id as graph label.
- Use compact graph memory independent from vector records.
- Use reusable visited/search contexts.
- Use KVLite distance facade and SIMD where available.
- Support `reserve`, `capacity`, `add`, `search`, `info`, `reset`.
- Start with manager logical delete plus rebuild; add `mark_delete` later only if clean.

Do not do:

- Do not copy hnswlib source into `src/`.
- Do not port Faiss index zoo.
- Do not change VSET/VSEARCH protocol.
- Do not change AOF/RDB/replication.

Evaluation:

- Compare against `hnswlib_hnsw`, `faiss_hnsw`, and `faiss_flat`.
- Use record-level rebuild tests.
- Use recall against flat baseline.
- Keep benchmark output backend-neutral.

## 6. Phase 3: usearch_hnsw Adapter Trial

Goal:

- Evaluate USearch as a compute backend without letting it own KVLite semantics.

Adapter mapping:

- `init` -> `usearch_init`
- `reserve` -> `usearch_reserve`
- `capacity` -> `usearch_capacity`
- `add` -> `usearch_add(record_id)`
- `search` -> `usearch_search`
- `delete` -> `usearch_remove`, optional in first trial
- `info` -> size/capacity/connectivity/expansion/memory
- `destroy` -> `usearch_free`

Constraints:

- Do not enable USearch multi-key by default.
- Do not expose USearch scalar quantization as KVLite product behavior in the first adapter.
- Do not use USearch save/view as durable KVLite persistence.
- Do not pull in non-C/C++ language bindings.
- Keep adapter isolated under `src/vector/`.

Evaluation:

- Build complexity.
- Add/search latency.
- Recall.
- Delete behavior.
- Memory footprint.
- Reserve behavior.
- Failure handling and rebuild path.

## 7. Phase 4: Backend Decision

Decision options:

- Keep `hnswlib_hnsw` if it remains simplest and acceptable.
- Promote `self_hnsw` if it gives better control and maintainability.
- Promote `usearch_hnsw` if it gives clear performance/feature value with acceptable dependency risk.
- Keep `faiss_hnsw` as fallback/baseline unless product requirements change.

Promotion requirements:

- Same VSET/VSEARCH/VDEL/VINFO semantics.
- Same record-based persistence and replication.
- Clean rebuild from records.
- Stable failure handling.
- No mandatory third-party snapshot.
- Backend-neutral VINFO.
- Repeatable benchmark and recall results.

## 8. Cleanup Plan

Clean after Phase 1:

- hnswlib internal profiling hooks and docs references from active paths.
- backend-specific VINFO fields that duplicate generic backend info.
- direct `fprintf(stderr, ...)` logs in backend adapters.
- obsolete `native_hnsw` config messaging once replacement path is clear.
- old `CLEANUP_CANDIDATE` text scoring if no callers remain.

Keep:

- historical `docs/vector_native_faiss_optimization/` reports;
- benchmark runners that compare backend-level behavior;
- hnswlib and Faiss source trees;
- USearch source tree for audit/prototype work;
- record-based snapshot/restore logic.

## 9. Non-goals

This route does not:

- integrate USearch in the current stage;
- switch backend in the current stage;
- modify production code in the current stage;
- implement self_hnsw in the current stage;
- run benchmark in the current stage;
- delete hnswlib or Faiss;
- copy third-party code into `src/`;
- make KVLite a wrapper over any third-party vector engine.

## 10. Recommended Next Action

Next engineering step after this documentation stage:

1. Freeze the manager/compute boundary in code review terms.
2. Add missing backend ops only when implementation begins.
3. Clean old profiling/logging in a separate mechanical patch.
4. Prototype `self_hnsw` and `usearch_hnsw` behind the same ops table.
5. Decide backend promotion from reproducible add/search/recall/rebuild data.
