# core_hnsw_v2 interview review

## One-line project summary

KVLite vector search was refactored from manager-coupled ANN code into a clean manager/vector_core architecture, then a self-contained `core_hnsw_v2` ANN backend was built, optimized, integrated, and made the default backend while preserving exact `core_flat` fallback. The legacy `core_hnsw` source has since been removed; its config string remains only as a compatibility alias to `core_flat`.

## Why replace old core_hnsw

The old current `core_hnsw` was useful as a first in-repo HNSW implementation, but later audits found practical limits:

- 10K/512d recall could drop badly under normal ef settings.
- Graph construction semantics had correctness risk around bidirectional edge acceptance.
- Rebuild performance work became incremental and hard to reason about.
- The old code had accumulated stage-specific profiling and optimization debt.

After v2 became the default and compatibility tests covered old configuration behavior, the project removed old `core_hnsw` source and kept only the public enum / config string as compatibility residue.

## Why core_flat first

`core_flat` gives an exact in-repo oracle:

- validates recall without relying on FAISS/hnswlib behavior
- proves manager/core boundary with the simplest possible backend
- provides a fallback backend
- makes regression tests deterministic

It is a correctness baseline, not a high-performance IndexFlat replacement.

## Why self-built core_hnsw_v2

The v2 path was built to keep vector_core strict:

- input: `record_id + vector`
- output: `record_id + score`
- no manager global state
- no scope/TTL/metadata/question/answer
- no response formatting
- graph is runtime derived state

This avoids copying legacy FAISS/hnswlib adapter coupling into the new core.

## HNSW insertion flow

1. Allocate a slot and store `record_id`.
2. Copy the vector into aligned v2-owned vector storage.
3. Pick a random level.
4. Use greedy search on layers above the new node level.
5. Use bounded ef_construction layer search from upper layer down to level 0.
6. Select neighbors from bounded candidates.
7. Add forward edges from the new node.
8. Best-effort add reverse edges to existing neighbors.
9. Prune reverse neighbor lists when they exceed M/M0.

Important semantic fix from the v2 design: new-node forward edges are independently retained; reverse edge acceptance does not cancel the new node's forward edge.

## HNSW search flow

1. Start from entry point.
2. Greedily descend upper layers.
3. At level 0, run bounded candidate/top heap search with `ef_search`.
4. Skip tombstoned/deleted slots.
5. Export top results in score-descending order.
6. Return only `record_id + score` to the manager.
7. Manager resolves records and applies user-level filters.

## Tombstone deletion design

`VDEL` maps to v2 `mark_delete(record_id)`. The graph slot stays in memory as a tombstone. Search skips tombstoned slots, and manager filtering still verifies active/deleted state.

Same-record update uses tombstone re-add:

- old slot marked deleted
- new vector added as an active slot
- record_id map points to the active slot

Rebuild removes tombstones because only active records are re-added.

## Rebuild / shadow replay design

The current product path uses synchronous rebuild. Earlier 9F skeletons modeled manager-owned rebuild state, delta capture, shadow replay, and active/shadow swap. Those are not active background-thread production behavior yet.

The intended future direction, if needed:

1. Build shadow graph from active records.
2. Capture write deltas during rebuild.
3. Replay deltas into shadow.
4. Swap shadow to active only after validation.
5. Retire old graph safely.

Stage 9G3 does not add background threads or epoch/refcount complexity.

## Why RDB/AOF do not save graph

The record store is the source of truth. HNSW graph topology is derived state.

Reasons:

- avoids persistence format churn
- keeps RDB/AOF compatible
- avoids graph/version corruption across implementation changes
- allows graph rebuild from records after load/replay

The tradeoff is cold start rebuild cost.

## Performance optimization path

The v2 path followed staged attribution rather than jumping to layout/SIMD:

| step | result |
|---|---|
| full candidate construction attribution | build was too slow, about 129s at 10K/512d |
| add-time layer search | reduced build by replacing full-candidate construction |
| ef_construction attribution | default changed from 128 to 256 to protect recall |
| bounded heap optimization | search candidate/top maintenance replaced array scans |
| AVX2 distance attribution | current backend uses AVX2; distance is visible but not the only issue |
| reverse_prune score reuse | stopped after targeted reuse; no layout/cache rewrite in 9G |

Known stable 10K data from the latest 9G3 scale gate:

- 10K/512d ef=256 recall@10 = 0.9860
- 10K/512d ef=512 recall@10 = 1.0000
- 10K build_us = 61,308,701
- 10K ef=256 search_avg_us = 2049.00
- duplicate edge = 0
- degree overflow = 0

The search average varies by machine and test harness; it should be described as approximately millisecond-level, not as a hard SLA.

## Interview follow-ups

Q: Why not persist the HNSW graph?

A: The record store is the source of truth. Persisting graph topology would add compatibility and corruption risk. Rebuild is expensive, but safer and easier to validate. If startup time becomes a product blocker, a graph snapshot format should be a separate compatibility stage.

Q: Why keep `core_flat` after adding ANN?

A: It is the exact oracle and fallback. It lets us measure recall and isolate ANN bugs from manager bugs.

Q: Why keep the old `core_hnsw` config string after deleting the source?

A: It avoids breaking old config files. The string is accepted, logged as removed, and resolved to `core_flat` fallback; it does not create a legacy HNSW index.

Q: Why did 50K/100K recall fall in Stage 9G3?

A: The 10K default gate passes, but sampled 50K/100K data shows the current v2 parameters/graph are not yet production-ready for those scales. That is a scale finding, not something Stage 9G3 should hide with optimization.

Q: What would you do next if 100K became required?

A: Start a new profiling/audit stage. Measure candidate quality, graph degree distribution, entry point drift, ef sensitivity, and build/search distance cost at 100K before choosing optimization or graph-design work.

## Project limits to state clearly

- synchronous rebuild is slow
- cold start needs rebuild
- high tombstone ratio needs rebuild
- no active background worker yet
- no 1M production validation
- 50K/100K recall requires a future dedicated audit before claiming readiness
