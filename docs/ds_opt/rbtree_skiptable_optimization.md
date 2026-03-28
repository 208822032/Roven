# rbtree / skiptable 性能优化方案

## 1. 本步目标

本步只分析和规划 rbtree、skiptable 两个有序后端的后续优化，不改源码。

目标很明确：在保持按 key 有序语义不变的前提下，解释为什么这两个后端在当前基准里仍然比 Redis 慢，并给出下一阶段可落地的优化路线。

本步特别关注以下场景：

- `SET` / `GET` 的主路径吞吐
- `EXIST` / `MOD` / `DEL` 的命中与覆盖
- `RANGE` / `SORT` / `FULLDUMP`
- TTL 清理与持久化导出

## 2. 当前性能结论

基于 [results/summary.md](../../results/summary.md) 与 [results/raw.csv](../../results/raw.csv) 的当前基准，array 和 hash 已经接近或超过 Redis，而 rbtree / skiptable 仍有明显差距。

当前数据里，Redis 对比结果如下：

- rbtree `set`：KVLite 89126.56 qps，Redis 113765.64 qps
- rbtree `get`：KVLite 93283.58 qps，Redis 121065.38 qps
- skiptable `set`：KVLite 82644.62 qps，Redis 114810.56 qps
- skiptable `get`：KVLite 82304.52 qps，Redis 106609.80 qps

也就是说，当前 ordered 后端的主要差距大约在 20% 到 30% 左右，且 skiptable 的 `get` 已经是这轮 benchmark 中最弱项之一。

当前仓库未直接包含 Redis 源码，因此下面关于 Redis 的判断，基于 benchmark 现象、当前 KVLite 源码形态，以及 Redis 通用实现特征综合分析。

## 3. 为什么 rbtree / skiptable 还是比不上 Redis

### 3.1 Redis 的主路径更偏向 O(1) 哈希查找

从基准类型看，Redis 在这个场景下主要跑的是字符串 key 的普通 `SET` / `GET` 路径，通常会走高优化程度的字典结构，而不是有序树或跳表的查找路径。

相比之下，KVLite 的 rbtree 和 skiptable 都是有序结构：

- rbtree 需要沿树高做多次比较和指针跳转
- skiptable 需要从高层一路向下探测

这意味着即使理论复杂度不错，常数也明显更大。

### 3.2 pointer chasing 和分支预测成本更高

在 [src/storage/kvs_rbtree.c](../../src/storage/kvs_rbtree.c) 中，rbtree 的搜索路径要在多个节点间来回跳转，旋转和修复时也有较多父子指针操作。

在 [src/storage/kvs_skiptable.c](../../src/storage/kvs_skiptable.c) 中，skiptable 的每次查找都要跨层访问 `forward[]`，实际运行中会带来：

- CPU cache 命中率更差
- 分支预测更不稳定
- 比 hash 表更高的指针跳转成本

这类成本在 `GET`、`SET` 和 `EXIST` 的高频小包场景里会被放大。

### 3.3 字符串比较仍然是热点

当前 rbtree / skiptable 都保留了按 key 字典序比较的核心语义，因此比较成本无法被真正消掉，只能被压低。

当前源码里可直接看到：

- rbtree 使用 `rbtree_key_compare`，本质仍是 `memcmp + 长度比较`
- skiptable 仍然需要在多层查找时做键比较，只是现在已经把 `key_len` / `value_len` / `key_hash` 作为辅助元数据补上了

这意味着有序后端天然比 hash 路径更“重”，而 Redis 在这个场景里通常不会付出这类树/跳表维护成本。

### 3.4 节点式内存布局导致缓存局部性弱

rbtree 和 skiptable 都是典型的节点式结构：

- 每个 key/value 都单独分配
- 每个节点都带有额外指针或层级信息
- 遍历时更依赖离散内存访问

这和 Redis 的高优化对象/字典路径相比，内存局部性更弱，分配和访问都更容易成为瓶颈。

### 3.5 写路径还要维护有序语义

rbtree 在插入和删除时，需要维护平衡、颜色和旋转；skiptable 在插入和删除时，需要维护多层 forward 指针。

也就是说，ordered 后端并不只是“查找比 Redis 慢”，写路径本身也比纯哈希表更重。

### 3.6 全量导出和 TTL 扫描会进一步放大差距

在当前仓库里，rbtree / skiptable 不仅要承担普通读写，还要承担：

