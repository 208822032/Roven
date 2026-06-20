# 第 7C 步：分析 eBPF 主从复制路径中的 TTL 接入方案

## 1. 本步目标

本步只做分析和设计，不改代码。

目标是在不破坏 TCP 路径已验证正确的 TTL 语义前提下，把 TTL 同步扩展到 eBPF 路径，并尽量做到语义一致、行为可解释、后续实现最小化。

本步需要回答的问题是：

1. 当前 eBPF 复制链路到底是怎么工作的。
2. 它和 TCP 复制路径的本质差异在哪里。
3. TTL 在 eBPF 路径里应该复用哪些 TCP 语义。
4. 如何设计一个最小正确的 eBPF TTL 复制方案。
5. eBPF slave 应该主动过期，还是完全跟随 master。

## 2. 当前 eBPF 复制链路分析

### 2.1 eBPF 路径不是 TCP 路径的替代实现

从当前代码看，eBPF 路径和 TCP 路径不是同一种复制模型。

TCP 路径是在 master 命令执行之后，由复制模块把写动作编码后广播给 slave；而 eBPF 路径是在 master 侧通过内核探针抓取进入 master 的 TCP 接收流，再由用户态守护进程把抓到的原始字节流原样转发给 slave。相关入口可以看 [src/eBPF/ebpf_kern.c](src/eBPF/ebpf_kern.c#L82)、[src/eBPF/ebpf_syncd.c](src/eBPF/ebpf_syncd.c#L666) 和 [src/eBPF/ebpf_syncd.c](src/eBPF/ebpf_syncd.c#L849)。

这意味着 eBPF 路径的复制单位不是“master 已确认执行后的语义动作”，而是“master 收到的请求字节流”。

### 2.2 内核侧做的事情

内核程序只负责抓取目标 master 连接上的 tcp_recvmsg 数据，并把收到的 payload 送到 ringbuf。它关心的是字节流、连接目标和分片，不理解命令语义，也不理解 TTL。代码里已经明确写了“复制流当成纯字节流，只保证顺序，不保证边界”。参见 [src/eBPF/ebpf_kern.c](src/eBPF/ebpf_kern.c#L16) 和 [src/eBPF/ebpf_kern.c](src/eBPF/ebpf_kern.c#L82)。

所以 eBPF kernel 程序本身不能成为 TTL 语义来源，它只是一层抓流工具。

### 2.3 用户态守护进程做的事情

`ebpf_syncd` 负责三件事：

1. 从 ringbuf 取出 master 收到的原始请求字节。
2. 通过自己的 TCP 连接把这些字节转发给所有 slave。
3. 启动时先连 master 执行 FULLDUMP，再把 FULLDUMP 的响应原样广播给 slave。

相关代码在 [src/eBPF/ebpf_syncd.c](src/eBPF/ebpf_syncd.c#L429)、[src/eBPF/ebpf_syncd.c](src/eBPF/ebpf_syncd.c#L571) 和 [src/eBPF/ebpf_syncd.c](src/eBPF/ebpf_syncd.c#L729)。

这说明 eBPF 路径的用户态层也不是一个“语义复制器”，而是一个“字节流中继器”。

### 2.4 slave 侧的回放方式

slave 侧通过 REPLSYNC 标记复制连接后，会把收到的命令按本地命令处理链执行。REPLSYNC 只是在连接建立阶段把连接标记为复制通道，后续命令仍然走本地解析和执行，相关代码在 [src/network/ntyco.c](src/network/ntyco.c#L107)、[src/network/reactor.c](src/network/reactor.c#L224) 和 [src/network/proactor.c](src/network/proactor.c#L128)。

这意味着 eBPF slave 的执行模型是“收到什么命令就本地执行什么命令”，而不是“收到 master 计算后的最终写动作”。

### 2.5 FULLDUMP 的现状

eBPF 路径的全量同步依赖 master 端的 FULLDUMP。当前 FULLDUMP 会过滤已过期 key，但只输出 SET、RSET、HSET、SSET 这样的普通写命令，不会把 expire_at_ms 单独带出来。相关实现见 [src/core/kvs_operation.c](src/core/kvs_operation.c#L1099)。

这说明：

1. 已过期数据不会被全量同步回去，这是正确的。
2. 但 TTL 剩余时间和绝对过期点没有被保留下来，这会导致新 slave 只能恢复 value 语义，恢复不了完整 TTL 语义。

## 3. eBPF 与 TCP 在复制路径上的差异

### 3.1 复制时机不同

TCP 路径是在 master 命令执行后做复制，语义上更接近“回放 master 已确认的写动作”。

eBPF 路径是在 master 接收请求阶段抓流，复制对象更接近“客户端请求本身”。这会带来一个直接后果：eBPF 天然不知道 master 最终是否成功执行了该命令，也不知道命令执行后存储层的最终状态。

### 3.2 复制粒度不同

TCP 路径复制的是经过编码的操作语义，例如 DEL、PERSIST、PEXPIREAT 这类已归一化的动作。

eBPF 路径复制的是原始 RESP 字节流。它不理解命令边界之外的语义，也不会自动把 EXPIRE 折叠成 PEXPIREAT，更不会自动补出 TTL 的绝对过期时间。

### 3.3 是否拥有 master 的最终裁判权不同

TCP 路径天然把 master 放在唯一裁判位置上：master 决定写什么、删什么、TTL 何时生效、何时删除，然后把结果广播给 slave。

eBPF 路径如果只保留当前这种原始请求中继方式，那么 TTL 的最终裁判会被拆散到 master 和 slave 两边：master 与 slave 都会各自按本地时钟和本地执行时刻解释 EXPIRE，这和 TCP 路径的语义不一致。

### 3.4 全量同步形态不同

TCP 路径的全量同步是把 master 当前快照按复制协议重新编码，已经可以在协议层直接附带 TTL 状态。

eBPF 路径的全量同步是 FULLDUMP RESP 字节流，当前只包含 key/value，不包含 TTL 侧信息。因此它天然更容易丢失 TTL 语义。

### 3.5 slave 角色的差异

两条路径最终都应该保持一个共同原则：slave 不应该成为新的 TTL 裁判。

但实现层面不同：

1. TCP slave 是按 master 广播的语义动作回放。
2. eBPF slave 是按收到的请求流回放。

所以 eBPF 要想对齐 TCP，必须把 TTL 从“请求流”提升为“规范化语义流”。

## 4. eBPF 路径中 TTL 的语义设计

### 4.1 设计原则

eBPF 路径的 TTL 语义应尽量复用 TCP 路径已经验证正确的原则：

1. master 是 TTL 的唯一裁判者。
2. slave 不主动过期，不单独跑 TTL 扫描线程。
3. TTL 传播使用绝对过期时间 expire_at_ms，而不是相对秒数。
4. 过期删除最终统一收敛成 DEL。
5. 覆盖写清 TTL 仍然由存储层自身完成，不单独发一条清 TTL 消息。

### 4.2 为什么不能直接复用原始请求流

如果 eBPF 路径直接把 EXPIRE、PEXPIRE 之类的原始请求流转发给 slave，那么 slave 会在自己的接收时刻重新计算 TTL，结果会受到网络延迟、调度抖动、时钟偏差影响。

这和 TCP 方案的目标相冲突。TCP 方案已经明确把 TTL 传播成绝对时间，就是为了避免漂移。因此 eBPF 不能只做“原样搬运”，而必须做“语义归一”。

### 4.3 最小正确语义

最小正确的 eBPF TTL 语义不是“所有东西都改成协议层重写”，而是：

1. 普通写命令仍然可以沿用当前 eBPF 的原始字节流中继。
2. TTL 相关动作必须从原始请求里抽离出来，走一条规范化语义路径。
3. 这条规范化路径要和 TCP 语义保持一致，核心就是 PEXPIREAT、PERSIST、DEL 这三类动作。

也就是说，eBPF 的角色是运输层和触发层，不是 TTL 语义的最终来源。

### 4.4 统一语义的关键点

要让 eBPF 和 TCP 语义尽量一致，必须坚持两个统一：

1. 统一 TTL 表示：都使用 expire_at_ms。
2. 统一删除裁判：都由 master 决定何时删，slave 只回放。

只要这两个统一成立，具体是从 TCP 协议还是从 eBPF 字节流送达 slave，差异就会缩小到实现细节层。

## 5. eBPF 路径中 EXPIRE / PERSIST / DEL / 覆盖写清 TTL / 过期删除 的传播方案

### 5.1 EXPIRE 的传播方案

EXPIRE 不能直接按原始相对秒数在 eBPF 路径里盲转发。

推荐方案是：master 在命令层把 EXPIRE 规范化成绝对 expire_at_ms，再把 PEXPIREAT 作为标准 TTL 动作送入 eBPF 的复制流。

如果 expire_at_ms 已经落到过去，或者 EXPIRE 结果等价于立即失效，那么不应该再发一个马上过期的 TTL 命令，而应该直接收敛成 DEL。

### 5.2 PERSIST 的传播方案

PERSIST 仍然是清 TTL。它应该作为一个独立的语义动作传播，而不是依赖覆盖写的副作用来间接达成。

在 eBPF 路径里，PERSIST 应该和 TCP 保持一致：slave 收到后把 expire_at_ms 置为永不过期。

### 5.3 DEL 的传播方案

DEL 是最终收敛动作。

无论是用户显式 DEL，还是 master 因 TTL 到期而删除，最终都应该让 slave 收到 DEL，并删除本地副本。这样 eBPF 和 TCP 的最终一致性语义才能对齐。

### 5.4 覆盖写清 TTL 的传播方案

覆盖写清 TTL 不建议单独造一个“清 TTL 消息”。

原因是当前存储层已经把覆盖写定义成重置 TTL 的动作。对 eBPF 来说，最稳妥的方式是继续让覆盖写命令本身进入复制流，由 slave 按本地存储语义自动清掉 expire_at_ms。

所以 SET、RSET、HSET、SSET、MOD、RMOD、HMOD、SMOD 仍然按原写命令传播即可，不需要额外拆出清 TTL 子消息。

### 5.5 过期删除的传播方案

当 master 因惰性删除或后台回收发现 key 已过期时，应该本地先删，再向 eBPF 复制流发送 DEL。

这样 slave 不需要自己判断过期点，也不会因为时钟差先删或晚删。

### 5.6 推荐的传播矩阵

| 场景 | master 侧动作 | eBPF 侧建议传播 | slave 侧结果 |
| --- | --- | --- | --- |
| EXPIRE 成功 | 计算绝对过期点 | PEXPIREAT key expire_at_ms | 写入绝对过期时间 |
| EXPIRE 立即失效 | 本地直接删除 | DEL key | 删除副本 |
| PERSIST 成功 | 清 TTL | PERSIST key | 清除过期时间 |
| DEL | 删除 key | DEL key | 删除副本 |
| 覆盖写 | 写入新值并清 TTL | 原写命令 | 覆盖后清 TTL |
| 过期删除 | master 本地删 | DEL key | 删除副本 |

## 6. eBPF slave 行为策略

本步建议明确选择：eBPF slave 不主动过期。

具体要求如下：

1. 不启动独立 TTL 扫描线程。
2. 不根据本地时钟自行删除 key。
3. 不把本地过期判断变成新的写来源。
4. 只回放 master 发来的规范化 TTL 动作和 DEL。

这样做的理由和 TCP 路径一致：最容易做对，最容易调试，也最不容易被时钟偏差搞乱。

如果未来需要读路径兜底，可以考虑只做读取时的可见性过滤，但不要让 slave 产生独立删除行为。

## 7. 推荐修改文件列表

下面是如果后续真正落地 eBPF TTL 方案，建议优先看的文件：

1. [src/eBPF/ebpf_syncd.c](src/eBPF/ebpf_syncd.c)
2. [src/eBPF/ebpf_kern.c](src/eBPF/ebpf_kern.c)
3. [src/core/kvs_operation.c](src/core/kvs_operation.c)
4. [src/core/kvstore.c](src/core/kvstore.c)
5. [src/network/ntyco.c](src/network/ntyco.c)
6. [src/network/reactor.c](src/network/reactor.c)
7. [src/network/proactor.c](src/network/proactor.c)

### 7.1 为什么是这些文件

- [src/eBPF/ebpf_syncd.c](src/eBPF/ebpf_syncd.c) 是 eBPF 增量和全量同步的用户态中继，TTL 语义如果要分流，最可能从这里切入。
- [src/eBPF/ebpf_kern.c](src/eBPF/ebpf_kern.c) 负责抓 master 的接收流，如果未来需要更精确地过滤或标记 TTL 相关流量，这里会受影响。
- [src/core/kvs_operation.c](src/core/kvs_operation.c) 的 FULLDUMP 现在只输出 value 语义，TTL 要真正保留下来，必须补全这里的导出语义。
- [src/core/kvstore.c](src/core/kvstore.c) 是命令执行后的统一出口，eBPF 若要和 TCP 语义统一，最终还是要回到这里的命令层判断。
- [src/network/ntyco.c](src/network/ntyco.c)、[src/network/reactor.c](src/network/reactor.c) 和 [src/network/proactor.c](src/network/proactor.c) 负责 REPLSYNC 和复制连接上下文，TTL 复制如果继续走 RESP 命令流，这些地方需要保持兼容。

## 8. 风险点

### 8.1 原始请求流无法表达最终语义

eBPF 当前抓的是收到的请求字节，不是 master 执行后的结果。它天然不知道命令是否成功，也不知道 TTL 的最终绝对时间。这是最大的语义风险。

### 8.2 FULLDUMP 目前会丢 TTL

当前 FULLDUMP 只输出 SET、RSET、HSET、SSET，不携带 expire_at_ms。即使已过期 key 被过滤掉，剩余有效 key 的 TTL 仍然会在新 slave 上丢失。

### 8.3 slave 自己算 TTL 会分叉

如果 eBPF slave 按自己收到命令的时间重新解释 EXPIRE，就会出现 master 和 slave 过期点不一致的问题。这个问题和 TCP 路径已经避免掉的风险是同一个。

### 8.4 全量和增量可能交错

eBPF 先做 FULLDUMP，再进入增量字节流中继。如果 TTL 动作和普通写命令在这两个阶段交错，语义上需要明确顺序，否则可能出现短暂不一致。

### 8.5 协议兼容性

如果 eBPF 路径最终需要把 TTL 从原始请求流提升为规范化语义流，那么 slave 端必须能够识别 PEXPIREAT、PERSIST 以及 DEL 这类 TTL 动作，否则新旧行为会混杂。

## 9. 验证思路

### 9.1 先确认基础复制不被破坏

先验证 eBPF 路径下普通 SET、DEL、MOD、RSET、HSET、SSET 的复制行为不变，避免 TTL 设计把现有路径打坏。

### 9.2 TTL 传播验证

重点验证以下场景：

1. master 执行 EXPIRE 后，slave 最终拿到的是绝对过期时间，而不是相对秒数。
2. master 执行 PERSIST 后，slave 的 TTL 被清除。
3. master 执行覆盖写后，slave 的 TTL 自动清空。
4. key 到期后 master 删除，slave 也收到 DEL。

### 9.3 全量同步验证

验证新 slave 加入时：

1. 永不过期 key 能正确恢复。
2. 带 TTL 的 key 能恢复绝对过期时间。
3. 已过期 key 不会进入全量结果。

### 9.4 一致性验证

同时对比 TCP 和 eBPF 两条路径，确认同一批 TTL 操作在两种复制方式下，最终可见状态一致。

### 9.5 调试重点

调试时重点看三处：

1. master 是否把 TTL 相关动作规范化。
2. eBPF 守护进程是否把该动作送到了 slave。
3. slave 是否按同一套 TTL 语义落库。

## 10. 本步结论

eBPF 路径和 TCP 路径的本质差异，不在于网络是否走 TCP，而在于复制语义的来源不同。

TCP 是“master 已确认语义动作的广播”，eBPF 是“master 接收流的原样中继”。因此，如果想让 eBPF 的 TTL 行为与 TCP 尽量一致，就不能只依赖原始请求流，必须把 TTL 从请求流中提升为规范化语义流，并继续坚持 master 唯一裁判、slave 被动回放、绝对过期时间、过期删除统一 DEL 这四条原则。

最小正确的 eBPF TTL 方案不是重做一套复制协议，而是在现有 eBPF 字节流中继之上，为 TTL 单独建立语义对齐层，并让 FULLDUMP 和增量流都服从同一套 TTL 规则。

## 11. 下一步建议

如果要进入实现阶段，建议按下面顺序推进：

1. 先补 FULLDUMP 的 TTL 语义，避免新 slave 丢失 expire_at_ms。
2. 再把 EXPIRE、PERSIST、DEL 的 eBPF 复制流收敛到和 TCP 一致的规范化动作。
3. 最后再检查 slave 侧是否需要额外的 TTL 命令兼容和回放校验。

这条顺序的优点是先保证全量正确，再处理增量一致，最后处理边界兼容，风险最低。