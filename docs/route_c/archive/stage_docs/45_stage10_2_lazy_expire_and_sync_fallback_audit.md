# Stage 10.2: Lazy Expire Ownership / Synchronous Fallback Audit

## 背景

Stage 10.1 (`44_stage10_1_ttl_ownership_convergence.md`) 已将 TTL active expire 删除 owner
收敛到 single command executor。剩余的去锁 blocker 包括：

1. Lazy expire（GET/EXIST 触发的过期 key 删除）的 ownership 未审计
2. Ntyco 和 reactor/proactor replication/fallback 同步 handler 仍可能绕过 executor
3. Slave apply、master full sync、AOF/RDB rewrite 仍直接访问 live store

Stage 10.2 是审计和文档阶段，不修改生产代码（除非极小的文档/注释修复）。

## Stage 10 / Stage 10.1 后剩余 Blocker

Stage 10 (`43_stage10_rwlock_removal_readiness_audit.md`) 的结论是 `NOT_READY`，列出 7 个 blocker：

| # | Blocker | Stage 10.1 后状态 |
|---|---------|-------------------|
| 1 | TTL active expire 后台线程直接删除 KV | ✅ 已解决（Stage 10.1） |
| 2 | Master full sync 直接遍历 live store | ❌ 剩余 |
| 3 | Slave apply 直接修改 KV/TTL | ❌ 剩余 |
| 4 | AOF rewrite 直接遍历 live store | ❌ 剩余 |
| 5 | RDB save 直接遍历 live store | ❌ 剩余 |
| 6 | Ntyco/replication/fallback 同步调用 kvs_filter_protocol | ❌ 剩余 |
| 7 | Shutdown 顺序未统一 | ❌ 剩余（部分改善） |

Stage 10.2 重点审计 #6（同步 fallback 路径），以及评估 lazy expire（blocker #1 的子项）的 executor ownership。

## 本阶段目标

1. 审计 lazy expire 的完整路径：哪些命令触发、在哪些线程中执行、是否已 executor-owned
2. 审计所有同步 fallback handler 路径：ntyco、reactor replication、proactor replication、proactor fallback
3. 判断 lazy expire 当前是否已是 executor-owned（在 reactor/proactor normal async 路径下）
4. 明确 ntyco/fallback 路径是否形成 rwlock removal 新 blocker
5. 不修改生产代码（除非极小的文档/注释修复）

## 不做事项

- 不删除或弱化 `g_kv_data_rwlock`
- 不做 rwlock removal
- 不做 batch embedding / per-thread OrtSession
- 不做多 command executor / commit ordering / response reorder
- 不做 AOF/RDB/replication/full sync 重构
- 不做 HNSW/index 优化
- 不改变 RESP 协议
- 不改变任何用户可见 KV/vector/TTL 语义
- **不把 ntyco 大规模改造成完整 async executor**
- 不扩大到 RDB/AOF rewrite/full sync snapshot_task

## Lazy Expire 当前路径

### 触发 lazy expire 的命令

lazy expire 影响两类命令：

**A. 读锁命令（可能触发 `KVS_CMD_RETRY_WITH_WRITE_LOCK`）：**

| 命令 | Handler | 锁模式 | Lazy Expire 方式 |
|------|---------|--------|-----------------|
| GET / RGET / HGET / SGET / PAPERGET | `handle_get` | `KVS_CMD_LOCK_READ` | `ttl_expire_if_needed_by_structure()` → 过期时返回 `KVS_CMD_RETRY_WITH_WRITE_LOCK` |
| EXIST / REXIST / HEXIST / SEXIST | `handle_exist` | `KVS_CMD_LOCK_READ` | 同上 |

**B. 写锁命令（直接删除过期 key，无需 RETRY）：**

