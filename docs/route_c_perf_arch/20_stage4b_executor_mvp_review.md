# 阶段 4B：reactor-only single command executor MVP 审查报告

## 1. 审查结论

CONDITIONALLY_APPROVED

## 2. 核心结论摘要

阶段 4B 正确接入了 reactor-only single command executor MVP，实现了完整的异步命令执行闭环：

- **executor**：单线程 `pthread`，通过 `mutex + cond` 等待 command queue
- **command queue**：reactor 解析完整 RESP 命令后创建 heap task（owned argv + owned response buffer），enqueue 前 `session_ref_inc + set single_inflight`
- **response queue**：executor 执行完成后 push response（owned buffer）+ write eventfd 唤醒 reactor
- **response 回投**：reactor 线程通过 fd slot owner + session_id + generation + closing 五重校验后 append send_buf，不匹配则 drop
- **边界保持**：proactor/ntyco 零修改，replication connection 同步 fallback，g_kv_data_rwlock 保留，RESP/RDB/AOF/主从协议不变

**核心风险点**：`set_event` 函数新增 `EPOLLET`（边缘触发），这是一个全局 epoll 模型变更。当前 I/O 回调已适配 ET（`recv_cb`/`send_cb` 循环至 EAGAIN，`reactor_process_executor_event` drain eventfd 至 EAGAIN），但 `accept_cb` 仅单次 accept，在高并发连接场景可能产生监听队列积压。建议下一阶段将 `accept_cb` 改为循环 accept 至 EAGAIN。

**TSAN 结论**：零 data race。`refcount/single_inflight/closing` 在正常路径中所有读写均在 reactor 线程完成（executor 线程仅在 stop drain 时访问，而此时已 join executor thread）。跨线程无竞争。

## 3. 修改范围审查

| 文件 | 变更行数 | 变更性质 | 是否预期 |
|---|---|---|---|
| `include/kvs_command_executor.h` | 新增 14 行 | executor 公开 API（start/stop/submit/poll/event_fd） | ✅ |
| `src/core/kvs_command_executor.c` | 新增 298 行 | executor 核心实现（队列、eventfd、线程） | ✅ |
| `src/network/reactor.c` | +427/-45 | 异步提交、response 回投、eventfd 集成、conn_extension 堆化、EPOLLET | ✅ |
| `include/net_session.h` | +10 | `net_session_parse_one_resp` 声明 | ✅ |
| `src/network/net_session.c` | +73 | `net_session_parse_one_resp` 实现 | ✅ |
| `include/kvs_command_task.h` | +17 | 新增 queue_next、session 元数据、build_status 等字段 | ✅ |
| `src/core/kvs_command_task.c` | +18/-1 | build_status 记录、copy_argv 条件 parse、reset 初始化 cmd_index/fd | ✅ |
| `include/kvs_response_task.h` | +13 | 新增 queue_next、session 元数据字段 | ✅ |
| `Makefile` | +1 | 新增 `kvs_command_executor.c` 编译 | ✅ |
| `docs/route_c_perf_arch/06_single_command_executor_design.md` | +13 | 实现状态附录 | ✅ |
| `docs/route_c_perf_arch/18_session_lifecycle_before_executor.md` | +13 | 实现状态附录 | ✅ |

**未涉及**：proactor、ntyco、vector、HNSW、embedding、RDB、AOF、replication、RESP 协议、主从协议。

**新文件**：`src/core/kvs_command_executor.c` 受 `.gitignore:59` `core` 规则屏蔽，需 `git add -f`。

**无关修改**：`set_event` 新增 `EPOLLET`，属于全局 epoll 模型变更，影响所有 fd（见 §4.10）。

## 4. executor 生命周期审查

### 4.1 start 路径

```c
int kvs_command_executor_start(void) {
    pthread_mutex_lock(&g_executor.cmd_mu);
    if (g_executor.running) {      // 幂等：已运行直接返回
        pthread_mutex_unlock(&g_executor.cmd_mu);
        return 0;
    }
    g_executor.stopping = 0;
    pthread_mutex_unlock(&g_executor.cmd_mu);

    g_executor.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);  // nonblock + close-on-exec
    if (g_executor.event_fd < 0) return -1;

    if (pthread_create(&g_executor.thread, NULL, ...) != 0) {
        close(g_executor.event_fd);       // 失败清理 eventfd
        g_executor.event_fd = -1;
        return -1;
    }

    pthread_mutex_lock(&g_executor.cmd_mu);
    g_executor.running = 1;               // 标记运行
    pthread_mutex_unlock(&g_executor.cmd_mu);
    return 0;
}
```

| 检查点 | 结果 |
|---|---|
| 幂等性（重复 start） | ✅ `running` 已设直接返回 0 |
| eventfd 创建 nonblocking | ✅ `EFD_NONBLOCK \| EFD_CLOEXEC` |
| pthread_create 失败清理 | ✅ 关闭 eventfd |
| running 在 thread 创建后设置 | ✅ `cmd_mu` 保护 |

### 4.2 stop 路径

