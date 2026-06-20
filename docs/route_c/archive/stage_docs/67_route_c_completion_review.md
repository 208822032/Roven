# Route C Completion Review — 阶段性收口审计

Date: 2026-06-19

## 1. 本次审计目标

对照原始 Route C 设计文档、已完成阶段文档、当前代码状态和测试结果，判断 Route C 工作是否可以阶段性告一段落，是否可以打 tag，以及后续只保留哪些观察项。

本次审计**不开发新功能**。

## 2. 原始 Route C 目标清单

原始目标来源：`docs/route_c_perf_arch/00_route_c_overview.md`、`14_route_c_design_review.md`、`13_iteration_roadmap.md`

| # | 目标 | 状态 |
|---|------|------|
| 1 | 普通 KV handler 不再被网络线程直接调用 | ✅ 完成 |
| 2 | Vector handler 不再被网络线程直接调用 | ✅ 完成 |
| 3 | embedding prepare 与 commit 分离 | ✅ 完成 |
| 4 | single command executor 成为 store/TTL/vector/HNSW/AOF/RDB/replication 的状态 owner | ✅ 完成 |
| 5 | TTL active expire 不再直接访问 store（不经 executor） | ✅ 完成 |
| 6 | lazy expire 只在 executor 内完成 | ✅ 完成 |
| 7 | RDB save 不直接遍历 live store（不经 executor） | ✅ 完成 |
| 8 | AOF rewrite 不直接遍历 live store（不经 executor） | ✅ 完成 |
| 9 | master full sync 不直接遍历 live store（不经 executor） | ✅ 完成 |
| 10 | AOF append 顺序等于 commit 顺序 | ✅ 完成 |
| 11 | slave apply 通过 replicated_command_task | ✅ 完成 |
| 12 | shutdown/destroy 在 executor drain 后执行 | ✅ 完成 |
| 13 | vector store/id_map/HNSW owner 明确 | ✅ 完成 |
| 14 | reactor/proactor/ntyco 都没有绕 executor 的 shared-state 生产路径 | ✅ 完成 |
| 15 | g_kv_data_rwlock 不再是正确性主路径 | ✅ 完成 |
| 16 | weak-lock 默认启用 | ✅ 完成 |
| 17 | 真实锁回滚路径仍保留 | ✅ 完成 |
| 18 | TSAN 或等价竞态检测通过 | ✅ 完成（本次复跑） |
| 19 | mixed stress 通过 | ✅ 完成（本次复跑） |
| 20 | RDB/AOF/replication/TTL/vector/HNSW 回归通过 | ✅ 完成（本次复跑） |

**结论：全部 20 项目标已达成。**

## 3. 当前完成情况

### 3.1 路线 C 阶段总览

| 阶段 | 内容 | 状态 |
|------|------|------|
| Stage 0-3 | 命令/响应所有权审计、command-task/response-task 方向确立 | ✅ |
| Stage 4-6 | executor 模型在 reactor/proactor/ntyco 生命周期稳定 | ✅ |
| Stage 7-9 | embedding 工作移入受控 runtime 和 worker-pool 边界 | ✅ |
| Stage 10 | 持久化/复制/TTL/shutdown/slave-connect 收敛到 executor | ✅ |
| Stage 11 | rwlock wrapper 接管和 weak-mode 开关引入 | ✅ |
| Stage 12 | weak-rwlock 正确性、mixed stress、TSAN gate | ✅ |
| Stage 13 | weak-lock 默认启用、g_kv_data_rwlock 降级、final gate | ✅ |

### 3.2 架构所有权模型

```
网络层（reactor/proactor/ntyco）→ RESP parse → kvs_filter_protocol()
  → kvs_command_task_build() → kvs_command_task_prepare()
    → [需要 embedding] → embedding worker pool → prepared_task
    → [无需 prepare] → 直接提交
  → kvs_command_task_execute_sync() → executor 线程
    → kvs_owner_rwlock_* (weak-lock: no-op)
    → command handler → store/TTL/vector/HNSW 访问
  → kvs_response_task_finalize() → 网络层 send
```

所有共享状态访问路径均已收敛到 executor 线程。

## 4. 未完成项

**无。** 所有原始 Route C 目标均已达成。

已知非阻塞观察项见第 6 节。

## 5. 是否存在 Blocking Bug

**不存在。**

本次审计执行了以下验证：

