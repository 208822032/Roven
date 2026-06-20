# 第 1 步：仓库扫描 + TTL 实现前确认

## 1. 项目结构概览

KVLite 当前仓库已经把核心能力拆成了比较清晰的几条主线：

- 核心命令解析与分发：`src/core/kvstore.c`、`src/core/kvs_operation.c`、`include/kvs_command.h`、`include/kvs_operations.h`
- 四类数据结构：`src/storage/kvs_array.c`、`src/storage/kvs_rbtree.c`、`src/storage/kvs_hash.c`、`src/storage/kvs_skiptable.c`
- 持久化：`src/persistence/kvs_rdb.c`、`src/persistence/kvs_aof.c`、`src/persistence/kvs_aof_rewrite.c`
- 主从复制：`src/distributed/distributed.c`、`src/distributed/sync_backend_tcp.c`、`src/utils/master_slave_proto.c`
- 网络接入层：`src/network/net_session.c`、`src/network/resp_protocol.c`、`src/network/reactor.c`、`src/network/proactor.c`、`src/network/ntyco.c`、`src/network/ntyco_defult.c`

从仓库现状看，命令层、持久化层、复制层都已经有完整的主入口，但没有统一的“对象层”来承载 TTL 元数据。这意味着 TTL MVP 更适合按“各个底层数据结构分别挂字段 + 命令层统一判过期”的方式推进，而不是只在某一层做一个局部补丁。

## 2. TTL 相关关键数据结构定位

### 2.1 当前最接近 TTL 载体的结构体

当前四个存储后端的节点结构体都只有 key/value 语义，没有任何过期时间字段：

- 数组项：`include/kvs_array.h` 中的 `kvs_array_item_t`
- 红黑树节点：`include/kvs_rbtree.h` 中的 `_rbtree_node`
- 哈希节点：`include/kvs_hash.h` 中的 `hashnode_t`
- 跳表节点：`include/kvs_skiptable.h` 中的 `Node`

这些结构体在实现文件中被直接作为存储本体使用：

- `src/storage/kvs_array.c` 里 `global_array` 的元素是 `kvs_array_item_t`
- `src/storage/kvs_rbtree.c` 里 `rbtree_node` 直接保存 `key` 和 `value`
- `src/storage/kvs_hash.c` 里 `hashnode_t` 保存 `key`、`value`、`klen`、`vlen`、`used`
- `src/storage/kvs_skiptable.c` 里 `Node` 保存 `key`、`value`、`forward[]`

### 2.2 是否存在统一挂载点

当前仓库没有看到一个统一的 KV 元对象，例如 `struct kv_entry { key, value, expire_at_ms }` 再被四个后端共同引用。命令入口是按数据结构分发的，而不是通过统一对象层分发。因此：

- 如果 TTL 要覆盖所有命令路径，最终必须改四个节点结构体，而不是只改一个公共头文件。
- 如果只想先做最小 MVP，`kvs_array_item_t` 是最简单的试点结构，因为它最扁平，改动面最小；但它不能代表全局 TTL 语义。

### 2.3 当前不适合统一挂 `expire_at_ms` 的原因

原因不是字段本身难加，而是当前代码架构里没有统一节点抽象：

- `SET / GET / DEL / EXISTS / RANGE` 都是通过 `command_table` 分发到不同后端
- RDB/AOF/复制链路也都是按后端分别遍历或编码，不是通过统一对象层遍历
- 因此只在某一个后端结构里加 `expire_at_ms` 会造成行为不一致

结论：TTL 语义要一致，不能只挂在某一个结构体上；MVP 也应该接受“四个底层节点都加同一字段”的方向。

## 3. TTL 相关命令链路定位

### 3.1 命令主入口

