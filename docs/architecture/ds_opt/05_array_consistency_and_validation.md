# 第 5 步：array 一致性校验与验证

## 1. 本步目标

本步只针对 array 做内部一致性校验与验证，不改对外语义，不改 hash / rbtree / skiptable。

目标是把前几步已经落地的 array 结构优化再收一层：

- 收敛 `range / sort` 的公共辅助逻辑，降低后续维护复杂度；
- 增加 side index、尾覆盖删除、TTL 清理后的内部一致性校验；
- 复用现有测试验证语义正确；
- 基于现有压测程序补一轮 array 与 rbtree / skiptable 的性能对比，确认当前优化收益。

## 2. 当前风险点

### 2.1 side index 与 dense array 双写风险

array 现在是“主数组 + side index”双层结构，任何 insert、overwrite、delete、TTL 清理、扩容、rehash 都可能出现双写不一致。如果不做一致性校验，问题可能不会立刻暴露，但会在 GET、MOD、DEL、TTL、RANGE、SORT 这些路径上以错误索引、脏槽位或错误删除的方式出现。

### 2.2 尾覆盖删除风险

尾覆盖删除把物理顺序变成非稳定顺序，这是预期行为，但前提是所有依赖 dense_idx 的地方都能正确修复索引。风险点在于：

- 被搬到删除位置的尾元素必须同步更新 side index；
- 删除项对应的索引必须变成墓碑或被清理；
- TTL 清理如果仍然走按 key 二次查找，会重新引入 O(n) 成本。

### 2.3 range / sort 输出链路风险

`range / sort` 在前几步已经从“完整结构体复制”收敛为“指针候选集”，这降低了临时对象搬运成本，但前提是：

- 候选集排序必须仍然按 key 字典序；
- 输出必须仍然使用 `key_len / value_len` 的已知长度；
- 不能把 rbtree / skiptable 的有序语义改成 hash 语义。

### 2.4 性能验证风险

性能比较如果使用了不对应的命令或不对齐的数据量，很容易把网络输出、命令语义差异、响应大小差异混进结果里，最终看不到 array 优化的真实收益。因此本步只做最小扩展，复用现有测试程序和已有工作负载。

## 3. 公共辅助逻辑收敛方案

本步把 array 的 `range / sort` 收敛成统一的候选收集与输出路径：

1. 先做 TTL 过期清理；
2. 再把有效项收集成指针数组；
3. 对指针数组做字典序排序；
4. `sort` 直接做顺序/逆序/limit 输出；
5. `range` 在同一份排序结果上做区间过滤。

这样做的好处是：

- 排序前的候选收集只保留一份；
- `sort` 与 `range` 不再各自维护一套重复逻辑；
- 后续如果要再做输出优化，只需要改一个公共出口。

本步不改变排序规则，也不改变 `range / sort` 的返回格式。

## 4. side index 一致性校验方案

本步在 array 内部增加了可选的一致性校验函数，核心检查项是：

- `len`、`capacity`、`side_count` 的数量关系是否合理；
- dense array 中 `[0, len)` 的每个槽位是否都有合法 `key/value`；
- 每个槽位的 `key_len / value_len / key_hash` 是否与真实字符串一致；
- `key -> dense_idx` 的 side index 映射是否能回查到同一个槽位；
- 每个 `USED` 的 side index 条目是否指向合法的 `dense_idx`，并且其 `key` 指针和主数组槽位一致。

校验挂载点如下：

- `SET` 成功插入后；
- `MOD` 覆盖后；
- `DEL` / `DEL_AT` 尾覆盖删除后；
- `TTL` 清理完成后。

实现上用编译期开关控制，默认不影响线上性能；需要验证时可通过 `EXTRA_CFLAGS` 打开。例如：

- `EXTRA_CFLAGS=-DKVS_ARRAY_VERIFY_INTEGRITY=1`

## 5. 正确性验证方案

本步优先复用已有测试，不引入新的大规模测试框架。

### 5.1 语义回归

继续沿用现有 TTL / AOF 用例，重点确认：

- array 的 GET / MOD / DEL / EXISTS 语义不变；
- TTL lazy delete 与后台清理仍然正确；
- RDB / AOF 回放仍然可恢复原状态；
- range / sort / fulldump 输出格式保持一致。

