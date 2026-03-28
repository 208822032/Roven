# Benchmark Summary

| ds | op | kvstore_qps | redis_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | redis_p50(ms) | redis_p95(ms) | redis_p99(ms) | notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| array | set | 134952.770000 | 125786.160000 | 0.223000 | 0.399000 | 0.759000 | 0.199000 | 0.327000 | 0.535000 | set/get aligned; kv/redis qps=1.073 |
| array | get | 132450.330000 | 124688.280000 | 0.247000 | 0.455000 | 0.663000 | 0.199000 | 0.311000 | 0.583000 | set/get aligned; kv/redis qps=1.062 |
| hash | set | 104493.200000 | 129366.110000 | 0.247000 | 0.911000 | 1.351000 | 0.199000 | 0.279000 | 0.455000 | set/get aligned; kv/redis qps=0.808 |
| hash | get | 127551.020000 | 120336.950000 | 0.207000 | 0.447000 | 0.743000 | 0.207000 | 0.327000 | 0.559000 | set/get aligned; kv/redis qps=1.060 |
| rbtree | set | 105708.250000 | 114416.480000 | 0.407000 | 0.655000 | 0.863000 | 0.215000 | 0.407000 | 0.695000 | set/get aligned; kv/redis qps=0.924 |
| rbtree | get | 106837.610000 | 115473.450000 | 0.407000 | 0.663000 | 0.863000 | 0.215000 | 0.391000 | 0.615000 | set/get aligned; kv/redis qps=0.925 |
| skiptable | set | 71633.230000 | 117785.630000 | 0.567000 | 1.223000 | 1.703000 | 0.207000 | 0.399000 | 0.623000 | set/get aligned; kv/redis qps=0.608 |
| skiptable | get | 85034.020000 | 114810.560000 | 0.535000 | 0.831000 | 1.063000 | 0.215000 | 0.407000 | 0.607000 | set/get aligned; kv/redis qps=0.741 |

## Conclusion
- KVStore lowest observed throughput is ds=skiptable op=set qps=71633.230000, likely first bottleneck candidate.
- SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace.
- RANGE/SORT were skipped in this run scope.