命令主入口在 `src/core/kvstore.c` 的 `kvs_filter_protocol`（见 [src/core/kvstore.c](src/core/kvstore.c#L43)）。它做了：

- 通过 `lookup_command(tokens[0])` 找命令
- 用 `check_arity` 校验参数个数
- 用 `parse_command_argument` 解析上下文
- 调用 `cmd->handler(&ctx, response)` 执行命令
- 写命令成功后追加 AOF，并在 master 上广播复制

这意味着 TTL MVP 的命令级处理点应放在这里的下游：

- 新增 `EXPIRE / TTL / PERSIST` 等命令，需要先进入命令表
- 对 `GET / EXISTS / RANGE / SORT` 这类读命令，需要在具体 handler 或底层数据结构访问前做过期检查
- 对 `SET` 和各类写命令，需要决定是否清除旧 TTL 或是否保留 TTL

### 3.2 命令表与处理函数

命令表在 `src/core/kvs_operation.c`（见 [src/core/kvs_operation.c](src/core/kvs_operation.c#L35)）。当前已经覆盖：

- 数组后端：`SET / GET / DEL / MOD / EXIST / RANGE / SORT`
- 红黑树后端：`RSET / RGET / RDEL / RMOD / REXIST / RRANGE / RSORT`
- 哈希后端：`HSET / HGET / HDEL / HMOD / HEXIST / HRANGE / HSORT`
- 跳表后端：`SSET / SGET / SDEL / SMOD / SEXIST / SRANGE / SSORT`
- 额外命令：`PAPERSET / PAPERGET / PAPERDEL / PAPERMOD / QUIT / FULLDUMP`

关键处理函数位置如下：

- `handle_set`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L295)
- `handle_get`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L341)
- `handle_del`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L380)
- `handle_mod`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L416)
- `handle_exist`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L460)
- `handle_range`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L494)
- `handle_sort`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L540)
- `handle_fulldump`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L647)

### 3.3 当前未发现的 TTL 命令入口

当前 `command_table` 和 `kvs_operation.c` 中没有以下 TTL 相关命令：

- `EXPIRE`
- `PEXPIRE`
- `TTL`
- `PTTL`
- `PERSIST`
- `SETEX`

也没有看到任何过期检查逻辑、过期删除逻辑或时间驱动的扫描逻辑。

## 4. RDB 链路定位

### 4.1 主入口

RDB 对外接口定义在 `include/kvs_rdb.h`：

- `kvs_rdb_save(const char *filename)`
- `kvs_rdb_load(const char *filename)`

实现位于 `src/persistence/kvs_rdb.c`：

- 保存入口：[src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L46)
- 加载入口：[src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L130)

在启动流程里，`src/core/kvstore.c` 的 `main` 先调用 `kvs_rdb_load(g_cfg.rdb_filename)`，再初始化 AOF，再进入分布式和网络服务。

### 4.2 当前 RDB 格式特征

从实现看，RDB 文件格式当前包含：

- magic：`RDB_MAGIC`
- version：`RDB_VERSION`
- snapshot timestamp：`time_t timestamp`
- 逐个数据结构块：array / rbtree / hash / skiptable
- 结束标记：`0xFF`

每个数据结构块目前只保存 key 和 value：

- array：`rdb_save_array` / `rdb_load_array`，见 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L247) 和 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L267)
- rbtree：`rdb_save_rbtree` / `rdb_load_rbtree`，见 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L348) 和 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L369)
- hash：`rdb_save_hash` / `rdb_load_hash`，见 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L393) 和 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L432)
- skiptable：`rdb_save_skiptable` / `rdb_load_skiptable`，见 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L456) 和 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L480)

### 4.3 TTL 对 RDB 的影响

当前 RDB 没有保存任何过期时间字段，也没有在加载时做过期过滤。也就是说：

- 如果 TTL 数据被写入内存，但 RDB 不扩展格式，重启后 TTL 会丢失
- 如果只在加载后做清理，会导致快照中已经过期的数据被短暂恢复，语义不完整

因此 TTL MVP 至少需要让 RDB 增加每条记录的 `expire_at_ms` 编码与解码逻辑；同时建议把文件版本号升级，避免旧文件解析错位。

## 5. AOF 链路定位

### 5.1 主入口

AOF 对外接口定义在 `include/kvs_aof.h`，实现位于 `src/persistence/kvs_aof.c`：

