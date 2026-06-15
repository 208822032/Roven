# 第九章 向量语义缓存：从精确 KV 到 HNSW 近似检索

## 9.1 从精确 KV 到语义缓存：为什么 GET key 不够用了

前面几章里，KVLite 的主链路已经比较完整了。

客户端发请求，网络层接收字节流，`net_session` 处理半包、粘包和 Pipeline，RESP 协议层把字节流还原成参数列表，命令层通过 `command_table` 找到 handler，最后落到 array、hash、rbtree、skiptable 这些存储结构里。

到这里，一个传统内存 KV 的核心链路已经跑通了：

```text
SET key value
GET key
DEL key
RANGE / SORT
TTL
AOF / RDB
```

这套能力解决的是一个很明确的问题：**key 完全相等时，返回对应 value。**

比如：

```text
SET name zhengpan
GET name
```

只要 `name` 这个 key 存在，就能返回 `zhengpan`。这就是普通 KV 最擅长的场景。

但后来我把 KVLite 往 AI Chat / RAG / 语义缓存方向扩展时，发现普通 KV 的模型不够用了。

在 AI Chat 里，用户很少每次都问一模一样的问题。

比如第一次问：

```text
Redis 的 AOF 和 RDB 有什么区别？
```

下一次可能问：

```text
Redis 持久化里 AOF 和快照分别解决什么问题？
```

从字符串角度看，这两个问题不是同一个 key。普通 `GET key` 一定命不中。

但从语义角度看，它们问的是同一类问题。如果系统之前已经回答过第一个问题，第二次其实可以复用历史答案，或者至少把历史问答作为上下文召回出来。

这就是普通 KV 和语义缓存的根本区别：

```text
普通 KV 关心的是：key 是否完全相等。
语义缓存关心的是：两个问题是否足够像。
```

最开始我也想过，能不能直接把用户问题做 normalize，比如去空格、转小写、去标点，然后继续用普通 KV。这个方案能解决一小部分"格式变化"的问题，但解决不了"表达方式变化"的问题。

比如：

```text
怎么实现一个 KV 存储？
如何从零写一个 Redis-like 内存数据库？
```

这两个字符串差异很大，但语义上很接近。靠普通字符串 normalize 很难处理。

所以向量语义缓存要解决的不是点查，而是相似度检索：

```text
query text
→ embedding
→ vector similarity search
→ topK 相似历史问题
→ 返回对应 answer / context
```

这也是第九章要复盘的重点：KVLite 怎么从一个精确 key-value 系统，扩展出一套向量检索能力。

这里要注意，我并不是把 KVLite 包装成一个通用向量数据库。当前实现更准确的定位是：**在一个类 Redis KV 引擎里，扩展面向 AI Chat 的轻量语义缓存能力。**

这个定位很重要。面试时如果说"我做了一个向量数据库"，很容易被追问 Milvus、Faiss、PQ、IVF、分布式索引、GPU 检索。当前项目不适合这么包装。

更稳妥的说法是：

> KVLite 原来是一个传统内存 KV，后来我扩展了 V 族命令和 HNSW 索引，用来支持 AI Chat 场景里的相似问答召回。它不是通用向量数据库，而是 KV 引擎上的语义缓存扩展。

## 9.2 最早的 KV 存储模型为什么不适合直接承载向量检索

前面多数据结构那一章里，KVLite 已经支持了四种普通 KV 后端：

```text
array
hash
rbtree
skiptable
```

它们虽然底层结构不同，但解决的问题是一样的：**key 到 value 的映射。**

array 是顺序数组，最早用来跑通最小链路。

hash 适合 O(1) 点查。

rbtree 和 skiptable 支持有序遍历，所以适合 RANGE / SORT 这种按 key 顺序访问的场景。

抽象出来就是：

```text
set(key, value)
get(key)
del(key)
range(start_key, end_key)
sort(...)
```

这些能力都围绕 key 展开。

但向量检索的问题不一样。

向量检索的输入不是一个"我要找的 key"，而是一个 query embedding。系统要在一堆历史 embedding 里找到距离最近的 topK 个。

也就是说，核心问题从：

```text
有没有 key == query？
```

变成了：

```text
哪些 embedding 和 query_embedding 最相似？
```

这两类问题差别很大。

hash 很擅长精确点查，但它的 hash 值只负责把 key 分散到桶里。两个语义相似的问题，hash 值不会天然接近。你不能靠 hash table 找"最近的 10 个向量"。

rbtree 和 skiptable 是按 key 的字典序组织的。它们能回答：

```text
key 在 [a, z] 之间的有哪些？
```

但向量距离不是 key 的字典序。两个向量是否接近，取决于 L2、内积或者 cosine 这些数学距离，而不是字符串排序。

所以如果强行把向量存进普通 value，比如：

```text
HSET question embedding_blob
```

最多只能做到：

```text
根据 question 精确取出 embedding
```

这仍然不是向量搜索。

真正的 VSEARCH 需要的是：

```text
遍历或索引所有 embedding
计算 query 和每条 embedding 的相似度
维护 topK
按 scope / threshold 过滤
返回对应 answer
```

这已经超出了原来 `kvs_operations` 接口的职责。

这也是为什么当前实现没有把向量能力硬塞进 array/hash/rbtree/skiptable，而是单独做了 vector module。

我觉得这里是一个很适合面试表达的设计边界：

> 多数据结构存储层解决的是普通 KV 的存储后端多态；向量模块解决的是另一类检索语义。它不是 key-value 点查的另一个后端，而是新的查询模型。

如果把这两类东西混在一起，代码会很快变得混乱。

比如普通 `GET` 只需要返回一个 value，而 `VSEARCH` 要返回一组带 score 的结果。普通 `DEL` 删除一个 key，而 `VDEL` 还要考虑索引里是否残留 stale entry。普通 RDB 恢复 key-value 后就能查，而向量恢复 record 后还要重建索引。

所以我最后采用的是：

```text
普通 KV：
  command_table -> handle_set/get/del -> array/hash/rbtree/skiptable

向量缓存：
  command_table -> handle_vset/vsearch/vdel -> vector store + vector index
```

两条链路复用网络、RESP、命令分发、持久化入口，但在存储和检索语义上分开。

## 9.3 向量模块的整体结构：record、store、id_map 和 index

向量模块的核心结构可以拆成四层：

```text
record  : 一条语义缓存数据
store   : 管理所有 record 的生命周期
id_map  : question + scope 的精确查找索引
index   : embedding 的相似度检索索引
```

这几个结构各自解决不同问题。

### record：一条语义缓存记录

`kvs_vector_record_t` 代表一条语义缓存记录。

它里面最核心的字段可以理解成：

