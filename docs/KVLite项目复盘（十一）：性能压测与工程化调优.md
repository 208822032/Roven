# 第十一章 性能压测与工程化调优：从能跑到能扛压

## 11.1 为什么功能做完之后必须做压测

最开始我以为 testcase 全 PASS 就够了。

因为从功能角度看，SET 能写、GET 能读、DEL 能删，RESP 能解析，AOF/RDB 能恢复，TTL 能过期，主从能同步，向量 VSEARCH 也能返回结果。到这里，系统看起来已经“完成”了。

但后来做压测才发现，功能正确和系统能扛压是两回事。

功能测试通常只证明：

```text
在小规模输入下，返回结果符合预期。
```

它不能回答这些问题：

```text
QPS 能到多少？
P95 / P99 延迟怎么样？
数据量上来后是否退化？
Pipeline 下响应会不会乱序？
AOF fsync 会不会拖垮写路径？
TTL 过期删除会不会造成延迟尖刺？
向量搜索在 10 万条数据下还能不能稳定？
复制队列堆积时会不会反压主链路？
```

如果面试里只说“我实现了 SET/GET、AOF/RDB、主从复制、HNSW”，其实还停留在功能清单层面。后端系统更关心的是：这些功能放到压力环境下是否还站得住。

所以我后来对压测的理解变成了：

> 功能测试证明系统能跑，压测证明系统在压力下还能不能稳定地跑。

压测的价值不是炫 QPS 数字，而是验证设计取舍。比如 AOF `always` 和 `everysec` 的差距，不是为了证明哪个数字更好，而是为了理解“可靠性和吞吐量”之间的代价。HNSW 的 `ef_search` 也不是越小越好，因为 QPS 上去了，召回可能掉下来。

这一章要复盘的不是“KVLite 跑出了多少分”，而是我怎么一步步从功能验证走到性能验证，再用压测结果反过来指导优化。

## 11.2 从 testcase 到 benchmark：正确性测试和性能测试不是一回事

KVLite 里有两类测试。

第一类是 testcase，比如 `tests/testcase.c`、TTL 脚本、向量功能测试、主从复制测试。这类测试关注的是功能正确性。

它们通常是：

```text
发一条命令
→ 等响应
→ 和预期结果比对
```

比如：

```text
SET key value  是否返回 +OK
GET key        是否返回 value
EXPIRE key 1   是否能过期
RANGE          是否按预期返回
主从同步后     slave 是否读到 master 的数据
```

`tests/testcase.c` 里还专门覆盖了 Pipeline 的功能验证：一次发送多条 RESP 命令，然后按 RESP 语义解析多个响应，确认响应数量和顺序没有错。这类测试很重要，因为没有正确性，谈性能没有意义。

第二类是 benchmark，比如：

- `scripts/benchmark/bench_all.sh`
- `scripts/benchmark/kvstore_resp_bench.py`
- `scripts/benchmark/bench_vector_rw.sh`
- `scripts/benchmark/bench_vector_ef_compare.sh`
- `tests/vector_benchmark.c`
- `tests/ttl_lookup_split_bench.c`
- `tests/skiptable_targeted_bench.c`
- `tests/ttl_expired_path_bench.c`
- `tests/mempool_test.c`

benchmark 关注的是另一组问题：

```text
吞吐量
延迟分布
P50 / P95 / P99
多连接并发
Pipeline 深度
资源占用
长时间运行稳定性
```

比如 `scripts/benchmark/bench_all.sh` 会用同一组参数对 KVLite 和 Redis 做对齐压测：`c=50`、`n=100000`、`P=1`、`value_size=32`、`keyspace=100000`。它不是只看某一条命令是否正确，而是统计 QPS、平均延迟、P50、P95、P99。

`tests/vector_benchmark.c` 则把向量压测拆成两种模式：

```text
latency    单连接串行请求，重点看单次请求延迟
throughput 多连接 + pipeline，重点看服务端吞吐极限
```

这个拆分很关键。单连接串行更接近“单请求成本”，多连接 Pipeline 更接近“系统饱和能力”。如果混在一起看，很容易误读结果。

所以 testcase 和 benchmark 的关系是：

```text
testcase  保证系统是对的
benchmark 证明系统在压力下表现如何
```

两者缺一不可。

## 11.3 SET / GET 基础压测：先看普通 KV 主链路

性能压测不能一上来就打开所有功能。

我最先看的还是 SET / GET，因为它们代表普通 KV 的主链路。

SET 会覆盖：

```text
网络接收
→ RESP 解析
→ command_table 分发
→ 参数解析
→ 写锁
→ 存储结构写入
→ 响应返回
```

GET 会覆盖：

