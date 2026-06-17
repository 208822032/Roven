# Stage 10.4: Ntyco Executor Readiness / No-Rwlock-Removal Mode Decision

## 背景

Stage 10.3 (`46_stage10_3_replicated_command_task.md`) 已将 slave apply 收敛为
`replicated_command_task`，由 single command executor 串行 apply。

剩余 rwlock removal blocker 之一是 **ntyco 同步 handler**——所有 ntyco 命令（包括
GET/EXIST/TTL/VSET/VSEARCH）都在 coroutine 中同步执行 `kvs_filter_protocol`，
完全绕过 command executor。

本阶段审计 ntyco 是否应接入 command executor，还是标记为
legacy sync / no-rwlock-removal mode。

## Stage 10.3 后剩余 Blocker

| # | Blocker | 状态 |
|---|---------|------|
| 1 | TTL active expire | ✅ Stage 10.1 |
| 2 | Master full sync 直接遍历 live store | ❌ |
| 3 | Slave apply 直接修改 KV/TTL | ✅ Stage 10.3 |
| 4 | AOF rewrite 直接遍历 live store | ❌ |
| 5 | RDB save 直接遍历 live store | ❌ |
| 6 | Ntyco 同步 handler | → Stage 10.4 |
| 7 | Reactor/proactor replication fallback | ❌ |
| 8 | Proactor OOM fallback | ❌ |

## Ntyco 源码文件

| 文件 | 行数 | 状态 |
|------|------|------|
| `src/network/ntyco.c` | 389 | ✅ 编译生产文件 |
| `src/network/ntyco_defult.c` | 365 | ❌ 未编译（历史备份） |

`ntyco_defult.c` 是早期版本，编译系统使用的入口文件是 `ntyco.c`。

## Ntyco 当前完整调用链

### 启动路径

```
main()
  → init_kvengine()                                           [src/core/kvstore.c:311-315]
    → if (g_cfg.network_mode == NETWORK_NTYCO)                [kvs_config.h:20]
      → ntyco_start(g_cfg.port, kvs_filter_protocol)          [src/network/ntyco.c:362]
        → kvs_handler = handler                               // handler = kvs_filter_protocol
        → ntyco_init_server(port)
          → socket(AF_INET, SOCK_STREAM)
          → bind + listen
          → fcntl(O_NONBLOCK)
        → nty_coroutine_create(accept_co, ntyco_accept_routine, listen_fd)
        → nty_schedule_run()                                  // [ntyco.c:381]
          // 单线程 epoll + 协程调度循环
          // 阻塞直到所有协程退出或 g_shutdown_flag 置位
        → nty_close(listen_fd)
```

### Accept 路径

```
ntyco_accept_routine(void *arg)                                [ntyco.c:266]
  → while (!g_shutdown_flag):
    → nty_accept(listen_fd, &clientaddr, &len)                // 非阻塞 accept，内部 poll+挂起
    → kvs_malloc(sizeof(ntyco_conn_ctx_t))                    // 堆分配连接上下文
    → ntyco_init_conn_ctx(ctx, connfd)
      → net_session_init(&ctx->session, 8MB, 8MB)             // 8MB recv/send 缓冲区
      → setsockopt(TCP_NODELAY)
      → setsockopt(SO_RCVBUF/SO_SNDBUF, 8MB)
    → nty_coroutine_create(co, ntyco_conn_routine, ctx)       // 为每个连接创建协程
```

### 连接处理路径（核心）

