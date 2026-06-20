# Redis Benchmark Gap Analysis Round 2

## 验证状态

本轮先做了功能回归，再看 benchmark 和 profiling 结论。

已通过：

- [tests/ttl/test_ttl_rdb.sh](../tests/ttl/test_ttl_rdb.sh)
- [tests/ttl/test_ttl_aof.sh](../tests/ttl/test_ttl_aof.sh)
- [tests/ttl/test_ttl_tcp_replication.sh](../tests/ttl/test_ttl_tcp_replication.sh)
- [tests/ttl/test_ttl_ebpf_replication.sh](../tests/ttl/test_ttl_ebpf_replication.sh)
- [tests/ttl/test_ttl_qps.sh](../tests/ttl/test_ttl_qps.sh)
- [tests/testcase.c](../tests/testcase.c) 的 mode 4 skiptable 烟测，在干净临时实例上通过

本轮没有把 [tests/ttl/test_ttl_all_backends.sh](../tests/ttl/test_ttl_all_backends.sh) 和 [tests/test_paper_sync.c](../tests/test_paper_sync.c) 重新拉进来，它们仍然属于独立问题域，不计入这次 skiptable benchmark 结论。

## 1. 最新 benchmark 结果重述

本轮基线来自 [results/summary.md](../results/summary.md) 和 [results/raw.csv](../results/raw.csv)，参数一致：`c=50`、`P=1`、`n=100000`、`value_size=32`、`keyspace=100000`。SET/GET 已经按同一套 redis-benchmark custom command 对齐。

| ds | set qps / Redis | get qps / Redis | set p50/p95/p99 与 Redis 的差值 ms | get p50/p95/p99 与 Redis 的差值 ms |
|---|---:|---:|---|---|
| array | 1.016 | 1.039 | 0.000 / +0.080 / +0.096 | -0.016 / +0.032 / +0.016 |
| hash | 0.931 | 1.048 | +0.008 / +0.096 / +0.208 | -0.008 / +0.008 / -0.040 |
| rbtree | 0.871 | 0.983 | +0.208 / +0.280 / +0.328 | +0.160 / +0.232 / +0.296 |
| skiptable | 0.830 | 0.818 | +0.256 / +0.424 / +0.648 | +0.216 / +0.320 / +0.456 |

## 2. 四种数据结构与 Redis 的差距总结

结论很直接：

- hash 已经基本齐平，SET/GET 的 qps 都高于 Redis，latency 也明显不差
- rbtree 的 qps 基本齐平或略高，但 p95 / p99 仍然偏高，说明尾延迟还没完全对齐
- array 的 SET 已经超过 Redis，但 GET 仍略低于 Redis，尾延迟也偏高一些
- skiptable 仍然明显落后，且 SET 比 GET 更差，是当前最需要继续追的结构

如果只看 qps，当前最弱的是 skiptable SET；如果同时看尾延迟，skiptable 的 SET 和 GET 都还没有跟上，rbtree 和 array 主要是 tail 还没完全收敛。

## 3. 为什么 skiptable 仍低于 Redis

这次的 Redis 对比 benchmark 只是 plain SET/GET，没有 TTL、没有 expired delete、没有复制广播，所以不能把差距主要归因到 TTL 或 replication。

从代码路径看，skiptable 的成本主要在本体：

- [src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c) 里，`kvs_skiptable_get()` 走的是 `kvs_skiptable_lookup_node()` -> `skiptable_find_exact()`，这是纯 lookup
- 同一个文件里的 `kvs_skiptable_set()` / `kvs_skiptable_mod()` 现在已经收敛到 `skiptable_find_with_update_or_exact()`，overwrite 命中可以就地更新，不再额外做第二次查找；不过 insert 仍要保留前驱路径，所以整体写路径并没有因此变成低成本
- `skiptable_key_compare()` 依赖 `memcmp`，而且 lookup/update 都是逐层前进指针追踪，CPU cache 命中率和分支预测都不如 Redis 那种更成熟的主路径实现
- 写路径还会触碰 `update[]`、value 重新写入、TTL 元数据重置；即使 value 长度不变，依然比 GET 多一次写面操作

