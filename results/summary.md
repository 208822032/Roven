# Benchmark Summary

| ds | op | kvstore_qps | redis_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | redis_p50(ms) | redis_p95(ms) | redis_p99(ms) | notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| array | set | 142247.520000 | 128040.970000 | 0.175000 | 0.311000 | 0.559000 | 0.191000 | 0.359000 | 0.591000 | set/get aligned; kv/redis qps=1.111 |
| array | get | 146627.560000 | 129032.270000 | 0.167000 | 0.263000 | 0.415000 | 0.191000 | 0.303000 | 0.479000 | set/get aligned; kv/redis qps=1.136 |
| hash | set | 142045.450000 | 122850.120000 | 0.175000 | 0.279000 | 0.431000 | 0.207000 | 0.295000 | 0.479000 | set/get aligned; kv/redis qps=1.156 |
| hash | get | 134589.500000 | 129366.110000 | 0.183000 | 0.351000 | 0.647000 | 0.199000 | 0.311000 | 0.415000 | set/get aligned; kv/redis qps=1.040 |
| rbtree | set | 121802.680000 | 123001.230000 | 0.319000 | 0.599000 | 1.287000 | 0.207000 | 0.327000 | 0.479000 | set/get aligned; kv/redis qps=0.990 |
| rbtree | get | 134770.890000 | 118906.060000 | 0.279000 | 0.495000 | 0.703000 | 0.215000 | 0.327000 | 0.551000 | set/get aligned; kv/redis qps=1.133 |
| skiptable | set | 122399.020000 | 129870.130000 | 0.351000 | 0.583000 | 0.759000 | 0.199000 | 0.303000 | 0.519000 | set/get aligned; kv/redis qps=0.942 |
| skiptable | get | 116009.280000 | 122100.120000 | 0.367000 | 0.607000 | 0.863000 | 0.207000 | 0.295000 | 0.511000 | set/get aligned; kv/redis qps=0.950 |

## Conclusion
- KVStore lowest observed throughput is ds=skiptable op=get qps=116009.280000, likely first bottleneck candidate.
- SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace.