```
ntyco_conn_routine(void *arg)                                  [ntyco.c:161]
  → while (!g_shutdown_flag):
    → poll(&pfd, 1, -1)                                       // 标准 poll 等待 POLLIN/POLLOUT
    → 如果 POLLOUT 且有待发送数据:
      → send(fd, send_buf->data + send_offset, ...)           // 原生 send()
    → 如果 POLLIN:
      → recv_buf 扩容检查
      → recv(fd, recv_buf->data + recv_buf->size, ...)        // 原生 recv()
      → while(1):
        → ntyco_process_received_data(ctx)                      [ntyco.c:154]
          → net_session_process_resp(fd, session,               [net_session.c:152]
                                     ntyco_session_handler,
                                     ctx)
            → RESP 解析循环（net_session loop）
              → resp_peek_command()                            // 无状态探测完整命令
              → resp_parser()                                  // 有状态 RESP 解析
              → response_buf = send_buf->data + send_buf->size // 响应直接写入 send_buf
              → ntyco_session_handler(parser, response_buf,    [ntyco.c:91]
                                      response_buf_cap, ctx)
                → 解析 cmd_name = parser->tokens[0]
                → 如果是 REPLSYNC 命令：标记 ctx->is_replication = 1，返回 +OK
                → 否则：
                  → set_sync_context(is_replication ? 2 : 0)
                  → kvs_handler(parser->tokens,                 // = kvs_filter_protocol!
                                parser->arg_index,
                                parser->token_lens,
                                response_buf)
                    → kvs_filter_protocol()                     [kvstore.c:52]
                      → kvs_response_task_init_borrowed()       // 栈上 borrowed response
                      → kvs_command_task_build()                // 栈上 command_task
                      → kvs_command_task_prepare()              // VSET/VSEARCH embedding pre
                      → kvs_command_task_execute_sync()         // ← 同步执行！
                        → kvs_command_task_lock(task)
                          → pthread_rwlock_rdlock/wrlock(&g_kv_data_rwlock)
                        → kvs_command_task_execute_handler(task)
                          → task->cmd->handler(&task->ctx, task->response)
                          // handle_get / handle_set / handle_vset / handle_vsearch ...
                        → 如果需要 lazy expire (GET/EXIST):
                          → handler 返回 KVS_CMD_RETRY_WITH_WRITE_LOCK
                          → pthread_rwlock_unlock(&g_kv_data_rwlock)
                          → pthread_rwlock_wrlock(&g_kv_data_rwlock)  // 读锁升级写锁
                          → task->cmd->handler() 再次执行 → 删除过期 key
                        → kvs_command_task_unlock(task)
                      → kvs_command_task_post_commit()         // AOF + replication
                  → set_sync_context(old_ctx)
```

### 与 Reactor/Proactor 的关键对比

| 维度 | Reactor Normal | Reactor Repl | Ntyco Normal | Ntyco Repl |
|------|---------------|-------------|-------------|-----------|
| 入口函数 | `reactor_submit_async_command` | `reactor_submit_async_command` | `ntyco_conn_routine` | `ntyco_conn_routine` |
| 命令处理 | heap alloc task → executor thread | sync `kvs_filter_protocol` in epoll thread | sync `kvs_filter_protocol` in coroutine | sync `kvs_filter_protocol` in coroutine |
| command_task 分配 | heap (`kvs_malloc`) | stack (in `kvs_filter_protocol`) | stack (in `kvs_filter_protocol`) | stack (in `kvs_filter_protocol`) |
| response_task 分配 | heap owned buffer | borrowed buffer (send_buf) | borrowed buffer (send_buf) | borrowed buffer (send_buf) |
| argv 拷贝 | `kvs_command_task_copy_argv` | 无 (零拷贝) | 无 (零拷贝) | 无 (零拷贝) |
| executor submit | ✅ `kvs_command_executor_submit` | ❌ | ❌ | ❌ |
| 执行线程 | executor thread | epoll/reactor thread | ntyco coroutine thread | ntyco coroutine thread |
| `g_kv_data_rwlock` | 持有 (executor 内) | 持有 (epoll thread 内) | 持有 (coroutine 内) | 持有 (coroutine 内) |
| lazy expire owner | executor ✅ | epoll thread ❌ | coroutine ❌ | coroutine ❌ |
| response 投递 | response queue → epoll fd | 直接写入 send_buf | 直接写入 send_buf | 直接写入 send_buf |

## 审计问题逐一回答

### 1. ntyco 是否创建 command_task

**是，但是以栈变量形式。** `kvs_filter_protocol` (kvstore.c:54) 在栈上声明
`kvs_command_task_t task`，然后调用 `kvs_command_task_build(&task, ...)` 初始化。
没有堆分配，没有 `kvs_command_task_copy_argv`（零拷贝直接引用 parser->tokens）。

这与 reactor normal path 不同：reactor 在堆上 `kvs_malloc(sizeof(kvs_command_task_t))`，
然后 `kvs_command_task_copy_argv` 深拷贝所有参数，再 `kvs_command_executor_submit(task)`。