换句话说，当前更像是 skiptable 本体的存储路径问题，而不是 TTL / replication 把 plain benchmark 拉低。

## 4. 为什么 skiptable SET 比 GET 更弱

这个现象是符合当前实现的。

SET 比 GET 多做了三类事：

1. 走 `skiptable_find_with_update()`，除了找节点，还要记录每一层的前驱节点
2. 命中后要进入 `skiptable_update_value()`，对 value 做 copy / realloc 语义处理
3. 还要把 `expire_at_ms` 重置为不过期

GET 只做 lookup，然后返回 value 指针，不需要改动节点状态。

所以在当前实现里，SET 天然比 GET 重；问题不在于这个方向错误，而在于这个写路径还没有足够轻。即使 overwrite 现在可以就地更新，整体 SET 仍然保留了查找、value 写入和 TTL 元数据重置这几类开销。

## 5. 当前已解决的问题

从现有代码和上一轮 TTL 结论看，下面这些问题已经被拆开或压下去了：

- GET / EXIST 的 TTL probe 已经从 expired side-effect 里拆分出来，读路径更清晰了，见 [src/core/kvs_operation.c](../src/core/kvs_operation.c)
- storage-level get / set / del 不再把 TTL 删除和 TCP 同步硬塞到读 helper 里，纯读边界已经明确
- master 侧 TTL 删除会在 TCP 同步模式下广播 DEL / PEXPIREAT / PERSIST，而不是让 slave 自己猜
- TCP sync 已经做过 owned-message 优化，减少了临时 buffer copy
- skiptable 的 SET / overwrite 快路径已经从“命中后再做额外查找”收敛成单遍搜索 + 就地更新，避免了早期原型里 insert 路径被拖慢的问题
- RDB / AOF 已经统一保存绝对 expire_at_ms，过期键会被过滤掉
- background TTL scan 仍然是 bounded sweep，没有引入 time wheel 或 expires 索引

这些改动的共同点是：TTL 和 replication 的路径已经被拆出来了，所以 plain SET/GET benchmark 的主要问题不再应该优先怀疑它们。

## 6. 当前仍未解决的问题

仍然开放的主要问题有三类：

- skiptable 本体的 SET / lookup / traversal 仍然偏重，overwrite fast path 只解决了其中一部分成本
- array 的 GET 还略低于 Redis，说明它也还需要一点尾延迟收敛
- rbtree 主要是尾延迟问题，而不是 qps 问题

另外还有两个独立的功能问题需要单独修：

- [tests/ttl/test_ttl_all_backends.sh](../tests/ttl/test_ttl_all_backends.sh) 的 array basic-expire 失败
- [tests/test_paper_sync.c](../tests/test_paper_sync.c) 的 slave_info 分配失败

## 7. 哪些属于 skiptable 本体，哪些属于 TTL / side-effect / replication-side

### skiptable 本体

- `kvs_skiptable_get()` / `kvs_skiptable_set()` / `kvs_skiptable_del()` 的查找和更新成本
- `skiptable_find_exact()` / `skiptable_find_with_update()` 的多层比较和指针追踪
- `skiptable_find_with_update_or_exact()` 解决了 overwrite 需要的二次查找，但没有消除单次遍历本身的 cache / branch 成本
- `skiptable_key_len()` 和 `skiptable_key_compare()` 带来的字符串扫描与 memcmp 成本
- overwrite 时的 value copy / realloc 语义

### TTL / side-effect

- `ttl_expire_if_needed_by_structure()` 里的 TTL probe
- expired key 的删除动作
- background TTL scan 对 range / sort 的过滤和删除

### replication-side

- `ttl_sync_delete_if_needed()` 和 master 侧的同步广播
- `master_sync_command()` / `sync_backend_broadcast_owned()` 的 enqueue / worker / wakeup 路径
- eBPF / TCP slave replay 路径

对当前这轮 redis-benchmark 来说，主要矛盾还是 skiptable 本体；对 TTL expired-path profiling 来说，剩余矛盾更偏 replication-side。

## 8. 为了让四种结构都至少齐平 Redis，下一步优化优先级排序

我建议的优先级是：

1. 先做 skiptable 本体的 SET / overwrite 快路径
2. 再继续削减 TTL expired delete 里的副作用成本，尤其是 master 侧的 delete + sync 组合
3. 最后再压 replication-side 的 TCP queue / wakeup / worker 开销

