# 第十章 主从复制：从写命令广播到 eBPF 辅助同步

## 10.1 为什么有了 AOF/RDB 还需要主从复制

前面持久化那一章里，KVLite 已经实现了 AOF 和 RDB。

AOF 负责把写命令追加到本地日志文件里，RDB 负责把当前内存状态保存成本地快照。它们解决的是一个很具体的问题：**进程退出、崩溃、重启之后，数据怎么恢复回来。**

最开始我也容易把这个问题想简单：既然 AOF 能恢复，RDB 也能恢复，那是不是已经足够安全了？

后来才发现，这里面少了一个维度。

AOF/RDB 的前提是：机器还能回来，文件还能读，进程还能重新启动。如果 master 所在机器直接宕机，AOF/RDB 文件还躺在那台机器的磁盘上，但服务对外仍然不可用。客户端不能等我手动登录机器、拷文件、重启服务之后再恢复访问。

所以主从复制要解决的不是“本机数据能不能回来”，而是：

```text
同一份数据，能不能出现在另一个正在运行的节点上。
```

这件事带来的意义和持久化不一样：

- 有了 slave，master 之外还有一份内存副本。
- 有了副本，后续才可能做读扩展。
- 有了副本，master 出问题时才有故障切换的基础。
- 有了副本，也可以把某些备份、分析、迁移工作从 master 上挪开。

我后来总结成一句话：

> AOF/RDB 解决的是数据能不能回来，主从复制解决的是数据能不能出现在另一个节点上。

这也是第十章要复盘的核心。

## 10.2 复制解决的不是重启恢复，而是多副本和可用性

AOF、RDB、Replication 都和“写命令”有关，但它们的目标完全不同。

**AOF** 是本地写命令日志。每条成功的写命令被追加到本地文件，恢复时再从头重放。它关心的是时间维度：系统崩溃后，能不能回到崩溃前某个时间点。

**RDB** 是本地内存快照。它把某个时刻的 key、value、TTL 序列化成文件，恢复时直接加载。它同样关心时间维度，只是恢复粒度从“命令历史”变成了“某个状态快照”。

**Replication** 是远端状态追赶。master 写成功之后，把变化传给 slave，让另一个进程、另一台机器上的内存状态持续接近 master。它关心的是空间维度：数据有没有出现在别的节点。

所以三者可以这样区分：

```text
AOF         本地命令日志       目标：本机重放恢复
RDB         本地内存快照       目标：本机快速恢复
Replication 远端状态追赶       目标：多副本和可用性
```

这里有两个边界特别重要。

第一，AOF 不等于复制。AOF 写到了本地文件，不代表 slave 拿到了这条命令。如果 master 所在机器挂了，文件存在也不能自动变成在线副本。

第二，复制也不等于持久化。slave 可能已经同步了 master 的内存状态，但如果 slave 没有开启 AOF/RDB，它自己重启后照样会丢数据。

所以在设计上，AOF/RDB 和主从复制不是互相替代，而是分别覆盖不同风险：

```text
本机重启恢复  → AOF / RDB
运行时多副本  → Replication
```

## 10.3 最朴素的复制思路：写命令执行成功后广播给 slave

做主从复制时，最自然的想法是：master 执行了一条写命令，就把这条写命令发给 slave。

这个思路和 AOF 很像。

```text
AOF         把成功写命令写到本地文件
Replication 把成功写命令写到远端连接
```

但这里的关键不是“发命令”本身，而是**什么时候发**。

KVLite 的主命令入口在 `src/core/kvstore.c` 的 `kvs_filter_protocol`。命令经过 RESP 解析后，会查 `command_table`，构造 `command_context_t`，然后调用对应 handler：

```c
ret = cmd->handler(&ctx, response);

if (ret >= 0 && cmd->write_command) {
    aof_append(...);
    master_sync_command(...);
}
```

真实源码里还会处理锁、AOF loading、TTL 转换、同步上下文等细节，但核心顺序就是这几步：

```text
解析命令
→ 参数校验
→ 执行 handler
→ handler 成功
→ AOF append
→ replication broadcast
```

这个顺序保证了几个语义：

- 失败命令不能复制。
- 读命令不能复制。
- AOF/RDB 加载阶段不能再次复制。
- slave replay 阶段不能再次广播。
- TTL 这类带时间语义的命令，要先归一化再复制。

当前 TCP 主从复制挂载在 `master_sync_command` 上。它把命令转换成 `master_slave_proto` 里的 op，再交给 `sync_backend_broadcast_owned`，最终由 TCP 后端线程发给所有 slave。

这也是我理解复制之后最大的变化：复制不是 handler 里面顺手多写一行 send，而是写命令执行成功之后的一个统一副作用。

