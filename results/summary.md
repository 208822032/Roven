# Benchmark Summary

| ds | op | kvstore_qps | redis_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | redis_p50(ms) | redis_p95(ms) | redis_p99(ms) | notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| array | set | 122249.380000 | 111111.120000 | 0.223000 | 0.503000 | 0.799000 | 0.223000 | 0.415000 | 0.623000 | set/get aligned; kv/redis qps=1.100 |
| array | get | 115473.450000 | 107874.870000 | 0.231000 | 0.503000 | 0.751000 | 0.215000 | 0.463000 | 0.719000 | set/get aligned; kv/redis qps=1.070 |
| hash | set | 107296.140000 | 106837.610000 | 0.255000 | 0.663000 | 0.935000 | 0.215000 | 0.463000 | 0.711000 | set/get aligned; kv/redis qps=1.004 |
| hash | get | 117647.050000 | 113378.680000 | 0.239000 | 0.567000 | 0.831000 | 0.215000 | 0.391000 | 0.599000 | set/get aligned; kv/redis qps=1.038 |
| rbtree | set | 89126.560000 | 113765.640000 | 0.447000 | 0.911000 | 1.303000 | 0.207000 | 0.431000 | 0.615000 | set/get aligned; kv/redis qps=0.783 |
| rbtree | get | 93283.580000 | 121065.380000 | 0.407000 | 0.831000 | 1.279000 | 0.207000 | 0.351000 | 0.543000 | set/get aligned; kv/redis qps=0.771 |
| skiptable | set | 82644.620000 | 114810.560000 | 0.511000 | 0.887000 | 1.119000 | 0.207000 | 0.431000 | 0.799000 | set/get aligned; kv/redis qps=0.720 |
| skiptable | get | 82304.520000 | 106609.800000 | 0.503000 | 0.967000 | 1.503000 | 0.231000 | 0.455000 | 0.663000 | set/get aligned; kv/redis qps=0.772 |

## Conclusion
- KVStore lowest observed throughput is ds=skiptable op=get qps=82304.520000, likely first bottleneck candidate.
- SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace.
- RANGE/SORT were skipped in this run scope.
