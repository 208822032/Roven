# 第 7A 步：分析 TCP 主从复制路径中的 TTL 接入方案

## 1. 本步目标

本步只分析 TCP 主从复制路径中的 TTL 接入方案，不修改代码，不处理 eBPF 路径。

本步要回答四个问题：

1. 当前 TCP 主从复制模型是怎么跑起来的。
2. 命令传播与状态同步到底经过哪条链路。
3. TTL 在 TCP 主从中应该采用什么最小正确语义。
4. slave 是否主动过期，还是完全以 master 为准。

本步的目标不是复刻 Redis 全套 TTL 主从语义，而是先选一套最容易做对、最容易调试、最容易讲清楚的方案。

## 2. 当前 TCP 复制链路分析

### 2.1 TCP 路径的主入口

当前 TCP 复制链路的关键入口分别在：

- [src/core/kvstore.c](src/core/kvstore.c)
- [src/distributed/distributed.c](src/distributed/distributed.c)
- [src/distributed/sync_backend_tcp.c](src/distributed/sync_backend_tcp.c)
- [src/utils/master_slave_proto.c](src/utils/master_slave_proto.c)
- [include/master_slave_proto.h](include/master_slave_proto.h)

从实现上看，TCP 主从不是单点推送，而是两段式结构：

1. master 侧先把命令转换成主从协议消息。
2. TCP 后端 worker 再把消息广播给所有 slave。

### 2.2 master 侧的写命令出口

主命令执行入口在 [src/core/kvstore.c](src/core/kvstore.c) 的 kvs_filter_protocol。

它的关键逻辑是：

- 先查命令表。
- 再校验参数。
- 再执行命令 handler。
- 只要命令属于写命令，且当前是 master，且不是 slave 回放上下文，就会进入复制分发。

当前这段逻辑的含义很明确：

- master 是写入源头。
- slave 自己不能直接接收客户端写请求。
- 写命令成功之后才会进入复制链路。

### 2.3 master_sync_command 的角色

[src/distributed/distributed.c](src/distributed/distributed.c) 里的 master_sync_command 是 master 到 slave 的统一同步出口。

它当前做的事情是：

- 根据命令名映射到 ms_op_t。
- 把 key/value 编码进主从协议消息。
- 调用 TCP 后端广播接口发送给所有 slave。

这意味着当前 TCP 复制的真实语义不是“同步一个对象”，而是“同步一条写动作”。

### 2.4 TCP 后端广播线程

[src/distributed/sync_backend_tcp.c](src/distributed/sync_backend_tcp.c) 里的 tcp_backend_worker 负责把 master 侧排队的消息发给所有 slave。

这个线程的特征是：

- 先从消息队列取出一条已编码好的协议消息。
- 再拿当前 slave 列表快照。
- 最后把同一条消息按顺序发给所有 slave socket。

所以 TCP 复制的发送模型是典型的广播式 fan-out，不是每个 slave 一条独立语义流。

### 2.5 slave 侧的接收与回放

slave 的连接和回放逻辑在 [src/distributed/distributed.c](src/distributed/distributed.c) 的 slave_connect_master。

它的处理顺序是：

1. 连接 master。
2. 连续接收 TCP 字节流。
3. 用 ms_decode_message 解出 op、key、value。
4. 设置复制上下文，避免再次回写 master。
5. 按 op 调用本地存储的写接口。
6. 如果开启 AOF，再把回放动作写入 AOF。

这里有两个关键点：

- slave 不是在执行“读到什么就信什么”的通用流解析，而是在执行“把 master 发来的写动作重放到本地”的回放逻辑。
- 复制上下文是明确区分的，set_sync_context(1) 用来表示这是主从同步写，避免形成回环。

### 2.6 当前复制协议的能力边界

[include/master_slave_proto.h](include/master_slave_proto.h) 和 [src/utils/master_slave_proto.c](src/utils/master_slave_proto.c) 当前只提供了三段信息：