### 2. ntyco 是否接入 command executor

**否。** ntyco 完全不接触 `kvs_command_executor_t`。
- 不调用 `kvs_command_executor_submit()`
- 不访问 `g_executor` 的任何字段
- 不访问 executor 的四个队列（command/prepared/replicated/TTL）
- executor 的 cv/signal 机制对 ntyco 不生效

命令执行完毕后，响应通过 `kvs_response_task_finalize` 写入 send_buf，
然后由 `ntyco_conn_routine` 的 send 路径发送给客户端。

### 3. ntyco 是否绕过 executor 访问 store/TTL/vector

**是。** 所有 ntyco 命令通过 `kvs_filter_protocol → kvs_command_task_execute_sync`
直接访问底层 store。具体路径：

- **Store 访问**：`kvs_command_task_execute_handler` → `task->cmd->handler(&task->ctx, task->response)` → 直接调用 `kvs_array_set/get/del` 等底层 API
- **TTL 访问**：`handle_get/handle_exist` → `ttl_expire_if_needed_by_structure` → `ttl_target_delete`；`handle_expire` → `ttl_set_any_key_expire_at_ms`
- **Vector 访问**：`handle_vset` → `kvs_vector_set_commit_prepared`；`handle_vsearch` → `kvs_vector_search_prepared_to_resp` → `kvs_vector_search_with_prepared`

所有访问在 `g_kv_data_rwlock` 保护下进行，但不在 executor 线程内。

### 4. ntyco 下 GET/EXIST lazy expire 是否绕过 executor

**是。** lazy expire 的完整路径在 ntyco coroutine 内完成：

```
kvs_command_task_execute_sync(task)         // [ntyco coroutine]
  → pthread_rwlock_rdlock(&g_kv_data_rwlock)
  → kvs_command_task_execute_handler(task)
    → handle_get/handle_exist
      → ttl_expire_if_needed_by_structure(..., apply_expired_side_effects=0)
      → ttl_ret > 0 → return KVS_CMD_RETRY_WITH_WRITE_LOCK
  → pthread_rwlock_unlock(task)
  → pthread_rwlock_wrlock(&g_kv_data_rwlock)   // 读锁升级写锁
  → task->cmd->handler(&task->ctx, task->response)
    → ttl_expire_if_needed_by_structure(..., apply_expired_side_effects=1)
    → ttl_target_delete(key)                     // ← 在 ntyco coroutine 内删除！
```

**结论**：❌ ntyco lazy expire 完全绕过 executor。虽然在 `g_kv_data_rwlock` 保护下
语义正确，但 executor 对此一无所知。

### 5. ntyco 下 SET/DEL/EXPIRE/VSET/VSEARCH 是否绕过 executor

**全部绕过。** 这些写命令在 ntyco coroutine 内执行 `kvs_filter_protocol`，
直接拿 `g_kv_data_rwlock` 写锁，然后修改 store/TTL/vector。

| 命令 | 绕过 executor | 持有 g_kv_data_rwlock | 执行线程 |
|------|-------------|----------------------|---------|
| SET | ✅ 绕过 | wrlock | ntyco coroutine |
| DEL | ✅ 绕过 | wrlock | ntyco coroutine |
| EXPIRE | ✅ 绕过 | wrlock | ntyco coroutine |
| PEXPIREAT | ✅ 绕过 | wrlock | ntyco coroutine |
| PERSIST | ✅ 绕过 | wrlock | ntyco coroutine |
| TTL | ✅ 绕过 | wrlock | ntyco coroutine |
| GET | ✅ 绕过 | rdlock (可升级 wrlock) | ntyco coroutine |
| EXIST | ✅ 绕过 | rdlock (可升级 wrlock) | ntyco coroutine |
| VSET | ✅ 绕过 | wrlock | ntyco coroutine |
| VSEARCH | ✅ 绕过 | wrlock | ntyco coroutine |
| VGET | ✅ 绕过 | wrlock | ntyco coroutine |
| VDEL | ✅ 绕过 | wrlock | ntyco coroutine |
| VINFO | ✅ 绕过 | wrlock | ntyco coroutine |

