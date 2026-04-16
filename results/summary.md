# Benchmark Summary

| ds | op | kvstore_qps | redis_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | redis_p50(ms) | redis_p95(ms) | redis_p99(ms) | notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| array | set | 115340.260000 | 105708.250000 | 0.215000 | 0.447000 | 0.775000 | 0.223000 | 0.455000 | 0.615000 | set/get aligned; kv/redis qps=1.091 |
| array | get | 108695.650000 | 94876.660000 | 0.223000 | 0.559000 | 0.807000 | 0.239000 | 0.527000 | 0.855000 | set/get aligned; kv/redis qps=1.146 |
| hash | set | 107296.140000 | 117096.020000 | 0.223000 | 0.487000 | 0.783000 | 0.207000 | 0.439000 | 0.631000 | set/get aligned; kv/redis qps=0.916 |
| hash | get | 115340.260000 | 121506.680000 | 0.207000 | 0.415000 | 0.583000 | 0.207000 | 0.343000 | 0.543000 | set/get aligned; kv/redis qps=0.949 |
| rbtree | set | 114547.530000 | 108225.100000 | 0.343000 | 0.583000 | 0.807000 | 0.223000 | 0.447000 | 0.679000 | set/get aligned; kv/redis qps=1.058 |
| rbtree | get | 115473.450000 | 114025.090000 | 0.327000 | 0.567000 | 0.767000 | 0.215000 | 0.407000 | 0.567000 | set/get aligned; kv/redis qps=1.013 |
| skiptable | set | 102774.920000 | 123001.230000 | 0.407000 | 0.711000 | 1.103000 | 0.207000 | 0.271000 | 0.511000 | set/get aligned; kv/redis qps=0.836 |
| skiptable | get | 105820.110000 | 117370.890000 | 0.407000 | 0.671000 | 0.983000 | 0.215000 | 0.303000 | 0.591000 | set/get aligned; kv/redis qps=0.902 |

## Conclusion
- KVStore lowest observed throughput is ds=skiptable op=set qps=102774.920000, likely first bottleneck candidate.
- SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace.