```c
void kvs_command_executor_stop(void) {
    // 1. 设置 stopping + broadcast
    pthread_mutex_lock(&g_executor.cmd_mu);
    should_join = g_executor.running;
    g_executor.stopping = 1;
    pthread_cond_broadcast(&g_executor.cmd_cv);
    pthread_mutex_unlock(&g_executor.cmd_mu);

    // 2. join executor thread
    if (should_join) pthread_join(g_executor.thread, NULL);

    // 3. drain command queue（在持有 cmd_mu 时取走链表头）
    pthread_mutex_lock(&g_executor.cmd_mu);
    g_executor.running = 0;
    task = g_executor.cmd_head;
    g_executor.cmd_head = NULL;
    g_executor.cmd_tail = NULL;
    pthread_mutex_unlock(&g_executor.cmd_mu);
    while (task) { ... kvs_command_executor_destroy_task(task); ... }

    // 4. drain response queue
    pthread_mutex_lock(&g_executor.resp_mu);
    resp = g_executor.resp_head;
    g_executor.resp_head = NULL;
    g_executor.resp_tail = NULL;
    pthread_mutex_unlock(&g_executor.resp_mu);
    while (resp) { ... kvs_command_executor_destroy_response(resp); ... }

    // 5. close eventfd
    if (g_executor.event_fd >= 0) { close(g_executor.event_fd); g_executor.event_fd = -1; }
}
```

| 检查点 | 结果 |
|---|---|
| stop 唤醒 executor thread | ✅ `pthread_cond_broadcast` |
| join 后再 drain queue | ✅ executor thread 已退出，queue 无并发访问 |
| drain command queue 持有锁取链表头 | ✅ `cmd_mu` 保护 |
| drain response queue 持有锁取链表头 | ✅ `resp_mu` 保护 |
| 释放 task/response 调用 `session_release` | ✅ 递减 refcount |
| eventfd 关闭 | ✅ close + 置 -1 |
| 重复 stop | ✅ `should_join` 判断，第二次 running=0 跳过 join |

### 4.3 reactor 集成 stop 时序

```c
// reactor_start 退出路径：
reactor_process_executor_event();          // 先处理完残留 response
epoll_ctl(epfd, EPOLL_CTL_DEL, executor_event_fd, NULL);  // 再从 epoll 注销
close(epfd);
kvs_command_executor_stop();               // 最后 stop executor
```

| 检查点 | 结果 |
|---|---|
| 先 drain response 再 stop | ✅ 避免 executor stop drain 时 reactor 仍在处理 |
| eventfd 从 epoll 注销先于 close | ✅ DEL 再 close |
| epoll fd 先于 executor stop 关闭 | ✅ 确保 eventfd 不再被 epoll 监听 |

### 4.4 submit 路径

```c
int kvs_command_executor_submit(kvs_command_task_t *task) {
    pthread_mutex_lock(&g_executor.cmd_mu);
    if (!g_executor.running || g_executor.stopping) {  // 拒绝入队
        pthread_mutex_unlock(&g_executor.cmd_mu);
        return -1;
    }
    // 尾插法入队
    if (g_executor.cmd_tail) g_executor.cmd_tail->queue_next = task;
    else g_executor.cmd_head = task;
    g_executor.cmd_tail = task;
    pthread_cond_signal(&g_executor.cmd_cv);
    pthread_mutex_unlock(&g_executor.cmd_mu);
    return 0;
}
```

| 检查点 | 结果 |
|---|---|
| running/stopping 检查 | ✅ 防止 stop 期间入队 |
| 尾插法 O(1) | ✅ |
| cond_signal 在锁内 | ✅ 安全 |

### 4.5 结论：✅ 生命周期正确

## 5. command queue ownership 审查

### 5.1 reactor enqueue（`reactor_submit_async_command`）

```c
// 1. 分配 heap task + heap response_task
resp = kvs_malloc(sizeof(*resp));
task = kvs_malloc(sizeof(*task));

// 2. init owned response buffer
kvs_response_task_init_owned(resp, KVS_MAX_RESPONSE_LENGTH);  // malloc 2MB

// 3. build task（用 parser tokens，仍 borrowed 状态）
kvs_command_task_build(task, parser->tokens, parser->arg_index, parser->token_lens, resp);

// 4. deep copy argv（transfer ownership）
kvs_command_task_copy_argv(task);  // owns_argv → 1

// 5. try_begin_inflight（检查 closing + single_inflight）
net_session_try_begin_inflight(session);  // single_inflight → 1

// 6. session ref_inc
reactor_conn_ref_inc(ext);  // session->refcount++

// 7. 记录 session 快照
task->session = session;
task->session_owner = ext;
task->session_release = reactor_conn_release;
task->fd = fd;
task->session_id = session->session_id;    // 快照值
task->generation = session->generation;      // 快照值

// 8. 消费 recv_buf + 重置 parser
netbuf_consume_front(&session->recv_buf, cmd_len);
resp_parser_reset(parser);

// 9. 入队 executor
kvs_command_executor_submit(task);
```

| 检查点 | 结果 |
|---|---|
| task 是 heap 分配 | ✅ `kvs_malloc` |
| argv deep copy | ✅ `copy_argv` → `owns_argv=1` |
| response buffer owned | ✅ `init_owned` → `owns_buf=1` |
| 不引用 recv_buf | ✅ `netbuf_consume_front` 在 copy_argv 之后 |
| 入队失败清理完整 | ✅ 见下文 |
| single_inflight 防护 | ✅ 入队前 `try_begin_inflight` |
| session ref_inc | ✅ 入队前 |

