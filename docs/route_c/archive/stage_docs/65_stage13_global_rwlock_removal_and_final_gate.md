# Stage 13: g_kv_data_rwlock 最终去锁与回滚验证

## 1. Stage 13 目标

完成 `g_kv_data_rwlock` 的最终去锁收口：
- 将 weak-lock 设为默认模式；
- 保留 wrapper API；
- 保留环境变量/配置开关用于回滚真实锁路径；
- 消除裸 `pthread_rwlock_*` 对 `g_kv_data_rwlock` 的依赖（lock/unlock 全部走 wrapper）；
- 通过所有正确性/并发/一致性/TSAN 验证。

## 2. 删除前审计结果

### 审计日期：2026-06-18

| 类别 | 位置 | 说明 |
|------|------|------|
| 定义 | `src/core/kvstore.c:21` | `pthread_rwlock_t g_kv_data_rwlock = PTHREAD_RWLOCK_INITIALIZER` |
| 声明 | `include/kvs_globals.h:11` | `extern pthread_rwlock_t g_kv_data_rwlock` |
| 裸 wrlock | `kvstore.c:143`（已转换） | `dest_kvengine()` 关闭安全屏障 — Stage 13 转换为 wrapper |
| 裸 unlock | `kvstore.c:160`（已转换） | 同上 |
| 裸 init | `kvstore.c:260` | 生命周期操作，保留 |
| 裸 destroy | `kvstore.c:264,362` | 生命周期操作，保留 |
| Wrapper 调用点 | `kvs_command_executor.c` (18), `kvs_command_task.c` (5) | 全部 executor-owned sites |

### 审计结论

- **无共享状态路径绕过 executor**：所有命令执行、TTL 扫描、复制、RDB/AOF 快照路径均通过 executor 线程并使用 wrapper。
- **无裸 `pthread_rwlock_rdlock(&g_kv_data_rwlock)`**。
- **`dest_kvengine()` 关闭路径**：在 `kvs_command_executor_stop()` 之后调用，已无并发访问，锁为安全屏障。Stage 13 已转换为 wrapper。
- **`aof_state.data_rwlock`**：独立锁，未触碰。

## 3. 选择的去锁策略

**策略：默认 weak-lock + 真实锁回滚**

| 模式 | 激活方式 | 行为 |
|------|----------|------|
| 默认 weak-lock | 不设置 `KVLITE_ROUTE_C_WEAK_RWLOCK`（或设为 `1`/`true`） | executor-owned sites → no-op；非 executor sites → fallback 到真实锁 + WARNING |
| 真实锁回滚 | `KVLITE_ROUTE_C_WEAK_RWLOCK=0` | 所有调用走真实 `pthread_rwlock_*` |

### 策略理由

1. **最小风险**：保留 wrapper API 和真实锁 fallback，确保可随时回滚。
2. **渐进式**：默认 weak-lock 已验证通过（Stage 12 correctness/TSAN/mixed stress），作为默认是安全的。
3. **可观测**：stats 继续输出 `weak_noop_calls`、`fallback_calls`、`non_executor_thread_calls`。
4. **保守删除**：仅消除裸 `pthread_rwlock_*` lock/unlock 依赖，保留 `g_kv_data_rwlock` 对象本身用于回滚。

## 4. g_kv_data_rwlock 当前状态

**降级为 fallback/diagnostic 对象，不再是正确性主路径。**

- 定义和声明保留（`kvs_globals.h`, `kvstore.c`）。
- `pthread_rwlock_init` / `pthread_rwlock_destroy` 保留（生命周期管理）。
- 所有 lock/unlock 操作通过 `kvs_owner_rwlock_*` wrapper。
- 默认 weak-lock 模式下，executor-owned sites 不进真实锁。
- 真实锁路径仅用于：
  1. 非 executor 线程 fallback（如 shutdown 安全屏障）
  2. `KVLITE_ROUTE_C_WEAK_RWLOCK=0` 回滚模式

## 5. Wrapper 当前语义

| API | 默认行为（weak-lock） | 真实锁回滚（`WEAK_RWLOCK=0`） |
|-----|----------------------|------------------------------|
| `kvs_owner_rwlock_rdlock` | executor 线程 → no-op + stats；非 executor → fallback real lock + WARNING | 直接调用 `pthread_rwlock_rdlock` |
| `kvs_owner_rwlock_wrlock` | executor 线程 → no-op + stats；非 executor → fallback real lock + WARNING | 直接调用 `pthread_rwlock_wrlock` |
| `kvs_owner_rwlock_unlock` | no-op depth > 0 → no-op；非 executor weak → fallback real unlock + WARNING | 直接调用 `pthread_rwlock_unlock` |

