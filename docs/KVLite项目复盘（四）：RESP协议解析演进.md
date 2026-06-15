# 第四章 RESP 协议解析：为什么从 strtok 演进到 peek + parse 两阶段解析

## 4.1 早期 strtok 解析为什么能跑，但不能继续用

回顾第二章里最早的命令解析代码，它做了一件非常简单的事情：

```c
int kvs_split_token(char *msg, char *tokens[]) {
    int idx = 0;
    char *token = strtok(msg, " ");
    while (token != NULL) {
        tokens[idx++] = token;
        token = strtok(NULL, " ");
    }
    return idx;
}
```

客户端发过来 `"SET Teacher King"`，`strtok` 按空格切三刀，得到三个 `char*` 指针，丢给 `kvs_filter_protocol` 去做命令分发。一共不到 15 行代码，原型就跑通了。

最早我选 `strtok` 的原因非常朴素：它够快。不是性能快，而是"写代码快"——不用定义协议格式，不用写解析器，不用考虑缓冲区管理，`strtok` 一行搞定。在只有 SET/GET/DEL/MOD/EXIST 五条命令、value 不包含空格、客户端一问一答的场景下，这个方案没有任何问题。

但随着项目往前推进，`strtok` 的问题一个接一个暴露出来：

**不能处理带空格的 value。** `strtok` 用空格作为分隔符，如果用户想 `SET motto "stay hungry stay foolish"`，整句话会被切成七八个 token，解析层根本不知道哪几个属于 value。

**不能处理二进制 key/value。** `strtok` 依赖 C 字符串的 `\0` 结尾。如果 key 或 value 中间有 `\0` 字节，`strtok` 会在那里截断，剩下的一半数据就丢了。这意味着 KVLite 无法存储二进制数据——而 Redis 之所以能存二进制，正是因为 RESP 的 bulk string 用长度而非 `\0` 描述数据。

**无法判断一条命令到底有多长。** `strtok` 只知道"切到没有为止"，不知道"一条命令的边界在哪里"。当客户端连续发送多条命令时，`strtok` 会直接把第二条命令的内容也切进 tokens 里，造成串数据。

**会修改原始 buffer。** `strtok` 用 `\0` 替换分隔符来切分字符串，这会破坏 recv buffer 里的原始数据。如果后续需要从 buffer 中取原始字节做校验或重放，数据已经面目全非了。

**没有错误恢复机制。** 如果客户端发了一段乱码，`strtok` 切不出合法 tokens，但解析层不知道"到底哪里开始是乱码、哪里恢复了正常"，只能整个连接断掉重来。

我想强调的一点是：**这不是早期设计做错了，而是 `strtok` 天然只适合原型阶段。** 它让项目在三天之内跑通了 SET/GET 闭环，证明了链路是通的。但一旦要承接真实 TCP 场景下的半包、粘包、Pipeline、二进制安全这些需求，`strtok` 就完全不能胜任了。原型阶段的任务是"快速验证可行性"，而 RESP 引入的任务是"建立一个可持续演进的应用层协议基础"——这是两个阶段，两种目标。

## 4.2 为什么选择 RESP

到了 2025 年 8 月（commit `2208866`），KVLite 面临一个问题：用什么协议替换空格文本？

当时有几个选项。可以自定一个二进制协议（比如前 4 字节放命令长度，后面跟命令数据），也可以参考 HTTP 的文本行格式。最后选择 RESP，基于三个非常务实的考虑：

**第一，结构简单。** RESP 只有五种类型——简单字符串（`+`）、错误（`-`）、整数（`:`）、批量字符串（`$`）、数组（`*`）。一条命令就是"一个数组包含若干个批量字符串"。不需要复杂的 TLV 编码，不需要 IDL 定义，一看就懂。

