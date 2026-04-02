# Benchmark Summary

| ds | op | kvstore_qps | redis_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | redis_p50(ms) | redis_p95(ms) | redis_p99(ms) | notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| array | set | 139664.800000 | 128369.710000 | 0.207000 | 0.343000 | 0.535000 | 0.199000 | 0.303000 | 0.447000 | set/get aligned; kv/redis qps=1.088 |
| array | get | 133868.810000 | 123915.740000 | 0.239000 | 0.431000 | 0.711000 | 0.191000 | 0.351000 | 0.567000 | set/get aligned; kv/redis qps=1.080 |
| hash | set | 138504.160000 | 136239.780000 | 0.191000 | 0.311000 | 0.551000 | 0.183000 | 0.279000 | 0.439000 | set/get aligned; kv/redis qps=1.017 |
| hash | get | 136612.020000 | 129870.130000 | 0.183000 | 0.311000 | 0.567000 | 0.199000 | 0.295000 | 0.487000 | set/get aligned; kv/redis qps=1.052 |
| rbtree | set | 111731.840000 | 127877.230000 | 0.375000 | 0.639000 | 0.847000 | 0.191000 | 0.319000 | 0.503000 | set/get aligned; kv/redis qps=0.874 |
| rbtree | get | 112233.450000 | 126742.720000 | 0.383000 | 0.631000 | 0.815000 | 0.199000 | 0.311000 | 0.463000 | set/get aligned; kv/redis qps=0.886 |
| skiptable | set | 92081.030000 | 132978.730000 | 0.479000 | 0.767000 | 1.087000 | 0.191000 | 0.263000 | 0.391000 | set/get aligned; kv/redis qps=0.692 |
| skiptable | get | 95969.280000 | 131752.310000 | 0.455000 | 0.743000 | 0.991000 | 0.183000 | 0.335000 | 0.543000 | set/get aligned; kv/redis qps=0.728 |

## Conclusion
- KVStore lowest observed throughput is ds=skiptable op=set qps=92081.030000, likely first bottleneck candidate.
- SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace.
