# 第三章 为什么网络层不能直接处理命令：TCP 字节流、连接状态与用户态缓冲区

## 3.1 从最早的 read → parse → execute → write 说起

第二章里，我花了很大篇幅还原了 KVLite 最早的一条 SET 请求是怎么从头走到尾的。那个版本有一条非常清晰的链路：

```text
client send → server recv → strtok 切分 → for 循环匹配命令 → switch-case 分发
→ kvs_array_set → sprintf 拼响应 → server send → client recv
```

这条链路在 demo 场景下跑得很顺畅。一个客户端连上来，发一条 `SET Teacher King`，服务端读到 16 个字节，切出三个 token，找到 `KVS_CMD_SET`，写入 array，返回 `OK\r\n`。所有环节严丝合缝，testcase 全部 PASS。

但这条链路之所以能跑，靠的是一个非常脆弱的假设：**一次 `recv` 恰好读到一条完整的命令，一次 `send` 恰好写完整个响应**。

早期的 `kvs_protocol` 函数是这么写的：

```c
int kvs_protocol(char *msg, int length, char *response) {
    printf("recv %d : %s\n", length, msg);
    char *tokens[KVS_MAX_TOKENS] = {0};
    int count = kvs_split_token(msg, tokens);
    return kvs_filter_protocol(tokens, count, response);
}
```

它直接把 `recv` 读到的 `msg` 交给 `strtok` 去切。没有检查消息是否完整，没有考虑"如果数据不够怎么办"，也没有考虑"如果一次读到了多条命令怎么办"。

最早我其实没有马上意识到这个问题。因为在 testcase 里，客户端是一条一条发、一条一条收的——发完 `SET` 之后 `recv` 等响应，收到 `OK\r\n` 再发下一条。这种一问一答的模式下，每次 `recv` 确实大概率能读到一条完整命令。但这不是 TCP 的保证，只是"当前网络环境恰好如此"的巧合。

后面往客户端里加了 10 万次连续写入的压测用例，问题立刻暴露了。那个压测客户端不再等待响应，而是连续 `send` 几千条命令。服务端开始出现解析失败——`strtok` 切出来的 token 有时候是乱码，有时候 `key` 里混着上一条命令的 `value`，有时候一条命令的尾部被当成了下一条命令的头部。

这个时候我才真正理解一件事：**"先读再解析"这个模型在 demo 场景下是合理的，但它的隐含假设太强了，一旦脱离一问一答的交互模式，就完全站不住脚。**

## 3.2 TCP 是字节流，不会替应用层保存命令边界

这个问题不是 KVLite 的 bug，也不是 TCP 的 bug。它是 TCP 协议的本质特征——TCP 是**字节流（byte stream）**协议，不是**消息（message）**协议。

用一个最简单的例子就能说明。假设客户端连续向服务端发送两条命令：

```text
SET name zhengpan
GET name
```

这两条命令一共 28 个字节。TCP 会把这些字节可靠、有序地送到服务端，但它完全不知道——也不关心——哪里是第一条命令的结束，哪里是第二条命令的开始。对 TCP 来说，这就是 28 个连续的字节，和一段 28 字节的文件传输没有任何区别。

服务端的 `recv` 拿到什么，取决于内核 TCP 接收缓冲区的状态和网络包的到达时机，而不是应用层"发了多少条命令"。可能出现三种情况：

**情况一：半包**——服务端第一次 `recv` 只读到了部分数据：

```text
SET name z
```

这时候如果网络层直接把这段数据当一条完整命令去解析，会发现 token 不够——`SET` 命令需要 key 和 value 两个参数，但这里只有一个半。解析失败。

**情况二：粘包**——服务端一次 `recv` 读到了两条命令的全部或部分数据：

```text
SET name zhengpan
GET na
```

这时候如果用 `strtok` 按空格切，会切出一堆 token，但根本不知道哪几个属于第一条命令、哪几个属于第二条。

**情况三：一次读到多条完整命令**——当客户端连续发送几十条命令时，服务端一次 `recv` 可能把所有命令都读进来了：

```text
SET name zhengpan
GET name
DEL name
SET age 25
...
```

这种情况下，网络层必须有能力从这一大块数据里，逐条识别出完整命令、逐条解析、逐条执行。这就是 Pipeline 处理的雏形。

