# 第六章 多数据结构存储层：为什么 array、hash、rbtree、skiptable 需要统一接口

## 6.1 从最早的 array 说起：先让数据真的存下来

KVLite 的第一条 SET 请求，最终是落到一个叫做 `kvs_array_t` 的结构体里的。它的设计极其朴素——一块 `KVS_ARRAY_SIZE` 大小（最初 1024，后来扩到 2048）的 `kvs_array_item_t` 数组，每个 item 存一个 `char *key` 和 `char *value`。SET 操作就是线性扫描找空槽位，找到了就 `malloc` + `strncpy` 存进去。GET 操作是线性扫描找匹配的 key，找到了就返回 value。DELETE 就是找到那个槽位，`free` 掉 key 和 value，槽位置空。

如果从"数据结构与算法"的角度去看这段代码，它几乎"全是问题"：查找 O(n)、定长容量、删除产生碎片、没有排序、RANGE 需要全表扫描。但回到 2024 年 5 月的那个时刻，array 是最正确的选择。

最早我选择 array，不是因为它性能最好，而是因为它最容易验证主链路。当时 KVLite 连一条从网络到存储的闭环都没跑通过——网络代码刚写完、命令解析还是 echo、数据结构这块完全是空白。用 array 的好处是：我可以在半小时内写完一个能用的 key-value store，然后立刻接上网络层和命令层去验证"链路通不通"。如果一开始就去做哈希表或红黑树，我会花几天时间在各种数据结构 bug 上，而那条主链路——"客户端发命令 → 服务端解析 → 存到某个地方 → 返回响应"——始终没有跑通过。

现在回头看，array 的意义不是性能，而是验证从命令层到存储层的第一条落点。它让 KVLite 具备了"数据真的能存下来"这个最基本的语义，也暴露出了后面要解决的第一个问题：**线性扫描在大数据量下会变成瓶颈。**

## 6.2 为什么后续加入 rbtree、hash 和 skiptable

在 array 跑通之后的三天里（2024-05-23 到 2024-05-25），rbtree、hash、skiptable 三个数据结构被接连加入。这个速度说明它们不是"深思熟虑后选择的四个存储后端"，而是"把常见的几种内存存储结构都实现一遍，然后对比它们在同一个 KV 命令模型下的表现"。

从 KV 存储的角度来看，四种结构各有关键差异：

**array**：最简单的线性结构。查找 O(n)，插入 O(n)（需要扫描避让已占用的槽），删除 O(n)（需要移动元素或留空洞）。优势是实现简单、空间连续、cache 友好。适合小数据量快速原型。

**hash**：拉链法哈希表。平均查找 O(1)，插入 O(1)，删除 O(1)。但数据没有顺序，RANGE/SORT 需要把所有有效槽拷贝出来再排序。最纯粹的"点查最优"结构。

**rbtree**：红黑平衡二叉搜索树。查找 O(log n)，插入 O(log n)，删除 O(log n)。数据始终按 key 字母序排列，天然支持 RANGE（中序遍历一个区间）和 SORT（中序遍历整个树）。代价是旋转和颜色维护的常数开销。

**skiptable**：概率多层链表。期望查找 O(log n)，插入 O(log n)，删除 O(log n)。和 rbtree 一样是有序结构，但不需要旋转——插入时通过随机数决定新节点的层数，代码比红黑树简单不少。代价是额外的指针存储和随机数生成。

这里要澄清一件事：**KVLite 加入四种数据结构，不是为了"堆数据结构名词"。** 如果只是做一个能用的 KV 存储，选一个结构（比如 hash）就够了。但同时保留四种，是因为我想在同一个系统中比较它们：

- 在纯 SET/GET 场景下，hash 和 array 的表现有何差异？
- 在 RANGE/SORT 场景下，rbtree 和 skiptable 哪个更快？
- 相同功能的代码，红黑树（~550 行）和跳表（~140 行）的维护成本差多少？
- 不同数据结构的缓存局部性对端到端性能的影响有多大？

这个设计思路在后面的性能优化迭代中得到了充分验证——当我的 benchmark 可以统一对四种结构跑同一组 SET/GET 命令时，性能瓶颈一目了然：array 的线性扫描在 1000 条数据时就明显变慢，hash 在 100 万条时仍然接近 O(1)，rbtree 和 skiptable 在 RANGE/SORT 场景下完胜其余两个。

