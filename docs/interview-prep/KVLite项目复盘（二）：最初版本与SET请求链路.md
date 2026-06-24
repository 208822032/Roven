# 第二章 最初版本是什么样，一条 SET 请求是怎么跑通的

## 2.1 先跑通最短链路，而不是一开始就做完整系统

2024 年 5 月 18 日，KVLite 仓库里的第一个 commit 只有一份 GitLab 自动生成的 README 模板——没有代码，没有 Makefile，没有网络层，没有任何数据结构。这个 repo 的起点和大多数新项目一样：一个空目录，一份模板 README。

但仅仅七天之后，也就是 2024 年 5 月 25 日，这个 repo 里已经集齐了三种网络模型、四种内存数据结构、一个测试客户端、一个 Makefile 构建系统，以及五种语言的客户端示例。

这个速度说明一件事：早期阶段的目标非常明确——不是做一个功能完整的 KV 系统，而是用最快速度跑通一条最短请求链路。

什么是最短请求链路？就是从客户端发起一个 TCP 连接、发送一条 `SET key value` 命令，到服务端收到数据、解析出命令和参数、写入内存数据结构、生成响应、把结果发回客户端。这七个环节连在一起，就是 KVLite 的第一条闭环：

```text
client → socket connect → send "SET key value"
  → server accept → recv/read buffer
  → parse command → dispatch to handler
  → memory store (array/rbtree/hash/skiptable)
  → build response → send/write response → client recv
```

这个闭环虽然简单，但它的意义不在功能完整，而在于它证明了"这个项目能跑"。有了这条闭环之后，后续所有模块——RESP 协议解析、Pipeline 批量处理、AOF 持久化、RDB 快照、TTL 过期机制、主从复制、eBPF 内核旁路同步、向量检索——都是在这条链路上逐渐挂载进去的。换句话说，不是一开始就设计了一个"大系统"，而是先做了一条"最小的流水线"，然后每次迭代往上面加一道工序。

这也是我从这个项目中反复体会到的一点：做基础设施类的系统，不要一上来就画架构图、定接口、拆模块。先把一条请求跑通，哪怕代码很简陋，哪怕数据结构是 O(n) 的线性扫描，哪怕协议解析用的是 `strtok`。跑通了，就有了地基，后面的迭代才有锚点。

## 2.2 早期提交里，KVLite 的骨架是怎么长出来的

翻看 `git_iteration_data` 里的提交记录，KVLite 的早期成长轨迹非常清晰。这里把前五个关键 commit 按时间顺序展开，不是为了罗列 log，而是为了还原一个"项目骨架是怎么从零长出来的"过程。

### 第一块骨头：网络层（2024-05-18，commit `5a7fa95`）

第一个有代码的 commit 叫 `add network`，距离 Initial commit 只隔了一分钟。这很说明问题：项目启动之后，第一步不是写数据结构，而是先把网络收发跑通。

这个 commit 一次性引入了三个网络模型：reactor（基于 epoll 的事件驱动）、proactor（基于 io_uring 的异步 IO）、NtyCo（基于协程的同步写法异步执行）。三种模型通过 `NETWORK_SELECT` 宏在编译期选择，对外暴露统一的 `msg_handler` 回调接口。

这是一种很有意思的做法——不是先选一个"最好的"网络模型，而是把三种都实现一遍，然后按需切换。后面当 Pipeline 优化和粘包处理轮番上阵时，这个"三选一"的架构让对比测试变得非常自然：改了 reactor，马上可以切到 proactor 对比一下行为；优化了 NtyCo，切到 epoll 看看差距。

这时候的 `kvstore.c` 极其简单：

```c
int kvs_protocol(char *msg, int length, char *response) {
    printf("recv %d : %s\n", length, msg);
    memcpy(response, msg, length);
    return strlen(response);
}
```

它做的事情就是：收到什么就原样返回什么。没有任何命令解析，没有任何数据存储。但这个 echo 服务证明了一件事：网络收发链路已经通了——客户端能连上来，服务端能收到数据，能把响应发回去。

同时，`kvstore.h` 里已经预先定义了一个 `command[]` 数组：

```c
const char *command[] = {
    "SET", "GET", "DEL", "MOD", "EXIST"
};
```

这说明虽然网络层还在做 echo，但作者心里已经清楚接下来要支持哪些命令了。这张"命令清单"放在头文件里，等于给下一步迭代留了一个明确的 TODO。