```text
question
answer
scope
embedding
state / deleted
created_at_ms / updated_at_ms
```

`question` 是用户问题。

`answer` 是缓存的回答。

`scope` 是隔离维度，比如 user_id、session_id、tenant_id、project_id。这个字段很关键，因为语义缓存不能把不同用户或不同知识库的数据混在一起召回。

`embedding` 是 question 对应的向量。当前实现里 embedding provider 是本地 hash provider，固定 64 维。它不是一个真正的大模型 embedding，但对项目来说，它先把"文本转向量、向量入索引、向量搜索"这条工程链路跑通了。

这里面 `deleted` 和 `state` 也很重要。普通 KV 删除一个 key，直接把 entry 释放就可以。但向量索引里可能还有指向这个 record 的索引项，所以 record 需要状态字段来支持"逻辑删除"和后续 rebuild。

### store：record 的生命周期管理

`kvs_vector_store_t` 管理的是所有 record 的数组。

它不仅有 `records` 和 `capacity`，还有：

```text
active_records
deleted_records
next_slot
free_indices
free_count
next_id
```

这说明当前实现不是简单 `malloc` 一条 record，删掉就 `free`。它更像一个小型 record table：

```text
新增时找空槽
删除时放入 free list
后续写入可以复用 deleted slot
每条 record 有稳定递增的 id
```

这个 `record_id` 的设计很有价值。

因为索引里保存的不只是 `record_index`，还会保存 `record_id`。搜索时如果发现索引里保存的 `record_id` 和当前 store 里的 record id 对不上，就说明这个索引项可能是旧数据，要跳过。

这就是防 stale index 的一个关键手段。

### id_map：question + scope 的精确定位

向量模块里不仅有相似搜索，也需要精确查找。

比如：

```text
VGET question scope
```

这个命令不是做 ANN 搜索，而是判断某个 question + scope 是否已经存在。

如果每次 VGET 都扫描所有 record，就太浪费了。所以当前实现里有 `kvs_vector_id_map_t`，它用开放寻址的 hash map 保存：

```text
hash(question + scope) -> record_index
```

注意，这里的 hash 只用于定位候选槽位，真正命中后仍然要比较 question 和 scope。因为 hash 可能冲突，不能只靠 hash 判断相等。

这和普通 hash table 的设计思路是一样的：hash 用来快速缩小范围，最终仍然要比较原始 key。

### index：向量检索索引抽象

最核心的是 `kvs_vector_index_t`。

它不是某一种具体索引，而是一个抽象层：

```text
type
metric
dimension
impl
ops
```

`impl` 指向具体索引实现，比如 flat、HNSW、SQ8 flat。

`ops` 是一组函数指针：

```text
add
search
rebuild
info
destroy
```

这和前面多数据结构那一章里的 `kvs_operations` 很像。

区别在于，`kvs_operations` 抽象的是普通 KV 后端：

```text
array / hash / rbtree / skiptable
```

而这里的 `kvs_vector_index_ops_t` 抽象的是向量索引后端：

```text
flat / hnsw / sq8_flat
```

C 语言没有类和接口，但可以用结构体 + 函数指针手写多态。

这类设计在面试里很好讲：

> 我没有在 VSEARCH 里写一堆 if-else 判断当前索引类型，而是定义了统一的 vector index ops。flat、HNSW、SQ8 都实现 add/search/rebuild 这组接口，当前启用的索引通过 current_index 绑定。这样命令层和业务层不需要关心具体索引实现。

当前实现里有几个索引：

```text
flat_index
ann_index    // HNSW
sq8_flat_index
current_index
```

`flat_index` 是精确搜索 baseline。

`ann_index` 是 HNSW，用来做近似最近邻搜索。

`sq8_flat_index` 是 scalar quantization 的压缩 flat 原型，用 int8 code + scale 降低向量内存占用。

`current_index` 负责把当前配置选择的索引绑定到统一 ops 上。

整体结构可以抽象成：

```text
kvs_vector_state
  ├── store
  ├── id_map
  ├── flat_index
  ├── ann_index
  ├── sq8_flat_index
  └── current_index -> ops -> add/search/rebuild
```

这就是向量模块的主体。

## 9.4 VSET 请求链路：一条语义缓存记录是怎么写进去的

`VSET` 看起来像普通 `SET`，但实际上比普通 `SET` 多做了很多事情。

普通 SET 的语义是：

```text
key -> value
```

VSET 的语义是：

```text
question + scope -> answer
question -> embedding
embedding -> vector index
```

它同时写主数据和索引。

一条 VSET 请求大致链路是：

```text
client
→ RESP command
→ net_session
→ resp_peek_command / resp_parser
→ command_table 查到 VSET
→ parse_command_argument 填充 command_context_t
→ handle_vset_command
→ kvs_vector_set
→ 生成 embedding
→ 写入 vector store
→ 写入 id_map
→ 写入 current_index
→ 返回响应
```

前面的网络层、RESP、命令分发在前几章已经讲过了，这里不展开。关键是进入命令层以后发生了什么。

在 `command_table` 里，`VSET` 被注册成写命令：

```text
VSET -> handle_vset_command -> write_command = 1 -> write lock
```

这意味着它会走和普通写命令一样的横切能力：加写锁、成功后可进入 AOF、主从复制等逻辑。

`parse_command_argument` 会把参数解析成向量命令需要的字段：

```text
VSET question answer [scope]
```

映射到上下文里就是：

```text
ctx->query_text = question
ctx->value      = answer
ctx->scope      = scope
```

到了 `handle_vset_command`，它会调用 `kvs_vector_set`。

`kvs_vector_set` 里面最重要的逻辑有几步。

第一步，检查是否已经存在相同 `question + scope`。

如果存在，当前实现会更新 answer。因为 question 没变，所以 embedding 不需要重新生成，索引也不需要重新插入。

这其实是一个重要取舍。

如果只是更新 answer：

```text
question 不变
embedding 不变
vector position 不变
```

那只改 record 里的 answer 就够了。

但如果未来支持"更新 question"，就不能这么简单了。因为 question 一变，embedding 也会变，旧索引项也要处理。这时候就要么删除旧 record 再插入新 record，要么提供更复杂的 index update 能力。

第二步，如果不存在，就找一个可写 slot。

store 会先尝试从 free list 里复用 deleted slot，如果没有，再扩容 record 数组。

第三步，初始化 record 字段。

这里会复制 question、answer、scope、normalized_question，并生成 embedding。

当前 embedding provider 是本地 hash provider，大致思路是：

```text
固定 64 维
按文本字节 hash 到不同 bucket
做 L1 归一化
```

这个 provider 的意义不是证明语义质量，而是让系统具备完整的向量链路。真实语义效果需要后续接入真正的 embedding 模型。

