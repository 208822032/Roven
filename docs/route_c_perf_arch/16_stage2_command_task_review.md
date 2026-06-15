# 阶段 2：command_task 同步抽象审查报告

## 1. 审查结论

APPROVED

## 2. 核心结论摘要

阶段 2 实现正确。`kvs_command_task_t` 是栈对象，在整个 `kvs_filter_protocol` 调用栈内完成 build → prepare → execute_sync → post_commit → destroy 生命周期。执行仍完全同步，未引入队列/executor/异步 response。锁模型、argv 生命周期、AOF/replication 后置语义均完全保持与改造前一致。

变更范围精确：仅 `src/core/kvstore.c`（+320/-148）和设计文档 `04_command_task_design.md`（+9 行）。

## 3. 实际执行流程

```
kvs_filter_protocol(tokens, count, token_lens, response)
  │
  ├─ kvs_command_task_build(&task, ...)       // 栈对象，解析命令，确定锁模式
  │   └─ 失败 → kvs_command_task_destroy → return
  │
  ├─ kvs_command_task_prepare(&task)           // 锁外：VSET/VSEARCH embedding prepare
  │   └─ 失败 → kvs_command_task_destroy → return
  │
  ├─ kvs_command_task_execute_sync(&task)      // 加锁 → handler → 解锁
  │   ├─ kvs_command_task_lock                // rdlock/wrlock
  │   ├─ kvs_command_task_execute_handler     // VSET commit / VSEARCH search / 普通 handler
  │   ├─ [retry] rdlock→unlock→wrlock→handler  // KVS_CMD_RETRY_WITH_WRITE_LOCK
  │   ├─ kvs_command_task_capture_post_state  // EXPIRE/PEXPIREAT/PERSIST 状态快照
  │   └─ pthread_rwlock_unlock
  │
  ├─ kvs_command_task_post_commit(&task)       // 锁外：AOF append + TCP replication
  │
  └─ kvs_command_task_destroy(&task)           // prepared data 清理，保险解锁
```

## 4. command_task 生命周期审查

| 检查点 | 结果 | 说明 |
|---|---|---|
| task 是栈对象 | ✅ | `kvs_command_task_t task;` 在 `kvs_filter_protocol` 栈上 |
| 不保存到全局结构 | ✅ | 无全局 task 队列/列表 |
| 不逃逸调用栈 | ✅ | task 地址未传递给任何异步回调或后台线程 |
| 生命周期 ≤ kvs_filter_protocol | ✅ | destroy 在 return 前必定调用 |
| 未引入 command queue | ✅ | 无队列数据结构 |
| 未引入 executor thread | ✅ | 无 executor 相关代码 |
| 未引入异步 response | ✅ | response 仍同步写入 response_buf |

## 5. argv 生命周期审查

### 普通命令（GET/SET/DEL/EXPIRE/...）

- `task->argv = tokens` — 指向 RESP parser 的 `recv_buf` 内部 token 数组
- `task->owns_argv = 0` — 明确标记不拥有 argv
- `kvs_command_task_destroy` 不释放 argv
- argv 仅在 `kvs_filter_protocol` 调用栈内被 handler 和 post_commit 引用
- post_commit 中 `aof_append(task->argc, task->argv, ...)` 和 `master_sync_command(task->argv[0], ...)` 都在 destroy 之前调用

✅ **普通命令 argv 零拷贝，不逃逸调用栈。**

### VSET/VSEARCH

- question/answer/scope/query 在 prepare 阶段深拷贝（`kvs_vector_set_prepare` / `kvs_vector_search_prepare` 内部）
- 深拷贝后的字符串由 prepared 结构管理
- 不依赖 session recv_buf

✅ **VSET/VSEARCH prepared 数据深拷贝，独立于 recv_buf。**

## 6. prepared data owner 审查

