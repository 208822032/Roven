# 第 2 步：TTL 面试版 MVP 实施方案

## 1. 本步目标

本步不改代码，只把 TTL MVP 的实现边界、语义和接入路径收敛清楚，目标是为下一步真正改代码提供一份可以直接按图施工的方案。

本方案严格基于上一轮扫描结果，重点依托以下已确认入口：

- 命令主入口： [src/core/kvstore.c](src/core/kvstore.c#L43)
- 命令表与处理函数： [src/core/kvs_operation.c](src/core/kvs_operation.c#L35)
- 四类存储后端： [include/kvs_array.h](include/kvs_array.h)、[include/kvs_rbtree.h](include/kvs_rbtree.h)、[include/kvs_hash.h](include/kvs_hash.h)、[include/kvs_skiptable.h](include/kvs_skiptable.h)
- RDB： [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L46)
- AOF： [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L37)
- 复制： [src/distributed/distributed.c](src/distributed/distributed.c#L293)、[src/utils/master_slave_proto.c](src/utils/master_slave_proto.c#L19)
- 周期线程参考： [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L621)

本次只做方案，不做代码修改。

## 2. TTL MVP 总体设计

### 2.1 设计原则

TTL MVP 采用最小侵入方案：

1. 每条记录或节点内增加一个统一字段 `expire_at_ms`。
2. 使用绝对毫秒时间戳，避免相对 TTL 在复制、重写和恢复时产生语义偏移。
3. 用 `-1` 表示永不过期。
4. 过期策略采用“惰性删除为主 + 轻量后台扫描为辅”。
5. 只做最核心的命令集：`EXPIRE key seconds`、`TTL key`、`PERSIST key`。
6. `SET` 覆盖成功时默认清除旧 TTL，保持语义简单、可讲清楚、可落地。
7. `DEL` 删除 key 时同步清理 TTL 元数据。
8. RDB、AOF、主从复制都只做必要接入，不引入额外索引结构，不上时间轮、最小堆或独立 expires 字典。

### 2.2 为什么这套设计最适合当前仓库

当前 KVLite 的结构特点是：

- 命令层按数据结构分发，不存在统一对象层。
- 四类后端各自维护 key/value，不共享公共节点结构。
- RDB、AOF、复制链路已经成型，但都是围绕现有写命令和遍历数据展开。

因此最容易做对、也最适合面试表达的方案不是做一套 Redis 级别 TTL 子系统，而是：

- 先把过期信息直接挂到四类节点上。
- 读路径发现过期就顺手删除。
- 后台线程只做抽样清理，负责兜底。
- 持久化和复制只保留当前状态和 TTL 变更本身。

这样能把完整链路讲成一条闭环：

数据结构存储 TTL -> 命令修改 TTL -> 读路径判断过期 -> 持久化恢复 TTL -> 主从同步 TTL -> 后台线程兜底清理。

## 3. 数据结构改动方案

### 3.1 统一字段定义

建议四类节点都增加同名字段：`int64_t expire_at_ms` 或等价的有符号 64 位整数。

语义如下：

- `-1`：永不过期。
- `>= 0`：绝对过期时间，单位毫秒。

统一字段名的好处是：

- 命令层和持久化层可以写成统一逻辑。
- 复制和 RDB/AOF 回放时，不需要为每种结构单独翻译语义。
- 面试时容易解释：所有数据结构都遵守同一套 TTL 规则。

### 3.2 预计改动文件

数据结构头文件：

- [include/kvs_array.h](include/kvs_array.h)
- [include/kvs_rbtree.h](include/kvs_rbtree.h)
- [include/kvs_hash.h](include/kvs_hash.h)
- [include/kvs_skiptable.h](include/kvs_skiptable.h)

对应实现文件：

- [src/storage/kvs_array.c](src/storage/kvs_array.c)
- [src/storage/kvs_rbtree.c](src/storage/kvs_rbtree.c)
- [src/storage/kvs_hash.c](src/storage/kvs_hash.c)
- [src/storage/kvs_skiptable.c](src/storage/kvs_skiptable.c)

### 3.3 对各后端的具体影响

1. 数组后端
- 结构最简单，适合作为最先改的试点。
- 每个 `kvs_array_item_t` 直接加 `expire_at_ms`。
- 适合先把 TTL 语义跑通，再复制到其他后端。

2. 红黑树后端
- `rbtree_node` 直接保存 key/value，最自然的做法也是直接加字段。
- TTL 不影响排序和树结构，但会影响遍历输出和删除时机。

3. 哈希后端
- `hashnode_t` 已经保存 `used / klen / vlen / key / value`。
- 直接扩展一字段最省事，不需要引入额外链表或索引。

4. 跳表后端
- `Node` 是节点式结构，TTL 字段加入后对查找和遍历影响最小。
- 后台扫描时便于直接沿 forward[0] 遍历。

### 3.4 结构层不做的事

本步不建议：

- 新建独立 expires 索引。
- 做双字典设计。
- 引入最小堆、时间轮或定时轮询队列。

原因很简单：当前仓库的核心问题不是“扫描效率不够好”，而是“连最基本的 TTL 语义都还没有”。MVP 阶段先把语义做完整，结构再谈优化。

## 4. 命令行为定义

### 4.1 新增命令

MVP 建议新增三个命令：

1. `EXPIRE key seconds`
- 成功：返回 1。
- key 不存在：返回 0。
- seconds 小于等于 0：建议按立即过期处理，即如果 key 存在则删除并返回 1，不存在返回 0。

2. `TTL key`
- key 不存在：返回 -2。
- key 存在但没有 TTL：返回 -1。
- key 存在且有 TTL：返回剩余秒数，向下取整。
- 如果 TTL 已到期：先做惰性删除，再返回 -2。

3. `PERSIST key`
- key 不存在：返回 0。
- key 存在但本来就没有 TTL：返回 0。
- key 存在且成功清除 TTL：返回 1。

### 4.2 与 Redis 对齐的部分

TTL 语义尽量按 Redis 的用户预期来对齐：

- 不存在 -> -2
- 永不过期 -> -1
- 有 TTL -> 剩余秒数

这部分最适合面试表达，因为它可以直接说明你不是在做一个“自定义过期标记”，而是在实现一套熟悉且可解释的语义。

### 4.3 与当前仓库命令体系的衔接

当前命令分发在 [src/core/kvs_operation.c](src/core/kvs_operation.c#L35) 的 `command_table`，所以 TTL 命令要在这里新增处理入口，再由 `kvs_filter_protocol` 统一走命令校验和执行。

需要额外注意的是：

- 现有的 `SET / MOD / DEL / EXIST / RANGE / SORT` 都是按四个后端分别落到 `handle_*` 中。
- TTL 相关逻辑不应该散在各个 storage 文件里各写一套判断，而应该先在命令层收口，再下发到后端操作。

### 4.4 对现有写命令的统一约定

为了让 MVP 简单、可讲清楚，建议统一约定：

- 所有成功写入 value 的命令，在没有显式 TTL 参数时都清除旧 TTL。
- 这包括当前仓库中的 `SET / RSET / HSET / SSET` 以及对应的 `MOD / RMOD / HMOD / SMOD` 成功更新路径。

这样做的理由：

- 当前仓库的写命令本来就按“覆盖式”使用。
- `SET` 默认清 TTL 是最常见也最容易解释的语义。
- 统一规则可以避免每个后端和每个命令各自处理一套 TTL 保留逻辑。

如果后续要更贴近 Redis，再单独细化不同命令的保留策略；但第 2 步不建议扩展到这一步。

## 5. 读路径过期检查方案

### 5.1 核心原则

读路径采用“先检查 TTL，再决定是否读值”的方式。过期 key 需要当场删除，这样能保证：

- 用户读不到脏数据。
- 过期数据不会长时间滞留。
- 后台扫描就只需要兜底，不需要承担正确性主责任。

### 5.2 需要覆盖的读路径

根据第一步扫描结果，至少要覆盖以下处理函数：

- `handle_get`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L341)
- `handle_exist`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L460)
- `handle_range`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L494)
- `handle_sort`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L540)
- `handle_fulldump`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L647)