## 10.4 command_table 的 write_command 如何成为复制挂载点

第五章讲 `command_table` 时，重点是把命令分发从 switch-case 演进成表驱动。到了复制这里，`command_table` 的价值又体现出来了。

复制层不应该自己维护一份判断逻辑：

```text
SET 是写命令
DEL 是写命令
EXPIRE 是写命令
VSET 是写命令
VGET 不是写命令
...
```

如果复制层、AOF 层、权限层各自判断一遍，新增命令时一定会漏。比如新增一个 `VDEL`，handler 已经写好了，AOF 也许忘记加，复制也许忘记加，slave 就会和 master 不一致。

KVLite 在 `command_info` 里放了 `write_command` 字段：

```c
typedef struct {
    const char *name;
    command_handler handler;
    int arity;
    int write_command;
    int data_structure;
    kvs_cmd_lock_mode_t lock_mode;
} command_info;
```

这让命令的“读写语义”集中在命令表里。`kvs_filter_protocol` 不需要知道某条命令具体改了哪个数据结构，只需要判断：

```text
ret >= 0 && cmd->write_command
```

这一个条件就能同时服务于：

- AOF append。
- 主从复制。
- slave 只读限制。
- 将来可能加入的 metrics、watcher、binlog。

当前源码里 TCP 复制的 `master_sync_command` 还需要把命令名映射成 `MS_OP_ARRAY_SET`、`MS_OP_HASH_HSET` 这类 op，这是协议层的映射问题。但“这条命令该不该进入复制路径”，应该由 `write_command` 决定。

这就是 command_table 作为横切能力挂载点的意义：它不只是为了少写 switch，而是把命令元信息变成系统能力的共同入口。

## 10.5 全量同步：slave 第一次连接时如何追上 master 当前状态

slave 第一次连接 master 时，不能只接收之后的新写命令。

假设 master 已经运行了一小时，内存里有 100 万个 key。此时 slave 刚启动，如果它只从连接建立之后开始接收增量写命令，那么之前那 100 万个 key 它永远没有。

所以复制必须分成两段：

```text
全量同步：解决 slave 初始状态问题
增量同步：解决全量之后的持续变化问题
```

当前 TCP 复制实现中，全量同步不是直接复用 RDB 文件，而是更接近“命令重放式全量同步”。

`src/distributed/distributed.c` 里的 `slave_sync_thread` 在 master 接受 slave 连接之后，会遍历当前内存里的几种数据结构：

- array 里的有效 key。
- rbtree 里的有效节点。
- hash 表里的有效 slot。
- skiptable 里的有效节点。

遍历时会跳过已经过期的 key。对每个还活着的 key，master 调用 `master_sync_command` 发出等价写命令，比如 `SET`、`RSET`、`HSET`、`SSET`。如果 key 带 TTL，再追加一条 `PEXPIREAT key expire_at_ms`。

也就是说，这里的全量同步不是“把内存块原样发过去”，而是：

```text
当前内存状态
→ 转成一批等价写命令
→ slave 按顺序应用
→ 得到近似相同的状态
```

eBPF 模式里还有另一条全量路径。`src/eBPF/ebpf_syncd.c` 里的 `do_fullsync_from_master` 会连到 master 的普通服务端口，发送 `FULLDUMP` 命令。`handle_fulldump` 会遍历数据结构并生成 RESP 命令流，然后 syncd 把这些数据广播给 slave。

所以当前源码里存在两种全量表达：

```text
TCP 分布式模块：遍历数据结构，编码成 master_slave_proto 消息
eBPF syncd：发送 FULLDUMP，拿到 RESP 命令流后转发
```

它们的共同点是：都没有实现 Redis PSYNC 那种 offset/backlog 体系。当前更准确的描述是“第一次连接做全量，后续靠增量广播继续追赶”。

## 10.6 增量同步：后续写命令如何持续发送给 slave

全量同步完成后，master 还会继续收到客户端写命令。slave 要保持接近 master，就必须持续接收这些增量变化。

当前 TCP 模式下，增量同步的路径是：

```text
客户端写命令
→ kvs_filter_protocol
→ handler 执行成功
→ master_sync_command
→ ms_encode_message
→ sync_backend_broadcast_owned
→ msg_queue
→ TCP backend worker
→ send 到所有 slave
```

`master_sync_command` 会把命令名映射为 `ms_op_t`。比如：

```text
SET       → MS_OP_ARRAY_SET
HSET      → MS_OP_HASH_HSET
SDEL      → MS_OP_SKIPTABLE_SDEL
PEXPIREAT → MS_OP_TTL_PEXPIREAT
PERSIST   → MS_OP_TTL_PERSIST
```