- op
- key_len / key
- val_len / value

也就是说，当前 TCP 复制协议没有专门的 TTL 字段，也没有过期时间元数据。

这会直接影响 TTL 接入：

- 如果只同步写 value，不同步 expire_at_ms，slave 永远无法恢复完整 TTL 状态。
- 如果只同步相对秒数，不同步绝对过期点，重放和延迟会导致 TTL 漂移。

### 2.7 当前全量同步的现状

master 侧的全量同步也走同一条 TCP 路径。

slave_sync_thread 会遍历 master 当前内存里的各个数据结构，然后对每个 key 调用 master_sync_command。

这说明当前 TCP 主从已经具备“先全量、后增量”的基本模型，但全量同步只同步了 value 语义，没有同步 TTL 元数据。

## 3. TCP 路径中 TTL 的语义设计

### 3.1 推荐的最小语义

本步推荐的最小语义是：

1. master 是 TTL 的唯一裁判者。
2. slave 不主动运行独立的过期扫描线程。
3. slave 只跟随 master 发来的 TTL 变更和删除动作。
4. 所有 TTL 相关状态都以绝对过期时间为准，而不是相对秒数。

这套语义最重要的好处是清晰：

- 只有一个权威时钟源。
- 只有一个删除决策者。
- 不会因为 master/slave 时钟偏差导致提前删或晚删。

### 3.2 为什么要用 master 作为唯一裁判

如果 slave 自己按本地时钟主动过期，会出现三个问题：

1. 时钟偏差会导致 slave 先删或后删。
2. 网络延迟会导致 slave 和 master 在短时间内看到不同的有效性结果。
3. 全量同步和增量同步交叠时，slave 很难判断是“还没收到更新”还是“真正过期了”。

所以最稳的方案是：

- master 决定 key 何时过期。
- master 决定 key 何时真正删除。
- slave 只按 master 的动作收敛本地状态。

### 3.3 slave 是否主动过期

本步建议明确选择：slave 不主动过期。

这里的“不主动过期”是指：

- 不起独立的 TTL 定时扫描线程。
- 不根据本地时钟自行删除 key。
- 不把本地过期判断升级为新的写动作。

如果后续读路径需要避免返回明显过期的数据，slave 可以只做被动可见性过滤，但不要把这个过滤动作变成独立删除来源。

一句话总结就是：slave 跟随 master 的 TTL 变更与删除动作，不自己当裁判。

### 3.4 绝对时间语义

TTL 在 TCP 路径里应当使用绝对过期时间，即 expire_at_ms。

原因有两个：

- 复制时不会因为延迟而重新计算相对 TTL。
- 全量同步和 AOF / RDB 的设计可以保持一致，便于排查。

也就是说，TCP 复制层应该传播“这个 key 什么时候过期”，而不是“还有多少秒过期”。

## 4. EXPIRE / PERSIST / DEL / 覆盖写清 TTL 的传播方案

### 4.1 EXPIRE 的传播方案

EXPIRE 在 master 上执行成功后，不建议把相对秒数原样广播给 slave。

更稳的做法是：

- master 先把 seconds 计算成绝对 expire_at_ms。
- 再把这个绝对时间传播给 slave。

这样 slave 不需要重新计算过期点，也不会因为网络延迟产生语义偏移。

### 4.2 PERSIST 的传播方案

PERSIST 的语义是清除 TTL。

它应该直接作为一个 TTL 清除动作传播给 slave，而不是拆成其他隐式行为。

这样做的好处是：

- 语义直观。
- slave 好调试。
- 不需要猜测是不是某个写命令顺带把 TTL 清掉了。

### 4.3 DEL 的传播方案

DEL 是最简单也最关键的动作。

无论是用户显式删除，还是 master 因 TTL 到期而删除，最终都应该走同一个删除传播路径。

也就是说：