## 6.3 早期命令层直接调用具体数据结构的问题

在早期的 `kvs_filter_protocol` 中，命令分发和存储调用是硬编码在一起的。一条 `HSET` 和一条 `SSET` 的区别仅仅在于 switch-case 跳到的代码行：

```c
case KVS_CMD_HSET:
    ret = kvs_hash_set(&global_hash, key, value);
    break;
case KVS_CMD_SSET:
    ret = kvs_skiptable_set(&global_skiptable, key, value);
    break;
```

每个 case 里直接写了全局变量名（`global_hash`、`global_skiptable`）和具体函数名（`kvs_hash_set`、`kvs_skiptable_set`）。这种写法在早期有几个优势：调用链短、一眼能看到数据写到哪了、出了问题 `gdb` 直接断在具体函数上。但当项目逐渐变大，问题就开始浮现：

**命令层和存储层强耦合。** 如果新增一个数据结构（比如 BTree），需要在 switch-case 里为每条命令（SET/GET/DEL/MOD/EXIST/RANGE/SORT）各加一个 case 分支——总计 7 个分支。四种数据结构就是 28 个分支，五种就是 35 个。

**同一类命令的逻辑大量重复。** SET 操作不管落到 array 还是 hash，命令层的处理逻辑是一样的：拿到 key 和 value，调存储函数，根据返回值生成 OK/EXIST/ERROR 响应。但 switch-case 把这套逻辑在 4 个分支里各写了一遍，只是调用的存储函数不同。

**RANGE/SORT 的适配需要重复代码。** 不同数据结构如何做范围查询？array 需要全表扫描 + 过滤 + qsort（O(n log n)），rbtree 是中序遍历（O(k + log n)，k 为结果数），skiptable 是逐层前进收集节点。这些实现差异如果不能在命令层被屏蔽，RANGE 的处理代码就需要针对每种结构单独写。

**TTL、AOF/RDB、复制等横切功能难以统一接入。** 后面要加的 TTL 过期需要修改存储 entry 中的 `expire_at_ms` 字段，AOF 重写需要遍历所有数据结构中的活跃数据，复制 slave 端需要把 master 发来的命令应用到本地存储——这些操作如果每种结构调用方式不同，就需要写多份适配代码。

**benchmark 难以做到统一口径。** 如果四种结构的命令名不同（SET/RSET/HSET/SSET），压测脚本就要维护四套命令模板，而且不能直接在同一条命令上比较四种结构的性能差异。

commit `b805a36`（2025-09-05）通过把业务逻辑从 `kvstore.c` 拆分到 `kvs_operation.c` 缓解了部分问题——命令分发变成了 `command_table` 查找 + `cmd->handler` 调用。但 handler 内部的存储调度仍然是通过 `switch(ctx->data_structure)` 硬编码的，存储调用的耦合问题并没有完全解决。

## 6.4 统一 ops 接口：C 语言里的存储层多态

在 commit `b2e3ba3`（2025-11-20）中，头文件重组时引入了 `kvs_operations` 结构体（定义在 `include/kvstore.h:328-341` 和独立的 `include/kvs_operations.h`）：

```c
typedef struct {
    int   (*set)(void*, char*, char*);
    char* (*get)(void*, char*);
    int   (*del)(void*, char*);
    int   (*mod)(void*, char*, char*);
    int   (*exist)(void*, char*);
    int   (*sort)(void*, int, int, char*, int);
    int   (*range)(void*, char*, char*, char*, int);
} kvs_operations;
```

以及四个 ops 实例：

```c
static kvs_operations array_ops = {
    .set   = (int (*)(void*, char*, char*))kvs_array_set,
    .get   = (char* (*)(void*, char*))kvs_array_get,
    .del   = (int (*)(void*, char*))kvs_array_del,
    .mod   = (int (*)(void*, char*, char*))kvs_array_mod,
    .exist = (int (*)(void*, char*))kvs_array_exist,
    .sort  = (int (*)(void*, int, int, char*, int))kvs_array_sort,
    .range = (int (*)(void*, char*, char*, char*, int))kvs_array_range
};
// rbtree_ops, hash_ops, skiplist_ops 结构相同，绑定各自的具体函数
```