然后 `master_slave_proto.c` 负责把 op、key_len、val_len、key、value 编成一条带长度的二进制消息。

增量同步最怕几个问题：

- 顺序错了。
- 命令漏了。
- 命令重复执行了。
- slave 断开后不知道从哪里补。

当前实现已经把“消息边界”和“发送线程”拆出来了，但还没有完整的 offset/backlog。也就是说，slave 在线时能持续收到增量；如果断线，当前更稳妥的做法仍然是重新全量同步，而不是按 offset 部分追赶。

这也是我在正文里会谨慎表达的地方：当前 KVLite 有全量 + 在线增量复制雏形，但还不是生产级 Redis 复制。

## 10.7 为什么复制不能发生在 handler 执行之前

复制最容易犯的错误，是在 handler 执行之前就把命令发出去。

看起来这样更简单：

```text
收到 SET
→ 先发给 slave
→ master 再执行 SET
```

但这会破坏一致性。

比如 master 收到一条写命令，可能在很多地方失败：

- 参数数量不对。
- key 不存在，某些修改命令返回失败。
- value 超过限制。
- 内存分配失败。
- 存储结构写入失败。
- TTL 参数非法。

如果先复制，master 失败了，但 slave 成功执行了，主从状态就分叉了。

正确顺序必须是：

```text
解析命令
→ 校验参数
→ 执行 handler
→ handler 成功
→ AOF append
→ replication broadcast
```

这就是 `ret >= 0 && cmd->write_command` 这个判断的重要性。复制不是命令执行的一部分，而是写命令成功之后的副作用。

eBPF 辅助同步这里要额外说明。eBPF 捕获的是 master TCP 收包路径，也就是从内核侧看到“客户端发来了什么”。这降低了业务代码侵入，但它天然不知道 handler 最终是否成功。`ebpf_syncd.c` 里会解析 RESP 并过滤写命令，但它不能像 `kvs_filter_protocol` 那样直接拿到 handler 的返回值。

所以 eBPF 模式更适合在当前项目里作为旁路同步探索，而不是被夸大成严格复制语义的完整替代品。

## 10.8 TTL 在复制中的特殊问题：EXPIRE 为什么要转成 PEXPIREAT

TTL 在复制里有一个非常典型的坑：相对时间不能原样复制。

客户端在 master 上执行：

```text
EXPIRE token 10
```

这条命令的语义是：从 master 执行这一刻开始，10 秒后过期。

如果 master 原样把 `EXPIRE token 10` 发给 slave，slave 收到时可能已经过了几十毫秒、几百毫秒，甚至更久。slave 再从自己的接收时刻开始计 10 秒，就会比 master 晚过期。

这不是实现细节，而是语义不一致。

更合理的做法是：master 在执行 EXPIRE 后，拿到真实的绝对过期时间：

```text
expire_at_ms = master_now_ms + ttl_ms
```

然后复制：

```text
PEXPIREAT token expire_at_ms
```

这样 slave 不需要猜 master 是什么时候执行的，只要把 key 的过期时间设置成同一个绝对毫秒时间戳。

当前源码在 `kvs_filter_protocol` 里就是这个思路。`EXPIRE` 执行成功后，通过 `ttl_get_any_key_expire_at_ms` 取出实际的 `expire_at_ms`，AOF 和 TCP 复制都会把它转换成 `PEXPIREAT`。如果过期后 key 已经不存在，则同步 `DEL`。

eBPF syncd 里也有类似归一化逻辑：`normalize_and_broadcast_command` 解析到 `EXPIRE` 后，会把它转换为 `PEXPIREAT` 或 `DEL` 再广播。

这和 TTL 那一章形成呼应：TTL 不是给 key 加字段这么简单，它会影响持久化和复制的命令语义。

## 10.9 复制协议：为什么不能只靠普通文本转发

一开始做复制，很容易想直接发字符串：

```text
SET name zhengpan\r\n
DEL name\r\n
```

这种方式可读性好，但很快会遇到问题。

TCP 是字节流，没有消息边界。一次 `send` 不等于一次 `recv`，一条命令可能被拆成两次收到，两条命令也可能粘在一起。如果没有长度字段，接收端就很难可靠地区分消息边界。

另外，复制链路不只需要“命令文本”，还可能需要表达状态：

- 全量同步开始。
- 全量数据块。
- 增量命令。
- ACK。
- 心跳。
- 错误。
- 版本号。
- offset。

当前 `master_slave_proto.c` 采用的是一个很小的结构化二进制协议。头部包含：

```text
magic
version
op
key_len
val_len
reserved
```

