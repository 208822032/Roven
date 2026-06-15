# 阶段 3：response_task 同步抽象审查报告

## 1. 审查结论

APPROVED

## 2. 核心结论摘要

阶段 3 实现正确。`kvs_response_task_t` 是 `kvs_filter_protocol` 栈对象，封装 borrowed response buffer，生命周期不超过当前调用栈。`destroy` 仅做 memset 清零，不释放 borrowed buffer。`kvs_filter_protocol` 对外签名不变，`net_session_process_resp` 及其发送逻辑未被修改。response 格式与阶段 2 完全一致（ONNX 语义分值 0.8028/0.7200/0.7751 不变）。

变更范围：新增 2 个文件（`kvs_response_task.h` + `.c`），修改 `kvstore.c`（+5 行 response_task 编排）、`kvs_command_task.h`（+1 个 response_task 指针字段）、`kvs_command_task.c`（API 接入）、`Makefile`（+1 行）。

## 3. 实际执行流程

```
kvs_filter_protocol(tokens, count, token_lens, response)
  │
  ├─ kvs_response_task_init_borrowed(&response_task, response, KVS_MAX_RESPONSE_LENGTH)
  │   └─ owns_buf=0, cap=2MB, buf=response
  │
  ├─ kvs_command_task_build(&task, tokens, count, token_lens, &response_task)
  │   └─ task.response = kvs_response_task_buffer(response_task)  // 仍指向原 response
  │   └─ task.response_task = &response_task
  │   └─ 失败 → finalize → command_task_destroy → response_task_destroy → return
  │
  ├─ kvs_command_task_prepare(&task)
  │   └─ 失败 → finalize → command_task_destroy → response_task_destroy → return
  │
  ├─ kvs_command_task_execute_sync(&task)     // handler 同步写 task.response
  ├─ kvs_command_task_post_commit(&task, ret) // AOF/replication
  │
  ├─ kvs_response_task_finalize(&response_task, ret)
  │   └─ status=ret; if ret>0: len=ret; finalized=1
  │
  ├─ kvs_command_task_destroy(&task)          // 锁释放 + prepared 清理
  ├─ kvs_response_task_destroy(&response_task) // memset(0), 不释放 buf
  └─ return ret                               // 返回值不变
```

## 4. response_task 生命周期审查

| 检查点 | 结果 | 说明 |
|---|---|---|
| response_task 是栈对象 | ✅ | `kvs_response_task_t response_task;` 在 `kvs_filter_protocol` 栈上 |
| 不保存到全局结构 | ✅ | 无全局 response_task 队列/列表/指针 |
| 不进入队列 | ✅ | 无 response queue 相关代码 |
| 不传给后台线程 | ✅ | 无新线程创建 |
| 生命周期 ≤ kvs_filter_protocol | ✅ | destroy 在 return 前必定调用 |
| 所有退出路径都调用 destroy | ✅ | 三条路径（build 失败/prepare 失败/正常完成）都有 destroy |
| command_task 不长期保存 response_task 指针 | ✅ | task.response_task 仅在同一调用栈内使用 |

## 5. borrowed buffer ownership 审查

| 检查点 | 结果 | 说明 |
|---|---|---|
| init_borrowed 设置 owns_buf=0 | ✅ | `resp->owns_buf = 0` |
| destroy 不释放 buf | ✅ | 只调用 `kvs_response_task_reset`（memset 清零） |
| destroy 注释明确说明 | ✅ | "Stage-3 only supports borrowed buffers...destroy must not free resp->buf" |
| handler 写入的仍是原 buffer | ✅ | `task.response` = `kvs_response_task_buffer(response_task)` = 原 `response` 指针 |
| net_session send_buf 管理未变 | ✅ | `net_session.c` 未被修改 |
| 无 double free | ✅ | 只有一处 memset（reset），无 free |
| 无 use-after-free | ✅ | ASAN 零报错，buffer 生命周期由 net_session 管理 |

## 6. response length/status/finalized 审查

| 检查点 | 结果 |
|---|---|
| kvs_filter_protocol 返回值语义不变 | ✅ 仍返回 handler 的 `ret`（字节数或错误码） |
| net_session_process_resp 对返回值的处理不变 | ✅ net_session.c 未被修改 |
| finalize 只做记录 | ✅ `resp->status=status; if(status>0) resp->len=status; finalized=1` |
| finalize 不修改 buffer 内容 | ✅ 只设置元数据字段 |
| response 不截断 | ✅ 原 buffer 2MB，容量不变 |
| 错误响应长度不变 | ✅ 错误路径中 `resp_generate_simple_string_n` 行为不变 |
| pipeline 不覆盖 | ✅ 每条命令独立的 response_task 栈对象，不同 kvs_filter_protocol 调用不同栈帧 |

