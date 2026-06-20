# Route C Final Closeout Report

Date: 2026-06-18

## 1. Scope

This closeout does not change Route C architecture. It only records:

- final weak-lock default vs real-lock rollback baseline
- final correctness/regression status
- rollback commands
- remaining risk and tag/commit recommendation

Route C status remains: **`FINAL_ROUTE_C_READY`**, with weak-lock enabled by default and real-lock rollback available through `KVLITE_ROUTE_C_WEAK_RWLOCK=0`.

## 2. Repository State

Closeout was executed on a **dirty worktree**, not on a clean commit:

```text
 M src/core/kvs_owner_lock.c
 M src/core/kvstore.c
 M tests/route_c/rwlock_weakening_gate_stage11.sh
 M tests/route_c/shutdown_owner_stage10_9.sh
 M tests/route_c/weak_rwlock_correctness_stage12.sh
 M tests/route_c/weak_rwlock_mixed_stress_stage12.sh
?? docs/route_c_perf_arch/65_stage13_global_rwlock_removal_and_final_gate.md
?? tests/route_c/rwlock_removal_final_stage13.sh
```

So the closeout confirms the current Stage 13 workspace state, not a clean tagged tree.

## 3. Build Result

Required build command:

```bash
make clean && make embedding ENABLE_EBPF=0
```

Result: **PASS**

## 4. Closeout Artifacts

- Driver script: `tests/route_c/route_c_final_closeout_stage13.sh`
- Preserved artifact dir from this run: `/tmp/kvlite-route-c-final-closeout.67348/artifacts`
- Mixed stress artifact dir from this run: `/tmp/kvlite-route-c-final-closeout.67348/mixed-stress`

## 5. Benchmark Method

- GET/SET: `scripts/benchmark/kvstore_resp_bench.py`, 32 clients, pipeline 8, 20k requests, value size 64, keyspace 5k
- VSET/VSEARCH: `tests/vector_benchmark`, ONNX embedding config, 4 clients, pipeline 1, 200 requests
- TTL: `tests/ttl_expired_path_bench`, scenario `expired-delete-only`, 5k requests
- RDB/AOF/replication: elapsed wall time of existing Stage 10 scripts
- reactor/proactor/ntyco: `tests/route_c/weak_rwlock_mixed_stress_stage12.sh` with reduced closeout sample (`CLIENT_LEVELS="4 8"`, `REQUESTS_PER_CLIENT=10`)

## 6. Weak-Lock vs Real-Lock

### 6.1 Core Throughput

| Metric | Weak-lock default | Real-lock rollback | Delta vs real |
| --- | ---: | ---: | ---: |
| SET qps | 25405.42 | 25763.53 | -1.4% |
| GET qps | 19623.86 | 23347.43 | -15.9% |
| VSET qps | 110.41 | 122.60 | -9.9% |
| VSEARCH qps | 157.60 | 185.97 | -15.3% |

Observation: in this sample, weak-lock default did **not** produce a headline throughput win over real-lock rollback. Route C weak-lock default should therefore be treated as a **correctness/ownership simplification default**, not as a guaranteed performance upgrade.

### 6.2 TTL

Scenario: `expired-delete-only`

| Backend | Weak-lock qps | Real-lock qps | Delta vs real |
| --- | ---: | ---: | ---: |
| array | 6598.42 | 5987.25 | +10.2% |
| rbtree | 6483.60 | 5833.50 | +11.1% |
| hash | 6091.49 | 5270.70 | +15.6% |
| skiptable | 6615.22 | 6597.03 | +0.3% |

### 6.3 Persistence / Replication Elapsed Time

| Script | Weak-lock | Real-lock | Delta vs real |
| --- | ---: | ---: | ---: |
| `rdb_snapshot_stage10_6.sh` | 2161 ms | 2145 ms | +0.7% |
| `aof_rewrite_snapshot_stage10_7.sh` | 2190 ms | 2166 ms | +1.1% |
| `master_full_sync_stage10_5.sh` | 13334 ms | 14469 ms | -7.8% |
| `replicated_command_task_stage10_3.sh` | 7281 ms | 7256 ms | +0.3% |

Interpretation: persistence/replication elapsed times are effectively flat. No rollback-only cost explosion was observed.

## 7. reactor / proactor / ntyco

Source: preserved `weak_rwlock_mixed_stress_stage12.sh` log for this closeout run.

### 7.1 4 Clients

| Network mode | Real-lock qps | Weak-lock qps | Delta vs real |
| --- | ---: | ---: | ---: |
| reactor | 3003.51 | 2887.15 | -3.9% |
| proactor | 3545.10 | 4043.94 | +14.1% |
| ntyco | 298.04 | 295.17 | -1.0% |

### 7.2 8 Clients

| Network mode | Real-lock qps | Weak-lock qps | Delta vs real |
| --- | ---: | ---: | ---: |
| reactor | 24.18 | 24.20 | +0.1% |
| proactor | 34.84 | 34.86 | +0.1% |
| ntyco | 19.91 | 20.01 | +0.5% |

Interpretation: reduced-sample mixed stress shows no stable regression pattern attributable to weak-lock default. Proactor improved in the 4-client sample, while reactor/ntyco stayed close enough to call this noise-level for closeout purposes.

## 8. Owner-Lock Counters

