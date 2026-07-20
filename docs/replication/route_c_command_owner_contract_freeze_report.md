# Route C Re-entry Stage 1: Command Owner Contract Freeze

Date: 2026-07-20

## 1. Scope and conclusion

This stage adds a debug/test-enforced command-owner contract. It does not remove
locks or claim that the whole database is single-owner. Runtime mutation is
owned by the single command executor; startup load and post-drain shutdown are
explicit lifecycle owners. The async vector rebuild worker remains the only
registered legacy second-publisher exception.

No public protocol, RDB/AOF format, replication wire format,
`kvs_vector_index_ops_t`, HNSW algorithm, command queue, or response queue was
changed. No correctness lock was removed.

## 2. Owner API and phases

`include/kvs_owner_lock.h` and `src/core/kvs_owner_lock.c` now expose:

```text
kvs_command_owner_register_startup_thread()
kvs_owner_lock_set_executor_thread()
kvs_command_owner_register_shutdown_thread_after_drain()
kvs_command_owner_is_current_thread()
kvs_command_owner_phase()
kvs_command_owner_mutation_allowed()
kvs_command_owner_assert_mutation(site)
KVS_COMMAND_OWNER_ASSERT_MUTATION(site)
```

The explicit phases are `UNREGISTERED`, `STARTUP`, `RUNTIME`, and
`SHUTDOWN_DRAINED`. Identity is a concrete `pthread_t`, not a broad background
thread allowance. `kvstore.c` registers the main thread before startup load and
only registers the shutdown owner after `kvs_command_executor_stop()` has
drained and joined the executor.

`KVS_COMMAND_OWNER_ASSERTIONS` defaults to `0`. In release builds mutation
assertion call sites preprocess to no-ops. Debug/test builds set the macro to
`1` and abort with phase and site when an unauthorized thread enters a guarded
mutation path.

## 3. Guarded mutation boundaries

| Boundary | Assertion site |
| --- | --- |
| Ordinary KV write handler | `command_task.execute.write` |
| Lazy-expire write retry | `command_task.lazy_expire.retry` |
| TTL observed delete | `ttl.expire_observed_key` |
| TTL expire/scan apply | `executor.ttl_expire.apply`, `executor.ttl_scan.apply` |
| TCP slave mutation apply | `executor.replicated.apply` |
| VSET/VDEL commit | `vector.vset.commit`, `vector.vdel.commit` |
| Record ID allocation | `vector.record_id.allocate` |
| Active index mutation | `vector.active_index.*` |
| Rebuild delta append | `vector.rebuild_delta.upsert/delete` |

Records, question/scope maps, and the manager-owned record-ID map are changed
inside the guarded VSET/VDEL commit boundary. Lower-level containers were not
given generic thread exceptions.

## 4. Ordering contract

```text
same connection: strict order through the existing single-inflight rule
different connections: linearized by actual command-owner commit order
embedding worker: prepare task-private data only; no commit
TTL scanner: submit scan task only; no delete
replication receiver: submit replicated task only; no apply
record_id: allocated by the command owner
future replication sequence: allocated from command-owner commit order
```

No global embedding reorder buffer was added. Across connections, the owner's
actual commit order is the linearization order.

## 5. Legal exceptions

Startup RDB/AOF load is legal only on the registered startup thread before the
runtime executor becomes owner. Store destruction is legal only on the
registered main thread after executor drain. These lifecycle states do not
authorize arbitrary runtime background mutation.

The async rebuild worker has a separate TLS registration and assertion:

```text
kvs_command_owner_register_legacy_async_rebuild_thread()
kvs_command_owner_assert_legacy_async_rebuild(site)
kvs_command_owner_unregister_legacy_async_rebuild_thread()
```

It is asserted at worker entry and before active/shadow publication. This
identity does not satisfy `KVS_COMMAND_OWNER_ASSERT_MUTATION`; the focused test
proves that a registered rebuild worker aborts if it attempts an ordinary
runtime mutation. Existing rebuild, active-index and status locks remain.

## 6. Tests and builds

Passed:

```text
tests/route_c_command_owner_contract_test.sh
make ENABLE_EBPF=0 -j$(nproc)
make ENABLE_EBPF=0 EXTRA_CFLAGS=-DKVS_COMMAND_OWNER_ASSERTIONS=1 -j$(nproc)
assertions-enabled tests/vector_core_flat_correctness_test.sh
assertions-enabled tests/ttl/test_ttl_tcp_replication.sh (array backend)
assertions-enabled public VSET + VINDEX REBUILD ASYNC + STATUS
tests/vector_rebuild_async_worker_test.sh
tests/vector_rebuild_delta_capture_test.sh
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_v2_graph_construction_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
ASan/UBSan focused owner test (ASAN_OPTIONS=detect_leaks=0)
TSAN focused owner test
```

The focused owner test covers startup/runtime/shutdown identities, non-owner
fail-fast, the narrow rebuild exception, denial of ordinary mutation to that
exception, release no-op behavior, producer-only static checks, and required
assertion sites. The TCP test covers ordinary KV write/delete, TTL/PERSIST,
full sync and slave replay. The async server test completed with
`last_status:1` and no command-owner assertion failure.

Known unrelated regression results:

- `tests/ttl/test_ttl_mvp.sh` first required a test-only vector disable to pass
  current path validation, then failed because `TTL` immediately returned
  `-2` after `EXPIRE 1`. The temporary test edit was reverted.
- `tests/vector_core_hnsw_v2_product_integration_test.sh` failed its existing
  direct recovery case with `normalization mismatch ... norm_sq=0.312500000`.
  The focused async worker and public async rebuild runtime paths pass.
- LeakSanitizer cannot run under the current traced sandbox; ASan/UBSan passed
  with leak detection disabled. TSAN is available and passed the focused test.

## 7. Modified files

Production code:

```text
include/kvs_owner_lock.h
src/core/kvs_owner_lock.c
src/core/kvstore.c
src/core/kvs_command_task.c
src/core/kvs_command_executor.c
src/core/kvs_operation.c
src/vector/kvs_vector.c
```

Tests and documentation:

```text
tests/route_c_command_owner_contract_test.c
tests/route_c_command_owner_contract_test.sh
docs/replication/route_c_command_owner_contract_freeze_report.md
docs/development/kvstore_development_checklist.md
```

## 8. Remaining work

- Async rebuild remains a real second publisher; this stage registers rather
  than removes that exception.
- Existing correctness locks still protect rebuild, active index, shutdown
  fallback, queues, AOF and replication internals.
- A complete database single-owner proof and lock removal remain future work.
- Replication Stage 1A still owns HNSW capacity and failure-atomicity gates.
- Vector mutation replication, sequence, offset, ACK, backlog and full sync are
  not implemented here.

## 9. Workspace checks

`git diff --check` passes. Final `git status --short` contains this stage's
seven modified production files, the modified development checklist, the new
report, and the two new owner-contract tests. Pre-existing modified/untracked
`AGENTS.md`, replication audits, and interview-prep documents remain untouched.
No generated snapshot artifact remains in the repository root.

## 10. Final status

```text
command_owner_identity_api_added=yes
runtime_mutation_owner_assertions_added=yes
startup_owner_exception_frozen=yes
shutdown_owner_exception_frozen=yes
async_rebuild_legacy_exception_registered=yes
new_second_writer_introduction_blocked=yes
correctness_locks_removed=no
database_single_owner_verified=no
runtime_command_owner_contract_enforced=yes
ready_for_replication_stage1a_implementation=yes
```