| 场景 | 行为 | 结果 |
|---|---|---|
| VSET prepare 成功 | prepared_set 挂到 task | ✅ `task->prepared = &task->prepared_set` |
| VSEARCH prepare 成功 | prepared_search 挂到 task | ✅ `task->prepared = &task->prepared_search` |
| 普通命令 | prepared_kind = NONE | ✅ 无 prepared 数据 |
| VSET commit 新建 record | record 接管 prepared 字段后置空 | ✅ `kvs_vector_record_take_prepared_fields` |
| VSET commit 重复 question+scope | 释放未使用 embedding | ✅ `kvs_vector_set_commit_prepared` 内部处理 |
| VSEARCH 完成后 | destroy 释放 query embedding | ✅ `kvs_vector_search_prepared_destroy` |
| prepare 失败 | destroy 释放部分分配的资源 | ✅ prepare 函数内部已做清理 + destroy 再做安全释放 |
| task destroy | 统一释放 prepared | ✅ 根据 `prepared_kind` 调用对应 destroy |

✅ **prepared data owner 清晰，无泄漏路径，无 double free。**

## 7. 锁模型审查

| 检查点 | 结果 |
|---|---|
| command_table.lock_mode 未修改 | ✅ VSET=WRITE, VSEARCH=READ, VGET=READ, VDEL=WRITE, VINDEX=WRITE, VINFO=READ |
| GET/EXIST 仍按 command_table 锁模式 | ✅ 通过 `task->cmd->lock_mode` 驱动 |
| LOCK_NONE 命令不加锁 | ✅ `kvs_command_task_lock` 中无对应分支，直接返回 0 |
| VSET embedding prepare 在锁外 | ✅ `kvs_command_task_prepare` 在 `kvs_command_task_lock` 之前调用 |
| VSEARCH embedding prepare 在锁外 | ✅ 同上 |
| VSET commit (HNSW insert) 在写锁内 | ✅ `kvs_command_task_execute_handler` 在 `kvs_command_task_lock` 之后调用 |
| VSEARCH search (HNSW search) 在读锁内 | ✅ 同上 |
| 无普通写命令绕过写锁 | ✅ 所有命令走同一 lock 路径 |
| 无普通读命令失去读锁 | ✅ 同上 |
| KVS_CMD_RETRY_WITH_WRITE_LOCK 仍可用 | ✅ 详见第 8 节 |

✅ **锁模型完全不变。**

## 8. read-then-write retry 审查

原逻辑：
```
读锁 → handler 返回 KVS_CMD_RETRY_WITH_WRITE_LOCK → 释放读锁 → 写锁 → 重试 handler
```

新实现（`kvs_command_task_execute_sync`）：

```c
ret = kvs_command_task_execute_handler(task);
if (task->use_read_lock && ret == KVS_CMD_RETRY_WITH_WRITE_LOCK) {
    pthread_rwlock_unlock(&g_kv_data_rwlock);
    task->lock_held = 0;          // 标记锁已释放
    task->use_read_lock = 0;
    task->use_write_lock = 1;

    if (pthread_rwlock_wrlock(&g_kv_data_rwlock) != 0) {
        task->status = ...;
        return task->status;      // 失败返回，lock_held=0，destroy 不会 unlock
    }
    task->lock_held = 1;          // 标记持有写锁
    task->ctx.lock_mode = KVS_LOCK_MODE_WRITE;
    ret = task->cmd->handler(&task->ctx, task->response);
    task->command_committed = (ret >= 0);  // 重试结果覆盖
}
```