这个结构就是 C 语言里手写的"虚函数表"。如果你熟悉 C++，可以把它理解为：`kvs_operations` 是抽象基类，`array_ops`/`rbtree_ops`/`hash_ops`/`skiplist_ops` 是四个具体子类的 vtable。`void*` 的第一个参数就是 `this` 指针——运行时传进去的是 `&global_array` 或 `&global_rbtree`，具体函数内部再通过类型转换拿回自己的类型。

这个设计的直接收益是：命令层只需要面对 `kvs_operations*`，不需要知道具体是哪个数据结构。调用变成了一行：

```c
kvs_operations *ops = get_ops_by_type(ctx->data_structure);
ret = ops->set(instance, key, value);
```

加一个新的存储后端（比如 BTree），只需要：实现七个函数 → 填一个 `btree_ops` 表 → 在 `get_ops_by_type` 里加一个 case。命令层的 handler 函数一行都不用改。

但这里必须如实地讲一个现状：**`kvs_operations` 表在当前代码中已经定义好了，但 KV 命令层（`handle_set`、`handle_get` 等）实际上并没有使用它。** 当前的 `handle_set` 仍然是这样写的（`src/core/kvs_operation.c:1089-1112`）：

```c
switch(ctx->data_structure) {
#if ENABLE_ARRAY
    case DATA_STRUCTURE_ARRAY:
        ret = kvs_array_set_len(kvs_get_global_array(), ctx->key, ctx->key_len, ctx->value, ctx->value_len);
        break;
#endif
#if ENABLE_RBTREE
    case DATA_STRUCTURE_RBTREE:
        ret = kvs_rbtree_set_len(...);
        break;
#endif
    // ... hash, skiptable ...
}
```

ops 表定义了但没被用——这是一个"设计方向已经指明，但迁移还没做完"的状态。相比之下，vector 索引模块已经完整地使用了 ops 模式（`kvs_vector_index_ops_t`，定义在 `include/kvs_vector.h:390-416`），`index->ops->add()`、`index->ops->search()`、`index->ops->rebuild()` 是 flat/HNSW/SQ8 Flat 三种索引的统一入口。这说明 ops 模式在项目中是经过验证的，只是 KV 命令层还没有迁移过来。

**为什么还没有迁移？** 主要有两个原因。第一，`kvs_operations` 的函数签名仍然使用 `char*` 而不是 `const char*` + `size_t`，这意味着它还没有适配 binary-safe 的 `_len` 接口——如果现在就改用 ops，反而会丢失 `token_lens` 带来的长度信息。第二，当前的 handler 内部 switch 结构是"能用且改动风险低"的状态，在没有明确的性能或功能需求驱动时，迁移的优先级排在 TTL、AOF、向量检索等更紧迫的工作之后。

## 6.5 从命令 handler 到 storage ops 的当前调用链

跳过 ops 表还没完全启用的现状，当前实际的调用链是这样的：

```text
kvs_filter_protocol (kvstore.c:49)
  │ lookup_command → cmd_info*
  │ check_arity → 参数个数校验
  │ parse_command_argument → 填充 command_context_t
  │    ctx.key="key", ctx.key_len=3
  │    ctx.value="value", ctx.value_len=5
  │    ctx.data_structure = DATA_STRUCTURE_ARRAY  (来自 command_table)
  │ 加读写锁
  ▼
cmd->handler(&ctx, response)
  │
  ├─ handle_set (kvs_operation.c:1078)
  │    switch(ctx->data_structure) {
  │      case DATA_STRUCTURE_ARRAY:
  │        ret = kvs_array_set_len(&global_array,
  │              ctx->key, ctx->key_len,
  │              ctx->value, ctx->value_len);
  │        break;
  │      case DATA_STRUCTURE_RBTREE:
  │        ret = kvs_rbtree_set_len(&global_rbtree, ...);
  │        break;
  │      // ... hash, skiptable
  │    }
  │    → 根据 ret 生成 RESP 简单字符串/错误
  │
  ├─ handle_get (kvs_operation.c:1124)
  │    → ttl_expire_if_needed_by_structure (TTL 惰性删除)
  │    → 根据 data_structure 查找具体存储
  │    → 取出 value 和 value_len
  │    → resp_generate_bulk_string_n(response, result, result_len)
  │
  └─ handle_del / handle_mod / handle_exist / handle_range / handle_sort
       → 结构类似：switch data_structure → 调用具体函数 → 生成 RESP
  ▼
返回 resp_len 到 kvs_filter_protocol
  → 解锁
  → AOF 追加（write_command）
  → master_sync_command（主从复制）
```