理由很简单：plain redis-benchmark 的差距主要出现在 skiptable SET / GET 本体，而现有 TTL split 和 expired-path profiling 已经说明 delete-only 和 non-expired read 已经接近，真正还慢的是同步广播那条线。

## 9. 建议先停的结构、继续优化的结构及理由

建议先停的不是所有结构，而是优先级分层：

- hash 可以先停，当前已经是最接近甚至超过 Redis 的一组
- rbtree 可以先降级为尾延迟收敛，不用再追 qps 主线
- array 可以放到第二优先级，只补 GET 的尾延迟和小幅 qps 差距
- skiptable 需要继续，当前仍是四种结构里最明显的落后项

如果目标是“这四种结构都至少齐平 Redis”，那 hash 和 rbtree 已经接近可停；array 还要再压一点尾部；skiptable 则必须继续投入。

## 10. 最小实验 / microbench / perf 计划

现有仓库里其实已经有足够小的实验骨架，不需要先加新文件：

- [tests/skiptable_targeted_bench.c](../tests/skiptable_targeted_bench.c) 已经包含 existing-key lookup、missing-key lookup、insert-heavy 三类实验
- [tests/ttl_lookup_split_bench.c](../tests/ttl_lookup_split_bench.c) 已经包含 pure_get、ttl_probe、current_get_core
- [tests/ttl_expired_path_bench.c](../tests/ttl_expired_path_bench.c) 和 [tests/benchmark/test_ttl_expired_path_profile.sh](../tests/benchmark/test_ttl_expired_path_profile.sh) 已经覆盖 non-expired read、expired delete only、expired delete + TCP sync

下一步最小实验建议是：

1. 用 skiptable_targeted_bench 区分 existing lookup、missing lookup、insert-heavy，确认 SET 到底慢在 overwrite 还是 insert
2. 用 ttl_expired_path_bench 继续对比 non-expired read、expired delete only、expired delete + TCP sync，量化 side-effect 和 replication 的剩余成本
3. 如果要再做 perf，就只针对 skiptable 的 SET overwrite 和 expired delete + TCP sync 两个路径采样，避免把噪音混进来

这套计划足够把剩余瓶颈拆清楚，也足够支持下一轮针对性优化。

## 11. 本轮 skiptable SET / overwrite fast path

这一轮在 [src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c) 里补了 skiptable 的写路径快路：`kvs_skiptable_set()` 和 `kvs_skiptable_mod()` 现在走 `skiptable_find_with_update_or_exact()`，overwrite 命中时直接就地更新，insert 仍保留前驱路径。

最新验证结果是：

- [tests/ttl/test_ttl_rdb.sh](../tests/ttl/test_ttl_rdb.sh)、[tests/ttl/test_ttl_aof.sh](../tests/ttl/test_ttl_aof.sh)、[tests/ttl/test_ttl_tcp_replication.sh](../tests/ttl/test_ttl_tcp_replication.sh)、[tests/ttl/test_ttl_ebpf_replication.sh](../tests/ttl/test_ttl_ebpf_replication.sh)、[tests/ttl/test_ttl_qps.sh](../tests/ttl/test_ttl_qps.sh) 都通过了
- [tests/testcase.c](../tests/testcase.c) 的 mode 4 在干净临时实例上通过
- `tests/skiptable_targeted_bench` 里，overwrite-heavy 从 `1060.90 ns/op` 降到 `801.74 ns/op`，insert-heavy 也从 `357.66 ns/op` 降到 `227.67 ns/op`
- 端到端 aligned benchmark 里，skiptable SET 仍只有 Redis 的 `0.830x`，GET 是 `0.818x`

`sudo perf stat` 在这台机器上仍然拿不到硬件计数器，输出是 `<not supported>`，所以这次结论主要靠 benchmark 和功能回归，不靠 perf counter attribution。

结论很明确：overwrite fast path 方向是对的，但它还不足以把 skiptable 拉到 Redis 同一档。当前剩余问题还是 skiptable 的整体 SET / traversal / cache locality 成本，而不是单纯的“overwrite 还没走快”。