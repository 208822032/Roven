# 第 1 步：四种数据结构扫描与热点分析

## 1. 本步目标

本步只做代码扫描和问题归类，不改任何源码。目标是先把 KVLite 当前四种数据结构的实现边界、操作链路和性能热点摸清楚，再为后续分步优化建立可验证的分析基线。

本步聚焦四个后端：array、hash、rbtree、skiptable，并同时查看它们在命令层、TTL、RDB、AOF 全量导出中的调用路径。

## 2. 当前代码现状

当前仓库已经把四种结构拆成了独立实现，但它们还没有形成统一的“低成本元数据层”。从现状看，主要问题不是单一算法复杂度，而是大量字符串扫描、重复长度计算、临时分配和遍历拷贝叠加在一起。

- array 仍然是线性查找，删除靠 memmove，排序和范围查询都要先整表复制再 qsort。
- hash 已经有 klen/vlen，但大量路径仍然重复调用 strlen/strcmp，且 range/sort 仍然要把所有有效槽拷贝出来再排序。
- rbtree 和 skiptable 都依赖字典序比较，不能改变按 key 有序的核心语义；它们的问题更偏向比较次数、遍历次数和删除/重连成本。
- TTL、RDB、AOF rewrite 都在做额外的字符串扫描和全量遍历，且很多地方还会重复分配临时 key/value。

## 3. 四种数据结构文件定位

### 3.1 array

- 结构体定义在 [include/kvs_array.h](include/kvs_array.h)
- 核心实现文件在 [src/storage/kvs_array.c](src/storage/kvs_array.c)
- 命令层 TTL 访问入口在 [src/core/kvs_operation.c](src/core/kvs_operation.c)
- 后台过期扫描在 [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c)
- RDB 保存/加载在 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- AOF 重写在 [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)

关键结构和函数：kvs_array_item_t、kvs_array_t、kvs_array_find_index、kvs_array_set、kvs_array_get、kvs_array_del、kvs_array_mod、kvs_array_sort、kvs_array_range、kvs_array_reap_expired。

### 3.2 hash

- 结构体定义在 [include/kvs_hash.h](include/kvs_hash.h)
- 核心实现文件在 [src/storage/kvs_hash.c](src/storage/kvs_hash.c)
- 命令层 TTL 访问入口在 [src/core/kvs_operation.c](src/core/kvs_operation.c)
- 后台过期扫描在 [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c)
- RDB 保存/加载在 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- AOF 重写在 [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)

关键结构和函数：hashnode_t、hashtable_t、_hash_str、_find_idx、kvs_hash_set、kvs_hash_get、kvs_hash_mod、kvs_hash_del、kvs_hash_sort、kvs_hash_range、kvs_hash_reap_expired。

### 3.3 rbtree

- 结构体定义在 [include/kvs_rbtree.h](include/kvs_rbtree.h)
- 核心实现文件在 [src/storage/kvs_rbtree.c](src/storage/kvs_rbtree.c)
- 命令层 TTL 访问入口在 [src/core/kvs_operation.c](src/core/kvs_operation.c)
- 后台过期扫描在 [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c)
- RDB 保存/加载在 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- AOF 重写在 [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)

关键结构和函数：rbtree_node、rbtree、kvs_rbtree_set、kvs_rbtree_get、kvs_rbtree_del、kvs_rbtree_mod、kvs_rbtree_sort、kvs_rbtree_range、rbtree_traversal_with_callback。

### 3.4 skiptable

- 结构体定义在 [include/kvs_skiptable.h](include/kvs_skiptable.h)
- 核心实现文件在 [src/storage/kvs_skiptable.c](src/storage/kvs_skiptable.c)
- 命令层 TTL 访问入口在 [src/core/kvs_operation.c](src/core/kvs_operation.c)
- 后台过期扫描在 [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c)
- RDB 保存/加载在 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- AOF 重写在 [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)

关键结构和函数：Node、kvs_skiptable_t、create_node、free_node、kvs_skiptable_set、kvs_skiptable_get、kvs_skiptable_mod、kvs_skiptable_del、kvs_skiptable_sort、kvs_skiptable_range、kvs_skiptable_reap_expired。