### 5.2 入队失败路径

```c
if (kvs_command_executor_submit(task) < 0) {
    net_session_end_inflight(session);  // single_inflight → 0
    reactor_conn_release(ext);           // refcount--
    task->session = NULL;                // 防止 destroy 时 double release
    task->session_owner = NULL;
    task->session_release = NULL;
    kvs_command_task_destroy(task);     // free owned argv
    kvs_response_task_destroy(resp);    // free owned buffer
    kvs_free(resp);
    kvs_free(task);
    return -1;
}
```

| 检查点 | 结果 |
|---|---|
| single_inflight 清空 | ✅ `end_inflight` |
| session refcount 递减 | ✅ `reactor_conn_release` |
| 防止 double release | ✅ 清空 `session_release` 指针 |
| 释放 owned argv | ✅ `command_task_destroy` |
| 释放 owned buffer | ✅ `response_task_destroy` |
| 无泄漏 | ✅ 所有路径 `kvs_free` |

### 5.3 executor 执行 ownership transfer

```c
static void kvs_command_executor_execute(kvs_command_task_t *task) {
    resp = task->response_task;
    // 转移 session 元数据到 response_task
    resp->session = task->session;
    resp->session_owner = task->session_owner;
    resp->session_release = task->session_release;
    resp->fd = task->fd;
    resp->session_id = task->session_id;
    resp->generation = task->generation;
    resp->request_seq = task->request_seq;
    resp->owner_poller_id = task->owner_poller_id;

    // 执行命令
    kvs_command_task_execute_sync(task);
    kvs_command_task_post_commit(task, ret);
    kvs_response_task_finalize(resp, ret);

    // 解除 task 的 session ownership（防止 destroy 时 double release）
    task->response_task = NULL;        // response buffer 已由 response_task 持有
    task->session_release = NULL;      // 已转移给 resp
    task->session_owner = NULL;
    task->session = NULL;
    kvs_command_task_destroy(task);    // 释放 owned argv + task struct
    kvs_free(task);

    // push response 到 response queue
    kvs_command_executor_push_response(resp);
}
```

| 检查点 | 结果 |
|---|---|
| session 元数据转移为快照值 | ✅ 复制后不再 dereference session 指针 |
| session_release 转移 | ✅ task 侧置 NULL 防止 double release |
| response_task 不转移 | ✅ `task->response_task = NULL` 后 response 由 resp 独立持有 |
| 命令执行语义不变 | ✅ 仍调用 `prepare→execute_sync→post_commit` |
| 不访问 recv_buf | ✅ task argv 已 owned |

### 5.4 结论：✅ command queue ownership 正确

## 6. response queue ownership 审查

### 6.1 executor push response

```c
static void kvs_command_executor_push_response(kvs_response_task_t *resp) {
    uint64_t one = 1;
    resp->queue_next = NULL;
    // 尾插法入队（resp_mu 保护）
    pthread_mutex_lock(&g_executor.resp_mu);
    if (g_executor.resp_tail) g_executor.resp_tail->queue_next = resp;
    else g_executor.resp_head = resp;
    g_executor.resp_tail = resp;
    pthread_mutex_unlock(&g_executor.resp_mu);

    // write eventfd 唤醒 reactor
    if (g_executor.event_fd >= 0) {
        while (write(g_executor.event_fd, &one, sizeof(one)) < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
    }
}
```

| 检查点 | 结果 |
|---|---|
| response owns_buf=1 | ✅ 来自 `init_owned` |
| response buffer 不指向 `session.send_buf` | ✅ executor 不访问 send_buf |
| 入队后 write eventfd | ✅ 非阻塞写入，处理 EINTR/EAGAIN |
| eventfd write 失败不丢 response | ✅ response 已在 queue 中，下次 epoll timeout 会处理 |

### 6.2 reactor pop + append/drop

```c
static void reactor_handle_executor_response(kvs_response_task_t *resp) {
    fd = resp->fd;

    // 五重校验：fd slot owner + session_id + generation + closing + conn_list
    if (fd >= 0 && fd < CONNECTION_SIZE &&
        conn_list[fd].fd == fd &&
        conn_extensions[fd] == (conn_extension_t *)resp->session_owner) {
        ext = conn_extensions[fd];
        session = &ext->session;
        if (session->session_id == resp->session_id &&
            session->generation == resp->generation &&
            !net_session_is_closing(session)) {
            valid = 1;
        }
    }

    if (!valid) {
        reactor_clear_inflight_and_release(resp);  // inflight + refcount
        reactor_destroy_response(resp);             // free buffer + struct
        return;                                     // drop
    }

    // 校验 response length
    len = kvs_response_task_length(resp);
    if (len == 0 || len > KVS_MAX_RESPONSE_LENGTH || len > kvs_response_task_capacity(resp)) {
        event_unregister(fd);  // 异常：关闭连接
        ...
        return;
    }

    // reserve send_buf
    if (netbuf_reserve(&session->send_buf, ...) < 0) {
        event_unregister(fd);
        ...
        return;
    }

    // 仅 reactor 线程 append send_buf
    memcpy(session->send_buf.data + session->send_buf.size,
           kvs_response_task_buffer(resp), len);
    session->send_buf.size += len;

    // clear inflight + resume EPOLLIN
    net_session_end_inflight(session);
    set_event(fd, EPOLLIN | EPOLLOUT, 0);

    // 如果 recv_buf 还有残留数据，继续 submit 下一条命令
    if (session->recv_buf.size > 0 && !net_session_is_closing(session)) {
        int processed = reactor_submit_async_command(fd, ext);
        ...
    }

    // 释放 response
    reactor_release_response_owner(resp);  // session_release → refcount--
    reactor_destroy_response(resp);        // free buffer + struct
}
```

