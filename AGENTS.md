# KVLite Vector Core Agent Guardrails

## 1. Project Baseline

Project:

```text
KVLite / 9.1-kvstore
```

Current vector architecture:

```text
manager:
  VSET / VSEARCH / VDEL / VINDEX / VINFO
  record store / key / scope / TTL / metadata / question / answer
  AOF / RDB / replication / rebuild / resolve / response

vector_core:
  record_id + vector -> record_id + score
  no scope / TTL / metadata / question / answer
  no response assembly
  no access to g_vector_state
```

Source of truth:

```text
record store is source of truth
core_flat / core_hnsw / faiss / hnswlib indexes are derived state
RDB/AOF save records, not core_hnsw graph
load / replay / rebuild reconstruct indexes from active records
```

## 2. Completed Baseline

Current completed stages:

```text
Stage 1: manager/core boundary split
Stage 2: core_flat
Stage 3: core_hnsw
Stage 4: benchmark baseline
Stage 4A: FAISS overfetch validity fix
Stage 4B: hnswlib overfetch fix and corrected benchmark
Stage 50: post-refactor audit
```

Current recommended next stage:

```text
Stage 5A: manager record_id fast resolve
```

## 3. Hard Boundaries

Core may only own:

```text
record_id
vector
score
index-local slot map
index-local deleted flags
index-local graph/search state
index-local memory/stat info
```

Manager must own:

```text
record store
scope
TTL
metadata
question / answer
deleted / active filtering
threshold filtering
response formatting
AOF / RDB / replication
rebuild orchestration
```

Forbidden unless explicitly requested:

```text
Do not change RDB/AOF format.
Do not change kvs_vector_index_ops_t signatures.
Do not replace faiss / hnswlib.
Do not delete old flat baseline.
Do not move src/vector directory.
Do not let core_flat/core_hnsw handle scope / TTL / metadata / question / answer / response.
Do not access g_vector_state from src/vector/core.
Do not process unrelated docs deletion or unrelated git diff.
```

## 4. External Backend Adapter Boundary

faiss and hnswlib adapters are compatibility / benchmark adapters.

They may still contain legacy manager-adapter behavior, but they must not be used as templates for new vector_core code.

Rules:

```text
Do not copy faiss/hnswlib adapter design into core_flat/core_hnsw.
Do not expand adapter access to manager/global state unless explicitly scoped.
Do not treat adapter debt as vector_core boundary regression.
If adapter debt is found, document it and isolate it.
```

## 5. Stage Discipline

Every stage must have exactly one primary goal.

For every stage:

```text
1. Modify only files needed for the stage.
2. Preserve public V command behavior.
3. Preserve response format.
4. Preserve manager/core boundary.
5. Preserve RDB/AOF compatibility.
6. Add focused tests when behavior or risk changes.
7. Add a stage report under docs/vector_core_reset/.
8. Report build, tests, static checks, modified files, and git status.
```

Do not mix optimization classes:

```text
P0: common low-risk manager/profiling/benchmark fixes
P1: localized performance improvements
P2: structural core/index redesign
```

A P0 stage must not include core_hnsw memory layout rewrite, graph redesign, rebuild algorithm rewrite, shard/partition work, or graph persistence.

## 6. Benchmark Interpretation Rules

Corrected Stage 4B recall results are trusted after overfetch fixes.

But current benchmark limitations must be respected:

```text
VSEARCH endpoint latency includes backend search, manager resolve, response formatting, embedding path, and socket overhead.
Do not infer exact backend latency without profiling.
Do not compare FAISS memory_bytes while it is reported as 0.
Do not treat hnswlib 100K RDB reload timeout as a root cause without attribution.
```

For future benchmark/profiling work, prefer splitting:

```text
backend_search_us
manager_resolve_us
response_assembly_us
raw_hit_count
resolved_hit_count
filtered_hit_count
```

## 7. Stage 5A Safety Invariants

Stage 5A target:

```text
kvs_vector_manager_resolve_hits():
  O(raw_topk * record_store_capacity)
  -> O(raw_topk)
```

Required safety checks for fast resolve:

```text
record_id -> slot lookup must be manager-owned.
slot must be in range.
record must exist.
record->id must equal hit.record_id.
record must be active.
record must not be deleted.
scope filtering must remain in manager.
threshold filtering must remain in manager.
response formatting must remain unchanged.
```

