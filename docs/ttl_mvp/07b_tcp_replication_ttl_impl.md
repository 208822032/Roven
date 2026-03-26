# 第 7B 步：实现 TCP 路径的 TTL 主从同步

## 1. 本步目标

本步只处理 TCP 路径，不修改 eBPF 路径。

目标是把 07A 的设计真正落到代码里，让 TCP 主从复制在 TTL 上形成最小正确闭环：

- master 负责判断 TTL 状态并传播。
- slave 不主动过期，只跟随 master 的 TTL 变更和删除动作。
- TCP 路径传播的是写动作和 TTL 状态的结合，而不是单独的当前值快照。

本步需要覆盖的语义是：

1. EXPIRE key seconds
2. PERSIST key
3. DEL key
4. 覆盖写清 TTL
5. key 到期后删除时，slave 最终与 master 保持一致

## 2. 修改前现状

当前仓库已经具备 TTL 内存态、RDB 和 AOF 能力，但 TCP 复制链路还只理解普通写命令和删命令。

现状可以概括为三点：

1. master 侧会把写命令交给 TCP 后端广播，但只传 command / key / value。
2. slave 侧只会把收到的 op 回放成 SET / DEL / MOD / RSET / HSET / SSET 这些动作。
3. TCP 协议里没有 TTL 元数据字段，因此 EXPIRE / PERSIST 的语义不会自动同步到 slave。

另外，四个后端节点已经都带了 expire_at_ms：

- 数组：kvs_array_item_t
- 红黑树：rbtree_node
- 哈希：hashnode_t
- 跳表：Node

这意味着 TCP 复制不需要再改存储结构，只需要把 TTL 的状态变化同步出去。

## 3. 修改文件列表

### 3.1 必改文件

- [include/master_slave_proto.h](include/master_slave_proto.h)
- [src/utils/master_slave_proto.c](src/utils/master_slave_proto.c)
- [src/core/kvstore.c](src/core/kvstore.c)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- [include/kvstore.h](include/kvstore.h)
- [include/kvs_command.h](include/kvs_command.h)
- [src/distributed/distributed.c](src/distributed/distributed.c)

### 3.2 每个文件为什么要改

- [include/master_slave_proto.h](include/master_slave_proto.h)：需要新增 TCP 复制的 TTL 操作码，让协议能区分 PEXPIREAT 和 PERSIST。
- [src/utils/master_slave_proto.c](src/utils/master_slave_proto.c)：需要让编解码器认识新的 TTL 操作码。
- [src/core/kvstore.c](src/core/kvstore.c)：这里是命令执行后的统一出口，需要把 EXPIRE / PEXPIREAT / PERSIST 规范化成 master 侧真正该传播的动作。
- [src/core/kvs_operation.c](src/core/kvs_operation.c)：需要补一个可复用的 TTL 写入辅助函数，让 slave 回放 TTL 时能直接修改任意后端节点。
- [include/kvstore.h](include/kvstore.h) 和 [include/kvs_command.h](include/kvs_command.h)：需要暴露 TTL 辅助函数声明，供 master / slave 复制路径调用。
- [src/distributed/distributed.c](src/distributed/distributed.c)：这里同时承担 master 广播、slave 回放和全量同步，需要把 TTL 变更和过期删除动作接上。

## 4. TTL 在 TCP 主从中的实现方式

### 4.1 总体方式

本步采用“命令传播 + 状态传播”结合的方式，但主导还是命令传播。

具体来说：

- 普通写命令继续传播原命令，让 slave 走同样的 SET / MOD / DEL 回放。
- EXPIRE 不直接传播相对秒数，而是传播绝对过期时间 expire_at_ms。
- PERSIST 直接传播为清 TTL 动作。
- 过期触发删除时，master 传播 DEL。

这样做的结果是：

- slave 不需要自己重新计算 TTL。
- master 是唯一的过期裁判。
- TTL 复制和数据删除能落到同一条广播链路上。

### 4.2 EXPIRE 的实现方式

master 执行 EXPIRE 成功后，先在本地把 seconds 转成 expire_at_ms，再广播 PEXPIREAT key expire_at_ms。

这样 slave 收到的是绝对过期点，不会因为网络延迟或时钟差导致 TTL 漂移。

如果 EXPIRE 的结果是立即删除，则 master 广播 DEL。

### 4.3 PERSIST 的实现方式

master 执行 PERSIST 成功后，直接广播 PERSIST key。

slave 收到后只做一件事：把该 key 的 expire_at_ms 设为 KV_TTL_NO_EXPIRE。

### 4.4 覆盖写清 TTL 的实现方式

SET / RSET / HSET / SSET / MOD / RMOD / HMOD / SMOD 成功后，存储层已经会把 expire_at_ms 重置为永不过期。

因此复制层不需要额外发“清 TTL”消息，只要继续传播覆盖写本身即可。

slave 执行同样的覆盖写后，也会自动清掉 TTL。

## 5. 命令传播 / 状态同步改动

### 5.1 master 侧改动

`kvs_filter_protocol` 仍然是统一出口，但 TTL 命令要特殊处理：

- EXPIRE / PEXPIREAT 成功后，不再直接把原 tokens 原样广播出去。
- PERSIST 成功后，广播清 TTL 动作。
- 其他写命令仍然按原样广播。

这是因为：