这一点面试时要说清楚：

> 当前 hash embedding 是工程占位和测试手段，不代表真实语义召回质量。它的价值是让 VSET/VSEARCH/HNSW/RDB 恢复这条链路可以端到端跑起来。

第四步，写入 `id_map`。

这一步让 `VGET question scope` 可以快速定位 record。

第五步，写入向量索引。

如果当前索引不是 SQ8 flat，代码会维护一份 flat index。flat 一方面可以直接用于精确搜索，另一方面也可以作为 baseline。

如果当前启用的是 HNSW 或 SQ8，会通过 `current_index.ops->add` 写入对应索引。

所以 VSET 不是单纯写一条记录，而是同时维护了几份结构：

```text
record store
id_map
flat index
current vector index
```

这也是向量模块比普通 KV 更容易出 bug 的地方。

普通 SET 失败了，大多数时候只是 key/value 没写进去。VSET 如果中途失败，就要考虑前面已经写进去的结构怎么回滚。比如 record 已经写了，id_map 写失败怎么办？index 写失败怎么办？

当前实现里对这些失败路径做了一些清理，比如 index add 失败时移除 id_map、销毁 record，避免留下半成品。

这就是向量写路径的核心难点：**它不是一个单结构写入，而是多结构一致性维护。**

## 9.5 VGET 和 VSEARCH：精确查找与相似召回是两条不同路径

向量模块里，`VGET` 和 `VSEARCH` 很容易被混在一起理解，但它们其实是两条不同路径。

### VGET：精确查找

`VGET` 的输入是：

```text
VGET question [scope]
```

它做的是：

```text
question + scope -> id_map -> record
```

如果能找到完全相同的 question 和 scope，就返回 answer。

所以 VGET 更像普通 GET。

它解决的问题是：

```text
这个问题是否已经被精确缓存过？
```

比如用户真的问了同一句：

```text
KVLite 的 AOF rewrite 怎么实现？
```

那 `VGET` 可以直接命中。

它不计算向量距离，也不走 HNSW。它依赖的是 `id_map`。

### VSEARCH：相似召回

`VSEARCH` 的输入更复杂：

```text
VSEARCH query [scope] [topk] [threshold]
```

当前代码为了兼容旧协议，参数解析稍微宽松一些，但核心字段就是：

```text
query_text
scope
topK
threshold
```

它做的是：

```text
query text
→ embedding
→ current_index.search
→ score 计算
→ topK 选择
→ threshold 过滤
→ scope 过滤
→ 返回 question / answer / scope / score
```

VSEARCH 解决的问题是：

```text
有没有相似问题可以复用答案？
```

这才是语义缓存的核心。

举个例子：

```text
VSET "AOF 和 RDB 有什么区别" "AOF 是日志，RDB 是快照" "user:1"

VSEARCH "Redis 持久化日志和快照怎么选" "user:1" 3 0.5
```

即使 query 和历史 question 字符串不完全相同，只要 embedding 足够接近，就有机会被召回。

这也是为什么向量模块不能只做 `VGET`。如果只有 VGET，它仍然停留在精确缓存；有了 VSEARCH，才真正进入语义缓存。

我在面试里会这样解释：

> VGET 是精确命中路径，类似普通 GET；VSEARCH 是语义召回路径，它会把 query 转成 embedding，然后在向量索引里找 topK 相似历史问题。语义缓存真正提升命中率的是 VSEARCH，而不是 VGET。

当前 `VSEARCH` 返回的是 RESP array，每个结果里包含 question、answer、scope、score 等信息。这样上层 AI Chat 可以决定：

```text
直接复用 answer
把 answer 作为上下文
只用于提示用户
或者低于阈值时忽略
```

这比只返回 value 更适合语义检索。

## 9.6 距离计算：L2、内积、Cosine 和 AVX2 优化

向量搜索首先要回答一个问题：**什么叫相似？**

在普通 KV 里，相等很简单：

```text
strcmp(key1, key2) == 0
```

或者二进制安全一点：

```text
memcmp(key1, key2, len) == 0
```

但向量里没有"完全相等"这个主语义。向量检索关心的是距离或相似度。

KVLite 当前支持三种 metric：

```text
ip
l2
cosine
```

### inner product

内积就是：

```text
score = sum(query[i] * vector[i])
```

在很多 embedding 场景里，如果向量已经归一化，内积可以作为相似度。score 越大，表示越相似。

当前配置里默认使用 `ip`，也就是 inner product。

### L2

L2 距离是欧氏距离：

```text
dist = sum((query[i] - vector[i])^2)
```

原始 L2 是距离越小越相似。

但代码里为了统一 topK 逻辑，把 L2 转成了 score：

```text
score = 1 / (1 + squared_l2)
```

这样仍然保持"score 越大越好"。

这个统一很重要。

如果有的 metric 是越大越好，有的是越小越好，那么 topK heap、threshold 判断、HNSW 比较逻辑都会变复杂，也更容易写错。

所以当前实现选择统一成：

```text
larger score is better
```

这是一个很工程化的设计。

### Cosine

Cosine similarity 是：

```text
dot(a, b) / (||a|| * ||b||)
```

它更关注方向相似，而不是向量长度。很多文本 embedding 场景会用 cosine。

当前实现里 cosine 会计算 dot、lhs norm、rhs norm，再返回归一化相似度。

### AVX2 优化

距离计算是向量搜索里的热点。

如果有 100K 条向量，每条 64 维，一次 flat search 就要做：

```text
100000 * 64
```

次 float 计算。

如果维度更高，比如 384 或 768，成本会更明显。

普通 C 循环是一项一项算：

```text
for i in dim:
    score += lhs[i] * rhs[i]
```

AVX2 的思路是一次处理多个 float。比如 256-bit SIMD 寄存器可以同时处理 8 个 float。这样同样的 dot product，可以减少循环次数，并让 CPU 用向量指令并行算。

KVLite 里 `kvs_vector_distance_init` 会根据配置和 CPU 能力选择 backend：

```text
auto     -> CPU 支持 AVX2 就用 AVX2，否则 fallback
fallback -> 强制普通 C 实现
avx2     -> 尝试 AVX2，不支持则回退
```

这点适合放在面试里作为系统优化点讲。

不是说"我写了 AVX2 所以一定很快"，而是：

> 我识别到向量检索中的距离计算是热点，所以把距离函数抽象成 function pointer，在初始化时根据 CPU 能力选择 scalar 或 AVX2 backend。这样主搜索逻辑不需要关心底层实现，后续也可以继续扩展 SSE、AVX512 或其他后端。

这里和前面的 ops 思想是一致的：把变化点抽象出去。

## 9.7 flat index：最简单但最可靠的 baseline

在做 HNSW 之前，最自然的向量搜索方式是 flat search。