每一步的职责：

- **`command_table` + `parse_command_argument`**：把协议层的 tokens 映射到 `command_context_t`，包括确定 `data_structure` 类型。这个映射使得命令层不需要理解 RESP，存储层不需要理解命令名。
- **`handle_xxx`**：只做 KV 语义层面的逻辑——SET 存数据、GET 取数据并检查过期、DEL 删数据、RANGE 范围查询。handler 不关心数据来自 RESP 还是 AOF 回放。
- **具体存储函数（`kvs_array_set_len` 等）**：只做本数据结构的增删改查。不关心 RESP 格式、不关心 AOF、不关心复制。

如果将来 handler 迁移到 ops 模式，中间那个 `switch(ctx->data_structure)` 会消失，变为一行 `ops->set(instance, ctx->key, ctx->key_len, ctx->value, ctx->value_len)`。但在此之前，当前设计已经保证了三层之间的接口是清晰的——即使中间多了一个 switch，每层的职责边界也没有混淆。

## 6.6 RANGE 和 SORT 为什么更能体现统一接口的价值

普通的 SET/GET/DEL 操作，四种数据结构的处理流程差异不大——都是"找到 key、操作数据、返回结果"。虽然性能差异巨大（hash 的 O(1) vs array 的 O(n)），但接口层面看不出什么区别。

RANGE 和 SORT 就不一样了。它们直接暴露了数据结构的本质差异：

**array 做 RANGE/SORT**：没有顺序信息，必须全表扫描所有有效槽，把符合范围条件的挑出来，然后 `qsort` 排序。时间复杂度 O(n log n)。当 `n = 100000` 时，每次 RANGE 都要遍历 10 万个槽并排序。

**hash 做 RANGE/SORT**：同样没有顺序。需要遍历整张哈希表的所有槽（包括空槽），把有效 key-value 拷贝到一个临时数组里，再做 `qsort`。唯一的优化点是可以根据负载因子确定扫描范围，但本质还是 O(table_size + k log k)。

**rbtree 做 RANGE**：中序遍历，从 `start_key` 的位置开始，沿着树向右走，遇到 `end_key` 就停。时间复杂度 O(k + log n)，k 是结果数量。不需要扫描无关节点。SORT 就是整个树的中序遍历（O(n)），数据天然有序。

**skiptable 做 RANGE**：从最高层开始，逐层下降到 `start_key` 的位置，然后沿第 0 层向右遍历收集节点，直到 `end_key`。和 rbtree 一样，不需要扫描无关节点。

如果命令层直接面对这些差异，RANGE 的处理代码会变成这样：

```c
// 如果没有统一接口，这是伪代码
if (data_structure == ARRAY) {
    scan_all_slots(&global_array, start, end, result);
} else if (data_structure == HASH) {
    scan_all_buckets(&global_hash, start, end, result);
} else if (data_structure == RBTREE) {
    inorder_traversal_range(&global_rbtree, start, end, result);
} else if (data_structure == SKIPTABLE) {
    skiplist_range_scan(&global_skiptable, start, end, result);
}
```

有了统一接口后（无论是当前的 switch-case 收敛模式，还是未来的 ops 模式），命令层只需要一行 `ops->range(instance, start_key, end_key, response, max_len)`。具体是扫描还是遍历，由 ops 背后的实现决定。命令层不需要知道 rbtree 有中序遍历、skiptable 有逐层下降——它只需要知道"给我这个范围内按序排列的 key-value 列表"。

commit `b26ef36`（2025-08-19）为 SORT 增加了 high/low limit 参数，`829623f`（2026-02-22）为 RRANGE/SRANGE 增加了 limit 参数——这些功能都是基于统一接口才能做到的。如果每个数据结构的 RANGE/SORT 调用方式不同，加一个 limit 参数意味着改四处代码。

## 6.7 binary-safe 与 _len 接口：从字符串 KV 走向字节 KV

第四章讲了 RESP 的 bulk string 用长度描述数据，`token_lens` 保存了真实字节长度。第五章讲了 `command_context_t` 同时携带 `key`/`key_len` 和 `value`/`value_len`。到了存储层，这个"长度"线索继续往下传递。