我想强调的一点是：**半包和粘包不是 TCP 的缺陷，而是字节流协议的应有之义。**TCP 的职责是保证字节按顺序、不丢不重地到达，至于这些字节代表什么含义、一条"消息"到哪里结束——这是应用层协议要解决的问题。HTTP 用 `Content-Length` 和 `\r\n\r\n` 解决，Redis 用 RESP 的 `*N\r\n$L\r\n...\r\n` 解决。KVLite 最早用空格分隔的文本格式，没有消息边界标记，所以当数据量上去之后，解析层就崩溃了。

## 3.3 网络层真正应该负责什么

这个问题是我在修复粘包 bug 的过程中逐渐想清楚的。最初我把所有逻辑都堆在 `kvs_protocol` 和 `kvs_filter_protocol` 里——从 socket recv 到命令分发到数据写入，全在一个调用链上。但这样做的问题是：网络层和业务层纠缠在一起，改一个地方就要同时理解 TCP 语义和 KV 存储语义。

正确的分层应该是：

**网络层应该负责的：**
- 监听端口、accept 新连接
- 设置非阻塞 IO（`fcntl(fd, F_SETFL, O_NONBLOCK)`）
- 将连接注册到 epoll / io_uring / 协程调度器
- 从 socket recv 数据，追加到该连接的接收缓冲区
- 从连接的发送缓冲区取数据，send 到 socket
- 管理连接生命周期：注册、错误处理、关闭、资源释放
- 处理 EAGAIN / EWOULDBLOCK（缓冲区满/空时的非阻塞语义）

**网络层不应该负责的：**
- 判断一条 RESP 命令是否完整（那是协议层的事）
- 理解 SET/GET/DEL 的语义（那是命令层的事）
- 判断参数个数是否正确
- 决定写入哪个数据结构
- 处理 AOF 追加、TTL 检查、主从同步（那是业务层的事）

在 KVLite 当前的代码中，这条边界是通过 `net_session_t` 来划定的。网络层（reactor / ntyco / proactor）拿到连接之后，只操作 `net_session_t` 里的 `recv_buf` 和 `send_buf`：

```text
网络层 (reactor.c)
  │
  ├── recv_cb: recv 数据 → 追加到 ext->session.recv_buf
  │            调用 net_session_process_resp() 把控制权交给会话层
  │
  └── send_cb: 从 ext->session.send_buf 取数据 → send 到 socket
               处理 EAGAIN，维护 send_offset

会话层 (net_session.c)
  │
  ├── recv_buf 中循环: resp_peek_command() 探测完整命令
  │   → resp_parser() 有状态解析
  │   → handler (reactor_session_handler) 调用上层
  │   → 响应写入 send_buf 尾部
  │
  └── netbuf_consume_front() 从 recv_buf 头部裁掉已消费数据

命令层 (kvs_operation.c)
  │
  └── 根据 tokens 查 command_table → 执行具体命令 → 生成 RESP 响应字符串
```

这个分层的好处是：如果有一天想把 epoll 换成 io_uring，网络层（reactor.c）可以整个换掉，但会话层（net_session.c）和命令层（kvs_operation.c）完全不用动。实际上，KVLite 已经有三个网络模型在跑——reactor、proactor、NtyCo——它们共享同一套 `net_session_t` + `resp_parser_t` + `command_table` 的上层代码，这正是分层带来的复用效果。

## 3.4 为什么每个连接都需要自己的用户态 recv buffer

分层的前提是有一个"缓冲区"来连接网络层和协议层。这个缓冲区就是 `recv_buf`。

如果没有 `recv_buf`，网络层每读到一些数据就要立刻交给协议层去解析。但如果读到的是半包，协议层解析到一半就卡住了——剩余的数据没地方放，只能丢掉。下次 `recv` 到来时，协议层又从零开始解析，永远拼不出一条完整命令。

所以 `recv_buf` 的核心作用是：**保存那些"已经从 socket 读出来，但还没被协议层完整消费"的数据**。

每个连接的数据进度不同，所以 buffer 必须是连接级别的。在 KVLite 中，每个连接的抽象结构大致如下：