```text
网络接收
→ RESP 解析
→ command_table 分发
→ 读锁
→ 存储查找
→ TTL 判断
→ 响应返回
```

基础压测的目的不是把数字刷到最高，而是建立 baseline。只有 baseline 稳了，后面才知道 TTL、AOF、Pipeline、不同数据结构、向量检索分别带来了多少影响。

当前 `results/summary.md` 里保留了一组和 Redis 对齐的 SET/GET 压测数据。参数是同口径的 `c=50`、`n=100000`、`P=1`、`value_size=32`、`keyspace=100000`。

这组数据里，KVLite 四种结构都在 10 万 QPS 左右：

```text
array     SET 115340 QPS，GET 108696 QPS
hash      SET 107296 QPS，GET 115340 QPS
rbtree    SET 114548 QPS，GET 115473 QPS
skiptable SET 102775 QPS，GET 105820 QPS
```

这些数字本身不是重点。重点是它告诉我：

1. 普通 SET/GET 主链路已经具备基本吞吐能力。
2. hash、array、rbtree 的点查路径大体可接受。
3. skiptable 是四种结构里最弱的一组，后续优化应该优先关注它。

这就是 baseline 的价值：它不是终点，而是后续对比的参照系。

## 11.4 Pipeline 压测：为什么网络往返会成为瓶颈

一问一答模式下，客户端每发一条命令，都要等一次响应。

```text
send SET
wait response
send GET
wait response
...
```

这种模式下，吞吐很容易被 RTT 和系统调用开销限制。即使服务端处理一条命令很快，客户端也在不断等待网络往返。

Pipeline 的思路是一次发多条命令：

```text
send SET + SET + GET + DEL + ...
read response + response + response + ...
```

这会显著提高吞吐，但它对服务端提出了更高要求：

```text
recv_buf 要能保存多条命令
resp_peek_command 要能逐条识别完整命令
resp_parser 要能重置并继续解析下一条
send_buf 要能累积多个响应
响应顺序必须和请求顺序一致
```

KVLite 的 `net_session_process_resp` 就是在验证这个设计。它从 `recv_buf` 里循环调用 `resp_peek_command`，确认一条完整命令后再交给 `resp_parser`，执行 handler，把响应追加到 `send_buf`。处理完一批后，再把已消费的字节从接收缓冲区裁掉。

所以 Pipeline 压测本质上是在验证第三、第四章的网络层和 RESP 设计是否真的成立。

`tests/testcase.c` 里有 mode 5 的大 Pipeline 测试，README 里也记录了“单次 100 万条命令”的验证。`shutdown_segfault_fix_report.md` 里保留过一次 testcase mode 5 pipeline 的结果：普通编译下 qps 约 `255,427`，ASan 编译下也能通过，只是 qps 降到约 `125,391`。

这里我更关心的不是这个 qps 数字，而是三个问题：

1. 多条命令粘在一起时，RESP 解析是否正确。
2. 多个响应是否按请求顺序返回。
3. 大 Pipeline 下 recv_buf / send_buf 是否异常增长或越界。

Pipeline 能把吞吐打上去，但它也最容易暴露缓冲区、解析边界和响应乱序问题。

## 11.5 四种数据结构性能对比：array、hash、rbtree、skiptable

KVLite 支持 array、hash、rbtree、skiptable 四种结构。它们不是为了证明“我会写四种数据结构”，而是为了观察不同结构在不同访问模式下的性能边界。

array 最简单。早期用它跑通 SET/GET 很合适，但如果查找仍然靠线性扫描，数据量上来后一定退化。

hash 适合点查。SET/GET 这种 key-value 精确访问，平均 O(1) 的 hash 路径最自然。

rbtree 和 skiptable 的点查是 O(log n)，理论上不如 hash 直接，但它们支持有序遍历，所以 RANGE / SORT 更适合放在这类结构上讨论。

所以压测四种结构时，不能只看 SET/GET。

```text
SET/GET    看点查和写入成本
RANGE      看有序遍历能力
SORT       看排序/扫描成本
LIMIT      看能否提前停止，避免全量遍历
```

`docs/redis_benchmark_gap_analysis_round2.md` 对四种结构做过一轮分析。结论很清楚：hash 已经基本接近或超过 Redis 对齐结果；rbtree QPS 接近，但尾延迟还需要收敛；skiptable 仍然是明显弱项。

skiptable 慢不是因为 TTL 或复制拖累，因为那轮 plain SET/GET benchmark 没有打开这些复杂路径。更像是 skiptable 本体路径成本：

```text
多层指针追踪
memcmp 比较
update[] 前驱数组维护
overwrite 时 value 更新
cache locality 不如成熟实现
```

后续 `skiptable_targeted_bench` 又把问题拆得更细。优化后：

