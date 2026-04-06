# Benchmark Summary

| ds | op | kvstore_qps | redis_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | redis_p50(ms) | redis_p95(ms) | redis_p99(ms) | notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| array | set | 120627.270000 | 128534.700000 | 0.199000 | 0.479000 | 1.343000 | 0.191000 | 0.351000 | 0.527000 | set/get aligned; kv/redis qps=0.938 |
| array | get | 127877.230000 | 126422.250000 | 0.191000 | 0.327000 | 0.455000 | 0.199000 | 0.359000 | 0.559000 | set/get aligned; kv/redis qps=1.012 |
| hash | set | 127388.530000 | 125000.000000 | 0.199000 | 0.367000 | 0.559000 | 0.199000 | 0.327000 | 0.559000 | set/get aligned; kv/redis qps=1.019 |
| hash | get | 123304.560000 | 120918.980000 | 0.199000 | 0.535000 | 0.695000 | 0.207000 | 0.423000 | 0.607000 | set/get aligned; kv/redis qps=1.020 |
| rbtree | set | 116686.120000 | 116009.280000 | 0.367000 | 0.599000 | 0.799000 | 0.215000 | 0.351000 | 0.551000 | set/get aligned; kv/redis qps=1.006 |
| rbtree | get | 119904.080000 | 104602.520000 | 0.335000 | 0.599000 | 0.815000 | 0.215000 | 0.511000 | 0.871000 | set/get aligned; kv/redis qps=1.146 |
| skiptable | set | 97560.980000 | 115207.380000 | 0.439000 | 0.807000 | 1.295000 | 0.215000 | 0.439000 | 0.703000 | set/get aligned; kv/redis qps=0.847 |
| skiptable | get | 102880.660000 | 112233.450000 | 0.423000 | 0.695000 | 0.983000 | 0.215000 | 0.431000 | 0.703000 | set/get aligned; kv/redis qps=0.917 |

## Conclusion
- KVStore lowest observed throughput is ds=skiptable op=set qps=97560.980000, likely first bottleneck candidate.
- SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace.