**第二，与 Redis 生态兼容。** 用 RESP 意味着可以直接拿 `redis-cli` 连上 KVLite 做调试，可以直接用 `redis-benchmark` 做压测对比。这对于后续的 benchmark 工作来说省了大量力气——不用自己写一个压测工具就能对比 KVLite 和 Redis 的性能。后面第三章里提到的那些十万级压测，很大程度都受益于"协议和 Redis 一致"这个决定。

**第三，长度前缀天然解决消息边界问题。** 这是最关键的一点。RESP 的格式是这样：

```text
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nzhengpan\r\n
```

逐段解读：

- `*3` — 这是一个包含 3 个元素的数组
- `$3` — 第一个 bulk string 有 3 个字节
- `SET` — 第一个 bulk string 的内容
- `$4` — 第二个 bulk string 有 4 个字节
- `name` — 第二个 bulk string 的内容
- `$8` — 第三个 bulk string 有 8 个字节
- `zhengpan` — 第三个 bulk string 的内容
- 每段以 `\r\n` 结尾

注意这个格式的巧妙之处：**你不需要靠分隔符来推断参数边界，因为每个参数的长度都已经明确写在前面了。** 解析器读到一个 `$N`，就知道接下来要读 N 个字节的数据再加上 `\r\n`。数据不够？那就等着。够了？就读走。不需要猜。

这就是 RESP 对 TCP 字节流场景的天然适配：第三章里讲的半包、粘包问题，在 RESP 格式下变成了一个可以精确计算的工程问题——"缓冲区里有几个字节"和"$N 告诉我需要几个字节"比一下，就知道数据齐了没有。

## 4.3 RESP 解决的不是"怎么执行命令"，而是"怎么从字节流还原参数列表"

这里有一个重要的概念区分。协议层的职责不是"处理 SET/GET 的业务语义"，而是做一件事：**把 TCP 字节流中连续的、无意义的字节，还原成有结构的参数列表。**

用一张简图来表示这个转换：

```text
TCP 字节流 (无结构)
  *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nzhengpan\r\n
      │
      ▼  RESP parser（协议层）
      │
tokens[] & token_lens[] (有结构)
  tokens[0] = "SET",        token_lens[0] = 3
  tokens[1] = "name",       token_lens[1] = 4
  tokens[2] = "zhengpan",   token_lens[2] = 8
  arg_index = 3
      │
      ▼  command handler（命令层）
      │
  → kvs_filter_protocol → lookup_command → handle_set
```

网络层看到的是字节。命令层需要的是参数。协议层就是中间这个转换器。

这里我想特别讲一下 `token_lens`。在当前代码中，RESP 解析器为每个参数同时保存了 `tokens[i]`（C 字符串指针）和 `token_lens[i]`（真实字节长度）。为什么需要两个？

因为 C 字符串以 `\0` 结尾，`strlen(tokens[i])` 只能在数据中不含 `\0` 时给出正确长度。如果用户存储的是一个二进制数据（比如一段序列化的 protobuf），数据中间完全可能有 `\0` 字节，此时 `strlen` 会严重低估实际长度。

`token_lens` 是 RESP 解析器从 `$N` 中直接读出来的——它不依赖 `\0`，所以不管数据内容是什么，长度都是准的。后面的 `command_context_t`（定义在 `include/kvs_command.h:18-38`）也同时保存了 `key`/`key_len` 和 `value`/`value_len`，正是为了让整个调用链都能拿到真实长度。

当然，诚实地说，KVLite 目前并不是在所有路径上都完整地使用了 `token_lens`。早期从 `strtok` 时代迁过来的部分代码仍然在依赖 `strlen`，一些存储操作在比较 key 时用的是 `strcmp` 而不是 `memcmp`。这个问题在后续的优化迭代中逐步在修——第四章能做的就是指出这个方向和已经打下的基础。

## 4.4 为什么要先 peek 再 parse

这是第四章最想讲清楚的一个设计决策。