flat index 的思路很简单：

```text
for each vector:
    score = metric(query, vector)
    if score >= threshold:
        push into topK heap
return topK
```

它不做任何剪枝，也不做近似。

只要实现正确，它返回的就是精确 topK。

所以 flat 的优点很明显：

```text
实现简单
结果精确
容易 debug
适合作为 correctness baseline
```

但缺点也很明显：

```text
O(n * dim)
数据量越大，延迟线性增长
```

假设 100K 条数据、64 维，一次搜索要算 100K 次距离。数据变成 100 万，成本基本也会跟着涨 10 倍。

所以 flat 不适合作为大数据量下的最终方案。

但即使有了 HNSW，flat 仍然非常有价值。

第一，它可以作为 HNSW 的正确性基准。

做 ANN 的时候，不能只看 QPS 和延迟，还要看召回率。比如 HNSW 返回的 top10 和 flat 精确 top10 有多少重合。没有 flat，就很难知道 ANN 到底漏了多少。

第二，小数据量下 flat 可能更划算。

如果只有几百条、几千条记录，HNSW 的图维护成本不一定比全量扫描更低。flat 简单、稳定、无构建成本。

第三，debug 时 flat 更可靠。

当 HNSW 结果不符合预期时，可以切到 flat 看看是 embedding 本身问题，还是索引搜索问题。

当前压测文档里也有类似思路：`ef_search` 对比报告说明，`ef_search=128` 相对 `ef_search=256` 的 topK overlap 较高，但也提醒 hash embedding 不代表真实语义质量，真正 recall@K 应该用 flat 精确搜索作为 ground truth。

这就是 flat 的定位：

```text
小数据量可直接使用
大数据量作为 baseline
ANN 召回率评估的参考答案
```

## 9.8 HNSW：为什么用分层图做近似最近邻搜索

flat 的问题是每次都扫全量。

当数据量上去之后，向量检索就会变成 CPU 密集型操作。为了降低搜索延迟，就需要近似最近邻搜索，也就是 ANN。

KVLite 当前默认推荐的是 HNSW。

HNSW 可以用一句话理解：

> 用一个分层图把相似向量连接起来，搜索时从入口点开始沿着更相似的邻居跳转，而不是扫描所有向量。

它解决的是 flat 的全表扫描问题。

### 为什么图结构有用

如果每个向量都连接到一些相似向量，那么搜索时就可以从某个入口点开始，比较当前点和邻居，往更接近 query 的方向走。

这有点像在地图上找目的地：

```text
flat: 每条路都走一遍，最后选最近的
HNSW: 先走高速接近目标区域，再在局部街区细找
```

HNSW 的"高速"就是上层图。

上层图节点少、连接稀疏，适合快速跨越空间。

下层图节点多、连接密集，适合局部精细搜索。

当前源码里的 HNSW 节点结构大致包含：

```text
record_index
record_id
level
neighbors[level][...]
neighbor_counts
neighbor_capacities
active
```

每个节点有自己的最高层级 `level`，并且在每一层维护邻居列表。

索引整体有：

```text
entry_node
max_level
m
ef_construction
ef_search
```

`entry_node` 是搜索入口。

`max_level` 是当前最高层。

`m` 控制每个节点保留多少邻居。

`ef_construction` 控制构建时候选集大小。

`ef_search` 控制搜索时候选集大小。

### 插入过程

插入一个新向量时，HNSW 会先给它分配一个层级。

当前实现里 level 是基于 record id 的确定性 pseudo random 生成，而不是直接依赖 `rand()`。这样 rebuild 后图结构更稳定。

插入时大致流程是：

```text
从 entry_node 开始
在高层做 greedy search，快速接近新节点应该插入的区域
从较低层开始收集候选邻居
选择 top M 个邻居
建立双向连接
必要时更新 entry_node 和 max_level
```

这对应源码里的几个关键动作：

```text
kvs_vector_ann_random_level
kvs_vector_ann_greedy_search_layer
kvs_vector_ann_collect_candidates_layer
kvs_vector_ann_neighbor_add_or_replace
```

### 搜索过程

搜索时也类似：

```text
从 entry_node 开始
先在高层贪心跳转
到第 0 层后，用 ef_search 收集候选集
再从候选里按 score 选 topK
```

这里 `ef_search` 很关键。

`ef_search` 越大，搜索时保留的候选越多，越不容易错过真正近邻，召回率通常越高。但候选越多，距离计算和图遍历越多，延迟也越高。

所以 HNSW 的核心取舍是：

```text
更大的 ef_search -> 更高召回率，更高延迟
更小的 ef_search -> 更低延迟，可能漏召回
```

KVLite 的 `ef_compare_summary.md` 里就体现了这个思路。当前报告用 `ef_search=256` 作为基准，对比 `ef_search=128`，top1 same rate 约 93.2%，top10 overlap rate 约 96.5%。这说明在当前数据和 hash embedding 下，`ef_search=128` 可以作为比较轻量的默认配置。

当然，这个结果不能包装成真实语义召回率。因为当前 embedding 是 hash provider，不是真实语义模型。更严谨的说法是：

> 这个测试比较的是 HNSW 参数变化导致的结果一致率，不代表真实语义质量。真实 recall@K 应该用 flat 精确检索作为 ground truth，并接入真实 embedding 后重新评估。

这个表述很重要，能避免面试官抓住"hash embedding 怎么能语义检索"这个点追着打。

### 为什么 HNSW 适合这个项目

HNSW 适合 KVLite 的原因有几个。

第一，它支持在线插入。

AI Chat 语义缓存是边写边查的，不适合那种必须离线训练后才能查的索引。HNSW 写入后可以马上搜索。

第二，它不要求预先训练。

像 IVF 这类索引通常需要训练 centroid。对一个轻量 KV 项目来说，训练流程会引入更多复杂度。HNSW 更适合先把在线检索能力跑起来。

第三，它有清晰的性能调参点。

`M`、`ef_construction`、`ef_search` 都能清楚解释：

```text
M: 图的连接密度
ef_construction: 构建质量和构建成本
ef_search: 查询召回率和查询延迟
```

这些点在面试里也很好展开。

## 9.9 topK、threshold 和 scope：搜索结果不是越多越好

向量搜索不是把所有相似结果都返回给上层。

语义缓存尤其需要控制结果质量。因为返回一个错误的相似问答，可能比不命中更糟。

当前 VSEARCH 里几个参数很关键：

```text
topK
threshold
scope
```

### topK：最多返回几个

topK 控制返回数量。

比如：

```text
VSEARCH query scope 5
```

表示最多返回 5 条候选。

