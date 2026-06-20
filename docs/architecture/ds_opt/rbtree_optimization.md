# rbtree 后端专项优化

## 1. 本步目标

本步只优化 rbtree 后端，不改 array、hash、skiptable 的实现和语义。

目标是在严格保持按 key 字典序有序语义不变的前提下，围绕比较路径、覆盖写、TTL 扫描、range/full dump、RDB/AOF 导出做最小侵入优化，重点减少重复 strcmp、重复 strlen、覆盖写重复分配，以及整树遍历带来的额外成本。

## 2. 当前 rbtree 热点分析

### 2.1 比较路径反复 strcmp

当前 rbtree 的查找、插入、删除、exist 都会走树搜索路径，核心比较集中在 [src/storage/kvs_rbtree.c](src/storage/kvs_rbtree.c)。

问题主要有两个：

- 同一条查询路径上会反复对同一个 key 做字符串比较。
- `strcmp` 既承担字典序判断，又会在每一层树节点上重复扫描字符串前缀，热 key 场景下成本会被放大。

### 2.2 重复 strlen

rbtree 原始节点只保存 key/value 指针，没有显式缓存长度，因此这些路径都会隐式扫描字符串：

- `SET` / `MOD` 覆盖写时重新分配或复制 value。
- `RANGE` / `SORT` 的输出拼接。
- `FULLDUMP`、RDB、AOF rewrite 的导出路径。
- TTL 背景扫描在记录待删 key 时还要再算一次长度。

### 2.3 覆盖写重复分配

`kvs_rbtree_set` 和 `kvs_rbtree_mod` 原来都是命中后直接 free 旧 value，再分配新 value。

这会导致两个问题：

- 相同长度或更短的新 value 也会走完整分配路径。
- 高频覆盖写下会产生不必要的 malloc/free 抖动。

### 2.4 TTL 背景扫描整树遍历

TTL 背景线程在 [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c) 里对 rbtree 使用中序 callback 遍历。

当前问题是：

- budget 很小，但遍历函数本身没有早停能力。
- 即使本轮只需要扫少量节点，递归也可能把整棵树走到底。
- callback 里还会重复记录 key 的字符串长度。

### 2.5 range / full dump / RDB / AOF 导出重复长度计算

rbtree 的全量导出链路分散在多个地方：

- `handle_fulldump` 在 [src/core/kvs_operation.c](src/core/kvs_operation.c)
- RDB 保存/加载在 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- AOF rewrite 在 [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)

这些路径里原来都更偏向用字符串指针直接拼装，rbtree 节点没有长度元数据时，就会在输出侧再次触发长度扫描。

## 3. 优化方案

### 3.1 补齐 key_len / value_len

本步为 rbtree 节点补充：

- `key_len`
- `value_len`

用途是把长度从写入路径一路带到搜索、比较、导出和 TTL 记录路径，减少重复 strlen。

### 3.2 保持有序语义不变

rbtree 仍然只按 key 字典序比较，不引入 key_hash 作为排序依据。

本步的比较优化仅做两件事：

- 用 `key_len + memcmp + 长度回落` 替代多次 `strcmp`。
- 用节点缓存长度减少重复字符串扫描。

### 3.3 覆盖写复用 value buffer

命中覆盖时不再默认 free + malloc，而是优先复用旧 buffer：

- 新 value 长度小于等于旧长度时，直接原地覆盖。
- 新 value 更长时，再走扩容或重新分配。

这样能减少高频覆盖写下的分配抖动。

### 3.4 TTL 扫描增加早停 traversal

为 rbtree 额外提供带 budget 的中序回调遍历：

- TTL 背景扫描只要预算耗尽就立即停止。
- 不再为了少量过期候选扫描整棵树。
- rbtree callback 直接复用节点上的 `key_len` 做待删 key 复制。

### 3.5 range / full dump / RDB / AOF 复用长度

- `RANGE` / `SORT` 的输出拼接改成基于 key_len/value_len 的字节拷贝。
- `FULLDUMP` 对 rbtree 节点使用长度版 RESP 拼装。
- RDB 保存 rbtree 节点时直接写入缓存长度。
- AOF rewrite 的 rbtree 回放使用长度版命令输出。

### 3.6 key_hash 的评估结论

本步评估了 key_hash 作为辅助字段的可能性，但未把它作为 rbtree 结构字段落地。

原因是：

- rbtree 的分支方向仍然必须由字典序决定。
- key_hash 不能替代排序语义，只能做弱辅助。
- 在当前最小侵入目标下，key_len/value_len 的收益更直接。

## 4. 修改文件列表

本步实际修改的文件如下：

- [include/kvs_rbtree.h](include/kvs_rbtree.h)
- [src/storage/kvs_rbtree.c](src/storage/kvs_rbtree.c)
- [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)
- [docs/ds_opt/rbtree_optimization.md](docs/ds_opt/rbtree_optimization.md)

## 5. 有序语义保持说明

rbtree 的核心语义没有变：

- 查找仍按 key 字典序。
- 插入仍按 key 字典序决定左右分支。
- 删除仍按树节点关系维护红黑树平衡。
- range / sort 仍然依赖中序遍历得到的有序序列。

本步只把比较实现从重复 `strcmp` 收敛成“先比较长度，再用 `memcmp` 确认字典序”，不会把 key_hash 作为排序依据，也不会改变范围查询的边界语义。

## 6. 风险点

- `key_len` / `value_len` 必须和实际字符串同步，否则导出和覆盖写会产生错误。
- `memcmp` 只能在长度已知前提下使用，任何未初始化长度都会放大 bug。
- 覆盖写的 buffer 复用必须保证新 value 不会越界写入。
- TTL 背景扫描如果早停条件处理不当，可能出现本轮扫描不完整的问题。
- `rbtree_delete` 中节点内容交换时必须同步交换长度和 TTL 元数据，否则删除后记录会失真。

## 7. 验证方式

建议优先复用现有测试：

- `make -j2 kvstore tests/testcase`
- `bash -n tests/test_ttl_aof.sh && ./tests/test_ttl_aof.sh`
- `bash -n tests/test_ttl_rdb.sh && ./tests/test_ttl_rdb.sh`
- `bash -n tests/test_ttl_tcp_replication.sh && ./tests/test_ttl_tcp_replication.sh`
- `./tests/testcase 127.0.0.1 <port> 3`

重点确认：

- RSET / RGET / RDEL / RMOD / REXIST 的语义不变。
- RRANGE / RSORT 的字典序结果不变。
- FULLDUMP、RDB、AOF rewrite 输出内容不变，只是长度复用更充分。
- TTL 过期后删除和复制链路仍然一致。

## 8. 本步结论

rbtree 的瓶颈并不是树结构本身，而是比较、长度扫描、覆盖写分配和全树遍历的重复成本。

本步通过补齐长度元数据、收敛比较路径、复用覆盖写 buffer、引入 TTL 早停遍历，以及让导出路径直接复用长度，保留了字典序有序语义，同时把主要热点从“重复扫描字符串”压缩到“单次定位 + 元数据复用”。

## 9. 下一步建议

如果后续继续推进 rbtree，建议优先做两件事：

1. 在更大数据量下观察 TTL 早停遍历的收益和 budget 触发频率。
2. 如果导出路径仍然是热点，再考虑把 RESP 组包进一步抽成通用的长度版拼装 helper，但不要改动 rbtree 的有序比较语义。