- EXPIRE 传播相对秒数会漂。
- PEXPIREAT 传播绝对时间才是稳定语义。
- 立即过期的场景应该广播 DEL，而不是广播一个马上失效的 TTL。

### 5.2 slave 侧改动

slave 侧需要支持两类新消息：

- PEXPIREAT key expire_at_ms
- PERSIST key

slave 回放时不做独立过期扫描，只执行 master 发来的动作。

对于 PEXPIREAT：

- 如果过期时间仍有效，就写入 expire_at_ms。
- 如果过期时间已经不有效，就删除 key。

对于 PERSIST：

- 直接把 expire_at_ms 清为 KV_TTL_NO_EXPIRE。

### 5.3 全量同步改动

slave 首次连上 master 时的全量同步，也要把 TTL 带上。

具体做法是：

1. 先同步 key/value。
2. 如果该 key 带 TTL，再同步 PEXPIREAT。
3. 已过期 key 在全量同步阶段直接跳过。

这样 slave 拿到的是当前有效状态，不是历史状态加过期标签。

## 6. 过期删除同步方案

### 6.1 删除动作由 master 负责

本步明确采用 master 负责传播删除动作的策略。

当 master 因读路径惰性删除、后台回收或其他触发点确认 key 已过期时：

1. master 本地先删。
2. master 再广播 DEL。
3. slave 收到 DEL 后删除本地副本。

### 6.2 为什么这样最稳

这样做的优点是：

- slave 不需要自己判断过期点。
- master / slave 的最终状态不会因为时钟偏差分叉。
- 过期删除和普通删除走的是同一套广播路径，最容易调试。

### 6.3 处理 TTL 过期的边界

如果 master 发现 key 在 EXPIRE 或 PEXPIREAT 之后已经立即过期，那么 master 不发送 PEXPIREAT，而是直接发送 DEL。

这样 slave 看到的是最终删除结果，而不是一个马上失效的中间态。

## 7. slave 行为策略

本步选择：slave 不主动过期。

具体含义是：

- slave 不启动独立 TTL 扫描线程。
- slave 不根据本地时钟自行删除 key。
- slave 只回放 master 广播下来的 TTL 变更与 DEL。

这个选择是当前项目里最容易做对、最容易解释、最容易排障的方案。

如果后续要做读路径兜底，也只建议做读取时过滤，不要升级成独立删除源。

## 8. 风险点

### 8.1 协议需要增加新 op

TCP 协议现在只有普通写操作码，没有 TTL 操作码。

风险是：协议扩展后，旧版本 master / slave 可能不认识新消息。

### 8.2 全量同步必须带 TTL

如果全量同步只发 SET / RSET / HSET / SSET，不补发 PEXPIREAT，那么 slave 会把带 TTL 的 key 变成永久 key。

这是本步最需要避免的错误。

### 8.3 过期删除必须最终广播 DEL

如果 master 只在本地删，不广播 DEL，slave 会长期保留脏数据。

### 8.4 slave 主动过期会引入分叉

如果 slave 也跑自己的 TTL 线程，会出现两个节点删点不一致的问题，调试会明显变难。

### 8.5 立即过期要转成 DEL

EXPIRE seconds <= 0 或 PEXPIREAT 指向过去时间时，直接广播 DEL 更稳。

## 9. 验证方式

### 9.1 编译验证

先执行构建，确认协议枚举、TTL 辅助函数、master 广播、slave 回放都能通过编译。

### 9.2 基础复制验证

1. 启动 master。
2. 启动 slave。
3. 在 master 上执行 SET、DEL、MOD，确认 slave 行为和原来一致。

### 9.3 EXPIRE 验证

1. master 执行 SET a 1。
2. master 执行 EXPIRE a 3。
3. 确认 slave 收到的是绝对过期时间，而不是 3 秒这个相对值。
4. 等待过期后，确认 master 删除时广播 DEL，slave 最终也删除。

### 9.4 PERSIST 验证

1. master 执行 SET b 2。
2. master 执行 EXPIRE b 60。
3. master 执行 PERSIST b。
4. 确认 slave 跟随清 TTL，TTL 变成永不过期。

### 9.5 覆盖写清 TTL 验证

1. master 执行 SET c 3。
2. master 执行 EXPIRE c 60。
3. master 再执行 SET c 4 或对应后端的 MOD / HSET / SSET。
4. 确认 slave 通过同样的覆盖写把 TTL 清掉。

### 9.6 全量同步验证

1. master 准备一个永久 key 和一个带 TTL 的 key。
2. 新 slave 连接 master。
3. 确认全量同步后，永久 key 仍然存在，TTL key 仍然带有 expire_at_ms。
4. 确认已过期 key 不会进入 slave 的全量结果。

## 10. 本步结论

TCP 路径的 TTL 主从同步，最小正确实现就是：

- master 传播 TTL 相关命令和删除动作。
- slave 只跟随回放，不主动过期。
- EXPIRE 传播绝对过期时间，PERSIST 传播清 TTL，过期删除传播 DEL。

这是一套改动很小、语义很稳、测试很好写的方案，适合 KVLite 当前阶段的 TTL MVP。

## 11. 下一步建议

下一步建议先补 TCP 路径的回归测试，再观察是否需要进一步收敛协议：

1. 验证 EXPIRE / PERSIST / DEL / 覆盖写清 TTL 的复制一致性。
2. 验证 master 新增 slave 的全量同步结果。
3. 如果测试稳定，再考虑是否需要把更多 TTL 边界行为做得更细。