- 逻辑来源可以不同。
- 最终对 slave 的动作应该一致。

这能保证 slave 的最终可见状态和 master 保持一致。

### 4.4 覆盖写清 TTL 的传播方案

覆盖写清 TTL 不建议单独发一条“清 TTL”消息。

推荐方案是：

- SET、RSET、HSET、SSET、MOD、RMOD、HMOD、SMOD 这类成功覆盖写，仍然按原写命令传播。
- slave 在执行这些写命令时，按存储层规则把 expire_at_ms 重置为永不过期。

也就是说，覆盖写清 TTL 是写命令自身的副作用，不是一个独立复制事件。

### 4.5 推荐的传播顺序

对于一个带 TTL 的 key，推荐传播顺序是：

1. 先同步 value 写入。
2. 再同步 TTL 状态变更。

如果是覆盖写，则只需要同步覆盖写本身，因为覆盖写会自动清 TTL。

如果是 EXPIRE，则同步绝对过期时间。

如果是 PERSIST，则同步清 TTL 动作。

如果是实际过期删除，则同步 DEL。

## 5. 过期删除时 master / slave 一致性方案

### 5.1 一致性目标

过期删除的目标不是让 slave 自己“算到某一刻就删”，而是让 slave 最终和 master 收敛到同一结果。

因此，删除动作必须以 master 为准。

### 5.2 推荐的一致性流程

推荐流程如下：

1. master 在读路径或后台回收中发现 key 过期。
2. master 本地先删除该 key。
3. master 再把 DEL 广播给所有 slave。
4. slave 收到 DEL 后删除对应 key。

这条链路的关键点是：

- 删除决策只有 master 做。
- slave 不提前删。
- DEL 是最终一致性的收敛动作。

### 5.3 为什么不能让 slave 自己删

如果 slave 自己主动删，后果是：

- 读路径和写路径可能在不同时间点看见不同状态。
- master 还没删，slave 已经删了，会造成复制语义难以解释。
- 调试时无法分辨是“复制延迟”还是“本地时钟先跑了”。

对于当前项目来说，最稳的方式就是把删除权放在 master。

### 5.4 全量同步与增量同步的关系

全量同步时，slave 接收的是 master 当前快照。

这意味着 TTL 也必须以快照状态的形式进入全量同步，而不是只同步 value。

最稳妥的思路是：

- 全量同步时先发 value。
- 如果 key 带 TTL，再补发绝对过期时间。
- 增量阶段继续只传播后续的 TTL 变更和 DEL。

这样 slave 重启或新接入时，才能从一开始就拿到完整状态。

## 6. slave 是否主动过期的设计选择

本步的明确结论是：slave 不主动过期，master 为准。

推荐理由如下：

1. 最容易讲清楚。
2. 最容易调试。
3. 最不容易被时钟偏差和同步延迟搞乱。
4. 和当前 TCP 复制模型最契合。

如果未来要补读路径兜底，可以只做可见性过滤，不做独立删除线程。

也就是说：

- 主动删除权在 master。
- slave 只跟随 master 的 TTL 变更与删除动作。
- 这是一套比 Redis 更朴素、但更适合当前项目现状的方案。

## 7. 推荐修改文件列表

如果下一步开始真正改 TCP 路径，建议优先修改这些文件：

- [include/master_slave_proto.h](include/master_slave_proto.h)
- [src/utils/master_slave_proto.c](src/utils/master_slave_proto.c)
- [src/distributed/distributed.c](src/distributed/distributed.c)
- [src/distributed/sync_backend_tcp.c](src/distributed/sync_backend_tcp.c)
- [src/core/kvstore.c](src/core/kvstore.c)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)

这些文件分别对应：

- 协议扩展。
- 编码和解码。
- master 分发逻辑。
- TCP 广播线程。
- 命令出口和同步上下文。
- TTL 命令和读路径的统一处理。

## 8. 风险点