| 命令 | Handler | 锁模式 | Lazy Expire 方式 |
|------|---------|--------|-----------------|
| TTL | `handle_ttl` | `KVS_CMD_LOCK_WRITE` | `ttl_apply_any_key_op()` → 过期时直接 `ttl_target_apply_expired_side_effects()` |
| EXPIRE | `handle_expire` | `KVS_CMD_LOCK_WRITE` | 同上（如 seconds≤0 或已过期则删除） |
| PEXPIREAT | `handle_pexpireat` | `KVS_CMD_LOCK_WRITE` | 同上（如时间戳已过期则删除） |
| PERSIST | `handle_persist` | `KVS_CMD_LOCK_WRITE` | 同上（如 key 已过期则删除，不 persist） |
| PTTL | `handle_ttl` | `KVS_CMD_LOCK_WRITE` | 同上 |

**C. 不受 lazy expire 影响的命令：**

| 命令 | 说明 |
|------|------|
| VGET | 只搜索 vector store，不检查 normal KV TTL |
| VSEARCH | 只搜索 vector index，不检查 normal KV TTL |
| SET / DEL / MOD | 写锁命令但 handler 不调用 TTL 检查 |
| VSET / VDEL | Vector 命令，独立于 normal KV TTL |

### KVS_CMD_RETRY_WITH_WRITE_LOCK 机制

代码路径（`src/core/kvs_command_task.c:526-563`）：

```c
int kvs_command_task_execute_sync(kvs_command_task_t *task) {
    // 1. 获取读锁（对于读锁命令）
    kvs_command_task_lock(task);  // pthread_rwlock_rdlock(&g_kv_data_rwlock)

    // 2. 执行 handler（第一次）
    ret = kvs_command_task_execute_handler(task);

    // 3. 如果 handler 返回 KVS_CMD_RETRY_WITH_WRITE_LOCK
    if (task->use_read_lock && ret == KVS_CMD_RETRY_WITH_WRITE_LOCK) {
        // 3a. 释放读锁
        kvs_command_task_unlock(task);

        // 3b. 获取写锁
        task->use_read_lock = 0;
        task->use_write_lock = 1;
        pthread_rwlock_wrlock(&g_kv_data_rwlock);

        // 3c. 重新执行 handler（第二次，ctx->lock_mode = KVS_LOCK_MODE_WRITE）
        ret = task->cmd->handler(&task->ctx, task->response);
        // 这次 apply_expired_side_effects = 1 → 删除过期 key
    }

    // 4. 释放锁
    kvs_command_task_unlock(task);
}
```

`handle_get` 和 `handle_exist` 中的关键判断（`src/core/kvs_operation.c:1172,1284`）：

```c
int apply_expired_side_effects = (ctx->lock_mode != KVS_LOCK_MODE_READ);
int ttl_ret = ttl_expire_if_needed_by_structure(..., apply_expired_side_effects);
if (ttl_ret > 0) {
    if (!apply_expired_side_effects) {
        return KVS_CMD_RETRY_WITH_WRITE_LOCK;  // 读锁下：只检测不删除
    }
    return resp_generate_simple_string_n(..., "NO EXIST", ...); // 写锁下：已删除
}
```

## Lazy Expire 是否 Executor-Owned

按执行路径分类：

### 路径 1：reactor/proactor normal async → Executor

```
reactor_submit_async_command / proactor_submit_async_command
  → kvs_command_executor_submit(task)
  → kvs_command_executor_thread_main
    → kvs_command_executor_dispatch_or_execute
      → kvs_command_executor_execute
        → kvs_command_task_execute_sync  ← lazy expire 在此执行
```

**结论**：✅ **LAZY EXPIRE IS EXECUTOR-OWNED** 对于此路径。

`kvs_command_task_lock` → `pthread_rwlock_rdlock(&g_kv_data_rwlock)` 和
`KVS_CMD_RETRY_WITH_WRITE_LOCK` → `pthread_rwlock_wrlock(&g_kv_data_rwlock)` 的锁升级
全部在 executor 线程中执行。这是 reactor 和 proactor 正常 async client 命令的路径。