```text
insert-heavy    从 357.66 ns/op 降到 227.67 ns/op
overwrite-heavy 从 1060.90 ns/op 降到 801.74 ns/op
```

但端到端 benchmark 里 skiptable SET 仍然只有 Redis 的约 `0.83x`。这个结果很有价值：它说明局部优化有效，但不足以完全解决整体路径问题。

面试时我会这样讲：

> 小数据量和原型阶段，array 简单；点查场景 hash 最合适；需要有序 RANGE/SORT 时，rbtree 和 skiptable 才有价值。压测不是为了证明某个结构永远最好，而是为了说明不同结构服务不同访问模式。

## 11.6 TTL 对性能的影响：过期判断、惰性删除和后台扫描

TTL 在功能上是一条 EXPIRE 命令，但在性能上会影响读写路径。

读路径多了一个判断：

```text
找到 key
→ 检查 expire_at_ms
→ 如果没过期，正常返回
→ 如果过期，用户看到 NO EXIST
```

如果 key 已经过期，事情更复杂。读路径可能从“纯读”变成“删除路径”，因为系统需要把过期 key 从数据结构里清掉，还可能触发主从同步里的 DEL。

这就是 TTL 性能压测要拆开的原因。

`docs/benchmark_optimization_progress.md` 里记录过 TTL read path split。它把读路径拆成：

```text
pure lookup
TTL probe
expired side-effect mutation
```

这一步的意义不是为了让代码更好看，而是为了知道成本到底在哪里。

`tests/ttl_lookup_split_bench.c` 的微基准保留过一组结果：

```text
rbtree pure_get          607.75 ns/op
rbtree ttl_probe         618.62 ns/op
rbtree current_get_core  583.83 ns/op

skiptable pure_get       765.60 ns/op
skiptable ttl_probe      1000.34 ns/op
skiptable current_get    854.28 ns/op
```

这些数字说明，TTL 在 skiptable 上的 probe 成本更明显。它不一定是最大瓶颈，但已经能被微基准观察到。

TTL 还会带来后台扫描成本。`kvs_ttl_background` 会周期性扫描过期 key，避免只依赖惰性删除。扫描太频繁会消耗 CPU，扫描太慢又会让过期 key 堆积。

所以 TTL 压测不能只测“有 TTL 的 GET”，至少要看：

```text
无 TTL vs 有 TTL
大量未过期 key
大量已过期 key
后台扫描开启时的 QPS
惰性删除是否造成延迟尖刺
过期删除是否触发复制成本
```

`ttl_expired_path_bench` 后来又把 expired path 拆成：

```text
non-expired read
expired delete only
expired delete + TCP sync
```

保留结果里 skiptable 最新一组是：

```text
non-expired read          16471 QPS
expired delete only       16364 QPS
expired delete + TCP sync 15620 QPS
```

这个结果说明，过期删除本身已经不是最大问题，TCP sync 仍然会带来额外成本。这就是压测指导优化的典型例子：不要凭感觉猜，要把路径拆开测。

## 11.7 AOF fsync 策略：可靠性和吞吐量的取舍

AOF 压测最适合讲后端系统里的经典取舍。

KVLite 支持三种 fsync 策略：

```text
no       不主动 fsync，依赖 OS flush
everysec 后台线程周期性 fsync
always   每条写命令都 fsync
```

从可靠性看：

```text
always   最安全
everysec 折中
no       风险最高
```

从性能看通常反过来：

```text
no       吞吐最好
everysec 折中
always   最慢
```

原因很简单：`fsync` 是昂贵系统调用。write 只是把数据写到用户态缓冲区或内核 page cache，`fsync` 才要求内核把数据刷到磁盘。每条命令都 fsync，等于每条写命令都要承担磁盘同步成本。

KVLite 的 `aof_append` 里会先把命令写入 AOF buffer。`AOF_FSYNC_ALWAYS` 下会在写命令后触发 `aof_fsync(1)`；`AOF_FSYNC_EVERYSEC` 由后台线程负责周期刷盘；大命令还有直写路径，避免撑爆 AOF buffer。

压测 AOF 时，我不会只报一个“开启 AOF 后 QPS 多少”。更合理的对比是：

```text
AOF off
AOF no
AOF everysec
AOF always
```

然后同时看：

```text
QPS
P95 / P99
写入延迟尖刺
AOF 文件增长
rewrite 触发时的影响
```

这部分的核心表达是：

> AOF always 不是“实现得慢”，而是它选择了更高可靠性；everysec 不是“不安全”，而是用最多丢一小段时间数据换吞吐。

性能压测要服务于取舍，而不是只追最高数字。

