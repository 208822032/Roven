# Stage 12: weak rwlock correctness and stress gate

## Goal

Stage 12 validates whether the Stage 11 weak-lock mode satisfies the Route C preconditions for a later lock-removal decision. This stage does not delete `g_kv_data_rwlock`, does not delete `pthread_rwlock_destroy(&g_kv_data_rwlock)`, and does not make weak-lock the default.

The decision is correctness-first. Throughput or latency improvements are not accepted as a substitute for race-free execution, persistence consistency, replication consistency, TTL correctness, vector correctness, and clean shutdown.

## Baseline Audit

The Stage 12 audit confirmed:

- Executor-owned `g_kv_data_rwlock` command, replication, TTL, snapshot, and vector sites are routed through `kvs_owner_rwlock_*`.
- The only production naked `pthread_rwlock_*(&g_kv_data_rwlock)` calls are retained at the `dest_kvengine()` destroy boundary.
- `g_kv_data_rwlock` remains defined and initialized.
- `pthread_rwlock_destroy(&g_kv_data_rwlock)` remains present.
- `aof_state.data_rwlock` remains a separate non-target internal lock and was not replaced.
- Stage 10.14 and Stage 11 files are still uncommitted in the current worktree, so Stage 12 changes were made on top of that local baseline.

## Default Real-Lock Mode

Default mode remains real pthread rwlock mode:

- `KVLITE_ROUTE_C_WEAK_RWLOCK` unset or set to `0`.
- `kvs_owner_rwlock_rdlock()` calls `pthread_rwlock_rdlock()`.
- `kvs_owner_rwlock_wrlock()` calls `pthread_rwlock_wrlock()`.
- `kvs_owner_rwlock_unlock()` calls `pthread_rwlock_unlock()`.

This remains the rollback and production-safe path.

## Explicit Weak-Lock Mode

Weak-lock mode is explicit:

- Environment: `KVLITE_ROUTE_C_WEAK_RWLOCK=1`.
- Test/control API: `kvs_owner_lock_set_weak_mode(1)`.

Only executor owner thread calls may take the no-op candidate path. Non-executor calls warn and fall back to real pthread rwlock. Thread-local no-op depth keeps lock/unlock pairing bound to the actual acquire path.

## Owner Assertion, Fallback, and Stats

Stage 12 exposes owner rwlock stats at executor shutdown:

```text
owner rwlock stats: weak_mode=... total_calls=... rdlock_calls=... wrlock_calls=... unlock_calls=... executor_thread_hits=... non_executor_thread_calls=... strict_assertion_failures=... weak_noop_calls=... weak_fallback_calls=...
```

Observed examples:

- real-lock proactor c4: `weak_mode=0 total_calls=1082 rdlock_calls=158 wrlock_calls=383 unlock_calls=541 executor_thread_hits=1082 non_executor_thread_calls=0 strict_assertion_failures=0 weak_noop_calls=0 weak_fallback_calls=0`
- weak-lock proactor c4: `weak_mode=1 total_calls=1082 rdlock_calls=158 wrlock_calls=383 unlock_calls=541 executor_thread_hits=1082 non_executor_thread_calls=0 strict_assertion_failures=0 weak_noop_calls=1082 weak_fallback_calls=0`

Interpretation:

- Default mode did not no-op.
- Weak mode did no-op executor-owned lock sites.
- No non-executor fallback was observed in the successful proactor c4 rerun.

## Regression Matrix

`tests/weak_rwlock_correctness_stage12.sh` runs the core Route C regression suite twice:

| Mode | Switch | Result |
| --- | --- | --- |
| real-lock | unset `KVLITE_ROUTE_C_WEAK_RWLOCK` | PASS |
| weak-lock | `KVLITE_ROUTE_C_WEAK_RWLOCK=1` | PASS |

The suite includes rwlock gate, executor coverage, ntyco/eBPF audits, TTL scanner, shutdown, slave connect, replication fallback, replicated command task, TTL executor, lazy expire, master full sync, RDB snapshot, and AOF rewrite snapshot tests.

## Mixed Stress Matrix

`tests/weak_rwlock_mixed_stress_stage12.sh` runs both real-lock and weak-lock mode. It covers:

- KV: `GET`, `SET`, `DEL`.
- Client concurrency: c4, c8, c16 by default.
- TTL: `EXPIRE`, `TTL`, `PERSIST`, active expire, lazy expire, many expired keys.
- Vector/HNSW: `VSET`, `VGET`, `VSEARCH`, `VDEL`, repeated `VSET`, and post-`VDEL` search filtering.
- Persistence: `AOFREWRITE` while writes are active; RDB and AOF save/load via existing snapshot regressions.
- Replication: master full sync, slave apply, full-sync while writing, TTL/PERSIST/PEXPIREAT consistency via existing replication regressions.
- Network modes: `reactor`, `proactor`, and `ntyco` when the local environment can run their socket paths.

Results observed during Stage 12:

