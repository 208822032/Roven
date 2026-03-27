# 第 3 步：array 第一轮 side index 落地说明

## 1. 本步目标

本步完成 array 的第一轮 side index 实现，目标是在不破坏现有命令层语义的前提下，把 GET、EXISTS、SET 覆盖、MOD、DEL 的查找和删除路径从线性扫描切到索引定位，并让 TTL 与后台清理复用按索引删除，避免重复按 key 查找。

本步没有重写 range、sort、full dump，也没有改 rbtree / skiptable 的有序语义，只针对 array 做最小侵入优化。

## 2. 修改文件列表

本步实际修改了以下文件：

- [include/kvs_array.h](include/kvs_array.h)
- [src/storage/kvs_array.c](src/storage/kvs_array.c)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c)
- [docs/ds_opt/03_array_side_index_impl.md](docs/ds_opt/03_array_side_index_impl.md)

其中，前四个是代码落地文件，最后一个是本步分析与实施说明。

## 3. side index 落地方式

array 仍然保留连续主数组作为真实存储层，同时新增一层 side index 作为 key -> dense_idx 的辅助定位层。

### 3.1 主数组字段变化

[include/kvs_array.h](include/kvs_array.h) 中的 `kvs_array_item_t` 增加了以下元数据：

- `key_len`
- `value_len`
- `key_hash`

这样做的目的不是改变语义，而是把后续遍历、删除、TTL 清理里会反复用到的信息提前固化下来，减少重复 `strlen` 和重复哈希计算。

### 3.2 side index 结构

同一文件中新增加了 `kvs_array_index_entry_t`，每个索引项保存：

- `key`
- `key_len`
- `key_hash`
- `dense_idx`
- `state`

索引层采用开放定址方式维护，不额外挂链表，也不改变主数组的密集存储形式。

### 3.3 容量与扩容

`kvs_array_t` 里新增了：

- `side_index`
- `side_capacity`
- `side_count`
- `side_tombstones`

插入前会检查 side index 是否需要扩容或重建，避免因为墓碑过多导致 probe 链过长。主数组继续按原有 `reserve` 逻辑扩容，优先保证结构最小改动。

## 4. 查找路径改动

这一步把 array 的核心查找接口都切到了 side index：

- `kvs_array_lookup_index`
- `kvs_array_get`
- `kvs_array_exist`
- `kvs_array_set` 的覆盖路径
- `kvs_array_mod`
- `kvs_array_del`

具体表现是：

- 先算 `key_hash` 和 `key_len`；
- 再在 side index 中定位；
- 找到后直接拿到 `dense_idx`；
- 进而直接访问主数组槽位。

这意味着 GET、EXISTS、SET 覆盖、MOD、DEL 不再依赖主数组全表线性扫描。

## 5. 删除与索引修复方案

本步删除策略改成了尾元素覆盖。

### 5.1 删除入口

新增了 `kvs_array_del_at`，可直接按主数组下标删除，供内部清理和 TTL 场景使用。

### 5.2 删除流程

删除时的步骤是：

1. 从 side index 定位要删的 dense_idx；
2. 先从 side index 中移除该 key 的索引项；
3. 如被删位置不是尾部，则用尾元素覆盖当前槽位；
4. 修复被搬来的尾元素在 side index 里的 dense_idx；
5. 清理尾槽并释放被删 key/value；
6. len 和 total 同步回退。

### 5.3 语义影响

本步没有把 array 的外部顺序语义作为稳定承诺，因此尾元素覆盖不影响 GET、EXISTS、SET、MOD、DEL 的结果语义。

range、sort、full dump 本步没有重写，仍维持现有逻辑，因此对外行为保持一致。

## 6. TTL 清理如何复用按索引删除

TTL 路径也已经接入按索引删除，避免再次按 key 查找。

### 6.1 命令层 TTL

[src/core/kvs_operation.c](src/core/kvs_operation.c) 中的 array TTL 查找改为先走 `kvs_array_lookup_index`，再把 `entry` 指向主数组槽位。

当 TTL 命中需要删除时，`ttl_target_delete` 的 array 分支会直接把 `entry` 转回 dense_idx，然后调用 `kvs_array_del_at`。

### 6.2 后台扫描

[src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c) 中的 array 扫描改成按 dense_idx 顺扫，发现过期项后直接调用 `kvs_array_del_at`，不再先记录 key 再二次查找删除。

这一步是本轮优化的重点之一，直接消掉了“扫描后再按 key 删除”的重复路径。

## 7. 风险点

- 尾元素覆盖会改变主数组的物理顺序，但当前语义并不依赖主数组顺序。
- 主数组和 side index 是双写结构，插入、删除、搬移、扩容都必须同步，否则会出现脏索引。
- TTL 清理里如果继续走按 key 删除，性能收益会被抵消，所以内部清理必须坚持按索引删除。
- side index 使用开放定址，若墓碑过多，probe 链会变长，因此需要重建策略。
- 本步没有重写 range、sort、full dump，因此这三条路径的成本还没有被彻底优化掉。

## 8. 验证方式

本步已经用现有测试完成回归验证，覆盖了语义正确性和持久化链路：

- `make -j2 kvstore`
- `bash -n tests/test_ttl_mvp.sh && ./tests/test_ttl_mvp.sh`
- `bash -n tests/test_ttl_aof.sh && ./tests/test_ttl_aof.sh`

回归结果：

- `tests/test_ttl_mvp.sh` 通过，array、rbtree、hash、skiptable 的 TTL 矩阵均为 PASS。
- `tests/test_ttl_aof.sh` 通过，array、rbtree、hash、skiptable 的 AOF TTL 场景均为 PASS。

这说明本轮 array 索引改动没有破坏现有 TTL 语义、AOF 回放语义和跨后端命令行为。

## 9. 本步结论

array 的第一轮 side index 已经落地，查找和删除的主路径已经从线性扫描迁移到索引定位。

这一步的主要收益是：

- GET、EXISTS、SET 覆盖、MOD、DEL 不再扫全表；
- 删除改成尾元素覆盖，避免 memmove；
- TTL 命令和后台清理都复用了按索引删除；
- 现有 TTL MVP 和 AOF 回归都通过，说明语义没有被打坏。

## 10. 下一步建议

下一步如果继续推进 array，建议只做两类事情：

1. 进一步收敛 range / sort / full dump 的候选收集成本，但仍保持对外语义不变。
2. 给 side index 补更稳的重建和一致性校验逻辑，降低墓碑和异常状态下的风险。

如果要继续往性能上推进，最值得优先改的仍然是 array 的遍历型路径；rbtree / skiptable 不要改成按 hash 排序，保持有序语义不变。