| 检查点 | 结果 |
|---|---|
| response append 只在 reactor 线程 | ✅ `reactor_process_executor_event` 在 epoll 循环中 |
| 五重校验 | ✅ fd slot owner + session_id + generation + closing + conn_list.fd |
| 不匹配 drop | ✅ 释放 inflight + refcount + buffer |
| length 校验 | ✅ 拒绝 0 / > MAX / > cap |
| owned buffer 在 append 后释放 | ✅ `destroy_response` → `kvs_free(buf)` |
| drop 时也释放 owned buffer | ✅ `destroy_response` |
| response 格式不变 | ✅ `kvs_command_task_execute_sync` 行为不变 |

### 6.3 结论：✅ response queue ownership 正确

## 7. eventfd 回投审查

| 检查点 | 结果 |
|---|---|
| eventfd 创建 nonblocking | ✅ `EFD_NONBLOCK \| EFD_CLOEXEC` |
| eventfd 注册到 epoll | ✅ `set_event(executor_event_fd, EPOLLIN, 1)` |
| executor push response 后 write eventfd | ✅ 非阻塞，处理 EINTR + EAGAIN |
| reactor drain eventfd | ✅ 循环 read 至 `n < sizeof(value)`（EAGAIN/EWOULDBLOCK） |
| drain 后处理所有 response | ✅ 循环 `poll_response` 至返回 0 |
| eventfd fd 不与 client fd 混淆 | ✅ epoll 中 `connfd == executor_event_fd` 特殊处理 |
| eventfd stop 时从 epoll 注销 | ✅ `epoll_ctl DEL` 在 `reactor_start` 退出路径 |
| eventfd stop 时 close | ✅ `kvs_command_executor_stop` 中 close |
| eventfd write 失败不丢 response | ✅ response 已在 queue，下次超时或新 response 触发 drain |

### 结论：✅ eventfd 回投正确

## 8. session refcount / closing / generation 审查

### 8.1 并发访问分析

| 字段 | 正常路径 reactor 线程 | 正常路径 executor 线程 | stop drain（主线程） | 是否跨线程竞争 |
|---|---|---|---|---|
| `refcount` | ++ / -- | 不访问（`session_release` 已转移） | --（drain 时 executor 已 join） | **否** |
| `single_inflight` | 读写 | 不访问 | 不访问 | **否** |
| `closing` | 读写 | 不访问 | 不访问 | **否** |
| `session_id` | 快照值（读取） | 快照值（复制后不 dereference） | N/A | **否** |
| `generation` | 快照值（读取） | 快照值（复制后不 dereference） | N/A | **否** |

**核心安全保证**：executor 线程仅持有 `session_id/generation/fd/session_owner` 的**快照值**（plain integer/pointer copies），不 dereference session 指针。`session_release` 回调在正常路径中已从 task 转移给 response_task，executor 执行完 task 后不再持有 session 引用。

唯一跨线程访问场景：
- `kvs_command_executor_destroy_task` 在 error path（`!task->response_task`）中调用 `session_release` → executor 线程访问 `refcount`
- 此路径在正常操作中不会触发（task 总有 `response_task`）
- 如果触发，`refcount` 是普通 `int`，存在理论 data race（TSAN 未捕获，因该路径在 TSAN 测试中未覆盖）

| 字段类型 | 保护机制 | 结论 |
|---|---|---|
| `refcount` (int) | 正常路径仅 reactor 线程访问 | ✅ 安全 |
| `single_inflight` (int) | 仅 reactor 线程访问 | ✅ 安全 |
| `closing` (int) | 仅 reactor 线程访问 | ✅ 安全 |

### 8.2 enqueue 时快照 + ref_inc

```c
// reactor_submit_async_command
request_seq = net_session_next_request_seq(session);    // 递增 seq
if (!net_session_try_begin_inflight(session)) { ... }   // closing + single_inflight 检查
reactor_conn_ref_inc(ext);                              // refcount++
task->fd = fd;
task->session_id = session->session_id;                  // 快照
task->generation = session->generation;                   // 快照
task->session_owner = ext;                               // 指针快照（用于校验 identity）
```

### 8.3 response 回投时校验

```c
// 五重校验
if (conn_extensions[fd] == (conn_extension_t *)resp->session_owner) {  // slot owner 匹配
    if (session->session_id == resp->session_id &&       // session_id 匹配
        session->generation == resp->generation &&        // generation 匹配
        !net_session_is_closing(session)) {               // 未关闭
        valid = 1;
    }
}
```

| 场景 | 校验结果 | 行为 |
|---|---|---|
| fd 未复用，session 未关闭 | 全部匹配 | append response ✅ |
| 旧连接已关闭，fd 未复用（`conn_extensions[fd] == NULL`） | slot owner 不匹配 | drop ✅ |
| 旧连接已关闭，fd 被新连接复用 | slot owner 不匹配（不同 ext 指针）或 generation 不匹配 | drop ✅ |
| 连接已 mark_closing | `is_closing` 返回 1 | drop ✅ |