### 路径 2：reactor replication/REPLSYNC → 绕过 executor

```
reactor_submit_async_command (ext->is_replication || REPLSYNC)
  → net_session_process_resp → reactor_session_handler → kvs_handler
  → kvs_filter_protocol
    → kvs_command_task_execute_sync  ← lazy expire 在此执行，但在 reactor epoll 线程中
```

**结论**：❌ 绕过 executor。lazy expire 在 reactor epoll 线程中执行。

此路径仅在 eBPF 同步模式下，slave 接收到 REPLSYNC 命令后标记 `is_replication=1`。
在 TCP 同步模式下不走此路径（TCP 同步使用 distributed.c 的独立 sync 线程）。

### 路径 3：proactor replication/REPLSYNC → 绕过 executor

```
proactor_submit_async_command (ext->is_replication || REPLSYNC)
  → p_process_received_data → net_session_process_resp
  → proactor_session_handler → kvs_handler
  → kvs_filter_protocol
    → kvs_command_task_execute_sync  ← lazy expire 在此执行，但在 proactor io_uring 线程中
```

**结论**：❌ 绕过 executor。与路径 2 对称。

### 路径 4：proactor build/alloc failure fallback → 绕过 executor

```
proactor_submit_async_command
  → task/resp alloc 失败 or response_task_init_owned 失败 or build 失败
  → p_process_received_data → kvs_filter_protocol
    → kvs_command_task_execute_sync  ← lazy expire 在此执行，但在 proactor io_uring 线程中
```

**结论**：❌ 绕过 executor。此路径是异常降级路径（如内存不足），不常见。

### 路径 5：ntyco → 绕过 executor

```
ntyco_conn_routine
  → ntyco_process_received_data
  → net_session_process_resp → ntyco_session_handler → kvs_handler
  → kvs_filter_protocol
    → kvs_command_task_execute_sync  ← lazy expire 在此执行，在 ntyco coroutine 中
```

**结论**：❌ 绕过 executor。ntyco 模式下**所有命令**（包括 GET/EXIST/TTL）都走此路径。

### 路径 6：ntyco replication → 绕过 executor

与路径 5 相同，但 `ctx->is_replication = 1`，`sync_context = 2`。

**结论**：❌ 绕过 executor。

### 路径 7：AOF load → 绕过 executor（启动期）

```
aof_load → kvs_filter_protocol → kvs_command_task_execute_sync
```

**结论**：❌ 绕过 executor。但在网络启动前执行（main 线程），不与其他线程并发。

## Lazy Expire 汇总

| 路径 | 线程 | Lazy Expire Owner | rwlock 保护 | 是否 rwlock removal blocker |
|------|------|------------------|-------------|---------------------------|
| Reactor async (normal) | executor | ✅ Executor | ✅ rdlock/wrlock | 否 |
| Proactor async (normal) | executor | ✅ Executor | ✅ rdlock/wrlock | 否 |
| Reactor repl/REPLSYNC | reactor epoll | ❌ 绕过 | ✅ rdlock/wrlock | **是** |
| Proactor repl/REPLSYNC | io_uring worker | ❌ 绕过 | ✅ rdlock/wrlock | **是** |
| Proactor fallback | io_uring worker | ❌ 绕过 | ✅ rdlock/wrlock | **是** |
| Ntyco | coroutine | ❌ 绕过 | ✅ rdlock/wrlock | **是** |
| Ntyco replication | coroutine | ❌ 绕过 | ✅ rdlock/wrlock | **是** |
| AOF load | main thread | ❌ 绕过（启动期） | ✅ rdlock/wrlock | 否（单线程启动路径） |

## Synchronous Fallback 路径清单

以下列出所有仍可能绕过 executor 的同步 handler 路径。

### SF-1: reactor replication/REPLSYNC handler