## 11.8 RDB save/load：快照恢复快，但生成快照也有成本

RDB 和 AOF 的性能关注点不同。

AOF 恢复要重放命令，命令越多，恢复越慢。RDB 恢复直接加载状态，通常更快。

但 RDB 生成快照也不是免费的。`kvs_rdb_save` 会遍历 array、rbtree、hash、skiptable，还会保存 vector section。数据量越大，save 期间的 CPU、IO、锁竞争越明显。

所以 RDB 压测要看两个方向：

```text
save 成本：运行时生成快照会不会影响服务
load 成本：重启恢复需要多久
```

普通 KV 的 RDB load 是读 key/value/expire_at_ms，然后恢复到对应数据结构。向量模块还多一层成本：RDB 恢复的是 vector record，索引状态还需要 rebuild。`kvstore.c` 启动流程里 RDB/AOF load 后会调用 `kvs_vector_rebuild_after_load()`。

这意味着向量恢复不能只看“文件读完了”，还要看：

```text
record 加载耗时
flat index rebuild 耗时
HNSW 图重建耗时
SQ8 index rebuild 耗时
```

RDB 的优势是恢复当前状态更直接，但它把成本转移到了 save 和 rebuild 上。这个边界在压测时必须讲清楚。

## 11.9 向量检索压测：VSET、VGET、VSEARCH、VDEL 分别看什么

向量模块不能用普通 SET/GET 的思路压测。

V 族命令每个关注点都不同：

```text
VSET     写 record、生成 embedding、写 id_map、写 index
VGET     question + scope 精确查找
VSEARCH  生成 query embedding、索引搜索、距离计算、topK、scope/threshold 过滤
VDEL     逻辑删除、id_map 删除、标记索引 stale
```

`results/vector_rw_summary.md` 保留了一组 100K 向量、64 维、HNSW/IP/auto distance backend 的数据。

Latency 模式下：

```text
VSET     p99 0.16ms
VGET     p99 0.16ms
VSEARCH  p99 2.02ms
VDEL     p99 0.17ms
```

Throughput 模式下：

```text
VSET     133024 QPS，p99 0.74ms
VGET     164761 QPS，p99 5.32ms
VSEARCH  1133 QPS，p99 271.58ms
VDEL     163473 QPS，p99 5.11ms
```

这组数据说明两个问题。

第一，VSET/VGET/VDEL 本质上还是比较轻的 KV/索引操作，多连接 Pipeline 下吞吐能打上去。

第二，VSEARCH 是完全不同的负载。它是 CPU 密集型搜索，吞吐不可能和 VGET 一个量级。尤其 throughput 模式下，50 个客户端、pipeline=4 把服务端压满后，单请求排队时间会上升，所以 p99 会远高于 latency 模式。

所以向量压测不能只说“VSEARCH QPS 是多少”。必须同时说明：

```text
数据量 count
向量维度 dim
topK
index type
metric
distance backend
scope 分布
deleted ratio
是否 rebuild
latency 模式还是 throughput 模式
```

否则数字没有解释力。

## 11.10 flat vs HNSW：精确搜索和近似搜索的性能边界

flat search 是最容易理解的向量搜索：

```text
遍历所有向量
→ 计算 query 和每个向量的距离
→ 维护 topK
```

它的优点是精确，结果可以作为 correctness baseline。缺点是复杂度太直接：

```text
O(n * dim)
```

数据量越大，维度越高，搜索越慢。

HNSW 是近似搜索。它通过图结构缩小候选范围，不再遍历所有向量。代价是结果不一定和 flat 完全一致，需要用 recall 或 topK overlap 来评估。

所以 flat 和 HNSW 的边界是：

```text
小数据量、调试、验证正确性 → flat
大数据量、低延迟召回       → HNSW
```

但 HNSW 也不是永远更好。小数据量下，flat 简单、无图维护成本，可能更划算。HNSW 的优势通常在数据量上来后才明显。

压测时不能只看 QPS，还要看：

```text
P95 / P99
recall@K 或 topK overlap
内存占用
构建/写入成本
删除后的 stale index 成本
rebuild 成本
```

KVLite 当前的向量模块保留 flat，是很有必要的。它不是为了追求性能，而是为了当 HNSW 参数变化时，有一个精确基准可以对照。

## 11.11 ef_search 参数实验：为什么 ANN 不能只看 QPS

HNSW 里 `ef_search` 是一个很典型的性能/质量开关。

`ef_search` 越大，搜索时保留的候选集越大，通常召回更好，但延迟更高。`ef_search` 越小，搜索更快，但更可能漏掉真实近邻。

`results/ef_compare/ef_compare_summary.md` 做过一组实验：基准 `ef_search=256`，测试 `ef_search=128`，数据量 100000，查询 1000 条，topK=10。

