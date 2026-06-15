# 阶段 3.5：session 生命周期与异步响应前置设计

## 实现状态：阶段 4A 前置安全底座

- 当前状态：已补齐 reactor-only executor 前置安全能力，但未启用 executor。
- session：已增加 identity/generation/closing/refcount/owner_poller/single_inflight 等字段和辅助函数；reactor 注册连接时设置 identity/generation，注销连接时标记 closing。
- response_task：已支持 owned buffer 模式；当前同步路径仍使用 borrowed buffer。
- command_task：已支持 owned argv/deep-copy argv 能力；当前同步路径仍使用 borrowed argv。
- 未引入内容：未引入 command queue、response queue、eventfd、executor、异步 response。
- 行为保持：锁模型、执行模型、response 格式、AOF/replication 后置语义均不变。
- 后续阶段 4B：接入 reactor-only command queue + single executor + response queue/eventfd 回投。

## 1. 设计目标

本阶段只做设计和代码审计，不修改源码。目标是在进入阶段 4：reactor-only single command executor 前，明确当前 `net_session_t`、fd、`recv_buf`、`send_buf`、`command_task`、`response_task` 的真实生命周期，并给出第一版跨线程 executor 必须补齐的安全边界。

核心结论：

阶段 4 不能直接把当前 command_task 丢到 executor。

原因：

1. 当前普通命令的 `argv` 指向 `recv_buf` 内部解析结果。
2. 当前 `response_task` 借用 `send_buf` 尾部 response buffer。
3. 当前 session 没有 `refcount`、`closing`、`generation`。
4. 当前 response 没有 owner buffer。
5. 当前没有 response 回投 owner poller/reactor 的机制。

阶段 4 之前必须完成：

1. command_task 跨线程模式下 `argv` deep copy。
2. response_task 跨线程模式下 own buffer。
3. session refcount。
4. session closing。
5. session generation 或等价身份校验。
6. owner reactor/poller 标识。
7. 每 session 单 in-flight 或 seq reorder。
8. response queue + eventfd wakeup。
9. shutdown drain 策略。

## 2. 当前 session 生命周期审计

当前 `net_session_t` 只有 `recv_buf`、`send_buf`、`resp_parser`、`processing`、`first_packet_checked`。它不是独立 owner 对象，也没有跨线程生命周期字段。

| network_mode | session owner | 创建位置 | 释放位置 | fd owner | 当前风险 |
|---|---|---|---|---|---|
| reactor | `conn_extensions[fd]` 静态数组元素，和 fd 下标强绑定 | `event_register(fd)` -> `init_conn_extension(fd)` -> `net_session_init(&ext->session, 8MB, 8MB)` | `event_unregister(fd)` -> `free_conn_extension(fd)` -> `net_session_free(&ext->session)`，随后 `close(conn_list[fd].fd)` | reactor event loop / `conn_list[fd]` | fd 复用会覆盖同一下标 session；没有 generation/refcount，异步 response 可能误投递到新连接或访问已释放 buffer |
| proactor | 每连接 heap `conn_extension_t` | accept CQE 中 `kvs_malloc(sizeof(conn_extension_t))` -> `p_init_conn_extension()` -> `net_session_init(1MB, 1MB)` | read/write CQE 出错或 EOF 时 `p_free_conn_extension(ctx)` -> `net_session_free` -> `close(fd)` -> `kvs_free(ctx)` | io_uring op 通过 `io_uring_op_t.ctx` 持有连接指针 | SQE/CQE 持有裸 `ctx` 指针；如果引入 executor 或更多 outstanding op，缺少 refcount/closing 容易 UAF |
| ntyco | 每连接协程持有 heap `ntyco_conn_ctx_t` | accept 协程中 malloc ctx -> `ntyco_init_conn_ctx()` -> `net_session_init(8MB, 8MB)` -> 创建连接协程 | 连接协程退出 `ntyco_free_conn_ctx()` -> `net_session_free` -> `nty_close(fd)` -> `kvs_free(ctx)` | 连接协程 | 生命周期绑定协程栈和循环；不适合阶段 4 直接接入跨线程 executor，需另做协程安全回投设计 |

### 2.1 reactor

reactor 的连接上下文是静态数组：

- `conn_list[fd]` 保存 fd 和回调。
- `conn_extensions[fd]` 保存 `net_session_t`、`send_offset`、iovecs、replication 标记。
- `event_register(fd, EPOLLIN)` 初始化 `conn_list[fd]` 和 `conn_extensions[fd]`。
- `event_unregister(fd)` 先释放 session buffer，再关闭 fd，并将 `conn_list[fd].fd = -1`。