Slot reuse invariant:

```text
VDEL may release a slot.
Later VSET may reuse that slot.
A stale backend hit must never resolve to a different newer record.
Always verify record->id == hit.record_id.
```

Forbidden in Stage 5A:

```text
Do not modify src/vector/core/*.
Do not optimize core_hnsw memory.
Do not optimize core_hnsw rebuild.
Do not modify RDB/AOF.
Do not modify index ops signatures.
Do not change response format.
Do not expose record_id or slot in user-facing responses.
```

## 8. Validation Baseline

Preferred full build:

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

Core regression set:

```bash
tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh

tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
```

Stage-specific tests must be run after core regressions.

Static boundary checks:

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
grep -RIn "record_index" src/vector include 2>/dev/null
grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
```

## 9. Completion Report Format

Every stage report must include:

```text
1. Modified / added files
2. Whether production code changed
3. Current call chain
4. Before complexity or behavior
5. After design
6. Safety invariants
7. Correctness tests
8. Benchmark result if applicable
9. Static boundary check result
10. Build result
11. git status --short
12. Next suggested step
```
## Stage 5A Completed Invariant

Stage 5A has completed:

```text id="eh1afp"
Stage 5A: manager record_id fast resolve
```

Completed behavior:

```text id="8czcn9"
kvs_vector_manager_resolve_hits()
  before: O(raw_topk * record_store_capacity)
  after:  O(raw_topk) average via manager-owned record_id_map
```

New manager-owned state:

```text id="hl5i7j"
kvs_vector_store_t.record_id_map:
  record_id -> record_index
```

Ownership rules:

```text id="qa4kc9"
record_id_map belongs to manager / record store only.
record_id_map is memory state only.
record_id_map is not persisted into RDB/AOF.
record_id_map must be restored from active records or replay write paths.
```

Fast resolve safety invariants:

```text id="fwxbs2"
Every record_id fast resolve must verify:
  1. record_id exists in record_id_map
  2. record_index is in range
  3. record exists
  4. record->id == record_id
  5. record is ACTIVE
  6. record is not deleted
```

Slot reuse invariant:

```text id="yx36ke"
A stale backend hit must never resolve to a newer record after slot reuse.
The check record->id == hit.record_id is mandatory and must not be removed.
```

Forbidden regressions:

```text id="xwms3e"
Do not reintroduce per-hit full record store scan in kvs_vector_manager_resolve_hits().
Do not move record_id_map into vector_core.
Do not persist record_id_map into RDB/AOF.
Do not expose record_id or slot in VSEARCH responses.
```

## Next P0 Direction

The next recommended P0 stage is:

```text id="x9lhrl"
Stage 5B: VSEARCH profiling breakdown
```

Reason:

```text id="gfgj5o"
Stage 5A fixed the obvious O(raw_topk * capacity) manager resolve path.
Current benchmark still reports mostly endpoint VSEARCH latency.
Before core_hnsw memory/rebuild optimization, profiling must split:
  backend search
  manager resolve
  response assembly
```

Stage 5B must not optimize algorithms. It is instrumentation/profiling only.
Profiling Policy:
  Stable profiling counters are product observability and should be kept if overhead is low.
  Temporary debug-only counters must be removed or guarded after the stage.
  Profiling must not change VSEARCH response format.
  Profiling must not enter RDB/AOF.
  Profiling must not move manager semantics into vector_core.
  Stage 5B is instrumentation-only; it must not optimize algorithms.
  ## Stage 5B Completed Invariant

Stage 5B has completed:

```text
Stage 5B: VSEARCH profiling breakdown
```

Completed behavior:

```text
VSEARCH profiling now exposes:
  vsearch_total_*
  vsearch_backend_search_*
  vsearch_manager_resolve_*
  vsearch_response_assembly_*
  vsearch_raw_hit_count
  vsearch_resolved_hit_count
  vsearch_filtered_hit_count