在第一次引入 RESP 的 commit `2208866`（2025-08-17）中，其实只有一个解析函数 `resp_parser`。它直接面对 buffer 做有状态解析——读一个字符推进一个状态，数据不够就返回 0 等更多数据，数据够了就一路解析到底。

这个设计在当时是够用的。但它有一个隐含的问题：**`resp_parser` 是一个有状态的解析器。它在解析过程中会逐步填充 `parser->tokens` 数组，分配内存，写入数据。如果解析到一半发现数据不完整，这些中间状态就卡在半路——不做清理，下次再调就可能读脏数据；清理掉，下次又要重新 malloc。**

当 Pipeline 处理加进来之后，问题变得更加尖锐。在同一个 recv_buf 中可能有十几条命令。每条命令在交给 `resp_parser` 之前，必须先知道"这条命令占多少字节"——否则解析器不知道从哪里开始、到哪里结束。但如果用 `resp_parser` 去探测边界，就需要跑完整套解析流程，包括分配 tokens 内存——而如果解析到一半发现不完整，还要再释放掉。这种"先跑一遍再回滚"的模式效率太低。

解决方案就是把"判断命令边界"和"解析命令内容"拆成两个步骤。这就是 `resp_peek_command` 和 `resp_parser` 的关系：

**`resp_peek_command`（无状态探测）**：在 `src/network/resp_protocol.c:89-180`。它不分配任何内存，不修改任何 parser 状态，只做一件事——从头到尾读一遍 RESP 结构，判断从 buffer 的某个 offset 开始，是否存在一条完整命令。

它的逻辑非常直白：读 `*N` → 确定有 N 个 bulk string → 循环 N 次，每次读 `$L`，检查 `L+2` 个字节是否都在 buffer 内 → 都在？那就走到下一个。如果中间任何一步发现数据不够（比如 `$L` 要求 100 字节但 buffer 里只剩 30 字节），立刻返回 0，表示"数据不完整"。

如果整条命令的结构完整，返回它总共占用的字节数（`cmd_len`）。如果格式错误（比如 `*` 后面不是数字，或者 `$L` 后面的 `\r\n` 缺失），返回 -1。

**`resp_parser`（有状态解析）**：在确认 `resp_peek_command` 返回了正数 `cmd_len` 之后，`resp_parser` 才被调用。它只在已知"cmd_len 范围内数据是完整且格式正确的"这个前提下工作，所以可以放心地分配内存、拷贝数据、填充 tokens。

现在回头看，**peek 的价值在于把"命令边界判断"从"命令内容解析"里拆了出来。** 没有 peek 的时候，parse 要同时做两件事：判断够不够，和解析内容。两个职责糅在一起，导致 Pipeline 批处理时效率低下、代码路径复杂。拆开之后，每个函数只做一件事，组合起来更清晰：

```text
peek:  "这条命令完整吗？占多少字节？"       → 无状态，只读不写
parse: "这段字节具体是什么参数？"           → 有状态，构造 tokens
```

`resp_peek_command` 是在 commit `b2e3ba3`（2025-11-20）中引入的，距离第一次 RESP 实现大约过了三个月。这个时间差正好说明：**两阶段解析不是一开始就设计好的，而是 Pipeline 优化过程中暴露出来的需求。**

## 4.5 net_session_process_resp 如何串起 recv_buf、peek、parse 和 handler

第三章讲了 `net_session_process_resp` 的总体流程，第四章聚焦它在 RESP 解析层面的具体行为。以下是结合当前源码的核心循环（`src/network/net_session.c:115-188`），每条命令的处理步骤：

