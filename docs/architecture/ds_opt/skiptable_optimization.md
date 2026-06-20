# skiptable 专项优化说明

## 1. 本步目标

本步只优化 skiptable 后端，不改变按 key 有序的核心语义，重点收敛以下路径成本：

- 多层查找中的重复 `strcmp`
- 重复 `strlen`
- 覆盖写时的重复分配
- 降序 `SORT` 的临时收集成本
- TTL 清理沿 `forward[0]` 顺扫后的删除成本
- `RANGE` / `SORT` / `FULLDUMP` / AOF / RDB 导出时的长度复用

本步的约束是：`key_hash` 只能作为辅助元数据，不能作为排序依据，也不能改变 skiptable 的字典序语义。

## 2. 当前 skiptable 热点分析

skiptable 当前是四种后端里唯一没有补齐长度元数据的有序结构，因此热点主要集中在字符串扫描和导出重复计算上。

- 查找路径：`SET` / `GET` / `MOD` / `DEL` / `EXIST` / `EXPIRE` 都要从高层向下做字典序搜索，原实现依赖重复 `strcmp`。
- 覆盖写路径：命中后只替换 value，但原实现仍然是重新 `malloc` / `free`，没有复用已知长度和已有缓冲。
- 降序排序路径：原实现必须先把全量节点收集到临时数组，再逆序输出，limit 场景也没有做收缩。
- `RANGE` / `FULLDUMP` / AOF / RDB 导出：原实现多处对 key/value 反复 `strlen`，输出阶段还用通用格式化函数重复扫描。
- TTL 清理路径：后台和部分读路径都沿 `forward[0]` 扫描，删除仍然要再次按 key 搜索，额外放大了扫描成本。

## 3. 优化方案

### 3.1 节点补齐长度元数据

在 [include/kvs_skiptable.h](include/kvs_skiptable.h) 的 `Node` 中补齐：

- `key_len`
- `value_len`
- `key_hash`

这样做的目的不是改变排序逻辑，而是把后续查找、覆盖写、导出时会反复用到的信息提前固化下来。

### 3.2 查找路径改为长度驱动

在 [src/storage/kvs_skiptable.c](src/storage/kvs_skiptable.c) 中，查找路径改成：

- 先计算一次 key 长度和 hash
- 多层前进时用长度感知的比较函数
- 只有命中候选节点后再做最终等值确认

这样可以减少重复 `strlen`，并把 `strcmp` 收敛到更少的比较点。

### 3.3 覆盖写尽量复用 value 缓冲

覆盖写时新增 value 复用逻辑：

- 新 value 长度不超过旧缓冲时直接原地覆盖
- 只有新 value 更长时才扩容/重分配

这和 rbtree / hash 的做法保持一致，能减少重复分配和释放。

### 3.4 降序 SORT 采用限长收集

`SORT` 降序不再默认完整收集全表；当存在 `limit` 且 `limit < count` 时，只保留最近 `limit` 个节点进入环形缓冲区。

这样可以把常见的“只看前 N 条”场景从 O(n) 临时收集降到 O(limit) 临时收集。

### 3.5 RANGE / SORT 输出改为长度拼装

`RANGE` / `SORT` 的输出不再用 `snprintf("%s:%s\r\n")` 反复扫描字符串，而是直接利用 `key_len` / `value_len` 做 `memcpy` 拼装。

### 3.6 FULLDUMP / RDB / AOF 导出复用长度

skiptable 的 full dump、RDB、AOF rewrite 都改为直接使用 `key_len` / `value_len`：

- [src/core/kvs_operation.c](src/core/kvs_operation.c) 的 `FULLDUMP` skiptable 分支改为长度版 RESP 组装
- [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c) 的 skiptable 保存改为长度复用
- [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c) 的 skiptable 重写改为长度版命令输出

### 3.7 TTL 清理保持顺扫，但减少辅助开销

skiptable 的 TTL 清理仍然沿 `forward[0]` 顺扫，保持有序结构下最简单、最稳妥的遍历方式。

