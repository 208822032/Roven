# Benchmark Optimization Progress

## Scope

This note now tracks the TTL-aware read-path split phase.

The work is intentionally narrow:

1. Keep GET/EXIST semantics unchanged.
2. Split pure lookup, TTL probe, and expired-side-effect mutation into separate internal steps.
3. Validate the split with functional suites first, then a small microbench and a light perf check.

Benchmark settings used for the clean validation runs:

- TTL lookup split bench: `keyspace=100000`, `requests=500000`, `warmup=10000`, `value_size=32`
- TTL QPS smoke: `TTL_QPS_COUNT=2000`, `TTL_QPS_SAMPLE_COUNT=20`, `TTL_QPS_TTL_SECS=3`, `TTL_QPS_WAIT_BUFFER_SECS=2`

## Call Chain

The current GET/EXIST path is:

1. RESP dispatch maps `GET` / `RGET` / `HGET` / `SGET` and `EXIST` / `REXIST` / `HEXIST` / `SEXIST` to `handle_get` and `handle_exist` in [`src/core/kvs_operation.c`](../src/core/kvs_operation.c).
2. `handle_get` and `handle_exist` call `ttl_expire_if_needed_by_structure()`.
3. `ttl_expire_if_needed_by_structure()` now does three separate things:
	- pure structure lookup via `ttl_lookup_by_structure()`
	- TTL probe via `ttl_target_is_expired()`
	- expired-key mutation via `ttl_target_apply_expired_side_effects()`
4. Storage-layer read helpers such as `kvs_array_get()`, `kvs_rbtree_get()`, `kvs_hash_get()`, and `kvs_skiptable_get()` remain pure reads and do not delete or sync.

The same split is also used by `ttl_apply_any_key_op()` for `TTL` / `EXPIRE` / `PERSIST` style operations.

## Boundary Definition

### Pure-read lookup

Pure-read lookup means:

- only resolve the target object
- do not delete anything
- do not send replication
- do not update any TTL state

In practice this is the combination of `ttl_lookup_by_structure()` in core and the storage-level `kvs_*_get()` / `kvs_*_exist()` helpers.

### TTL-aware read

TTL-aware read means:

- lookup the object
- check whether it is expired
- decide whether the caller should see `NO EXIST`
- do not directly own the mutation or sync side effects

In practice this is `ttl_expire_if_needed_by_structure()` and the shared `ttl_target_is_expired()` probe.

### Mutation / side-effect path

Mutation / side-effect path means:

- delete expired keys from the active structure
- trigger TCP replication when running on master
- leave user-visible command semantics unchanged

In practice this is `ttl_target_apply_expired_side_effects()`, which wraps the existing delete helpers and `ttl_sync_delete_if_needed()`.

## Code Changes

[`src/core/kvs_operation.c`](../src/core/kvs_operation.c)

- Added `ttl_target_is_expired()` as the pure TTL probe helper.
- Added `ttl_target_apply_expired_side_effects()` as the mutation / sync helper.
- Refactored `ttl_expire_if_needed_by_structure()` and `ttl_apply_any_key_op()` to use the split helpers.
- External RESP behavior stayed unchanged.

## Functional Validation

The following regression suites were run and passed after the split:

- [`tests/test_ttl_all_backends.sh`](../tests/test_ttl_all_backends.sh)
- [`tests/test_ttl_rdb.sh`](../tests/test_ttl_rdb.sh)
- [`tests/test_ttl_aof.sh`](../tests/test_ttl_aof.sh)
- [`tests/test_ttl_tcp_replication.sh`](../tests/test_ttl_tcp_replication.sh)
- [`tests/test_ttl_ebpf_replication.sh`](../tests/test_ttl_ebpf_replication.sh)
- [`tests/test_ttl_qps.sh`](../tests/test_ttl_qps.sh)

