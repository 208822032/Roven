# Benchmark Summary

| ds | op | kvstore_qps | redis_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | redis_p50(ms) | redis_p95(ms) | redis_p99(ms) | notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| array | set | 129701.680000 | 128369.710000 | 0.191000 | 0.439000 | 0.663000 | 0.191000 | 0.375000 | 0.599000 | set/get aligned; kv/redis qps=1.010 |
| array | get | 132978.730000 | 130039.020000 | 0.183000 | 0.335000 | 0.743000 | 0.191000 | 0.311000 | 0.511000 | set/get aligned; kv/redis qps=1.023 |
| hash | set | 138888.890000 | 127226.460000 | 0.183000 | 0.295000 | 0.479000 | 0.199000 | 0.327000 | 0.503000 | set/get aligned; kv/redis qps=1.092 |
| hash | get | 140449.440000 | 126103.410000 | 0.175000 | 0.295000 | 0.423000 | 0.199000 | 0.327000 | 0.527000 | set/get aligned; kv/redis qps=1.114 |
| rbtree | set | 121654.500000 | 130039.020000 | 0.343000 | 0.575000 | 0.743000 | 0.191000 | 0.351000 | 0.591000 | set/get aligned; kv/redis qps=0.936 |
| rbtree | get | 131406.050000 | 130718.950000 | 0.319000 | 0.535000 | 0.727000 | 0.191000 | 0.295000 | 0.471000 | set/get aligned; kv/redis qps=1.005 |
| skiptable | set | 111731.840000 | 127388.530000 | 0.399000 | 0.615000 | 0.743000 | 0.199000 | 0.295000 | 0.471000 | set/get aligned; kv/redis qps=0.877 |
| skiptable | get | 119474.310000 | 121359.230000 | 0.359000 | 0.607000 | 0.815000 | 0.199000 | 0.423000 | 0.567000 | set/get aligned; kv/redis qps=0.984 |

## Conclusion
- KVStore lowest observed throughput is ds=skiptable op=set qps=111731.840000, likely first bottleneck candidate.
- SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace.