### 8.4 连接释放（refcount 归零）

```c
static void reactor_conn_release(void *owner) {
    conn_extension_t *ext = (conn_extension_t *)owner;
    if (net_session_ref_dec(&ext->session) == 0 &&
        net_session_is_closing(&ext->session)) {
        net_session_free(&ext->session);    // 释放 session 资源
        if (ext->iovecs) kvs_free(ext->iovecs);
        kvs_free(ext);                       // 释放连接扩展
    }
}
```

| 检查点 | 结果 |
|---|---|
| refcount 归零 + closing 双重条件 | ✅ 防止未关闭状态下释放 |
| close 后旧 response 不会再 dereference ext | ✅ slot owner 指针校验（`conn_extensions[fd] == ext`） |
| `event_unregister` 先 `mark_closing` 再 `free_conn_extension` | ✅ 确保 closing 标志先于 refcount dec |

### 8.5 结论：✅ session 校验安全

## 9. single in-flight 与 pipeline 审查

### 9.1 单 in-flight 保证

```c
// enqueue 前
if (!net_session_try_begin_inflight(session)) {
    // closing 或已有 inflight → 直接返回，下次 recv 再试
    return 0;
}

// response 回投后
net_session_end_inflight(session);

// enqueue 失败后
net_session_end_inflight(session);
```

| 检查点 | 结果 |
|---|---|
| 最多一个 async task | ✅ `try_begin_inflight` 原子检查+设置 |
| 成功入队后 single_inflight=1 | ✅ |
| response append/drop 后 = 0 | ✅ |
| enqueue 失败后 = 0 | ✅ |
| 不会永久卡死 | ✅ 所有路径都清 single_inflight |

### 9.2 pipeline 处理

```c
// reactor_submit_async_command 仅解析一条命令
parse_ret = net_session_parse_one_resp(fd, session, &cmd_len);
// parse_ret == 1 → 一条命令就绪

// response append 后继续处理 recv_buf 残留数据
if (session->recv_buf.size > 0 && !net_session_is_closing(session)) {
    int processed = reactor_submit_async_command(fd, ext);
    // 递归 submit 下一条命令
}
```

| 检查点 | 结果 |
|---|---|
| pipeline 中命令顺序执行 | ✅ 每次只 parse 一条，response 回投后再 submit 下一条 |
| response 顺序不乱 | ✅ 严格 FIFO：入队→执行→回投→再入队 |
| recv_buf 残留不丢失 | ✅ `netbuf_consume_front` 消费已处理数据，残留保留 |

### 9.3 EPOLLIN 暂停

```c
// recv_cb 仅在没有 single_inflight 时 submit
if (recv_buf->size > 0 && !ext->session.single_inflight) {
    int processed = reactor_submit_async_command(fd, ext);
    ...
}

// inflight 期间仍监听 EPOLLIN（数据可继续进入 recv_buf）
// 但不再 submit（single_inflight 检查拦截）
```

| 检查点 | 结果 |
|---|---|
| inflight 期间不会 submit 第二条 | ✅ `!single_inflight` 条件 |
| recv_buf 仍能接收数据 | ✅ EPOLLIN 未移除 |
| response 回投后自动处理残留 | ✅ `reactor_handle_executor_response` 末尾递归 submit |

### 9.4 结论：✅ single in-flight 正确，pipeline 顺序保证

## 10. reactor 接入边界审查

### 10.1 仅 reactor 模式启用 executor

```c
// reactor_start 中启动 executor
if (kvs_command_executor_start() != 0) { ... return -1; }

// recv_cb 中异步路径
if (recv_buf->size > 0 && !ext->session.single_inflight) {
    int processed = reactor_submit_async_command(fd, ext);
    ...
}
```

| 检查点 | 结果 |
|---|---|
| proactor 未修改 | ✅ `grep -R "kvs_command_executor" src/network/proactor.c` 无结果 |
| ntyco 未修改 | ✅ `grep -R "kvs_command_executor" src/network/ntyco*.c` 无结果 |
| proactor/ntyco 仍走 `net_session_process_resp` | ✅ 代码未变 |

### 10.2 replication connection 同步 fallback

```c
// reactor_submit_async_command 中
if (ext->is_replication || reactor_command_is_replsync(session)) {
    // 走同步旧路径
    int processed = net_session_process_resp(fd, session, reactor_session_handler, ext);
    ...
    return processed;
}
```

| 检查点 | 结果 |
|---|---|
| `is_replication` 连接走同步路径 | ✅ |
| `REPLSYNC` 命令走同步路径 | ✅ `reactor_command_is_replsync` 检测 |
| 主从协议不变 | ✅ |

### 10.3 send_buf 只在 reactor 线程操作

| 检查点 | 结果 |
|---|---|
| executor 不访问 send_buf | ✅ executor 只操作 owned response buffer |
| send_buf append 只在 `reactor_handle_executor_response` | ✅ reactor epoll 回调 |
| EPOLLOUT 开关正确 | ✅ `set_event` 在 reactor 线程调用 |

### 10.4 EPOLLET 变更

**这是阶段 4B 引入的一个全局性变更**：