```text
Connection (fd=N)
├── fd                  = N
├── state               = EPOLLIN / EPOLLOUT / closing
├── recv_buf (netbuf_t)
│   ├── data            → 从 socket 读入但尚未消费的字节
│   ├── size            → 当前有效数据长度
│   └── capacity        → 已分配的缓冲区总容量
├── send_buf (netbuf_t)
│   ├── data            → 已生成但尚未发送完的响应字节
│   ├── size            → 待发送数据长度
│   └── capacity        → 已分配的缓冲区总容量
├── resp_parser         → RESP 有状态解析器（state/tokens/token_lens）
├── send_offset         → 发送游标（send_buf 中已发送到的位置）
└── callback / user_data
```

在当前代码中，reactor 模式下这个结构体叫 `conn_extension_t`（定义在 `src/network/reactor.c:47-55`），它内嵌了一个 `net_session_t`。`net_session_t`（定义在 `include/net_session.h:11-18`）则包含了 `recv_buf`、`send_buf` 和 `resp_parser_t`。

`netbuf_t` 本身是一个动态缓冲区（定义在 `include/netbuf.h:9-13`）：

```c
typedef struct {
    char* data;      // 缓冲区起始地址
    size_t size;     // 已用字节数
    size_t capacity; // 当前总容量
} netbuf_t;
```

它的三个关键操作：

- **`netbuf_reserve`**：按 2 倍扩展容量，上限 64MB（`NETBUF_MAX_CAPACITY`）。从 64KB 起步，每次不够就翻倍。
- **`netbuf_consume_front`**：从头部裁掉已消费的 n 字节，内部用 `memmove` 把剩余数据前移。
- **`netbuf_free`**：释放缓冲区内存，在连接关闭时调用。

值得注意的是，这个设计不是我一开始就想好的。早期版本里，`struct conn`（定义在 `include/server.h:20-60`）的 buffer 是一个固定大小的静态数组：

```c
char rbuffer[BUFFER_LENGTH];  // BUFFER_LENGTH = 1024
int rlength;
char wbuffer[BUFFER_LENGTH];
int wlength;
```

这在 demo 阶段够用——1024 字节足够装一条 SET 命令。但一旦遇到大 value（比如一次 SET 写入 10KB 的 value）或者批量 Pipeline（一次发 100 条命令），1024 字节的固定缓冲区要么装不下，要么需要多次 recv 才能凑齐一条命令，而且没有扩展机制。

从固定 buffer 到动态 `netbuf_t` 的演进，是在 commit `d01fc36`（2025-12-03）前后完成的——那次提交明确提到了"修改数据结构支持动态长度"。同一时期，`server.h` 中的 `rbuffer`/`wbuffer` 从固定数组变成了动态指针加 size/capacity 字段（通过 `#if 0` / `#else` 条件编译切换）。

现在回头看，**用户态 recv buffer 是网络层和协议层之间的分界线**。网络层只管往 recv_buf 里塞数据，协议层只管从 recv_buf 里取数据。两个层次不需要知道对方的存在细节——网络层不知道 RESP 是什么，协议层不知道 epoll 是什么。这个解耦是后续所有网络优化（Pipeline、RESP、大 value 支持、慢客户端处理）的基础。

## 3.5 recv buffer 如何支持半包、粘包和 Pipeline

有了 recv buffer 之后，三种问题场景都有了明确的处理方式。下面结合当前代码的具体流程来说明。

### 场景 1：半包——数据还没凑齐

客户端发了一条 RESP 格式的命令：`*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nzhengpan\r\n`，但服务端第一次 `recv` 只读到了前 20 个字节：`*3\r\n$3\r\nSET\r\n$4\r\nna`。

在 `recv_cb` 中（`src/network/reactor.c:306-387`），这 20 个字节被追加到 `recv_buf`。然后 `recv_cb` 调用 `net_session_process_resp`。

`net_session_process_resp`（`src/network/net_session.c:60-198`）先做首次对齐检查（跳过前导空白，确认首个非空白字符是 `*`），然后进入主循环。循环内调用 `resp_peek_command` 探测 `recv_buf` 中从 `parsed_total` 偏移开始是否有完整命令。