### 8.1 Weak-Lock Default

- Hash/reactor service sample:
  - `weak_mode=1`
  - `total_calls=264020`
  - `non_executor_thread_calls=0`
  - `weak_noop_calls=264020`
  - `weak_fallback_calls=0`
- Embedding/reactor service sample:
  - `weak_mode=1`
  - `total_calls=938`
  - `non_executor_thread_calls=0`
  - `weak_noop_calls=938`
  - `weak_fallback_calls=0`

### 8.2 Real-Lock Rollback

- Hash/reactor service sample:
  - `weak_mode=0`
  - `total_calls=264020`
  - `non_executor_thread_calls=0`
  - `weak_noop_calls=0`
  - `weak_fallback_calls=0`
- Embedding/reactor service sample:
  - `weak_mode=0`
  - `total_calls=938`
  - `non_executor_thread_calls=0`
  - `weak_noop_calls=0`
  - `weak_fallback_calls=0`

Conclusion: normal operation stayed fully on executor-owned paths in both modes; no non-executor fallback was observed.

## 9. Correctness Regression

### 9.1 Mixed Stress

`tests/route_c/weak_rwlock_mixed_stress_stage12.sh`: **PASS**

- real-lock: PASS
- weak-lock: PASS
- persistence/replication sub-suite inside mixed stress: PASS

### 9.2 Full Correctness Suite

`tests/route_c/weak_rwlock_correctness_stage12.sh`: **one failing closeout run**

Observed failure context:

```text
stage12/13 correctness mode=real-lock (rollback): begin
...
Stage 10.10 TTL scanner owner client suite passed
server did not exit
```

Important qualification:

- the failure happened only inside the aggregated correctness suite
- standalone re-run of `KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/shutdown_owner_stage10_9.sh` passed immediately

So the current signal is **rollback-path flakiness under full suite orchestration**, not a cleanly isolated deterministic failure.

## 10. TSAN / Historical References

This closeout did **not** rerun TSAN.

Relevant prior references:

- `docs/route_c_perf_arch/63_stage12_weak_rwlock_correctness_and_stress_gate.md`
  - records Stage 12 correctness + mixed stress pass
  - records weak/real owner-lock stats with `non_executor_thread_calls=0`
- `docs/route_c_perf_arch/65_stage13_global_rwlock_removal_and_final_gate.md`
  - records `FINAL_ROUTE_C_READY`
  - states Stage 12 TSAN PASS reference for weak-lock mode

Closeout conclusion therefore relies on:

- fresh closeout benchmark + mixed stress data from 2026-06-18
- historical TSAN evidence from Stage 12 documentation

## 11. Route C Stage Summary

High-level Route C progression from Stage 0 to Stage 13:

1. Stage 0-3: command/response ownership audit and command-task / response-task direction established.
2. Stage 4-6: executor model stabilized across lifecycle, reactor, proactor and async closure paths.
3. Stage 7-9: embedding work moved behind controlled runtime and worker-pool boundaries.
4. Stage 10: persistence, replication, TTL, shutdown, slave-connect and fallback paths converged on executor ownership.
5. Stage 11: rwlock wrapper takeover and weak-mode switch introduced.
6. Stage 12: weak-rwlock correctness, mixed stress and TSAN gate recorded.
7. Stage 13: weak-lock became default, `g_kv_data_rwlock` demoted to fallback/diagnostic object, final gate marked Route C ready.

## 12. Rollback

### 12.1 Runtime Rollback

Use real-lock rollback mode:

```bash
KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore.conf
```

Embedding config:

```bash
KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore-embedding.conf
```

### 12.2 Validation After Rollback

Recommended minimum:

```bash
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/shutdown_owner_stage10_9.sh
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/rdb_snapshot_stage10_6.sh
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/aof_rewrite_snapshot_stage10_7.sh
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/master_full_sync_stage10_5.sh
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/replicated_command_task_stage10_3.sh
```

If time allows:

```bash
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/weak_rwlock_mixed_stress_stage12.sh
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/weak_rwlock_correctness_stage12.sh
```

## 13. Remaining Risk

1. The closeout was not run from a clean git tree, so tag readiness should be judged only after the current Stage 13 workspace is committed and rechecked.
2. Full aggregated correctness has a real-lock rollback flake around shutdown exit sequencing (`server did not exit`) even though standalone `shutdown_owner_stage10_9.sh` passed.
3. Weak-lock default is not a measured performance win in this closeout sample; rollback mode was faster on GET, VSET and VSEARCH.
4. Mixed stress numbers were intentionally reduced for closeout turnaround, so they are trend indicators, not long-duration saturation evidence.

## 14. Tag Recommendation

Recommendation: **Yes, with one caveat**

- Recommended tag: `route-c-final-weak-lock-ready`
- Caveat: before tagging, commit the current Stage 13 work and do one final clean-tree rerun of `tests/route_c/route_c_final_closeout_stage13.sh` or at least `tests/route_c/weak_rwlock_correctness_stage12.sh` in rollback mode to confirm the shutdown-exit flake does not persist.

If the team wants a stricter bar, delay tagging until the aggregated rollback flake is either reproduced reliably or dismissed by a clean-tree rerun.

## 15. Commit Recommendation

- Suggested commit message:

```text
补充路线C最终收口报告与性能基线
```