后面再跟 key 和 value 的原始字节。`ms_decode_message` 根据 `key_len` 和 `val_len` 判断一条消息是否完整：

```text
返回 >0：解出一条完整消息，消费这些字节
返回  0：数据不够，继续 recv
返回 <0：协议错误
```

这就把 TCP 半包、粘包问题从“靠猜”变成了“按长度解析”。

需要注意的是，当前 TCP 协议还比较轻量，只覆盖 op + key/value，并没有实现 ACK、offset、backlog、心跳等生产级复制状态。但它已经比随便拼字符串更适合扩展。

eBPF 模式里则更多使用 RESP 作为 payload。RESP 本身也是结构化协议，有数组长度和 bulk string 长度。`ebpf_syncd.c` 通过 `resp_peek_command` 从捕获到的字节流里识别完整命令，再决定是否广播。

所以这里的结论不是“文本一定不行”，而是：复制链路必须有明确消息边界和状态表达。RESP 可以作为 payload，但外层仍然需要管理复制连接、全量/增量阶段和失败处理。

## 10.10 slave 侧如何复用 RESP 解析和命令执行链路

理想情况下，slave 收到 master 的写命令后，不应该重新写一套存储逻辑。

最好的路径是复用已有链路：

```text
RESP parse
→ command_table
→ handler
→ 存储层 ops
```

这样做的好处很明显：

- 参数校验复用同一套逻辑。
- 写命令语义和客户端直连一致。
- 新增命令时不需要在 slave replay 再写一遍。
- AOF、TTL、锁、二进制安全等能力更容易保持一致。

当前源码里有两条实现路线。

第一条是 TCP 分布式模块。`slave_connect_master` 从 master 同步端口读取 `master_slave_proto` 二进制消息，解码出 op、key、value 后，直接调用 `kvs_array_set`、`kvs_hash_set`、`kvs_skiptable_set`、`ttl_set_any_key_expire_at_ms` 等底层接口。这条路径性能直接，但确实没有完整复用 RESP/command_table，所以新增命令时要同步扩展 `ms_op_t` 和 slave apply 逻辑。

第二条是 eBPF syncd 路线。syncd 与 slave 建立普通 TCP 连接，先发送 `REPLSYNC`。网络层在 reactor、proactor、ntyco 几种实现里都会把该连接标记为复制连接。后续 syncd 转发 RESP 命令到 slave 时，slave 仍然走 `kvs_handler` / `kvs_filter_protocol`，只是通过 `set_sync_context(2)` 标记这是复制上下文。

这个上下文标记很重要：

```text
普通客户端写 slave  → 拒绝，返回 READ ONLY
复制连接写 slave    → 允许，用于 replay master 命令
replay 期间         → 不再向外广播，避免循环复制
```

所以更准确地说：当前 TCP 复制路径还没有完全复用命令执行链路；eBPF 同步路径更接近复用 RESP 和 command_table 的设计。后续如果要统一架构，我会倾向于让 TCP 复制的 slave apply 也尽量复用命令层，而不是长期维护两套写入语义。

## 10.11 eBPF 辅助同步：为什么要从内核侧捕获主节点流量

传统 TCP 复制是在业务代码里主动调用 broadcast。比如 `kvs_filter_protocol` 里 handler 成功后调用 `master_sync_command`。

eBPF 辅助同步换了一个思路：既然客户端写命令一定会经过 master 的 TCP 收包路径，那能不能从内核侧观察这段流量，把写命令旁路复制出去？

当前 `src/eBPF/ebpf_kern.c` 挂在 `tcp_recvmsg` 的 kprobe / kretprobe 上：

- kprobe 入口记录 `msghdr` 里的用户态缓冲区位置。
- kretprobe 返回时，根据 `ret` 知道这次真正收到了多少字节。
- 如果本地地址和端口匹配配置里的 master，就把这段字节复制到 ringbuf 事件里。

这样做的好处是业务侵入更低。主业务代码不需要在每个写命令路径里显式调用 eBPF broadcast，syncd 作为旁路进程观察 master 收到的命令流。

但这个方案也有明显边界。

eBPF 捕获到的是字节流，不是业务命令。它不知道 RESP 命令边界，不知道 handler 是否成功，也不适合在内核态做复杂解析。内核 eBPF 程序还要受 verifier 限制，不能随意循环、分配内存、调用复杂库函数。

所以当前设计里，eBPF 只负责捕获和上报：

```text
内核态：抓取 master tcp_recvmsg 收到的字节
用户态：解析 RESP、过滤写命令、做 TTL 归一化、广播给 slave
```

这更像一次“内核旁路同步探索”，而不是生产级强一致复制方案。