同一个 commit 还引入了 `server.h`，定义了一个 `struct conn`，把每个连接的 fd、读缓冲区、写缓冲区、回调函数封装在一起。这个结构体后来成了整个网络层的核心抽象——所有连接的状态都挂在 `conn_list` 这个固定大小的数组里，fd 直接当下标用。

### 第二块骨头：第一个数据结构 array（2024-05-21，commit `1b86583`）

有了网络层之后，下一步就是"收到命令之后往哪里存"。第一个被加进来的数据结构是 array——一个固定 1024 容量的 `kvs_array_item_t` 数组，每个 item 存一个 `char *key` 和 `char *value`。

选 array 作为第一个数据结构是非常务实的选择：它不需要树旋转、不需要哈希函数、不需要跳表层级概率——就是一个 `malloc` 一块内存、遍历找 key、`strcmp` 比较的朴素实现。这保证了数据结构本身不会成为阻塞点——如果出了问题，一定出在网络层或者命令解析层，不会怀疑是红黑树的旋转写错了。

这个 commit 把 `kvstore.c` 从 43 行拉到了约 200 行，加了三样东西：

**令牌切分（token split）**：用 `strtok` 按空格把 `"SET key value"` 拆成三个 token——`SET`、`key`、`value`。这是最朴素的解析方式，没有 RESP 协议，没有长度前缀，就是字符串按空格切。

**命令分发（command dispatch）**：一个 for 循环遍历 `command[]` 数组找到命令名匹配的索引，然后进 switch-case，分别调用 `kvs_array_set`、`kvs_array_get`、`kvs_array_del`、`kvs_array_mod`、`kvs_array_exist`。

**存储引擎初始化（init_kvengine）**：在 `main()` 里启动网络之前，先调用 `init_kvengine()` 初始化全局的 array 实例。

这时候的调用链已经基本成形：

```text
main() → init_kvengine() → reactor_start(port, kvs_protocol)
  → client连接 → epoll_wait → accept_cb → recv_cb
  → kvs_request() → kvs_protocol()
  → kvs_split_token() → kvs_filter_protocol()
  → kvs_array_set/get/del/mod/exist()
  → 构建响应字符串 → send_cb → 返回客户端
```

### 第三块骨头：红黑树、测试用例和 Makefile（2024-05-23，commit `ea5291f`）

array 跑通之后，第二个数据结构红黑树只隔了一天就加进来了。红黑树解决了 array 的 O(n) 查找问题——当 key 数量增多时，二叉搜索树的 O(log n) 优势开始显现。

但这个 commit 真正重要的不是红黑树本身，而是另外两个东西：

**testcase.c**：一个 162 行的测试客户端。它做的事情很直白：用 socket 连上服务端，`send` 一条命令过去，`recv` 回响应，`strcmp` 跟预期结果比较，一样就打印 `PASS`，不一样就打印 `FAILED` 然后退出。这个模式虽然简陋，但它建立了一个关键原则——每次改完代码，跑一下 testcase，看看基本功能有没有被破坏。后面当 Pipeline、AOF、TTL 这些复杂特性加进来的时候，这个测试习惯保护了代码不被反复踩坏。

**Makefile**：只有 15 行，`gcc` 一把梭把 `.c` 文件编译成 `kvstore` 二进制，加上 clean 目标。但它的存在意味着项目从"手工敲 gcc 命令"进入了"有构建系统"的阶段。后面 Makefile 逐渐长到了 279 行，引入了子目录编译、条件编译、jemalloc 链接，但骨架就是这 15 行奠定的。

### 第四块骨头：hash、skiptable 和多语言客户端（2024-05-25，commit `480bccf`）

到第五个 commit，剩下的两个数据结构——哈希表和跳表——一起加了进来。至此，四种内存数据结构全部到位。

但这个 commit 里还有一个容易被忽视的细节：`kvs-client/` 目录下出现了五种语言的客户端示例——Go（42 行）、Java（37 行）、JavaScript（26 行）、Python（30 行）、Rust（18 行）。这些客户端做的事情完全一样：连上服务端，发几条 SET/GET 命令，打印结果。

这些多语言客户端不是给生产环境用的。它们的作用是双重的：一来验证了"协议是语言无关的"——只要按空格分隔的文本格式发数据，任何语言的 socket 库都能对接；二来为后续的 benchmark 对比（KVStore vs Redis）留了测试入口——后面用 Python 写的 RESP benchmark 脚本，思想源头就在这里。

