# 第 6 步：AOF 接入 TTL

## 1. 本步目标

本步要让 AOF 在追加、重放和重写之后，都能正确恢复 TTL 语义。这里的核心不是单纯把 TTL 命令写进日志，而是让 AOF 里的 TTL 记录在“重放时间点”仍然保持绝对时间语义，避免相对 TTL 在恢复时漂移。

本步采用的原则是：优先复用当前 AOF 的 RESP 命令流表达方式，不单独发明新的复杂日志格式。

在实现上，AOF 仍然保留自动 rewrite 机制；为了让 TTL 回归更稳定，本阶段额外补了一个显式的管理命令 `AOFREWRITE`，用于在测试里直接触发重写路径并验证重写后的 AOF 内容。

## 2. 当前 AOF 链路分析

### 2.1 追加链路

当前写命令在 [src/core/kvstore.c](src/core/kvstore.c) 的统一出口中执行：命令先走 `lookup_command`、`parse_command_argument` 和具体 handler，成功后再进入 AOF 追加与主从同步。

AOF 追加实现位于 [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c)。它本质上就是把命令按 RESP 数组格式原样写入 AOF 文件：

- `*<argc>`
- `$<len>`
- `<arg>`

因此，AOF 当前最强的表达能力就是“记录命令本身”，而不是记录一套独立的 TTL 元数据结构。

### 2.2 回放链路

AOF 回放同样在 [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c) 中完成。回放时会逐条解析 RESP 命令，然后再次进入命令层执行。

这意味着：

- 只要命令表能识别某个 TTL 命令，AOF 就能回放它。
- 如果 TTL 记录仍然用相对秒数表达，那么回放时会受到“重放时间偏移”影响。

### 2.3 重写链路

AOF 重写在 [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c) 中完成。当前实现的重写逻辑本质上是把当前内存里的有效数据重新导出为命令流。

这条链路如果只导出 `SET/RSET/HSET/SSET`，而不把 TTL 一并导出，那么重写完成后，TTL 一定会丢失。

所以 AOF 重写必须把 TTL 作为当前状态的一部分写回去，而不是只写 value。当前实现里，TTL 会在重写时以 `PEXPIREAT` 的形式重新写回，过期项则不应进入重写后的文件。

### 2.4 显式 rewrite 触发

当前代码保留了基于阈值的自动 rewrite，同时新增了一个显式命令 `AOFREWRITE`。测试脚本会先准备 TTL 场景，再直接执行 `AOFREWRITE`，然后检查：

- rewrite 后 AOF 中保留 `PEXPIREAT`
- rewrite 后 AOF 中不残留原始 `EXPIRE`
- 已过期 key 不会写回重写后的 AOF
- 重启后 TTL 语义仍然正确

这样可以把“是否触发 rewrite”与“rewrite 语义是否正确”分开验证，避免只靠文件增长阈值推断结果。

## 3. TTL 相关命令的 AOF 记录策略

### 3.1 记录原则

TTL 相关命令不要直接以相对秒数长期保存到 AOF。对于 `EXPIRE`，应该在追加时把它规范化成绝对时间记录；对于 `PERSIST`，可以继续按原命令记录，因为它本身表达的就是“清除过期属性”。

### 3.2 追加策略

推荐策略如下：

- `EXPIRE key seconds`：追加时转换为 `PEXPIREAT key expire_at_ms`
- `PERSIST key`：直接写入 `PERSIST key`
- `SET/RSET/HSET/SSET`：保持原样，但由于覆盖写会清 TTL，所以 AOF 里只需要记录新的写命令即可

这里的 `PEXPIREAT` 不是额外设计一套复杂协议，而是用现有 RESP 命令流增加一个内部 canonical 形式。

### 3.3 为什么用 `PEXPIREAT`

因为 AOF 重放发生时，系统时钟已经不是写入时刻。若继续记录 `EXPIRE key 60`，那么回放时会从“重放发生的时刻”重新计算 60 秒，TTL 会发生偏移。

记录绝对时间 `PEXPIREAT key <ms>` 可以避免这个问题：

- 写入时把过期点固定下来
- 重放时直接按绝对时间判断是否仍有效
- 如果已经过期，则回放时直接删除或跳过

### 3.4 `PERSIST` 的表达方式

`PERSIST` 不需要转换成别的格式。它的语义就是“清除 TTL”，重放时再次执行 `PERSIST` 就能恢复一致状态。

## 4. 覆盖写清 TTL 的处理策略

### 4.1 当前语义

当前内存层已经实现了覆盖写清 TTL：

- `SET` 成功覆盖时，`expire_at_ms = -1`
- `MOD` 成功覆盖时，`expire_at_ms = -1`
- `DEL` 删除时，TTL 元数据一起销毁

这意味着只要 AOF 里记录的是覆盖写本身，重放后 TTL 就会自然清掉，不需要额外记录一个“清 TTL”命令。

### 4.2 AOF 中的体现

覆盖写清 TTL 在 AOF 中的体现就是：

- 旧的 `EXPIRE` 不需要再额外保留一个“取消 TTL”的日志
- 只要后续写命令到达，AOF 重放时会再次执行覆盖写逻辑
- 由于 `SET/MOD` 本身会把 `expire_at_ms` 重置为 `-1`，TTL 会自动被清除

所以这里的关键不是“多写一条 TTL 清除日志”，而是“确保写命令本身在 AOF 中完整保留”。

## 5. AOF 重放行为说明

### 5.1 绝对时间重放