### 5.3 推荐读路径实现方式

建议把 TTL 判断拆成两层：

1. 通用判断函数
- 输入 key 和节点指针。
- 判断 `expire_at_ms` 是否过期。
- 如果过期，返回一个“需要删除”的结果。

2. 惰性删除执行函数
- 删除节点。
- 如果当前节点处于 master 且不是复制回放上下文，则同步触发 AOF 追加和主从广播。

这样做的好处是：

- `TTL` 和 `GET` 这类精确访问可以直接复用。
- `RANGE / SORT / FULLDUMP` 这类遍历访问可以只做过滤，不需要把删除逻辑写散。
- 复制和 AOF 的行为可以统一从一个删除入口发出。

### 5.4 对不同读命令的处理建议

1. `GET`
- 先查 TTL。
- 过期则删除并返回不存在。
- 未过期则返回 value。

2. `EXIST`
- 先查 TTL。
- 过期则删除并返回不存在。
- 未过期则返回存在。

3. `TTL`
- 这是 TTL 的专用读取命令。
- 如果 key 过期，直接删除并返回 -2。

4. `RANGE` / `SORT` / `FULLDUMP`
- 在遍历过程中跳过过期节点。
- 如实现成本可控，可对遇到的过期节点做顺手删除；如果遍历中删除复杂，先跳过也能满足 MVP 正确性。

