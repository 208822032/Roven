# 第 4 步：遍历 / 范围类命令的过期行为对齐

## 1. 本步目标

本步只修正一件事：`RANGE / SORT / FULLDUMP` 以及当前仓库中的等价遍历接口，不再返回已过期 key。

本步不做的事情：

- 不重写现有遍历框架
- 不新增独立 expires 索引
- 不引入时间轮、最小堆或后台全量扫描器
- 不改变 TTL 的基础语义（仍然沿用 `expire_at_ms` + 惰性删除）

本步的核心判断标准是：遍历类命令的结果必须只包含“当前仍有效”的 key。

## 2. 受影响接口清单

### 命令层入口

- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- `handle_range`
- `handle_sort`
- `handle_fulldump`

### 数组后端

- [src/storage/kvs_array.c](src/storage/kvs_array.c)
- `kvs_array_sort`
- `kvs_array_range`

### 红黑树后端

- [src/storage/kvs_rbtree.c](src/storage/kvs_rbtree.c)
- `kvs_rbtree_sort`
- `kvs_rbtree_range`

### 哈希后端

- [src/storage/kvs_hash.c](src/storage/kvs_hash.c)
- `kvs_hash_sort`
- `kvs_hash_range`

### 跳表后端

- [src/storage/kvs_skiptable.c](src/storage/kvs_skiptable.c)
- `kvs_skiptable_sort`
- `kvs_skiptable_range`

## 3. 当前遍历路径分析

### 3.1 命令分发路径

`RANGE / SORT` 先在 [src/core/kvs_operation.c](src/core/kvs_operation.c) 里进入 `handle_range` / `handle_sort`，再按 `ctx->data_structure` 分发到四个 storage 后端。

`FULLDUMP` 不走单个后端的命令接口，而是直接在 [src/core/kvs_operation.c](src/core/kvs_operation.c) 里遍历全局结构：

- 数组：直接扫 `global_array.table`
- 哈希：直接扫 `global_hash.nodes`
- 红黑树：通过 `rbtree_traversal_with_callback`
- 跳表：直接沿 `forward[0]` 扫

### 3.2 数组后端的遍历方式

`kvs_array_sort` / `kvs_array_range` 的典型路径是：

1. 把当前 `inst->table` 里的元素复制到临时数组
2. 对临时数组按 key 排序
3. 按排序结果输出

这条路径的特点是：遍历结果来自“快照式临时数组”，不是直接原地输出，所以最小接入点是在“复制前”先把过期项清掉，或者在复制时跳过过期项。

### 3.3 哈希后端的遍历方式

`kvs_hash_sort` / `kvs_hash_range` 的路径是：

1. 扫描所有槽位
2. 把 `used == 1` 的节点收集到临时数组
3. 对临时数组排序
4. 输出

这里同样适合在“收集临时数组之前”做一轮过期清理，然后再按现有逻辑输出。

### 3.4 跳表后端的遍历方式

`kvs_skiptable_sort` / `kvs_skiptable_range` 主要是沿 `forward[0]` 顺序扫描，属于链式遍历。

这类遍历最适合做的是：

- 遍历时过滤过期节点
- 如果删除动作容易保持链结构稳定，则可顺带清理

### 3.5 红黑树后端的遍历方式

`kvs_rbtree_sort` / `kvs_rbtree_range` 使用递归中序遍历。

这类遍历不适合在递归过程中原地删除节点，因为删除会改变树结构，增加实现复杂度，也容易让最小变更变成框架重写。这里更合适的做法是：

- 遍历时判断 `expire_at_ms`
- 过期则跳过输出
- 清理动作保留给精确命令路径或后续专门回收路径

### 3.6 FULLDUMP 的遍历方式

`FULLDUMP` 是“全量导出当前状态”的命令，命令层直接遍历每一种后端。

它的目标不是返回某个范围，而是生成可重放的数据流。因此它必须遵守同样的 TTL 过滤规则；但它也不适合在遍历过程中做复杂删除，因为它本身就在做跨后端导出，原地删除会显著增加风险。

## 4. 过期过滤方案

### 4.1 统一判定规则

沿用现有 TTL 语义：

- `expire_at_ms < 0`：永不过期
- `expire_at_ms <= now_ms`：已过期
- 其他情况：有效

### 4.2 最小侵入接入点

本步不改遍历框架，只在以下位置插入过期判断：

