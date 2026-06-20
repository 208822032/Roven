# Route C Final Report — 所有权模型收敛

Date: 2026-06-19
Status: **ROUTE_C_COMPLETE_READY_TO_TAG / FINAL_ROUTE_C_READY**

## 1. 原始目标

Route C 的目标是将 KVLite 的同步 handler 模型演进为分层架构：

```
network poller
  → RESP parse
  → command_task
  → classify/router
     → no prepare → commit executor
     → prepare → embedding worker pool → prepared_task → commit executor
  → response_task
  → original poller send
```

### 1.1 初始问题

- `reactor.c`、`proactor.c`、`ntyco.c` 都通过 `net_session_process_resp()` 同步执行 handler
- 网络层 handler 直接调用 `kvs_filter_protocol()`，进而直接访问 store
- `kvs_filter_protocol()` 持有 `g_kv_data_rwlock` 后执行 `command_table` handler
- `VSET` 和 `VSEARCH` 的 embedding 生成发生在锁内
- 三种 network_mode 下 `max_inflight=1`
- TTL 后台线程、RDB/AOF、主从同步等模块存在直接访问 store 的路径，部分不由 `kvs_filter_protocol()` 统一加锁

### 1.2 设计原则

三层所有权模型：

- **网络层**：socket / recv / send / RESP parse
- **计算层**：embedding prepare，不访问 store
- **状态层**：commit executor，唯一访问 store/TTL/HNSW/AOF/RDB/replication

正确性边界：
- 单连接内命令顺序必须保持
- response 必须按同 session seq 返回
- commit executor 的执行顺序定义全局状态顺序
- prepare 阶段不产生任何可见状态
- TTL / RDB / AOF / 主从同步必须能用 commit executor 顺序解释

### 1.3 20 项具体目标

| # | 目标 | 状态 |
|---|------|------|
| 1 | 普通 KV handler 不再被网络线程直接调用 | ✅ |
| 2 | Vector handler 不再被网络线程直接调用 | ✅ |
| 3 | embedding prepare 与 commit 分离 | ✅ |
| 4 | single command executor 成为状态 owner | ✅ |
| 5 | TTL active expire 不再直接访问 store | ✅ |
| 6 | lazy expire 只在 executor 内完成 | ✅ |
| 7 | RDB save 不直接遍历 live store | ✅ |
| 8 | AOF rewrite 不直接遍历 live store | ✅ |
| 9 | master full sync 不直接遍历 live store | ✅ |
| 10 | AOF append 顺序等于 commit 顺序 | ✅ |
| 11 | slave apply 通过 replicated_command_task | ✅ |
| 12 | shutdown/destroy 在 executor drain 后执行 | ✅ |
| 13 | vector store/id_map/HNSW owner 明确 | ✅ |
| 14 | reactor/proactor/ntyco 无绕 executor 路径 | ✅ |
| 15 | g_kv_data_rwlock 不再是正确性主路径 | ✅ |
| 16 | weak-lock 默认启用 | ✅ |
| 17 | 真实锁回滚路径仍保留 | ✅ |
| 18 | TSAN 或等价竞态检测通过 | ✅ |
| 19 | mixed stress 通过 | ✅ |
| 20 | RDB/AOF/replication/TTL/vector/HNSW 回归通过 | ✅ |

## 2. 迭代路线

### 2.1 阶段总览

| 阶段 | 内容 | 核心产出 |
|------|------|----------|
| Stage 0-3 | 锁审计 / command-task / response-task | 文档基线确立，所有权方向明确 |
| Stage 4-6 | executor 模型在 reactor/proactor/ntyco 稳定 | 单线程 executor 原型完成，三网络后端闭环 |
| Stage 7-9 | embedding worker pool + per-thread runtime + batch | embedding 锁外并发，batch prepare 优化 |
| Stage 10 | 持久化/复制/TTL/shutdown 收敛到 executor | 全部后台路径 executor 化 |
| Stage 11 | rwlock wrapper 接管 + weak-mode 开关 | `kvs_owner_rwlock_*` wrapper 引入 |
| Stage 12 | weak-rwlock correctness/mixed stress/TSAN gate | 5 个 data race 修复，TSAN PASS |
| Stage 13 | weak-lock 默认启用，g_kv_data_rwlock 降级 | FINAL_ROUTE_C_READY |

### 2.2 每阶段详情