### 5.5 为什么这条路线最适合当前项目

当前仓库已经有 RESP 解析和统一命令入口，但 storage 层是分散的。若在每个 storage 文件里各写一套 TTL 读取逻辑，后续会很难维护。

相反，把 TTL 检查先收口到 [src/core/kvs_operation.c](src/core/kvs_operation.c) 的命令处理层，再下探到各后端的删除接口，既容易做对，也容易解释。

## 6. 写路径 TTL 维护方案

### 6.1 需要处理的写路径

第一步扫描里已经确认，当前写路径主入口是：

- `handle_set`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L295)
- `handle_del`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L380)
- `handle_mod`：[src/core/kvs_operation.c](src/core/kvs_operation.c#L416)

还要覆盖具体 storage 的写函数：

- `kvs_array_set / mod / del`
- `kvs_rbtree_set / mod / del`
- `kvs_hash_set / mod / del`
- `kvs_skiptable_set / mod / del`

### 6.2 写路径规则

建议写路径规则如下：

1. `SET` 类命令成功插入或覆盖后，`expire_at_ms = -1`。
2. `MOD` 类命令如果更新成功，也按覆盖写处理，清除旧 TTL。
3. `DEL` 成功删除后，TTL 一并释放。
4. `EXPIRE` 成功后，仅修改 TTL，不改 value。
5. `PERSIST` 成功后，仅把 `expire_at_ms` 重置为 `-1`。

### 6.3 为什么 SET 默认清 TTL

这条规则非常适合面试回答，因为它：

- 简单。
- 和主流数据库直觉一致。
- 避免用户“同一个 key 被覆盖后 TTL 仍然保留”导致的语义惊讶。
- 对当前仓库的命令模型最友好，因为写命令本来就不细分更多修饰参数。

### 6.4 写路径和复制/AOF的联动

写路径一旦修改 TTL 状态，就必须同时满足两件事：

- AOF 要记录下来，保证重启能恢复。
- 主从复制要广播出去，保证 slave 看到同一状态。

这意味着 TTL 不是纯本地内存字段，而是一个会进入持久化和复制的状态变更。

## 7. RDB 接入方案

### 7.1 当前 RDB 状态

当前 RDB 在 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L46) 和 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c#L130) 分别负责保存和加载，格式版本号定义在 [include/kvs_rdb.h](include/kvs_rdb.h) 中的 `RDB_VERSION`。

现有格式只保存 key/value，没有 TTL 元数据。

### 7.2 推荐方案：版本升级并增加 expire_at_ms

建议把 RDB 格式升级一版，在每条记录中附加 `expire_at_ms`：

- 保存时：每条记录多写 8 字节的 `expire_at_ms`。
- 加载时：如果记录已经过期，直接跳过，不回灌到内存。
- 永不过期则写 `-1`。

### 7.3 为什么要在加载时直接跳过过期记录

这是最符合 TTL 语义的做法，因为：

- 避免快照里已经过期的数据被恢复出来后再被清理。
- 省掉启动后大规模过期清理的瞬时压力。
- 让 RDB 更像“当前时刻的有效状态快照”，而不是“历史数据加 TTL 标记”。

### 7.4 预计改动文件