```text
recv_cb
  │ 从 socket recv 数据，追加到 session.recv_buf
  ▼
net_session_process_resp(fd, &session, handler, user_data)
  │
  │  1. 检查 processing 标志（防止重入）
  │  2. 首次对齐检查：跳过前导空白，确认第一个非空白字符是 '*'
  │     （net_session.c:78-110）
  │
  │  3. 主循环（while parsed_total < recv_buf.size）
  │
  ├─→ resp_peek_command(buf, size, parsed_total)
  │     │
  │     ├─ 返回 cmd_len > 0：有一条完整命令，占 cmd_len 字节
  │     ├─ 返回 0：数据不够，break 等下次 recv
  │     └─ 返回 -1：协议错误，return -1，上层关闭连接
  │
  ├─→ resp_parser(&parser, buf + parsed_total, cmd_len)
  │     │
  │     │  有状态解析：*N → $L → body → CRLF
  │     │  逐字段填充 parser->tokens[] 和 parser->token_lens[]
  │     │  完成后 parser->state = RESP_PARSE_COMPLETE
  │     │  parser->arg_index = 参数个数
  │     │  parser->tokens[i] = C 字符串（末尾补了 \0）
  │     │  parser->token_lens[i] = 真实字节长度
  │     │
  │     └─ 返回已解析字节数（应等于 cmd_len）
  │
  ├─→ handler(&parser, response_buf, response_buf_cap, user_data)
  │     │
  │     │  handler 拿到 parser->tokens、parser->arg_index、parser->token_lens
  │     │  调用 kvs_filter_protocol(tokens, arg_index, token_lens, response_buf)
  │     │  内部：lookup_command → check_arity → parse_command_argument → handler(&ctx)
  │     │  生成 RESP 格式响应字符串，返回长度
  │     │
  │     └─ 返回 resp_len
  │
  ├─→ session->send_buf.size += resp_len   （响应追加到发送缓冲区）
  ├─→ parsed_total += cmd_len              （累计已消费的字节数）
  ├─→ commands_count++
  └─→ resp_parser_reset(&parser)           （重置状态，准备下一条）
  │
  │  4. netbuf_consume_front(&recv_buf, parsed_total)
  │     → 从 recv_buf 头部裁掉所有已处理的数据
  │
  │  5. processing = 0
  ▼
回到 recv_cb → 根据 send_buf 状态决定是否注册 EPOLLOUT
```

每个步骤的职责：

- **recv_cb**：只管往 recv_buf 里塞字节，不管字节内容。
- **resp_peek_command**：只管"有没有完整命令、占多少字节"，不分配内存、不修改状态。
- **resp_parser**：只管"把已知完整的一段 RESP 字节流转成 tokens + token_lens"，不做边界判断。
- **handler（reactor_session_handler → kvs_filter_protocol）**：只管"拿到 tokens 之后做什么"，不知道 RESP 长什么样。
- **netbuf_consume_front**：只管"把已处理的数据从 buffer 头部裁掉"，不关心裁掉的是什么。

这个分层的好处是每一层都可以独立测试、独立替换。比如写 RESP 解析的单元测试时，只需要准备一段 RESP 字节流喂给 `resp_peek_command` 和 `resp_parser`，完全不需要启动网络服务。

## 4.6 半包、粘包和 Pipeline 在 RESP 层怎么处理

第三章从 buffer 角度讲了半包、粘包和 Pipeline。这一节从 RESP 解析角度再讲一遍，因为这是"为什么 RESP 格式本身就能解决这些问题"的核心论证。

### 半包：数据不够，peek 说不完整

客户端发送的 RESP 命令在传输过程中被截断了。比如完整命令应该长这样：

```text
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nzhengpan\r\n
```

但由于网络分包，服务端第一次 recv 只读到了前半段：

```text
*3\r\n$3\r\nSET\r\n$4\r\nna
```

`resp_peek_command` 开始扫描：读到 `*3`，要解析 3 个 bulk string。第一个 `$3\r\nSET\r\n` 完整。第二个 `$4\r\n` 告诉它需要 4 字节数据 + `\r\n`，但 buffer 里只剩 `na` 两个字节。