This covers GET/EXIST, TTL, lazy expire deletion, overwrite/persist behavior, RDB/AOF reload, TCP replication, eBPF replication, and `SORT` / `RANGE` filtering.

[`tests/testcase.c`](../tests/testcase.c) was also scanned and is still relevant because it contains RESP smoke coverage for GET/EXIST and `SORT` / `RANGE`, but the TTL suites above already exercise the same user-visible behavior more directly.

## Microbench

`tests/ttl_lookup_split_bench.c` on the current build produced:

| case | ns/op | qps |
|---|---:|---:|
| rbtree pure_get | 564.06 | 1772868.44 |
| rbtree ttl_probe | 566.20 | 1766163.99 |
| rbtree current_get_core | 534.79 | 1869893.30 |
| skiptable pure_get | 666.18 | 1501092.16 |
| skiptable ttl_probe | 764.09 | 1308751.47 |
| skiptable current_get_core | 775.17 | 1290035.38 |

Key deltas:

- For skiptable, the TTL probe adds about `97.91 ns/op`, and the current GET core is about `108.99 ns/op` slower than pure read.
- The TTL-aware gap is therefore mostly the expiry probe itself; the benchmark does not yet include a real delete/sync sample.
- For rbtree, the numbers are close enough that the difference is dominated by run-to-run noise in this small benchmark.

## Perf

A lightweight `perf stat` run was collected on the same microbench. The environment did not expose hardware counters here, but it did confirm the benchmark completed cleanly with about `2030.50 ms` task-clock and `2.031200583 s` elapsed time.

The important part for this phase is structural, not a new kernel hotspot claim:

- pure read stays on the storage lookup path
- TTL probe stays isolated in the probe helper
- delete / sync is still only in the side-effect helper

## Current Conclusion

The TTL-aware read path is now cut into clear internal layers.

The pure lookup boundary is explicit, the TTL-aware probe is isolated, and the expired-key side effects are separated from the read path. The existing functional suites still pass, and the microbench shows the remaining cost on the read path is now mostly the TTL probe itself.

## Lock Scope Experiment

This phase did not require a new global lock design. The minimal experiment was to validate the existing two-stage GET/EXIST path under the TTL split:

1. First pass runs under the existing read lock and only does pure lookup plus TTL probe.
2. If the key is expired, the dispatcher retries under the write lock so the mutation and replication side effects stay on the write path.
3. Non-expired reads stay on the read path and never take the delete or sync branch.

That means the lock-range boundary is now explicit and externally safe, but further shrinking the lock still needs a storage-memory ownership redesign. Without that, we cannot safely move response serialization outside the lock because the returned value pointer would become stale after unlock.

### Functional Validation

All of the following passed in this run:

- `tests/test_ttl_all_backends.sh`
- `tests/test_ttl_rdb.sh`
- `tests/test_ttl_aof.sh`
- `tests/test_ttl_tcp_replication.sh`
- `tests/test_ttl_ebpf_replication.sh` (run under `sudo` on this host; plain user mode hit `Operation not permitted` from the BPF loader)
- `tests/test_ttl_qps.sh`

### Microbench

`tests/ttl_lookup_split_bench` produced:

| case | ns/op | qps |
|---|---:|---:|
| rbtree pure_get | 607.75 | 1645425.54 |
| rbtree ttl_probe | 618.62 | 1616508.39 |
| rbtree current_get_core | 583.83 | 1712820.23 |
| skiptable pure_get | 765.60 | 1306172.32 |
| skiptable ttl_probe | 1000.34 | 999664.30 |
| skiptable current_get_core | 854.28 | 1170576.65 |

The microbench still shows the remaining overhead is concentrated in the TTL probe and, for skiptable, in the current read path around that probe. It does not yet isolate delete or sync cost because the sample here is the non-expired hot path.

### End-To-End Benchmark