topK 的作用是控制结果规模。即使有很多相似记录，上层通常也只需要前几个。特别是 AI Chat 场景，如果把几十条历史问答都塞给模型，反而会增加 token 成本，还可能引入噪声。

当前实现里用 topK min heap 维护候选。

小根堆的堆顶是当前 topK 里分数最低的候选。如果新候选比堆顶更好，就替换堆顶；否则丢弃。

这样不需要保存所有候选再排序，内存和计算都更稳定。

### threshold：最低相似度要求

只做 topK 不够。

因为即使所有结果都很差，topK 也会返回"最不差"的几个。

比如一个完全无关的问题，系统里没有任何相似历史记录，但如果只按 topK，它仍然会返回 top1。这对语义缓存来说很危险。

所以需要 threshold：

```text
score < threshold -> 不返回
```

threshold 控制最低相似度要求。

它解决的是：

```text
不要为了凑够 topK 返回低质量结果
```

当前实现里 threshold 小于等于 0 时，会转成一个默认无效阈值，等价于不做严格过滤。实际业务中可以根据 embedding 和 metric 调一个合理阈值。

这里有个容易出错的点：不同 metric 的方向不一样。

L2 原本是越小越相似，IP/Cosine 是越大越相似。当前代码把 L2 也转成"越大越好"的 score，统一 threshold 判断。这是一个减少 bug 的设计。

### scope：业务隔离

scope 是语义缓存里非常重要的字段。

它可以表示：

```text
user_id
session_id
tenant_id
project_id
knowledge_base_id
```

为什么需要 scope？

因为语义相似不代表业务上可以复用。

比如两个用户都问：

```text
我的订单什么时候发货？
```

语义上非常相似，但答案不能跨用户复用。

再比如两个知识库里都有"部署文档"，但一个是 KVLite，一个是另一个项目，也不能混着召回。

所以 VSEARCH 需要 scope 过滤：

```text
只在同一个 scope 内召回相似问答
```

这就是业务约束和算法检索的衔接点。

从源码看，flat search 是遍历时直接检查 scope；HNSW 是先收集候选，再过滤 scope。如果 scope 很窄，就可能出现候选集里大部分都被过滤掉，最后返回数量不足。

当前实现里有 `vector_scope_overfetch_factor` 配置。scope 非空时，可以把 HNSW 的 effective ef_search 放大一些，最多限制到一个上限。这样在 scope 过滤前先拿更多候选，减少过滤后结果不足的问题。

这点也很适合面试：

> topK 是算法层的结果规模控制，threshold 是质量控制，scope 是业务隔离。语义缓存不能只做 ANN，还要把检索结果放回真实业务约束里看。

## 9.10 删除与重建：为什么 VDEL 后索引可能变脏

普通 KV 的删除很直接。

hash 里删一个 entry，rbtree 里删一个 node，skiptable 里摘掉一个节点。删除完成后，这个 key 就不存在了。

但向量索引删除复杂很多，尤其是 HNSW。

HNSW 是图结构。一个节点被删除，不只是删掉这个节点本身，还涉及其他节点的邻居列表。

如果硬删，需要处理：

```text
谁指向了这个节点？
这些邻居要不要重新连接？
图的连通性会不会变差？
entry_node 如果删了怎么办？
max_level 是否变化？
```

这比普通 KV 删除复杂得多。

工程上常见做法不是立刻物理删除，而是逻辑删除：

```text
record 标记 deleted
id_map 移除 question + scope
搜索时跳过 deleted record
index 标记 stale
后续 rebuild 重建干净索引
```

KVLite 当前就是这个方向。

`VDEL` 会找到 record，把它标记为 deleted，并从 id_map 里移除。record index 放入 free list，后续可以复用。flat / sq8 这类索引会标记 needs_rebuild 或 stale。HNSW 搜索时也会通过 record 状态、record_id 等信息跳过已经无效的 record。

这里 `record_id` 很关键。

因为 record slot 可能复用。假设索引里有一条旧 entry 指向 `record_index = 10`，后来这个 record 被删除，slot 10 被新 record 复用了。如果只检查 index，就可能把旧索引项错当成新数据。

所以索引项里保存 `record_id`，搜索时比较：

```text
index_record_id == current_record.id
```

不一致就跳过。

这就是 stale index 防护。

当然，长期只做逻辑删除也有问题。

索引里 stale entry 越来越多，搜索时跳过的无效节点也越来越多，性能和召回质量都会受影响。所以需要 `VINDEX REBUILD`。

重建时的关键点是：不能先把旧索引销毁，再慢慢建新索引。

如果新索引构建到一半失败，系统就没有可用索引了。

当前 `kvs_vector_index_rebuild` 的设计更稳妥：先 compact store，重建 id_map，再构建临时 flat / HNSW / SQ8 索引。只有临时索引全部构建成功，才 swap 到全局状态里。

这个思路可以总结成：

```text
先构建新索引
构建成功再替换旧索引
失败则保留旧索引
```

这也是一个典型的系统设计点：**派生结构重建要避免中间态破坏线上可用性。**

所以这一节的核心结论是：

> 向量删除不是单点删除，而是 record 生命周期和索引一致性问题。

## 9.11 向量持久化：为什么 RDB 恢复后还要重建索引

前一章已经讲过 AOF 和 RDB 的整体设计，这里只讲向量模块和持久化的交界。

向量数据需要持久化，否则服务重启后，语义缓存全部丢失。

当前实现里，VSET / VDEL 属于写命令。命令层里它们的 `write_command` 字段为 1，所以成功执行后可以进入 AOF 追加路径。

这意味着 AOF 可以通过重放 VSET / VDEL 恢复向量数据。这里也要注意一个当前边界：命令层向量 handler 主要面向文本问题和回答，普通 KV 的二进制安全能力不能直接等价外推到所有 V 族字段。

RDB 侧，当前实现有单独的 vector section。它保存的重点是 record 数据，而不是直接把整个 HNSW 图完整写入文件。

向量 record 里会保存：

```text
record_index
state
deleted
record_id
question
answer
scope
normalized_question
embedding_dim
embedding data
created_at_ms
updated_at_ms
```

恢复时，RDB 会调用向量模块的 restore 函数，把 record 恢复到 vector store 里，然后调用 rebuild 相关逻辑恢复 id_map 和索引。

这里有一个重要设计取舍：

```text
record 是真实数据
index 是派生结构
```

真实数据必须保存。派生结构可以重建。

HNSW 图当然也可以持久化，但那会带来更多问题：

```text
文件格式更复杂
版本兼容更难
record 和 index 一致性更难校验
删除后 stale 状态更难处理
不同参数下图结构是否复用也要考虑
```

对 KVLite 当前阶段来说，保存 record + embedding，再在恢复后 rebuild index，是更稳的选择。

它的缺点是恢复后需要花时间重建索引。