结果是：

```text
top1_same_rate      93.2%
top3_overlap_rate   95.3%
top10_overlap_rate  96.5%
avg_missing_rate     3.5%
avg_rank_shift       0.10
```

这个结论比单纯 QPS 有价值。它说明 `ef_search=128` 相比 `256`，结果一致率较高，可以作为轻量默认配置。

README 里还记录了一个更完整的对比：

```text
ef_search=256  p99 23.03ms，throughput 73 QPS，top10 一致率 100%
ef_search=128  p99 2.02ms，throughput 1133 QPS，top10 一致率 96.5%
ef_search=64   p99 1.49ms，throughput 3155 QPS，top10 一致率 90.3%
```

这里最重要的是表达方式：

> ef_search=64 更快，但不是无条件更好；ef_search=128 是当前数据和 hash embedding 下的平衡点。

还要诚实说明：KVLite 当前 `embedding_provider=hash`，它能验证索引流程和结果一致性，但不代表真实语义质量。真正的 recall@K 需要接入真实 embedding，并用 flat 精确搜索或标注数据作为 ground truth 重新测试。

## 11.12 AVX2 距离计算优化：热点函数为什么值得单独抽象

向量搜索的热点通常是距离计算。

flat search 最明显，因为它要对所有向量计算距离。HNSW 虽然减少了候选数量，但每个候选仍然要算距离。

KVLite 把距离计算抽象成 backend：

```text
fallback  标量 C 实现
avx2      AVX2 SIMD 实现
auto      CPU 支持 AVX2 就自动选择，否则 fallback
```

源码里 `kvs_vector_distance.c` 通过函数指针选择当前实现，`kvs_vector_distance_avx2.c` 单独用 `-mavx2` 编译。Makefile 里也把 AVX2 文件单独加了编译规则，避免所有源码都强制依赖 AVX2。

这个设计体现的是性能优化的基本顺序：

```text
先识别热点
→ 再抽象变化点
→ 最后按硬件能力选择实现
```

AVX2 的价值在于一次处理多个 float，减少循环开销，提高 dot、L2、cosine 这类基础操作的吞吐。

但这里也要注意边界。SIMD 只能优化距离计算本身。如果瓶颈在 HNSW 图遍历、cache miss、锁竞争、网络排队，AVX2 不会神奇地解决所有问题。

所以面试里不能只说“我用了 AVX2”。更好的说法是：

> 我把向量搜索里的距离计算抽成 backend，先保留 fallback，AVX2 作为可选优化，通过 auto 检测 CPU 能力选择。这样既能优化热点，又不破坏可移植性。

## 11.13 内存管理：malloc、内存池和 jemalloc 的取舍

C 项目里，内存管理既影响性能，也影响稳定性。

KVLite 支持三种内存后端：

```text
pool
system malloc
jemalloc
```

默认配置里使用自定义内存池。`kvs_mempool.c` 里把小对象按 16、24、32、48、64、96、128 等规格分池，大对象单独走 large block。这样做适合大量小对象分配，比如 key/value 元信息、队列节点、协议解析中间对象等。

普通 malloc 的优点是简单、成熟、行为清楚。缺点是在大量小对象、高并发分配下，可能有碎片和锁竞争。

jemalloc 更适合高并发和长时间运行场景，通常能提供更稳定的分配性能和碎片控制。KVLite 里通过 `mem_backend=jemalloc` 接入，`jemalloc_wrapper.c` 封装了 `je_malloc` / `je_free` / `je_realloc`。

向量模块尤其容易产生内存压力：

```text
record
question / answer / scope
embedding float 数组
id_map
HNSW neighbor 数组
flat index
SQ8 code / scale
```

复制队列、AOF buffer、网络 recv/send buffer 也都依赖内存管理。

`tests/mempool_test.c` 不是简单测“分配快不快”，它还看不同大小对象、保留比例、读多写少模式、大块分配、RSS、page fault 等指标。这比只测一次 malloc/free 更接近真实服务里的分配压力。

面试时不要只说“我用了内存池”。要讲清楚：

```text
哪些对象适合池化
哪些对象应该走大块分配
为什么需要 system malloc 作为 fallback
jemalloc 适合什么场景
释放顺序如何保证
怎么用 ASan / Valgrind 验证没有泄漏和 UAF
```

## 11.14 日志系统：同步日志和异步日志对主链路的影响

日志是排查问题必须的，但日志本身也可能成为性能问题。

如果高 QPS 下每条命令都 `fprintf`、`fflush`，主线程就会把大量时间花在格式化和文件 IO 上。尤其在错误日志风暴里，同步日志可能直接拖垮主链路。