```c
size_t need = (size_t)bulk_len + 2;      // need = 4 + 2 = 6
if (remain < need) return 0;             // remain = 2, 2 < 6 → 返回 0
```

`resp_peek_command` 返回 0。`net_session_process_resp` 的循环 break，recv_buf 中的数据原封不动。下次 recv 追加上 `me\r\n$8\r\nzhengpan\r\n` 后，buffer 变完整了，peek 就能返回完整的 `cmd_len`。

关键点：**peek 的"返回 0"是一种正常状态，不是错误。** 它意味着"我能看懂这个格式，但数据还没到齐，你再等等"。这和协议错误（返回 -1）有本质区别。

### 粘包：第一条完整，第二条不完整

服务端一次 recv 读到了两条命令的数据，但第二条命令的数据不完整。比如：

```text
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$8\r\nzhengpan\r\n    ← 第一条，完整（45 字节）
*3\r\n$3\r\nGET\r\n$4\r\nna                            ← 第二条，不完整（只到了 na）
```

`resp_peek_command` 从 offset=0 开始扫描：第一条命令完整，返回 cmd_len=45。

`resp_parser` 解析 offset=0 到 offset=45 这段，handler 执行 SET。`parsed_total += 45`。

循环继续，`resp_peek_command` 从 offset=45 开始扫描：读到 `*3`，需要 3 个 bulk string。第一个 `$3\r\nGET\r\n` 完整。第二个 `$4\r\n` 需要 4+2=6 个字节，但 `remain` 只有 2（即 `na`）。返回 0。

break。`netbuf_consume_front(&recv_buf, 45)` 裁掉第一条的 45 字节。recv_buf 里剩下第二条命令的前半段。等下次 recv 补齐。

### Pipeline：循环处理多条完整命令

客户端一口气发来 50 条 RESP 命令，服务端一次 recv 全部读进来了。

`net_session_process_resp` 的 while 循环会逐条处理：peek 第一条 → parse 第一条 → handler 执行 → resp_parser_reset → peek 第二条 → parse 第二条 → … 直到：

1. peek 返回 0（最后一条不完整），或者
2. `parsed_total >= recv_buf.size`（buffer 里所有数据都处理完了），或者
3. `commands_count >= NET_SESSION_MAX_COMMANDS_PER_BATCH`（达到单次批处理上限）

最后，`netbuf_consume_front` 一次性裁掉 `parsed_total` 字节。

我想在这里强调一个重要的认知：**Pipeline 本质上不是网络层做了什么特殊处理，而是协议层可以在同一个 recv_buf 中连续识别出多条完整命令。** 网络层不需要知道"这是一批命令还是单条命令"——它只管把字节塞进 buffer。Pipeline 是 RESP 格式 + peek/parse 两阶段解析自然带来的能力，不是额外实现的特性。

## 4.7 RESP 对 binary-safe key/value 的意义

KVLite 早期所有的 key/value 操作都依赖 C 字符串：`strcmp` 比较 key，`strlen` 获取长度，`strncpy` 复制。这在文本场景下没有问题，但当 value 是二进制数据（比如序列化后的 protobuf、一张图片的缩略图、一段加密后的 token）时，C 字符串就不够用了。

RESP 的 bulk string 设计天然支持二进制安全，因为它用**长度而非终止符**来描述数据：

```text
$10\r\nhe\x00lo\x00wo\r\n
```

这个 bulk string 包含 10 个字节，中间有两个 `\0`。如果依赖 `strlen`，只能读到 2。但 RESP parser 从 `$10` 中明确知道它是 10 个字节，`memcpy` 完整拷贝 10 字节，然后在末尾补 `\0` 的同时把 `token_lens[0]` 记为 10。

这就是为什么 `token_lens` 必须和 `tokens` 一起保存。上层代码在使用这些数据时，应该：