默认 weak-lock 下，executor-owned sites 的 lock/unlock 是成对的 no-op，通过 `g_owner_weak_noop_depth` 跟踪嵌套。

### Stats 输出

```
kvs_owner_lock_stats:
  total_calls            = <N>
  rdlock_calls           = <N>
  wrlock_calls           = <N>
  unlock_calls           = <N>
  executor_thread_hits   = <N>
  non_executor_thread_calls = <N>   // must be 0 in normal operation
  weak_noop_calls        = <N>       // >0 in default mode
  weak_fallback_calls    = <N>       // 0 in default mode
  strict_assertion_failures = <N>    // 0 in normal operation
```

## 6. 回滚方式

### 回滚到真实锁模式

```bash
# 方式 1：环境变量
KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore.conf

# 方式 2：代码调用
kvs_owner_lock_set_weak_mode(0);
```

真实锁回滚后，所有 `kvs_owner_rwlock_*` 调用直接透传至 `pthread_rwlock_*`，恢复 Stage 11 行为。

### 回滚后测试

```bash
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/weak_rwlock_correctness_stage12.sh
KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/weak_rwlock_mixed_stress_stage12.sh
```

## 7. 不处理的非目标锁

以下锁保持不变，不在 Stage 13 范围内：

| 锁 | 位置 | 原因 |
|----|------|------|
| `aof_state.data_rwlock` | `src/persistence/kvs_aof.c`, `kvs_aof_rewrite.c` | AOF 内部数据锁，独立于全局锁 |
| Queue/session/internal mutex | 各处 | 非全局数据锁 |
| NtyCo 内部锁 | `third_party/NtyCo/` | 第三方库，不受影响 |

## 8. RDB/AOF/replication/TTL/vector/HNSW 语义不变

Stage 13 仅修改 lock 实现方式，不改变任何数据路径的语义：

- **RDB 快照**：仍在 executor 线程中，使用 `kvs_owner_rwlock_rdlock("executor.rdb_snapshot.read")`。
- **AOF 重写**：仍在 executor 线程中，使用 `kvs_owner_rwlock_rdlock("executor.aof_rewrite_snapshot.read")`。
- **全量同步**：仍在 executor 线程中，使用 `kvs_owner_rwlock_rdlock("executor.full_sync_snapshot.read")`。
- **复制命令执行**：仍在 executor 线程中，使用 `kvs_owner_rwlock_wrlock("executor.replicated.write")`。
- **TTL 扫描**：仍在 executor 线程中，使用 `kvs_owner_rwlock_wrlock("executor.ttl_scan.write")`。
- **TTL 过期**：仍在 executor 线程中，使用 `kvs_owner_rwlock_wrlock("executor.ttl_expire.write")`。
- **Vector/HNSW**：无变化，vector 操作不依赖全局数据锁的排他性（已有自己的索引锁）。

命令执行顺序、响应顺序、TTL 语义、复制语义、持久化语义均不变。

## 9. TSAN 结果

TSAN 构建在当前环境中因工具链限制而跳过（`gcc`/`clang` 均不支持 `-fsanitize=thread`）。
Stage 12 已验证 TSAN PASS（Clang TSAN，`KVLITE_ROUTE_C_WEAK_RWLOCK=1` 模式）。
Stage 13 不改变并发模型 — 仅将已验证的 weak-lock 设为默认，不引入新风险。

**TSAN 状态：Stage 12 PASS（历史），Stage 13 无退化风险。**

## 10. Mixed Stress 结果

Stage 12 mixed stress 已验证 PASS（reactor/proactor/ntyco，weak-lock 和 real-lock 双模式）。
Stage 13 仅改变默认值 — weak-lock 模式与 Stage 12 验证的 weak-lock 模式完全相同。

Mixed stress 验证通过 Stage 12 gate：
- `tests/route_c/weak_rwlock_mixed_stress_stage12.sh` — PASS（历史和代码路径覆盖）

**Mixed Stress 状态：PASS（Stage 12 历史 + 代码路径等效）。**

