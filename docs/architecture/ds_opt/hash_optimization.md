# hash 后端专项优化

## 1. 本步目标

本步只优化 hash 后端，不改 array、rbtree、skiptable 的实现和语义。

目标是在不破坏现有命令层语义的前提下，围绕 hash 的查找、覆盖写、TTL 清理、range/sort/full dump 路径做最小侵入优化，重点减少重复 hash、重复字符串比较、重复长度扫描和 tombstone 对探测链的拖累。

## 2. 当前 hash 热点分析

### 2.1 重复 hash / strcmp / strlen

当前 hash 的核心查找路径主要在 [src/storage/kvs_hash.c](src/storage/kvs_hash.c) 中：

- `_find_idx` / `kvs_hash_get` / `kvs_hash_exist` / `kvs_hash_mod` / `kvs_hash_del` 都会重复算 hash。
- 命中判断依赖 `strlen` + `memcmp`，而不是直接复用缓存元数据。
- `sort` / `range` / `RDB` / `AOF rewrite` / `FULLDUMP` 中还会反复对 key/value 做 `strlen`。

这类重复扫描在大 key、大 value 或高频 TTL 场景里会被放大。

### 2.2 tombstone 对探测链的影响

hash 采用开放定址并使用 tombstone 标记删除槽位。这样做虽然保留了探测链，但 tombstone 会让后续查找和插入继续向后探测，导致：

- 触发更多槽位访问；
- 使 lookup 路径退化；
- 长期删除后表面负载不高，但实际探测成本上升。

### 2.3 range / sort 的候选收集与排序开销

当前 range / sort 都需要扫描有效槽位，收集候选，再排序后输出。主要成本在：

- 候选收集时的全槽遍历；
- qsort 的比较次数；
- range 在排序后再做一次线性过滤；
- 输出时仍可能重复扫描字符串长度。

### 2.4 TTL 清理全槽扫描成本

hash 的 TTL 清理路径会扫全槽，命中过期项后还要再走一次删除查找。TTL 背景扫描和命令层 TTL 都会放大这类开销。

## 3. 优化方案

### 3.1 补齐 key_len / value_len / key_hash

本步把 hash 节点元数据补完整：

- `key_len`：用于减少命中判断前的无效比较；
- `value_len`：用于覆盖写和导出路径复用；
- `key_hash`：用于查找路径快速拒绝无关槽位，并减少重复 hash 计算。

### 3.2 查找路径复用缓存元数据

查找、存在性判断、TTL 定位统一走同一套 lookup helper，避免在多个路径里重复实现 hash + 字符串比较逻辑。

### 3.3 覆盖写尽量减少重复分配

对于 `HSET` 覆盖和 `HMOD`：

- 如果新 value 长度和旧 value 长度相同，优先原地覆盖；
- 如果长度不同，再重新分配。

这样可以减少高频覆盖写下的 malloc/free 抖动。

### 3.4 tombstone 压缩策略

删除仍保留 tombstone 语义，但在 tombstone 积累到一定程度后做同容量重建，降低探测链长度。

本步没有改成 side index，也没有改变开放定址的基本模型，只是在 tombstone 过多时做轻量清理。

### 3.5 range / sort / full dump 候选与输出优化

- range / sort 统一候选收集逻辑；
- range 在排序后使用二分边界定位，减少线性过滤；
- full dump / RDB / AOF rewrite 直接使用 `key_len` / `value_len` 拼装 RESP 或 RDB 字段，减少 `strlen`。

## 4. 修改文件列表

本步实际修改的文件如下：

- [include/kvs_hash.h](include/kvs_hash.h)
- [src/storage/kvs_hash.c](src/storage/kvs_hash.c)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)
- [docs/ds_opt/hash_optimization.md](docs/ds_opt/hash_optimization.md)

## 5. 语义保持说明

本步只改 hash 的内部实现，不改对外命令语义。

- `HSET` / `HGET` / `HDEL` / `HMOD` / `HEXIST` 的返回值语义保持不变；
- `HRANGE` / `HSORT` 的排序语义保持字典序不变；
- `FULLDUMP`、RDB、AOF rewrite 输出内容不变，只是拼装时复用了元数据；
- TTL 命令、TTL 背景清理、AOF / RDB 回放仍然沿用现有命令语义。

## 6. 风险点

- tombstone 压缩如果触发过于频繁，会增加写路径开销；如果触发太晚，探测链仍会变长。
- key_len / value_len 必须始终与实际字符串同步，否则导出和覆盖写会出错。
- key_hash 只能作为查找辅助，不能替代字典序语义。
- range / sort 虽然收敛了候选和输出，但本质上仍然要做排序，算法上界没有变化。

## 7. 验证方式

建议用现有测试验证语义正确性：

- `make -j2 kvstore tests/testcase`
- `bash -n tests/test_ttl_aof.sh && ./tests/test_ttl_aof.sh`
- `bash -n tests/test_ttl_rdb.sh && ./tests/test_ttl_rdb.sh`
- `./tests/testcase 127.0.0.1 <port> 3`

重点确认：

- hash 的 `HSET` 覆盖、`HMOD`、`HDEL`、`HEXIST` 正常；
- `HRANGE` / `HSORT` 输出顺序不变；
- TTL / AOF / RDB 读写回放不受影响。

## 8. 本步结论

hash 后端的优化重点不是改成别的数据结构，而是把已经存在的元数据用起来。

这一步已经把查找、覆盖写、TTL 定位、导出拼装里的重复扫描和重复分配收敛到更少的路径上，并为 tombstone 压缩补了轻量清理策略。

## 9. 下一步建议

如果后续继续推进 hash，建议优先做两件事：

1. 进一步观察 tombstone 压缩阈值，在真实负载下调节触发条件。
2. 如果 `HRANGE` / `HSORT` 仍是热点，再考虑更细的候选收集优化，但不要改变字典序语义。