- `KEEP_STAGE12_TMP=1 TMP_DIR=/tmp/kvlite-stage12-mixed-rerun RUN_MODES=reactor CLIENT_LEVELS=4 REQUESTS_PER_CLIENT=20`: FAIL in real-lock mode because the server log contains `failed to acquire COW snapshot`; retained log: `/tmp/kvlite-stage12-mixed-rerun/real-lock/reactor/kvstore.log`.
- Earlier `RUN_MODES=reactor CLIENT_LEVELS=4 REQUESTS_PER_CLIENT=20` socket operations completed in real-lock and weak-lock mode, but the stricter Stage 12 log gate now treats the COW snapshot error as a failure.
- `RUN_MODES=proactor CLIENT_LEVELS=4 REQUESTS_PER_CLIENT=20 KEEP_STAGE12_TMP=1` socket operations completed in real-lock and weak-lock mode; logs kept under `/tmp/kvlite-stage12-weak-rwlock.555374`. Those retained logs also include `failed to acquire COW snapshot`, so the stricter gate treats the run as failed for Stage 13 readiness.
- A previous fuller mixed run reached reactor/proactor c4/c8/c16 coverage, but weak-lock proactor had one readiness failure without retained logs.
- A previous default run including ntyco did not return cleanly from the harness after real-lock ntyco; that is treated as a shutdown/harness blocker until reproduced with retained logs.
- Proactor c4 retained logs show `failed to acquire COW snapshot` in both real-lock and weak-lock mode during the mixed stress `AOFREWRITE` path. The script now treats this as a Stage 12 gate failure.

Performance samples from the retained proactor c4 rerun:

| Mode | QPS | avg ms | p95 ms | p99 ms |
| --- | ---: | ---: | ---: | ---: |
| real-lock reactor c4, blocked by COW snapshot failure | 35.8878 | 0.2685 | 1.299 | 1.639 |
| real-lock proactor c4 | 35.8398 | 0.2555 | 0.865 | 2.167 |
| weak-lock proactor c4 | 35.8230 | 0.3113 | 0.785 | 1.105 |

These numbers are observational only. They are not a readiness decision.

## TSAN Race Detection

`tests/weak_rwlock_tsan_stage12.sh` attempts an optional TSAN build using:

```text
make embedding ENABLE_EBPF=0 OPT_CFLAGS="-O1" EXTRA_CFLAGS="-fsanitize=thread -fno-omit-frame-pointer" EXTRA_LIBS="-fsanitize=thread"
```

In the current environment the TSAN build succeeded. The retained rerun failed in real-lock reactor mode:

- command: `KEEP_STAGE12_TMP=1 TSAN_TMP=/tmp/kvlite-stage12-tsan-rerun tests/weak_rwlock_tsan_stage12.sh`
- log: `/tmp/kvlite-stage12-tsan-rerun/stress/real-lock/reactor/kvstore.log`
- report: `WARNING: ThreadSanitizer: data race`
- location: global `g_shutdown_flag`
- writer: `handle_quit()` at `src/core/kvs_operation.c:1443`
- reader: `master_sync_server()` at `src/distributed/distributed.c:211`
- summary: `ThreadSanitizer: data race src/core/kvs_operation.c:1443 in handle_quit`

This race appears in real-lock mode, so it is not proof that weak-lock introduced the race. It still blocks Stage 13 because Stage 12 requires a clean race gate before any lock-removal decision.

## Consistency Results

Core regressions passed in both default real-lock and explicit weak-lock mode:

- RDB snapshot/save/load: PASS in the correctness suite.
- AOF rewrite/load: PASS in the correctness suite.
- Master full sync and replicated command task: PASS in the correctness suite.
- TTL executor, active scanner, and lazy expire: PASS in the correctness suite.
- Vector/HNSW static and socket checks: PASS in the successful reduced mixed stress runs.
- Shutdown owner regression: PASS in the correctness suite.

Mixed stress still has blockers:

- `AOFREWRITE` during socket mixed stress can log `failed to acquire COW snapshot`.
- ntyco-inclusive mixed stress did not complete cleanly in one run.
- TSAN reports a real-lock shutdown flag race.

## Shutdown, Stop, and Destroy

The destroy boundary remains conservative:

- `dest_kvengine()` still takes the real `pthread_rwlock_wrlock(&g_kv_data_rwlock)`.
- `pthread_rwlock_destroy(&g_kv_data_rwlock)` remains present.
- Weak-lock does not apply to init/destroy/shutdown boundary locks.

Stage 12 cannot claim complete shutdown safety because TSAN found a `g_shutdown_flag` race between `QUIT` and `master_sync_server()`, and a ntyco mixed-stress run did not return cleanly.

## Decision

Decision: `STAGE13_NOT_READY`.

Blockers:

1. TSAN reported a data race on `g_shutdown_flag` in real-lock reactor mode.
2. Mixed stress `AOFREWRITE` can fail to acquire the COW snapshot under proactor in both real-lock and weak-lock mode.
3. A default mixed run including ntyco did not complete cleanly, so ntyco shutdown/harness behavior needs retained-log reproduction.

Because Stage 12 found a TSAN data race and mixed-stress persistence/shutdown blockers, Stage 13 must not delete `g_kv_data_rwlock`, must not remove `pthread_rwlock_destroy(&g_kv_data_rwlock)`, and must not make weak-lock the default.

## Rollback

Rollback remains runtime-only:

1. Unset `KVLITE_ROUTE_C_WEAK_RWLOCK`.
2. Or set `KVLITE_ROUTE_C_WEAK_RWLOCK=0`.
3. Or call `kvs_owner_lock_set_weak_mode(0)` in tests/control code.

No source change is needed to return to real pthread rwlock behavior.

## Next Stage Recommendation

Before re-opening the Stage 13 removal/no-op permanence decision:

1. Fix or formally synchronize `g_shutdown_flag`.
2. Re-run `tests/weak_rwlock_tsan_stage12.sh` with retained logs and require zero TSAN reports.
3. Fix or classify the mixed-stress `failed to acquire COW snapshot` path.
4. Re-run `tests/weak_rwlock_mixed_stress_stage12.sh` across reactor/proactor/ntyco with c4/c8/c16 and retained logs.
5. Keep weak-lock explicit and rollback-safe until those blockers are closed.
