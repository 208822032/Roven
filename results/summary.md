# Benchmark Summary

| ds | op | kvstore_qps | redis_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | redis_p50(ms) | redis_p95(ms) | redis_p99(ms) | notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| array | set | 114416.480000 | 96993.210000 | 0.215000 | 0.439000 | 0.703000 | 0.255000 | 0.479000 | 0.735000 | set/get aligned; kv/redis qps=1.180 |
| array | get | 119904.080000 | 96246.390000 | 0.207000 | 0.367000 | 0.687000 | 0.255000 | 0.495000 | 0.679000 | set/get aligned; kv/redis qps=1.246 |
| hash | set | 100502.520000 | 92850.510000 | 0.239000 | 0.495000 | 0.767000 | 0.255000 | 0.511000 | 0.719000 | set/get aligned; kv/redis qps=1.082 |
| hash | get | 99108.030000 | 96246.390000 | 0.247000 | 0.599000 | 0.879000 | 0.255000 | 0.487000 | 0.711000 | set/get aligned; kv/redis qps=1.030 |
| rbtree | set | 100200.400000 | 94876.660000 | 0.415000 | 0.751000 | 1.007000 | 0.255000 | 0.495000 | 0.727000 | set/get aligned; kv/redis qps=1.056 |
| rbtree | get | 98328.420000 | 94339.620000 | 0.359000 | 0.743000 | 1.023000 | 0.255000 | 0.503000 | 0.695000 | set/get aligned; kv/redis qps=1.042 |
| skiptable | set | 72674.410000 | 98039.220000 | 0.543000 | 1.103000 | 2.279000 | 0.247000 | 0.503000 | 0.751000 | set/get aligned; kv/redis qps=0.741 |
| skiptable | get | 77579.520000 | 97751.710000 | 0.527000 | 0.951000 | 1.239000 | 0.255000 | 0.495000 | 0.687000 | set/get aligned; kv/redis qps=0.794 |

## Conclusion
- KVStore lowest observed throughput is ds=skiptable op=set qps=72674.410000, likely first bottleneck candidate.
- SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace.