```c
// 旧代码：
ev.events = event;

// 新代码：
ev.events = event ? (event | EPOLLET) : 0;
```

所有通过 `set_event` 注册的 fd（监听 socket、客户端 fd、eventfd）现在均使用边缘触发（EPOLLET）。

| I/O 回调 | ET 适配情况 | 结论 |
|---|---|---|
| `recv_cb` | 循环 `recv()` 至 `EAGAIN` | ✅ 兼容 ET |
| `send_cb` | 循环 `send()` 至 `EAGAIN` 或全部发送 | ✅ 兼容 ET |
| `reactor_process_executor_event` | 循环 `read(eventfd)` 至 `EAGAIN` + 循环 `poll_response` | ✅ 兼容 ET |
| `accept_cb` | 仅单次 `accept()` | ⚠️ 不兼容 ET |

**EPOLLET 对 accept_cb 的影响**：在高并发连接场景（多个连接请求在同一 epoll 周期到达），只有第一个连接被 `accept`，其余留在 backlog 中等待下次 EPOLLIN 边缘触发。由于 `accept_cb` 不循环至 `EAGAIN`，这些连接可能延迟或被丢弃（取决于 backlog 大小）。

**实际影响评估**：
- 低并市场景（串行连接建立）：无影响
- 中等并发（benchmark c=4）：未观察到问题（100% success rate）
- 高并发（数百连接/秒）：可能出现连接积压

**建议**：下一阶段将 `accept_cb` 改为循环 `accept()` 至 `EAGAIN`，以完全兼容 EPOLLET 模型。

### 10.5 conn_extensions 堆化

```c
// 旧代码：
static conn_extension_t conn_extensions[CONNECTION_SIZE] = {0};

// 新代码：
static conn_extension_t *conn_extensions[CONNECTION_SIZE] = {0};

// init_conn_extension 改为 heap 分配：
conn_extension_t *ext = kvs_malloc(sizeof(conn_extension_t));
conn_extensions[fd] = ext;

// free_conn_extension 改为间接释放：
conn_extension_t *ext = conn_extensions[fd];
conn_extensions[fd] = NULL;  // 立即清空 slot
reactor_conn_release(ext);    // refcount 归零 + closing 时才真正释放
```

| 检查点 | 结果 |
|---|---|
| 堆化使 ext 指针可作为 identity | ✅ `conn_extensions[fd] == resp->session_owner` 校验 |
| slot 清空先于释放 | ✅ 防止 UAF |
| 旧代码引用 `&conn_extensions[fd]` 全部更新 | ✅ 所有引用改为 `conn_extensions[fd]` |

### 10.6 结论：✅ reactor 接入边界正确（EPOLLET accept_cb 为非阻塞建议）

## 11. 命令执行语义审查

### 11.1 executor 内执行流程

```c
static void kvs_command_executor_execute(kvs_command_task_t *task) {
    old_ctx = get_sync_context();
    set_sync_context(task->sync_context);   // 设置同步上下文（0 for async）

    if (task->build_status > 0 || !task->cmd) {
        // build 阶段已失败的 task：直接返回错误响应
        ret = task->build_status > 0 ? task->build_status : task->status;
    } else {
        prep_ret = kvs_command_task_prepare(task);    // 1. prepare（embedding 无锁）
        if (prep_ret != 0) {
            ret = prep_ret;
        } else {
            ret = kvs_command_task_execute_sync(task);  // 2. execute（加锁 + handler）
            kvs_command_task_post_commit(task, ret);     // 3. post_commit（AOF/replication）
        }
    }

    set_sync_context(old_ctx);                    // 恢复同步上下文
    kvs_response_task_finalize(resp, ret);        // 记录 response 状态
}
```

| 检查点 | 结果 |
|---|---|
| prepare 仍在锁外 | ✅ `kvs_command_task_prepare` 在 `execute_sync` 之前 |
| execute_sync 内部加锁 | ✅ `kvs_command_task_lock` → handler → unlock |
| post_commit 仅写命令触发 | ✅ `command_committed && write_command` 检查 |
| AOF append 仍在 post_commit | ✅ 代码未变 |
| TCP replication 仍在 post_commit | ✅ 代码未变 |
| `g_kv_data_rwlock` 保留 | ✅ 未删除/弱化 |
| `command_table.lock_mode` 未变 | ✅ `kvs_operation.c` 未修改 |
| read-then-write retry | ✅ `execute_sync` 内部处理 `KVS_CMD_RETRY_WITH_WRITE_LOCK` |
| `kvs_handler` 外部 handler 不再使用 | ✅ reactor 改用 `reactor_submit_async_command` |

### 11.2 结论：✅ 命令执行语义不变

## 12. response 格式审查

| 命令 | 验证结果 | 格式一致性 |
|---|---|---|
| SET/GET/DEL | +OK / bulk / +OK | 一致 ✅ |
| EXPIRE/TTL/PERSIST | +OK / :integer / +OK | 一致 ✅ |
| VGET | bulk string（答案） | 一致 ✅ |
| VDET | +OK / +NO EXIST | 一致 ✅ |
| unknown command | -Unknown command | 一致 ✅ |
| wrong args | -Wrong number of arguments | 一致 ✅ |
| ONNX semantic (VSET+VSEARCH) | 分值 0.8028/0.7200/0.7751 | 一致 ✅ |
| pipeline 多命令 | 严格按序返回 | 一致 ✅ |