本步新增的 canonical 形式是 `PEXPIREAT`。重放时应这样处理：

- 如果 `expire_at_ms > now_ms`，恢复 TTL
- 如果 `expire_at_ms <= now_ms`，说明这个记录在重放时已经过期，应该直接删除或跳过

这样可以把重放时间偏移控制住，避免过期 key 被错误恢复。

### 5.2 `SET` 覆盖后的重放

对于 `SET` 覆盖清 TTL 的场景，AOF 重放只要按原写命令执行即可。因为 `SET` 在存储层会把 TTL 置为 `-1`，所以重放结果天然正确。

### 5.3 已过期记录的重放

如果 AOF 中出现一条 `PEXPIREAT`，而它在重放时已经过期，那么它不应该把一个失效 key 再恢复出来。正确行为是：

- 不恢复该 key
- 或者在回放时立即删掉它

本步采用的策略是“回放时直接丢弃/删除过期项”，这样更符合“恢复当前有效状态”的目标。

### 5.4 重放时间偏移问题

这是本步必须明确说明的风险点：

- 相对 TTL 在写入后会随着时间流逝而变化
- 绝对 TTL 在 AOF 中如果不做规范化，也会在重放时发生偏移

解决办法是：

- 追加时把 `EXPIRE` 规范化成 `PEXPIREAT`
- 回放时按 `expire_at_ms` 和当前时间比较
- AOF 重写时重新输出当前有效的绝对过期点

## 6. 修改文件列表

本步实际需要关注的文件是：

- [include/kvs_types.h](include/kvs_types.h)
- [include/kvs_command.h](include/kvs_command.h)
- [include/kvstore.h](include/kvstore.h)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- [src/core/kvstore.c](src/core/kvstore.c)
- [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)

如果后续再扩展主从复制，再补 [src/distributed/distributed.c](src/distributed/distributed.c) 和 [include/master_slave_proto.h](include/master_slave_proto.h) 即可。

## 7. 风险点

1. 旧 AOF 文件如果只记录了相对 `EXPIRE`，仍然会按旧语义回放。
- 处理方式：重写后生成的 AOF 会转成 `PEXPIREAT`，新文件不再依赖相对时间。

2. `PEXPIREAT` 是内部 canonical 形式，虽然可以被命令表识别，但并不建议作为对外 API 重点宣传。
- 处理方式：文档和测试都以 `EXPIRE/PERSIST/SET` 为主，`PEXPIREAT` 只作为 AOF 内部记录形式。

3. AOF 重写阶段如果不带 TTL，会导致重写后 TTL 丢失。
- 处理方式：重写时对每条有效记录补写 `PEXPIREAT`。

4. 重放时如果已经过期，必须明确删除或跳过。
- 处理方式：在 `PEXPIREAT` handler 里对过期时间做实时判断。

## 8. 验证方式

### 8.1 编译验证

先执行构建，确认新增的 `PEXPIREAT` 分支、`AOFREWRITE` 命令、AOF canonical 化和重写输出都能编译通过。

### 8.2 AOF replay 测试序列

建议按下面的序列验证：

1. 启动服务并开启 AOF。
2. 写入一个永久 key：`SET keep v1`
3. 写入一个长 TTL key：`SET live v2`，然后 `EXPIRE live 60`
4. 写入一个会被覆盖清 TTL 的 key：`SET overwrite v3`，`EXPIRE overwrite 60`，再 `SET overwrite v4`
5. 写入一个会在重放前过期的 key：`SET soon v5`，`EXPIRE soon 1`
6. 执行 `PERSIST live`，确认 TTL 能被清掉。
7. 直接执行 `AOFREWRITE`，让服务端进入重写路径。
8. 检查重写后的 AOF 文件中，`EXPIRE live 60` 已被规范化为 `PEXPIREAT`，过期项没有被写回。
9. 重启服务并验证：
   - `keep` 仍存在
   - `live` 仍存在且 TTL 仍为永不过期或已按 `PERSIST` 清除
   - `overwrite` 的 TTL 已被覆盖写清掉
   - `soon` 不应被恢复

对应的自动化回归脚本是 [tests/test_ttl_aof.sh](tests/test_ttl_aof.sh)。

### 8.3 断言点

重点断言以下结果：

- `EXPIRE` 的 AOF 记录不再依赖重放时的相对时间，rewrite 后应看到 `PEXPIREAT`
- `SET` 覆盖后 TTL 仍然清除
- `PERSIST` 重放后 TTL 仍然清除
- 已过期 key 不会在重启后复活

## 9. 本步结论

AOF 接入 TTL 的最小正确方案是：

- 运行时用绝对过期点表达 TTL
- AOF 追加时把 `EXPIRE` 规范化为 `PEXPIREAT`
- AOF 重放时按绝对时间判断是否仍有效
- AOF 重写时把当前 TTL 一并写回，并剔除已过期项
- `SET` 覆盖清 TTL 的行为继续由存储层负责，AOF 只要记录真实写命令即可
- `AOFREWRITE` 作为测试用的显式触发入口，帮助稳定验证 rewrite 语义

这套方案不需要引入额外的 TTL 日志格式，也不需要单独做一套 expires 结构。

## 10. 下一步建议

如果后续继续扩展，建议优先做两件事：

1. 把 `PEXPIREAT` 的同步语义补到主从复制里。
2. 给 AOF / RDB / 复制三条链路补一组统一的 TTL 回放回归。

这样 TTL 才能从“内存态正确”扩展成“恢复链路完整正确”。