但优点是：

```text
文件格式简单
恢复逻辑清晰
不会把脏索引持久化
索引参数变化后也能重建
```

这和普通 KV 的 RDB 有区别。

普通 KV 恢复 key-value 后，GET 立刻可用。

向量模块恢复 record 后，VGET 依赖 id_map，VSEARCH 依赖 vector index，所以还要恢复或重建可搜索能力。

可以这样理解：

```text
普通 KV：
  RDB load -> key/value in memory -> GET ready

向量模块：
  RDB load -> vector records in memory
           -> rebuild id_map
           -> rebuild flat/HNSW/SQ8 index
           -> VGET / VSEARCH ready
```

这也是向量模块比普通 KV 多的一层复杂度。

面试里可以说：

> 我把 record 看作主数据，把 HNSW 看作派生索引。RDB 持久化主数据和 embedding，恢复后重建索引。这样牺牲一部分恢复时间，换来文件格式简单和一致性风险更低。

## 9.12 这个模块里最容易出 bug 的地方

向量模块看起来只是多了几个 V 命令，但真实工程风险不少。

### 1. embedding 维度不一致

索引初始化时有 dimension，record 也有 embedding_dim。如果写入 64 维，搜索却传进来 384 维，就不能直接算距离。当前代码在 add/search 时都会检查维度，不一致返回 invalid arg。

这个问题在未来接入真实 embedding provider 时更容易出现，因为不同模型维度可能不同。

### 2. question / answer / scope 字符串内存所有权不清楚

record 里保存的是字符串副本，不应该直接引用 RESP parser 的 token 内存。因为 parser reset 后 token 会被释放。

所以 VSET 时必须复制 question、answer、scope。销毁 record 时也必须释放这些字段。

只要所有权不清楚，就容易出现 use-after-free 或 double free。

### 3. 覆盖更新时旧 record 没释放干净

如果 question + scope 已存在，当前实现更新 answer。这里要释放旧 answer，再复制新 answer，同时不能误删 question、scope、embedding。

如果未来支持更新 question，就还要处理旧 embedding 和旧 index entry，这比更新 answer 复杂得多。

### 4. VDEL 后索引残留 stale entry

删除 record 后，flat/HNSW/SQ8 里可能还有旧索引项。不能假设 `VDEL` 后索引立刻干净。

所以搜索时必须检查 record 状态和 record_id，必要时通过 rebuild 清理。

### 5. HNSW 搜索结果引用 deleted record

HNSW 节点里保存 record_index。如果 record 被逻辑删除，搜索时还沿图走到了这个节点，就必须跳过。

当前实现里通过 record state、deleted、record_id 等判断避免返回 deleted record。

### 6. scope 过滤导致返回结果不足

HNSW 先按向量相似度找候选，再按 scope 过滤。如果 scope 很窄，候选可能大量被过滤掉，最终返回数量少于 topK。

所以需要 overfetch 或者后处理策略。当前有 `vector_scope_overfetch_factor` 这个配置点。

### 7. RDB 恢复后忘记 rebuild index

如果只恢复 records，不重建 id_map 和 index，VGET/VSEARCH 都可能异常。尤其 VSEARCH 依赖 current_index，如果没有 bind/rebuild，索引状态会不完整。

### 8. topK heap 边界错误

topK heap 容量、空堆、score 相等、record_index tie-break，都容易写错。

如果堆顶比较方向错了，就会把最差结果留下，把最好结果丢掉。

### 9. threshold 方向搞反

L2 是距离越小越好，IP/Cosine 是分数越大越好。如果不统一 score 方向，很容易把 threshold 判断写反。

当前实现把 L2 转成 `1 / (1 + dist)`，统一成 score 越大越好，是为了降低这个风险。

### 10. SIMD 优化路径和普通路径结果不一致

AVX2 路径和 fallback 路径必须保持语义一致。否则同一个 query 在不同机器上可能返回不同结果。

这类问题通常要用单元测试对比 scalar 和 SIMD 的误差范围，而不能只看性能。

### 11. 当前 V 族文本边界和普通 KV binary-safe 边界不同

普通 RESP parser 已经维护了 `token_lens`，普通 KV 的 `_len` 接口可以支持二进制 key/value。

但向量语义缓存本身更偏文本场景，当前 VSET 的 answer 路径里仍然有使用字符串长度的地方。这个在 AI Chat 文本缓存场景一般够用，但如果未来要支持任意二进制 answer，就要继续补齐 length-safe 处理。

这也是面试里可以主动说明的边界：不是所有模块都已经达到同一等级的 binary-safe。

## 9.13 可以关联哪些八股知识点

### C 语言八股

**struct 设计。**  
向量模块把 record、store、id_map、index 拆开，每个结构体负责不同职责。可以讲结构体如何表达模块边界。

**指针和内存所有权。**  
question、answer、scope、embedding 都是动态分配，record 拥有这些内存，destroy 时统一释放。

**malloc/free 生命周期。**  
VSET 初始化失败时要释放中间资源，VDEL 不一定物理释放 record，而是标记 deleted 和复用 slot。

**函数指针实现多态。**  
`kvs_vector_index_ops_t` 用 add/search/rebuild/destroy 抽象 flat、HNSW、SQ8。这是 C 语言手写接口的典型例子。

**void* 抽象。**  
`kvs_vector_index_t` 里用 `void *impl` 指向具体索引，实现统一外壳 + 具体实现。

**memcpy / memcmp / strlen 的边界。**  
文本复制、embedding 数组复制、question/scope 精确比较都涉及长度边界。这里也可以顺带讲为什么 token_lens 很重要。

**二进制安全。**  
普通 KV 已经做了 `_len` 接口。向量模块当前主要面向文本语义缓存，未来如果 answer 支持二进制，也需要完全使用 len 而不是 strlen。

**topK heap 实现。**  
用小根堆维护当前 topK，堆顶是最差候选，新候选更好才替换。

**资源释放顺序。**  
vector destroy 时要销毁 embedding provider、索引、id_map、store；record destroy 时要释放内部字段再 reset。

### Linux / 系统八股

**SIMD / AVX2。**  
距离计算是热点，AVX2 一次处理多个 float，适合 dot/L2/cosine 这类循环计算。

**CPU cache。**  
flat index 连续存储向量，遍历时 cache locality 比分散节点更好；HNSW 图搜索会有更多随机访问。

**内存分配器。**  
向量模块大量分配 record 字段、embedding、neighbors、heap，可以结合项目里的 pool/system/jemalloc 后端讲内存管理。

**多线程读写锁。**  
VSET/VDEL 走写锁，VGET/VSEARCH 走读锁。向量索引修改和搜索不能无保护并发访问。