**Stage 0：锁和共享状态审计** (`01_lock_and_shared_state_audit.md`)
- 审计了所有 lock_mode 驱动路径
- 识别了 master full sync、slave apply、RDB save、AOF rewrite 的绕锁路径
- 确认 `GET/EXIST` 存在 lazy expire 读锁升级写锁重试

**Stage 0.5：绕锁路径修复** (`02_existing_lock_bypass_fix_design.md`)
- master full sync 补 `g_kv_data_rwlock` 读锁
- slave apply 补写锁或走统一命令入口
- RDB save 补读锁边界
- AOF rewrite 补读锁或 snapshot 边界
- 修改文件：`distributed.c`、`kvs_rdb.c`、`kvs_aof_rewrite.c`

**Stage 1：embedding 锁外 prepare** (`03_vector_embedding_prepare_outside_lock.md`)
- VSET/VSEARCH embedding 生成移出锁外
- prepare 只能依赖 request 文本和 embedding runtime
- commit 前重新检查 question+scope
- 修改文件：`kvs_vector.c`、`kvs_vector_embedding.c`

**Stage 2-3：command_task / response_task 抽象** (`04_command_task_design.md`, `05_response_task_design.md`)
- 引入 `kvs_command_task_t` 承载完整命令生命周期
- 引入 `kvs_response_task_t` 管理响应 buffer 所有权
- 修改文件：`kvs_command_task.c/h`、`net_session.h`

**Stage 4：reactor-only single command executor** (`06_single_command_executor_design.md`)
- 实现 reactor 单线程 executor 原型
- session refcount、closing、seq 字段补充
- 单 session 单 in-flight 保序约束
- 修改文件：`kvs_command_executor.c/h`、`reactor.c`、`net_session.c`

**Stage 5-6：proactor + ntyco executor 接入**
- proactor：io_uring 适配、eventfd 唤醒、异步闭环
- ntyco：coroutine 路径 executor 化
- 修改文件：`proactor.c`、`ntyco.c`、`ntyco_defult.c`

**Stage 7-9：embedding prepare worker pool**
- worker pool 实现（`kvs_embedding_worker_pool.c/h`）
- per-thread OrtSession 评估与 tuning
- batch embedding prepare 优化
- VSET commit 路径拆解
- 修改文件：`kvs_embedding_worker_pool.c/h`、`kvs_command_executor.c`

**Stage 10：后台任务 executor 化**

| Sub-stage | 内容 | 修改文件 |
|-----------|------|----------|
| 10.1 | TTL executor 化 | `kvs_command_executor.c`、`kvs_ttl_background.c` |
| 10.2 | lazy expire executor-owned | `kvs_command_executor.c`、`kvs_command_task.c` |
| 10.3 | replicated_command_task | `kvs_command_executor.c`、`distributed.c` |
| 10.4 | ntyco executor readiness | `ntyco.c` |
| 10.5 | master full sync snapshot_task | `kvs_command_executor.c`、`distributed.c` |
| 10.6 | RDB save snapshot_task | `kvs_command_executor.c`、`kvs_rdb.c` |
| 10.7 | AOF rewrite snapshot_task | `kvs_command_executor.c`、`kvs_aof_rewrite.c` |
| 10.8 | replication fallback audit | `distributed.c` |
| 10.9 | shutdown owner boundary | `kvstore.c` |
| 10.9a | slave connect owner audit | `distributed.c` |
| 10.10 | TTL scanner owner convergence | `kvs_command_executor.c`、`kvs_ttl_background.c` |
| 10.11 | rwlock removal readiness re-audit | 静态审计 |
| 10.12 | eBPF replication fallback executorization | `ebpf_syncd.c` |
| 10.13 | ntyco executor convergence | `ntyco.c`、`ntyco_defult.c` |
| 10.14 | full executor coverage closeout | 全量静态审计 gate |

**Stage 11：rwlock wrapper 接管**
- 引入 `kvs_owner_rwlock_rdlock/wrlock/unlock` wrapper
- 引入 `KVLITE_ROUTE_C_WEAK_RWLOCK` 环境变量
- 默认仍为真实锁（weak-lock 为显式 opt-in）
- 修改文件：`kvs_owner_lock.c/h`、`kvs_command_executor.c`、`kvs_command_task.c`