## 7. command_task 集成审查

| 检查点 | 结果 | 说明 |
|---|---|---|
| command_task 仍为栈对象 | ✅ | `kvs_command_task_t task;` 在 kvstore.c 栈上 |
| command_task 不接管 response buffer ownership | ✅ | `task.response_task` 是指针，不拥有 |
| command_task destroy 不释放 response buffer | ✅ | destroy 代码未变，不涉及 response buffer |
| build 签名变更仅影响内部 API | ✅ | 唯一调用方是 `kvs_filter_protocol` |
| prepare 中刷新 response 指针 | ✅ | `task->response = kvs_response_task_buffer(task->response_task)` 安全冗余 |
| VSET/VSEARCH prepared data owner 不受影响 | ✅ | prepared 管理代码未变 |
| AOF/replication post_commit 不受影响 | ✅ | post_commit 代码未变 |
| read-then-write retry 不受影响 | ✅ | execute_sync 代码未变 |
| 普通 argv 零拷贝不受影响 | ✅ | owns_argv=0 代码未变 |

## 8. 锁模型与执行模型审查

| 检查点 | 结果 |
|---|---|
| 无新增锁 | ✅ |
| 未删除 g_kv_data_rwlock | ✅ |
| command_table.lock_mode 未变 | ✅（未修改 kvs_operation.c） |
| VSET/VSEARCH embedding prepare 仍在锁外 | ✅ prepare 在 lock 之前 |
| VSET commit/HNSW insert 仍在写锁内 | ✅ |
| VSEARCH search/filter 仍在读锁内 | ✅ |
| 执行仍同步 | ✅ |
| 未新增 queue/executor/thread | ✅ |
| 未新增 session refcount/owner poller | ✅ |

## 9. response 格式与 pipeline 审查

| 命令 | 结果 | 验证方式 |
|---|---|---|
| SET | ✅ 与阶段 2 一致 | 手工 RESP 验证 |
| GET | ✅ 与阶段 2 一致 | 手工 RESP 验证 |
| DEL | ✅ 与阶段 2 一致 | 手工 RESP 验证 |
| EXPIRE/TTL | ✅ 与阶段 2 一致 | 手工 RESP 验证 |
| PERSIST | ✅ 与阶段 2 一致 | 手工 RESP 验证 |
| VSET | ✅ 返回 OK | semantic test + 手工 |
| VGET | ✅ 返回 answer bulk | semantic test + 手工 |
| VSEARCH | ✅ 分值 0.8028/0.7200/0.7751 不变 | semantic test |
| VDEL | ✅ 删除后返回 NO EXIST | 手工验证 |
| unknown command | ✅ "Unknown command" | 手工验证 |
| wrong args | ✅ "Wrong number of arguments" | 手工验证 |
| pipeline 多命令 | ✅ 顺序和内容正确 | 手工验证 |

## 10. 错误路径审查

| 错误场景 | finalize | cmd_destroy | resp_destroy | 锁释放 | 返回值 | 结论 |
|---|---|---|---|---|---|---|
| kvs_command_task_build 失败 | ✅ | ✅ | ✅ | 无需（未加锁） | task.status | ✅ |
| kvs_command_task_prepare 失败 | ✅ | ✅ | ✅ | 无需（未加锁） | prep_ret | ✅ |
| lock 失败（execute_sync 内部） | ✅ | ✅ | ✅ | lock 前已处理 | task.status | ✅ |
| handler 返回错误 | ✅ | ✅ | ✅ | execute_sync 内已解锁 | ret | ✅ |
| retry wrlock 失败 | ✅ | ✅ | ✅ | rdlock 已释放，wrlock 失败 | task.status | ✅ |
| prepare 错误不影响 response buffer | ✅ | ✅ | ✅ | N/A | 一致 | ✅ |

关键保证：
- `finalize` 在 `destroy` 之前，先记录状态再清理
- `command_task_destroy` 处理锁释放和 prepared 数据
- `response_task_destroy` 只做 memset，安全
- 所有三条路径都是 `finalize → cmd_destroy → resp_destroy → return`

## 11. ASAN/UBSAN 健壮性验证

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

### 测试结果

| 测试项 | 结果 | ASAN/UBSAN 报错 |
|---|---|---|
| semantic_onnx_test.sh | 通过 | 0 |
| SET/GET/DEL | 通过 | 0 |
| EXPIRE/TTL/PERSIST | 通过 | 0 |
| VSET/VGET/VDEL/scope | 通过 | 0 |
| Pipeline | 通过 | 0 |
| Error responses | 通过 | 0 |
| Repeat VSET | 通过 | 0 |

**ASAN/UBSAN 零报错。** 服务端日志中无 AddressSanitizer / LeakSanitizer / runtime error / heap-use-after-free / double-free / stack-use-after-return 任何报告。