KVLite 的日志系统经历过工程化拆分：

```text
KVS_LOG_* 宏
→ kvs_log.c 统一格式化
→ kvs_log_writer.c 同步或异步写出
```

异步模式下，日志先进入有界队列，再由后台线程批量 flush。`docs/log/phase3_async_logging.md` 里明确写了这个目标：业务线程不再直接承担文件 IO。

但异步日志不是没有代价：

```text
队列满怎么办？
崩溃时队列里的日志会不会丢？
ERROR 日志是否应该丢？
日志顺序和业务顺序如何对应？
退出时是否 flush 剩余日志？
```

当前实现里，队列容量是固定的。队列满时，INFO/DEBUG 可以丢弃，WARN/ERROR 会同步退化写出，同时记录丢弃计数。这是一个很务实的取舍：低优先级日志保性能，高优先级日志保可见性。

这和复制队列、AOF buffer 的思想是一致的：

```text
主流程生成事件
→ 队列削峰
→ 后台批量处理
→ 队列满时必须有明确策略
```

日志系统看起来不是核心业务，但它直接影响压测结果和线上排障能力。工程化调优不能忽略它。

## 11.15 稳定性工具：ASan、Valgrind 和 gdb 如何帮我发现问题

C 项目做性能优化，不能只盯 QPS。

很多时候，QPS 越高，越容易把隐藏的内存问题打出来：

```text
越界写
use-after-free
double free
野指针
buffer 扩容错误
线程退出顺序错误
```

KVLite 里容易出问题的地方很多：

```text
RESP token 生命周期
recv_buf / send_buf 扩容
vector record 释放
HNSW neighbor 数组
AOF/RDB load/free
复制消息引用计数
shutdown 时后台线程访问全局数据结构
```

`docs/shutdown_segfault_fix_report.md` 里记录过一次典型排障：服务关闭时偶发 segfault，dmesg 里多次指向 `handle_mod` / rbtree lookup，最后定位为 shutdown 期间后台线程还可能访问已释放的数据结构，本质是 use-after-free。

修复不是靠猜，而是按证据推进：

```text
dmesg 看崩溃位置
addr2line / gdb 定位源码
分析释放顺序
修正 distributed_shutdown 和 dest_kvengine 生命周期
ASan 编译验证
pipeline / mempool / binary key / shutdown 回归
```

ASan 适合发现：

```text
heap-use-after-free
double-free
invalid-free
heap-buffer-overflow
```

Valgrind 更适合做泄漏检查和非法访问排查，虽然慢，但适合小规模回归。gdb 则用来定位崩溃现场、调用栈和变量状态。

`shutdown_segfault_fix_report.md` 里 ASan 验证结果包括：

```text
test_binary_key 36 passed
mempool_test PASS
testcase mode 5 pipeline PASS
正常 shutdown 干净退出
无 sanitizer 错误
```

这个经历让我意识到：稳定性工具不是性能优化之外的东西，它就是工程化能力的一部分。一个高 QPS 但偶发 UAF 的系统，不能算真正优化完成。

## 11.16 这个模块可以关联哪些八股知识点

### C 语言八股

- `malloc/free`：内存后端、对象生命周期、释放顺序。
- 内存泄漏：AOF buffer、vector record、HNSW neighbor、复制消息都可能泄漏。
- double free：自定义内存池和复杂释放路径里最容易出现。
- use-after-free：shutdown 顺序、后台线程、索引 stale entry 都可能触发。
- 指针生命周期：RESP token、RDB load 临时 buffer、vector result 引用。
- 结构体内存布局：协议头、队列节点、内存池 header。
- 函数指针：command handler、distance backend、tx send callback。
- ring buffer：日志队列、msg_queue、BPF ringbuf。
- heap：topK 小根堆。
- qsort：结果排序、topK 导出。
- 错误码设计：benchmark、parser、decode 都依赖清晰错误返回。

### Linux / 系统八股

- epoll：reactor 模型的事件驱动基础。
- 非阻塞 IO：高并发连接和 Pipeline 处理。
- send / recv：TCP 字节流、短写、短读、断连。
- syscall 开销：Pipeline、tx_batcher、批量 flush 都是为了减少系统调用。
- fsync：AOF always 的核心性能瓶颈。
- page cache：write 成功不等于落盘。
- 文件 IO：AOF append、RDB save/load、日志写盘。
- CPU cache：skiptable 指针跳转和 cache locality。
- SIMD / AVX2：向量距离计算优化。
- jemalloc：高并发内存分配和碎片控制。
- perf / top / strace：热点、CPU、系统调用定位工具。

### 后端八股