在四种数据结构的头文件中，都能看到两套接口共存：

```c
// 旧接口：依赖 C 字符串，用 strlen 获取长度
int   kvs_array_set(kvs_array_t *inst, char *key, char *value);
char* kvs_array_get(kvs_array_t *inst, char *key);

// 新接口：显式传入长度，不依赖 \0
int   kvs_array_set_len(kvs_array_t *inst,
        const char *key, size_t key_len,
        const char *value, size_t value_len);
char* kvs_array_get_len(kvs_array_t *inst,
        const char *key, size_t key_len,
        size_t *value_len_out);
```

四套数据结构（array、hash、rbtree、skiptable）都实现了完整的 `_len` 版本接口——`set_len`、`get_len`、`del_len`、`mod_len`、`exist_len`。而且当前命令层的 `handle_set`、`handle_get` 等已经全部使用的是 `_len` 版本（通过 `ctx->key_len`、`ctx->value_len` 传参）。

数据结构内部也在逐步从 `strcmp`/`strlen` 迁移到 `memcmp`/显式长度。例如 rbtree 的 `kvs_rbtree_lookup_node` 接收 `key_len` 参数，内部用 `memcmp` 比较 key；skiptable 的 `Node` 结构保存了 `key_len` 和 `key_hash` 字段，查找时用 `memcmp` + key_len。

但仍然有一些路径还没有完全迁移。hash 的 `_hash_str` 函数内部仍然对 key 调用 `strlen`；某些 RANGE/SORT 路径在拼接响应时依赖 C 字符串的 `%s` 格式化。这些历史残留不影响当前功能（因为实际使用中 key/value 基本都是可打印文本），但确实意味着 KVLite 距离"完全二进制安全"还有一点距离。

这个迁移是一个渐进过程，不是一次重构能完成的——因为每改一处 `strcmp` 为 `memcmp`，都需要确认上游传入的长度信息是否准确、是否存在没有传 `key_len` 的调用路径。**`_len` 接口的全面落地，依赖全体调用链上的每一环都传递长度——从 RESP parser 到 command_context，从 handler 到存储函数。** 第四章和第五章已经把这根链条的前半段接上了，存储层是链条的末端。

## 6.8 TTL、AOF/RDB 和复制为什么依赖稳定的存储接口

这里只简要说明它们和存储层的接口关系，不展开具体逻辑。

**TTL**：每个存储 entry 里都有一个 `expire_at_ms` 字段（`kvs_array_item_t`、`hashnode_t`、`rbtree_node`、`Node` 中都有）。TTL 的惰性删除发生在 `handle_get` 等读路径上——查到 key 后，先检查 `expire_at_ms`，如果已过期就删除并返回"不存在"。后台扫描线程（`src/core/kvs_ttl_background.c`）需要遍历每种数据结构的全部 entry，检查过期时间——这意味着每种数据结构需要提供遍历能力。

**AOF**：命令执行成功后追加到 AOF 文件，这依赖 `command_info.write_command` 字段——和存储层没有直接关系。AOF 重写（rewrite）则是遍历所有数据结构中的活跃 entry，生成等价的 RESP 命令写入新文件——这需要每种数据结构提供全量遍历/导出接口。

**RDB**：快照保存需要遍历所有数据结构的全部 entry，序列化为二进制格式；快照加载需要把二进制记录解析后插入到对应的数据结构中。这同样依赖存储层的遍历和插入接口。

**主从复制**：从节点接收到 master 的命令，解析后通过同样的 `kvs_filter_protocol` → `command_table` → `handle_xxx` → 存储函数路径执行。如果四种数据结构的存储接口不一致，slave 端就需要对每种结构做不同的应用逻辑。

这些横切功能能不能简洁地实现，很大程度取决于存储层有没有统一的"增删改查遍历"接口。当前 KVLite 的实现中，这些横切功能在"数据结构差异"这一层写了不少重复的适配代码（比如 AOF 重写中为四种结构各写了一段遍历逻辑）。如果将来完成了 ops 迁移，这些适配代码可以被统一到一个 iterator 或 scanner 抽象中。

## 6.9 性能优化和 benchmark：为什么统一接口后才能公平比较

从 2026 年 3 月到 4 月，KVLite 经历了一轮集中的性能优化。翻看 commit 记录中的时间线和文档，优化路径是这样的：