## 10.12 eBPF syncd 链路：抓包、解析、队列和广播

从源码结构看，eBPF 辅助同步的大致链路是：

```text
client 写命令
→ master TCP recv
→ eBPF kprobe/kretprobe 捕获 tcp_recvmsg 字节
→ BPF ringbuf 输出事件
→ ebpf_syncd 读取 ringbuf
→ 拼接成用户态 rx_stream
→ resp_peek_command 识别完整 RESP 命令
→ resp_parser 解析参数
→ 过滤写命令并归一化 EXPIRE
→ msg_queue 缓冲
→ tx_batcher 批量发送
→ slave 复制连接接收 RESP
→ slave 复用 kvs_filter_protocol 应用写命令
```

这里有几个设计点值得复盘。

第一，eBPF 程序只做捕获。`ebpf_kern.c` 里定义了 ringbuf map 和 config map，通过 `target_master` 判断当前 socket 是否是目标 master 端口。真正的 RESP 解析不放在内核态，而是交给用户态 syncd。

第二，syncd 要重新识别命令边界。因为 eBPF 上报的是 TCP recv 的字节片段，可能是半条命令，也可能包含多条命令。`g_rx_stream` 把事件数据累积起来，`process_rx_stream` 通过 `resp_peek_command` 一条条切完整 RESP 命令。

第三，syncd 会过滤写命令。`is_replicated_write_command` 只允许 SET/RSET/HSET/SSET、DEL、MOD、EXPIRE、PEXPIREAT、PERSIST、PAPER 等写类命令进入广播。

第四，syncd 在启动时会先做一次全量同步。`do_fullsync_from_master` 连到 master，发送 `FULLDUMP`，把 master 返回的 RESP 命令流广播给 slave。全量之后，再靠 ringbuf 捕获增量。

第五，syncd 面向多个 slave 时，每个 slave 有自己的 `slave_ctx_t`、队列和发送线程。广播时创建一个带引用计数的 `ebpf_sync_msg_t`，多个 slave 队列共享同一份消息数据。

这里我最深的感受是：eBPF 降低了业务路径侵入，但没有降低同步系统本身的复杂度。字节流重组、命令过滤、全量和增量衔接、队列堆积、发送失败，这些问题仍然要在用户态认真处理。

## 10.13 msg_queue 与 tx_batcher：为什么同步链路也需要削峰和批量发送

复制链路不能让每条命令都直接阻塞主流程。

如果 master 每执行一条写命令，就同步地对每个 slave 调一次 `send`，那么任何一个慢 slave 都可能拖慢 master。复制链路必须把“产生同步消息”和“真正发送到网络”解耦。

当前 TCP 后端在 `src/distributed/sync_backend_tcp.c` 里使用 `msg_queue`：

```text
master_sync_command
→ sync_backend_broadcast_owned
→ msg_queue_push
→ tcp_backend_worker
→ send to slaves
```

这样主命令路径只负责把消息放进队列，具体发送由后台 worker 做。

`msg_queue` 本身是一个环形队列，容量会向上取 2 的幂，通过 head/tail 原子变量推进。队列满时 `msg_queue_push` 返回失败，调用方需要决定丢弃、报错还是退避。当前实现里 TCP 后端会记录错误并释放消息。

eBPF syncd 里还加了 `tx_batcher`。每个 slave sender 线程不是拿到一条消息就立刻 send，而是先 append 到一块发送缓冲区里：

```text
消息进入 slave 队列
→ sender pop
→ tx_batcher_append
→ buffer 满或定时到期
→ tx_batcher_flush
→ send_all
```

这能减少系统调用次数，提高高写入场景下的吞吐。

但队列和批量发送也会带来风险：

- 队列过大，内存压力会上升。
- 队列满了，必须有明确策略。
- 批量发送会增加一点延迟。
- send 失败后，已经进入 batch 的数据怎么处理，需要谨慎。
- 多 slave 共享消息时，引用计数必须正确释放。

这里和前面网络层 send buffer 的思想是一致的：业务生成数据，发送链路异步处理。但复制链路多了一个要求：不能只追求吞吐，还要知道什么时候会丢、丢了之后系统处于什么状态。

## 10.14 主从复制和 AOF/RDB 的边界

主从复制、AOF、RDB 都围绕写命令和数据状态展开，但边界不同。

```text
AOF         本地命令日志，目标是本机重放
RDB         本地状态快照，目标是快速恢复
Replication 远端状态追赶，目标是多副本
```

同一条写命令成功后，可能同时触发多个副作用：

```text
handler 成功
→ AOF append
→ replication broadcast
→ 将来可能还有 metrics / watcher / binlog
```