- QPS：单位时间完成请求数。
- TPS：写事务或操作吞吐。
- P50 / P95 / P99：延迟分布，不只看平均值。
- Pipeline：减少 RTT 和系统调用开销。
- 吞吐和延迟：高吞吐可能带来排队延迟。
- 背压：慢 slave、满队列、慢磁盘都需要策略。
- 队列削峰：日志、复制、tx_batcher 都是类似思想。
- 批量发送：减少 send 次数，提高吞吐。
- 缓存命中率：hash、skiptable、HNSW 都会受 cache locality 影响。
- 压测方法论：固定变量、对齐参数、分阶段打开功能。

### 存储系统八股

- WAL / AOF：写命令日志。
- snapshot / RDB：状态快照。
- compaction / rewrite：AOF rewrite 压缩历史。
- 索引：hash、rbtree、skiptable、flat、HNSW。
- 主数据和派生索引：vector record 是主数据，HNSW/flat 是派生索引。
- rebuild：删除、恢复后重建索引。
- 可靠性和性能取舍：fsync always/everysec/no。

### 向量检索八股

- flat search：精确全量扫描。
- ANN：近似最近邻。
- HNSW：图索引近似搜索。
- topK：维护最优 K 个结果。
- recall@K：评估 ANN 质量。
- ef_search：召回和延迟的调节参数。
- 向量距离：IP、L2、cosine。
- SIMD 优化：AVX2 加速 float 距离计算。

## 11.17 面试中怎么讲性能压测与调优

### 1 分钟版本

功能完成后，我没有只停在 testcase PASS，而是补了一套 benchmark。testcase 主要验证正确性，benchmark 验证吞吐、延迟和稳定性。我先做 SET/GET baseline，再逐步打开 Pipeline、TTL、AOF、向量检索和复制，观察每个模块对主链路的影响。性能优化不是只报 QPS，而是通过对比定位瓶颈。比如 AOF always 和 everysec 体现可靠性与吞吐的取舍；flat 和 HNSW 体现精确搜索与近似搜索的取舍；HNSW 的 ef_search 不能只看 QPS，还要看 topK overlap；同步日志会阻塞主链路，所以我做了异步日志队列。最后用 ASan、gdb、微基准和 perf 脚本去验证优化没有引入稳定性问题。

### 3 分钟版本

KVLite 的压测是分层做的。第一层是 testcase，保证 SET/GET/TTL/AOF/RDB/主从/向量这些功能正确。第二层是基础 benchmark，用 `scripts/benchmark/bench_all.sh` 和 redis-benchmark 对齐参数，比较 array、hash、rbtree、skiptable 在 SET/GET 上的吞吐和 P95/P99。这样先建立普通 KV 主链路 baseline。

有了 baseline 后，再看 Pipeline。Pipeline 能减少 RTT 和系统调用开销，但要求 recv_buf 能累积多条命令，RESP peek 能逐条切命令，send_buf 能累积多个响应，而且响应不能乱序。KVLite 里用 `net_session_process_resp` 做批量解析和执行，testcase 里也有大 Pipeline 验证。

数据结构方面，我不是简单说 hash 比 array 快，而是按场景看。hash 适合点查，rbtree/skiptable 适合有序 RANGE/SORT。压测发现 skiptable 仍是弱项，所以我又做了 `skiptable_targeted_bench`，把 insert-heavy、overwrite-heavy、lookup 分开测，定位到写路径和指针遍历成本。

持久化方面，AOF fsync 策略是典型取舍。always 可靠性最高但吞吐最低，everysec 折中，no 性能最好但风险最高。RDB load 恢复快，但 save 要遍历内存；向量数据 load 后还要 rebuild index。

向量方面，我单独做 VSET/VGET/VSEARCH/VDEL benchmark。VSEARCH 是 CPU 密集型，不能和 VGET 用同一个标准看。flat 是精确 baseline，HNSW 是近似搜索，ef_search 要同时看 QPS、P99 和 topK overlap。当前 ef_search=128 相比 256 top10 overlap 约 96.5%，是性能和一致性的平衡点。距离计算热点抽象成 fallback/AVX2 backend，auto 根据 CPU 能力选择。

工程化方面，我还做了内存后端对比、异步日志、ASan/gdb 排障。比如 shutdown 偶发 segfault，最后定位到后台线程和数据结构销毁顺序导致的 use-after-free，修复后用 ASan 和 pipeline 压测回归验证。

### 面试官追问方向

**你怎么证明 hash 比 array 快？**

不能只靠理论复杂度。要固定 keyspace、value size、连接数、请求数，用同一套 SET/GET benchmark 对比。同时说明 hash 适合点查，array 如果没有索引，大数据量会退化。

**Pipeline 为什么能提升吞吐？**