- `aof_init`：[src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L37)
- `aof_append`：[src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L179)
- `aof_fsync`：[src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L331)
- `aof_load`：[src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L371)
- `aof_free`：[src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L562)
- `aof_background_fsync`：[src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L621)
- `aof_is_loading`：[src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L648)

`src/core/kvstore.c` 在启动时会先做 RDB，再做 AOF 初始化和加载，这说明 AOF 是启动恢复链路中的第二层。

### 5.2 当前 AOF 记录内容

`aof_append` 直接写 RESP 数组格式命令，不记录额外元数据。写入路径是：

- `kvs_filter_protocol` 执行成功后调用 `aof_append(count, tokens)`
- `aof_append` 将命令按 RESP 追加到 AOF 文件或缓冲区
- 重写期间通过 `aof_rewrite_append_command` 记录增量

这意味着 TTL MVP 要想可恢复，至少要保证 TTL 相关命令本身也进入 AOF，例如：

- `EXPIRE key seconds` 或 `PEXPIRE key ms`
- `PERSIST key`
- 读取状态命令通常不需要写 AOF

如果只在数据节点里静态保存 `expire_at_ms`，但不把 TTL 变更写入 AOF，重启后 TTL 一定丢失。

### 5.3 AOF 加载与重写现状

`aof_load` 会逐条解析 RESP 命令并回放，回放时通过 `set_sync_context(2)` 避免再次写入 AOF。当前 AOF 重写链路还包含：

- `aof_rewrite_background`：触发后台重写
- `aof_rewrite_append_command`：重写期间缓存新命令
- `aof_stop_rewrite`：停止重写
- `aof_background_fsync`：后台刷盘线程

这说明 AOF 链路已经有后台线程、条件变量和重写状态机，TTL MVP 可以直接复用这些机制，但不建议把 TTL 扫描逻辑塞进 AOF 线程里，因为它的职责是持久化，不是过期回收。

## 6. 主从复制链路定位

### 6.1 主从配置与状态结构

主从配置定义在 `include/distributed.h`：

- `distributed_config_t`
- `slave_info_t`
- `distributed_init`
- `master_sync_command`
- `slave_connect_master`
- `slave_connect_thread`
- `distributed_shutdown`

其中 `distributed_config_t` 只保存角色、主节点地址、同步端口和从节点列表，没有 TTL 元数据相关字段。

### 6.2 复制主入口

复制主逻辑在 `src/distributed/distributed.c`：

