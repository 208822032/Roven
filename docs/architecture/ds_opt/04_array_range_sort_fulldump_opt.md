# 第 4 步：array range / sort / full dump 路径优化说明

## 1. 本步目标

本步只优化 array 的遍历型路径，不改对外语义，不改 hash / rbtree / skiptable。

目标是进一步降低 array 在 `range / sort / full dump` 场景下的开销，重点减少：

- 整表复制
- 不必要的结构体搬运
- 重复 key/value 长度计算
- 直接可复用元数据未被利用的输出成本

本步延续第 3 步的 side index 和密集主数组基础，只对 array 的遍历输出链路做最小侵入优化。

## 2. 当前遍历型路径分析

### 2.1 sort 路径

当前 `kvs_array_sort` 的主要成本来自：

- `kvs_array_reap_expired` 先做一轮过期清理；
- 再把 `inst->table` 中的有效项复制到临时数组；
- 然后 `qsort`；
- 最后逐条 `snprintf` 拼接输出。

在第 3 步之前，临时数组复制的是完整 `kvs_array_item_t` 结构，包含 key/value 指针和元数据，属于不必要的结构体搬运。

### 2.2 range 路径

`kvs_array_range` 的成本和 sort 类似：

- 先做过期清理；
- 再复制全量候选项；
- 再 `qsort`；
- 然后在排序结果上做 `start_key / end_key` 过滤；
- 最后逐条输出。

这意味着 range 的大部分成本并不在范围过滤，而在候选收集和排序阶段。

### 2.3 full dump 路径

`handle_fulldump` 的 array 分支在 [src/core/kvs_operation.c](src/core/kvs_operation.c) 中直接遍历数组槽位并输出命令序列。

原始实现里有两个可以降低的成本：

- 遍历条件依赖 `global_array.total`，会带来额外的历史兼容含义；
- 组装 `SET` / `PEXPIREAT` 时会通过通用 helper 反复对 key/value 做 `strlen`。

虽然 full dump 本身仍然要遍历有效项，但可以把字符串长度开销收掉。

## 3. 优化方案

### 3.1 sort / range 由“结构体复制”改为“指针候选集”

本步把 `kvs_array_sort` 和 `kvs_array_range` 的临时收集对象从完整 `kvs_array_item_t` 结构改成 `kvs_array_item_t *` 指针数组。

这样做的收益是：

- 不再复制 key/value 指针和元数据到临时结构体；
- qsort 只操作指针数组，临时对象更小；
- 仍然保持原有排序比较逻辑和输出格式，不改变语义。

注意：这不是把 range/sort 改成新的排序规则，只是把候选集的承载方式改轻。

### 3.2 sort / range 输出改成按元数据长度拼装

array 的每个槽位已经在第 3 步补了 `key_len` / `value_len`，因此可以直接用长度进行拼装：

- `key`
- `:`
- `value`
- `\r\n`

这样避免 `snprintf` 对 key/value 做隐式扫描，也避免再次 `strlen`。

### 3.3 full dump 使用长度版命令拼装

full dump 的 array 分支仍然输出相同的 RESP 命令序列：

- `SET key value`
- `PEXPIREAT key expire_at_ms`（当 key 仍有过期时间时）

本步只是把组装 helper 改成长度版，直接使用 `item->key_len` / `item->value_len`，避免对 key/value 的重复 `strlen`。

### 3.4 保留现有语义

本步不引入新的排序规则，不改变 range 的边界规则，也不改变 full dump 的命令内容和 TTL 表达方式。

## 4. 修改文件列表

本步实际修改了以下文件：

- [src/storage/kvs_array.c](src/storage/kvs_array.c)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- [docs/ds_opt/04_array_range_sort_fulldump_opt.md](docs/ds_opt/04_array_range_sort_fulldump_opt.md)

其中：

- [src/storage/kvs_array.c](src/storage/kvs_array.c) 负责 range / sort 的候选集和输出优化；
- [src/core/kvs_operation.c](src/core/kvs_operation.c) 负责 array full dump 的长度版命令拼装。

## 5. 语义保持说明

本步只改实现，不改对外语义。

### 5.1 sort 语义保持

- 仍然按 key 字典序排序；
- 仍然支持 `order` 正反序；
- 仍然支持 `limit`；
- 仍然在输出前过滤过期项。

### 5.2 range 语义保持

- 仍然按 key 字典序输出；
- 仍然使用 `start_key / end_key` 边界过滤；
- 仍然在输出前过滤过期项；
- 仍然返回相同格式的多行文本。

### 5.3 full dump 语义保持

- 仍然输出 `SET` / `PEXPIREAT` 命令序列；
- 仍然跳过过期项；
- 仍然可被现有回放逻辑识别；
- 只是拼装时利用了已知长度元数据。

### 5.4 不能改变的部分

- 不能把 rbtree / skiptable 改成按 hash 排序；
- 不能改变 range / sort 的 key 字典序语义；
- 不能在本步引入新的输出格式。

## 6. 风险点

- 指针候选集虽然比结构体复制轻，但仍然需要 qsort，因此 range / sort 的算法上界没有改变。
- 输出拼装改成长度版后，需要确保 `key_len` / `value_len` 始终和实际字符串一致；否则会出现输出截断或越界风险。
- full dump 仍依赖数组遍历，如果后续再次引入稀疏槽位机制，遍历逻辑要同步调整。
- 本步没有改 hash / rbtree / skiptable，因此跨结构性能差异仍然存在。

## 7. 验证方式

本步使用现有测试做语义回归，重点确认：

- range 结果内容不变；
- sort 排序顺序不变；
- full dump 生成与加载不变；
- TTL 相关行为不受影响。

建议验证命令：

- `make -j2 kvstore`
- `bash -n tests/test_ttl_mvp.sh && ./tests/test_ttl_mvp.sh`
- `bash -n tests/test_ttl_aof.sh && ./tests/test_ttl_aof.sh`

## 8. 本步结论

本步把 array 的遍历型路径进一步收轻：

- sort / range 的临时候选集从“复制完整结构体”改成了“指针数组”；
- full dump 的 array 分支改成了长度版拼装，减少了 `strlen`；
- 对外输出格式和命令语义保持不变。

这属于最小侵入优化，收益集中在减少不必要的拷贝和扫描，而不是改变算法模型。

## 9. 下一步建议

如果下一步继续推进 array，建议优先做两件事：

1. 给 range / sort 再补一层一致性校验和更明确的辅助函数，降低后续维护复杂度；
2. 如果要进一步提速，再考虑更激进的候选收集策略，但前提是仍然不改变 key 字典序语义。

目前不建议把这套优化扩展到 rbtree / skiptable，更不建议把它们改成按 hash 排序。