- TTL 命令查询
- TTL 后台扫描
- RDB 保存 / 加载
- AOF rewrite
- FULLDUMP
- RANGE / SORT

这些路径都需要保持有序语义，无法像 hash 那样简单地走轻量查找，因此在混合负载下会进一步把性能差距拉开。

## 4. rbtree 优化方案

### 4.1 先做“比较少一次、路径短一点”

rbtree 的第一优先级不是改算法，而是减少无效比较和无效跳转：

- 保持 `key_len`、`value_len` 一路贯穿
- 查找时优先用长度做快速剪枝，再进入字典序比较
- 覆盖写时尽量复用 value 缓冲，减少重复分配

当前 [src/storage/kvs_rbtree.c](../../src/storage/kvs_rbtree.c) 已经具备长度元数据基础，后续应继续把这个基础用到底。

### 4.2 把递归遍历换成更轻的迭代/游标遍历

当前 rbtree 的 `RANGE` / `SORT` / `FULLDUMP` / TTL 相关遍历，仍然有比较明显的递归和回调开销。

建议下一阶段把高频遍历改成：

- 非递归中序遍历
- 支持 early stop 的游标式遍历
- limit 场景只访问必要节点，不扫完整棵树

这对 `SORT limit N`、`RANGE limit N` 和 TTL 扫描都更友好。

### 4.3 继续压缩写路径分配成本

rbtree 的 set/mod 仍然是节点式写入，建议继续收敛：

- 命中覆盖时优先原地复用 value 缓冲
- 仅在 value 变长时重新分配
- 新插入时避免重复复制 key/value

这类优化不会改变有序语义，但能直接减少 malloc/free 抖动。

### 4.4 用 key_hash 做辅助剪枝，不做排序依据

可以考虑在 rbtree 节点上继续补充或强化 `key_hash` 作为辅助字段，用于：

- 快速拒绝明显不可能命中的节点
- 减少部分路径的无效 memcmp
- 帮助导出/扫描时做缓存判断

但必须保持：

- 排序仍然以 key 字典序为准
- 旋转和树形结构仍然以 key 比较为准

### 4.5 持久化路径统一用长度版输出

RDB / AOF / FULLDUMP 都应继续坚持长度版拼装，避免在高频导出时再次扫描字符串。

## 5. skiptable 优化方案

### 5.1 继续强化长度元数据的复用

skiptable 已经补齐了 `key_len` / `value_len` / `key_hash`，下一步重点不是再加字段，而是把这些字段真正贯穿到所有路径：

- 查找
- 覆盖写
- `RANGE`
- `SORT`
- `FULLDUMP`
- RDB / AOF 导出

当前 [src/storage/kvs_skiptable.c](../../src/storage/kvs_skiptable.c) 已经把这些方向打通，后续应继续围绕“少比较、少扫描、少分配”推进。

### 5.2 降序 SORT 继续优化临时收集策略

skiptable 的一个明显热点是降序 `SORT`。

建议下一步继续做两件事：

- 维持 `limit` 场景的限量收集，而不是完整收集再反转
- 如果后续需要更强的逆序能力，评估增加尾指针或额外反向游标，但不能破坏有序语义

这类优化比单纯缩短比较链路更容易见效，因为它直接减少了临时对象数量和遍历成本。

### 5.3 RANGE / FULLDUMP 继续长度化

skiptable 的 `RANGE` 和 `FULLDUMP` 不应该再重复走 `strlen` + `snprintf` 的老路。

建议继续保持：

- 输出时直接用 `key_len` / `value_len`
- 内部拼装尽量使用 `memcpy`
- 导出函数只负责遍历，不再负责字符串扫描

### 5.4 TTL 清理保持顺扫，但减少二次定位

skiptable 的 TTL 清理天然适合沿 `forward[0]` 顺扫，继续保持这条路线是合理的。

下一步优化重点不是改变顺扫方向，而是减少“扫到后还要再定位一次”的额外开销，例如：

- 尽量在扫描上下文里直接记录可删位置
- 删除时减少重复比较
- 保持后台扫描 budget 化，避免一次性扫太多节点

### 5.5 如果要继续提速，可以评估更强的 skiplist 元数据

如果后续仍然要追 Redis，可以再评估更激进但仍不破坏有序语义的方案，例如：

- 局部 span / rank 元数据
- 更稳定的尾部游标
- 针对 `limit` 的跳跃优化

但这些方案复杂度更高，应该在当前长度元数据方案稳定后再推进。