```
2c18982 优化：array，避免线性扫描在大数据量下造成的性能下降
eeab1c3 优化：优化热点查找
5f20e59 优化：优化rbtree
7cde5b5 优化：skiptable
8f82fc4 添加：ttl添加十万压测，均通过
62ddd0d 添加：统异步日志，更新redis-benchmark压测结果
db46f16 优化：优化rbtree/skiptable路径中重复路径，rbtree读写已超过redis
6e4a1b6 优化：跳表路径获得部分收益
e57311b 拆分TTL路径，压缩查找/响应冗余，目前支持的四种数据结构读写均与redis持平
37d2e7a 优化：四种数据结构读写基本与redis持平
```

这个优化序列之所以能成立，前提是四种数据结构暴露了同一套语义（SET/GET/DEL），并且可以用同一个 benchmark 脚本（`scripts/benchmark/bench_all.sh`）对它们分别压测。如果每个数据结构的命令名不同、调用路径不同，就不可能做这种"同口径对比"。

从最终 benchmark 结果来看（引用 `docs/rbtree_skiptable_vs_redis_analysis.md` 中的对比表格）：

| 数据结构 | 操作 | KVStore QPS | Redis QPS | 比值 |
|---------|------|------------|-----------|------|
| array   | SET  | 139,664    | 128,369   | 1.088 |
| array   | GET  | 133,868    | 123,915   | 1.080 |
| hash    | SET  | 138,504    | 136,239   | 1.017 |
| hash    | GET  | 136,612    | 129,870   | 1.052 |
| rbtree  | SET  | 111,731    | 127,877   | 0.874 |
| rbtree  | GET  | 112,233    | 126,742   | 0.886 |
| skiptable | SET | 92,081    | 132,978   | 0.692 |
| skiptable | GET | 95,969    | 131,752   | 0.728 |

几个有意思的发现：

1. **array 和 hash 在 SET/GET 场景下已经追平甚至小幅超过 Redis**。这说明 KVLite 的公共路径（网络、RESP、命令分发、内存分配）没有把性能拖垮——至少在和 Redis 同样的命令模型下，轻量级数据结构可以跑到同一量级。

2. **rbtree 和 skiptable 明显落后于 Redis**。这不是因为 KVLite 的实现有 bug，而是因为 Redis 的 SET/GET 走的是哈希表路径（O(1)），而 KVLite 的 rbtree/skiptable 是有序结构路径（O(log n)）。这是"结构选择"的差异，不是"实现水平"的差异——benchmark 的意义正在于让这种差异变得可量化。

3. **当前 benchmark 只覆盖了 SET/GET**。RANGE 和 SORT 的性能数据还没有被系统化地收集。这正是后续要补齐的方向——有了统一接口，用同样的脚本、同样的数据量、同样的并发参数去对比四种结构的 RANGE/SORT 性能，结果会更有说服力。

统一接口的价值在这里体现得很清楚：**不是为了让代码更好看，而是为了让性能结果是可比的。** 如果每种结构的调用方式不同，"benchmark 脚本"本身就变成了一个需要为每种结构定制适配代码的工程——那就失去了"公平对比"的基础。

## 6.10 当前设计解决了什么，又留下了什么问题

### 解决了什么

1. **命令层和存储层在概念上解耦**。`command_context_t.data_structure` 让命令 handler 不需要知道"key 要去哪个全局变量"，存储函数不需要知道"调用我的是 SET 还是 RSET 还是 HSET"。

2. **四种数据结构暴露了统一的 API 模式**。set/get/del/mod/exist/range/sort + 对应的 `_len` 版本，让每种结构的调用方式保持一致。

3. **`_len` 接口已定义且命令层已使用**。binary-safe 的"长度"线索从 RESP → token_lens → command_context → 存储函数 _len 接口全线贯通。遗留的 `strlen`/`strcmp` 调用是局部实现细节，不是接口层面的缺陷。

4. **ops 表已定义，方向已明确**。`kvs_operations` 结构和四个 `_ops` 实例已经在头文件中就位，vector 索引模块已经完整使用了 ops 模式并验证了可行性。KV 命令层的迁移是"什么时候做"而非"能不能做"的问题。

5. **benchmark 可以在统一口径下公平比较四种结构**。共用同一套命令语义、同一套 benchmark 脚本、同一组并发和数据量参数。