本步没有尝试引入反向链表或尾指针，也没有把 key_hash 当成排序依据；TTL 删除仍然是结构化 delete，只是查找和导出阶段的字符串成本被压低了。

## 4. 修改文件列表

本步实际修改了以下文件：

- [include/kvs_skiptable.h](include/kvs_skiptable.h)
- [src/storage/kvs_skiptable.c](src/storage/kvs_skiptable.c)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)

其中：

- [include/kvs_skiptable.h](include/kvs_skiptable.h) 负责节点元数据扩展
- [src/storage/kvs_skiptable.c](src/storage/kvs_skiptable.c) 负责查找、覆盖写、range/sort 输出和 TTL 清理相关的主优化
- [src/core/kvs_operation.c](src/core/kvs_operation.c) 负责 skiptable 的 TTL / FULLDUMP 复用长度
- [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c) 负责 RDB 保存的长度复用
- [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c) 负责 AOF rewrite 的长度复用

## 5. 有序语义保持说明

本步没有改变 skiptable 的有序语义，仍然保持按 key 字典序排序和遍历。

- 查找仍然是按 key 的字典序做多层跳跃
- `RANGE` 仍然按字典序输出区间内节点
- `SORT` 仍然按字典序输出，只是降序收集方式更轻
- `FULLDUMP`、RDB、AOF 仍然按节点实际顺序遍历导出
- `key_hash` 只用于辅助确认，不参与排序，也不影响遍历顺序

## 6. 风险点

- `key_len` / `value_len` 必须和真实字符串一致，否则导出和输出都会出错。
- `key_hash` 只能辅助比较，不能替代字典序比较，否则会破坏 ordered 语义。
- 降序 `SORT` 的环形缓冲区只优化 `limit` 场景，`limit` 很大时仍然会回退到全量收集。
- TTL 清理仍然会触发按 key 删除，最坏情况下仍有 `O(n log n)` / `O(n)` 的结构性成本，只是把重复字符串扫描降下来了。
- skiptable 的读路径如果继续引入过多副作用，会放大删除和遍历交织的维护复杂度，因此本步没有把读路径改成更激进的缓存式结构。

## 7. 验证方式

本步复用了现有回归测试，重点覆盖 TTL、range/sort、FULLDUMP 和 AOF 重写：

- `make -B -j2 kvstore`
- `bash -n tests/ttl/test_ttl_all_backends.sh && ./tests/ttl/test_ttl_all_backends.sh`
- `bash -n tests/test_ttl_aof.sh && ./tests/test_ttl_aof.sh`

验证结果：

- `tests/ttl/test_ttl_all_backends.sh` 通过，skiptable 的 TTL、覆盖写、`RANGE`、`SORT`、`FULLDUMP` 均保持正确
- `tests/test_ttl_aof.sh` 通过，skiptable 的 AOF rewrite / reload 以及 TTL 保留均保持正确

## 8. 本步结论

skiptable 本轮已经补齐了和其他有序后端一致的元数据能力，并把最密集的字符串热点压了下去。

当前收获主要是：

- 查找路径减少了重复字符串扫描
- 覆盖写减少了不必要的重复分配
- 降序 `SORT` 的临时收集在 `limit` 场景下明显降本
- `RANGE` / `FULLDUMP` / RDB / AOF 导出统一复用了长度元数据
- 现有 TTL / AOF 回归已经通过，说明 ordered 语义没有被破坏

## 9. 下一步建议

如果后续继续推进 skiptable，建议只做两类事情：

1. 继续收敛 TTL 清理和删除路径的常数开销，例如把删链和扫描的边界再做一次整理。
2. 如果要继续做性能收益评估，优先补充 skiptable 的 range / sort 大数据量基准，而不是改动有序语义本身。

不建议把 skiptable 改成依赖 hash 的排序结构，也不建议为了省一次 `strcmp` 去引入破坏字典序的比较逻辑。