`resp_peek_command` 是一个无状态探测函数（`src/network/resp_protocol.c:89-180`）。它会从头到尾解析 `*N\r\n`、每个 `$L\r\n<data>\r\n` 的结构——但它在第 164-165 行遇到关键的一步：

```c
size_t need = (size_t)bulk_len + 2;
if (remain < need) return 0; // body 或 CRLF 不完整
```

当它发现当前 buffer 中的数据不够一条完整命令时，返回 0。`net_session_process_resp` 收到 0 之后，跳出循环（第 127-129 行），不消费 `recv_buf` 中的任何数据。数据原封不动地留在 `recv_buf` 中，等待下次 `recv` 追加新数据后再试。

下次 `recv` 读到剩余数据后，`recv_buf` 变完整了，`resp_peek_command` 就能返回完整命令的长度，然后 `resp_parser` 有状态解析，handler 执行，响应写入 `send_buf`，最后 `netbuf_consume_front` 把已消费的数据从 `recv_buf` 头部裁掉。

### 场景 2：粘包——一次读到一条半命令

客户端发了两条命令，服务端一次 `recv` 读到了第一条的完整内容加上第二条的前半部分。

`resp_peek_command` 会返回第一条命令的完整长度（比如 45 字节）。`net_session_process_resp` 把这 45 字节交给 `resp_parser` 解析，handler 执行，第一条命令处理完成。`parsed_total += 45`。

循环继续。`resp_peek_command` 从 offset=45 处探测第二条命令，发现数据不完整，返回 0。跳出循环。`netbuf_consume_front(&recv_buf, 45)` 把前 45 字节裁掉，`recv_buf` 里只剩下第二条命令的前半部分。

下次 recv 追加上后半部分，第二条命令就完整了。

### 场景 3：Pipeline——一次读到多条完整命令

客户端连续发来了 50 条 RESP 命令，服务端一次 `recv` 全读进来了。

`net_session_process_resp` 的主循环会一条一条地处理（`src/network/net_session.c:115-188`）：

```c
while(parsed_total < session->recv_buf.size
      && commands_count < NET_SESSION_MAX_COMMANDS_PER_BATCH){
    
    int cmd_len = resp_peek_command(...);
    if(cmd_len == 0) break;        // 不完整，等数据
    if(cmd_len < 0) return -1;     // 协议错误
    
    int parsed = resp_parser(...); // 有状态解析
    int resp_len = handler(...);   // 执行命令，生成响应
    session->send_buf.size += resp_len;  // 响应追加到 send_buf
    
    parsed_total += cmd_len;
    commands_count++;
    resp_parser_reset(parser);     // 重置解析状态，准备下一条
}
```

这里有一个重要的设计细节：**循环的退出条件不只是"数据不够"，还包括"处理命令数达到上限"**。`NET_SESSION_MAX_COMMANDS_PER_BATCH` 默认 10,000,000（基本等于不加限制），但可以根据需要调低。这个机制存在的意义是防止单个连接通过超长 Pipeline 长时间占用 CPU 导致其他连接饥饿。

循环退出后，`netbuf_consume_front(&recv_buf, parsed_total)` 一次性清掉所有已处理的命令数据。如果 Pipeline 非常大（比如 10000 条），这次 `memmove` 的开销也不可忽略——但比起每条命令处理完就裁一次，批量裁剪显然更高效。

### 一点补充：memmove 的性能代价

`netbuf_consume_front` 内部使用 `memmove` 把剩余数据从尾部移到头部。当 Pipeline 吞吐量极高时，频繁的 `memmove` 会带来额外开销。一种常见的优化是使用**环形缓冲区（ring buffer）**，用读写指针替代数据搬移。KVLite 当前选择的是更简单的线性 buffer + memmove 方案，这是因为：

1. RESP 解析器需要在一块连续内存上工作（`resp_peek_command` 遍历 `$L\r\n` 结构），环形缓冲区需要处理跨越 buffer 末尾的"回绕"问题，代码复杂度会大幅增加。
2. 在典型的 Pipeline 场景下，大部分命令数据在处理后都被裁掉了，`memmove` 的次数其实不多——每轮 epoll 事件最多做一次。
3. 对于高吞吐场景，后续可以考虑引入零拷贝方案（如 `iovec` + `writev`），但那是 send 侧的优化了。