- 用 `token_lens[i]` 来获取真实数据长度
- 用 `memcmp` 而不是 `strcmp` 来做 key 的比较
- 用 `memcpy` + 长度来复制数据，而不是 `strcpy`/`strncpy`

在 KVLite 当前的代码中，`command_context_t` 已经同时携带了 `key`/`key_len` 和 `value`/`value_len`（定义在 `include/kvs_command.h:18-38`），存储层的 `kvs_array_set_len`、`kvs_rbtree_set_len` 等接口也接受 `key_len` 和 `value_len` 参数。跳表的 `Node` 结构体中也保存了 `key_len`（`src/network/../kvs_skiptable.c`）。

这些设计从 RESP 的 `$L` 一路贯穿到了存储层——虽然当前还没有在所有路径上完整覆盖（比如 `kvs_hash` 的某些路径仍然依赖 `strlen`），但数据结构和接口层面的准备已经做好了。后续如果想全面支持二进制安全 key/value，改法就是沿着已经存在的 `_len` 参数和 `token_lens` 字段，把 `strcmp` 替换成 `memcmp` 即可。这个改造的难度不会特别大，因为"长度"这条线索已经被 RESP 的 bulk string 格式带进了整个系统。

## 4.8 当前 RESP 解析设计解决了什么，又留下了什么问题

### 解决了什么

1. **命令边界明确**。RESP 的 `*N` + `$L` 结构让每条命令的字节长度可以被精确计算，不需要依赖分隔符或启发式猜测。

2. **半包可以安全保留**。`resp_peek_command` 返回 0 时，recv_buf 中的数据完全不动，等下次 recv 追加后重试。

3. **粘包可以逐条消费**。peek 返回第一条命令的长度 → parse 第一条 → consume 第一条 → 剩余数据自然就是下一条（或下一条的半包）。

4. **Pipeline 可以循环解析**。while 循环 + peek/parse/reset 的组合让同一次 recv_buf 中的数据可以逐条处理，Pipeline 从"额外实现"变成了"自然结果"。

5. **命令层只面对 tokens**。`kvs_filter_protocol` 拿到的是 `char** tokens`、`int count`、`size_t* token_lens`，完全不知道这些数据来自于 RESP 还是别的什么协议。如果将来想支持别的协议格式，只要协议层能产生同样格式的 tokens，命令层一行都不用改。

6. **支撑 binary-safe 的基础**。`token_lens` 保存了从 RESP `$L` 中读出的真实字节长度，不依赖 `\0` 结尾，为二进制 key/value 存储提供了数据基础。

7. **兼容 Redis 生态**。`redis-cli` 可以直接连 KVLite 调试，`redis-benchmark` 可以直接压测 KVStore vs Redis 的性能对比。

### 留下了什么问题

1. **KVLite 当前只实现了 RESP 的数组 + bulk string 子集**。RESP2 定义的五种类型中，KVLite 只完整实现了客户端请求路径上必需的数组和批量字符串。服务端响应虽然支持简单字符串（`+`）、错误（`-`）、整数（`:`）、批量字符串（`$`），但客户端请求层面不支持 RESP3 的 map、set、null、bool、double 等新类型。这是一个务实的选择——KV 命令只需要数组和 bulk string 就够用——但需要明确它是一个"子集实现"，不是"完整 Redis 协议栈"。

2. **不支持 inline command**。Redis 允许客户端以空格分隔的文本格式直接发送命令（如 `SET key value\r\n`），KVLite 不支持。`net_session_process_resp` 在首次对齐检查时，如果第一个非空白字符不是 `*`，会直接返回协议错误（`net_session.c:97-102`）。

3. **协议错误的恢复策略比较简单**。`resp_peek_command` 或 `resp_parser` 返回 -1 时，`net_session_process_resp` 直接返回 -1，上层关闭连接。没有尝试"跳过错误数据找到下一个 `*`"的恢复机制。这对于本地压测和原型场景来说足够，但在公网环境下可能过于激进——一个格式错误的包就断连接。