### 这一周的意义

从 5 月 18 日到 5 月 25 日，一共 7 天，5 个核心 commit。这段时间做的事情可以总结为一句话：**把 KV 存储系统的"最小可行骨架"搭出来**。

- 网络层（三种模型）——负责收发包
- 数据结构层（四种实现）——负责存数据
- 命令解析层（strtok + switch-case）——负责路由请求
- testcase + Makefile —— 负责验证和构建
- 多语言客户端 —— 验证协议的通用性

这里没有什么"高并发优化"，没有什么"分布式一致性"，没有什么"持久化策略"。就是最朴素的问题：客户端发一条命令，服务端能不能收、能不能解析、能不能存储、能不能返回。

但这个骨架的价值在于：它为后续所有迭代定义了"在哪里挂东西"。后面要加 RESP 协议，改的是 `kvs_protocol` 和解析层；要加 AOF 持久化，挂载点在 `kvs_filter_protocol` 里每个命令执行完之后；要加 TTL，挂载点在数据结构层的 key 存储结构上；要加主从复制，挂载点在命令执行之后、响应返回之前。

一个清晰的骨架，比一个功能齐全但不可扩展的"完整系统"有价值得多。

## 2.3 一条最早的 SET 请求是怎么跑通的

现在把镜头拉近，看一条 `SET Teacher King` 命令在早期版本中从发出到响应的完整路径。以下还原基于 commit `ea5291f`（2024-05-23，集成了 array + rbtree + testcase + Makefile 的版本）的实际代码。

### Step 1：客户端建立 TCP 连接并发送命令

testcase 客户端做的事情非常简单：

```c
int connfd = socket(AF_INET, SOCK_STREAM, 0);
struct sockaddr_in server_addr;
server_addr.sin_family = AF_INET;
server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
server_addr.sin_port = htons(9300);
connect(connfd, (struct sockaddr*)&server_addr, sizeof(server_addr));

send(connfd, "SET Teacher King", strlen("SET Teacher King"), 0);
```

没有连接池，没有非阻塞 IO，就是最朴素的 TCP socket：创建、connect、send。

### Step 2：服务端 accept 连接

服务端启动时调用 `reactor_start(port, kvs_protocol)`，内部做了三件事：创建 listen socket、绑定端口、注册到 epoll 监听 EPOLLIN 事件。epoll 事件循环在 `epoll_wait` 上阻塞，当客户端的 SYN 到达时，listen fd 变为可读，触发 `accept_cb`：

```c
int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
```

拿到 clientfd 之后，调用 `event_register` 把它注册到 `conn_list` 里——以后这个连接的所有状态都挂在 `conn_list[clientfd]` 上，fd 直接当下标用。同时把 clientfd 也注册到 epoll 中，监听 EPOLLIN。

### Step 3：服务端 recv 数据

客户端发送的 `"SET Teacher King"` 到达服务端的 TCP 接收缓冲区后，clientfd 变为可读，epoll 触发 `recv_cb`。recv_cb 调用 `recv` 把数据读到 `conn_list[clientfd].rbuffer` 中：

```c
int n = recv(fd, conn_list[fd].rbuffer, BUFFER_LENGTH, 0);
conn_list[fd].rlength = n;
```

然后调用 `kvs_request`，把读缓冲区和读长度传给上层。在 reactor 模式下，`kvs_request` 直接调用 `kvs_handler`，也就是 `kvs_protocol` 函数。

### Step 4：解析命令

`kvs_protocol` 首先调用 `kvs_split_token` 做令牌切分：

```c
char *tokens[KVS_MAX_TOKENS] = {0};
int count = kvs_split_token(msg, tokens);
```

`kvs_split_token` 内部用 `strtok(msg, " ")` 把 `"SET Teacher King"` 按空格切分，得到：
- `tokens[0]` = `"SET"`
- `tokens[1]` = `"Teacher"`
- `tokens[2]` = `"King"`
- 返回 count = 3

### Step 5：命令分发

拿到 tokens 后，`kvs_protocol` 调用 `kvs_filter_protocol(tokens, count, response)`。

`kvs_filter_protocol` 先做命令名匹配——用一个 for 循环遍历 `command[]` 数组：