| 检查点 | 结果 |
|---|---|
| retry 触发条件正确 | ✅ `use_read_lock && ret == KVS_CMD_RETRY_WITH_WRITE_LOCK`（值为 -3） |
| retry 前释放读锁 | ✅ `pthread_rwlock_unlock` + `lock_held = 0` |
| retry 后加写锁 | ✅ `pthread_rwlock_wrlock` + `lock_held = 1` |
| lock_mode 更新 | ✅ `ctx.lock_mode = KVS_LOCK_MODE_WRITE` |
| command_committed 被 retry 结果覆盖 | ✅ 第一轮设为 0（因 -3 < 0），第二轮由 handler 返回值重设 |
| retry 不会重复 prepare VSET/VSEARCH | ✅ VSET/VSEARCH 不走 retry（write_lock 命令无 use_read_lock） |
| retry 后 response 不重复写 | ✅ handler 直接写入 response_buf，第二次覆盖第一次 |
| retry 后 lock/unlock 成对 | ✅ execute_sync 尾部统一 unlock（lock_held=1 → unlock → lock_held=0） |
| retry wrlock 失败路径安全 | ✅ 返回前 lock_held=0，destroy 不会 double-unlock |

✅ **retry 机制完全保留，语义不变。**

## 9. AOF/replication 后置逻辑审查

| 检查点 | 结果 |
|---|---|
| AOF append 仅在写命令成功后执行 | ✅ `post_commit` 检查 `command_committed && write_command` |
| TCP replication 仅在写命令成功后执行 | ✅ 同上 |
| VSET prepare 阶段不触发 AOF/replication | ✅ prepare 在 lock 之前，不设置 command_committed |
| VSET commit 成功后 AOF/replication 行为保持 | ✅ commit 成功 → command_committed=1 → post_commit 执行 |
| VSEARCH 不触发 AOF/replication | ✅ VSEARCH 不是 write_command，post_commit 直接 return |
| 命令失败不触发 AOF/replication | ✅ command_committed=0 → post_commit 直接 return |
| AOF append 内容来自原始命令 | ✅ `aof_append(task->argc, task->argv, task->argv_lens)` |
| EXPIRE/PEXPIREAT→PEXPIREAT/DEL 转换保留 | ✅ `capture_post_state` + `post_commit` 中保留 |
| PERSIST 同步保留 | ✅ `need_persist_sync` 逻辑保留 |
| replication 内容不变 | ✅ `master_sync_command(task->argv[0], task->argv[1], ...)` |

✅ **AOF/replication 后置逻辑完全不变。**

## 10. 错误路径审查

| 错误场景 | lock_held | prepared 清理 | 响应格式 | 结论 |
|---|---|---|---|---|
| tokens[0] == NULL | 0（未加锁） | NONE | 原格式 | ✅ |
| Unknown command | 0 | NONE | 原格式 | ✅ |
| Wrong number of arguments | 0 | NONE | 原格式 | ✅ |
| SLAVE READ ONLY | 0 | NONE | 原格式 | ✅ |
| Invalid arguments (parse) | 0 | NONE | 原格式 | ✅ |
| VSET prepare 失败 | 0 | prepared_set destroy | 原格式 | ✅ |
| VSEARCH prepare 失败 | 0 | prepared_search destroy | 原格式 | ✅ |
| rdlock/wrlock 失败 | 0（未设置 lock_held） | NONE/VSET/VSEARCH destroy | 原格式 | ✅ |
| handler 返回错误 | 1 → execute_sync 解锁 | destroy 处理 | 原格式 | ✅ |
| retry wrlock 失败 | 0（已释放 rdlock，wrlock 失败未设置） | destroy 处理 | 原格式 | ✅ |
| VSET commit 失败 | 1 → execute_sync 解锁 | destroy 处理（已接管字段被释放） | 原格式 | ✅ |
| VSEARCH search 失败 | 1 → execute_sync 解锁 | destroy 处理 | 原格式 | ✅ |
| AOF append 失败 | 0（已解锁） | destroy 处理 | 不修改响应 | ✅ |
| TCP replication 失败 | 0（已解锁） | destroy 处理 | 不修改响应 | ✅ |

关键保证：
- `kvs_command_task_destroy` 在所有路径被调用（`kvs_filter_protocol` 中每个 return 前都有 destroy）
- `lock_held` 精确跟踪锁状态，destroy 中保险解锁防止锁泄漏
- `prepared_kind` 精确跟踪 prepared 类型，destroy 中按类型释放