- **文件**: `src/network/reactor.c:635-641`
- **函数**: `reactor_submit_async_command`
- **调用链**: `reactor_submit_async_command` → `net_session_process_resp(fd, session, reactor_session_handler, ext)` → `reactor_session_handler` → `kvs_handler` → `kvs_filter_protocol` → `kvs_command_task_execute_sync`
- **是否绕过 executor**: 是
- **是否可能读共享状态**: 是（rdlock → handler → 可能 wrlock 升级）
- **是否可能写共享状态**: 是（lazy expire 删除）
- **是否可能触发 lazy expire**: 是
- **是否受 g_kv_data_rwlock 保护**: 是
- **是否是 rwlock removal blocker**: 是
- **后续建议收敛方式**: REPLSYNC/replication 命令不应走同步 kvs_handler。在 rwlock removal 前，replication 命令应封装为 replicated_command_task 进入 executor。

### SF-2: proactor replication/REPLSYNC handler

- **文件**: `src/network/proactor.c:778-780`
- **函数**: `proactor_submit_async_command`
- **调用链**: 同 reactor 路径，对称
- **是否绕过 executor**: 是
- **是否可能读共享状态**: 是
- **是否可能写共享状态**: 是
- **是否可能触发 lazy expire**: 是
- **是否受 g_kv_data_rwlock 保护**: 是
- **是否是 rwlock removal blocker**: 是
- **后续建议收敛方式**: 同 SF-1

### SF-3: proactor build/alloc failure fallback

- **文件**: `src/network/proactor.c:787-821`
- **函数**: `proactor_submit_async_command`
- **调用链**: 多个 fallback 点 → `p_process_received_data(ext)` → `net_session_process_resp` → `kvs_filter_protocol`
- **触发条件**: `kvs_malloc` 失败、`kvs_response_task_init_owned` 失败、`kvs_command_task_build` 失败、`kvs_command_task_copy_argv` 失败、`kvs_command_executor_submit` 失败
- **是否绕过 executor**: 是（异常降级）
- **是否可能读/写共享状态**: 是
- **是否可能触发 lazy expire**: 是
- **是否受 g_kv_data_rwlock 保护**: 是
- **是否是 rwlock removal blocker**: 是（但优先级低，仅在 OOM 时触发）
- **后续建议收敛方式**: fallback 不应走 kvs_filter_protocol；应返回错误或降级为同步简单响应。

### SF-4: ntyco normal command

- **文件**: `src/network/ntyco.c:135`
- **函数**: `ntyco_session_handler`
- **调用链**: `ntyco_conn_routine` → `ntyco_process_received_data` → `ntyco_session_handler` → `kvs_handler` → `kvs_filter_protocol`
- **是否绕过 executor**: 是（**所有** ntyco 命令都绕过）
- **是否可能读/写共享状态**: 是
- **是否可能触发 lazy expire**: 是
- **是否受 g_kv_data_rwlock 保护**: 是
- **是否是 rwlock removal blocker**: 是（最大范围的 blocker）
- **后续建议收敛方式**: ntyco 需要完整改造为 async executor 模型，或将 ntyco 标记为 "不支持 rwlock removal" 模式。

### SF-5: ntyco replication command

- **文件**: `src/network/ntyco.c:128-132`
- **函数**: `ntyco_session_handler`
- **调用链**: 同 SF-4，但 `ctx->is_replication = 1`, `sync_context = 2`
- **是否绕过 executor**: 是
- **后续建议收敛方式**: 同 SF-4

### SF-6: Master full sync traversal

- **文件**: `src/distributed/distributed.c:113-196`
- **函数**: `slave_sync_thread`
- **调用链**: `slave_sync_thread` → `pthread_rwlock_rdlock(&g_kv_data_rwlock)` → 遍历 array/rbtree/hash/skiptable → `master_sync_command()` → `pthread_rwlock_unlock`
- **是否绕过 executor**: 是
- **是否可能读共享状态**: 是（只读遍历）
- **是否可能写共享状态**: 否（不修改 store，只发送 sync 消息）
- **是否可能触发 lazy expire**: 否（只调用 `kv_is_expired` 过滤 + `master_sync_command`）
- **是否受 g_kv_data_rwlock 保护**: 是（rdlock）
- **是否是 rwlock removal blocker**: 是（读锁覆盖遍历 + 写并发互斥）
- **后续建议收敛方式**: executor snapshot_task