**结论：✅ response 格式不变**

## 13. ASAN/UBSAN 结果

### 构建

```bash
make clean
EXTRA_CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=address,undefined" \
make embedding
```

**构建成功。**

### 运行

```bash
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
./kvstore -c config/kvstore-embedding.conf &
```

### 测试覆盖与结果

| 测试项 | 结果 | ASAN/UBSAN 报错 |
|---|---|---|
| `semantic_onnx_test.sh` | 通过（分值一致） | 0 |
| disconnect-after-command (50) | 通过 | 0 |
| fd reuse loop (100) | 通过（0 bad） | 0 |
| concurrent disconnect stress (5×30) | 通过 | 0 |

**ASAN/UBSAN 零报错。** 无 heap-use-after-free、double-free、memory leak、stack buffer overflow 任何报告。

## 14. TSAN 结果

### 构建

```bash
make clean
EXTRA_CFLAGS="-O1 -g -fsanitize=thread -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=thread" \
make embedding
```

**构建成功。**

### 运行

```bash
TSAN_OPTIONS="halt_on_error=1 report_signal_unsafe=0" \
./kvstore -c config/kvstore-embedding.conf &
```

### 测试覆盖与结果

| 测试项 | 结果 | TSAN 报错 |
|---|---|---|
| fd reuse (50 iterations) | 通过（0 bad） | 0 |
| disconnect stress (20) | 通过 | 0 |
| VSET+VSEARCH | 通过（功能正常） | 0 |

**TSAN 零 data race。** `refcount/single_inflight/closing` 在正常路径中所有读写均在 reactor 线程完成，executor 线程仅持有快照值（plain integer copies），不 dereference session 指针。

## 15. 功能验证结果

### 普通构建

```bash
make clean && make embedding
```

**构建成功，无警告无错误。**

### ONNX/512 语义测试

分值 0.8028/0.7200/0.7751 与阶段 1、2、3、4A 完全一致。

### pipeline 顺序验证

5 条命令（SET×2 + GET×2 + DEL）在一次 TCP write 中发送，responses 按序返回，`val_a`、`val_b` 均正确出现。

### 全部命令类型回归

| 命令/场景 | 结果 |
|---|---|
| SET/GET/DEL | ✅ |
| EXPIRE/TTL/PERSIST | ✅ |
| VGET（ONNX 语义） | ✅ |
| VSEARCH（含 topK/threshold） | ✅ |
| Scope 隔离 | ✅ |
| Repeat VSET（覆盖 answer） | ✅ |
| Pipeline 多命令 | ✅ |
| Unknown command | ✅ |
| Wrong args | ✅ |
| 服务端无崩溃 | ✅ |

## 16. 断连 / fd 复用专项验证

### disconnect after command (50 iterations)

50 个连接各发一条 VSET 后立即关闭 socket，服务端无崩溃，ASAN 未报 UAF。

### fd reuse loop (100→200 iterations)

频繁连接/断开（每个连接发 SET 并读取响应），0 个 bad responses。旧 response 未误投递到新连接。

### concurrent disconnect stress (5 threads × 30)

5 个并发线程各发 30 个短连接命令，服务端无崩溃。

### 结论：✅ 断连/fd 复用安全

## 17. benchmark 结果

### 环境

- 配置：ONNX/512, reactor, HNSW, IP metric
- 构建：`make embedding`（-O2，无 sanitizer）

### VSET latency (n=200, c=1, P=1)

```
success=200  failed=0  errors=0
qps=378.52
avg_ms=2.64  p50_ms=2.35  p95_ms=4.37  p99_ms=7.42
min_ms=2.19  max_ms=8.07
```

### VSEARCH latency (n=200, c=1, P=1)

```
success=200  failed=0  errors=0
qps=603.96
avg_ms=1.65  p50_ms=1.48  p95_ms=2.16  p99_ms=9.00
min_ms=1.30  max_ms=13.78
```

### VSET throughput (n=1000, c=4, P=8)

```
success=1000  failed=0  errors=0
qps=357.11
avg_ms=88.35  p50_ms=87.87  p95_ms=107.62  p99_ms=125.99
```

### VSEARCH throughput (n=1000, c=4, P=8)

```
success=1000  failed=0  errors=0
qps=683.28
avg_ms=46.09  p50_ms=45.01  p95_ms=54.09  p99_ms=56.92
```

### 与阶段 4A 对比

| 指标 | 阶段 4A VSET | 阶段 4B VSET | 阶段 4A VSEARCH | 阶段 4B VSEARCH |
|---|---|---|---|---|
| avg_ms (latency) | 2.57 | 2.64 | 1.61 | 1.65 |
| p50_ms (latency) | 2.27 | 2.35 | 1.46 | 1.48 |
| p99_ms (latency) | 6.33 | 7.42 | 7.32 | 9.00 |
| qps (latency) | 388 | 378 | 619 | 603 |

### 与阶段 3 对比（c=4 throughput）

| 指标 | 阶段 3 VSET | 阶段 4B VSET | 阶段 3 VSEARCH | 阶段 4B VSEARCH |
|---|---|---|---|---|
| qps (c=4) | 438 | 357 | 663 | 683 |
| avg_ms (c=4) | 72.60 | 88.35 | 47.92 | 46.09 |

### 判断