4. **tokens 内存是在 RESP parser 中分配的**，每次 `resp_parser_reset` 会释放上一次的 tokens 内存。在高频 Pipeline 场景下，频繁的 malloc/free 会成为开销。一种优化方向是让 parser 复用 tokens 数组，只在需要扩容时才重新分配——当前代码中的 `array_len_allocated` 和 `parser->tokens` 已经通过比较 `array_len` 大小的方式做了部分复用，但每条命令的 bulk string 数据本身仍然每次 malloc。

5. **token_lens 还没有在所有路径上被完整使用**。虽然 RESP 解析器保存了 `token_lens`，并且通过 `kvs_filter_protocol` → `parse_command_argument` → `command_context_t` 传递到了命令层，但某些存储操作（特别是早期实现的 hash 和 skiptable 部分路径）可能仍然依赖 `strlen` 而不是传入的 `_len` 参数。这意味着在极端情况下（key/value 中包含 `\0`），这些路径可能出现行为不一致。

6. **超大 bulk string 的限制**。`MAX_BULK_LENGTH` 当前设置为 10MB（`include/resp_protocol.h:30`）。这是对单条命令中单个参数大小的限制，防止内存攻击。但 10MB 对于某些使用场景（比如用 KVLite 做 AI Chat 会话缓存的完整对话历史存储）可能太小。这个值是可以在配置层面调整的，当前硬编码为宏定义。

这些都是可以在后续迭代中逐步完善的，没有一个属于"当前无法工作"的阻塞性问题。KVLite 的 RESP 解析在"支撑整个 KV 系统的命令处理"这一点上是完整且可用的。

## 4.9 面试时我怎么讲这个阶段

### 30 秒版

问："为什么你们要从 strtok 换成 RESP 解析？"

答："最早用 strtok 按空格切是因为它够快——三行代码就能跑通原型。但 strtok 无法处理带空格的 value、无法处理二进制数据、也无法判断一条命令的边界。换成 RESP 之后，一切都不一样了——bulk string 的 `$N` 明确告诉解析器"这个参数有多长"，所以半包时可以精确判断数据够不够，粘包时可以逐条切出命令边界，Pipeline 时可以循环解析多条命令。本质上，RESP 的长度前缀设计天然解决了 TCP 字节流的消息边界问题。"

### 2 分钟版

问："KVLite 的 RESP 解析是怎么支持半包、粘包和 Pipeline 的？"

答："KVLite 的 RESP 解析经历了两轮迭代。第一版是最简单的——用 strtok 按空格切字符串，切完直接执行。这在原型阶段完全够用，但一遇到连续的、大批量的请求就原形毕露——TCP 字节流下面没有消息边界，strtok 根本不知道一条命令到哪里结束。

第二版引入了 RESP 协议。RESP 的核心是数组加带长度的批量字符串——每个参数前面有个 `$N` 告诉你这个参数有多长。这就把一个模糊的'命令到哪结束'的问题变成了精确的'需要读多少字节'的算术题。

但这还不够。RESP 格式本身不能解决'怎么高效地从 buffer 中识别多条命令'的问题。所以我又把解析拆成了两步：先 peek，再 parse。

peek 是一个无状态函数——它不分配内存，不修改任何状态，只扫描 buffer 判断'有没有完整命令、占多少字节'。如果数据不够，返回 0，buffer 原封不动等下次 recv。这就是半包的处理。

如果 buffer 里有完整命令，peek 返回 cmd_len，然后 parse 用 cmd_len 限定范围做有状态解析——分配内存、拷贝数据、构造 tokens 和 token_lens。为什么 token_lens 重要？因为 RESP 的 `$N` 给出的长度不依赖 `\0`——如果 value 里有 `\0`，strlen 算不出来正确长度，但 token_lens 始终是对的。这为二进制安全的 key/value 打下了基础。