客户端主动断开、读错误、写错误、协议处理错误都会走 `event_unregister(fd)`。如果 `send_buf` 还有未发送数据，当前同步模型下直接释放并关闭连接，不做 drain。

当前没有异步 response 时这可以成立，因为所有命令执行和 response append 都发生在 `recv_cb()` 当前调用栈内。`event_unregister()` 发生时，不存在外部线程持有 session 或 `send_buf` 尾部指针。

引入 executor 后，fd 下标复用成为主要风险：旧连接 fd=10 关闭并释放 `conn_extensions[10]` 后，新连接也可能获得 fd=10 并重新初始化同一数组元素。旧 task 的 response 如果只按 fd 回投，就可能写入新连接。

### 2.2 proactor

proactor 的连接上下文是每连接 heap 对象。io_uring 的 user data 是 `io_uring_op_t *`，其中 `ctx` 指向连接对象：

- accept CQE 创建连接对象并投递 READ。
- READ CQE 追加到 `recv_buf`，同步调用 `net_session_process_resp()`，然后继续投递 read/write。
- WRITE CQE 更新 `send_offset`，发送完后清空 `send_buf.size`。
- read/write 返回 `n <= 0` 时直接 `p_free_conn_extension(ctx)`。

当前风险是 io_uring op 和连接对象之间没有引用计数。阶段 4 第一版不支持 proactor，否则 executor、read SQE、write SQE、response 回投都可能同时持有 `ctx`。

### 2.3 ntyco

ntyco 中每个连接由一个连接协程持有：

- accept 协程创建 `ntyco_conn_ctx_t`。
- 连接协程使用 `poll()` 等待读写事件。
- recv 后同步调用 `net_session_process_resp()`。
- send 完后清空 `send_buf.size`。
- 协程退出时释放 session、关闭 fd、释放 ctx。

阶段 4 不应直接支持 ntyco。它需要协程调度器感知 response 回投和 session 引用，而这不是 reactor-only executor 的最小路径。

## 3. 当前 response 写回链路

当前链路：

```text
net_session_process_resp
  -> 在 send_buf 尾部预留 response buffer
  -> kvs_filter_protocol(..., response)
  -> command_task / response_task 同步写 response
  -> 返回 response length
  -> send_buf.size 增加
  -> reactor/proactor/ntyco 发送 send_buf
```

关键事实：

1. response buffer owner 是 `net_session.send_buf`。
2. `response_task` 只是 borrowed wrapper，`owns_buf=0`，destroy 不释放 buffer。
3. `net_session_process_resp()` 每解析一条完整命令，就在 `send_buf.data + send_buf.size` 处预留 `KVS_MAX_RESPONSE_LENGTH`。
4. 如果剩余容量不足，会先 `netbuf_reserve(&session->send_buf, needed)`，这可能搬迁 `send_buf.data`。
5. handler 同步写入 response 后，`send_buf.size += resp_len`。
6. pipeline 多命令时，多个 response 按解析顺序连续排列在同一个 `send_buf` 中。
7. reactor/proactor/ntyco 发送完成后将 `send_offset=0`、`send_buf.size=0`。

当前同步模式下，`response_task` 不需要独立 buffer，因为 borrowed 指针只在 `kvs_filter_protocol()` 当前调用栈内有效，且 `net_session_process_resp()` 在 handler 返回前不会释放 session。

异步模式下不能继续借用 `send_buf` 尾部指针，原因是：

- command_task 进入 executor 后，reactor 线程可能继续读、继续 parse、扩容或关闭 session。
- `send_buf` 可能因为后续 response reserve 而 `realloc`。
- 客户端断开时 `net_session_free()` 会释放 `send_buf.data`。
- fd 复用后相同 fd 对应的 `send_buf` 可能属于新连接。

## 4. 当前同步模型为什么安全

当前安全性来自调用栈不逃逸：

1. `recv_buf` 中的 RESP token 只在 `net_session_process_resp()` -> handler -> `kvs_filter_protocol()` 同步路径内使用。
2. `kvs_command_task_t` 是 `kvs_filter_protocol()` 栈对象，普通命令 `owns_argv=0`。
3. VSET/VSEARCH prepared 数据虽然会深拷贝输入，但仍在当前同步生命周期内 prepare/execute/destroy。
4. `kvs_response_task_t` 是 `kvs_filter_protocol()` 栈对象，只包装当前传入的 response 指针。
5. `send_buf` 由网络 session 管理，handler 返回长度后才推进 `send_buf.size`。
6. 连接关闭时没有后台 task 仍持有 session、argv 或 response 指针。

