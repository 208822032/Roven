# KVLite 阶段 1：统一日志出口

## 1. 本阶段目标

本阶段只做“统一出口”，不做异步队列、不做日志滚动、不做模块级动态采样。

目标是先把当前项目里分散的 `printf`、`fprintf(stderr, ...)`、`perror(...)` 收口到一套最小日志接口上，保证：

- 输出格式统一。
- 关键错误路径可追踪。
- 不改变原有业务语义和控制流。
- 默认仍然输出到 `stderr`，便于启动即见。

## 2. 已实现内容

### 2.1 最小日志骨架

新增文件：

- [include/kvs_log.h](../../include/kvs_log.h)
- [src/utils/kvs_log.c](../../src/utils/kvs_log.c)

提供的基础能力：

- 日志级别：`ERROR / WARN / INFO / DEBUG`
- 统一宏：`KVS_LOG_ERROR(...)`、`KVS_LOG_WARN(...)`、`KVS_LOG_INFO(...)`、`KVS_LOG_DEBUG(...)`
- 统一 errno 输出：`KVS_LOG_ERROR_ERRNO(...)`
- 默认输出到 `stderr`
- 也支持通过 `kvs_log_init(path, level)` 切换到文件输出

### 2.2 统一格式

每条日志统一输出：

- 时间戳
- 级别
- 模块名
- 文件名与行号
- 正文

当前格式示例：

```text
ts=2026-03-29 12:00:00.123 level=INFO module=core file=kvstore.c:289 msg="Starting as MASTER on port 9300, sync port 10300"
```

### 2.3 首批替换范围

本阶段优先替换以下低频但关键的错误和启动出口：

- 启动与退出路径： [src/core/kvstore.c](../../src/core/kvstore.c)
- 网络/协议错误： [src/network/net_session.c](../../src/network/net_session.c)、[src/network/resp_protocol.c](../../src/network/resp_protocol.c)、[src/network/reactor.c](../../src/network/reactor.c)
- 主从复制错误： [src/distributed/distributed.c](../../src/distributed/distributed.c)、[src/distributed/sync_backend_tcp.c](../../src/distributed/sync_backend_tcp.c)
- AOF / RDB 初始化与失败： [src/persistence/kvs_aof.c](../../src/persistence/kvs_aof.c)、[src/persistence/kvs_rdb.c](../../src/persistence/kvs_rdb.c)

## 3. 设计取舍

### 3.1 为什么先做 stderr

当前阶段只需要尽快把日志收口并验证格式，因此默认继续输出到 `stderr`。

这样做的好处是：

- 不依赖额外配置。
- 不影响现有启动方式。
- 排障时能立即看到日志。

文件输出已经在接口层预留，但没有在配置系统里强制接入。

### 3.2 为什么不做异步

异步队列、滚动、采样会引入更多状态和边界处理，不符合本阶段“最小可用”的目标。

先把统一出口做对，再考虑性能优化，是更稳妥的分层方式。

### 3.3 为什么先替换关键错误点

当前项目中真正影响排障和对外可观测性的，是启动、协议错误、复制失败、持久化失败这些低频关键点，而不是每条成功命令。

因此本阶段只替换最有价值的出口，避免热路径被日志侵入。

## 4. 已替换的旧日志点

### 4.1 启动与退出

文件： [src/core/kvstore.c](../../src/core/kvstore.c)

- 配置文件加载失败警告
- AOF 初始化失败
- slave 模式缺少 master endpoint
- master/slave 启动信息
- 禁用 TCP 分布式模式的信息
- TTL 后台线程启动失败警告
- 启动网络监听信息
- 退出保存信息

### 4.2 网络与协议错误

文件： [src/network/net_session.c](../../src/network/net_session.c)、[src/network/resp_protocol.c](../../src/network/resp_protocol.c)、[src/network/reactor.c](../../src/network/reactor.c)

- RESP 协议首字节非法
- resp_peek_command / resp_parser 错误
- response buffer 申请失败
- send/recv/accept/bind/listen/epoll 错误
- 网络缓冲区扩容失败

### 4.3 复制关键错误

文件： [src/distributed/distributed.c](../../src/distributed/distributed.c)、[src/distributed/sync_backend_tcp.c](../../src/distributed/sync_backend_tcp.c)

- slave 连接建立/断开信息
- master sync server 监听错误
- slave 列表管理错误
- 复制连接接收/解码/分发错误
- TCP sync backend 队列和广播失败

### 4.4 AOF / RDB 初始化与失败

文件： [src/persistence/kvs_aof.c](../../src/persistence/kvs_aof.c)、[src/persistence/kvs_rdb.c](../../src/persistence/kvs_rdb.c)

- AOF sync mode 非法
- AOF 相关内存、文件、锁、线程失败
- AOF 直接写入失败、fsync 失败、加载失败
- RDB 文件缺失、打开失败、读写失败、格式错误、版本错误
- RDB 保存/加载成功信息统一为日志输出

## 5. 如何验证

### 5.1 编译验证

执行：

```bash
make kvstore
```

如果编译成功，说明日志组件已经参与主程序构建。

### 5.2 启动验证

直接启动服务，应该能看到统一格式的 `INFO` 日志，例如启动模式、端口、退出保存信息。

### 5.3 错误场景验证

建议至少验证以下 3 类场景：

1. 使用不存在的配置文件路径，观察配置加载警告。
2. 使用 slave 配置但不提供 master endpoint，观察角色检查错误。
3. 使用不可写的 AOF 路径，观察 AOF 初始化失败日志。

这些场景都应该输出统一格式日志，且不改变原有返回值和退出逻辑。

## 6. 当前遗留问题

### 6.1 还有少量旧输出

仓库中仍然存在一些非关键成功路径输出和注释掉的调试打印，本阶段没有全部替换。

原因是本阶段只追求“统一出口的最小闭环”，不追求一次性清零所有历史输出。

### 6.2 还没有配置化日志级别

当前日志级别默认在代码里初始化，尚未接入配置文件。

这属于下一阶段可以补的能力。

### 6.3 还没有异步写盘

当前日志仍然直接写到 `stderr` 或文件流，不做队列和后台 flush。

这一步是后续性能优化的重点，但不属于本阶段范围。

## 7. 下一步建议

1. 把剩余零散的 `printf` / `fprintf` 继续逐步迁移到统一日志接口。
2. 增加配置项支持日志级别和日志文件路径。
3. 引入异步写盘和丢弃策略，减少高频错误场景下的日志开销。