### 8.1 协议当前没有 TTL 字段

当前协议只有 op、key、value，没有过期时间字段。

风险是：如果直接把 EXPIRE 的相对秒数原样同步，slave 会在自己的接收时刻重新解释 TTL，语义会漂。

### 8.2 全量同步和增量同步可能交错

当前 TCP 复制既有全量同步，又有增量广播。

风险是：如果消息顺序没有讲清楚，TTL 变更和 DEL 可能与快照阶段交错，导致 slave 看到短暂不一致。

### 8.3 slave 主动过期会引入分叉

如果 slave 也跑自己的 TTL 线程，clock skew、网络延迟、负载抖动都会把结果搞乱。

这会让问题从“复制是否同步到”变成“为什么两个节点删的时间不同”，排查成本会明显上升。

### 8.4 过期删除必须广播 DEL

如果 master 只是本地删掉，但没有把 DEL 发出去，slave 会长期保留脏数据。

所以 TTL 的最终删除必须进入复制链路。

### 8.5 兼容性风险

一旦协议扩展了 TTL 语义，旧 slave 或旧 master 都可能不认识新消息。

因此下一步实现时需要明确协议版本或最小兼容策略。

## 9. 验证思路

### 9.1 基础连通性验证

先确认 TCP 主从正常联通：

- master 启动后能接受 slave 连接。
- slave 能先完成全量同步，再接收增量广播。
- 普通 SET / DEL / MOD 仍然按原逻辑工作。

### 9.2 TTL 传播验证

再验证 TTL 相关动作：

1. 在 master 上写入 key。
2. 对 key 设置 TTL。
3. 确认 slave 收到的是绝对过期时间，而不是相对秒数。
4. 等待过期后，确认 master 删除时会广播 DEL。
5. 确认 slave 在收到 DEL 之前不主动删除，在收到 DEL 之后状态一致。

### 9.3 覆盖写清 TTL 验证

验证覆盖写时：

1. 先给 key 设置 TTL。
2. 再用 SET 或 MOD 覆盖写。
3. 确认 slave 跟随覆盖写后，TTL 被清除。
4. 不需要额外发送清 TTL 消息。

### 9.4 全量同步验证

验证 slave 新接入时：

1. master 先准备一个永久 key。
2. 再准备一个带 TTL 的 key。
3. 新 slave 连接 master。
4. 检查全量同步后，两种 key 都能恢复正确状态。
5. 带 TTL 的 key 必须保留其绝对过期时间。

### 9.5 调试建议

调试时重点看三处：

- master 有没有发出 TTL 相关复制消息。
- TCP worker 有没有把消息完整发出去。
- slave 有没有按同样的写动作落库。

如果这三处都对，TTL 复制问题基本就能定位清楚。

## 10. 本步结论

TCP 路径里的 TTL 接入，最稳妥的方案不是做复杂的双端定时过期，而是让 slave 完整跟随 master 的 TTL 变更和删除动作。

本步的核心结论可以归纳成一句话：

master 负责决定 TTL 什么时候生效、什么时候失效、什么时候真正删除；slave 只跟随 master 的动作回放，不主动当裁判。

这套方案的优点是：

- 语义简单。
- 调试简单。
- 和当前 TCP 复制模型匹配。
- 不需要先引入 Redis 那种更复杂的 TTL 主从细节。

## 11. 下一步建议

下一步只做 TCP 路径的代码落地，不碰 eBPF：

1. 先扩主从协议，让 TTL 能传绝对过期时间。
2. 再改 master 侧复制分发，让 EXPIRE / PERSIST / DEL / 覆盖写都能走统一语义。
3. 再改 slave 回放，让它只跟随 master 的 TTL 变化，不主动过期。
4. 最后补一组 TCP 路径的 TTL 回归测试。

如果后续要继续推进，我建议下一步直接进入 TCP 复制代码改造，不要先碰 eBPF。