### SF-7: Slave apply

- **文件**: `src/distributed/distributed.c:567-664`
- **函数**: `slave_connect_master`
- **调用链**: `slave_connect_master` → `pthread_rwlock_wrlock(&g_kv_data_rwlock)` → 直接调用 `kvs_array_set/del/mod`、`kvs_rbtree_set/del/mod`、`ttl_set_any_key_expire_at_ms` → `pthread_rwlock_unlock` → `slave_append_aof_from_op`
- **是否绕过 executor**: 是（直接修改 store，不经过任何 handler）
- **是否可能读共享状态**: 是
- **是否可能写共享状态**: 是（直接 SET/DEL/MOD + TTL 变更）
- **是否可能触发 lazy expire**: 否（不调用 handler，无 TTL 过期检查）
- **是否受 g_kv_data_rwlock 保护**: 是（wrlock）
- **是否是 rwlock removal blocker**: 是
- **后续建议收敛方式**: replicated_command_task → executor

### SF-8: AOF load

- **文件**: `src/persistence/kvs_aof.c:501`
- **函数**: `aof_load`
- **调用链**: `aof_load` → `kvs_filter_protocol(parser.tokens, ...)`
- **是否绕过 executor**: 是（启动期，main 线程）
- **是否可能读/写共享状态**: 是
- **是否可能触发 lazy expire**: 是（如 AOF 中有 GET 命令，但这不常见）
- **是否受 g_kv_data_rwlock 保护**: 否（不获取锁，因为是启动期单线程）
- **是否是 rwlock removal blocker**: 否（启动期路径，网络未启动）
- **后续建议收敛方式**: 如果未来支持在线 AOF replay，需封装为 executor task

### SF-9: RDB save

- **文件**: `src/persistence/kvs_rdb.c:521-529`
- **函数**: `kvs_rdb_save`
- **调用链**: `kvs_rdb_save` → `pthread_rwlock_rdlock(&g_kv_data_rwlock)` → 遍历 KV + vector store → `pthread_rwlock_unlock`
- **是否绕过 executor**: 是
- **是否可能写共享状态**: 否（只读）
- **后续建议收敛方式**: executor snapshot_task

### SF-10: AOF rewrite

- **文件**: `src/persistence/kvs_aof_rewrite.c:690-699`
- **函数**: `aof_rewrite_acquire_store_read_lock`
- **调用链**: `aof_rewrite` → `pthread_rwlock_rdlock(&g_kv_data_rwlock)` → 遍历 KV + vector store → `pthread_rwlock_unlock`
- **是否绕过 executor**: 是
- **后续建议收敛方式**: executor snapshot_task

### SF-11: Shutdown destroy

- **文件**: `src/core/kvstore.c:127`
- **函数**: `dest_kvengine`
- **调用链**: `dest_kvengine` → `pthread_rwlock_wrlock(&g_kv_data_rwlock)` → `kvs_array_destroy` / `kvs_rbtree_destroy` / `kvs_vector_destroy` → `pthread_rwlock_unlock`
- **是否绕过 executor**: 是（shutdown 路径）
- **后续建议收敛方式**: executor drain → destroy 不需要锁

## 是否仍存在绕过 executor 的写路径

**是。** 以下路径在 executor 外部执行写操作：