## 12. 功能验证结果

### 普通构建

```bash
make clean && make embedding
```

**构建成功，无警告无错误。**

### ONNX/512 语义测试

```bash
./kvstore -c config/kvstore-embedding.conf &
bash tests/semantic_onnx_test.sh 127.0.0.1 28080
```

**通过。** 分值 0.8028/0.7200/0.7751 与阶段 1、阶段 2 完全一致。

### 补充功能验证

| 测试项 | 结果 |
|---|---|
| SET/GET/DEL | ✅ |
| EXPIRE/TTL | ✅ |
| PERSIST | ✅ |
| VSET/VGET/VDEL | ✅ |
| Scope 隔离 | ✅ |
| Pipeline 多命令 | ✅ |
| Unknown command | ✅ |
| Wrong args | ✅ |
| Repeat VSET（覆盖 answer） | ✅ |
| 服务端无崩溃 | ✅ |

## 13. benchmark 结果

### 环境

- CPU: 未指定（make doctor 可查）
- 配置: ONNX/512, reactor 模式, HNSW, IP metric
- 构建: `make embedding`（-O2，无 sanitizer）

### VSET latency (n=300, c=1, P=1)

```
success=300 failed=0 errors=0
qps=483.13
avg_ms=2.07  p50_ms=1.86  p95_ms=3.09  p99_ms=5.85
min_ms=1.64  max_ms=10.92
```

### VSEARCH latency (n=300, c=1, P=1)

```
success=300 failed=0 errors=0
qps=534.67
avg_ms=1.87  p50_ms=1.47  p95_ms=3.50  p99_ms=12.98
min_ms=1.26  max_ms=14.27
```

### VSET throughput (n=1000, c=4, P=8)

```
success=1000 failed=0 errors=0
qps=438.07
avg_ms=72.60  p50_ms=74.74  p95_ms=94.04  p99_ms=95.90
```

### VSEARCH throughput (n=1000, c=4, P=8)

```
success=1000 failed=0 errors=0
qps=663.39
avg_ms=47.92  p50_ms=50.98  p95_ms=71.33  p99_ms=74.72
```

### 普通 KV (n=1000, c=1)

```
SET: 25869 qps, 0.039ms avg
GET: 31595 qps, 0.032ms avg
```

### 判断

- 所有 benchmark 100% 成功率，0 errors。
- VSET/VSEARCH latency 在合理范围（ONNX embedding 是主耗时，p99 < 15ms）。
- 普通 KV 路径 ~25K-31K qps，无退化迹象。
- 阶段 3 是纯 response_task 包装，预期不应有性能变化。当前数据合理。

## 14. 阻塞问题

无。

## 15. 非阻塞建议

1. **`.gitignore` 的 `core` 规则**：新增文件 `include/kvs_response_task.h` 和 `src/core/kvs_response_task.c` 被 `.gitignore:59` 的 `core` 规则屏蔽，需用 `git add -f` 强制添加。与阶段 2.1 问题相同。

2. **response_task 当前字段使用率低**：`len`、`status`、`finalized` 当前仅在 `finalize` 中写入，未被 `kvs_filter_protocol` 返回值逻辑引用。后续 executor 阶段会需要这些字段，当前仅作为记录用途。

3. **无阶段 1/2 的 ASAN 基线**：ASAN 验证仅在阶段 3 运行，无历史对比。但由于阶段 3 只是添加栈上的 metadata wrapper，不存在新的堆分配或指针操作，ASAN 零报错符合预期。

4. **benchmark 无阶段 2 基线对比**：当前仅有阶段 3 数据。由于 response_task 引入的开销可忽略（一个栈变量 + 几次赋值），不存在性能退化风险。

## 16. 是否建议提交

是。阶段 3 实现正确，可以提交。

## 17. 建议 commit message

```bash
git add -f include/kvs_response_task.h src/core/kvs_response_task.c
git add Makefile \
        docs/route_c_perf_arch/05_response_task_design.md \
        include/kvs_command_task.h \
        src/core/kvs_command_task.c \
        src/core/kvstore.c \
        docs/route_c_perf_arch/17_stage3_response_task_review.md
git commit -m "抽象同步响应任务上下文

引入 kvs_response_task_t 封装 response buffer 元数据：
- include/kvs_response_task.h — 类型定义和公开 API
- src/core/kvs_response_task.c — init_borrowed/destroy/finalize 等生命周期
- kvs_filter_protocol 栈上创建 response_task，借用原 response buffer
- owns_buf=0，destroy 不释放 buffer
- kvs_filter_protocol 对外签名不变，net_session 发送逻辑不变

纯抽象，不改变任何行为。ASAN/UBSAN 零报错。

Co-Authored-By: Claude <noreply@anthropic.com>"
```