因为它减少了每条命令一次 RTT 的等待，也减少了频繁 read/write 的系统调用开销。服务端可以一次 recv 多条命令，连续解析执行，再批量发送响应。

**AOF always 为什么慢？**

因为每条写命令都要 fsync，fsync 会把 page cache 中的数据同步到磁盘，是昂贵系统调用。它慢是可靠性选择带来的成本。

**everysec 会不会丢数据？**

会有风险。它最多可能丢最近一小段时间尚未 fsync 的数据。它用有限数据丢失风险换吞吐和延迟。

**HNSW 为什么不能只看 QPS？**

因为 HNSW 是近似搜索。QPS 高可能是因为候选集变小，但召回下降。必须同时看 recall@K、topK overlap、P95/P99。

**ef_search 怎么调？**

用较大的 ef_search 或 flat 作为基准，再测试较小 ef_search 的 topK overlap、延迟和吞吐。选择业务可接受的平衡点。

**ASan 帮你发现过什么类型问题？**

适合发现 UAF、double free、越界访问。KVLite 里 shutdown 排障就围绕后台线程访问已释放数据结构这类 UAF 风险做过验证。

**你怎么定位一次性能瓶颈？**

先做端到端 benchmark 发现哪条路径慢，再写 microbench 拆分路径，比如 TTL 拆成 pure lookup、TTL probe、expired delete、TCP sync，最后用 perf 或日志确认热点。

**如何避免压测结果不可信？**

固定参数，预热数据，区分 latency 和 throughput 模式，记录 P95/P99，不只看平均值；每次只改变一个变量，并保留原始数据和脚本。

**如果 QPS 上不去，你会怎么排查？**

先看 CPU 是否打满，再看系统调用、锁、内存分配、网络 send/recv、AOF fsync、日志、复制队列。然后用更小的 microbench 定位具体模块。

## 11.18 如果继续优化，可以怎么做

1. 引入 perf 火焰图定位热点。当前已有 perf 脚本，但还可以形成固定报告，直接展示热点函数占比。

2. 增加 P95/P99 统计覆盖面。不只向量 benchmark，普通 KV、TTL、AOF、复制也应该统一输出尾延迟。

3. 增加 Prometheus metrics。暴露 QPS、延迟、队列长度、AOF buffer、复制延迟、vector index 状态。

4. 增加不同数据量下的系统 benchmark。比如 1K、10K、100K、1M key，观察退化曲线。

5. 增加 AOF fsync 策略对比报告。固定写入压力，对比 no/everysec/always 的 QPS、P99 和数据丢失风险。

6. 增加 RDB save/load 时间统计。记录普通 KV 和向量 record 的 save/load/rebuild 时间。

7. 增加复制延迟和队列长度指标。慢 slave、队列堆积、批量发送失败都应该可观测。

8. 增加 HNSW recall@K benchmark。用 flat 精确结果作为 ground truth，而不只是比较 ef_search 之间的一致率。

9. 接入真实 embedding 后重新压测语义召回。hash embedding 只能验证工程链路，不能代表真实语义质量。

10. 增加 jemalloc vs glibc malloc 对比。用相同长稳压测比较 RSS、碎片、P99 和分配热点。

11. 增加异步日志压测。制造高频日志场景，对比同步日志和异步日志对主链路的影响。

12. 增加长稳压测。比如 1 小时、6 小时、24 小时，观察内存增长、文件增长、队列堆积和尾延迟漂移。

13. 增加故障注入测试。比如 slave 断连、AOF 文件损坏、RDB 恢复失败、ringbuf 事件丢失、磁盘写失败。

14. 增加 lock profiling。对读写锁等待时间做统计，避免只看最终 QPS 而看不到锁竞争。

15. 增加 vector deleted ratio benchmark。测试 VDEL 后 stale index 比例上升时，VSEARCH 和 rebuild 成本如何变化。

## 11.19 本章小结

前面几章回答的是 KVLite 有哪些能力：

```text
网络层
RESP
command_table
多数据结构
TTL
AOF/RDB
主从复制
向量检索
```

第十一章回答的是另一个问题：

```text
这些能力在压力下是否站得住。
```

好的项目复盘不能只讲“我实现了什么”，还要讲：

```text
我怎么验证它是对的
我怎么压测它
我怎么发现瓶颈
我怎么拆分路径
我怎么做取舍
我怎么证明优化有效
```

这也是面试里性能章节最重要的表达方式。不要只报空洞数字，也不要把 QPS 当成唯一目标。更有价值的是讲清楚测试设计、变量控制、瓶颈定位和优化依据。

我最后对这一章的总结是：

> 如果说前面章节回答的是 KVLite 有哪些能力，那么这一章回答的是：这些能力在压力下是否站得住。