因此当前同步模型不需要 session refcount、closing、generation，也不会出现旧 response 回投到新 fd 的问题。

## 5. 引入 executor 后新增风险

引入 executor 后，命令执行线程和 reactor 线程分离，当前隐含前提都会失效：

1. `argv` 风险：普通命令 token 指向 `recv_buf`，reactor 可能 consume、memmove、reserve 或释放 `recv_buf`。
2. response 风险：当前 response_task 借用 `send_buf` 尾部指针，executor 返回前该指针可能失效。
3. session 风险：客户端断开后 session 可能被释放，但 executor 仍持有 task。
4. fd 复用风险：旧 response 只按 fd 回投会写入新连接。
5. poller 归属风险：response 必须回到 owner reactor 线程追加 `send_buf` 和修改 epoll 事件。
6. pipeline 保序风险：同连接多条命令并发执行会导致 response 乱序。
7. shutdown 风险：服务端退出时需要停止接收新命令，并 drain command/response，否则可能释放 store 后仍执行命令。

## 6. 阶段 4 reactor-only 最小安全模型

第一版只支持 reactor，不支持 proactor/ntyco。

建议模型：

```text
reactor thread:
  recv + parse
  build command_task
  deep copy argv for cross-thread task
  allocate owned response_task buffer or reserve owned response container
  session_ref_inc
  mark session single_inflight
  push command_task to executor queue
  disable EPOLLIN or stop parsing more commands for this session

executor thread:
  execute command_task
  build/finalize owned response_task
  push response_task to owner reactor response queue
  wake owner reactor by eventfd

reactor thread:
  on eventfd wakeup
  pop response_task
  if session closing or generation mismatch:
      drop response
      free owned response buffer
      session_ref_dec
  else:
      append response buffer to session.send_buf
      clear single_inflight
      enable EPOLLOUT
      restore EPOLLIN if needed
      free owned response buffer
      session_ref_dec
```

第一版限制：

1. 只支持 reactor。
2. 不支持 proactor。
3. 不支持 ntyco。
4. 每 session 只允许一个 in-flight command。
5. 不做 pipeline 并行。
6. 不做 response reorder。
7. 不做 dynamic response buffer；可先沿用 `KVS_MAX_RESPONSE_LENGTH` 上限分配 owned buffer，后续再优化。
8. command_task 进入 executor 后 `argv` 必须 deep copy。
9. response_task 进入 queue 后必须 own buffer，不能 borrowed `send_buf`。
10. store 访问第一版仍保留 `g_kv_data_rwlock`。

## 7. session 字段设计

建议第一版最小字段：

```c
typedef struct net_session {
    ...
    uint64_t session_id;
    uint32_t generation;
    int closing;
    int refcount;
    int owner_poller_id;
    uint64_t next_request_seq;
    uint64_t next_response_seq;
    int single_inflight;
} net_session_t;
```

字段取舍：

| 字段 | 第一版是否需要 | 作用 | 不加的风险 | 延后条件 |
|---|---|---|---|---|
| `session_id` | 需要 | 给每个连接分配稳定身份，response 回投时校验目标 session | fd 复用后旧 response 可能写入新连接 | 若使用不可复用 heap session 指针且 refcount 完整，也仍建议保留可观测 id |
| `generation` | 需要 | 配合 fd 下标数组识别同一 fd 槽位的新旧连接 | reactor 的 `conn_extensions[fd]` 被新连接覆盖后无法区分 | 如果 reactor 改为 heap session 且 response 持有强引用，可弱化但不建议第一版延后 |
| `closing` | 需要 | 标记连接已断开或正在关闭，response 回投时丢弃 | 断开后仍 append response 或重新启用 EPOLLOUT | 无异步 response 时可不需要；进入 executor 前必须有 |
| `refcount` | 需要 | command_task/response_task 跨线程持有 session 生命周期 | executor 执行期间 reactor 释放 session 导致 UAF | 若阶段 4 不跨线程则可延后，但阶段 4 目标就是 executor |
| `owner_poller_id` | 需要 | response 回投到创建 session 的 reactor；reactor-only 可先固定为 0 | 后续多 poller 时可能跨线程直接操作错误 epoll/session | 如果阶段 4 确认单 reactor，可实现为常量字段，但接口应预留 |
| `next_request_seq` | 需要 | 给同连接命令分配序号，便于单 in-flight 校验和日志定位 | pipeline 或重试路径无法识别 response 所属请求 | 严格单 in-flight 且无调试需求时可延后，但建议第一版加入 |
| `next_response_seq` | 可延后 | 多 in-flight 或 response reorder 时按序发送 | 多并发 pipeline response 乱序 | 第一版明确每 session 单 in-flight，不做 reorder |
| `single_inflight` | 需要 | 第一版限制每 session 只有一个 executor task | 同连接 pipeline 多命令并发执行后 response 乱序 | 实现完整 seq reorder 后可取消 |

