# KVLite Engineering Agent Guardrails

## 1. Purpose and Source Priority

This file contains current invariants and near-term implementation boundaries.
It is not a stage history. Historical measurements and decisions belong in
`docs/`.

Use evidence in this order:

```text
current source and actual diff
> executable tests and raw results
> completed-stage reports
> design/audit documents
> prompts, plans, and commit messages
```

Before changing code:

1. Read this file.
2. Inspect `git status --short`; preserve unrelated worktree changes.
3. Read the latest audit/execution plan for the requested subsystem.
4. Verify current source rather than assuming a report still describes HEAD.
5. If the user roadmap, latest audit, and current source conflict, report the
   conflict before inventing or reordering work.

Do not use a plan, prompt, test file, or stage name as proof that a capability
is implemented.

## 2. Current Baseline

Current product state:

```text
configuration:
  one validated effective kvs_config_t
  built-in defaults < config file < explicit CLI override

vector backends:
  core_hnsw_v2 = default ANN backend
  core_flat    = exact oracle and explicit fallback
  FAISS/hnswlib = benchmark/reference only, not product backends

vector lifecycle:
  records are source of truth
  HNSW graph and index snapshot are derived state
  valid snapshot may accelerate startup
  invalid/missing snapshot falls back to records-only rebuild
  async rebuild uses shadow build, delta replay, validation, publish, retire

benchmark:
  synthetic index-only gates exist
  real T2Ranking 10K/100K index-only and product prepared/text runs exist
  current 100K evidence is not a high-concurrency service SLA

replication:
  ordinary KV/TTL TCP replication exists with limited full-sync semantics
  vector VSET/VDEL replication is not implemented
  replication offset, ACK, durable backlog, partial resync, and promotion
  recovery are not implemented
```

Current references:

- `README.md`
- `docs/configuration/unified_configuration_audit.md`
- `docs/hnsw_index_snapshot/hnsw_s_final_consolidation_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r_final_product_rebuild_consolidation_report.md`
- `docs/vector_benchmark_readiness/README.md`
- `docs/replication/vector_replication_stage0_audit.md`
- `docs/replication/replication_stage1a_preimplementation_audit.md`
- `docs/replication/route_c_main_path_lock_elimination_audit.md`

## 3. Ownership and Source of Truth

### Manager / vector core

Manager owns:

```text
VSET / VGET / VDEL / VSEARCH / VINDEX / VINFO semantics
record store and stable record_id allocation
question / answer / scope / TTL / metadata
record_id -> record slot map
active/deleted/scope/threshold filtering
response assembly
RDB / AOF / replication integration
snapshot and rebuild orchestration
```

Vector core owns only:

```text
record_id
vector
score
index-local internal slot and id map
index-local deleted flags
graph/search state
index-local memory and profiling stats
```

Vector core must not access `g_vector_state`, manager `record_id_map`, scope,
TTL, metadata, question, answer, RESP assembly, or persistence/replication
formats.

### Command owner

The target runtime model is one command owner for database-visible commits:

```text
network / embedding / replication workers
  -> prepare immutable or task-private data
  -> submit to command owner
  -> command owner validates and commits in one order
  -> post-commit hands immutable work to AOF/replication/background owners
```

Embedding workers may prepare task-private embeddings. They must not allocate
record IDs, replication sequence numbers, or mutate manager/core state.

The current async rebuild worker is still a guarded second publisher. Do not
claim that database single ownership or main-path lock elimination is complete,
and do not remove correctness locks until publish/cleanup is returned through
the command-owner completion path and executable race tests pass.

Startup recovery and drained shutdown may be explicit non-runtime owners, but
they must not run concurrently with the runtime command owner.

## 4. Stable Interfaces and Formats

Unless the active stage explicitly authorizes and tests a migration, do not
change:

```text
public V command behavior or response formats
kvs_vector_index_ops_t signatures
RDB or AOF formats
vector snapshot manifest/snapshot formats
default vector backend or default HNSW parameters
manager/core ownership boundary
core_flat exact semantics
FAISS/hnswlib product role
```

RDB/AOF persist or replay records, not HNSW graph state. A graph snapshot is a
deletable cache, never the data truth.

External adapters are compatibility/benchmark code. Do not copy their
manager/global-state behavior into new vector core code.

## 5. Vector Correctness Invariants

### Identity and resolve