这是一个典型的"工程权衡"——不是不能优化，而是在当前阶段，简单方案的维护成本低于它带来的性能损耗。

## 3.6 为什么还需要 send buffer

复盘文章里很容易只讨论 recv buffer，因为半包和粘包是更"明显"的问题。但 send buffer 同样重要，它是非阻塞网络模型中响应发送的基础。

问题的根源是：**`send()` 不保证一次写完你给它的全部数据**。

在非阻塞 socket 下，`send(fd, buf, len, 0)` 的行为是这样的：

- 如果内核的 TCP 发送缓冲区有足够空间，`send` 会拷贝全部 `len` 字节到内核缓冲区，返回 `len`。
- 如果内核发送缓冲区空间不够，`send` 只拷贝它能装下的那部分数据，返回实际拷贝的字节数（可能小于 `len`）。
- 如果内核发送缓冲区完全满了，`send` 返回 -1，`errno` 设为 `EAGAIN` 或 `EWOULDBLOCK`。

第一种情况是理想情况，第二种和第三种才是现实中的常客——尤其是当客户端网络较慢（"慢客户端"）或发送数据量较大时。

如果没有 send buffer，网络层就只有两个选择：要么在事件循环里阻塞等待 `send` 写完（阻塞住整个事件循环，所有连接都被卡住），要么丢掉没发完的数据（响应就丢了）。这两个选择都不可接受。

send buffer 的设计就是解决这个问题的：**把"还没发完的响应数据"暂存在 send buffer 里，等 fd 变为可写时继续发**。

在 KVLite 的 reactor 模式中，整个发送流程是这样的：

1. `net_session_process_resp` 处理完一条命令后，把生成的 RESP 响应字符串追加到 `send_buf` 尾部（`session->send_buf.size += resp_len`）。
2. `recv_cb` 结尾检查 `send_buf->size > 0`，如果有待发送数据，就把 fd 的事件从 `EPOLLIN` 切换为 `EPOLLIN | EPOLLOUT`——同时监听可读和可写。
3. 下次 epoll 触发 `EPOLLOUT` 时，`send_cb` 被调用。`send_cb`（`src/network/reactor.c:394-450`）进入一个 while 循环，不断调用 `send` 直到全部数据发完或遇到 `EAGAIN`：

```c
while (ext->send_offset < send_buf->size) {
    size_t remaining = send_buf->size - ext->send_offset;
    ssize_t sent = send(fd,
                        send_buf->data + ext->send_offset,
                        remaining,
                        MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            set_event(fd, EPOLLOUT | EPOLLIN, 0);  // 等下次可写
            return 0;
        }
        event_unregister(fd);  // 真正的错误，关闭连接
        return -1;
    }
    ext->send_offset += sent;
}
```

4. 全部发完后，`send_offset` 重置为 0，`send_buf->size` 清零，fd 的事件切回 `EPOLLIN`（只监听可读）。

这个流程解决了"非阻塞写"的核心问题。但说实话，当前实现还有一些可以完善的地方。

**当前方案的不足：**

- **没有高水位保护**：`send_buf` 可以一直涨到 64MB（`NETBUF_MAX_CAPACITY`）。如果一个慢客户端导致 send buffer 持续积压，这个连接会吃掉大量内存。Redis 的做法是在 send buffer 超过一定阈值时主动关闭连接——KVLite 目前没有这个机制。

- **没有背压（backpressure）**：当 send buffer 积压严重时，理想的做法是对该连接暂停接收新命令（解除 EPOLLIN 注册，只保留 EPOLLOUT），等 send buffer 降到安全水位再恢复。KVLite 当前是 EPOLLIN 和 EPOLLOUT 同时注册的，慢客户端仍然可以继续发送命令进来，send buffer 只会越积越多。

- **send_buf 的无限增长可能**：虽然 64MB 是硬上限，但对于一个有 1024 个连接的服务来说，如果每个连接都逼近上限，内存占用会非常恐怖。当前没有连接级别的 send buffer 水位告警。

这些问题我都记录在项目的改进方向里了——不是没意识到，而是在 KVLite 当前的使用场景（本地 benchmark、单机测试、AI Chat 会话缓存原型）下，它们的严重程度还不足以成为最高优先级。如果有一天需要把 KVLite 部署成面向公网的服务，send buffer 的背压和高水位保护会是第一个要加的特性。