这也是为什么我不希望把 AOF、复制、TTL、权限判断散落在各个 handler 里。handler 最好只表达“这条命令如何修改数据”，命令成功后的横切副作用统一挂在 `kvs_filter_protocol` 这类入口上。

当前源码里还有几个边界处理：

- AOF load 期间，`aof_state.loading` 会阻止再次 append。
- AOF load 期间会 `set_sync_context(2)`，避免恢复命令再次进入复制路径。
- slave 普通客户端写入会被 `ROLE_SLAVE` + `write_command` 拦住。
- 复制连接或加载上下文会绕过只读限制，但不能再次广播。
- TTL 的 EXPIRE 在 AOF 和复制里都尽量转成 PEXPIREAT。

这些判断看起来琐碎，但都是为了避免“恢复触发复制、复制触发恢复、slave 再广播出去”这种循环副作用。

## 10.15 这个模块最容易出 bug 的地方

1. 写命令还没执行成功就复制。master 本地失败但 slave 执行成功，会直接造成主从状态不一致。

2. 失败命令被复制。参数错误、内存失败、业务失败的命令不应该进入复制流，否则 slave 会重放一条 master 上没有生效的写。

3. 读命令被复制。GET、TTL、VSEARCH 这类命令没有状态变更，复制它们只会浪费带宽；如果读命令内部有副作用，还可能污染状态。

4. slave replay 时再次 broadcast。slave 应用 master 命令时如果又向外广播，轻则重复执行，重则形成复制风暴。

5. AOF load / RDB load 时把恢复命令又复制出去。恢复阶段是在重建本机状态，不是新的客户端写入，必须用 loading 或 sync_context 隔离。

6. EXPIRE 原样同步导致过期时间不一致。相对时间在网络传输中会漂移，必须转成 PEXPIREAT 这种绝对时间。

7. TCP 半包粘包导致同步协议解析错误。无论是 RESP 还是 master_slave_proto，都必须有长度字段和流式解析，不能假设一次 recv 就是一条消息。

8. 全量同步期间又来了增量写，顺序处理不当导致漏数据。当前实现是基础版本，后续要靠锁、offset 或 backlog 更严谨地处理全量与增量边界。

9. slave 断线重连后缺少 offset / backlog，无法部分追赶。没有复制位点时，只能重新全量，无法只补断开期间的增量。

10. eBPF 抓到的是字节流，命令边界识别错误。ringbuf 事件可能拆分 RESP 命令，用户态必须累积并用 RESP parser 切完整命令。

11. ringbuf 事件丢失导致同步缺口。eBPF ringbuf 满了或用户态消费慢，可能直接丢事件；一旦丢写命令，slave 状态就有缺口。

12. msg_queue 堆积导致内存压力。高写入、慢 slave、网络抖动叠加时，队列会变成内存风险点。

13. tx_batcher 批量发送失败后的重试和回滚问题。一批数据可能只发出一部分，断线后怎么恢复、是否重发、是否乱序，都需要明确策略。

## 10.16 可以关联哪些八股知识点

### C 语言八股

- struct 协议头设计：`master_slave_proto` 的 magic、version、op、长度字段就是典型例子。
- 指针和缓冲区生命周期：slave 解码后 key/value 指向接收缓冲区，使用前需要拷贝并补 `\0`。
- 函数指针 / 回调：`command_table` 的 handler、`tx_batcher` 的 `tx_send_fn` 都是回调思想。
- 内存拷贝和长度字段：key/value 不能只依赖 `strlen`，二进制安全依赖显式长度。
- 错误码设计：decode 返回 `>0 / 0 / <0`，分别表示完整、不够、错误。
- 状态机：RESP parser、复制连接状态、AOF loading 状态都可以按状态机理解。
- 资源释放顺序：队列、线程、socket、BPF link、ringbuf、内存池都要按依赖关系关闭。

### Linux / 网络八股

- TCP 字节流：复制协议必须处理半包和粘包。
- 非阻塞 IO：eBPF fullsync 里对 master socket 设置了非阻塞读取。
- epoll：reactor 网络模型用 epoll 管理连接。
- send buffer / recv buffer：复制链路也需要缓冲，不能直接让业务线程阻塞在 send 上。
- socket 断连处理：send 返回错误、recv 返回 0 都要关闭或重连。
- 系统调用开销：每条命令一次 send 成本高，`tx_batcher` 通过批量发送降低开销。
- `MSG_NOSIGNAL`：发送到断开的 socket 时避免 SIGPIPE 杀掉进程。

### eBPF 八股

