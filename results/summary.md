# Benchmark Summary

| ds | op | kvstore_qps | redis_qps | kvstore_p50(ms) | kvstore_p95(ms) | kvstore_p99(ms) | redis_p50(ms) | redis_p95(ms) | redis_p99(ms) | notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| array | set | 1226.890000 | 108577.630000 | 39.359000 | 59.583000 | 73.791000 | 0.223000 | 0.455000 | 0.791000 | set/get aligned; kv/redis qps=0.011 |
| array | get | 1191.280000 | 129032.270000 | 40.895000 | 63.583000 | 77.695000 | 0.191000 | 0.351000 | 0.559000 | set/get aligned; kv/redis qps=0.009 |
| hash | set | 88967.980000 | 123001.230000 | 0.287000 | 0.447000 | 0.607000 | 0.207000 | 0.351000 | 0.543000 | set/get aligned; kv/redis qps=0.723 |
| hash | get | 90579.710000 | 124378.110000 | 0.287000 | 0.423000 | 0.607000 | 0.199000 | 0.367000 | 0.615000 | set/get aligned; kv/redis qps=0.728 |
| rbtree | set | 95785.440000 | 121802.680000 | 0.271000 | 0.455000 | 0.679000 | 0.207000 | 0.391000 | 0.583000 | set/get aligned; kv/redis qps=0.786 |
| rbtree | get | 94786.730000 | 112359.550000 | 0.271000 | 0.431000 | 0.639000 | 0.223000 | 0.383000 | 0.623000 | set/get aligned; kv/redis qps=0.844 |
| skiptable | set | 88495.580000 | 122699.390000 | 0.287000 | 0.511000 | 0.823000 | 0.207000 | 0.367000 | 0.623000 | set/get aligned; kv/redis qps=0.721 |
| skiptable | get | 93632.960000 | 121802.680000 | 0.279000 | 0.431000 | 0.591000 | 0.207000 | 0.367000 | 0.607000 | set/get aligned; kv/redis qps=0.769 |

## Conclusion
- KVStore lowest observed throughput is ds=array op=get qps=1191.280000, likely first bottleneck candidate.
- SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace.
- RANGE/SORT were skipped in this run scope.