## 8. command_task 跨线程前置要求

当前 command_task 仍是同步栈对象，不能直接进入 executor。

跨线程前必须调整：

1. task 必须 heap 分配或从 task pool 分配，生命周期由 queue/executor/response 回投明确管理。
2. 普通命令 `argv` 和 `argv_lens` 必须 deep copy，不能引用 `resp_parser.tokens` 或 `recv_buf`。
3. `command_context_t` 中所有指向 argv/value/scope/query 的字段必须指向 task-owned 内存。
4. VSET/VSEARCH prepared 数据可继续沿用 deep copy 语义，但 owner 必须归 task 或 record，destroy 路径必须单一。
5. replication context、是否复制连接、sync_context 不能依赖 reactor 线程的临时状态，必须随 task 传入 executor。
6. AOF/replication post_commit 仍必须只在 command commit 成功后执行，不能在 enqueue/prepare 阶段执行。
7. task destroy 必须覆盖 build 失败、enqueue 失败、execute 失败、response drop、shutdown drain 等路径。

## 9. response_task 跨线程前置要求

当前 response_task 只适合同步 borrowed buffer。跨线程前必须调整：

1. response_task 必须拥有 response buffer，`owns_buf=1`。
2. response buffer 不能指向 `net_session.send_buf`。
3. response_task 必须记录 session identity：至少包含 session pointer/ref、`session_id`、`generation`、request seq。
4. response_task 必须可进入 response queue，并由 owner reactor 线程消费。
5. response_task destroy 必须释放 owned buffer。
6. response append 到 `send_buf` 必须发生在 owner reactor 线程。
7. response 被丢弃时也必须释放 buffer 并 `session_ref_dec`。

第一版可以先固定 response buffer 上限为 `KVS_MAX_RESPONSE_LENGTH`，避免在阶段 4 同时引入动态扩容策略。

## 10. 断连与 fd 复用处理

### 场景 A：客户端发命令后立即断开

流程：

1. reactor 已 deep copy command_task 并投递 executor。
2. 客户端断开，reactor 标记 session `closing=1`，关闭或注销 fd。
3. 如果 `refcount > 0`，session 资源不能立即释放到可复用状态。
4. executor 执行完成后回投 response。
5. owner reactor 发现 `closing=1`，丢弃 response，释放 owned buffer，`session_ref_dec`。
6. 最后一个 ref 释放时销毁 session。

### 场景 B：fd 被系统复用

流程：

1. 旧 session fd=10 断开。
2. 新连接也拿到 fd=10。
3. 旧 response 回投。
4. reactor 必须校验 `session_id/generation`，不匹配则禁止写入新连接的 `send_buf`。
5. 旧 response 只允许 drop + free + ref_dec。

### 场景 C：pipeline 多条命令

第一版策略：

1. 不做多 in-flight。
2. 单 session 正在处理一个 command 时，不继续从该 session parse 新命令，或 parse 后不 enqueue。
3. 可临时暂停 EPOLLIN，让内核 socket buffer 或 `recv_buf` 保留后续数据。
4. response 回来并 append 后，再恢复 EPOLLIN/继续 parse。

这样避免 response reorder 和 batch 调度复杂度。

### 场景 D：executor 还在执行，服务端 shutdown

正确策略：

1. stop accept。
2. stop read new command。
3. 标记所有 session closing。
4. drain command queue：已开始执行的 task 允许完成，未开始的 task 可按策略取消并生成/drop response。
5. drain response queue：回到 owner reactor 后 append 或 drop。
6. 等待 session refcount 归零后关闭 sessions。
7. flush AOF/replication 必要状态。
8. destroy store。

## 11. pipeline 与单连接保序策略

当前同步 pipeline 的顺序由 `net_session_process_resp()` 的 while loop 自然保证：解析一条、同步生成一条 response、append 到 `send_buf`，再解析下一条。

阶段 4 第一版不保留 pipeline 并行能力。建议使用 `single_inflight`：

- session 无 in-flight 时，允许 parse 一条完整命令并 enqueue。
- enqueue 成功后设置 `single_inflight=1`。
- `single_inflight=1` 时暂停继续 parse 该 session 的后续命令。
- response 回投并 append/drop 后清除 `single_inflight`。