- kprobe / kretprobe：入口记录参数，返回时根据 ret 读取实际收到的数据。
- `tcp_recvmsg`：这是当前内核侧捕获 master 收包的挂载点。
- BPF map：`config_map` 传入 master 地址端口，`recv_args_map` 暂存入口参数。
- ringbuf：内核态通过 ringbuf 把事件传给用户态 syncd。
- 内核态到用户态事件传递：eBPF 不直接发网络包，而是上报事件给用户态处理。
- verifier 限制：eBPF 程序不能像普通 C 程序那样随意循环和访问内存。
- eBPF 程序不能做复杂业务逻辑：RESP 解析和复制策略放在用户态更合理。

### 后端 / 分布式八股

- 主从复制：master 写入，slave 追赶。
- 全量同步：slave 第一次连接时拿到当前状态。
- 增量同步：全量之后持续发送写命令。
- 写命令重放：复制本质上是让 slave 重放 master 的状态变更。
- 幂等性：重复 DEL、重复 PEXPIREAT 的影响要考虑。
- 顺序性：同一个 key 的多次写必须按 master 顺序到达。
- offset / backlog：断线部分重同步的基础能力。
- 最终一致性：异步复制下，slave 通常会滞后 master。
- 故障恢复：master 挂了之后，是否能提升 slave，是复制之后的下一层能力。
- 读写分离：slave 有副本后才可能承担读流量。

## 10.17 面试中怎么讲主从复制

### 1 分钟版本

KVLite 做主从复制是因为 AOF/RDB 只能解决本机重启恢复，不能解决运行时单点问题。我的实现里，写命令先走正常 RESP 解析、command_table 分发和 handler 执行，只有 handler 成功且 `write_command` 为真，才会进入 AOF 和复制路径。TCP 模式下 master 会把写命令编码成自定义 master-slave 协议，异步广播给 slave；slave 第一次连接时先做全量同步，之后再接收增量写。TTL 里 `EXPIRE` 不能原样复制，会转成 `PEXPIREAT` 绝对时间，避免主从过期时间漂移。另外我还做了 eBPF 辅助同步探索，通过 kprobe/kretprobe 观察 master 的 `tcp_recvmsg` 流量，用户态 syncd 再解析 RESP、过滤写命令、广播给 slave，这部分降低业务侵入，但也有字节流边界和事件丢失等限制。

### 3 分钟版本

我做复制前先区分了持久化和复制的边界。AOF 是本地写命令日志，RDB 是本地快照，它们解决的是进程重启后数据怎么恢复；主从复制解决的是另一个节点能不能持续拥有数据副本，所以它是空间维度的多副本问题。

实现上我把复制挂在写命令成功之后。命令经过 RESP parser 还原参数，`command_table` 找到 handler，执行成功后通过 `ret >= 0 && cmd->write_command` 判断是否进入副作用路径。这样 AOF 和 replication 都不需要自己判断 SET、DEL、EXPIRE、VSET 哪些是写命令，统一依赖命令元信息。

TCP 复制分全量和增量。slave 第一次连接 master 的同步端口时，master 遍历 array、hash、rbtree、skiptable，把现有 key/value 转成 SET/HSET/RSET/SSET 等等价写命令发给 slave；带 TTL 的 key 再追加 PEXPIREAT。之后 master 每次写命令成功，调用 `master_sync_command`，把命令映射成 `ms_op_t`，通过 `master_slave_proto` 编成带 magic/version/op/key_len/value_len 的二进制消息，再放进 `msg_queue`，由 TCP backend worker 发给所有 slave。

slave 侧当前有两条路线。TCP 模式解码 master-slave 协议后直接调用底层存储接口应用 op；eBPF 模式更接近复用普通命令链路，syncd 给 slave 发送 REPLSYNC 后，slave 把该连接标记为复制连接，后续 RESP 命令仍然走 `kvs_filter_protocol`，只是通过 `sync_context` 避免只读拦截和重复广播。

TTL 是复制里的特殊点。EXPIRE 是相对时间，不能直接发给 slave，否则网络延迟会导致 slave 过期更晚。所以 master 会把 EXPIRE 转成 PEXPIREAT 的绝对毫秒时间再复制。

eBPF 辅助同步是一个旁路方案。内核程序挂在 `tcp_recvmsg`，用 ringbuf 把 master 收到的字节传给用户态 syncd。syncd 累积字节流，用 RESP parser 切完整命令，过滤写命令，做 EXPIRE 归一化，再通过每个 slave 的 `msg_queue` 和 `tx_batcher` 批量发送。它的好处是业务侵入低，限制是看不到 handler 是否真正成功，也要处理 ringbuf 丢事件、命令边界和调试复杂度。

### 面试官追问方向

**如果 slave 断线怎么办？**

