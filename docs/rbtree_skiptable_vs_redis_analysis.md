# rbtree / skiptable 与 Redis 压测对比分析

## 1. 结论先行

当前仓库里的压测结果不是一个纯粹的“数据结构微基准”，而是端到端的命令、RESP、网络、分配和存储结构一起参与的结果。就现有结果看，array 和 hash 已经接近 Redis，rbtree 和 skiptable 明显落后，这说明瓶颈不是单一的网络层或者 RESP 层，而是 ordered 结构自身的比较和遍历成本叠加了公共路径上的固定开销。

从 [results/summary.md](../results/summary.md) 看，当前结果最清楚的模式是：

| ds | op | KVStore qps | Redis qps | 比值 |
|---|---:|---:|---:|---:|
| array | set | 139664.80 | 128369.71 | 1.088 |
| array | get | 133868.81 | 123915.74 | 1.080 |
| hash | set | 138504.16 | 136239.78 | 1.017 |
| hash | get | 136612.02 | 129870.13 | 1.052 |
| rbtree | set | 111731.84 | 127877.23 | 0.874 |
| rbtree | get | 112233.45 | 126742.72 | 0.886 |
| skiptable | set | 92081.03 | 132978.73 | 0.692 |
| skiptable | get | 95969.28 | 131752.31 | 0.728 |

这意味着：

1. array / hash 的吞吐已经能压到和 Redis 同一个量级，说明公共路径并没有把所有后端都拖垮。
2. rbtree 约比 Redis 慢 11% 到 13%，skiptable 约比 Redis 慢 27% 到 31%，差距不是小噪声。
3. 当前结果文件里只有 populate / set / get，没有 range / sort 的实测数值，所以关于 range / sort 的判断只能先基于源码路径做分析，不能当成当前 benchmark 的直接结论。

## 2. 这次 benchmark 到底在测什么

