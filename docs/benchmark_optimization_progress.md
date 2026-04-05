# Benchmark Optimization Progress

## Scope

This note now tracks three steps in order:

1. The earlier lock sweep that showed shared serialization is still the first bottleneck.
2. The response-path optimization pass that removed the per-request temporary response buffer and extra copy.
3. The current TTL-aware read split experiment, which isolates pure lookup from lazy-expire/delete/sync behavior.

Profile setup for the earlier perf runs:

- Server process: existing `./kvstore --config config/master.conf` instance
- Benchmark shape: `c=50`, `P=1`, `value_size=32`, `keyspace=100000`
- Profile run size: `n=500000` for each set/get case, with a populate warmup of `100000`
- Tooling: [`scripts/profile_benchmark_perf.sh`](../scripts/profile_benchmark_perf.sh)

## Current Conclusion

The bottleneck split is now clearer:

- Response copy/allocation was a real cost, but it is no longer the dominant one after the send-buffer reuse pass.
- The rbtree exact-search hot path is now centered on `rbtree_search_with_len`; the TTL wrapper path no longer shows up as a top perf symbol after the length-aware lookup split.
- The remaining skiptable hot symbols have narrowed to the exact-match search loop and compare path: `skiptable_find_exact`, `skiptable_find_with_update`, and `skiptable_key_compare`.
- TTL-aware reads are still non-trivial, but GET/EXIST now reuse the located entry after the TTL check instead of repeating a second backend lookup.

## GET / EXIST / TTL Call Chain

### Pure lookup layer

These functions only perform structure lookup and return the in-memory value pointer or presence result:

- [`kvs_rbtree_get`](../src/storage/kvs_rbtree.c#L632)
- [`kvs_skiptable_get`](../src/storage/kvs_skiptable.c#L294)
- [`kvs_hash_get`](../src/storage/kvs_hash.c#L403)
- [`kvs_array_get`](../src/storage/kvs_array.c#L545)
- [`kvs_rbtree_exist`](../src/storage/kvs_rbtree.c#L677)
- [`kvs_skiptable_exist`](../src/storage/kvs_skiptable.c#L379)
- [`kvs_hash_exist`](../src/storage/kvs_hash.c#L455)
- [`kvs_array_exist`](../src/storage/kvs_array.c#L645)

### TTL-aware wrapper layer

These functions do a lookup first and then inspect expiry metadata:

- [`ttl_expire_if_needed_by_structure`](../src/core/kvs_operation.c#L275)
- [`ttl_apply_any_key_op`](../src/core/kvs_operation.c#L303)
- [`ttl_get_any_key_expire_at_ms`](../src/core/kvs_operation.c#L160)
- [`ttl_set_any_key_expire_at_ms`](../src/core/kvs_operation.c#L176)

### Mutation / side-effect layer

These code paths can delete keys or propagate TTL changes:

- [`ttl_target_delete`](../src/core/kvs_operation.c#L246)
- [`ttl_sync_delete_if_needed`](../src/core/kvs_operation.c#L152)
- [`ttl_scan_flush_expired`](../src/core/kvs_ttl_background.c#L90)
- [`ttl_scan_array`](../src/core/kvs_ttl_background.c#L103)
- [`ttl_scan_hash`](../src/core/kvs_ttl_background.c#L134)
- [`ttl_scan_rbtree`](../src/core/kvs_ttl_background.c#L166)
- [`ttl_scan_skiptable`](../src/core/kvs_ttl_background.c#L174)
- [`ttl_scan_once`](../src/core/kvs_ttl_background.c#L191)

### Command entry points

These commands call the TTL wrapper before the storage lookup:

- [`handle_get`](../src/core/kvs_operation.c#L725)
- [`handle_exist`](../src/core/kvs_operation.c#L852)
- [`handle_ttl`](../src/core/kvs_operation.c#L905)
- [`handle_expire`](../src/core/kvs_operation.c#L894)
- [`handle_persist`](../src/core/kvs_operation.c#L913)
- [`handle_pexpireat`](../src/core/kvs_operation.c#L924)

At the top level, [`kvs_filter_protocol`](../src/core/kvstore.c#L47) adds lock and replication bookkeeping around those handlers.

## Boundary Definition

The minimal split is:

- Pure lookup path: structure lookup only, no delete, no sync, no expiry enforcement.
- TTL-aware path: current behavior, including lazy-expire checks and delete/sync side effects when a key is expired.

For GET/EXIST, the current implementation is not pure lookup because `ttl_expire_if_needed_by_structure()` can remove expired keys on master and call `ttl_sync_delete_if_needed()`.

## Minimal Experiment

To avoid changing external semantics, I added an isolated microbench instead of wiring a new public command path.

New file:

- [`tests/ttl_lookup_split_bench.c`](../tests/ttl_lookup_split_bench.c)

New test target:

- [`tests/ttl_lookup_split_bench`](../Makefile)

The benchmark is offline and does not use network, RESP, or any delete/sync path. It measures:

- `pure_get`: direct `kvs_rbtree_get` / `kvs_skiptable_get`
- `ttl_probe`: lookup plus `kv_now_ms()` and expiry check only
- `current_get_core`: current GET-style core path approximation, meaning TTL probe first and then value access from the already found entry

## Microbench Results

Run parameters:

- `keyspace=100000`
- `requests=500000`
- `warmup=10000`
- `value_size=32`

Results:

| ds | variant | ns/op | qps |
|---|---|---:|---:|
| rbtree | pure_get | 649.89 | 1538719.95 |
| rbtree | ttl_probe | 609.86 | 1639709.19 |
| rbtree | current_get_core | 606.30 | 1649345.78 |
| skiptable | pure_get | 662.38 | 1509708.70 |
| skiptable | ttl_probe | 825.45 | 1211457.12 |
| skiptable | current_get_core | 824.51 | 1212840.03 |

Interpretation:

- The microbench now exercises the length-aware rbtree lookup helper, so `current_get_core` is the more relevant number for this pass.
- The rbtree GET core is now materially lower than the public-API `pure_get` path, which still pays the outer `strlen` + wrapper cost.
- For skiptable, the exact-match fast path still keeps the cost concentrated in ordered compare/search rather than wrapper duplication.

## Current Perf Evidence

Fresh `cycles:u` samples from the lookup-optimization pass show the repeated lookup cost has been reduced, but compare-heavy ordered search is still visible:

| case | top hotspot | share |
|---|---|---:|
| rbtree set | `rbtree_search_with_len` | 9.59% |
| rbtree get | `rbtree_search_with_len` | 4.76% |
| rbtree get | `__memcmp_avx2_movbe` | 1.59% |
| skiptable set | `skiptable_find_exact` | 1.59% |
| skiptable set | `skiptable_find_with_update` | 1.59% |
| skiptable set | `__memcmp_avx2_movbe` | 1.59% |
| skiptable get | `skiptable_key_compare` | 1.59% |

For rbtree, `ttl_lookup_rbtree` no longer appears in the top 1% perf report, which is consistent with the wrapper-side length propagation. The remaining visible work is the tree walk and compare itself.

## Answer To The Split Question

TTL-aware reads are still paying some cost, but the biggest avoidable waste was the repeated lookup, not the TTL probe itself.

- rbtree: the one-lookup GET core path is down to 606.30 ns/op in the updated microbench, and the TTL wrapper symbol is no longer a visible top hotspot.
- skiptable: the one-lookup GET core path is down to 824.51 ns/op in the updated microbench.

So the ordering is:

1. For rbtree, the exact-search path is still worth another targeted pass if there is a concrete compare-shortcut idea.
2. The next structural step should be a cleaner TTL-aware read split so read-only and mutation-capable paths are separated before any lock redesign.

## Next Best Priority

The next most valuable optimization is now a cleaner TTL-aware read split around rbtree, not another skiptable compare tweak.

- For skiptable, the remaining gains look incremental; only continue if a new targeted microbench shows a clear win.
- For rbtree, the next target is still lowering the compare cost inside `rbtree_search_with_len`.
- A read-lock split is worth preparing once the TTL-aware read path is cleanly separated from lazy-expire mutation and replication side effects.

## Validation Status

- Functional regression is now green on a clean temporary server: `tests/testcase` modes 1-4 pass after aligning the legacy sort cases to the current `low 10` syntax.
- The TTL regression suites still pass end to end, including TCP replication and the sudo-backed eBPF replication path.
- The latest three `ttl_lookup_split_bench` runs stayed in the same band: rbtree `current_get_core` ranged from 599.84 ns/op to 632.52 ns/op, and skiptable `current_get_core` ranged from 769.46 ns/op to 812.34 ns/op.

## Evidence Artifacts

- [`results/raw.csv`](../results/raw.csv)
- [`results/summary.md`](../results/summary.md)
- [`results/profile/20260405_033810_rbtree_set_rbtree_set.perf.report.txt`](../results/profile/20260405_033810_rbtree_set_rbtree_set.perf.report.txt)
- [`results/profile/20260405_033822_rbtree_get_rbtree_get.perf.report.txt`](../results/profile/20260405_033822_rbtree_get_rbtree_get.perf.report.txt)
- [`results/profile/20260405_033833_skiptable_set_skiptable_set.perf.report.txt`](../results/profile/20260405_033833_skiptable_set_skiptable_set.perf.report.txt)
- [`results/profile/20260405_033845_skiptable_get_skiptable_get.perf.report.txt`](../results/profile/20260405_033845_skiptable_get_skiptable_get.perf.report.txt)
- [`results/profile/20260405_085558_rbtree_set.perf.report.txt`](../results/profile/20260405_085558_rbtree_set.perf.report.txt)
- [`results/profile/20260405_085611_rbtree_get.perf.report.txt`](../results/profile/20260405_085611_rbtree_get.perf.report.txt)
- [`tests/ttl_lookup_split_bench.c`](../tests/ttl_lookup_split_bench.c)