后续若要支持 pipeline 并行，需要：

- request seq。
- response seq。
- per-session pending response map/list。
- 按 `next_response_seq` 顺序 append。
- 对缺口 response 处理超时/关闭策略。

## 12. shutdown/drain 策略

阶段 4 前需要定义 shutdown 状态机：

1. `STOP_ACCEPT`：关闭 listen fd 或从 epoll 移除 listen fd。
2. `STOP_READ`：所有 session 不再读取新命令。
3. `DRAIN_COMMANDS`：executor 处理已入队 task，或取消未执行 task。
4. `DRAIN_RESPONSES`：reactor 消费 response queue，能发送则发送，closing 则丢弃。
5. `CLOSE_SESSIONS`：等待 refcount 归零后释放 session。
6. `FLUSH_STORAGE`：完成 AOF flush / replication shutdown / store destroy。

不能在 command queue 或 response queue 仍可能访问 store/session 时先释放 store 或 session。

## 13. 不支持范围

阶段 4 第一版不支持：

1. proactor executor 化。
2. ntyco executor 化。
3. 多 poller。
4. 多 executor。
5. embedding worker pool。
6. response queue 跨多个 owner poller 的复杂路由。
7. pipeline 多 in-flight。
8. response reorder。
9. dynamic response buffer。
10. 删除或弱化 `g_kv_data_rwlock`。
11. 修改 RESP 协议。
12. 修改 RDB/AOF 文件格式。
13. 修改主从协议。

## 14. 阶段 4 实现前检查清单

- [ ] reactor session 增加 `session_id/generation/closing/refcount/owner_poller_id/single_inflight` 或等价字段。
- [ ] session ref inc/dec 规则明确，并覆盖断连、response drop、shutdown。
- [ ] command_task 支持跨线程 heap 生命周期。
- [ ] command_task 跨线程路径 deep copy `argv/argv_lens` 和 context 指针字段。
- [ ] response_task 支持 owned buffer，destroy 释放 owned buffer。
- [ ] response_task 记录 session identity 和 request seq。
- [ ] response queue 只能由 owner reactor 消费。
- [ ] eventfd wakeup 机制明确。
- [ ] fd 复用通过 generation/session_id 校验。
- [ ] 每 session 单 in-flight 策略落地。
- [ ] shutdown drain 顺序落地。
- [ ] proactor/ntyco 明确保持同步旧路径或禁用 executor。

## 15. 建议实现顺序

1. 先只在 reactor 模式增加 session identity、closing、refcount、owner_poller_id、single_inflight 字段和初始化/释放规则。
2. 增加 response_task owned buffer 能力，但保持同步路径仍可使用 borrowed buffer。
3. 增加 command_task 跨线程 build/destroy 路径，普通 argv deep copy。
4. 增加单 reactor response queue + eventfd wakeup，只做回投 append 到 `send_buf`。
5. 增加 single command executor，先串行执行，保留 `g_kv_data_rwlock`。
6. 处理断连 drop response、fd generation mismatch、shutdown drain。
7. 跑普通 KV、TTL、VSET/VSEARCH/VDEL、scope、pipeline 单 in-flight、断连、fd 复用压力验证。
8. 阶段 4 稳定后，再评估多 poller、pipeline reorder、dynamic response buffer、worker pool。

## 实现状态：阶段 4B reactor-only single command executor MVP

- 当前状态：已接入 reactor-only single command executor。
- 支持范围：reactor 模式、single executor、每 session 单 in-flight。
- command_task：async 路径使用 heap task + owned argv。
- response_task：async 路径使用 owned response buffer。
- response 回投：executor 通过 response queue + eventfd 唤醒 reactor，由 owner reactor append 到 send_buf。
- session 安全：使用 session_id/generation/closing 校验；reactor fd 槽位保存当前 heap 连接 owner，旧 response 必须同时匹配 fd 槽位 owner、session_id 和 generation，断连或 fd 复用时 drop response。
- 生命周期策略：async task 入队前 session ref_inc；response append/drop 或 queue drain 后 release；closing session 在 refcount 归零后释放。
- 保持不变：g_kv_data_rwlock、命令锁模型、RESP 格式、RDB/AOF 格式、主从协议。
- 同步 fallback：REPLSYNC 和已标记 replication connection 继续走 reactor 同步旧路径。
- 不支持范围：proactor/ntyco、多 poller、多 executor、pipeline 多 in-flight、response reorder、embedding worker pool。