### 3.5 公共分发层

- 命令分发和 TTL 统一入口在 [src/core/kvs_operation.c](src/core/kvs_operation.c)
- 数据结构初始化/销毁在 [src/core/kvstore.c](src/core/kvstore.c)
- 统一操作表和类型声明也出现在 [include/kvstore.h](include/kvstore.h)

## 4. 各结构核心操作链路

### 4.1 array

- key 查找路径：kvs_array_get 和 kvs_array_set 先走 kvs_array_find_index，再做 strcmp 线性扫描。
- set/mod 覆盖写路径：命中后只替换 value，未命中则为 key/value 分别 malloc，再写入 table 尾部。
- del 路径：先找到索引，再释放 key/value，然后 memmove 后续元素前移。
- range/sort/full dump 路径：range 和 sort 都会先复制全部有效项到临时数组，再 qsort；full dump 则直接按 table 顺序遍历。
- ttl/过期扫描路径：kvs_array_reap_expired 线性遍历 len 范围，发现过期后调用 kvs_array_del；后台扫描也按数组索引顺序推进。

### 4.2 hash

- key 查找路径：_find_idx 先算 hash，再按槽位线性探测；命中判断依赖 klen + memcmp。
- set/mod 覆盖写路径：set 会为 key/value 分别申请新内存；mod 只替换 value；两者都会清掉 expire_at_ms。
- del 路径：找到槽位后释放 key/value，把 used 标成 tombstone。
- range/sort/full dump 路径：range 和 sort 都先收集所有 used=1 的槽位到指针数组，再 qsort；full dump 按 max_slots 顺序扫槽。
- ttl/过期扫描路径：kvs_hash_reap_expired 直接扫整个槽数组，过期则走 kvs_hash_del；后台扫描同样是全槽顺扫。

### 4.3 rbtree

- key 查找路径：kvs_rbtree_get、kvs_rbtree_set、kvs_rbtree_del、kvs_rbtree_mod 都通过 rbtree_search 走字典序比较路径。
- set/mod 覆盖写路径：命中节点则替换 value；未命中则分配新节点和 key/value，再插入树中。
- del 路径：先搜索节点，再做 rbtree_delete 维护树平衡和指针关系。
- range/sort/full dump 路径：sort 走中序遍历；range 也是中序递归并按 start_key/end_key 剪枝；full dump 在 [src/core/kvs_operation.c](src/core/kvs_operation.c) 的 handle_fulldump 中通过中序回调导出。
- ttl/过期扫描路径：TTL 访问、RDB/AOF 导出和后台扫描都要遍历树；当前后台扫描的 rbtree 路径使用 rbtree_traversal_with_callback，属于整棵树级遍历。

### 4.4 skiptable

- key 查找路径：kvs_skiptable_get、kvs_skiptable_set、kvs_skiptable_del、kvs_skiptable_mod 都从最高层向下按 strcmp 搜索。
- set/mod 覆盖写路径：命中节点则替换 value；未命中则 create_node 后插入前向链。
- del 路径：先保存 update 路径，再重连 forward 指针并释放节点。
- range/sort/full dump 路径：range 利用有序 forward[0]；sort 升序直接顺扫，降序先收集到临时数组再反向输出；full dump 也沿 forward[0] 逐个导出。
- ttl/过期扫描路径：kvs_skiptable_reap_expired 和后台扫描都沿 forward[0] 顺扫，遇到过期键再删除。

### 4.5 TTL 命令和持久化链路