- `distributed_init`：[src/distributed/distributed.c](src/distributed/distributed.c#L293)
- `slave_connect_master`：[src/distributed/distributed.c](src/distributed/distributed.c#L332)
- `master_sync_command`：[src/distributed/distributed.c](src/distributed/distributed.c#L554)
- `slave_sync_thread`：[src/distributed/distributed.c](src/distributed/distributed.c#L97)
- `master_sync_server`：[src/distributed/distributed.c](src/distributed/distributed.c#L173)
- `slave_connect_thread`：[src/distributed/distributed.c](src/distributed/distributed.c#L242)
- `distributed_shutdown`：[src/distributed/distributed.c](src/distributed/distributed.c#L249)

### 6.3 当前复制协议特征

复制消息的二进制格式由 `src/utils/master_slave_proto.c` 实现，头部长度是 `MS_HEADER_LEN = 12`，载荷是：

- `op`
- `key_len`
- `val_len`
- `key`
- `value`

见 [src/utils/master_slave_proto.c](src/utils/master_slave_proto.c#L7)、[src/utils/master_slave_proto.c](src/utils/master_slave_proto.c#L19) 和 [include/master_slave_proto.h](include/master_slave_proto.h)

当前协议没有为 TTL 单独预留字段。现有实现中，master 只会把写命令的 `cmd / key / value` 广播出去，slave 收到后按操作类型回放：

- master 广播调用：`master_sync_command(tokens[0], tokens[1], tokens[2])`
- slave 解码后执行：`kvs_array_set / kvs_rbtree_set / kvs_hash_set / kvs_skiptable_set` 等

这意味着：

- 如果 TTL 只存在本地内存，不进入复制消息，从节点不会得到过期语义
- 如果 TTL 相关操作需要跨节点一致，复制协议至少要支持新的操作类型，或者在现有写命令里携带 `expire_at_ms`

### 6.4 当前复制链路对 TTL 的现实约束

当前复制层还有一个明显限制：`master_sync_command` 只接收 `cmd / key / value` 三段语义，调用点也只有 `kvs_filter_protocol` 中的写命令成功路径。若要做 TTL MVP，最小做法通常是：

- 新增 TTL 命令时，把 TTL 变化当成新的写命令同步到 slave
- 或者在 `SET` 的 RESP/AOF/复制载荷里扩展成带过期时间的命令形态

## 7. 后台扫描可复用机制分析

### 7.1 当前存在的可复用机制

仓库里已经有几种“后台持续运行”的机制，可以作为 TTL MVP 的参考，但它们的职责不同：

1. AOF 后台刷盘线程
- `aof_background_fsync` 使用 `pthread_cond_timedwait` 做周期等待，见 [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L621)
- 这是当前最像“周期任务”的实现

2. AOF 重写后台线程
- `aof_rewrite_background` / `aof_rewrite_thread` 已经是完整后台工作流
- 但它服务于持久化重写，不适合作为 TTL 回收主循环

3. 主从连接维护线程
- `slave_connect_thread` 里有 `sleep(1)`，见 [src/distributed/distributed.c](src/distributed/distributed.c#L242) 和 [src/distributed/distributed.c](src/distributed/distributed.c#L244)
- 这只是启动等待，不是周期扫描

4. TCP 同步后端 worker
- `tcp_backend_worker` 里用 `msg_queue_pop`，并在队列空时 `usleep(10)` / `usleep(100)`，见 [src/distributed/sync_backend_tcp.c](src/distributed/sync_backend_tcp.c#L33) 和 [src/distributed/sync_backend_tcp.c](src/distributed/sync_backend_tcp.c#L43)
- 这是消息消费线程，不是 TTL 扫描线程

5. 网络层事件循环
- reactor / proactor / ntyco 都是连接驱动型循环，不适合作为 TTL 统一回收器
- 这些循环的职责是处理请求与响应，不应该承担数据过期扫描

### 7.2 对 TTL MVP 的结论

当前仓库没有一个专门的“定时扫描 TTL 线程”。真正可复用的只有 AOF 的周期线程模型，而不是它的逻辑本身。对于 TTL MVP，最稳妥的做法是：

- 先做懒删除：读路径发现过期就删除并返回不存在
- 再补一个轻量后台扫描线程，按固定周期抽样检查少量 key
- 不要把 TTL 清理绑定到 AOF / 复制线程里

## 8. 推荐修改文件列表

下面是本次 TTL MVP 的候选改动清单，按“最小侵入”优先级排序：

### 8.1 必改：数据结构层

- [include/kvs_array.h](include/kvs_array.h)
- [include/kvs_rbtree.h](include/kvs_rbtree.h)
- [include/kvs_hash.h](include/kvs_hash.h)
- [include/kvs_skiptable.h](include/kvs_skiptable.h)
- [src/storage/kvs_array.c](src/storage/kvs_array.c)
- [src/storage/kvs_rbtree.c](src/storage/kvs_rbtree.c)
- [src/storage/kvs_hash.c](src/storage/kvs_hash.c)
- [src/storage/kvs_skiptable.c](src/storage/kvs_skiptable.c)

原因：四个后端都直接保存 key/value，没有公共对象层，TTL 只能下沉到节点级。

### 8.2 必改：命令层

- [include/kvs_command.h](include/kvs_command.h)
- [include/kvs_operations.h](include/kvs_operations.h)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- [src/core/kvstore.c](src/core/kvstore.c)

原因：需要新增 TTL 命令，并在 `GET / EXISTS / RANGE / SORT` 的处理链路里补过期判断。

### 8.3 必改：RDB / AOF

- [include/kvs_rdb.h](include/kvs_rdb.h)
- [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- [include/kvs_aof.h](include/kvs_aof.h)
- [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c)
- [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)

原因：TTL 变更和过期语义必须能跨重启保留，AOF 重写也要能输出正确状态。

### 8.4 必改：复制链路

- [include/distributed.h](include/distributed.h)
- [include/master_slave_proto.h](include/master_slave_proto.h)
- [src/distributed/distributed.c](src/distributed/distributed.c)
- [src/distributed/sync_backend_tcp.c](src/distributed/sync_backend_tcp.c)
- [src/utils/master_slave_proto.c](src/utils/master_slave_proto.c)

原因：slave 目前只接收 `op/key/value`，TTL 语义要么新增操作，要么扩展载荷字段。

### 8.5 可选：后台任务

- [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c)
- [src/core/kvstore.c](src/core/kvstore.c)

原因：如果 TTL MVP 需要后台抽样清理，最容易复用的是 AOF 的线程启动与销毁方式，但不建议直接复用其业务逻辑。

## 9. 不确定点与需要确认的问题

1. 当前仓库没有统一的 TTL 设计目标文档在代码里显式约束命令集合，是否只做 `EXPIRE / TTL / PERSIST` 三个命令，还是连 `PEXPIRE / PTTL` 一起做，需要后续确认。
2. 复制链路当前只支持三段式 `cmd / key / value`，TTL 是否要求精确到毫秒并同步到 slave，还是只在 master 本地做懒删除，需要确认。
3. RDB/AOF 的版本策略还没有 TTL 兼容分支，是否允许直接把格式版本号升一档，需要确认。
4. 四种数据结构的值生命周期由各自实现文件管理，TTL 过期删除时是否要保持各自返回值语义完全一致，需要确认。
5. `FULLDUMP` 目前只导出 key/value，不导出元信息；如果后续希望把它用于快照恢复或调试，是否需要同步扩展，需要确认。
6. 当前仓库没有找到专用 TTL 后台线程，是否接受先做懒删除 + 轻量定时扫描的 MVP，需要确认。

## 10. 下一步建议

建议第 2 步按下面顺序推进：

1. 先定 TTL MVP 的语义边界：新增哪些命令、SET 是否清 TTL、GET/EXISTS/RANGE 遇到过期如何返回
2. 先改四个底层节点结构体，统一增加 `expire_at_ms` 或等价字段
3. 再改命令入口和读路径，把过期检查做成公共工具函数
4. 然后补 RDB/AOF 序列化与回放
5. 最后再接主从复制，保证 slave 能收到 TTL 语义

## 本步结论

当前 KVLite 没有 TTL 的现成实现，也没有单一可复用的 TTL 元对象。要做“TTL 面试版 MVP”，最稳妥的方案不是先做复杂后台回收，而是先把 TTL 元数据下沉到四个底层节点结构体，再通过命令层统一做懒删除；持久化和复制链路都需要跟着扩展，否则 TTL 只能停留在内存里，无法跨重启和跨节点一致。

## 下一步最推荐执行的文件改动顺序

1. [include/kvs_array.h](include/kvs_array.h)
2. [include/kvs_rbtree.h](include/kvs_rbtree.h)
3. [include/kvs_hash.h](include/kvs_hash.h)
4. [include/kvs_skiptable.h](include/kvs_skiptable.h)
5. [src/storage/kvs_array.c](src/storage/kvs_array.c)
6. [src/storage/kvs_rbtree.c](src/storage/kvs_rbtree.c)
7. [src/storage/kvs_hash.c](src/storage/kvs_hash.c)
8. [src/storage/kvs_skiptable.c](src/storage/kvs_skiptable.c)
9. [include/kvs_command.h](include/kvs_command.h)
10. [src/core/kvs_operation.c](src/core/kvs_operation.c)
11. [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
12. [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c)
13. [src/utils/master_slave_proto.c](src/utils/master_slave_proto.c)
14. [src/distributed/distributed.c](src/distributed/distributed.c)
15. [src/distributed/sync_backend_tcp.c](src/distributed/sync_backend_tcp.c)