```c
int cmd = KVS_CMD_START;
for (cmd = KVS_CMD_START; cmd < KVS_CMD_COUNT; cmd++) {
    if (strcmp(tokens[0], command[cmd]) == 0) {
        break;
    }
}
```

`"SET"` 匹配到数组下标 0，即 `KVS_CMD_SET`。

然后进入 switch-case，走 `KVS_CMD_SET` 分支：

```c
case KVS_CMD_SET:
    ret = kvs_array_set(&global_array, key, value);
    if (ret < 0) {
        length = sprintf(response, "ERROR\r\n");
    } else if (ret == 0) {
        length = sprintf(response, "OK\r\n");
    } else {
        length = sprintf(response, "EXIST\r\n");
    }
    break;
```

### Step 6：写入 array 数据结构

`kvs_array_set` 做的事情：

1. 先调用 `kvs_array_get` 检查 key 是否已存在——遍历 `inst->table[0..total-1]`，发现 `"Teacher"` 不存在。
2. `malloc` 一块新内存，把 key `"Teacher"` 和 value `"King"` 分别 `strncpy` 进去。
3. 在 table 中找到一个空位（或者追加到末尾），把 key、value 指针存进去。
4. `total++`，返回 0（表示成功，新插入）。

这里可以清楚地看到 array 的几个特点：容量固定 1024、查找是 O(n) 线性扫描、key 和 value 各自独立 `malloc`。这些在后面都会成为优化点，但在早期版本中，它们最重要的是"正确"——先跑对，再跑快。

### Step 7：生成响应并返回

`kvs_array_set` 返回 0 表示新插入成功，`kvs_filter_protocol` 用 `sprintf(response, "OK\r\n")` 生成响应字符串，返回长度 4。

回到 `kvs_request`，响应长度赋值给 `c->wlength`。然后 `recv_cb` 切换到 EPOLLOUT 事件，等待客户端可写。epoll 下次循环触发 `send_cb`，调用 `send(fd, c->wbuffer, c->wlength, 0)` 把 `"OK\r\n"` 发回客户端。

### Step 8：客户端验证

客户端 `recv` 到 `"OK\r\n"`，跟预期的 `"OK\r\n"` 做 `strcmp`，匹配上，打印 `PASS`。

```c
testcase(connfd, "SET Teacher King", "OK\r\n", "SET-Teacher");
```

一条 SET 请求就这样从头走到了尾。

### 简化链路总览

```text
testcase.c                          reactor.c / kvstore.c                 kvs_array.c
─────────                          ─────────────────────                 ───────────
socket()
connect()  ─────TCP SYN──────────→  accept_cb()
send("SET                              ↓
  Teacher King") ──TCP PSH/ACK────→  recv_cb()
                                      ↓
                                    kvs_request()
                                      ↓
                                    kvs_protocol()
                                      ↓
                                    kvs_split_token()
                                    → ["SET","Teacher","King"]
                                      ↓
                                    kvs_filter_protocol()
                                    → cmd = KVS_CMD_SET
                                      ↓
                                    switch → kvs_array_set() ──────────→  malloc key
                                        ↓                                  malloc value
                                    sprintf("OK\r\n")                      table[i]=...
                                      ↓
                                    send_cb()
recv()  ←─────TCP PSH/ACK─────────  send("OK\r\n")
strcmp() → PASS
```

这就是早期版本的核心链路。它不需要理解 RESP 的 `*3\r\n$3\r\nSET\r\n...`，不需要理解 AOF 的追加写盘，不需要理解 epoll 的 EPOLLONESHOT——就是一个 TCP 收、一把 `strtok` 切、一个 switch-case 分发、一个数据结构存、一个 `sprintf` 拼响应、一把 `send` 发回去。

但正是这六个"一个"，奠定了后续所有迭代的基础。

## 2.4 早期版本有哪些不足——从复盘角度看

早期版本跑通了，但这不代表它没有问题。恰恰相反，正是因为代码足够简单，问题才暴露得足够清晰。以下从复盘角度梳理早期版本的内在不足——不是为了批评当时的实现，而是为了说明"为什么后续每个迭代都是必要且自然的"。

### 2.4.1 协议解析：一次 recv 不等于一条完整命令

早期版本的核心假设是：**一次 `recv` 读到的数据恰好是一条完整的命令**。`kvs_split_token` 直接在 `rbuffer` 上调 `strtok`，没有检查数据是否完整、没有处理半包和粘包。