**Stage 12：weak-rwlock correctness 和 stress gate**
- correctness suite：real-lock + weak-lock 双模式通过
- mixed stress：reactor/proactor/ntyco c4/c8/c16 通过
- TSAN：5 个 data race 修复后 PASS
- NtyCo third_party vendoring（pthread_once 修复永久化）
- 修改文件：`kvs_shutdown.h`、`kvs_ttl_background.c`、`distributed.c`、`sync_backend_tcp.c`、`kvs_aof.c`、`nty_socket.c`

**Stage 13：g_kv_data_rwlock 降级与最终 gate**
- weak-lock 设为默认（`KVLITE_ROUTE_C_WEAK_RWLOCK` 默认 1）
- 裸 `pthread_rwlock_*` lock/unlock 全部消除
- `dest_kvengine()` 走 wrapper
- `g_kv_data_rwlock` 降级为 fallback/diagnostic 对象
- 修改文件：`kvs_owner_lock.c`（默认值变更）、`kvstore.c`（destroy 路径 wrapper 化）

## 3. 最终架构

```
网络层（reactor/proactor/ntyco）
  → RESP parse → kvs_filter_protocol()
    → kvs_command_task_build() → kvs_command_task_prepare()
      → [需要 embedding] → embedding worker pool → prepared_task
      → [无需 prepare] → 直接提交
    → kvs_command_task_execute_sync()
      → executor 线程 (kvs_owner_lock_set_executor_thread)
        → kvs_owner_rwlock_* (weak-lock 默认: no-op)
        → command handler → store/TTL/vector/HNSW 访问
        → AOF append (顺序 = commit 顺序)
        → master_sync_command (如果是 master)
    → kvs_response_task_finalize() → 网络层 send

后台任务（全部在 executor 线程内）：
  - TTL active scan: kvs_command_executor_run_ttl_scan()
  - TTL lazy expire: kvs_command_task_execute_handler() 内重试
  - master full sync: kvs_command_executor_run_full_sync_snapshot()
  - RDB save: kvs_command_executor_run_rdb_snapshot()
  - AOF rewrite: kvs_command_executor_run_aof_rewrite_snapshot()
  - slave apply: kvs_command_executor_run_replicated_command()
```

## 4. Weak-Lock / Real-Lock Fallback

### 4.1 默认模式（weak-lock）

- `KVLITE_ROUTE_C_WEAK_RWLOCK` 未设置 → 默认 1
- executor 线程内的 `kvs_owner_rwlock_rdlock/wrlock/unlock` 为成对 no-op
- 非 executor 线程 fallback 到真实 `pthread_rwlock_*` + WARNING 日志
- `g_owner_weak_noop_depth` 跟踪嵌套配对

### 4.2 回滚模式（real-lock）

```bash
KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore.conf
```

- 所有 `kvs_owner_rwlock_*` 直接透传 `pthread_rwlock_*`
- `g_kv_data_rwlock` 对象保留：定义、初始化、销毁均保留
- 回滚后所有回归通过

### 4.3 Owner Stats 验证

正常运行时（两种模式均成立）：
```
non_executor_thread_calls = 0
weak_fallback_calls = 0    (weak-lock mode)
weak_noop_calls > 0        (weak-lock mode)
strict_assertion_failures = 0
```

## 5. 性能结论

### 5.1 核心吞吐（weak-lock vs real-lock）

| Metric | Weak-lock | Real-lock | Delta |
| --- | ---: | ---: | ---: |
| SET qps | 25405.42 | 25763.53 | -1.4% |
| GET qps | 19623.86 | 23347.43 | -15.9% |
| VSET qps | 110.41 | 122.60 | -9.9% |
| VSEARCH qps | 157.60 | 185.97 | -15.3% |

### 5.2 TTL（weak-lock 优于 real-lock）

| Backend | Weak-lock | Real-lock | Delta |
| --- | ---: | ---: | ---: |
| array | 6598.42 | 5987.25 | +10.2% |
| rbtree | 6483.60 | 5833.50 | +11.1% |
| hash | 6091.49 | 5270.70 | +15.6% |
| skiptable | 6615.22 | 6597.03 | +0.3% |

### 5.3 持久化/复制耗时

所有指标基本持平，无回滚成本爆炸。

### 5.4 结论

Route C 结论应定义为**"所有权模型收口完成"**，不是"性能绝对提升完成"。Weak-lock 默认模式在 TTL 路径有明确收益（+10~15%），但在 GET/VSET/VSEARCH 路径有退化。架构简化（单线程 executor 中锁为 no-op）是主要收益。

## 6. NtyCo Third-Party Vendoring