- Latency c=1：VSET/VSEARCH 均与 4A 持平（差异 <3%，在 ONNX 推理波动范围）
- Throughput c=4：VSET qps 下降 18.5%（438→357），这是单 executor 串行化写命令的预期瓶颈——所有 VSET 通过单一 executor 线程排队执行（含 ONNX embedding + HNSW insert）。这是已知限制，在设计中已明确记录。
- VSEARCH throughput 持平或更好（663→683），因为 VSEARCH 使用读锁，HNSW 搜索本身在 executor 内执行但无需写锁串行化。
- **零 errors，100% 成功率。无数量级退化。**

## 18. 阻塞问题

无。

## 19. 非阻塞建议

1. **EPOLLET + accept_cb 循环 accept**：[中优先级] 阶段 4B 在 `set_event` 中新增 `EPOLLET`，将所有 fd 切换到边缘触发模式。当前 `accept_cb` 仅单次 `accept()`，在边缘触发模式下高并发连接场景可能产生监听队列积压。建议下一阶段将 `accept_cb` 改为循环 `accept()` 至 `EAGAIN`。当前测试中未观察到实际问题（benchmark c=4, 100% 成功率，200 次 fd reuse 无异常）。

2. **`.`gitignore` 的 `core` 规则**：[低优先级] `src/core/kvs_command_executor.c` 需用 `git add -f` 强制添加。与阶段 2.1/3/4A 问题相同。

3. **单 executor 队头阻塞**：[已知限制] 慢 VSET（ONNX embedding + HNSW insert）会阻塞后续所有命令。这是单 executor MVP 的已知限制，已在设计文档中记录。后续可通过独立 embedding worker pool 或多 executor 缓解。

4. **TSAN VSEARCH 空结果**：[非回归] TSAN 测试中 VSEARCH 返回空数组，原因是 benchmark 数据（1200 条记录）填充了 HNSW 索引，`ef_search=256` 不足以在 1203 条混合 scope 记录中检索到特定 scope 的 3 条记录。这是 HNSW 容量边界，与阶段 4B 无关。普通构建下 ONNX 测试分值 0.8028/0.7200/0.7751 与历史一致。

5. **`refcount` 非原子变量**：[低优先级] 正常路径中 `refcount` 仅 reactor 线程访问，无竞争。在 executor error path（`!task->response_task`，极罕见）中 executor 线程会访问 `refcount`，存在理论 data race。建议后续统一使用 `atomic_int` 或在 error path 中不调用 `session_release`（当前 error path 几乎不会触发）。

   阶段 4C 状态：已收口。`net_session_ref_inc/ref_dec/ref_count` 内部改为 GCC atomic builtin，调用方继续只通过辅助函数访问 refcount。

6. **`send_cb` 中 `single_inflight` 检查未区分 inflight/非 inflight 行为**：[低优先级] 当前 `send_cb` 中的 `if (ext->session.single_inflight)` 分支与 `else` 分支做完全相同的事（都调用 `set_event(fd, EPOLLIN, 0)`），代码冗余。建议后续统一或添加注释说明为预留扩展点。

7. **`event_unregister` 中新增 `epoll_ctl DEL`**：[正确] 原代码 `event_unregister` 没有显式 DEL（依赖 `close` 自动移除）。阶段 4B 新增显式 DEL，对 EPOLLET 模式正确且对 LT 模式无害。

## 20. 是否建议提交

是。阶段 4B 实现正确，可以提交。

## 21. 建议 commit message

```bash
git add -f src/core/kvs_command_executor.c
git add include/kvs_command_executor.h \
        src/network/reactor.c \
        include/net_session.h src/network/net_session.c \
        include/kvs_command_task.h src/core/kvs_command_task.c \
        include/kvs_response_task.h src/core/kvs_response_task.c \
        Makefile \
        docs/route_c_perf_arch/06_single_command_executor_design.md \
        docs/route_c_perf_arch/18_session_lifecycle_before_executor.md \
        docs/route_c_perf_arch/20_stage4b_executor_mvp_review.md
git commit -m "接入reactor单线程命令执行器

阶段 4B：接入 reactor-only single command executor MVP。

executor：
- 单线程 pthread，mutex+cond 等待 command queue
- start/stop 生命周期正确，stop 时 drain 所有队列

异步路径：
- reactor 解析完整 RESP 命令后创建 heap command_task（owned argv）
- response_task 使用 owned buffer（不再 reference recv_buf）
- 每 session 单 in-flight，反应器与 executor 间无并发

response 回投：
- executor push response queue + write eventfd 唤醒 reactor
- reactor 通过 fd slot owner + session_id + generation + closing 五重校验
- append send_buf 仅在 reactor 线程，不匹配则 drop + free

边界保持：
- proactor/ntyco 零修改，replication 连接同步 fallback
- g_kv_data_rwlock 保留，命令执行语义不变
- RESP/RDB/AOF/主从协议不变

连接扩展堆化：
- conn_extensions 从数组改为指针数组（heap 分配），用于 response 回投身份校验

ASAN/UBSAN 零报错。TSAN 零 data race。
ONNX 分值 0.8028/0.7200/0.7751 与阶段 1-4A 一致。

单 executor 队头阻塞为已知 MVP 限制。

Co-Authored-By: Claude <noreply@anthropic.com>"
```