| 写路径 | 写操作 | 线程 | 频率 |
|--------|--------|------|------|
| Lazy expire (GET/EXIST) - reactor/proactor async | 过期 key 删除 | executor | 常见（已 executor-owned） |
| Lazy expire (GET/EXIST) - ntyco | 过期 key 删除 | ntyco coroutine | ntyco 全量 |
| Lazy expire (TTL/EXPIRE/etc) - ntyco | 过期 key 删除 | ntyco coroutine | ntyco 全量 |
| Reactor repl handler | GET → lazy expire 删除 | reactor epoll | eBPF sync 模式 |
| Proactor repl handler | GET → lazy expire 删除 | io_uring worker | eBPF sync 模式 |
| Proactor fallback | GET → lazy expire 删除 | io_uring worker | OOM 等异常 |
| Slave apply | SET/DEL/MOD/TTL | sync thread | TCP sync 模式 |
| AOF load | 所有命令 | main（启动） | 启动时一次 |

## RWLock Removal Criteria 更新对照

| 条件 | Stage 10.2 审计结论 | 状态 |
|------|---------------------|------|
| 普通 KV handler 不再被网络线程直接调用 | reactor/proactor normal ✅, ntyco ❌, repl ❌, fallback ❌ | 未满足 |
| Vector handler 不再被网络线程直接调用 | 同普通 KV handler | 未满足 |
| TTL active expire 不再直接访问 store | ✅ Stage 10.1 | 已满足 |
| lazy expire 只在 executor 内完成 | reactor/proactor normal async ✅, ntyco ❌, repl ❌ | **部分满足** |
| RDB save 不直接遍历 live store | ❌ | 未满足 |
| AOF rewrite 不直接遍历 live store | ❌ | 未满足 |
| master full sync 不直接遍历 live store | ❌ | 未满足 |
| slave apply 通过 replicated_command_task | ❌ | 未满足 |
| shutdown/destroy 在 executor drain 后执行 | 部分改善（Stage 10.1 调整了顺序） | 部分满足 |

### 新增发现：lazy expire partial ready

"lazy expire 只在 executor 内完成" 条件从 Stage 10 的"未满足"更新为：

- **reactor/proactor normal async path**：✅ **已满足** — lazy expire 完全在 executor 线程内执行
- **ntyco**：❌ 未满足 — 所有命令绕过 executor
- **reactor/proactor replication/REPLSYNC**：❌ 未满足 — eBPF sync 模式绕过
- **proactor OOM fallback**：❌ 未满足 — 异常降级绕过

## BLOCKER 列表

以下列为 rwlock removal 的剩余 blocker（Stage 10.1 后，Stage 10.2 补充审计）：

**原有 Blocker（Stage 10 审计）：**

1. ~~TTL active expire 后台线程直接删除 KV~~ ✅ Stage 10.1 已解决
2. Master full sync 直接遍历 live store → 需 executor snapshot_task
3. Slave apply 直接修改 KV/TTL → 需 replicated_command_task
4. AOF rewrite 直接遍历 live store → 需 executor snapshot_task
5. RDB save 直接遍历 live store → 需 executor snapshot_task

**Stage 10.2 新确认/细化的 Blocker：**

6. **Ntyco 同步 handler**：所有 ntyco 命令（包括 GET/EXIST/TTL）绕过 executor，lazy expire 在 coroutine 中执行
7. **Reactor replication handler**：eBPF sync 模式下 REPLSYNC 后的复制命令绕过 executor
8. **Proactor replication handler**：同上
9. **Proactor OOM fallback**：异步 task 构建失败时的同步降级绕过 executor

## Non-Blocking Issue 列表

1. **Ntyco 路径的 lazy expire 语义正确性**：虽然绕过 executor，但因 `g_kv_data_rwlock` 仍存在，lazy expire 删除仍受 rdlock/wrlock 保护，当前不构成正确性问题。只在 rwlock removal 时成为 blocker。

2. **Proactor OOM fallback 不常见**：此路径仅在内存分配失败时触发。其 lazy expire 行为与正常路径一致（rdlock → wrlock 升级受锁保护），但绕过了 executor。可在 OOM 场景下直接返回错误而非 fallback。

3. **Reactor/proactor replication handler 仅在 eBPF 同步模式下触发**：TCP 同步模式不走此路径。