**ntyo 模式下，没有任何命令通过 executor 执行。** 这是所有同步 fallback 路径中
范围最大的绕过——覆盖 100% 的客户端命令。

## 方案评估

### 方案 A：Ntyco Async Executor 改造

**目标**：让 ntyco 像 reactor normal path 一样，将命令提交到 executor 线程执行，
然后通过 response queue 返回结果。

#### 需要的改造

1. **命令提交改造**（`ntyco_conn_routine` / `ntyco_session_handler`）：
   - 不再调用 `kvs_filter_protocol`
   - 改为 heap-allocate `kvs_command_task_t` + `kvs_response_task_t`
   - `kvs_command_task_copy_argv` 深拷贝参数（因为 parser->tokens 在 coroutine 栈上）
   - `kvs_command_executor_submit(task)` 提交到 executor
   - 协程需要 **yield** 等待 response 就绪

2. **Response 回调机制**：
   - 需要在 ntyco 和 executor 之间建立 response 通知通道
   - 选项 a：在 `kvs_response_task_t` 中设置回调函数/协程指针，executor 完成后 resume 协程
   - 选项 b：ntyco 定期 poll response queue
   - 都需要线程安全的协程 resume 机制

3. **Replication 路径**（`ctx->is_replication = 1`）：
   - 复制连接的命令也需要走 executor
   - 但复制连接不发送响应（`send_buf->size = 0`）
   - 需要 `replicated_command_task` 而非 `command_task`

4. **协程调度器集成**：
   - NtyCo 使用自己的单线程 epoll 调度器（`nty_schedule_run`）
   - executor 在另一个线程中运行
   - 协程 yield 后需要被 executor 线程 resume——这需要跨线程协程操作
   - NtyCo 库可能不支持跨线程 resume（需审计 NtyCo 源码）

#### 成本评估

| 改造项 | 复杂度 | 风险 | 预估工时 |
|--------|--------|------|---------|
| heap-alloc task + copy_argv | 低 | 低 | 0.5 天 |
| executor submit 路径 | 中 | 低 | 0.5 天 |
| response 回调/通知机制 | **高** | **高** | 2-3 天 |
| 协程 yield/resume 跨线程 | **很高** | **很高** | 3-5 天 |
| replication 路径 | 中 | 中 | 1 天 |
| 测试 + 回归 | 高 | 高 | 2-3 天 |
| **总计** | | | **9-13 天** |

#### 风险

1. **NtyCo 库限制**：NtyCo 可能不支持跨线程 resume 协程。如果协程 A 在 epoll 线程上运行，
   executor 线程需要 resume 协程 A——这需要 NtyCo 内部线程安全保证。

2. **死锁风险**：如果协程持有了某资源后在 executor 等待 response，而 executor
   需要同一资源，会死锁。虽然 KVLite 只用 `g_kv_data_rwlock`，但在 rwlock removal
   后可能有新的锁。

3. **性能退化**：ntyco 当前零拷贝（直接引用 parser->tokens），改造后需要
   `copy_argv` + heap alloc + 跨线程通知，可能引入明显的延迟开销。

4. **测试复杂度**：ntyco 模式的使用场景有限，完整覆盖 ntyco 改造后的
   并发测试成本高。

5. **实质等价于重写 ntyco**：改造后的 ntyco 与 reactor 结构几乎相同，
   只是底层 I/O 使用协程而非 epoll。这可能不值得——如果追求 executor 一致性，
   不如直接使用 reactor。

#### 收益

- ntyco 命令与 reactor/proactor 统一走 executor
- ntyco 不再阻止 rwlock removal
- ntyco 的 lazy expire 成为 executor-owned

### 方案 B：Ntyco 标记为 Legacy Sync / No-Rwlock-Removal Mode

**目标**：承认 ntyco 为遗留模式，保留其当前行为（同步执行 + `g_kv_data_rwlock` 保护），
不在 rwlock removal 范围内。

#### 需要的改动

1. **启动 warning**（`kvstore.c:314`）：
   ```c
   if (mode == NETWORK_NTYCO) {
       KVS_LOG_WARN("core",
           "network_mode=ntyco runs in legacy sync mode. "
           "rwlock removal is NOT supported in this mode. "
           "Commands execute synchronously in coroutine thread, "
           "not via command executor.");
       KVS_LOG_INFO("core", "starting in NTYCO mode on port %d", g_cfg.port);
       ntyco_start(g_cfg.port, kvs_filter_protocol);
   }
   ```