## 6. 为什么 hash / array 提升更明显

这个问题的本质是：hash / array 的优化更接近它们的主瓶颈，而 rbtree / skiptable 的瓶颈更偏结构性。

### 6.1 array 的收益更直接

array 通过 side index 把查找从线性扫描改成了索引定位，并把删除从 memmove 改成了尾覆盖，因此收益非常直接。

### 6.2 hash 的收益更直接

hash 本身就是面向查找的结构，补齐 `key_len` / `value_len` / `key_hash` 后，很多路径都能直接复用元数据，立刻减少重复扫描。

### 6.3 rbtree / skiptable 的优化更多是在“把有序成本压低”

rbtree / skiptable 不能放弃有序语义，所以它们没法像 array 那样大幅改结构，也没法像 hash 那样直接转成 O(1) 查找。

因此，它们的优化收益通常更像：

- 少一次比较
- 少一次分配
- 少一次遍历
- 少一次临时收集

而不是结构级跃迁。

## 7. 修改文件列表

本步只生成文档，不改源码。

参考分析的主要文件：

- [results/summary.md](../../results/summary.md)
- [results/raw.csv](../../results/raw.csv)
- [src/storage/kvs_rbtree.c](../../src/storage/kvs_rbtree.c)
- [src/storage/kvs_skiptable.c](../../src/storage/kvs_skiptable.c)
- [src/core/kvs_operation.c](../../src/core/kvs_operation.c)
- [src/core/kvs_ttl_background.c](../../src/core/kvs_ttl_background.c)
- [src/persistence/kvs_rdb.c](../../src/persistence/kvs_rdb.c)
- [src/persistence/kvs_aof_rewrite.c](../../src/persistence/kvs_aof_rewrite.c)

## 8. 有序语义保持说明

所有优化建议都必须满足一个前提：rbtree 和 skiptable 的对外语义仍然是按 key 有序。

这意味着：

- 不能把 key_hash 当作排序依据
- 不能把查找结果改成 hash 优先
- 不能破坏 `RANGE` / `SORT` 的字典序结果
- 不能为了加速逆序输出而改变正序语义

如果某个优化会动到这个前提，就不应该直接进入主线。

## 9. 风险点

- 如果为了减少比较而过早引入复杂缓存，可能把写路径维护成本抬高，得不偿失。
- 如果把 `key_hash` 用成主比较条件，会直接破坏 ordered 语义。
- 如果把 range/sort 的临时收集继续扩大，ordered 后端的优势会被遍历成本抵消。
- 如果 TTL 清理仍然反复“先扫再找再删”，优化收益会被重复删除链路吃掉。
- 如果继续在 rbtree / skiptable 上叠加过多通用逻辑，代码复杂度会上升得比收益更快。

## 10. 验证方式

本步是分析和方案文档，后续真正落地时建议沿用以下验证方式：

- 先看 `SET` / `GET` 的基准变化
- 再看 `RANGE` / `SORT` 的限量场景
- 再看 FULLDUMP / AOF rewrite / RDB 的导出成本
- 最后看 TTL 背景扫描和命中删除是否产生语义回退

建议继续使用当前仓库已有测试：

- `tests/test_ttl_all_backends.sh`
- `tests/test_ttl_aof.sh`

如果后续增加性能验证，建议再补一组更细的 ordered backend micro-benchmark，单独对比：

- rbtree `get` / `set`
- skiptable `get` / `set`
- `range limit`
- `sort desc limit`

## 11. 本步结论

当前结果可以归纳成一句话：array 和 hash 的提升是结构级收益，而 rbtree / skiptable 还停留在“把有序结构的常数项压低”的阶段。

因此，rbtree 和 skiptable 想继续追近 Redis，不能只盯着单个函数做微调，而要围绕以下方向系统推进：

- 减少比较次数
- 减少指针跳转
- 减少临时收集
- 减少重复分配
- 减少导出扫描
- 保持有序语义不变

## 12. 下一步建议

如果继续推进，建议优先级如下：

1. 先把 rbtree 的遍历型路径改成更轻的迭代式实现，优先收敛 `RANGE` / `SORT` / `FULLDUMP`。
2. 再把 skiptable 的降序 `SORT` 和 TTL 清理做成更明确的 limit 化、cursor 化路径。
3. 最后再评估是否需要为有序后端增加更强的辅助元数据，例如 tail 指针、span 或游标缓存。

这条路线的核心原则不变：优化性能，但不牺牲 key 的有序语义。