4. **AOF load 中的 lazy expire**：AOF 文件中极少包含 GET/EXIST 命令（通常只有写命令），即使触发，也是单线程启动路径。

5. **VGET/VSEARCH 不触发 lazy expire**：vector 命令不检查 normal KV TTL，语义正确。

6. **Lazy expire 与 Stage 10.1 的 DEL side-effect 一致性**：lazy expire（通过 `ttl_target_apply_expired_side_effects`）和 Stage 10.1 active expire（通过同一函数）使用相同的删除路径：`ttl_target_delete` + `ttl_sync_delete_if_needed`。两者都不写 AOF。语义一致。

7. **Lazy expire 在 executor 内时持有全局锁**：即使是 executor-owned 路径，lazy expire 的读锁升级写锁操作仍需要 `g_kv_data_rwlock`。如果未来要移除该锁，需要确保 lazy expire 删除是 executor 内的唯一写者——当前 reactor/proactor normal async 已满足此条件。

## Tests 结果

测试脚本 `tests/route_c/lazy_expire_stage10_2.sh` 覆盖：

| 测试用例 | 覆盖项 | 结果 |
|----------|--------|------|
| `basic_lazy_expire()` | GET/EXIST 触发 lazy expire，TTL 返回 -2，DEL 返回 NO EXIST | ✅ PASSED |
| `lazy_expire_new_value_protection()` | SET new after expire + GET 不删 new；PERSIST after expire | ✅ PASSED |
| `lazy_expire_ttl_commands()` | TTL/EXPIRE/PEXPIREAT on expired key 的删除行为 | ✅ PASSED |
| `lazy_plus_active_coexistence()` | 40 active + 20 lazy keys，验证两者共存 | ✅ PASSED |
| `concurrent_lazy_expire()` | 4 线程混合 SET/EXPIRE/GET/EXIST/DEL | ✅ PASSED |
| `vector_with_lazy_expire()` | VSET/VSEARCH with worker pool threads=2 + lazy expire GET | ✅ PASSED |
| `run_shutdown_case()` | 4 线程并发写入 + QUIT 优雅关闭 | ✅ PASSED |
| `verify_stats()` | queue_depth 归零检查 | ✅ PASSED |

运行：
```bash
bash -n tests/route_c/lazy_expire_stage10_2.sh: passed
tests/route_c/lazy_expire_stage10_2.sh: passed (reactor + proactor)
```

## Sanitizer 结果

### ASAN/UBSAN

```bash
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=address,undefined" \
make embedding ENABLE_EBPF=0

ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
tests/route_c/lazy_expire_stage10_2.sh
```

### TSAN

```bash
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=thread -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=thread" \
make embedding ENABLE_EBPF=0

TSAN_OPTIONS="halt_on_error=0 exitcode=0" \
tests/route_c/lazy_expire_stage10_2.sh
```

预期：无新 sanitizer 问题。仅有既有的 4 个 background/shutdown race。

## 回归验证

```bash
tests/route_c/ttl_executor_stage10_1.sh: passed
tests/embedding_worker_pool_stage9.sh: passed
tests/embedding_worker_pool_stage9_1.sh: passed
```

## 下一阶段建议

建议进入 **Stage 10.3：Slave Apply replicated_command_task**，将 slave apply 从直接 store 修改改为通过 replicated_command_task 进入 executor。这是第二个最高优先级的去锁 blocker（仅次于已解决的 TTL active expire）。

后续优先级排序：

1. Stage 10.3: Slave apply replicated_command_task
2. Stage 10.4: Ntyco async executor 改造或 ntyco 模式标记为 "no-rwlock-removal"
3. Stage 10.5: Master full sync snapshot_task
4. Stage 10.6: RDB save snapshot_task
5. Stage 10.7: AOF rewrite snapshot_task
6. Stage 10.8: Shutdown/destroy 收敛 + 最终 rwlock removal assessment