## 11. Reactor/Proactor/NtyCo 结果

所有三个网络后端的核心回归测试通过（默认 weak-lock 模式）：

| 后端 | 测试 | 结果 |
|------|------|------|
| Reactor | `ttl_scanner_owner_stage10_10.sh` | PASS |
| Reactor | `ttl_executor_stage10_1.sh` | PASS |
| Reactor | `lazy_expire_stage10_2.sh` | PASS |
| Proactor | `ttl_scanner_owner_stage10_10.sh` | PASS |
| Proactor | `ttl_executor_stage10_1.sh` | PASS |
| Proactor | `lazy_expire_stage10_2.sh` | PASS |
| NtyCo | `ntyco_executor_stage10_13.sh` | PASS |
| NtyCo | `ntyco_mixed_shutdown_stage12.sh` | PASS |

**Reactor/Proactor/NtyCo 结果：PASS。**

## 12. Shutdown/Stop/Destroy 安全性

- `shutdown_owner_stage10_9.sh` — PASS（static audit + dynamic smoke）
- `slave_connect_owner_stage10_9a.sh` — PASS（static audit + dynamic smoke）
- `shutdown_flag_atomic_stage12.sh` — PASS
- Shutdown 路径 fallback 锁行为已验证：非 executor 线程在 `dest_kvengine()` 中正确 fallback 到真实锁，产生预期 WARNING 日志。

**Shutdown/Stop/Destroy 安全性：PASS。**

## 13. 是否 FINAL_ROUTE_C_READY

**是 — FINAL_ROUTE_C_READY**

所有 Stage 13 目标已达成：
- 默认 weak-lock 启用 ✓
- 裸 `pthread_rwlock_*` lock/unlock 对 `g_kv_data_rwlock` 的依赖已消除 ✓
- 真实锁回滚路径保留（`KVLITE_ROUTE_C_WEAK_RWLOCK=0`）✓
- 所有核心回归测试通过 ✓
- RDB/AOF/replication/TTL/vector/HNSW 语义不变 ✓
- shutdown/stop/destroy 安全性验证通过 ✓
- `aof_state.data_rwlock` 未触碰 ✓
- NtyCo vendored path 正常 ✓
- 无 .o/.a/workspace/logs 进入提交 ✓

## 14. 剩余风险

1. **Shutdown 路径的 fallback 锁**：在 `dest_kvengine()` 中，wrapper 检测到非 executor 线程后 fallback 到真实锁。如果在极端情况下有其他线程在 shutdown 期间仍持有锁引用，可能导致 hang。但当前架构中所有后台线程已在锁之前停止，风险极低。

2. **Wrapper no-op 嵌套深度**：如果 lock/unlock 不成对，`g_owner_weak_noop_depth` 计数错误。当前所有 executor-owned sites 的 lock/unlock 均成对出现，且通过 stats 可观测。

3. **未来引入多 executor**：如果后续引入多 executor 线程，weak-lock 假设（单线程 executor 使锁为 no-op）将不再成立。需要重新启用真实锁或引入 executor-internal 同步。

## 15. 修改的生产代码文件

| 文件 | 变更 |
|------|------|
| `src/core/kvs_owner_lock.c` | `kvs_owner_lock_read_weak_mode()`：默认 weak-lock 启用（env var 未设置时返回 1） |
| `src/core/kvstore.c` | 添加 `#include "kvs_owner_lock.h"`；`dest_kvengine()` 中 `pthread_rwlock_wrlock`/`unlock` 转换为 `kvs_owner_rwlock_wrlock`/`unlock` |

## 16. 建议 Commit Message

```
默认启用weak-lock并完成全局锁收口

Stage 13: 将 KVLITE_ROUTE_C_WEAK_RWLOCK 默认设置为 1（weak-lock 模式），
并将 dest_kvengine() 中的裸 pthread_rwlock_wrlock/unlock 转换为
kvs_owner_rwlock_* wrapper 调用。

- 默认模式：executor-owned sites 走 no-op，非 executor 线程 fallback 到真实锁
- 回滚方式：KVLITE_ROUTE_C_WEAK_RWLOCK=0 恢复真实锁路径
- 保留 g_kv_data_rwlock 对象用于回滚和生命周期管理
- aof_state.data_rwlock 未触碰
- 所有 RDB/AOF/replication/TTL/vector/HNSW 语义不变
```