6. **RANGE/SORT 的适配被收敛到存储层内部**。命令层不需要知道 rbtree 用中序遍历、skiptable 用逐层下降——调用方式统一为 `ops->range(start, end)` 或等价的 switch 分支。

### 留下了什么问题

1. **KV 命令层的 handler 仍然通过 `switch(ctx->data_structure)` 调用存储函数**。`kvs_operations` 表已定义但未被命令层使用。这是一个"设计到位但实施未完"的状态。

2. **`kvs_operations` 的函数签名还停留在 `char*` 时代**，没有适配 `const char*` + `size_t` 的 `_len` 接口。如果现在就迁移到 ops，反而会退化为只能调用旧接口、丢失长度信息。需要先升级 ops 的函数签名（加上 key_len/value_len 参数），再迁移 handler。

3. **TLL/AOF/RDB/复制中的遍历代码仍然有按数据结构分别处理的历史残留**。AOF 重写为四种结构各写了一段遍历逻辑；RDB 保存/加载也是针对每种结构单独处理。理想的抽象是提供一个统一的 iterator 或 `for_each` 回调接口，让横切功能不需要知道数据结构类型。

4. **不同数据结构的 RANGE/SORT 语义可能不完全一致**。比如 `kvs_rbtree_range` 多了 limit 参数而 `kvs_array_range` 没有——它们的函数签名有微小差异。在统一到 ops 之前需要先对齐接口。

5. **全局存储实例的限制**。当前四种数据结构各有一个全局实例（`global_array`、`global_rbtree`、`global_hash`、`global_skiptable`），不能在同一种数据结构类型下创建多个独立实例。对于未来想要做多 DB（类似 Redis 的 SELECT）或多分片（sharding）来说，这个全局单例设计需要改。

6. **锁粒度是全局的**。`g_kv_data_rwlock` 是一把全局读写锁，所有数据结构的读写操作都在同一把锁下串行化。这个设计简化了并发控制，但也意味着即使是"hash 上做 GET + array 上做 SET"这样互不冲突的操作，也被同一把锁阻塞。

## 6.11 面试时我怎么讲这个阶段

### 30 秒版

问："为什么 KVLite 要支持四种数据结构？为什么要统一接口？"

答："array 是第一个用来验证链路的——实现简单，三天跑通。但每种数据结构有各自擅长的场景：hash 点查快，rbtree 和 skiptable 有序天然支持 RANGE。四种放一起不是为了堆名词，而是要用同一套 benchmark 在统一命令模型下比较它们的性能差异。统一接口就是让命令层不需要知道底层是树还是表——SET 就是 SET，具体怎么存的由存储层决定。这给后续加数据结构、做性能对比、接入 TTL/AOF/复制都留了统一的入口。"

### 2 分钟版

问："KVLite 的存储层是怎么设计的？array/hash/rbtree/skiptable 如何和命令层解耦？"

答："KVLite 的存储层经历了三个阶段。第一个阶段是纯 array——2024 年 5 月，我用一块 1024 槽的数组先让 KV 数据'能存下来'。这是原型阶段，目标是验证主链路通畅，不是追求性能。

第二阶段是三天之内加入了 hash、rbtree 和 skiptable。四种结构各有侧重：hash 点查 O(1)，rbtree 和 skiptable 有序所以 RANGE/SORT 高效。但这时候命令层是直接调用具体函数的——`kvs_array_set`、`kvs_hash_set`、`kvs_rbtree_set`、`kvs_skiptable_set` 在 switch-case 里各占一个分支。命令有很多条、数据结构有四个，两两相乘就是几十个分支，代码重复严重。

第三阶段是引入 `kvs_operations` ops 表——把每种数据结构绑定的 set/get/del/mod/exist/range/sort 七个函数封装成一个结构体。这实际是 C 语言里的手写虚函数表：第一个参数是 `void*` 实例指针，内部做类型转换，类似 C++ 的 this。命令层只需要拿到对应的 ops 表，一行 `ops->set(instance, key, value)` 就能完成操作，不管底层是 array 还是 rbtree。

坦率说，ops 表在当前项目中定义了但 KV 命令层还没完全迁移过来——handler 内部仍有用 switch 选择存储函数的代码。但 vector 索引模块已经完整使用了 ops 模式，flat/HNSW/SQ8 Flat 三种索引通过同一套 `add/search/rebuild` 接口切换。这证明了 ops 模式是行得通的，KV 命令层的迁移是时间问题。