parse 完成后 handler 执行命令，响应写入 send_buf，然后 resp_parser_reset 重置解析状态，从 buffer 头部裁掉已处理的数据，接着 peek 下一条——这就是 Pipeline 的循环处理。Pipeline 本质上不是网络层技巧，而是协议层可以在同一个 buffer 中连续识别多条完整命令的自然结果。

整个设计解决的核心问题就是一件事：在 TCP 字节流之上，如何安全、高效地还原出结构化参数。peek 管边界，parse 管内容，handler 管业务，三层各司其职。"

## 4.10 本章小结

这一章解释了 KVLite 的协议层为什么从 `strtok` 按空格切分演进到了 RESP 的 `peek + parse` 两阶段解析。

`strtok` 是原型阶段的合理起点——它用极少的代码验证了"客户端发命令 → 服务端解析 → 存储执行"这条链路是通的。但它隐含的"参数不含空格、数据不含 `\0`、一次 recv 就是一条完整命令"的假设，在真实 TCP 场景下全部不成立。

RESP 协议的核心价值不是"表达命令"，而是**给字节流提供明确的消息边界**。每个参数前面带长度（`$N`），让解析器可以把"这条命令完整吗"转化成一个可精确计算的判断——不需要猜，不需要试探。

peek + parse 的两阶段设计进一步把这件事拆成了两个独立职责：peek 负责判断命令边界（无状态、只读不写），parse 负责解析命令内容（有状态、构造 tokens）。这个拆分让半包、粘包、Pipeline 都能被统一处理——Pipeline 不是额外实现的功能，而是"在同一个 buffer 中连续 peek 到完整命令"的自然结果。

`token_lens` 的保存为 binary-safe key/value 打下了数据基础——虽然当前还没有在所有路径上完整覆盖，但 RESP → `token_lens` → `command_context_t._len` → 存储层 `_len` 接口这条线索已经贯穿了整个系统。

下一章将复盘命令分发层的演进——为什么从早期"for 循环匹配字符串 + 巨型 switch-case"演进到了 `command_table` 注册表 + 统一 handler 接口，以及命令层如何和协议层、存储层解耦。

## 4.11 本章对应的面试追问清单

1. 为什么早期 `strtok` 不能继续用？它具体有哪些问题？
2. `strtok` 会修改原始 buffer 吗？这会带来什么隐患？
3. `strtok` 能处理带空格的 value 吗？如果不能，为什么早期没有暴露？
4. RESP 的 `$N` 为什么能解决 TCP 半包问题？
5. 如果不用 RESP，自定一个二进制协议要怎么设计消息边界？
6. `resp_peek_command` 返回 0 和返回 -1 有什么区别？分别对应什么场景？
7. 为什么需要先 peek 再 parse？只用 parse 不行吗？
8. `resp_peek_command` 是无状态的，`resp_parser` 是有状态的——你怎么理解"有状态"和"无状态"这两种设计？
9. `token_lens` 和 `strlen(tokens[i])` 有什么区别？为什么不能只用 `strlen`？
10. Pipeline 在 RESP 层面是怎么实现的？需要网络层做特殊处理吗？
11. 协议错误时，`net_session_process_resp` 做了什么？连接会被关闭吗？
12. `resp_parser_reset` 做了什么？为什么解析完一条命令之后需要 reset？
13. `netbuf_consume_front` 在裁掉已解析数据时，内部用的是 `memmove`。为什么不能直接把指针往后移？
14. KVLite 的 RESP 实现支持 RESP2 的全部类型吗？不支持哪些？不支持的怎么处理？
15. `redis-cli` 能直接连 KVLite 吗？`redis-benchmark` 呢？为什么能或不能？
16. 如果你想在 KVLite 中支持 RESP3 的新类型（如 map、push），需要改哪些地方？

---

*第四章完。下一章主题：命令分发层——从 switch-case 到 command_table 注册表的演进。*