`tests/test_ttl_qps.sh` with `TTL_QPS_COUNT=2000`, `TTL_QPS_SAMPLE_COUNT=20`, `TTL_QPS_TTL_SECS=3`, `TTL_QPS_WAIT_BUFFER_SECS=2` produced:

- `rbtree`: `qps_pairs=125000.00`, `qps_ops=250000.00`
- `skiptable`: `qps_pairs=8968.61`, `qps_ops=17937.22`

The `skiptable` lane is still the main hotspot; the GET/EXIST split did not create a semantic regression, but it also did not materially eliminate the underlying contention pressure on this host.

### Perf

`sudo perf stat -e task-clock,context-switches,cpu-migrations,cycles,instructions ./tests/ttl_lookup_split_bench` completed successfully and reported:

- `task-clock`: `2,068.98 msec`
- `context-switches`: `13`
- `cpu-migrations`: `0`
- `cycles` / `instructions`: not supported in this environment

This is enough to confirm the benchmark runs cleanly under perf, but it is not a direct lock-wait profiler. I do not have a reliable before/after lock-wait counter from this host.

## Ready For Next Step?

The current split is sufficient to continue the read-lock plan, but it is not enough to prove that additional lock shrinking is safe or profitable without more attribution.

The next best step is to profile expired-key delete and TCP sync side effects directly, because that is where the remaining write-path cost is concentrated and where a later read-lock rollout would need a precise budget.

## Skiptable SET Fast Path

The focus later shifted to the skiptable write path, specifically SET / overwrite behavior.

### Code Change

[`src/storage/kvs_skiptable.c`](../src/storage/kvs_skiptable.c)

- `kvs_skiptable_set()` and `kvs_skiptable_mod()` now use a single traversal helper, `skiptable_find_with_update_or_exact()`.
- Overwrite hits update in place on the first pass instead of forcing a second lookup.
- Insert still preserves the predecessor path so the first overwrite optimization does not regress the insert path again.

### Functional Validation

The following regression suites passed after the change:

- [`tests/test_ttl_rdb.sh`](../tests/test_ttl_rdb.sh)
- [`tests/test_ttl_aof.sh`](../tests/test_ttl_aof.sh)
- [`tests/test_ttl_tcp_replication.sh`](../tests/test_ttl_tcp_replication.sh)
- [`tests/test_ttl_ebpf_replication.sh`](../tests/test_ttl_ebpf_replication.sh)
- [`tests/test_ttl_qps.sh`](../tests/test_ttl_qps.sh)
- [`tests/testcase.c`](../tests/testcase.c) mode 4 on a clean temporary server

### Targeted Bench

`tests/skiptable_targeted_bench` on the current build produced:

| case | ns/op | qps |
|---|---:|---:|
| existing_lookup | 720.91 | 1387128.25 |
| missing_lookup | 90.84 | 11008539.25 |
| insert_heavy legacy | 357.66 | 2795942.32 |
| insert_heavy current | 227.67 | 4392302.16 |
| overwrite_heavy legacy | 1060.90 | 942591.49 |
| overwrite_heavy current | 801.74 | 1247289.97 |

The important part is that overwrite-heavy is now faster than the old path, while insert-heavy no longer regressed.

### End-To-End Benchmark

The aligned `redis-benchmark` run against the updated skiptable path produced:

- `skiptable set`: `91659.03 qps` vs Redis `110375.27 qps` -> `0.830x`
- `skiptable get`: `99304.87 qps` vs Redis `121359.23 qps` -> `0.818x`

This means the microbench improvement is real, but the full benchmark still leaves skiptable below Redis.

### Perf

`sudo perf stat` was attempted on this host, but the environment still reports the requested hardware counters as `<not supported>`. The benchmark itself completed, so the remaining evidence here is the timing result plus the benchmark deltas above, not counter-level attribution.

### Conclusion

The overwrite fast path is worth keeping, but it is not sufficient by itself to close the skiptable gap. The remaining work is broader SET / traversal / cache-path reduction, not just the overwrite branch.