```

Profiling ownership rules:

```text
Profiling counters are runtime observability state.
Profiling counters must not change VSEARCH response format.
Profiling counters must not enter RDB/AOF.
Profiling counters must not change score / topK / filtering behavior.
Profiling counters must not move manager semantics into vector_core.
```

Backend timing interpretation:

```text
vsearch_backend_search_us is measured at kvs_vector_index_search_current().
For legacy faiss/hnswlib adapters, manager resolve may happen inside ops->search().
Current backend_search_us is therefore computed as:

  ops_search_wall_us - manager_resolve_us

This is an estimated backend search timing, not pure index-kernel timing.
Do not use it as the only proof for core-level optimization.
```

Stable profiling fields should be kept if overhead is low:

```text
vsearch_total_*
vsearch_backend_search_*
vsearch_manager_resolve_*
vsearch_response_assembly_*
vsearch_raw_hit_count
vsearch_resolved_hit_count
vsearch_filtered_hit_count
```

Temporary debug-only fields must be removed or guarded:

```text
per-query hit traces
per-neighbor HNSW traces
visited node dumps
malloc stack traces
debug printf logs
one-off benchmark hack fields
```

## Next P0 Direction

The next recommended P0 stage is:

```text
Stage 5C: VINFO backend capacity / resize / memory stats completion
```

Reason:

```text
Stage 5A fixed manager resolve.
Stage 5B made VSEARCH internal timing visible.
Before P1 core_hnsw rebuild/memory work, VINFO must expose backend capacity / resize / memory stats so benchmark reports have enough attribution fields.
```

Stage 5C must be observability-only.

Forbidden in Stage 5C:

```text
Do not optimize core_hnsw graph.
Do not optimize core_hnsw memory layout.
Do not optimize core_hnsw rebuild.
Do not modify RDB/AOF.
Do not modify kvs_vector_index_ops_t signatures.
Do not change VSEARCH response format.
```
## Stage 5C Completed Invariant

Stage 5C has completed:

```text
Stage 5C: VINFO backend capacity / memory stats completion
```

Completed behavior:

```text
VINFO now exposes unified backend stats:
  backend_name
  backend_size
  backend_capacity
  backend_dim
  backend_metric
  backend_memory_bytes
  backend_memory_bytes_source
  backend_deleted_count
  backend_needs_rebuild
  backend_resize_count
  backend_reserve_count
  backend_resize_count_source
  backend_reserve_count_source
```

Memory source policy:

```text
backend_memory_bytes_source must be explicit:
  measured     actual KVLite-owned measured value
  estimated    conservative estimate with known formula
  unsupported  unavailable or not reliable

Do not treat memory_bytes=0 as real memory usage unless source explicitly says measured.
FAISS/hnswlib memory values must be marked estimated or unsupported unless truly measured.
```

Resize / reserve policy:

```text
backend_resize_count and backend_reserve_count may remain unsupported.
Do not fake unsupported counters as 0.
Do not add broad resize/reserve instrumentation in an unrelated stage.
```

Stage 5C forbidden regressions:

```text
Do not remove backend_memory_bytes_source.
Do not let benchmark compare memory_bytes without checking memory source.
Do not change VSEARCH response format.
Do not move backend stats into RDB/AOF.
Do not move manager-owned stats into vector_core.
```

## Stage 5D Scope

The next optional P0 hardening stage is:

```text
Stage 5D: VINFO / benchmark stats correctness hardening
```

Stage 5D is validation-only / reporting-hardening.

Allowed:

```text
multi-scale benchmark verification
benchmark JSON/Markdown format stabilization
VINFO field consistency checks
memory source consistency checks
test hardening
report cleanup
```

Forbidden:

```text
core_hnsw graph optimization
core_hnsw memory layout optimization
core_hnsw rebuild optimization
backend replacement
RDB/AOF format changes
kvs_vector_index_ops_t signature changes
VSEARCH response format changes
large resize/reserve counter implementation
```
## Stage 5D Completed Invariant

Stage 5D has completed:

```text
Stage 5D: VINFO / benchmark stats correctness hardening
```

Completed behavior:

```text
VINFO backend stats fields are machine-checkable.
VSEARCH profile fields are machine-checkable.
Benchmark JSON / Markdown output now separates:
  endpoint latency
  backend search estimated timing
  manager resolve timing
  response assembly timing
  backend memory bytes
  backend memory source
```

Timing source rule:

```text
backend_search_us is not pure kernel time.
It is currently:
  ops_search_wall_us - manager_resolve_us