- 数组 / 哈希：在收集临时数组前先做过期清理，随后沿用现有排序与输出逻辑
- 跳表 / 红黑树：在递归或线性遍历的输出点前判断是否过期，过期则跳过
- FULLDUMP：在命令层遍历时直接跳过过期项

### 4.3 为什么不在统一迭代器里重构

当前仓库没有统一的对象层迭代器，四种后端的遍历形态也不同：

- 数组 / 哈希：快照式临时数组
- 跳表：线性链式遍历
- 红黑树：递归中序遍历

如果为了 TTL 再抽一层统一遍历适配器，会把“只是不返回过期 key”扩大成“重做一个抽象层”，不符合本步的最小侵入原则。

## 5. 必要的清理策略

### 5.1 适合顺带清理的接口

- 数组后端的 `SORT / RANGE`
- 哈希后端的 `SORT / RANGE`
- 跳表后端的 `SORT / RANGE`

这些路径要么是槽位扫描，要么是线性链表扫描，顺手清理不会改变遍历框架。

### 5.2 只过滤、不原地删除的接口

- 红黑树后端的 `SORT / RANGE`
- `FULLDUMP`

原因：

- 红黑树递归遍历中原地删除，结构改动和边界处理成本高
- `FULLDUMP` 是跨结构的全量导出，边遍历边删风险更高

### 5.3 清理策略的语义边界

本步的清理策略只保证“尽快删掉已经过期的节点”，不追求一次遍历把所有过期节点都删光。

换句话说：

- 先保证用户看不到过期 key
- 能安全清理的地方顺手清理
- 不安全的地方只做过滤，留给后续精确命令或后台回收兜底

## 6. 修改文件列表

- [src/storage/kvs_array.c](src/storage/kvs_array.c)
- [src/storage/kvs_hash.c](src/storage/kvs_hash.c)
- [src/storage/kvs_skiptable.c](src/storage/kvs_skiptable.c)
- [src/storage/kvs_rbtree.c](src/storage/kvs_rbtree.c)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- [tests/ttl/test_ttl_all_backends.sh](tests/ttl/test_ttl_all_backends.sh)

## 7. 风险点

1. 过期清理放在遍历路径里，会引入额外写操作。
   - 影响：遍历命令的开销略高
   - 约束：只对已过期项执行，属于正确性优先的代价

2. 红黑树不做原地删除。
   - 影响：过期节点可能仍然滞留到精确访问或后续回收
   - 结论：这是可接受的，因为本步的目标是“不再返回过期 key”

3. `FULLDUMP` 只过滤不清理。
   - 影响：全量导出过程中不会主动回收过期节点
   - 结论：这是刻意选择，避免跨结构遍历期间修改容器

4. 测试对时间敏感。
   - 影响：TTL 边界可能受机器负载影响
   - 结论：测试应使用“最终满足条件”的轮询，而不是一次性断言

## 8. 验证方式

### 8.1 编译验证

- 执行 `make`

### 8.2 命令级验证

建议用当前仓库的客户端对每个后端执行以下序列：

```text
SET a 1
SET b 2
SET c 3
EXPIRE b 1
RANGE a c
SORT low 10
FULLDUMP
```

预期：

- `RANGE` 返回 `a` 和 `c`，不返回 `b`
- `SORT` 返回 `a` 和 `c`，不返回 `b`
- `FULLDUMP` 导出的内容中不再包含 `b`

### 8.3 自动化脚本验证

使用 [tests/ttl/test_ttl_all_backends.sh](tests/ttl/test_ttl_all_backends.sh) 跑完整 TTL 矩阵，重点看以下断言：

- 过期后 `RANGE` 不再返回 expired key
- 过期后 `SORT` 不再返回 expired key
- 过期后 `FULLDUMP` 不再包含 expired key

## 9. 本步结论

本步的正确实现方式不是重做遍历框架，而是在现有遍历点加 TTL 过滤，并在不破坏容器结构的地方顺手清理。

最终语义应当是：

- 精确读路径已经会懒删除过期 key
- 遍历 / 范围 / 全量导出路径不会再回传过期 key
- 可安全清理的后端顺手回收，难以安全删除的后端只做过滤

## 10. 下一步建议

下一步建议进入 TTL 的持久化与复制对齐：

1. 把 `expire_at_ms` 写入 RDB
2. 让 AOF 回放保留 TTL 语义
3. 让主从复制同步 TTL 变更

这样 TTL 才能从“内存可见”变成“重启、复制、导出都一致”。