**后台 rebuild。**  
当前有显式 VINDEX REBUILD，未来可以做后台异步 rebuild。这里涉及旧索引可用、新索引构建、最终 swap。

**文件持久化和恢复。**  
record 是主数据，index 是派生结构。RDB 保存 record，恢复后 rebuild index。

### 后端 / 存储系统八股

**索引和数据分离。**  
store 保存真实数据，index 只是加速查询的派生结构。

**主数据和派生索引。**  
派生索引可以删除重建，主数据不能丢。

**rebuild。**  
删除多了之后，索引变脏，需要从 active records 重建干净索引。

**一致性。**  
VSET 同时更新 store、id_map、index，中途失败要避免半成品。

**缓存命中率。**  
普通 KV 是 exact hit，语义缓存增加 semantic hit，提高相似问题复用概率。

**召回率和延迟权衡。**  
HNSW 的 ef_search 越大，召回通常越好，延迟也更高。

**topK 查询。**  
返回结果数量受 topK 控制，不是全量返回。

**多租户 / scope 隔离。**  
scope 防止不同用户、会话、知识库之间互相污染召回结果。

### 算法八股

**向量距离。**  
L2、inner product、cosine 都是衡量向量相似度的方法。

**cosine similarity。**  
关注方向相似，适合很多文本 embedding 场景。

**inner product。**  
当前默认 metric，score 越大越相似。

**L2。**  
距离越小越相似，但当前实现转成 score 越大越好，统一 topK 逻辑。

**HNSW。**  
分层小世界图，通过上层快速导航、下层精细搜索降低全量扫描成本。

**ANN。**  
近似最近邻，用可接受的召回损失换更低延迟。

**heap。**  
小根堆维护 topK。

**greedy search。**  
HNSW 高层搜索通过贪心选择更接近 query 的邻居。

**recall@K。**  
评估 ANN 结果和 flat 精确结果的重合程度。

## 9.14 面试中怎么讲这个模块

### 1 分钟版本

可以这样说：

> KVLite 前面已经实现了传统 KV 的 SET/GET/TTL/AOF/RDB，但普通 KV 只能做 key 完全匹配，不适合 AI Chat 里"相似问题复用答案"的场景。所以我扩展了一个向量语义缓存模块，提供 VSET、VGET、VSEARCH、VDEL、VINFO、VINDEX REBUILD 这组命令。
>
> VSET 会把 question、answer、scope 存成一条 vector record，并为 question 生成 embedding，再写入向量索引。VGET 走 question+scope 的精确 id_map，VSEARCH 则把 query 转成 embedding，在 flat 或 HNSW 索引里做 topK 相似搜索，再按 threshold 和 scope 过滤结果。
>
> 我把 record store 和 vector index 分开设计，record 是主数据，HNSW 是派生索引。删除时先标记 deleted，搜索时跳过 stale record，后续通过 rebuild 重新构建干净索引。持久化上，RDB 保存 record 和 embedding，恢复后重建索引，而不是直接依赖旧图结构。这个模块主要体现了索引抽象、ANN 检索、内存生命周期和派生索引一致性这些系统设计能力。

### 3 分钟版本

如果面试官愿意听展开版，可以这样讲：

> 这个模块的背景是，传统 KV 的 GET 只能回答 key 是否完全相等。但 AI Chat 语义缓存里，用户经常换一种问法，字符串不一样但语义一样，所以 exact key 命中率会很低。我想让 KVLite 不只是缓存完全相同的问题，而是能召回相似历史问答。
>
> 一开始我也考虑过把 embedding 当成普通 value 存到 hash 或 skiptable 里，但后来发现这解决不了 VSEARCH。hash 适合 O(1) 点查，rbtree/skiptable 是按 key 排序，而向量搜索是在高维空间里按距离找 topK。这和普通 KV 是不同检索语义，所以我单独做了 vector store 和 vector index。
>
> 当前一条向量记录用 `kvs_vector_record_t` 表示，里面有 question、answer、scope、embedding、deleted 状态和时间戳。`kvs_vector_store_t` 管理 record 数组、容量和 free list。`kvs_vector_id_map_t` 用 question+scope 做精确查找，服务 VGET。真正的相似搜索交给 `kvs_vector_index_t`，它用函数指针抽象 flat、HNSW、SQ8 flat。这样命令层只调用 current_index 的 search，不关心底层索引。
>
> VSET 的链路是：RESP 解析命令后进入 command_table，找到 `handle_vset_command`，再调用 `kvs_vector_set`。它会检查 question+scope 是否已存在；如果存在就更新 answer；如果不存在就分配 record，生成 embedding，写 id_map，再写当前索引。
>
> VSEARCH 的链路是：query 先生成 embedding，然后进入当前索引的 search。flat 会遍历所有向量，计算 score，用 topK heap 保留最好的结果；HNSW 则用分层图做近似搜索，从入口点开始在高层快速接近目标区域，再在第 0 层用 ef_search 收集候选。最后再按 threshold 和 scope 过滤。
>
> HNSW 的优势是不用全量扫描，适合数据量上来后的低延迟搜索。它的核心参数是 M、ef_construction、ef_search。M 控制图连接密度，ef_construction 影响构建质量，ef_search 控制查询候选集大小。ef 越大召回通常越高，但延迟也越高。
>
> 删除是这个模块里比较容易出问题的地方。普通 KV 删除 key 就结束了，但 HNSW 是图结构，硬删一个节点会影响邻居关系。所以当前实现采用逻辑删除：record 标记 deleted，id_map 移除，搜索时通过 deleted 状态和 record_id 跳过 stale index entry，后续用 VINDEX REBUILD 从 active records 重建干净索引。
>
> 持久化上，我把 record 作为主数据，把 HNSW 看成派生索引。RDB 保存 question、answer、scope、embedding、record_id 等数据，恢复后重建 id_map 和向量索引。这样文件格式简单，也避免把可能变脏的图结构持久化下来。
>
> 这个模块能体现的能力包括：C 语言结构体和函数指针抽象、内存生命周期管理、topK heap、SIMD 距离计算、HNSW ANN、索引和主数据分离、持久化恢复后的派生索引 rebuild。

### 面试官追问方向

**追问：为什么不用普通 hash 存 embedding？**

回答方向：

> hash 只能根据 key 精确查 record，不能按向量距离找 topK。embedding 存进 hash 只是解决了"能不能取出来"，没有解决"怎么找相似"。所以我保留 id_map 做精确查找，同时单独做 vector index 做相似搜索。

**追问：为什么需要 HNSW？flat 不行吗？**

回答方向：

> flat 是精确的，但复杂度是 O(n * dim)，数据量大后延迟线性增长。HNSW 用分层图减少搜索范围，用可控的召回损失换低延迟。flat 仍然保留，用作小数据量搜索和 recall baseline。