Reports and benchmark output must label it as:
  estimated_from_ops_search_wall_us_minus_manager_resolve_us
```

Memory source rule:

```text
measured means actual KVLite-owned measured value.
estimated means formula-based estimate.
unsupported means unavailable or unreliable.

Do not compare memory_bytes without memory source.
Do not treat estimated memory as measured memory.
Do not treat unsupported or missing memory as 0.
```

P0 closeout rule:

```text
After Stage 5D, do not keep expanding P0 observability unless a correctness issue is found.
The next P0 stage, if any, must be baseline freeze only.
After baseline freeze, move to P1.
```

## Stage 5E Scope

Stage 5E is optional and should be the final P0 stage:

```text
Stage 5E: P0 stats/reporting final audit and baseline freeze
```

Allowed:

```text
freeze benchmark file naming
freeze benchmark JSON / Markdown schema
record final Stage 5A-5D validation matrix
decide whether multi-run median is needed
write final P0 closeout report
```

Forbidden:

```text
do not add new VINFO fields unless fixing a bug
do not add new profiling fields unless fixing a bug
do not modify production code
do not optimize core_hnsw
do not modify RDB/AOF
do not modify index ops signatures
do not change VSEARCH response format
```

After Stage 5E, next stage should be:

```text
Stage 6A: core_hnsw rebuild profiling-only audit
```
## Stage 6A Completed Invariant

Stage 6A has completed:

```text id="dyddcj"
Stage 6A: core_hnsw rebuild profiling-only audit
```

Completed profiling fields:

```text id="ad0buj"
rebuild_total_us
rebuild_manager_scan_us
rebuild_core_reset_us
rebuild_core_reserve_us
rebuild_core_add_total_us
rebuild_core_add_avg_us
rebuild_core_add_max_us
rebuild_core_add_count
hnsw_add_random_level_us
hnsw_add_greedy_search_us
hnsw_add_layer_search_us
hnsw_add_neighbor_select_us
hnsw_add_link_us
hnsw_add_vector_copy_us
```

Stage 6A conclusion:

```text id="gn374c"
core_hnsw rebuild bottleneck is inside kvs_hnsw_index_add().
Main hotspots:
  1. hnsw_add_layer_search_us
  2. hnsw_add_neighbor_select_us
  3. hnsw_add_link_us

Not primary bottlenecks:
  manager scan
  reset
  reserve
  vector copy
  random level
```

Stage 6B comparison rule:

```text id="cppa96"
Stage 6B before/after comparisons must use the same profiling mode.
Profiling overhead from clock_gettime exists, so do not compare profiled numbers against old non-profiled Stage 4B/5D numbers as absolute performance.
Use Stage 6A profiled 1K/10K numbers as the baseline.
```

Stage 6B optimization boundary:

```text id="seqaua"
Allowed:
  targeted optimization of add-time layer search
  targeted optimization of neighbor selection
  targeted optimization of bidirectional link maintenance
  small helper/data-structure improvements inside existing HNSW add path

Forbidden:
  memory layout rewrite
  graph persistence
  RDB/AOF format changes
  kvs_vector_index_ops_t signature changes
  manager scope/TTL/metadata/response changes
  external backend replacement
```
## Stage 6B Completed Invariant

Stage 6B has completed:

```text
Stage 6B: core_hnsw rebuild targeted optimization
```

Completed optimization:

```text
core_hnsw add-time link optimization
```

Stage 6B result:

```text
1K:
  rebuild_core_add_total_us: 99,273 -> 76,190
  hnsw_add_link_us: 20,170 -> 1,050
  recall@10: 1.0000 -> 1.0000

10K:
  rebuild_core_add_total_us: 1,369,458 -> 1,126,478
  hnsw_add_link_us: 205,229 -> 11,294
  recall@10: 0.9870 -> 0.9870
```

Stage 6B conclusion:

```text
link duplicate scan / worst-neighbor scan / reverse edge decision duplication has mostly been removed.
Remaining largest bottleneck is hnsw_add_layer_search_us.
```

Stage 6C direction:

```text
Stage 6C must focus on layer_search.
Do not continue optimizing link unless a correctness bug is found.
Do not start memory layout rewrite in Stage 6C.
Do not reduce ef_construction to get speed.
Do not trade recall for rebuild speed.
```

Stage 6C baseline:

```text
10K Stage 6B:
  rebuild_core_add_total_us: 1,126,478
  hnsw_add_layer_search_us: 796,544
  hnsw_add_neighbor_select_us: 267,797
  hnsw_add_link_us: 11,294
  recall@10: 0.9870
