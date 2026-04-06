# Benchmark Optimization Progress

## Scope

This note now tracks the skiptable-only optimization pass.

The current work is intentionally narrow:

1. Keep the scope on skiptable compare/search only.
2. Do not change TTL semantics, lock semantics, protocol behavior, or rbtree.
3. Validate skiptable behavior first, then measure with targeted microbench, end-to-end benchmark, and perf.

Benchmark settings used for the clean validation runs:

- Clean master: temporary instance on port `19331`
- Benchmark shape: `c=50`, `P=1`, `value_size=32`, `keyspace=100000`
- End-to-end benchmark: `n=100000`
- Perf run: `n=500000`, `PERF_TIMEOUT_SEC=5`

## Current Conclusion

The skiptable hot path has improved in the exact place we targeted:

- `kvs_skiptable_set()` no longer does a separate exact lookup plus update-path traversal for a missing key.
- Existing-key updates still reuse the same update traversal.
- The new targeted microbench shows the legacy insert path at `359.97 ns/op` and the current path at `220.43 ns/op`, a `38.77%` reduction.
- End-to-end set/get is higher than the baseline summary on a clean master.
- Perf is now dominated by lock / kernel overhead in the sampled window; `skiptable_key_compare` is still visible, but `skiptable_find_exact` and `skiptable_find_with_update` do not surface as top symbols in the clean run.

## What Changed

### Code path

[`src/storage/kvs_skiptable.c`](../src/storage/kvs_skiptable.c)

- `kvs_skiptable_set()` now uses `skiptable_find_with_update()` once and checks whether the returned node is an exact match before deciding whether to update or insert.
- The previous two-step path, where `kvs_skiptable_set()` first did `skiptable_find_exact()` and then did `skiptable_find_with_update()`, is removed.

### Benchmarking

[`tests/skiptable_targeted_bench.c`](../tests/skiptable_targeted_bench.c)

- Added a skiptable-only microbench that separates existing-key lookup, missing-key lookup, and insert-heavy behavior.
- The benchmark also keeps a legacy insert implementation in-process so the before/after insert cost can be compared directly.

### Build wiring

[`Makefile`](../Makefile)

- Added the `tests/skiptable_targeted_bench` build target.

## Functional Validation

The following regression suites were run and passed:

- [`tests/testcase`](../tests/testcase.c) mode `4` for skiptable RESP behavior, including `SORT` and `RANGE`.
- [`tests/test_ttl_all_backends.sh`](../tests/test_ttl_all_backends.sh)
- [`tests/test_ttl_rdb.sh`](../tests/test_ttl_rdb.sh)
- [`tests/test_ttl_aof.sh`](../tests/test_ttl_aof.sh)
- [`tests/test_ttl_tcp_replication.sh`](../tests/test_ttl_tcp_replication.sh)
- [`tests/test_ttl_ebpf_replication.sh`](../tests/test_ttl_ebpf_replication.sh)

This covers skiptable GET/EXIST, TTL, SORT/RANGE, RDB/AOF, TCP replication, and eBPF replication.

## Targeted Microbench

`tests/skiptable_targeted_bench.c` on the clean master produced:

| variant | ns/op | qps |
|---|---:|---:|
| existing_lookup | 746.70 | 1339222.44 |
| missing_lookup | 101.22 | 9879117.47 |
| insert_heavy_legacy | 359.97 | 2777971.70 |
| insert_heavy_current | 220.43 | 4536622.98 |

Key delta:

- Insert-heavy path improved by `139.55 ns/op`, or `38.77%`.
- Missing-key lookup stays cheap because the traversal is now paid once in the insert/update path.

## End-To-End Benchmark

Baseline from [`results/summary.md`](../results/summary.md) versus the clean master on port `19331`:

| op | baseline qps | current qps |
|---|---:|---:|
| set | 72674.41 | 92506.94 |
| get | 77579.52 | 103950.10 |

Latency also improved on the clean run:

- set p50/p95/p99 moved from `0.543 / 1.103 / 2.279 ms` to `0.439 / 0.863 / 1.303 ms`.
- get p50/p95/p99 moved from `0.527 / 0.951 / 1.239 ms` to `0.367 / 0.743 / 1.007 ms`.

## Perf

Clean-master perf reports on the 500k runs show the search symbols are no longer the dominant cost:

| case | top visible skiptable symbol | share |
|---|---|---:|
| set | `skiptable_key_compare` | 2.19% |
| get | `skiptable_key_compare` | 0.98% |

The clean set report is still dominated by lock and scheduler overhead, with `__lock_text_start` at `93.92%`. The clean get report is likewise lock-heavy, with `__lock_text_start` at `88.73%`.

## Final Answer

Skiptable has entered the diminishing-returns zone.

The missing-key duplicate traversal was worth fixing and did move both the targeted microbench and the end-to-end benchmark, but the current perf window is now dominated by lock / kernel overhead rather than skiptable search. That means a further compare/search pass is unlikely to pay back as much as this one.

## Evidence Artifacts

- [`src/storage/kvs_skiptable.c`](../src/storage/kvs_skiptable.c)
- [`tests/skiptable_targeted_bench.c`](../tests/skiptable_targeted_bench.c)
- [`results/summary.md`](../results/summary.md)
- [`results/profile/20260406_clean_skiptable_set_skiptable_set.perf.report.txt`](../results/profile/20260406_clean_skiptable_set_skiptable_set.perf.report.txt)
- [`results/profile/20260406_clean_skiptable_get_skiptable_get.perf.report.txt`](../results/profile/20260406_clean_skiptable_get_skiptable_get.perf.report.txt)