`record_id` is the stable identity. Manager record slots and HNSW internal
slots are separate and reusable according to different rules.

Every manager fast resolve must verify:

```text
record_id exists in manager record_id_map
record slot is in range
record exists
record->id == hit.record_id
record is ACTIVE and not deleted
scope and threshold still pass
```

The `record->id` check is mandatory: a stale index hit must never resolve to a
new record after manager slot reuse.

### Delete and update

```text
VDEL:
  manager changes product record state
  core marks an index-local tombstone

same-record update:
  tombstone old record_id
  append/re-add the new record_id/vector
```

A tombstone does not physically remove every edge. Re-add is not a complete
in-place graph reconnect. Rebuild is the compaction/recovery path.

### Capacity and failure atomicity

The current `ensure_add_capacity` path can call destructive
`core_hnsw_v2 reserve` on a populated product index. Do not treat that growth
path as lossless or add new dependencies on it; it is a Replication Stage 1A
blocker.

The required replacement is lossless allocate-copy-validate-publish growth.
VSET/VDEL must converge on:

```text
fallible prepare and capacity reservation
-> no-fail command-owner commit
-> derived post-commit work
```

On failure, do not consume IDs/slots or leave records, maps, graph, delta, AOF,
replication mutation, and response success describing different outcomes.
Partial HNSW add rollback is not sufficient if old adjacency was already
modified.

## 6. Snapshot and Rebuild Invariants

Snapshot:

- Validate backend, dimension, metric/parameters, records fingerprint, version,
  length, and checksum before use.
- Missing, incompatible, or corrupt snapshot must fall back to records-only
  rebuild.
- Snapshot load/write failure must not corrupt records.
- Snapshot write is best effort and must not roll back a successful publish.
- Do not add graph state to RDB/AOF as a shortcut.

Async rebuild:

```text
active serves VSEARCH
shadow builds from an immutable or guarded record view
VSET/VDEL mutations are captured after commit
delta is replayed to shadow
final record/vector/graph/generation/search validation passes
publish swaps active/shadow safely
old active becomes retired and is reclaimed after reader safety
```

Publish failure keeps old active. Cancel/retry/failure cleanup must not expose
shadow state. A worker may build private state, but the target owner model
requires database-visible publish and cleanup to return to the command owner.

Do not infer low write latency from background rebuild: current implementation
conservatively serializes some worker and VSET/VDEL access, uses an rwlock for
reader safety, and can hold active + shadow + retired memory.

## 7. Configuration Rules

Runtime modules consume the validated effective `kvs_config_t`; do not add new
module-local config parsers, production `getenv()` switches, or silent
hard-coded overrides.

Configuration rules:

```text
built-in defaults < main config file < explicit CLI override
parse and validate through the shared descriptor/schema
derive values only after merge
record per-field source metadata
reject unknown/duplicate/invalid explicit values
reject requested capabilities that were not compiled
resolve runtime paths from config_root/data_dir, not process cwd
```

Fields marked future/unsupported must fail when explicitly configured. Do not
pretend they are active.

Model path/version, embedding dimension, normalization, vector dimension, and
metric are one compatibility boundary. Do not reuse records, exact truth, graph
snapshot, or benchmark cache across incompatible settings.

## 8. Replication Roadmap and Hard Boundaries

Read the latest replication audit before implementation. The current intended
order is:

```text
freeze executable command-owner contract
-> Replication 1A: lossless HNSW growth and VSET/VDEL failure atomicity
-> Replication 1B: deterministic vector mutation codec/handoff/replica apply
-> Replication 2: offset-bound full sync, local index readiness, VSEARCH gate
-> Replication 3: ACK, bounded backlog, reconnect and partial resync
-> Replication 4: restart, promotion, offset persistence and compatibility
```

Do not skip Replication 1A and build wire replication on a commit path that can
partially fail.

Vector replication rules:

- Replicate the deterministic manager mutation after master embedding/commit.
- Carry stable record IDs and the committed float32 embedding.
- Do not replay public text VSET on replicas and do not rerun tokenizer/ONNX.
- Do not replicate HNSW graph as truth; replicas derive/load their local index.
- Replication sequence/offset is global stream order. Record generation is
  local rebuild/snapshot state. Never conflate them.
- Sequence allocation follows command-owner commit order.
- Replica apply runs through the command owner and the manager mutation/delta
  boundary, never directly from a network thread into record/core state.