```
## Stage Planning Discipline

For any future Codex/agent prompt:

1. Follow the user-provided optimization roadmap strictly.
2. If a latest audit report exists, use its staged route as the source of truth.
3. If a completed stage report contains a "next suggested step", treat it only as a candidate unless it matches the roadmap/audit route.
4. Do not invent new stages, rename stages, or reorder stages based only on local benchmark interpretation.
5. If roadmap, audit report, and latest stage report conflict, stop and report the conflict instead of planning a new route.
6. Each prompt must contain only the current stage delta: goal, allowed files, forbidden changes, required commands, acceptance checks, and report requirements.
7. Stable constraints must live in `AGENTS.md`, not be repeated in every prompt.
## Stage Planning Discipline

For any future Codex/agent prompt:

1. Follow the user-provided optimization roadmap strictly.
2. If a latest audit report exists, use its staged route as the source of truth.
3. If a completed stage report contains a "next suggested step", treat it only as a candidate unless it matches the roadmap/audit route.
4. Do not invent new stages, rename stages, or reorder stages based only on local benchmark interpretation.
5. If roadmap, audit report, and latest stage report conflict, stop and report the conflict instead of planning a new route.
6. Each prompt must contain only the current stage delta: goal, allowed files, forbidden changes, required commands, acceptance checks, and report requirements.
7. Stable constraints must live in `AGENTS.md`, not be repeated in every prompt.
## Vector core / HNSW v2 stage discipline

KVLite vector module follows a strict KVmanager / Vector_core boundary.

KVmanager owns:

* VSET / VSEARCH / VDEL / VINDEX / VINFO semantics
* record store, key/scope, TTL, metadata, question, answer
* AOF / RDB / replication
* rebuild orchestration
* threshold / scope / TTL / active/deleted filtering
* response assembly

Vector_core owns only:

* record_id
* vector
* score
* index-local graph/search state

Vector_core must not access:

* g_vector_state
* manager-owned record_id_map
* scope / TTL / metadata / question / answer
* response assembly
* RDB/AOF serialization formats

Record store is the source of truth. HNSW graph is derived runtime state and must be rebuildable from active records. RDB/AOF save records, not HNSW graph.

core_flat is exact recall baseline / correctness oracle, not a FAISS IndexFlat performance target.

current core_hnsw is legacy / regression reference / fallback. Do not perform broad in-place refactors or Stage 7-style local optimization on current core_hnsw unless a prompt explicitly asks for a narrow audit.

core_hnsw_v2 is the new self-contained ANN candidate. It must remain independent until explicitly integrated.

Before product integration:

* v2 must pass index-only graph construction tests.
* v2 must pass index-only search recall gates.
* v2 must complete performance profiling and targeted optimization.
* v2 must not become the default backend before product hardening.

Do not change unless explicitly requested:

* RDB/AOF format
* kvs_vector_index_ops_t signature
* VSEARCH response format
* manager/core ownership boundary
* current core_hnsw behavior
* core_flat behavior
* FAISS/hnswlib adapters

FAISS / hnswlib are baseline only. They are used for recall, latency, memory, and parameter semantics comparison, not as replacement architecture.

For HNSW v2 performance stages:

* Do not use lower recall to claim speed improvement.
* Do not optimize SIMD before build/search attribution.
* Do not integrate VINDEX/VSEARCH before index-only performance is acceptable.
* Prefer FAISS/USearch-style proven HNSW paths:

  1. HNSW-style add-time layer search instead of full candidate construction.
  2. Bounded candidate/top heaps instead of O(ef) array scans.
  3. Metric-specialized distance backend after algorithmic bottlenecks are addressed.
  4. Compact graph/layout work only after recall and primary performance bottlenecks are understood.
* Each stage should optimize or audit one bottleneck class only.
* Every optimization stage must rerun v2 skeleton, graph construction, search recall, current core_hnsw regression tests, static boundary grep, and git diff --check.