建议回归命令：

- `make -j2 kvstore`
- `bash -n tests/test_ttl_mvp.sh && ./tests/test_ttl_mvp.sh`
- `bash -n tests/test_ttl_aof.sh && ./tests/test_ttl_aof.sh`

### 5.2 一致性校验

在打开 `KVS_ARRAY_VERIFY_INTEGRITY` 后，重复执行：

- 写入、覆盖、删除；
- TTL 触发与后台扫描；
- `RANGE / SORT`；
- `FULLDUMP / AOF`。

如果内部索引、元数据或 dense slot 有失配，校验会直接暴露。

### 5.3 最小 benchmark 验证

使用现有 `tests/testcase.c` 的测试程序，新增的 compare 模式复用同一套 RESP 客户端和数据加载逻辑，避免重写 benchmark 框架。比较对象是：

- array
- rbtree
- skiptable

比较项是：

- full `RANGE`
- `SORT low 20`

这样能直接观察 array 的候选收集和输出收敛后，是否比树/跳表更轻。

## 6. 性能对比方案

本步不引入 Redis 对照组，也不重建新的压测工具，只做当前项目内的横向比较。

### 6.1 负载设计

- 统一预热 10000 条数据；
- 每种结构分别写入各自前缀的数据集；
- 对每种结构执行同样的 `RANGE` 和 `SORT` 读路径；
- 使用同一份测试程序和同一台服务器，避免环境噪声。

### 6.2 比较维度

关注两类指标：

- 单次命令耗时：`time_used`；
- 响应长度：`resp_len`。

`resp_len` 主要用于确认命令实际返回量是否对齐，`time_used` 才是性能结论的核心。

### 6.3 结果解读

- 如果 array 在 `RANGE / SORT` 上更快，说明候选收集和输出收敛有效；
- 如果 rbtree / skiptable 接近或更快，这是正常的，因为它们本身保留了有序结构优势；
- 若 array 性能没有明显改善，需要优先检查：
  - 排序前候选收集是否仍在做多余拷贝；
  - TTL 清理是否引入了额外扫描；
  - 输出是否还在反复扫描字符串长度。

## 7. 修改文件列表

本步实际修改了以下文件：

- [src/storage/kvs_array.c](src/storage/kvs_array.c)
- [tests/testcase.c](tests/testcase.c)
- [docs/ds_opt/05_array_consistency_and_validation.md](docs/ds_opt/05_array_consistency_and_validation.md)

其中：

- [src/storage/kvs_array.c](src/storage/kvs_array.c) 负责公共辅助逻辑收敛与一致性校验；
- [tests/testcase.c](tests/testcase.c) 负责最小 benchmark 扩展；
- [docs/ds_opt/05_array_consistency_and_validation.md](docs/ds_opt/05_array_consistency_and_validation.md) 负责本步分析与验证归档。

## 8. 风险点

- 一致性校验如果默认开启，会拖慢写路径，所以默认应保持可选。
- 校验函数如果只检查部分字段，可能漏掉 side index 与 dense array 的某些失配情况，因此必须同时检查元数据、索引、密集槽位三个层面。
- 性能比较如果只看单次结果，可能受抖动影响；需要尽量用同一工作负载、多次运行观察趋势。
- range/sort 的公共 helper 如果后续继续演进，要避免把“候选收集”与“输出格式”重新耦合回去。

## 9. 本步结论

array 的第一阶段优化已经进入“结构稳定 + 一致性校验 + 验证归档”阶段。

- `range / sort` 的公共逻辑已经收敛；
- side index、尾覆盖删除、TTL 清理可以在内部校验下保持一致；
- 现有 TTL / AOF 测试足以验证语义正确；
- 新增的 benchmark 入口可以直接比较 array 与 rbtree / skiptable 的 range 与 sort 表现。

## 10. 下一步建议

如果继续推进 array，建议优先做两件事：

1. 把 `FULLDUMP` 的 array 分支也纳入同一套长度元数据校验与输出收敛思路，继续削减重复扫描。
2. 如果一致性校验在实际压测里暴露出问题，再考虑把校验拆成更细的 debug 开关，减少验证成本。

如果后续要继续比较性能，建议继续保留当前的 `tests/testcase.c` 入口，不要另起新的 benchmark 框架。