但 TCP 是字节流协议，不保证消息边界。实际运行中会出现三种情况：

- **半包**：客户端发了 `"SET Teacher King"`，但服务端一次 `recv` 只读到了 `"SET Teach"`——命令不完整，解析失败。
- **粘包**：客户端连续发了 `"SET A 1"` 和 `"SET B 2"`，服务端一次 `recv` 把两条都读进来了——`strtok` 会错误地把 `"1 SET B 2"` 当成一条命令的内容。
- **Pipeline**：客户端一次发送多条命令（比如 `"SET A 1\r\nSET B 2\r\nSET C 3\r\n"`），早期版本完全无法处理——它不知道哪里是一条命令的结束位置。

这些问题的根因是同一个：**缺少应用层协议帧界定**。后续引入 RESP 协议（commit `2208866`，2025-08-17）用 `\r\n` 和长度前缀定义消息边界，以及 reactor 的粘包处理（commit `6d6b62c`，2025-08-26），都是在解决这个根本问题。

### 2.4.2 命令分发：线性扫描 + 巨型 switch-case 无法扩展

早期版本的命令分发做了两重遍历：先 for 循环遍历 `command[]` 字符串数组找到命令枚举，再 switch-case 跳到具体处理逻辑。当命令只有 5 个时（SET/GET/DEL/MOD/EXIST），这不是问题；但当四种数据结构各自有 5 条命令，再加上 MOD/EXIST 等辅助命令时，switch-case 的分支数量直接膨胀到约 20 个。

更糟糕的是，每个 case 分支内部的结构几乎一模一样：

```c
case KVS_CMD_XXX:
    ret = kvs_xxx_xxx(&global_xxx, key, value);
    if (ret < 0) { length = sprintf(response, "ERROR\r\n"); }
    else if (ret == 0) { length = sprintf(response, "OK\r\n"); }
    else { length = sprintf(response, "EXIST\r\n"); }
    break;
```

这种"结构相同、参数不同"的重复代码是典型的"需要抽象"信号。后续 commit `b805a36`（2025-09-05）把业务逻辑从 `kvs_filter_protocol` 拆到 `kvs_operation.c` 中，并引入了 `command_table` 统一注册和查找，解决了这个问题。

### 2.4.3 多数据结构缺乏统一接口

早期版本中，array 的命令叫 `SET`/`GET`，红黑树的叫 `RSET`/`RGET`，哈希的叫 `HSET`/`HGET`，跳表的叫 `SSET`/`SGET`。命令名本身区别了数据结构，调用者必须先知道"我要存到哪里"，再选对应的命令前缀。

这种设计有两个问题。第一，调用者耦合了存储实现——如果想把一个 key 从 array 迁到红黑树，必须改命令前缀。第二，命令层直接耦合了每种数据结构的具体函数——`kvs_array_set`、`kvs_rbtree_set`、`kvs_hash_set`、`kvs_skiptable_set` 在 switch-case 里各占一个分支，加一种新数据结构就要改命令分发代码。

后续版本通过统一的存储接口抽象解决了这个问题——命令名不再包含数据结构类型，而是由配置或 key 属性决定使用哪种存储引擎。这是"面向接口编程"在 C 语言项目中的一次具体实践。

### 2.4.4 数据只在内存中，重启即丢失

早期版本的所有数据都保存在进程堆内存中（`malloc` 出来的 key 和 value 字符串）。进程退出后，所有数据全部丢失。

这个问题在 commit `b8ab141`（2025-08-06）开始被解决——引入了文件保存和启动加载功能。后续 AOF（commit `16f807b`，2025-09-07）和 RDB（在持久化模块中逐步完善）分别提供了操作日志回放和快照恢复两种持久化策略。从"纯内存"到"可持久化"，是 KV 存储从玩具走向实用的关键一步。

### 2.4.5 没有 TTL，没有复制，没有系统化 benchmark

早期版本可以存取数据，但它还缺少一个"完整 KV 系统"需要具备的几项关键能力：

- **TTL（过期机制）**：没有 key 过期能力。所有 key 都是永久的，除非显式 DEL。TTL 在 2026 年 3 月作为一个完整的子系统被加入（涉及所有数据结构、AOF/RDB、TCP 复制、eBPF 同步路径）。
- **主从复制**：单节点，没有数据冗余。commit `61f267a`（2025-08-12）实现了基于 TCP 的全量 + 增量同步，后续 eBPF 路径（2025-11~12）实现了内核级旁路同步。
- **系统化 benchmark**：早期只有 testcase 的功能验证，没有 QPS 压测。后续引入了 `write_qps_test`、Redis 对比 benchmark、TTL 各路径 QPS 测试、十万级向量检索压测。