2. **文档**：本文件 (`47_stage10_4_ntyco_executor_readiness.md`) 即为决策文档

3. **不做**：不删除 ntyco、不锁定功能、不改变 ntyco 行为

#### 成本评估

| 改动项 | 复杂度 | 风险 | 预估工时 |
|--------|--------|------|---------|
| 启动 warning | 极低 | 无 | 0.1 天 |
| 文档 | 低 | 无 | 0.5 天 |
| 测试 | 低 | 无 | 0.3 天 |
| **总计** | | | **<1 天** |

#### 风险

- **无风险。** 不改变任何功能行为、不改变锁语义、不改变用户 API。

#### 收益

- 明确 ntyco 在 rwlock removal 路线图中的位置
- 保留 ntyco 作为开发和测试选项
- 资源集中在更有价值的 blocker 上（master full sync / AOF rewrite / RDB save snapshot_task）
- 如果未来有实际需求，可以单独立项做 ntyco async executor 改造

## 推荐路线：方案 B

### 推荐理由

1. **成本不成比例**：方案 A 需要 9-13 天改造 ntyco 的协程调度模型，
   而 ntyco 的使用场景有限（主要是开发/测试环境）。

2. **NtyCo 库限制**：NtyCo 是一个外部协程库，其跨线程 coroutine resume 能力
   未经审计。如果 NtyCo 不支持，方案 A 的工作量会更高。

3. **生产环境主流是 reactor/proactor**：reactor 使用成熟 epoll 模型，
   proactor 使用 io_uring——两者都已经集成 command executor。
   ntyco 是第三选项，重要性相对较低。

4. **不影响 rwlock removal 的主线推进**：ntyco 可以留在自己的 `g_kv_data_rwlock`
   保护下，reactor/proactor 的 rwlock removal 不需要 ntyco 参与。

5. **可逆决策**：如果未来 ntyco 的使用场景增加，可以随时从方案 B 切换到方案 A。
   方案 B 只是"暂不做"，不是"永远不做"。

6. **与现有 blocker 优先级一致**：剩余 blocker 中，
   master full sync、AOF rewrite、RDB save 对性能和运维的影响远大于 ntyco。

### 方案 B 后的 Ntyco 定位

| 属性 | 值 |
|------|-----|
| 网络模式 | 遗留支持 (legacy) |
| 命令执行 | 同步（coroutine 内） |
| 锁保护 | `g_kv_data_rwlock` 永久持有 |
| rwlock removal | 不在范围内 |
| 功能演进 | 不新增功能，只做 bug 修复 |
| 推荐替代 | reactor（epoll）或 proactor（io_uring） |

## 当前是否可以移除 g_kv_data_rwlock（仅 reactor/proactor）

**否。** 即使标记 ntyco 为 no-rwlock-removal，`g_kv_data_rwlock` 仍不能移除，
因为还有以下 blocker：

| Blocker | 状态 |
|---------|------|
| Master full sync 遍历 live store | ❌ |
| AOF rewrite 遍历 live store | ❌ |
| RDB save 遍历 live store | ❌ |
| Reactor/proactor replication fallback | ❌ |
| Proactor OOM fallback | ❌ |
| Shutdown destroy | ❌ |

## Ntyco 是否是 rwlock removal blocker

**是，但通过方案 B 可将其排除在 rwlock removal 范围之外。**
具体来说：

- ntyco **不阻止**从 reactor/proactor 路径移除 `g_kv_data_rwlock`
- 如果移除 rwlock，ntyco 不能与 reactor/proactor 并发运行（因为 ntyco 仍依赖 rwlock）
- 实际上，`network_mode` 只能选择一个，三种网络模式不会同时运行
- 因此：**ntyco 不阻止 reactor/proactor 的 rwlock removal**

但需要注意：如果三种模式在同一个二进制中共存（通过运行时配置切换），
则 rwlock removal 后 ntyco 模式将失去数据一致性保护。
**这时需要在 ntyco 模式启动时主动拒绝或要求 rwlock 存在。**