## 3.7 当前设计的完整调用链

下面以 reactor 模式为例，给出一条数据从网络到达后、经过缓冲区、协议解析、到命令层的完整调用链：

```text
epoll_wait (reactor.c:534)
  │ 返回就绪的 fd 列表
  ▼
recv_cb(fd) (reactor.c:306)
  │ while 循环 recv，把字节追加到 ext->session.recv_buf
  │ 处理 EAGAIN（非阻塞读空）和 recv==0（对端关闭）
  │ recv_buf->size 增加
  ▼
net_session_process_resp(fd, &ext->session, reactor_session_handler, ext)
  │ (net_session.c:60)
  │ 1. 检查 processing 标志，避免重入
  │ 2. 首次对齐检查：跳过前导空白，确认首个非空白为 '*'
  │ 3. 主循环：
  │    a. resp_peek_command(buf, size, offset)
  │       → 无状态探测：从 offset 起是否有一条完整 RESP 命令
  │       → 返回 cmd_len (>0: 完整长度, 0: 数据不够, <0: 协议错误)
  │    b. 如果 cmd_len==0 → 跳出循环，等待更多数据
  │    c. 如果 cmd_len>0 → resp_parser(parser, buf+offset, cmd_len)
  │       → 有状态解析：*N → $L → body → CRLF，逐字段填充 tokens/token_lens
  │       → 解析完成后 parser->state = RESP_PARSE_COMPLETE
  │    d. reactor_session_handler(parser, response_buf, cap, user_data)
  │       (reactor.c:206)
  │       → 根据 parser->tokens 判断命令名
  │       → 处理 REPLSYNC（复制连接标记）
  │       → 调用 kvs_handler(parser->tokens, parser->arg_index,
  │                           parser->token_lens, response_buf)
  │           → kvs_operation.c 中的 command_table 查找和执行
  │           → 生成 RESP 响应字符串，返回长度
  │       → 返回 resp_len（响应字节数）
  │    e. session->send_buf.size += resp_len
  │       → 响应追加到 send_buf 尾部
  │    f. parsed_total += cmd_len, commands_count++
  │    g. resp_parser_reset(parser)
  │ 4. netbuf_consume_front(&recv_buf, parsed_total)
  │    → memmove 把剩余数据移到 recv_buf 头部
  │ 5. processing = 0
  ▼
回到 recv_cb
  │ 检查 send_buf->size > 0 ?
  │   是 → set_event(fd, EPOLLIN | EPOLLOUT)  // 需要发送响应
  │   否 → set_event(fd, EPOLLIN)              // 只等接收
  ▼
epoll_wait 下一次循环...
  │ 如果注册了 EPOLLOUT 且 fd 可写:
  ▼
send_cb(fd) (reactor.c:394)
  │ while 循环 send:
  │   从 send_buf->data + send_offset 处发送
  │   遇到 EAGAIN → 保留 EPOLLOUT，等下次可写
  │   全部发完 → send_offset=0, send_buf.size=0, 切回 EPOLLIN
```

每个节点的职责：

- **recv_cb**：只负责从 socket 读字节到 recv_buf，不关心字节内容。
- **net_session_process_resp**：只负责"recv_buf 中有没有完整命令、有几条、解析后交给 handler"，不关心网络 IO。
- **reactor_session_handler**：只负责"把 parsed tokens 交给 kvs_handler，把返回的响应字符串交给 send_buf"，不关心 RESP 解析细节。
- **kvs_handler / kvs_operation**：只负责"根据 command_table 找到处理函数、执行业务逻辑、生成响应"，不知道网络和协议的存在。
- **send_cb**：只负责"从 send_buf 取数据、send 到 socket、处理 EAGAIN"，不关心响应内容是什么。

## 3.8 这个设计解决了什么，又留下了什么问题

### 解决了什么

1. **网络层和命令层彻底解耦**。reactor/proactor/NtyCo 三个网络模型共享同一套 net_session + resp_parser + command_table，换网络层不影响业务逻辑。

2. **半包数据可以安全保留**。`resp_peek_command` 发现命令不完整时返回 0，recv_buf 中的数据原封不动，等下次 recv 追加后再试。