- [include/kvs_rdb.h](include/kvs_rdb.h)
- [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- 如果要在启动阶段记录加载结果，可能还要联动 [src/core/kvstore.c](src/core/kvstore.c)

### 7.5 兼容性建议

最稳妥的做法是：

- 把 `RDB_VERSION` 升级。
- 加载端兼容老版本，老版本记录默认 `expire_at_ms = -1`。

这样可以避免老文件完全失效，也方便逐步迁移。

## 8. AOF 接入方案

### 8.1 当前 AOF 状态

AOF 入口在 [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L37) 和 [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L179)，写入的是 RESP 命令流，不是对象快照。

这意味着 TTL 最自然的接入方式不是改成新格式，而是把 TTL 变化作为命令写进去。

### 8.2 推荐方案：命令式记录 TTL

建议 AOF 继续沿用命令式回放，只新增 TTL 命令的写入和回放支持：

- `EXPIRE` 写入 AOF。
- `PERSIST` 写入 AOF。
- `SET` 覆盖清 TTL 不需要单独写“清 TTL”命令，因为 replay `SET` 时会按同样规则把 TTL 清掉。

### 8.3 AOF 重写如何处理 TTL

当前重写入口在 [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)。它遍历当前内存状态并重新生成 AOF。

TTL MVP 下，重写需要输出的是“当前能恢复出同样状态的命令序列”，建议如下：

- 对永不过期 key：输出对应写命令，如 `SET key value`。
- 对带 TTL 的 key：输出 `SET key value`，再输出 `EXPIRE key seconds`。

这样可以保证重放后 TTL 状态一致，且不需要引入新的 AOF 文件格式。

### 8.4 预计改动文件

- [include/kvs_aof.h](include/kvs_aof.h)
- [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c)
- [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)

### 8.5 为什么不建议在 AOF 中直接写 expire_at_ms

因为当前 AOF 已经是命令流，继续保留命令语义的收益更大：

- 可读性更强。
- 重放逻辑和业务逻辑一致。
- 面试时更容易说明“这个状态是如何被恢复的”。

## 9. 主从复制接入方案

### 9.1 当前复制链路

复制主逻辑位于 [src/distributed/distributed.c](src/distributed/distributed.c#L293)，二进制消息协议位于 [src/utils/master_slave_proto.c](src/utils/master_slave_proto.c#L19)。

当前复制协议只承载 `op / key / value`，因此 TTL 变更必须显式进入复制链路。

### 9.2 推荐方案：为 TTL 命令新增复制操作

建议在 `ms_op_t` 中增加两个 TTL 相关操作：

- `MS_OP_EXPIRE`
- `MS_OP_PERSIST`

如果后续想更明确，也可以再加一个过期删除内部操作，但第 2 步不建议先做。

### 9.3 复制消息如何工作

1. master 执行 `EXPIRE` 或 `PERSIST` 成功后，通过 `master_sync_command` 广播。
2. slave 收到对应操作后，只更新本地 TTL 状态，不重新广播。
3. 当 master 因懒删除或后台扫描真正删除过期 key 时，再广播一个普通 `DEL`。

### 9.4 为什么 expiration 需要广播 DEL

这是 TTL 设计里最容易被忽略但非常关键的一点。

过期不仅仅是“设置了一个截止时间”，当 key 真正过期并被删除时，这个删除必须同步到 slave，否则 master 和 slave 的实际数据会逐渐分叉。

因此建议把“过期触发删除”视为一次内部写操作：

- master 本地删掉。
- 同时走现有复制通道广播 DEL。
- AOF 也记一条 DEL。

这样 TTL 的状态变化和真正删除都能在复制链路里闭环。

### 9.5 预计改动文件

- [include/distributed.h](include/distributed.h)
- [include/master_slave_proto.h](include/master_slave_proto.h)
- [src/distributed/distributed.c](src/distributed/distributed.c)
- [src/distributed/sync_backend_tcp.c](src/distributed/sync_backend_tcp.c)
- [src/utils/master_slave_proto.c](src/utils/master_slave_proto.c)

## 10. 后台扫描最小实现方案

### 10.1 为什么还需要后台扫描

惰性删除能保证正确性，但如果某些 key 很久没有被访问，过期项会长期滞留在内存里。后台扫描的作用是：

- 兜底回收内存。
- 让 TTL 语义更接近用户预期。
- 避免只读 key 永久挂着过期状态。

### 10.2 最小实现原则

后台扫描只做三件事：

1. 周期性唤醒。
2. 从四类数据结构中抽样查看少量 key。
3. 对已经过期的 key 执行删除，并在 master 上同步广播 DEL。

### 10.3 复用哪种现有机制

当前仓库里最适合借鉴的是 [src/persistence/kvs_aof.c](src/persistence/kvs_aof.c#L621) 的后台线程模型，因为它已经具备：

- pthread 线程。
- 条件变量。
- 周期唤醒。
- stop 标志。

但要强调：这里只复用“线程模型”，不复用它的业务逻辑。

### 10.4 扫描粒度建议

为了保持 MVP 简单，建议：

- 周期为 1 秒或 2 秒。
- 每轮每种数据结构只抽样有限数量的节点。
- 不做全量扫描，不做复杂优先级调度。

这样既能回收大部分过期 key，也不会给主业务线程带来明显压力。

### 10.5 预计改动文件

- 首选： [src/core/kvstore.c](src/core/kvstore.c) 负责启动和关闭 TTL 扫描线程。
- 如果后面想更整洁，可以新增独立 TTL 模块，例如 `src/core/kvs_ttl.c` 和 `include/kvs_ttl.h`。

本步不要求真的建新文件，但建议在第 3 步前明确是否拆出独立模块。

## 11. 风险点与取舍

### 11.1 语义一致性风险

四个后端是独立实现，TTL 字段加进去后，最容易出问题的是：

- 某个后端忘了在读路径检查过期。
- 某个后端忘了在删除时清理 TTL。
- 某个后端在遍历时把过期 key 还输出了。

所以命令层必须尽量抽公共逻辑，不能在四个后端里各自发散。

### 11.2 复制一致性风险

只同步 `EXPIRE` 不同步真正的过期删除，会导致 master 和 slave 状态慢慢不一致。

所以要把“真正删除”也纳入同步逻辑，至少在 master 上触发 DEL 广播。

### 11.3 RDB 和 AOF 的兼容风险

如果只改内存而不改持久化，TTL 一定丢失。

如果只改 AOF 不改 RDB，启动恢复和快照恢复会不一致。

因此这两个路径必须一起考虑，但第 2 步只需先定好方案，不需要一次性写完。

### 11.4 后台扫描复杂度风险

后台扫描最容易被做成“复杂但不可靠”的东西。

本方案刻意把它压到很小：

- 周期简单。
- 抽样简单。
- 只做兜底。

这是最适合面试版 MVP 的选择。

### 11.5 取舍总结

本方案的核心取舍是：

- 牺牲部分极致性能。
- 换取完整语义、清晰链路和容易讲清楚的实现。

这非常适合当前仓库，也非常适合面试表达。

## 12. 面试表达建议

如果要把这套 TTL MVP 讲给面试官，建议按下面四层来讲：

1. 数据模型
- 为什么把 `expire_at_ms` 挂到节点上。
- 为什么用绝对毫秒而不是相对秒数。
- 为什么 `-1` 表示永不过期。

2. 核心语义
- `EXPIRE / TTL / PERSIST` 的返回值。
- `SET` 覆盖时为什么默认清 TTL。
- 过期 key 为什么先惰性删除再返回。

3. 全链路一致性
- 读路径、AOF、RDB、复制如何一起工作。
- 为什么真正过期时要广播 DEL。

4. 性能取舍
- 为什么不一上来做时间轮或堆。
- 为什么先做 lazy delete + 轻量后台扫描。

这套讲法有两个优点：

- 很贴合当前仓库现状，不会空谈。
- 结构清楚，容易把“我怎么一步步把功能闭环”讲出来。

## 13. 下一步建议

建议第 3 步按以下顺序进入代码改动：

1. 先改四个节点结构体，统一加入 `expire_at_ms`。
2. 再补命令层 TTL 命令和读路径过期检查。
3. 然后把 DEL 清理、TTL 写入和惰性删除封装成公共逻辑。
4. 再改 RDB，让快照能保存和恢复 TTL。
5. 再改 AOF，让 TTL 变化能回放。
6. 最后改复制链路和后台扫描。

## 本步结论

TTL MVP 不应做成 Redis 完整子系统，而应当在当前 KVLite 架构里做成一条轻量闭环：

- 节点内直接存 `expire_at_ms`。
- 读时先判断过期。
- 写时统一维护 TTL。
- RDB/AOF/复制同步 TTL 状态。
- 后台线程只做兜底抽样清理。

这套方案改动范围可控，语义清楚，面试也容易讲透。