✅ **所有错误路径锁释放和内存释放正确。**

## 11. 性能影响审查

| 检查点 | 结果 |
|---|---|
| 普通命令无深拷贝 argv | ✅ `owns_argv = 0`，handler 仍引用 recv_buf token |
| 普通命令无新增 malloc/free | ✅ task 是栈变量，build/reset 是 memset + 赋值 |
| VSET/VSEARCH 无重复 embedding 生成 | ✅ prepare 只执行一次，retry 不适用于 VSET/VSEARCH |
| task 是栈对象 | ✅ `kvs_command_task_t task;` (~数百字节栈空间) |
| 无新增线程同步开销 | ✅ 无新锁、无原子操作、无 barrier |
| 命令分发路径开销 | ✅ 原 inline 逻辑拆分为函数调用，编译器内联后应与原代码等效 |

✅ **性能影响可忽略。普通 GET/SET 路径无额外堆分配。**

## 12. 测试结果

### 构建

```bash
make clean && make embedding
```

**通过**。无警告，无错误。

### ONNX/512 语义测试

```bash
./kvstore -c config/kvstore-embedding.conf &
bash tests/semantic_onnx_test.sh 127.0.0.1 28080
```

**通过**。分值（0.8028, 0.7200, 0.7751）与阶段 1 完全一致，确认 RESP 输出未变。

### 补充验证

| 测试项 | 结果 |
|---|---|
| VSET + VGET | ✅ |
| VSEARCH 语义命中（3 条正向 + 5 阈值） | ✅ |
| 负样本（无关 query）低分过滤 | ✅ |
| VDEL 后 VGET 返回 NO EXIST | ✅ |
| scope 隔离（scope_A VDEL 不影响 scope_B） | ✅ |
| 普通 KV SET/GET/DEL | ✅ |
| TTL EXPIRE/TTL | ✅ |
| Pipeline 多命令 | ✅ |
| Unknown command 错误响应 | ✅ |
| Wrong args 错误响应 | ✅ |
| 服务端无崩溃 | ✅ |

## 13. 阻塞问题

无。

## 14. 非阻塞建议

1. **后续补 ASAN/valgrind 验证**：当前 review 确认代码路径正确，但 prepared data 的接管/释放细节建议后续用 ASAN 确认无 heap-use-after-free。
2. **`command_committed` 命名**：当前 VSEARCH 也设置 `command_committed = (ret >= 0)`，但 VSEARCH 不是写命令，`post_commit` 中有 `write_command` 守卫所以安全。建议后续在 `execute_handler` 中只对写命令设置该标志，或改用更精确的命名如 `handler_ok`。
3. **`capture_post_state` 调用时机**：当前在 `execute_sync` 内锁释放前调用。这与改造前一致，但如果后续 executor 化时锁边界变化，需要注意该函数读取 store 状态（`ttl_get_any_key_expire_at_ms`）。
4. **doc/docs 目录**：当前统一为 `docs/route_c_perf_arch/`，但阶段 0 审批报告（`14_route_c_design_review.md`）中提到 `doc/` 路径。建议后续统一。

## 15. 是否建议提交

是。阶段 2 实现正确，可以提交。

## 16. 建议 commit message

```bash
git add docs/route_c_perf_arch/04_command_task_design.md \
        docs/route_c_perf_arch/16_stage2_command_task_review.md \
        src/core/kvstore.c
git commit -m "抽象同步命令任务上下文

引入 kvs_command_task_t 作为命令上下文容器，将 kvs_filter_protocol
重构为 build/prepare/execute_sync/post_commit/destroy 五阶段。

关键约束：
- task 为栈对象，不逃逸当前调用栈
- 执行仍完全同步，无队列/executor/异步
- 普通命令 argv 零拷贝，owns_argv=0
- VSET/VSEARCH prepared data 由 task 统一管理
- 锁模型、AOF/replication 后置语义完全不变

Co-Authored-By: Claude <noreply@anthropic.com>"
```