统一接口的另一个实际收益是 benchmark。四种结构用同一套 SET/GET 命令、同一个压测脚本、同一组参数跑——这才有可比性。当前的数据是：array 和 hash 在 SET/GET 上基本追平 Redis（~1.05x），rbtree 约慢 12%，skiptable 约慢 28%。这个差异不是实现水平的问题，是结构选择的问题——Redis 的 SET/GET 走哈希路径 O(1)，而 rbtree/skiptable 是有序结构 O(log n)。如果要对 RANGE/SORT 做公平对比，统一接口就更重要了。"

## 6.12 本章小结

这一章解释了 KVLite 存储层的演进——从最早用 array 验证第一条 SET 链路，到四种数据结构并行，再到定义 ops 表为统一接口铺路。

array 是原型阶段的最优选择——不是为了性能，而是为了速度。它用最低的代码成本证明了"数据能存下来，能查出来"，让主链路跑通。后续加入的 hash、rbtree、skiptable 不是为了堆数据结构名词，而是为了在同一个 KV 命令模型下对比不同存储后端的性能特征和能力边界。

早期命令层直接调用具体存储函数（`kvs_array_set` 等）在命令少、结构少的时候是最快的方式，但当四种数据结构 × 七种操作 × 后续 TTL/AOF/复制/VSEARCH 叠加时，直接调用的耦合代价开始大过它的简洁收益。

`kvs_operations` ops 表是锁定的设计方向——它已经在头文件中定义好，在 vector 索引模块中验证了可行性。KV 命令层的迁移还没有完成，handler 内部仍通过 `switch(ctx->data_structure)` 做分发。但这条"从具体调用到 ops 抽象"的路径是清晰的。

统一接口的真正价值不是代码整洁，而是三个实际收益：让命令层不感知存储差异、让 benchmark 可以公平比较不同结构、让 TTL/AOF/RDB/复制等横切能力有统一的接入点。

下一章将复盘 TTL 子系统——为什么 TTL 不是给 key 加一个 expire 字段就完事，而是一次从读写路径、后台扫描、持久化到主从同步的全链路改造。

## 6.13 本章对应的面试追问清单

1. 为什么最早先用 array？array 的优缺点是什么？
2. array 的查找复杂度是多少？什么时候会变成瓶颈？
3. 为什么后续加入了 hash？hash 在什么场景下最优？
4. rbtree 和 skiptable 都是有序结构，为什么两个都保留了？
5. hash 为什么不适合 RANGE/SORT？
6. skiptable 相比 rbtree 的工程优势是什么？
7. 四种数据结构最初在命令层是如何区分的？SET/RSET/HSET/SSET 有什么问题？
8. 早期直接调用 `kvs_array_set`、`kvs_hash_set` 等具体函数有什么问题？
9. `kvs_operations` 表里有哪些函数指针？`void*` 第一个参数是干什么的？
10. ops 表和 C++ 虚函数表有什么相似之处？`void*` 如何保证类型安全？
11. 当前 KV 命令层是否已经使用 ops 表？如果没有，为什么？
12. vector 索引模块的 ops 模式是怎么用的？和 KV 命令层有什么不同？
13. RANGE 在 array、hash、rbtree、skiptable 里的实现有什么区别？
14. `_len` 接口解决了什么问题？为什么 `kvs_array_set(key, value)` 不够？
15. 当前哪些路径还在用 `strlen`/`strcmp` 而非 `memcmp`/显式长度？
16. TTL 的 `expire_at_ms` 字段在四种结构的 entry 里都存在吗？
17. AOF 重写和 RDB 快照需要遍历所有数据——它们怎么处理四种数据结构的差异？
18. benchmark 为什么能公平比较四种数据结构？SET/GET 的口径是怎样的？
19. 为什么 rbtree/skiptable 的 SET/GET QPS 低于 hash 和 array？这是 bug 吗？
20. 如果新增一个 BTree 存储后端，需要改哪些地方？做完 ops 迁移之后呢？
21. 当前存储层抽象还有什么不足？锁粒度、全局实例、遍历接口各有什么问题？

---

*第六章完。下一章主题：TTL 子系统——全链路过期机制的设计与实现。*