**追问：HNSW 的 ef_search 怎么调？**

回答方向：

> ef_search 控制搜索候选集大小。越大召回率通常越高，但距离计算和图遍历更多，延迟也更高。我会用 flat 作为 ground truth，看不同 ef_search 下 recall@K、P95、P99 的变化，再选业务可接受的点。

**追问：删除为什么不直接从 HNSW 图里删？**

回答方向：

> 因为 HNSW 是图结构，一个节点被删除会影响邻居连接、entry_node、图连通性。直接维护删除复杂且容易破坏索引质量。当前项目采用逻辑删除 + 查询跳过 + 定期 rebuild，这样实现更稳，也符合很多索引系统处理派生索引的思路。

**追问：RDB 为什么不直接保存 HNSW 图？**

回答方向：

> HNSW 图是派生结构，record 和 embedding 才是主数据。保存图会增加文件格式复杂度和一致性风险。当前选择保存 record，恢复后 rebuild index，牺牲一部分恢复时间，换来更简单可靠的持久化格式。

**追问：当前 hash embedding 能代表真实语义吗？**

回答方向：

> 不能。当前 hash embedding 是为了把工程链路打通：VSET 生成向量、写索引、VSEARCH 检索、RDB 恢复后 rebuild。真实语义效果需要接入外部 embedding 模型，并用 flat ground truth 评估 recall@K。

## 9.15 如果继续优化，可以怎么做

这个模块后续还能继续往几个方向优化。

### 1. 接入真实 embedding provider

当前 hash embedding 只能验证工程链路，不代表真实语义质量。后续可以接入本地 embedding 模型或远程 embedding 服务。

接入后要重点处理：

```text
模型维度配置
超时和失败重试
批量 embedding
embedding 版本
旧数据是否需要重新向量化
```

### 2. 用 flat 结果评估 HNSW recall@K

HNSW 是近似搜索，不能只看延迟。

需要用 flat 精确 topK 作为 ground truth，统计：

```text
recall@1
recall@3
recall@10
topK overlap
rank shift
```

这样才能知道不同 HNSW 参数的真实效果。

### 3. 调整 M / ef_search / ef_construction

当前配置里 HNSW 有 M、ef_construction、ef_search。后续可以系统压测这些参数：

```text
M 增大：图更密，召回可能更好，内存更高
ef_construction 增大：构建更慢，图质量可能更好
ef_search 增大：查询更慢，召回可能更好
```

最终应该给出一组推荐配置，而不是拍脑袋选默认值。

### 4. 支持后台异步 rebuild

当前有 `VINDEX REBUILD` 显式重建。后续可以做后台异步 rebuild：

```text
删除比例超过阈值
或 stale entry 超过阈值
后台构建新索引
构建完成后原子 swap
```

这里要注意 rebuild 期间的写入增量怎么处理，否则新索引可能漏数据。

### 5. 支持向量 TTL

普通 KV 已经有 TTL，但 V 族记录当前还没有完整 TTL 语义。

如果语义缓存用于 AI Chat，很多问答其实应该过期。

可以扩展：

```text
VEXPIRE
VTTL
VPERSIST
或者 VSET 支持 EX 参数
```

同时要考虑：

```text
VSEARCH 跳过过期 record
RDB 保存向量 TTL
AOF 记录绝对过期时间
rebuild 时忽略过期记录
```

### 6. 支持批量 VSEARCH

当前压测工具已经支持多连接和 pipeline，但命令层可以进一步支持批量 VSEARCH。

批量搜索的收益在于：

```text
批量生成 embedding
批量执行距离计算
减少网络往返
更适合 AI Chat 多候选召回
```

不过批量接口的 RESP 返回格式要设计好，否则客户端解析会复杂。当前源码里可以看到批量 VSEARCH 曾经做过又移除，这也说明接口稳定性和复杂度需要继续权衡。

### 7. 支持索引文件单独持久化

当前 RDB 保存 record，恢复后 rebuild index。数据量很大时，rebuild 时间可能变长。

后续可以选择保存索引文件：

```text
vector.rdb      保存 record
vector.idx      保存 HNSW 图
```

但要解决版本兼容、校验、record_id 对齐、参数变化等问题。

### 8. 支持 PQ / IVF / 更复杂量化

当前有 SQ8 flat 原型。后续可以继续做：

```text
PQ
IVF
IVF-PQ
HNSW + quantization
```

这些能降低内存或提升速度，但实现复杂度会明显上升。对面试项目来说，要谨慎推进，不要为了堆名词牺牲稳定性。

### 9. 支持多 scope 分片索引

当前 scope 是搜索后过滤或 overfetch。数据量大、scope 很多时，可以考虑：

```text
每个 scope 一个小索引
或者 scope 分片索引
```

这样可以减少无关候选，提高搜索质量。

但缺点是索引数量变多，内存管理和 rebuild 复杂度也会上升。

### 10. 支持更完整 metrics

向量模块应该暴露更多指标：

```text
VSET QPS
VSEARCH QPS
P50 / P95 / P99
active_records
deleted_records
stale ratio
index memory
vector memory
rebuild time
recall@K
```

当前 VINFO 已经有一些统计字段，比如 record 数、index type、HNSW 参数、memory bytes。后续可以继续扩展成更完整的观测体系。

### 11. 支持更完整 benchmark

当前已有 `vector_rw_summary.md` 和 `ef_compare_summary.md`，能看到 VSET/VGET/VSEARCH/VDEL 的 latency / throughput，也能比较 ef_search 参数。

后续 benchmark 可以更完整：

```text
不同数据量：10K / 100K / 1M
不同维度：64 / 384 / 768
不同 topK：1 / 5 / 10 / 50
不同 scope 分布
不同 threshold
flat vs HNSW recall@K
AVX2 vs fallback
SQ8 内存和精度对比
```

这样文章和面试表达会更有说服力。

---

这一章最大的收获是，向量语义缓存不是"把 embedding 存进 value"这么简单。

普通 KV 的核心是：

```text
key 是否相等
```

向量语义缓存的核心是：

```text
embedding 是否相似
```

这会带来一整套新的系统问题：

```text
record 怎么组织
精确查找和相似搜索怎么分开
索引怎么抽象
删除后 stale index 怎么处理
RDB 恢复后索引怎么重建
topK / threshold / scope 怎么和业务结合
```

所以这篇在面试里可以作为一个很好的扩展能力来讲：

> 前面七章证明了我能从零实现一个类 Redis KV 的主链路；第九章证明的是，我能在已有系统上识别新的业务检索语义，并通过新的数据模型、索引抽象和恢复机制，把系统从传统 KV 扩展到 AI 语义缓存。