3. **粘包和 Pipeline 可以自然处理**。主循环逐条探测、解析、执行、裁剪，只要 recv_buf 中还有完整命令就继续处理。

4. **慢写响应可以暂存并分批发送**。send_buf + send_offset + EPOLLOUT 注册构成了完整的非阻塞发送机制，不会拖慢事件循环。

5. **内存可以动态增长**。netbuf 从 64KB 起步，按 2 倍扩展，上限通过 `NETBUF_MAX_CAPACITY` 控制，不会像固定数组那样在大 value / 大 Pipeline 场景下 OOM 或拒绝服务。

### 留下了什么问题

1. **buffer 容量上限的粒度太粗**。`NETBUF_MAX_CAPACITY`（64MB）是全局常量，不能按连接细粒度限制。一个恶意或异常客户端可以用大请求把单个 recv_buf 撑到 64MB。

2. **慢客户端没有背压机制**。send buffer 积压时，该连接仍然保持 EPOLLIN 注册，会继续接收新命令。缺少"send buffer 过高时暂停接收"的反馈回路。

3. **单连接 Pipeline 过长可能饿死其他连接**。当前 `NET_SESSION_MAX_COMMANDS_PER_BATCH` 默认值 10,000,000，基本等于不限制。虽然每轮 epoll 处理后都会回到事件循环，但如果 Pipeline 命令都非常轻量（比如 GET 不存在的 key），一个连接在一次 recv_cb 中可能处理数万条命令，其他连接的请求会有明显延迟。

4. **用户态 buffer 引入额外内存拷贝**。socket → recv_buf 是一次拷贝，recv_buf → parser tokens 是又一次拷贝（`kvs_malloc` 后 `memcpy`），response → send_buf 是第三次，send_buf → socket 是第四次。在性能敏感场景下这些拷贝开销可观。后续可以考虑用 `mmap`、`splice` 或零拷贝技术减少拷贝次数——但这会大幅增加代码复杂度，在当前阶段不是优先事项。

5. **非阻塞 send/recv 的错误处理还可以更细致**。当前处理了 `EAGAIN`/`EWOULDBLOCK` 和 recv 返回 0，但 `EINTR`（被信号中断）在 `recv_cb` 的 while 循环中没有显式处理——虽然在实际运行中极少触发，但严谨起见应该加上。

6. **`memmove` 在大 Pipeline 场景下的开销**。当 recv_buf 中积压了大量数据，`netbuf_consume_front` 的 `memmove` 可能会成为热点。环形缓冲区可以消除这个开销，但会增加代码复杂度和解析器适配成本。

如实说，这些问题在当前 KVLite 的典型使用场景（本地压测、AI Chat 会话缓存原型、单机部署）下都不会成为瓶颈。但如果有一天要考虑生产级部署，send buffer 背压和连接级资源限制会是首先要补的两个缺。

## 3.9 面试时我怎么讲这个阶段

### 30 秒版

问："为什么你的网络层不能直接处理命令？为什么要有 recv buffer？"

答："因为 TCP 是字节流协议，不保证消息边界。一次 recv 读到的可能是半条命令，也可能是多条命令拼在一起。如果网络层直接解析命令，遇到这种情况就会出错。所以我把网络层和协议层用 recv buffer 解耦——网络层只负责往 buffer 里塞字节，协议层从 buffer 里逐条探测完整命令再解析。send buffer 也是同样的道理，非阻塞 send 不一定一次写完，需要暂存未发完的响应，等 fd 可写时继续发。"

### 2 分钟版

问："KVLite 是怎么处理 TCP 半包、粘包和 Pipeline 的？"

答："最早我用的是一种很简陋的模型——`recv` 之后直接用 `strtok` 按空格切字符串，切成 token 就执行。这在一问一答的 demo 场景下能跑，但一旦客户端连续发送多条命令，问题立刻暴露：`strtok` 不知道哪里是一条命令的结束，会出现半包解析失败和粘包串数据的情况。

后来我引入了一整套用户态缓冲区机制来解决这个问题。核心是三层结构：

第一层是 `netbuf_t`，一个动态扩展的字节缓冲区，每个连接一个 recv_buf 和一个 send_buf。