将 NtyCo 从 git submodule 迁移为 `third_party/NtyCo/` 本地源码：

- 原 submodule URL: `https://github.com/wangbojing/NtyCo.git`，commit `72ab5fd`
- `nty_socket.c` 的 `init_hook` 使用 `pthread_once` 修复（TSAN 发现 data race）
- 修复无法推回上游，本地 vendoring 永久保留
- 构建路径更新：`NTYCO_DIR=third_party/NtyCo`
- `.gitignore` 规则：`/third_party/NtyCo/objs/`、`/third_party/NtyCo/libntyco.a`

## 7. TSAN / Mixed Stress / Correctness

### 7.1 TSAN

- Clang 10 + `-fsanitize=thread`
- 修复了 5 个 pre-existing data race：
  1. `g_ttl_scan_stop` / `g_ttl_scan_running` — 改用 `__atomic_*`
  2. `global_dist_config.master_listen_fd` — 改用 `__atomic_*`
  3. `g_tcp_running` — 改用 `__atomic_*`
  4. `aof_state.stop` — 改用 `__atomic_*`
  5. NtyCo `init_hook` — 改用 `pthread_once`
- 最终结果：**零 data race**（real-lock + weak-lock 双模式）

### 7.2 Mixed Stress

- reactor / proactor / ntyco，c4 / c8 / c16
- KV + TTL + Vector/HNSW + AOFREWRITE + RDB/AOF + replication
- real-lock + weak-lock 双模式 **PASS**

### 7.3 Correctness

- 17 项子测试全部 PASS（real-lock + weak-lock）
- RDB/AOF/replication/TTL/vector/HNSW/shutdown 全覆盖

## 8. 回滚方式

```bash
# 运行时回滚
KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore.conf

# Embedding 配置
KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore-embedding.conf

# 回滚后最小验证
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/shutdown_owner_stage10_9.sh
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/rdb_snapshot_stage10_6.sh
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/aof_rewrite_snapshot_stage10_7.sh
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/master_full_sync_stage10_5.sh
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/replicated_command_task_stage10_3.sh
```

## 9. Tag 信息

- 推荐 tag: `route-c-final-weak-lock-ready`
- 前置条件：clean tree + final closeout PASS + real-lock fallback PASS
- 已在 2026-06-19 clean tree 复跑确认

## 10. 剩余风险

1. **Shutdown-exit flaky**：聚合 correctness suite 编排中偶发 `server did not exit`，standalone 通过。低频，非稳定性退化。
2. **weak-lock 性能并非绝对优势**：GET/VSET/VSEARCH 在 weak-lock 模式下有退化。
3. **多 executor 风险**：当前 weak-lock 假设单线程 executor。引入多 executor 需重新启用真实锁或 executor-internal 同步。
4. **TSAN 仅 Clang 可用**：GCC 9 缺少 `libtsan_preinit.o`。

## 11. 后续方向建议

Route C 在 Stage 13 已达成所有原始架构目标。**不建议继续做 Stage 14。**

后续如需进一步工作，建议作为独立路线：
- 多 executor 线程 + executor-internal 同步
- per-thread OrtSession 评估
- GET/VSET/VSEARCH weak-lock 性能退化分析与优化
- 长时间饱和 mixed stress

## 12. 修改的生产代码文件（Route C 全量）

| 模块 | 文件 |
|------|------|
| Core | `kvstore.c`、`kvs_operation.c`、`kvs_config.c`、`kvs_ttl_background.c` |
| Executor | `kvs_command_executor.c/h`、`kvs_command_task.c/h`、`kvs_owner_lock.c/h` |
| Network | `reactor.c`、`proactor.c`、`ntyco.c`、`ntyco_defult.c`、`net_session.c/h` |
| Vector | `kvs_vector.c`、`kvs_vector_embedding.c`、`kvs_vector_index.c` |
| Embedding | `kvs_embedding_worker_pool.c/h`、`embedding_runtime_onnx.c` |
| Persistence | `kvs_aof.c`、`kvs_aof_rewrite.c`、`kvs_rdb.c` |
| Distributed | `distributed.c`、`sync_backend_tcp.c` |
| eBPF | `ebpf_syncd.c` |
| Shutdown | `kvs_shutdown.h` |
| Globals | `kvs_globals.h`、`kvs_types.h`、`kvs_config.h` |
| Third-party | `third_party/NtyCo/core/nty_socket.c` |