- Apply failure must not advance `applied_offset` or ACK success.
- Gap, checksum failure, backlog overrun, or incompatible config triggers
  blocked/full resync behavior, not silent continuation.
- Only READY replicas may serve VSEARCH.
- eBPF is not the reliable vector replication transport. Reliable vector
  mutation/full sync uses the application-level TCP path.
- Keep Replication 1A-3 independent of AOF format. Any AOF/RDB migration belongs
  to an explicitly versioned compatibility stage.

Do not call the current transient TCP send queue a backlog.

## 9. Stage Discipline

Every stage has one primary engineering goal.

Required behavior:

1. Modify only files needed for that goal.
2. Preserve public behavior and stable formats unless explicitly in scope.
3. Add focused tests for changed behavior and failure paths.
4. Put the report in the subsystem route directory, not automatically in
   `docs/vector_core_reset/`.
5. Separate skeleton, implementation, integration, test existence, and proven
   result.
6. Record modified files, production-code impact, call chain, invariants,
   tests, build, static checks, evidence, risks, and `git status --short`.
7. Run `git diff --check`.

Do not mix unrelated optimization classes, redesign ownership while tuning a
local algorithm, or turn a cleanup stage into an interface migration.

A completed report's "next step" is a candidate, not authority to override the
latest audited roadmap. Do not invent stage names or reorder stages to fit a
local benchmark observation.

## 10. Benchmark and Evidence Rules

Always distinguish:

```text
index-only kernel build/search
product prepared-vector endpoint
public text + embedding endpoint
synthetic nearest-neighbor recall
real embedding nearest-neighbor recall
semantic qrels/relevance
snapshot load and cold start
sync versus async rebuild
single-client versus concurrent SLA
```

Rules:

- Do not trade recall for speed without saying so.
- Do not extrapolate 1K/10K to 100K or one seed to general behavior.
- Synthetic recall is not semantic quality.
- Test presence is not test execution.
- Report timing boundaries, profiling mode, dataset/model/seed/dimension,
  normalization, metric, topK, HNSW parameters, query count, and concurrency.
- Memory must state measured/estimated/unsupported and the accounting scope.
- Process RSS is not exact index memory.
- FAISS/hnswlib numbers are comparable only with aligned data, metric,
  normalization, parameters, and timing boundaries.
- Current real 100K results are useful evidence, not a high-concurrency SLA or
  universal default-parameter proof.

Prefer raw result files. If only a stage report survives, state that evidence
boundary explicitly.

## 11. Validation Baseline

Choose tests in proportion to the change. Default build:

```bash
make clean
make -j"$(nproc)"
```

Optional external/embedding builds when relevant:

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
make setup-embedding
make embedding -j"$(nproc)"
```

Core vector gates:

```bash
tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_v2_graph_construction_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
tests/vector_core_hnsw_v2_default_backend_test.sh
```

Lifecycle/config gates when relevant:

```bash
tests/vector_command_persistence_recovery_test.sh
tests/vector_index_snapshot_load_after_records_test.sh
tests/vector_index_snapshot_shadow_publish_test.sh
tests/vector_rebuild_async_worker_test.sh
tests/unified_config_stage1_test.sh
tests/unified_config_stage2_test.sh
tests/unified_config_stage3_test.sh
tests/unified_config_stage4_test.sh
```

Replication/ownership work must also run current TCP TTL replication tests,
new stage-specific vector replication/fault-injection tests, and sanitizer
coverage appropriate to the changed threads. Do not rely on deleted historical
Route C scripts as executable evidence.

Static vector boundary checks:

```bash
rg -n "g_vector_state|record_id_map" src/vector/core include/vector/kvs_vector_hnsw_v2.h
rg -n "scope|ttl|metadata|question|answer" src/vector/core
rg -n "kvs_vector_index_ops_t" include src/vector
```

Expected boundary greps must be interpreted, not blindly required to be empty
outside the vector core paths.

## 12. Worktree and Completion Report

Do not revert, delete, stage, or reformat unrelated user changes. Do not commit
unless explicitly requested.

Final reports should be concise and include:

```text
goal and actual outcome
modified files and production-code impact
current call chain / ownership change
invariants and failure behavior
build/tests/static checks and whether they were run now
benchmark result and evidence boundary, if applicable
remaining risks and next audited step
git status --short
```