benchmark 脚本在 KVStore 和 Redis 两侧对齐了同一组核心参数。脚本默认把并发、请求数、pipeline、value size 和 keyspace 作为统一输入，并且 Redis 侧通过自定义命令和 KVStore 侧的命令模板对齐语义，入口见 [scripts/benchmark/bench_all.sh](../scripts/benchmark/bench_all.sh#L12-L17) 和 [scripts/benchmark/bench_all.sh](../scripts/benchmark/bench_all.sh#L120-L130)。KVStore 侧的命令模板在 [scripts/benchmark/bench_all.sh](../scripts/benchmark/bench_all.sh#L188-L205)，Redis 侧的对照命令在 [scripts/benchmark/bench_all.sh](../scripts/benchmark/bench_all.sh#L249-L257)。脚本最后还明确写了“SET/GET are directly aligned using redis-benchmark custom commands and same c/n/P/value_size/keyspace”，见 [scripts/benchmark/bench_all.sh](../scripts/benchmark/bench_all.sh#L333)。

这说明当前对比口径是“外部语义对齐”，不是“内部实现对齐”。Redis 的 SET/GET 命中的是它自己的哈希表路径，而 KVStore 的 rbtree / skiptable 是有序结构路径，所以不能把这组结果解释成“同一算法的不同实现速度比较”。更准确的说法是：这是一组面向应用语义的端到端对比，结果会同时受存储结构、RESP、内存分配和网络模型影响。

另一个需要明确的点是：当前仓库的 benchmark 汇总文件只记录了 populate / set / get，没有 range / sort 的数值，因此不要把 range / sort 的性能判断写成已被当前结果验证。range / sort 的代码路径我在后面单独拆解，但它们的数值还需要后续实验补齐。

## 3. 公平性检查

### 3.1 构建方式不是强优化构建

主目标的编译选项在 [Makefile](../Makefile#L59) 里只显式给了 -g，没有看到 -O2 或 -O3 作为默认开关。这意味着如果 benchmark 二进制就是按默认方式编出来的，它更接近调试构建而不是发布构建。这个因素会压低绝对吞吐，并且会放大字符串比较、分支和小对象分配的开销。

### 3.2 运行配置偏向单机命令路径

从代码默认值看，配置系统在 [src/core/kvs_config.c](../src/core/kvs_config.c#L32-L48) 里初始化了 AOF、网络模式、内存后端和同步模式，而 AOF 宏在 [include/kvs_config.h](../include/kvs_config.h#L30) 中定义为 1。仓库里的示例配置文件又把主路径改成了更轻的组合：AOF 关闭、network_mode=reactor、mem_backend=pool、sync_mode=ebpf、log_level=error，见 [config/master.conf](../config/master.conf#L7-L33)。

这说明 benchmark 如果按示例配置跑，测到的重点就应该是单机 RESP / 网络 / 存储路径，而不是 AOF 或复制链路。换句话说，当前结果不能拿来证明“持久化很慢”，因为持久化路径已经不在主路径里。

### 3.3 结果文件里没有 raw log

仓库里没有保留下来的单次 benchmark 原始日志，当前可直接依赖的结果文件就是 [results/summary.md](../results/summary.md) 和对应的原始表格文件。这个限制不影响方向判断，但会让某些细粒度解释只能停留在源码证据层，不能直接用日志回放验证。

## 4. 为什么 rbtree / skiptable 会低于 Redis

### 4.1 先说公共开销：这些都不是“零成本”路径

当前命令路径里有几层固定成本是所有后端都会付出的。

| 公共开销 | 证据 | 影响 |
|---|---|---|
| 全局锁 | [src/core/kvstore.c](../src/core/kvstore.c#L72-L98) | 每条命令都要经过同一把锁，读写都被串行化 |
| 每请求响应缓冲 | [src/network/net_session.c](../src/network/net_session.c#L119-L199) | 每条命令都分配 2MB 的 response_buf，再拷贝到 send_buf |
| RESP 解析与 token 分配 | [src/network/resp_protocol.c](../src/network/resp_protocol.c#L196-L223)，[src/network/resp_protocol.c](../src/network/resp_protocol.c#L434) | 每条命令都会做解析状态管理和 bulk token 的堆分配 |
| RESP 生成 | [src/network/resp_protocol.c](../src/network/resp_protocol.c#L590-L634) | bulk string 仍然要 strlen + snprintf，返回值还要重新组包 |
| reactor 收发 | [src/network/reactor.c](../src/network/reactor.c#L111-L165)，[src/network/reactor.c](../src/network/reactor.c#L305-L442) | 每连接 8MB recv/send 缓冲，加上 epoll 收发和 iovec 组装 |
| 大块分配回退 | [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L165-L182) | 2MB response_buf 不会走小对象池，仍然会落到 malloc / jemalloc 回退路径 |

这意味着当前 benchmark 的下限已经被公共层抬高了。即使某个后端的数据结构很轻，它也不可能只靠“算法复杂度”拿到理论峰值。

### 4.2 rbtree 的主要成本在比较和树遍历

rbtree 的 key 结构已经存了 key_len 和 value_len，见 [include/kvs_rbtree.h](../include/kvs_rbtree.h#L23-L24)，但真正执行查找时，热路径仍然是字典序比较和树下降，见 [src/storage/kvs_rbtree.c](../src/storage/kvs_rbtree.c#L11) 和 [src/storage/kvs_rbtree.c](../src/storage/kvs_rbtree.c#L587-L777)。也就是说，它的开销不是“缺少长度元数据”，而是 ordered structure 本身要求不断比较、不断追踪指针、不断维护树形结构。

对于 set / get 这种小 value、短 key 的 workload，单次比较看起来不大，但 100000 key 的 keyspace 和 50 并发会把比较次数、分支预测失误和缓存未命中放大出来。Redis 的 SET / GET 走的是哈希表平均 O(1) 路径，而 rbtree 这里仍然是 O(log n) 级别的搜索成本，所以落后是合理的。

### 4.3 skiptable 的主要成本在多层 forward 搜索和重复比较

skiptable 也已经把 key_len、value_len、key_hash 存进节点，见 [include/kvs_skiptable.h](../include/kvs_skiptable.h#L10-L12)，但它的热路径依然要先算 hash 和长度，再沿多个 forward 层做比较和跳转，见 [src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c#L10-L12)、[src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c#L107-L128)、[src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c#L251-L502)。

skiptable 的问题和 rbtree 相似，但更偏向“比较次数多 + 跳表层级多 + 指针跳转更碎”。它的结构本质上仍然是 ordered access，所以无法像哈希表那样把大多数 SET / GET 请求压成一次平均 O(1) 的键查找。

### 4.4 为什么 array / hash 会更接近 Redis

array 和 hash 在这份仓库里已经有更强的元数据和索引支撑。array 走的是侧索引查找和候选集收集，见 [src/storage/kvs_array.c](../src/storage/kvs_array.c#L360-L825)；hash 也已经有 key_len / value_len / key_hash，并且查找通过索引函数完成，见 [src/storage/kvs_hash.c](../src/storage/kvs_hash.c#L156-L565)。

这就是为什么它们能接近 Redis：当公共路径开销已经很重时，只有额外的数据结构成本足够低，才有机会把总吞吐维持在同一量级。

## 5. RANGE / SORT 为什么会更敏感

当前结果文件没有 range / sort 的数值，但源码已经说明这两类命令会比 set / get 更容易暴露 ordered 结构的成本。

在命令层，range / sort 先把后端结果写到临时文本缓冲，再转成 RESP 数组，见 [src/core/kvs_operation.c](../src/core/kvs_operation.c#L467-L509) 和 [src/core/kvs_operation.c](../src/core/kvs_operation.c#L934-L1022)。这条路径意味着两次字符串处理：先构造多行文本，再做 strtok 级别的拆分和 RESP 重组。

在 rbtree 和 skiptable 侧，range / sort 本身又需要遍历或收集节点，见 [src/storage/kvs_rbtree.c](../src/storage/kvs_rbtree.c#L729-L777) 和 [src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c#L404-L502)。因此这两类命令的主要成本会进一步偏向遍历、比较和输出，而不是单纯的 key lookup。

这也是为什么 range / sort 必须单独做实验，不能直接拿 set / get 的结果推断。

## 6. 证据链汇总

| 结论 | 证据文件 |
|---|---|
| benchmark 口径是对齐的 | [scripts/benchmark/bench_all.sh](../scripts/benchmark/bench_all.sh#L12-L17)，[scripts/benchmark/bench_all.sh](../scripts/benchmark/bench_all.sh#L130)，[scripts/benchmark/bench_all.sh](../scripts/benchmark/bench_all.sh#L191-L205)，[scripts/benchmark/bench_all.sh](../scripts/benchmark/bench_all.sh#L255-L257)，[scripts/benchmark/bench_all.sh](../scripts/benchmark/bench_all.sh#L333) |
| 当前结果里 rbtree / skiptable 明显落后 | [results/summary.md](../results/summary.md) |
| 主目标没有显式优化编译参数 | [Makefile](../Makefile#L59) |
| 运行基线偏向 reactor / pool / AOF off | [src/core/kvs_config.c](../src/core/kvs_config.c#L32-L48)，[include/kvs_config.h](../include/kvs_config.h#L30)，[config/master.conf](../config/master.conf#L7-L33) |
| 每条命令都要走全局锁 | [src/core/kvstore.c](../src/core/kvstore.c#L72-L98) |
| 每条命令都要分配 response_buf 并拷贝到 send_buf | [src/network/net_session.c](../src/network/net_session.c#L119-L199) |
| RESP 解析 / 生成有 token 分配和 strlen / snprintf | [src/network/resp_protocol.c](../src/network/resp_protocol.c#L196-L223)，[src/network/resp_protocol.c](../src/network/resp_protocol.c#L434)，[src/network/resp_protocol.c](../src/network/resp_protocol.c#L590-L634) |
| reactor 连接缓冲很大 | [src/network/reactor.c](../src/network/reactor.c#L111-L165)，[src/network/reactor.c](../src/network/reactor.c#L305-L442) |
| 大块分配会回退到 malloc / jemalloc 路径 | [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L165-L182) |
| rbtree 的查找和输出都依赖树遍历 | [src/storage/kvs_rbtree.c](../src/storage/kvs_rbtree.c#L11)，[src/storage/kvs_rbtree.c](../src/storage/kvs_rbtree.c#L587-L777) |
| skiptable 的查找和输出都依赖多层 forward 搜索 | [src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c#L10-L12)，[src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c#L107-L128)，[src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c#L251-L502) |
| array / hash 已经有更强的元数据和索引支撑 | [src/storage/kvs_array.c](../src/storage/kvs_array.c#L360-L825)，[src/storage/kvs_hash.c](../src/storage/kvs_hash.c#L156-L565) |
| range / sort 还会额外经历文本缓冲到 RESP 数组的转换 | [src/core/kvs_operation.c](../src/core/kvs_operation.c#L467-L509)，[src/core/kvs_operation.c](../src/core/kvs_operation.c#L934-L1022) |

## 7. 建议补的验证实验

| 假设 | 怎么验证 | 预期现象 |
|---|---|---|
| 绝大部分小请求损耗来自公共层 | 把 response_buf 改成连接级复用，减少每请求 malloc / free 和拷贝 | 所有后端都会涨速，rbtree / skiptable 的相对差距会缩小一些，但不会消失 |
| 当前构建偏保守 | 用同一配置重编译为 -O2 或 -O3，再跑同一套 benchmark | 绝对 qps 提升，且 compare-heavy 的后端提升更明显 |
| rbtree / skiptable 的主要差距来自结构本体 | 绕开 RESP / 网络，直接调用后端 set / get / range / sort 函数做微基准 | 如果差距仍然显著，说明主要问题在数据结构；如果差距大幅缩小，说明公共层占比更高 |
| pipeline 深度能暴露网络瓶颈 | 固定 keyspace 和 value size，逐步提高 P 和 C | 如果吞吐随 pipeline 明显上升，说明网络 / syscall 成本占比高；如果不敏感，说明 CPU / 分配更重 |
| range / sort 会放大 ordered 结构差异 | 额外补跑 RRANGE / RSORT / SRANGE / SSORT | rbtree / skiptable 相对 Redis 的差距大概率会比 set / get 更大 |
| 热点是否集中在比较和分配 | 用 perf 或火焰图看 rbtree_key_compare、skiptable_key_compare、resp_parser、resp_generate_bulk_string、net_session_process_resp、mem_alloc | 如果这些函数占比高，就能直接确认当前解释成立 |

## 8. 可执行优化方案

1. 先做最便宜的修正：把主目标切到发布编译，至少补上 -O2，最好在 benchmark 配置里固定 release 构建。
2. 把每请求 response_buf 的 2MB 申请改成连接级复用，尽量减少 malloc / free / memcpy 的次数。
3. 让 RESP 层尽量携带长度信息，避免 GET / range / sort 反复 strlen；既然节点里已经存了 key_len 和 value_len，就不要再把长度信息丢掉。
4. 收窄全局锁的作用域。如果读路径允许，可以把纯读命令从写锁改成更轻的读锁，或者进一步拆分为更细粒度的同步。
5. 对 rbtree 和 skiptable 做结构级优化时，优先压比较次数和指针跳转，而不是只做微调。skiptable 可以先盯 hash / len 复用，rbtree 可以先盯长度快速拒绝和比较短路。
6. 补齐 range / sort 的 benchmark，再决定是否需要专门为 ordered 结构做路径优化。没有 range / sort 数据，就很难判断这些结构的真正上限。

## 9. 一句话总结

当前结果不是“Redis 天生比 KVStore 快”，而是“Redis 的 SET / GET 走的是更轻的哈希表路径，而 KVStore 的 rbtree / skiptable 在端到端链路里还叠加了全局锁、RESP 解析/生成、每请求大缓冲分配和 ordered 结构比较成本”。

如果下一步只做一个实验，我建议先做 release 构建，再做纯后端微基准。这样最容易把“构建常数”和“数据结构本体”分开。