需要强调的是：**早期版本没有这些东西是正常的，也是正确的**。如果一开始就试图把 TTL、持久化、主从复制、RESP 全部塞进去，项目大概率会陷入"什么都想做，什么都跑不通"的困境。先做一个能跑的最小子集，然后在真实使用中发现问题、驱动迭代，这才是合理的工程节奏。

## 2.5 这个阶段我真正学到了什么

回头看 2024 年 5 月那七天写出来的代码，技术含量不高——没有巧妙的数据结构优化，没有精密的并发控制，没有复杂的协议设计。但这恰恰是这个阶段最大的价值：**它教给我的不是"怎么写更好的代码"，而是"怎么做软件工程的决策"**。

### 第一，不要一开始就追求完整系统

如果 2024 年 5 月就试图把 RESP、AOF、RDB、主从复制、TTL 全部设计好再开始写代码，KVLite 到今天可能还停留在设计文档阶段。早期版本的价值恰恰在于它不完整——正因为只做了最小闭环，所以三天之内就能把网络、解析、存储三层全部跑通，然后拿着一个能跑的东西去发现下一个问题。

### 第二，最短链路是项目的"第一口气"

一个能跑的 `SET` → `OK` 闭环，比一千行设计文档更能说明项目的可行性。这条闭环给了后续所有迭代一个锚点：每次加新功能，改法都是"在这条链路的某个环节插入新逻辑"。RESP 插在解析层前面，AOF 插在命令执行后面，TTL 插在数据结构查找路径上，主从复制插在响应返回之前。没有这条链路，所有模块都不知道"自己应该挂在哪个位置"。

### 第三，早期代码的价值是暴露问题

`strtok` 解析会暴露 TCP 粘包问题、线性扫描的 array 会暴露查找性能问题、巨型 switch-case 会暴露扩展性问题——这些"问题"不是早期代码的缺陷，而是早期代码的**产出**。写代码的目的不是一次写对，而是写出来之后，能让问题变得可见、可触摸、可讨论。看见问题了，下一个迭代就有了明确的方向。

### 第四，每次迭代都应该回应一个真实问题

翻看 commit log 能发现一个清晰的模式：每个后续迭代都对应着一个早期版本暴露的具体问题：

| 早期问题 | 后续迭代 |
|---------|---------|
| strtok 解析无法处理半包/粘包 | RESP 协议解析（`2208866`） |
| 一次 recv 一条命令，无法批量处理 | Pipeline 支持（`2deb462`） |
| 命令分发 switch-case 膨胀 | 业务逻辑拆分到 kvs_operation.c（`b805a36`） |
| 数据只存在内存中 | 文件保存（`b8ab141`）→ AOF（`16f807b`）→ RDB |
| 没有过期机制 | TTL 子系统（2026-03） |
| 单节点无冗余 | 主从复制 TCP（`61f267a`）→ eBPF（`82f8034`） |
| 缺少系统化压测 | benchmark 脚本和十万级压测 |

没有一个迭代是为了"技术炫技"而做的。每一个都是在回应"当前版本缺什么"。

### 第五，这也是我现在重新复盘项目的原因

距离第一个 commit 已经过去了两年。KVLite 从只有一份 README 模板，变成了一个集成了网络框架、四种数据结构、持久化、主从复制、eBPF 内核同步、向量语义检索的完整系统。但如果不去回头看最初那几行代码，很容易产生一种错觉——好像这些东西是一开始就设计好的，好像架构是"想清楚再写"的结果。

实际上不是。它是一步步长出来的，每一步都在解决一个具体的问题。我想通过这个复盘系列，把"长出来"的过程还原出来——不是为了展示自己多厉害，而是为了记录一个真实的、充满试错和迭代的工程过程。

这也是费曼学习法的核心：如果你不能把一件事从头到尾讲清楚，说明你还没有真正理解它。而把 KVLite 从第一个 commit 开始重新走一遍，就是我在做的"讲清楚"。

---

*第二章完。下一章主题：RESP 协议为什么是必要的，以及协议解析如何从 strtok 演进到完整 RESP 解析器。*