当前基础实现更适合断线后重新全量同步。要支持部分追赶，需要引入 replication offset 和 backlog，让 slave 带着上次 offset 回来补增量。

**如何保证主从顺序？**

单条连接上 TCP 保序，队列和 batcher 也按 append 顺序发送。但多线程、多 slave、断线重连场景要额外设计 offset 和 ACK，否则只能保证在线期间的基本顺序。

**为什么 EXPIRE 不能直接同步？**

EXPIRE 是相对时间，slave 收到时已经晚了。复制应传 PEXPIREAT 绝对时间，让 master 和 slave 使用同一个过期时间戳。

**eBPF 抓包会不会丢？**

会有风险。ringbuf 满、用户态消费慢、内核读取失败都可能丢事件。所以当前 eBPF 同步更像旁路实验，要做生产级需要丢包统计、补偿和全量校验。

**复制和 AOF 谁先执行？**

当前主链路里 handler 成功后先处理 AOF，再处理 replication。更重要的是两者都必须在 handler 成功之后。生产级还要讨论 AOF 成功但复制失败、复制成功但 AOF 失败时的策略。

**如何避免重复复制？**

用 `sync_context` 和 loading 标志区分普通客户端命令、AOF load、复制 replay。slave replay 和 AOF load 期间不能再次 broadcast。

**如何做部分同步？**

需要给每条复制消息分配单调递增 offset，master 维护 backlog 环形缓冲区，slave ACK 自己处理到的 offset，重连时请求从 offset 之后继续发。

**这个方案和 Redis 复制有什么差距？**

当前 KVLite 是教学和项目复盘级实现，有全量 + 在线增量的雏形，但没有成熟的 PSYNC、复制积压缓冲区、复制 offset、ACK、心跳、故障转移、哨兵/集群等能力。它适合面试里讲清楚复制的核心链路和工程取舍，但不能包装成完整 Redis 复制。

## 10.18 如果继续优化，可以怎么做

1. 引入 replication offset。每条同步消息带全局递增位点，让 slave 能知道自己同步到哪里。

2. 引入 backlog 环形缓冲区。master 保存最近一段增量消息，slave 短暂断线后可以从 offset 继续追。

3. 支持 slave ACK。slave 定期上报已应用 offset，master 才能判断复制延迟和 backlog 可清理范围。

4. 支持断线后部分重同步。能补增量就补增量，offset 太旧或 backlog 不够时再退化为全量。

5. 支持心跳和超时检测。master 需要知道 slave 是空闲、慢、断开，还是连接假死。

6. 支持 WAIT 类似语义。客户端可以要求写命令被至少 N 个 slave 确认后再返回。

7. 支持读写分离。slave 有副本后，可以承担读请求，但要暴露复制延迟，让调用方知道读到的可能不是最新值。

8. 支持主从切换。master 故障时提升某个 slave，需要处理角色切换、旧 master 回来后的脑裂问题。

9. 支持复制延迟指标。记录队列长度、最后发送时间、slave ACK offset、落后字节数。

10. 支持同步队列限流和丢弃策略。队列满时不能只打印错误，要明确是阻塞、断开慢 slave，还是触发重新全量。

11. 支持 eBPF 捕获丢包统计。ringbuf reserve 失败、事件丢失、RESP 解析失败都应该计数并暴露。

12. 支持同步协议版本号和兼容性。当前协议已有 version 字段，后续可以用于灰度升级和多版本兼容。

13. 支持向量 VSET/VDEL 的复制一致性测试。当前 `write_command` 已标记 V 族写命令，但 TCP master-slave op 还没有完整覆盖向量命令，后续需要补协议和测试。

14. 统一 slave apply 路径。尽量让 TCP 复制也复用 RESP/command_table 或抽象出统一 command replay 层，减少直接调用底层存储接口带来的语义分叉。

15. 强化全量和增量的切换边界。全量同步期间新增写命令要么进入 backlog，要么在锁和状态机上保证不会漏。

主从复制做到这里，我最大的收获不是“会发几条命令给 slave”，而是理解了复制系统真正难的地方：它必须把命令执行结果、协议边界、时间语义、加载上下文、网络故障和异步发送全部串起来。

面试时讲这个模块，不要只说“我实现了主从复制”。更好的表达是：

```text
我把复制挂在写命令成功后的统一副作用点上，
用 command_table 的 write_command 控制复制范围，
用全量同步解决初始状态，
用增量广播解决后续变化，
用 PEXPIREAT 处理 TTL 时间一致性，
并尝试用 eBPF 做旁路同步，理解了低侵入和严格语义之间的取舍。
```

这比单纯罗列函数名更像一个完整系统的复盘。