当前设计已满足此条件：三种网络模式互斥（`network_mode` 单选），
rwlock removal 只需确保 `NETWORK_NTYCO` 模式下保护仍然生效。

## 启动 Warning 实现

`src/core/kvstore.c:313` 增加：

```c
if (mode == NETWORK_NTYCO) {
    KVS_LOG_WARN("core",
        "network_mode=ntyco runs in legacy sync mode. "
        "Commands bypass command executor. "
        "rwlock removal is NOT supported in this mode.");
    KVS_LOG_INFO("core", "starting in NTYCO mode on port %d", g_cfg.port);
    ntyco_start(g_cfg.port, kvs_filter_protocol);
}
```

## 不做事项

- 不删除/弱化 `g_kv_data_rwlock`
- 不做 rwlock removal
- 不大改 ntyco 网络栈（不改造协程调度器）
- 不把 ntyco 改造成 async executor 模型
- 不做 batch/per-thread OrtSession/多 executor/commit ordering
- 不改 RESP/store/AOF/RDB/replication/HNSW 语义
- **不删除 ntyco 代码**
- **不锁定 ntyco 功能**

## 剩余 Blocker（Stage 10.4 后）

| # | Blocker | 状态 |
|---|---------|------|
| 1 | TTL active expire | ✅ Stage 10.1 |
| 2 | Master full sync 直接遍历 live store | ❌ |
| 3 | Slave apply 直接修改 KV/TTL | ✅ Stage 10.3 |
| 4 | AOF rewrite 直接遍历 live store | ❌ |
| 5 | RDB save 直接遍历 live store | ❌ |
| 6 | Ntyco 同步 handler | ✅ Stage 10.4（标记为 legacy no-rwlock-removal） |
| 7 | Reactor/proactor replication fallback | ❌ |
| 8 | Proactor OOM fallback | ❌ |

## Tests 结果

测试脚本 `tests/ntyco_stage10_4.sh`：

```
bash -n tests/ntyco_stage10_4.sh:    ✅ passed
tests/ntyco_stage10_4.sh:             ✅ passed (ALL PASSED)
```

| 测试用例 | 覆盖项 | 结果 |
|----------|--------|------|
| `test_ntyco_startup_warning` | 验证启动日志包含 "legacy sync mode" / "bypass command executor" / "rwlock removal NOT supported" | ✅ PASSED |
| `test_ntyco_basic_ops` | SET/GET/DEL 基本操作在 ntyco 模式下正确 | ✅ PASSED |
| `test_ntyco_lazy_expire` | GET 触发 lazy expire（过期 key 返回 NO EXIST），TTL 返回 -2 | ✅ PASSED |
| `test_ntyco_ttl_ops` | EXPIRE/TTL/PERSIST/PEXPIREAT 在 ntyco 模式下正确 | ✅ PASSED |
| `test_ntyco_shutdown` | QUIT 优雅关闭 | ✅ PASSED |

**关键验证**：
- ✅ 启动 warning 确认出现：`network_mode=ntyco runs in legacy sync mode. Commands bypass command executor. rwlock removal is NOT supported in this mode.`
- ✅ ntyco 模式下所有基本命令正常运行
- ✅ lazy expire 在 ntyco coroutine 中正确触发
- ✅ TTL 命令语义完整

## Regression 结果

```
tests/ttl_executor_stage10_1.sh:              ✅ passed
tests/lazy_expire_stage10_2.sh:               ✅ passed
tests/replicated_command_task_stage10_3.sh:   ✅ passed
tests/embedding_worker_pool_stage9.sh:        ✅ passed
tests/embedding_worker_pool_stage9_1.sh:      ✅ passed
```

## 下一阶段建议

建议进入 **Stage 10.5：Master Full Sync Snapshot Task**，将 master full sync 从
直接遍历 live store 改为通过 executor snapshot_task 生成一致性快照。

后续优先级：
1. Stage 10.5: Master full sync snapshot_task
2. Stage 10.6: RDB save snapshot_task
3. Stage 10.7: AOF rewrite snapshot_task
4. Stage 10.8: Reactor/proactor replication fallback 收敛
5. Stage 10.9: Proactor OOM fallback 收敛
6. Stage 10.10: Shutdown/destroy 收敛 + 最终 rwlock removal 评估