第二层是协议探测器 `resp_peek_command`，它是一个无状态函数，在一块连续内存中扫描 `*N\r\n$L\r\n<body>\r\n` 的格式，如果数据不全就返回 0，告诉上层'再读点数据进来'。这样就解决了半包问题——不完整的命令原封不动留在 buffer 里等下次 recv 追加。

第三层是 `net_session_process_resp` 的主循环。它在 buffer 中循环探测完整命令，每找到一条就交给 `resp_parser` 做有状态解析，再通过 handler 回调交给命令层执行，响应追加到 send_buf。循环处理完所有完整命令后，一次性 `memmove` 裁掉已消费的数据。这就同时解决了粘包和 Pipeline——几十条命令拼在一块到达，循环一条条吃进去，剩下的自然留着等下轮。

send 方向也是类似的思路。非阻塞 `send` 可能只发出去一部分响应，我把没发完的数据留在 send_buf 里，用 send_offset 记录进度，注册 EPOLLOUT 事件，等 socket 可写时继续发。遇到 EAGAIN 就退出等下次事件，不阻塞事件循环。

整个设计的核心思想就是分层解耦：网络层只管读写字节，协议层只管从 buffer 中识别完整命令，命令层只管执行。任何一层出问题都不会污染另一层。"

## 3.10 本章小结

这一章解释了为什么第二章中那条 `recv → strtok → switch-case → execute → send` 的简单链路，在真实 TCP 场景下不能继续支撑项目演进。

核心原因是 TCP 是字节流协议，没有消息边界。一次 recv 不等于一条命令，一次 send 也不等于一次完整响应。解决这个问题的关键是**在每个连接上引入用户态 recv buffer 和 send buffer**，让网络层和协议层解耦：

- recv buffer 保存"已读入但未完整消费"的数据，支持半包保留和粘包/Pipeline 逐条解析；
- send buffer 保存"已生成但未完全发送"的响应，配合 EPOLLOUT 和非阻塞 send 实现慢写处理。

这个设计没有使用复杂的零拷贝或内核旁路技术，而是通过清晰的模块边界（网络层→会话层→协议层→命令层）让项目具备了可演进性。后续的 RESP 完整解析、Pipeline 批量优化、大 value 支持，都是建立在这个 buffer 架构之上的。

下一章将继续复盘 RESP 协议解析的细节——从早期 `strtok` 按空格切分，演进到 `resp_peek_command` + `resp_parser` 的两阶段解析，以及为什么 RESP 的长度前缀设计天然解决了 TCP 消息边界问题。

## 3.11 本章对应的面试追问清单

1. TCP 为什么会有半包和粘包？是 TCP 的 bug 吗？
2. 半包和粘包的本质是什么？是在哪一层被解决的？
3. 为什么一次 `recv` 不能假设等于一条命令？
4. `recv` 的返回值代表什么？它和消息长度有什么关系？
5. 为什么网络层不能直接解析 SET/GET？如果解析了会怎么样？
6. recv buffer 保存的是什么数据？它是连接级别还是全局的？
7. `resp_peek_command` 返回 0 是什么意思？返回正数呢？返回负数呢？
8. Pipeline 怎么靠 recv buffer 支持？主循环的退出条件有哪些？
9. `send` 为什么也可能写不完？非阻塞 `send` 返回 -1 且 `errno=EAGAIN` 怎么处理？
10. `EPOLLIN` 和 `EPOLLOUT` 分别在什么时候注册？什么时候切换？
11. 慢客户端会导致什么问题？send buffer 会一直涨吗？怎么保护？
12. `netbuf_consume_front` 用 `memmove` 而不是环形缓冲区，性能开销如何？为什么不换成 ring buffer？
13. 协议错误时 buffer 和连接怎么处理？`net_session_process_resp` 返回 -1 后发生了什么？
14. 这个设计和 Redis 的网络层有什么类似之处？有什么不同？
15. 如果让你重写 KVLite 的网络层，你会怎么改进？（提示：背压、高水位、iovec 零拷贝、ring buffer）

---

*第三章完。下一章主题：RESP 协议解析——从 `strtok` 演进到 `resp_peek_command` + `resp_parser` 两阶段解析。*
