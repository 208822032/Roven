# KVLite 日志组件接入设计方案

## 1. 背景与现状

当前 KVLite 已经具备完整的命令处理、网络、持久化、复制和 TTL 链路，但日志输出仍然是分散式的，主要以 `printf`、`fprintf(stderr, ...)`、`perror(...)` 的方式直接散落在各个模块中。

可以直接观察到的日志出口包括：

- 启动与退出路径： [src/core/kvstore.c](../../src/core/kvstore.c#L220)
- RESP 会话与协议错误： [src/network/net_session.c](../../src/network/net_session.c#L1)、[src/network/resp_protocol.c](../../src/network/resp_protocol.c#L1)
- 网络循环与连接处理： [src/network/ntyco.c](../../src/network/ntyco.c#L200)、[src/network/reactor.c](../../src/network/reactor.c#L192)
- 主从复制与同步： [src/distributed/distributed.c](../../src/distributed/distributed.c#L117)、[src/distributed/sync_backend_tcp.c](../../src/distributed/sync_backend_tcp.c#L199)
- AOF / RDB / 重写： [src/persistence/kvs_aof.c](../../src/persistence/kvs_aof.c#L169)、[src/persistence/kvs_rdb.c](../../src/persistence/kvs_rdb.c#L179)、[src/persistence/kvs_aof_rewrite.c](../../src/persistence/kvs_aof_rewrite.c#L46)
- 基础工具层： [src/utils/netbuf.c](../../src/utils/netbuf.c#L21)

这种写法在功能上能工作，但存在三个明显问题：

1. 输出格式不统一，排障时需要在不同前缀、不同输出流之间切换。
2. 热路径上直接做字符串格式化、I/O 输出，会放大写请求、网络异常和复制抖动的成本。
3. 没有统一的级别控制和模块维度，无法做到“只开需要的日志”。

因此，这里建议接入一层轻量日志组件，把“业务事件记录”和“低成本输出”分离开。

## 2. 设计目标

日志组件的目标不是“把所有 printf 换成更花哨的打印”，而是同时满足以下约束：

- 输出规范化：统一格式、统一级别、统一字段。
- 可控性：支持按模块、按级别、按运行态配置开关。
- 低开销：默认不影响写路径和网络路径的吞吐。
- 可追踪：日志必须能关联角色、线程、连接、命令、复制状态和错误码。
- 可演进：后续可以扩展到文件滚动、采样、远程采集，而不改调用点。

## 3. 总体方案

建议采用“宏封装 + 运行时过滤 + 异步落盘”的三层结构。

### 3.1 宏封装层

业务代码只调用宏，不直接调用 `fprintf`。

推荐接口形态：

- `KVS_LOG_ERROR(...)`
- `KVS_LOG_WARN(...)`
- `KVS_LOG_INFO(...)`
- `KVS_LOG_DEBUG(...)`
- `KVS_LOG_TRACE(...)`

宏层负责：

- 在编译期裁掉低于阈值的日志。
- 在运行时先判断级别，再决定是否进入格式化。
- 自动补齐模块名、文件名、行号、函数名等上下文。

### 3.2 运行时过滤层

组件内部维护两个维度的过滤条件：

- 全局最低级别，例如默认 `INFO` 或 `WARN`。
- 模块级别覆盖，例如网络模块允许 `DEBUG`，存储模块只保留 `WARN`。

这样可以在不改代码的情况下，临时打开某个子系统的调试日志，而不污染整个进程的输出。

### 3.3 异步落盘层

真正写文件的动作放到后台线程中执行，前台线程只负责把格式化后的日志事件写入一个有界缓冲区。

推荐做法：

- 使用有界环形缓冲区或多生产者单消费者队列。
- 前台线程写入时只做轻量拷贝，不做磁盘 I/O。
- 后台线程批量 flush 到文件，必要时做 `writev` 合并。
- 队列满时优先丢弃 `DEBUG` / `TRACE`，保留 `ERROR` / `WARN`。

这条链路的核心原则是：**日志可以降级，业务线程不能被日志拖慢。**

## 4. 日志规范

### 4.1 统一字段

建议每条日志至少包含以下字段：

- `ts`：时间戳，建议毫秒级。
- `level`：`ERROR`、`WARN`、`INFO`、`DEBUG`、`TRACE`。
- `module`：如 `core`、`net`、`persistence`、`dist`、`ttl`。
- `role`：`master` / `slave`。
- `tid`：线程标识或轻量线程号。
- `fd`：可选，连接相关日志使用。
- `op`：可选，命令名或同步操作名。
- `key`：可选，仅在确有价值时输出。
- `err`：错误码或系统错误信息。

### 4.2 推荐输出格式

建议采用单行 kv 风格，而不是 JSON。

示例：

```text
ts=1711699200123 level=INFO module=core role=master tid=12 msg="server started" port=8080 sync_port=9080
ts=1711699200456 level=WARN module=net role=slave tid=18 fd=41 msg="protocol error" reason="first byte is not *"
ts=1711699200789 level=ERROR module=dist role=master tid=7 msg="sync backend broadcast failed" cmd=PEXPIREAT key=user:1 errno=32
```

选择 kv 风格的原因是：

1. 人可读，排障方便。
2. 机器也容易解析，后续接入采集系统成本低。
3. 相比 JSON，字符串拼接和转义成本更低。

### 4.3 规范化边界

不是所有信息都应该进入日志正文。

建议遵循以下原则：

- 高频、无异常、可通过计数器统计的内容，不打印逐条日志。
- key/value 只在错误或调试场景打印，默认要控制长度。
- 网络包内容、AOF 原文、RESP 原始字节串默认不打印。
- 过长字符串必须截断，避免单条日志膨胀。

## 5. 性能保护策略

这是这个方案的重点。日志组件如果设计不好，最容易把写路径和网络路径拖慢。

### 5.1 编译期裁剪

对于线上默认不需要的 `TRACE` 和大部分 `DEBUG`，建议在编译期直接裁掉。

推荐模式：

- `KVS_LOG_MIN_LEVEL` 作为编译期阈值。
- 低于阈值的宏展开为空操作。

这样即使调用点还在，编译后也不会产生格式化成本。

### 5.2 运行时短路

对于未被编译裁掉的日志，先判断级别是否开启，再进入 `vsnprintf`。

这是避免性能损耗的关键点，因为字符串格式化往往比一次分支判断贵得多。

### 5.3 异步写入

前台线程不直接 `fwrite` / `fprintf` 到文件。

原因很直接：

- 文件 I/O 有抖动。
- 锁竞争会放大尾延迟。
- 如果日志线程慢，业务线程不应该跟着慢。

推荐异步线程策略：

- 缓冲区按固定大小批量提交。
- 单次 flush 写入多个记录，减少 syscall 次数。
- 日志线程独立持有文件句柄和滚动逻辑。

### 5.4 高压场景退化策略

当日志队列满时，不能阻塞业务线程无限等待。

建议优先级如下：

1. 保留 `ERROR`、`WARN`。
2. 丢弃 `DEBUG`、`TRACE`。
3. 对重复错误做采样或合并计数。

如果遇到致命错误，例如进程即将退出，可以切换到同步 flush，确保最后一条错误能落盘。

### 5.5 频率控制

对于当前项目里可能出现的高频错误点，例如协议错误、连接关闭、同步失败、缓冲区不足，建议做简单限流：

- 每类事件按时间窗口只打印一次详细日志。
- 其余同类事件累计计数，周期性汇总。

例如可以输出：

```text
module=net event=protocol_error count=128 window_ms=1000
```

这样既保留问题信号，又不会在异常风暴中把磁盘打满。

## 6. 当前项目中的接入点

下面这些位置最适合第一批接入。

### 6.1 启动与配置层

文件： [src/core/kvstore.c](../../src/core/kvstore.c#L220)

建议记录：

- 配置文件加载结果。
- 网络模式、复制模式、AOF/RDB 开关。
- master/slave 角色和监听端口。
- 优雅退出与保存结果。

这部分日志属于低频、强价值，应该保留为 `INFO`。

### 6.2 网络与协议层

文件： [src/network/net_session.c](../../src/network/net_session.c#L1)、[src/network/resp_protocol.c](../../src/network/resp_protocol.c#L1)、[src/network/reactor.c](../../src/network/reactor.c#L192)、[src/network/ntyco.c](../../src/network/ntyco.c#L200)

建议记录：

- 连接建立/关闭。
- RESP 解析错误。
- 发送缓冲区不足。
- 协议非法输入。

这里要特别注意：不要对每个命令逐条打日志，否则吞吐会明显下降。协议层日志只保留异常和必要的连接生命周期信息。

### 6.3 命令执行层

文件： [src/core/kvstore.c](../../src/core/kvstore.c#L35)、[src/core/kvs_operation.c](../../src/core/kvs_operation.c#L1)

建议记录：

- 写命令失败原因。
- TTL 相关命令的关键状态变化。
- 从节点只读拒绝。

这里建议以事件计数为主，不要记录所有成功命令。

### 6.4 复制层

文件： [src/distributed/distributed.c](../../src/distributed/distributed.c#L117)、[src/distributed/sync_backend_tcp.c](../../src/distributed/sync_backend_tcp.c#L199)

建议记录：

- slave 上线/下线。
- master 广播失败。
- 同步后端初始化失败。
- 批量消息发送失败。

复制层是最适合标准化日志字段的地方，因为它天然需要 `role`、`peer`、`op`、`errno` 等上下文。

### 6.5 持久化层

文件： [src/persistence/kvs_aof.c](../../src/persistence/kvs_aof.c#L169)、[src/persistence/kvs_rdb.c](../../src/persistence/kvs_rdb.c#L179)、[src/persistence/kvs_aof_rewrite.c](../../src/persistence/kvs_aof_rewrite.c#L46)

建议记录：

- AOF/RDB 初始化结果。
- 保存/加载成功与失败。
- 重写开始、结束、超时、失败。

持久化日志不宜在循环内高频输出，尤其是重写和加载阶段，要避免每个 key 都打印。

### 6.6 基础工具层

文件： [src/utils/netbuf.c](../../src/utils/netbuf.c#L21)

建议记录：

- 缓冲区容量异常。
- 申请内存失败。

工具层日志应保持最少，主要用于定位异常边界，不承担业务追踪职责。

## 7. 建议的文件结构

如果要真正落代码，建议新增以下文件：

- [include/kvs_log.h](../../include/kvs_log.h)
- [src/utils/kvs_log.c](../../src/utils/kvs_log.c)

必要时再补一个后台线程实现或队列实现：

- [src/utils/kvs_log_writer.c](../../src/utils/kvs_log_writer.c)

职责建议如下：

- `include/kvs_log.h`：对外宏、级别定义、初始化接口。
- `src/utils/kvs_log.c`：过滤、格式化、公共状态管理。
- `src/utils/kvs_log_writer.c`：后台 flush、文件滚动、丢弃计数。

如果希望保持改动最小，也可以先把 writer 合并进一个源文件，后续再拆分。

## 8. 配置建议

建议把日志配置纳入现有配置系统，而不是单独做一个隐藏开关。

可新增配置项：

- `log_level=info|warn|error|debug|trace`
- `log_file=data/kvstore.log`
- `log_async=yes|no`
- `log_rotate_size=104857600`
- `log_drop_debug_when_busy=yes|no`

如果短期只做最小版本，至少保留：

- 级别
- 输出文件
- 是否异步

这样就足够覆盖绝大多数排障和性能调优场景。

## 9. 分阶段落地建议

### 阶段 1：统一出口

先把现有 `printf`、`fprintf`、`perror` 包成统一宏，不改语义，只改出口。

收益：

- 快速统一格式。
- 最小改动风险。
- 方便后续全局替换。

### 阶段 2：接入级别过滤

加入全局和模块级别控制，默认只保留 `INFO/WARN/ERROR`。

收益：

- 可以在运行时开关调试信息。
- 避免无意义的字符串格式化。

### 阶段 3：异步写盘

把文件输出迁移到后台线程，前台只入队。

收益：

- 消除日志 I/O 对请求链路的直接影响。
- 提升尾延迟稳定性。

### 阶段 4：限流与统计

对高频异常做采样和计数汇总。

收益：

- 减少异常风暴下的额外放大。
- 日志更加可读。

## 10. 验证标准

这套日志方案是否合格，建议至少看四个指标：

1. 功能正确：所有原有错误场景仍然能看到关键日志。
2. 格式统一：同类事件输出字段一致，可 grep、可聚合。
3. 性能稳定：默认级别下，写吞吐和延迟不能因为日志有明显退化。
4. 可控退化：高压和异常场景下，日志可以降级，但业务线程不阻塞。

## 11. 结论

结合当前 KVLite 的实现方式，最合适的日志接入方案不是“直接到处替换打印语句”，而是建立一层轻量、可过滤、可异步落盘的统一日志组件。

核心原则可以概括为一句话：

**日志默认不打扰业务，只在必要时提供足够上下文。**

这能同时解决当前代码里日志分散、格式不一、性能开销不可控三个问题，也为后续接入指标、审计和远程采集留出空间。