- TTL 统一入口在 [src/core/kvs_operation.c](src/core/kvs_operation.c#L1) 的 ttl_lookup_array、ttl_lookup_rbtree、ttl_lookup_hash、ttl_lookup_skiptable、ttl_apply_any_key_op。
- 过期判定统一通过 expire_at_ms 和 kv_is_expired 完成，delete 会进一步下发到对应后端。
- RDB 的保存/加载在 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c) 中分别对四种结构做全量遍历和回放。
- AOF rewrite 在 [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c) 中也要对四种结构做全量导出。
- 后台过期线程在 [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c#L1) 中按 budget 轮询四种结构。

## 5. 四种结构的当前复杂度分析

| 结构 | 查找 | set/mod 覆盖写 | del | range/sort | full dump | TTL 扫描 |
| --- | --- | --- | --- | --- | --- | --- |
| array | O(n) | O(n) 查找 + O(1) 写入，扩容时额外 realloc | O(n) + memmove | O(n log n) + 临时复制 | O(n) | O(n) |
| hash | 平均 O(1)，最坏 O(n) | 平均 O(1)，最坏 O(n) | 平均 O(1)，最坏 O(n) | O(n log n) + 临时复制 | O(n) | O(max_slots) |
| rbtree | 平均 O(log n) | 平均 O(log n) | 平均 O(log n) | 理论上接近 O(log n + m)，实际仍依赖遍历和 strcmp | O(n) | O(n) |
| skiptable | 平均 O(log n) | 平均 O(log n) | 平均 O(log n) | range 近似 O(log n + m)，降序 sort 仍有 O(n) 收集成本 | O(n) | O(n) |

说明：这里的复杂度既包含算法复杂度，也包含当前实现里的额外成本，比如字符串比较、临时复制和堆分配。

## 6. 共性热点

- 重复 strlen/strcmp：RDB 保存、AOF rewrite、TTL lookup、range/sort、删除后 reapply expire 都在反复算长度或做字符串比较。
- 频繁 malloc/free：array、rbtree、skiptable 都是 key/value 分别申请；hash 的 set/mod 也会频繁申请 value 或 key/value；RDB/AOF load 还会先申请临时 key/value 再交给 set 再复制一次。
- 遍历排序额外拷贝：array 和 hash 的 range/sort 都需要把所有候选项先拷贝到临时数组，排序后再输出。
- 删除搬移/节点重连：array delete 需要 memmove；hash delete 要处理 tombstone；rbtree/delete 和 skiptable/delete 都有结构重连成本。
- TTL 过期清理开销：后台扫描、读路径惰性删除、RDB/AOF 导出前过滤、导入后重置过期时间，都会触发额外遍历。
- 全量导出重复扫描：handle_fulldump、kvs_rdb_save、kvs_aof_rewrite 都是按后端逐个遍历，且导出时还会再次做字符串长度计算。

## 7. 差异点

- array 的瓶颈是纯线性扫描和删除搬移，最适合通过 dense array + side index 直接降复杂度。
- hash 的瓶颈不是查找，而是 range/sort 和 full dump，另外 tombstone 会拖长探测链。
- rbtree 和 skiptable 的核心优势是 key 有序，不能把它们改成按 hash 排序，也不能用 hash 替代字典序比较语义。
- rbtree 更适合利用 key_hash 作为辅助剪枝或缓存字段，而不是改变树的比较逻辑。
- skiptable 的 range 比 hash 更自然，但降序 sort 仍然要额外收集节点，当前实现没有反向链表或尾指针支撑真正的逆序遍历。
- TTL 扫描上，array/hash/skiptable 都能按线性 cursor 续扫，rbtree 目前更接近整树遍历，边扫边停的能力最弱。

## 8. 元数据字段适配性分析

### 8.1 key_len

- 适合四种结构都引入或显式保留。
- 对 array 和 skiptable 可以减少大量 strcmp 前的无效比较。
- 对 rbtree 只能作为辅助信息，不能替代字典序比较。
- 对 hash 价值已经部分体现，因为 hashnode_t 里已经有 klen；但当前很多路径仍然没把它用到底。

### 8.2 key_hash

- 最适合 hash、rbtree、skiptable 作为辅助字段。
- hash 已经有哈希探测链，缓存 key_hash 可以减少重复算 hash 和部分无效 memcmp。
- rbtree 和 skiptable 只能把 key_hash 当作快速拒绝条件或缓存条件，不能改变有序比较语义。
- array 可以借助 key_hash 做 side index 的索引键，但单独加 hash 字段并不能根治线性扫描。

### 8.3 value_len

- 适合四种结构都引入或保留。
- 目前 RDB、AOF、FULLDUMP、RESP 估算里都在重复 strlen(value)，value_len 可以直接消掉一批重复扫描。
- 对 hash 当前已经有 vlen，但没有被系统性复用。

### 8.4 value_capacity

- 不建议在本阶段统一引入。
- 当前四种结构的值存储都是按需精确分配，value_capacity 只有在改成可复用缓冲区或更激进的内联存储后才有明显收益。
- 对 rbtree、skiptable、hash 这类节点式结构，单纯加 capacity 不会显著减少分配次数，反而会增加字段维护复杂度。
- 对 array，如果后续做 dense array + side index，再评估 value_capacity 才更有意义。

## 9. 哪些优化适合统一推进

- 统一补 key_len 和 value_len 的元数据，并把这些长度从写入路径一路带到 dump、RDB、AOF、RESP 生成链路。
- 统一把过期判断抽象成“先查元数据，再决定是否删”的模式，减少到处重复的 strlen/strcmp。
- 统一收敛全量导出时的字符串长度计算，尽量让输出直接复用已知长度。
- 统一把临时响应缓冲和导出缓冲的分配模式梳理清楚，优先减少可预测的大块 malloc/free。
- 统一保留按 key 的字典序语义，不改 rbtree 和 skiptable 的比较基础。

## 10. 哪些优化必须分结构推进

- array 必须单独走 dense array + side index，不能简单照搬 hash 或树的思路。
- hash 需要单独处理探测链、tombstone 和负载因子问题。
- rbtree 必须保留按 key 有序的比较和旋转语义，key_hash 只能辅助，不能成为排序依据。
- skiptable 必须保留按 key 有序的查找和遍历语义，最多在辅助字段上做轻量优化，不能改成 hash 逻辑。
- TTL 扫描的结构化优化不能一刀切：array/hash 适合 cursor 续扫，rbtree 需要更强的早停能力，skiptable 可以继续利用 forward[0]。

## 11. 风险点

- 结构语义风险：rbtree/skiptable 如果把 key_hash 用成主比较条件，会直接破坏按 key 有序的核心语义。
- 返回值风险：部分 helper 的返回语义本身就不完全一致，后续优化前必须先确认测试对齐点，尤其是 delete/exist 类接口。
- 元数据一致性风险：一旦引入 key_len/value_len，所有 set/mod/load/rewrite 路径都要同步维护，否则容易出现长度和实际字符串不一致。
- 兼容性风险：RDB/AOF/TTL/复制链路都依赖当前节点内容，改元数据后必须确保读写回放不丢 TTL，不漏 key。
- 性能回退风险：如果为了减少 strcmp 过早引入复杂缓存，可能增加写路径维护开销，得不偿失。
- array 改造风险：dense array + side index 能显著提速，但要保证删除、扩容、full dump、TTL 扫描都不会和索引失配。
- rbtree TTL 风险：当前后台扫描对 rbtree 依赖整树遍历，若不增加早停机制，TTL 相关优化收益会被扫描成本吃掉。

## 12. 本步结论

当前四种结构的共性问题是“字符串太多、临时对象太多、遍历太多”，而不是单纯某一种结构的算法不够快。

- array 是最慢、最线性的后端，应该作为第一个结构化优化对象。
- hash 已有 klen/vlen 的雏形，适合先做字段复用和扫描降本，再看是否补 key_hash。
- rbtree 和 skiptable 的核心语义必须保持有序，只能做辅助字段和比较剪枝，不能改成按 hash 排序。
- TTL、RDB、AOF rewrite 是当前字符串扫描和重复分配最密集的公共热点，需要和数据结构优化一起考虑。

## 13. 下一步建议

建议第 2 步先只做 array 的专属方案设计，优先把 dense array + side index 的结构、索引维护规则、TTL 兼容方式和 full dump 影响画清楚；同时把 hash、rbtree、skiptable 的共性字段策略作为对照组保留下来，避免一开始就把四个结构混成一套。

如果按收益和风险排序，建议优先级如下：

1. array：dense array + side index。
2. 全结构统一补 key_len/value_len，并收敛重复 strlen/strcmp。
3. hash：利用现有 klen/vlen 再补 key_hash 的缓存收益。
4. rbtree/skiptable：只做辅助字段和比较剪枝，不改有序语义。
5. TTL / RDB / AOF rewrite 的重复扫描与重复分配降本。