| 测试 | 模式 | 结果 |
|------|------|------|
| `route_c_final_closeout_stage13.sh` | weak-lock（默认） | PASS |
| `route_c_final_closeout_stage13.sh` | real-lock（回滚） | PASS |
| `weak_rwlock_correctness_stage12.sh` | weak-lock + real-lock | PASS |
| `shutdown_owner_stage10_9.sh` | real-lock 回滚 | PASS |
| `rdb_snapshot_stage10_6.sh` | real-lock 回滚 | PASS |
| `aof_rewrite_snapshot_stage10_7.sh` | real-lock 回滚 | PASS |
| `master_full_sync_stage10_5.sh` | real-lock 回滚 | PASS |
| `replicated_command_task_stage10_3.sh` | real-lock 回滚 | PASS |
| `weak_rwlock_tsan_stage12.sh` | Clang TSAN | PASS（零 data race） |
| `weak_rwlock_mixed_stress_stage12.sh` | reactor/proactor/ntyco c4/c8/c16 | PASS |

## 6. 是否存在 Non-Blocking Watch Item

存在以下已知非阻塞观察项（不阻塞 tag）：

1. **Shutdown-exit flaky**：在聚合 correctness suite 全量编排中，real-lock 回滚路径曾出现一次 `server did not exit`，但 standalone 复跑通过。本次 clean tree 复跑 correctness suite（weak-lock + real-lock）全部通过。判定为低频编排时序 flaky，非稳定性退化。

2. **weak-lock 并非所有样本性能提升**：在本次 closeout 样本中，GET（-15.9%）、VSET（-9.9%）、VSEARCH（-15.3%）在 weak-lock 默认模式下较 real-lock 退化。Route C 结论应定义为"所有权模型收口完成"，不是"性能绝对提升完成"。

3. **未来多 executor**：当前 weak-lock 假设单线程 executor 使锁为 no-op。如果后续引入多 executor 线程，需要重新启用真实锁或引入 executor-internal 同步。此风险已记录在 `65_stage13_global_rwlock_removal_and_final_gate.md` 第 14 节。

4. **TSAN 仅在 Clang 下可用**：GCC 9 环境缺少 `libtsan_preinit.o`，TSAN 构建需使用 Clang。本次 TSAN 在 Clang 10 下通过。

## 7. Weak-Lock 默认模式状态

- **默认启用**：`KVLITE_ROUTE_C_WEAK_RWLOCK` 不设置时默认为 1（weak-lock）
- **行为**：executor 线程内的 `kvs_owner_rwlock_rdlock/wrlock/unlock` 为成对 no-op，通过 `g_owner_weak_noop_depth` 跟踪嵌套
- **非 executor 线程**：fallback 到真实 `pthread_rwlock_*` + WARNING 日志
- **Stats 验证**：`non_executor_thread_calls=0`、`weak_fallback_calls=0`、`strict_assertion_failures=0`

## 8. Real-Lock Fallback 状态

- **运行时回滚**：`KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore.conf`
- **验证通过**：所有核心回归（TTL/RDB/AOF/replication/vector/shutdown）在 real-lock 回滚模式下 PASS
- **Stats 验证**：`weak_mode=0`、`weak_noop_calls=0`、`non_executor_thread_calls=0`
- **保留对象**：`g_kv_data_rwlock` 定义、初始化、销毁均保留

## 9. TSAN / Mixed Stress 证据

### 9.1 TSAN（本次复跑）

- 构建：Clang 10 + `-fsanitize=thread`
- 测试：`tests/route_c/weak_rwlock_tsan_stage12.sh`
- 结果：**PASS，零 data race**
  - real-lock reactor：零 race
  - weak-lock reactor：零 race
  - 持久化/复制 sub-suite：零 race

### 9.2 Mixed Stress（本次复跑）

- 测试：`tests/route_c/weak_rwlock_mixed_stress_stage12.sh`
- 覆盖：reactor / proactor / ntyco，c4 / c8 / c16，real-lock + weak-lock
- KV 操作：GET / SET / DEL
- TTL：EXPIRE / TTL / PERSIST / active expire / lazy expire
- Vector/HNSW：VSET / VGET / VSEARCH / VDEL
- 持久化：AOFREWRITE / RDB save-load / AOF save-load
- 复制：master full sync / slave apply / replicated command task
- 结果：**PASS，所有模式通过**

## 10. Performance Closeout 结论

### 10.1 核心吞吐（weak-lock vs real-lock）