## Expired-Path Profiling

To answer that attribution question, I added a focused expired-path harness:

- [`tests/ttl_expired_path_bench.c`](../tests/ttl_expired_path_bench.c)
- [`tests/test_ttl_expired_path_profile.sh`](../tests/test_ttl_expired_path_profile.sh)

The harness splits the read path into three measurable cases:

1. non-expired read
2. expired delete only
3. expired delete + TCP sync

It also isolates warmup and measured keys per scenario so one phase does not poison the next phase.

### Functional Verification

These TTL / replication suites were rerun first and passed before profiling:

- [`tests/test_ttl_all_backends.sh`](../tests/test_ttl_all_backends.sh)
- [`tests/test_ttl_rdb.sh`](../tests/test_ttl_rdb.sh)
- [`tests/test_ttl_aof.sh`](../tests/test_ttl_aof.sh)
- [`tests/test_ttl_tcp_replication.sh`](../tests/test_ttl_tcp_replication.sh)
- [`tests/test_ttl_ebpf_replication.sh`](../tests/test_ttl_ebpf_replication.sh)
- [`tests/test_ttl_qps.sh`](../tests/test_ttl_qps.sh)

### Latest Validation

Before and after the TCP sync change, the following suites passed:

- `tests/test_ttl_all_backends.sh`
- `tests/test_ttl_rdb.sh`
- `tests/test_ttl_aof.sh`
- `tests/test_ttl_tcp_replication.sh`
- `tests/test_ttl_ebpf_replication.sh`
- `tests/test_ttl_qps.sh`

### TCP Sync Optimization

The TCP replication path was trimmed at the message allocation boundary:

- `master_sync_command()` now encodes directly into the final queued `sync_msg_t` instead of encoding into a temporary buffer and copying again.
- `sync_backend_broadcast_owned()` was added so the TCP queue can take ownership of an already allocated message.
- The change stays inside the existing TCP sync semantics; it does not change replication protocol meaning, lock scope, or the eBPF path.

### Profiling Result

The latest retained expired-path profiling run on skiptable produced:

| case | qps |
|---|---:|
| non-expired read | 16471.03 |
| expired delete only | 16363.54 |
| expired delete + TCP sync | 15620.43 |

Compared with the earlier retained run, the gap between delete-only and delete+TCP-sync shrank materially. The earlier numbers were:

| case | qps |
|---|---:|
| non-expired read | 13677.56 |
| expired delete only | 18274.15 |
| expired delete + TCP sync | 14260.65 |

The current picture is now:

- delete-only and non-expired read are very close on this host
- TCP sync is still slower than delete-only, but the delta is much smaller than before
- the remaining work is still on the replication side, not on read-lock expansion

### Perf

The retained `perf record` data from the latest run still shows the expected shape:

- non-expired read: `schedule_hrtimeout_range_clock`, `schedule`, `ep_poll`, `ttl_skiptable_key_compare`
- expired delete only: `__ip_local_out`, `nf_hook_slow`, `skb_page_frag_refill`, `release_sock`, `__alloc_skb`, `tcp_send_mss`, `futex_wake`, `wake_up_q`, `try_to_wake_up`
- expired delete + TCP sync: `wake_up_q`, `try_to_wake_up`, `tcp_backend_worker`, `mem_alloc`, `ttl_scan_once`, `epoll_ctl`, `__pthread_mutex_unlock`, `release_sock`, `__wake_up`, `iptable_filter_hook`

`perf stat` was less reliable on this host for the full scripted harness, so the symbol-level attribution above is the dependable evidence here.

### Conclusion

The new evidence still supports focusing on TCP sync first. The delete-only path is no longer the obvious bottleneck, the TCP sync path is cheaper after the owned-message change, and the remaining cost is still replication-side. It is not a good time to re-expand the read lock; the higher-value next step is still to continue trimming TCP sync.