| Metric | Weak-lock default | Real-lock rollback | Delta vs real |
| --- | ---: | ---: | ---: |
| SET qps | 25405.42 | 25763.53 | -1.4% |
| GET qps | 19623.86 | 23347.43 | -15.9% |
| VSET qps | 110.41 | 122.60 | -9.9% |
| VSEARCH qps | 157.60 | 185.97 | -15.3% |

### 10.2 TTL（weak-lock 优于 real-lock）

| Backend | Weak-lock qps | Real-lock qps | Delta vs real |
| --- | ---: | ---: | ---: |
| array | 6598.42 | 5987.25 | +10.2% |
| rbtree | 6483.60 | 5833.50 | +11.1% |
| hash | 6091.49 | 5270.70 | +15.6% |
| skiptable | 6615.22 | 6597.03 | +0.3% |

### 10.3 持久化/复制耗时

所有指标基本持平，无回滚成本爆炸。

### 10.4 结论

**Route C 结论应定义为"所有权模型收口完成"，不是"性能绝对提升完成"。** Weak-lock 默认模式带来的架构简化（单线程 executor 中锁为 no-op）是主要收益；性能表现因 workload 而异。

## 11. 是否建议打 Tag

**是 — 建议打 tag。**

本次审计在 clean tree 上完成，所有 gate 重新验证通过，无 blocker。

## 12. Tag 前置条件

所有前置条件已满足：

- ✅ Git tree clean
- ✅ Final closeout gate PASS（clean tree 复跑）
- ✅ Real-lock fallback correctness PASS（本次复跑）
- ✅ TSAN PASS（本次复跑，零 data race）
- ✅ Mixed stress PASS（本次复跑，reactor/proactor/ntyco）
- ✅ 无 blocker
- ✅ 产物未进入 git status（workspace/logs 在 .gitignore）

## 13. 是否建议继续做 Stage 14

**不建议。** Route C 在 Stage 13 已达成所有原始架构目标：

- 所有权模型收敛完成
- weak-lock 默认启用
- real-lock 回滚保留
- 所有正确性 gate 通过

后续如需进一步工作，建议作为独立路线（如 Route D）启动，方向可包括：
- 多 executor 线程 + executor-internal 同步
- per-thread OrtSession 评估
- weak-lock 下 GET/VSET/VSEARCH 性能退化分析与优化
- 长时间饱和 mixed stress（当前为缩减样本）

## 14. 后续建议方向

1. **打 tag 后保持 main 分支稳定**，Route C 产物作为阶段性基线。
2. **Shutdown-exit flaky** 作为 low-priority watch item，不阻塞当前 tag。
3. **性能退化**（GET/VSET/VSEARCH weak-lock vs real-lock）可在独立性能优化路线中分析，不混入所有权收敛路线。
4. **非阻塞观察项** 记录在 tag message 或后续 issue 中即可，不需要阻塞合入。

## 15. 审计结论

**结论：ROUTE_C_COMPLETE_READY_TO_TAG**

满足条件：
- ✅ Git tree clean
- ✅ Final closeout gate PASS
- ✅ Real-lock fallback correctness PASS
- ✅ 无 blocker
- ✅ 产物未进入 git status

## 16. 建议 Tag 命令

```bash
git tag -a route-c-final-weak-lock-ready -m "Route C 所有权模型收口完成

弱锁默认启用，真实锁回滚路径保留。所有原始 Route C 目标达成。
TSAN/mixed stress/correctness 全部通过。

已知非阻塞观察项：
- Shutdown-exit 聚合编排低频 flaky（standalone 通过）
- GET/VSET/VSEARCH weak-lock 模式下性能未优于 real-lock
- 多 executor 需重新启用真实锁或 executor-internal 同步

Co-Authored-By: Claude <noreply@anthropic.com>"
```

## 17. 建议 Commit Message

提交审计报告：

```bash
git add docs/route_c_perf_arch/67_route_c_completion_review.md
git commit -m "补充路线C完成度审计报告

Route C 全部 20 项目标达成。在 clean tree 复跑：
- final closeout gate PASS（weak-lock + real-lock）
- correctness suite PASS（weak-lock + real-lock）
- TSAN PASS（Clang，零 data race）
- mixed stress PASS（reactor/proactor/ntyco c4/c8/c16）

审计结论：ROUTE_C_COMPLETE_READY_TO_TAG

Co-Authored-By: Claude <noreply@anthropic.com>"
```

## 18. 修改/新增文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `docs/route_c_perf_arch/67_route_c_completion_review.md` | 新增 | 本次审计报告 |

**未修改任何生产代码。**
