# Stage 10.12-pre: Route C Original Plan Reconciliation Audit

## 1. 本阶段目标

本阶段只做分析和审计，不修改生产代码，不做 eBPF / ntyco 改造，不删除、不弱化 `g_kv_data_rwlock`。

目标：回到项目中原计划的 Route C 文档（00–14），分析原设计到底支持 Full Executor Coverage 还是 Owner-safe Profile + Legacy Isolation，并对当前 Stage 9–10.11 的完成情况与原计划进行一致性对照。

## 2. 原始 Route C 文档清单

全部已读取的原始 Route C 设计文档（14 份）：

| # | 文档 | 主题 |
|---|------|------|
| 0 | `00_route_c_overview.md` | Route C 总架构与最终目标 |
| 1 | `01_lock_and_shared_state_audit.md` | 锁点与共享状态审计 |
| 2 | `02_existing_lock_bypass_fix_design.md` | 绕锁路径修复设计 |
| 3 | `03_vector_embedding_prepare_outside_lock.md` | VSET/VSEARCH embedding 锁外 prepare |
| 4 | `04_command_task_design.md` | command_task 抽象 |
| 5 | `05_response_task_design.md` | response_task 抽象 |
| 6 | `06_single_command_executor_design.md` | reactor-only single command executor |
| 7 | `07_background_tasks_executor_design.md` | 后台任务 executor 化 |
| 8 | `08_multi_poller_queue_design.md` | 多 poller / 多队列 |
| 9 | `09_embedding_prepare_worker_pool_design.md` | embedding prepare worker pool |
| 10 | `10_rwlock_removal_criteria.md` | rwlock removal 判定标准 |
| 11 | `11_persistence_replication_under_route_c.md` | AOF/RDB/主从同步纳入 commit 状态流 |
| 12 | `12_test_and_benchmark_plan.md` | 测试与性能验证计划 |
| 13 | `13_iteration_roadmap.md` | 迭代路线图 |
| 14 | `14_route_c_design_review.md` | 设计审批报告 |

全部已读取的已完成阶段文档（Stage 9–10.11，14 份）：

| # | 文档 | 主题 |
|---|------|------|
| 41 | `41_stage9_embedding_prepare_worker_pool.md` | Stage 9 embedding worker pool |
| 42 | `42_stage9_1_worker_pool_shutdown_stabilization.md` | worker pool shutdown 稳定化 |
| 43 | `43_stage10_rwlock_removal_readiness_audit.md` | Stage 10 rwlock removal 初步审计 |
| 44 | `44_stage10_1_ttl_ownership_convergence.md` | TTL active expire 收敛 |
| 45 | `45_stage10_2_lazy_expire_and_sync_fallback_audit.md` | lazy expire 与同步 fallback 审计 |
| 46 | `46_stage10_3_replicated_command_task.md` | Slave apply replicated_command_task |
| 47 | `47_stage10_4_ntyco_executor_readiness.md` | Ntyco executor readiness 决策 |
| 48 | `48_stage10_5_master_full_sync_snapshot_task.md` | Master full sync snapshot 设计 |
| 49 | `49_stage10_5a_master_full_sync_snapshot_implementation.md` | Stage 10.5A 实现 |
| 50 | `50_stage10_6_rdb_save_snapshot_task.md` | RDB save snapshot task |
| 51 | `51_stage10_7_aof_rewrite_snapshot_task.md` | AOF rewrite snapshot task |
| 52 | `52_stage10_8_replication_fallback_audit.md` | eBPF replication fallback 审计 |
| 53 | `53_stage10_9_shutdown_owner_boundary_audit.md` | Shutdown owner boundary |
| 54 | `54_stage10_9a_slave_connect_thread_owner_audit.md` | Slave connect thread owner |
| 55 | `55_stage10_10_ttl_scanner_runtime_owner_convergence.md` | TTL scanner runtime 收敛 |
| 56 | `56_stage10_11_rwlock_removal_readiness_reaudit.md` | rwlock removal readiness 再审计 |

## 3. 原始 Route C 最终目标摘录与解释

### 3.1 总架构目标（00_route_c_overview.md）

原文：

```text
network poller
  -> RESP parse
  -> command_task
  -> classify/router
     -> no prepare -> commit executor
     -> prepare -> embedding worker pool -> prepared_task -> commit executor
  -> response_task
  -> original poller send
```

**解释**：所有命令路径的终点都是 commit executor。没有旁路，没有例外。

### 3.2 三层所有权模型（00_route_c_overview.md）

```text
网络层：socket / recv / send / RESP parse
计算层：embedding prepare，不访问 store
状态层：commit executor，唯一访问 store/TTL/HNSW/AOF/RDB/replication
```

**关键语句**："commit executor，唯一访问 store/TTL/HNSW/AOF/RDB/replication"。修饰词是"唯一"（sole/exclusive），不是"主要"或"在某个 profile 下"。

### 3.3 后台任务 executor 化（07_background_tasks_executor_design.md）

目标原文：

> 阶段 5 的目标是把所有后台 store 访问路径 executor 化。这是删除或弱化 `g_kv_data_rwlock` 前的硬前置。

覆盖范围：

- TTL active expire → `expire_scan_task`
- AOF append → executor 按 commit 顺序生成
- AOF rewrite → `snapshot_task`
- RDB save → `snapshot_task`
- Master full sync → `snapshot_task`
- Slave apply → `replicated_command_task`
- Shutdown → executor drain 后 destroy

**解释**："所有后台 store 访问路径"（all background store access paths），无例外。

### 3.4 持久化与复制纳入状态流（11_persistence_replication_under_route_c.md）

原文结论：

```text
持久化和主从同步不是附属模块，必须纳入同一条命令状态流。
```

具体要求：

> - slave apply 不得绕过 executor。
> - master full sync 不允许 sync thread 直接遍历 live store。
> - 持久化顺序必须等于 commit 顺序。
> - 复制顺序必须等于 commit 顺序。
> - snapshot 必须对应明确状态点。

**解释**：这是全路径要求，不是 profile 限定。

### 3.5 rwlock removal 判定标准（10_rwlock_removal_criteria.md）

硬条件原文：

```text
- 普通 KV handler 不再被网络线程直接调用。
- Vector handler 不再被网络线程直接调用。
- TTL active expire 不再直接访问 store。
- lazy expire 只在 executor 内完成。
- RDB save 不直接遍历 live store。
- AOF rewrite 不直接遍历 live store。
- master full sync 不直接遍历 live store。
- AOF append 顺序由 executor 产生。
- slave apply 通过 replicated_command_task。
- shutdown/destroy 在 executor drain 后执行。
- vector store / id_map / HNSW 所有访问有明确 owner。
```

**关键原则**：

```text
删除锁是最后一步，不是优化起点。
```

**解释**：每个条件针对的是特定路径类型，而不是特定 network_mode。条件中没有"仅在 reactor/proactor normal async 路径下"的限定词。

### 3.6 迭代路线图（13_iteration_roadmap.md）

Stage 5 的定义：

> 阶段 5：后台任务 executor 化。目标：把所有后台 store 访问路径 executor 化。

注意用词是"所有"（all），不是"部分"（some）或"主要"（main）。

### 3.7 设计审批报告（14_route_c_design_review.md）

长期路线审批：

> - single commit executor：合理，能替代全局锁成为 store/TTL/vector/HNSW/AOF/replication 状态 owner
> - background task executor 化：合理且必要，是去锁硬前置
> - persistence / replication 纳入同一状态流：合理且必要
> - 最终删除或弱化 g_kv_data_rwlock：合理，但只能在所有 live state 访问收敛后做

再次强调"所有 live state 访问收敛后"。

### 3.8 对 ntyco 的原始态度

原始文档对 ntyco 的论述分布在几处：

- `00_route_c_overview.md §2`：指出 ntyco 和其他两种模式一样通过 `net_session_process_resp()` 同步执行 handler，是当前问题的一部分。
- `06_single_command_executor_design.md §4`：第一版不支持 ntyco（作为阶段限制，不是永久排除）。
- `08_multi_poller_queue_design.md §2`：明确说"`ntyco` 当前实现中每连接协程使用阻塞 `poll/recv/send`，不适合作为路线 C 基础"。

**解释**：`08` 的判断是"不适合作为路线 C 基础"（not suitable as Route C foundation），意思是 ntyco 的协程模型不适合作为路线 C 多 poller 的底层基础。但这不等于"ntyco 被永久排除在 executor coverage 之外"。原始文档没有明确说"ntyco 可以永久绕过 executor"。

### 3.9 对 eBPF 的原始态度

原始文档（00–14）没有专门讨论 eBPF REPLSYNC / is_replication fallback 路径。复制路径的讨论集中在 TCP sync（`src/distributed/distributed.c`）。这是因为 eBPF syncd 是独立同步路径，原始设计中未将其作为独立的 store 访问 owner 进行建模。

## 4. 核心问题回答

### Q A: Route C 原始最终目标是否要求所有 shared state path 都进入 single command executor？

**是。** 原始文档在多个维度上一致要求：

1. 三层所有权模型（00 §3）：commit executor 是 store/TTL/HNSW/AOF/RDB/replication 的"唯一"访问者。
2. 后台任务 executor 化（07 §1）："所有后台 store 访问路径 executor 化"。
3. 持久化复制纳入状态流（11 结论）："必须纳入同一条命令状态流"。
4. rwlock removal 标准（10 §3）：逐路径列出条件，无"部分路径可例外"的条款。
5. 设计审批（14 §9）："所有 live state 访问收敛后"才能删除锁。

用词一致性：原始文档中的修饰词始终是"所有"（all）、"必须"（must）、"不得"（must not）、"唯一"（sole）。从未出现"主要"、"在 reactor/proactor profile 下"、"除 ntyco/eBPF 外"等限定。

### Q B: 原始文档是否允许长期存在 legacy sync path？

**不允许。** 原始文档的设计假设是：所有修改 live state 的路径最终都通过 commit executor。阶段 0.5（02）的绕锁修复被明确定义为"短期 bug fix"，后续路线 C 的长期方案是 executor 化。原始文档没有为任何路径设立"永久 legacy"分类。

### Q C: 原始文档对 ntyco 这类非 reactor/proactor async 路径是否有明确处理策略？

**没有完全明确的处理策略。** 原始文档承认 ntyco 的协程模型"不适合作为路线 C 基础"（08 §2），但没有明确说 ntyco 应该：

- (a) 被改造成 async executor 模型
- (b) 被标记为 legacy/no-rwlock-removal 模式
- (c) 被废弃/移除

原始文档在 ntyco 问题上留下了策略空白。但从整体设计精神（所有路径 executor 化）来看，(a) 最符合原设计方向，(b) 是务实妥协。

### Q D: 原始文档对 replication / REPLSYNC / full sync / slave apply 是否要求 executor-owned？

**是，全部要求。** 文档 11 明确列出：

- Master full sync → executor snapshot_task
- Slave apply → replicated_command_task（"不得绕过 executor"）
- 复制顺序必须等于 commit 顺序

但需注意：原始文档讨论的复制路径主要是 TCP sync（`distributed.c`），eBPF REPLSYNC 网络 fallback 路径未被原始设计显式覆盖。然而，从原则上看，eBPF 复制路径同样访问 live store 并执行写命令，应适用相同的 executor-owned 要求。

### Q E: 原始文档对 RDB/AOF/full sync snapshot 是否要求 executor-owned？

**是。** 文档 07 和 11 一致要求：

- RDB save → executor snapshot_task
- AOF rewrite → executor snapshot_task
- Master full sync → executor snapshot_task

当前 Stage 10.5A/10.6/10.7 已完成这些收敛。

### Q F: 原始文档对 rwlock removal criteria 的要求是"全路径 executor-owned"，还是"某个 profile 内 executor-owned"？

**全路径 executor-owned。** 文档 10 列出的 12 个硬条件中没有任何一个是按 network_mode 或 profile 分类的。每个条件是对路径类型的全局断言（例如"普通 KV handler 不再被网络线程直接调用"——这是一个全称判断，不区分是哪个网络线程）。

文档 10 的原则是：

```text
只有当所有 live state 访问都能归属到 executor 或明确 snapshot owner，才能移除全局读写锁。
```

如果 eBPF 网络线程或 ntyco coroutine 仍可以直接调用 handler，就不能说"所有 live state 访问都能归属到 executor"。

### Q G: 当前 Stage 10.11 的结论是否与原始 Route C 目标一致？

**部分一致，但在结论框架上存在偏差。**

一致的部分：

- Stage 10.11 正确识别了所有仍绕过 executor 的路径（ntyco、eBPF replication fallback）。
- Stage 10.11 正确判断全局 `NOT_READY` for unconditional rwlock removal。
- Stage 10.11 正确列出了后续需要的阶段（10.12 eBPF policy、10.13 ntyco hardening）。

偏差的部分：

- Stage 10.11 引入了"profile-based readiness"概念（`CONDITIONALLY_READY for reactor/proactor normal async command path`），这在原始文档中不存在。
- 原始文档的 readiness 判断是二元的：要么所有路径收敛完（READY），要么没有（NOT_READY）。原始文档没有"某个 profile 下 READY"的中间状态。
- Stage 10.11 将 eBPF 和 ntyco 标记为"legacy/no-rwlock-removal path"，暗示它们可以长期绕过 executor。这与原始文档的"所有路径必须收敛"精神存在张力。

### Q H: 如果不一致，偏差在哪里？

偏差主要体现在两个方面：

1. **Readiness 粒度的偏差**：原始文档的 rwlock removal criteria 是全局二值判断，Stage 10.11 引入了 per-profile 的部分 readiness 判断。这是一种务实细化，但改变了原始标准的语义——从"所有路径收敛"变为"主流路径收敛 + legacy 隔离"。

2. **对 legacy path 的定位偏差**：原始文档将阶段 0.5 的绕锁修复描述为"短期 bug fix"，暗示后续都要 executor 化。Stage 10.11 则将 eBPF/ntyco 描述为"legacy/no-rwlock-removal mode"，暗示可以长期不 executor 化。从"短期绕锁修复"到"长期 legacy 模式"是一个定位变化。

3. **eBPF 路径的原始设计空白**：原始文档未显式覆盖 eBPF REPLSYNC 路径，这是一个原始设计空白，不是偏差。但这个空白需要在当前阶段被填充——是按原始精神扩展覆盖，还是承认空白并隔离。

## 5. 当前 Stage 9–10.11 完成情况对照

### 5.1 原始路线图 vs 实际执行

| 原始阶段 | 原始目标 | 实际完成情况 |
|---------|---------|------------|
| Stage 0 | 锁点审计 | ✅ 已完成（01） |
| Stage 0.5 | 绕锁路径修复 | ✅ 已完成（02），后续被 executor 化替代 |
| Stage 1 | embedding 锁外 prepare | ✅ 已完成（03） |
| Stage 2 | command_task 抽象 | ✅ 已完成（04） |
| Stage 3 | response_task 抽象 | ✅ 已完成（05） |
| Stage 4 | reactor-only single executor | ✅ 已完成（06，已扩展到 proactor） |
| Stage 5 | 后台任务 executor 化 | ✅ 大部分完成（见下文） |
| Stage 6 | 多 poller | ⏸️ 未做（嵌入到 proactor adapter 中） |
| Stage 7 | embedding worker pool | ✅ 已完成（Stage 9） |
| Stage 8 | rwlock removal | ⏸️ 未做（当前在 Stage 10 审计阶段） |

### 5.2 原始 rwlock removal criteria 逐条对照

| 原始条件（10_rwlock_removal_criteria.md） | 当前状态 | 评估 |
|---|---|---|
| 普通 KV handler 不再被网络线程直接调用 | reactor/proactor normal ✅, ntyco ❌, eBPF repl ❌ | **未满足** |
| Vector handler 不再被网络线程直接调用 | 同上 | **未满足** |
| TTL active expire 不再直接访问 store | ✅ Stage 10.10 | 已满足 |
| lazy expire 只在 executor 内完成 | reactor/proactor normal ✅, ntyco ❌, eBPF repl ❌ | **未满足** |
| RDB save 不直接遍历 live store | ✅ Stage 10.6 | 已满足 |
| AOF rewrite 不直接遍历 live store | ✅ Stage 10.7 | 已满足 |
| master full sync 不直接遍历 live store | ✅ Stage 10.5A | 已满足 |
| AOF append 顺序由 executor 产生 | ✅ normal async + TCP slave apply | 已满足 |
| slave apply 通过 replicated_command_task | ✅ Stage 10.3 | 已满足 |
| shutdown/destroy 在 executor drain 后执行 | ✅ Stage 10.9/10.9A | 已满足 |
| vector store/id_map/HNSW 所有访问有明确 owner | 主流路径 ✅, ntyco/eBPF repl ❌ | **未满足** |
| TSAN 或等价竞态检测通过 | 部分（有已知 background/shutdown race） | 部分满足 |

### 5.3 当前已完成的收敛路径（9 项）

1. Reactor normal client command → ✅ executor
2. Proactor normal client command → ✅ executor
3. Embedding prepare worker → ✅ executor (commit)
4. TTL active expire → ✅ executor (scan task, Stage 10.10)
5. TTL lazy expire (reactor/proactor normal) → ✅ executor
6. TCP slave apply → ✅ executor (replicated_command_task, Stage 10.3)
7. Master full sync snapshot → ✅ executor (Stage 10.5A)
8. RDB save snapshot → ✅ executor (Stage 10.6)
9. AOF rewrite snapshot → ✅ executor (Stage 10.7)

### 5.4 当前仍绕过 executor 的路径（3 项）

1. **Reactor eBPF REPLSYNC/is_replication fallback**：通过 `net_session_process_resp → kvs_handler → kvs_filter_protocol → kvs_command_task_execute_sync`，在 reactor epoll 线程中同步执行完整命令语义（包括写命令、TTL lazy expire、vector 写）。

2. **Proactor eBPF REPLSYNC/is_replication fallback**：对称路径，在 proactor io_uring loop 线程中同步执行。

3. **Ntyco 同步模式**：所有命令在 coroutine 中通过 `kvs_filter_protocol` 同步执行。覆盖 100% 的 ntyco 客户端命令。

### 5.5 已不再是 blocker 的路径

- Proactor async build/submit failure：Stage 10.8A 后已改为 fail-fast + 关闭连接，不再 fallback 到同步 handler。
- TCP slave connect thread owner：Stage 10.9A 后已 joinable。
- Shutdown save/stop/destroy 边界：Stage 10.9 后已收敛。
- TTL scanner runtime：Stage 10.10 后已完全由 executor 执行 scan/check/delete。

## 6. 当前剩余路径详细分类

| # | 路径 | 当前 executor-owned? | 访问 shared state? | 仍绕过 executor? | 原始 Route C 必须改造? | 可长期 legacy? | 推荐处理 |
|---|------|---------------------|-------------------|-----------------|----------------------|---------------|---------|
| 1 | Reactor normal client cmd | ✅ | 是 | 否 | 已完成 | N/A | 已完成 |
| 2 | Proactor normal client cmd | ✅ | 是 | 否 | 已完成 | N/A | 已完成 |
| 3 | Embedding prepare worker | ✅ | 否（prepare 阶段不访问 store） | 否 | 已完成 | N/A | 已完成 |
| 4 | TTL active expire | ✅ | 是 | 否 | 已完成 | N/A | 已完成 |
| 5 | TTL lazy expire (normal) | ✅ | 是 | 否 | 已完成 | N/A | 已完成 |
| 6 | TCP slave apply | ✅ | 是 | 否 | 已完成 | N/A | 已完成 |
| 7 | Master full sync snapshot | ✅ | 是（snapshot） | 否 | 已完成 | N/A | 已完成 |
| 8 | RDB save snapshot | ✅ | 是（snapshot） | 否 | 已完成 | N/A | 已完成 |
| 9 | AOF rewrite snapshot | ✅ | 是（snapshot） | 否 | 已完成 | N/A | 已完成 |
| 10 | Proactor async failure | ✅ | 否（直接关闭连接） | 否 | 已完成（10.8A） | N/A | 已完成 |
| 11 | Shutdown save/stop/destroy | ✅ | 是（已收敛顺序） | 否 | 已完成 | N/A | 已完成 |
| 12 | Slave connect thread owner | ✅ | 否（只提交 replicated task） | 否 | 已完成 | N/A | 已完成 |
| 13 | Reactor eBPF REPLSYNC fallback | ❌ | 是 | 是 | **是**（按原始原则） | **否**（访问 shared state） | Stage 10.12 executor 化 |
| 14 | Proactor eBPF REPLSYNC fallback | ❌ | 是 | 是 | **是**（按原始原则） | **否**（访问 shared state） | Stage 10.12 executor 化 |
| 15 | Ntyco sync mode | ❌ | 是 | 是 | **原始文档未明确** | **可争议** | Stage 10.13 决策 |
| 16 | Executor 内 lock sites | ✅（但持锁） | N/A | N/A | 是（rwlock removal 的 mechanical 替换） | N/A | Stage 11 |

## 7. 方案 A：Full Executor Coverage 分析

### 7.1 含义

所有 shared state read/write/snapshot/mutation 路径最终都必须接入 single command executor。eBPF replication fallback 和 ntyco 不能长期 legacy，只能作为待改造路径。

### 7.2 是否符合原始 Route C 文档

**高度符合。** 原始文档的"所有路径 executor 化"、"唯一访问 store"、"必须纳入同一状态流"等表述，在精神上和字面上都支持 Full Executor Coverage。原始文档从未为任何路径设立"永久例外"类别。

但需要注意：原始文档对 eBPF REPLSYNC 路径没有显式覆盖（原始设计空白），对 ntyco 只有"不适合作为路线 C 基础"的技术判断，没有"应永久排除"的策略判断。

### 7.3 改造成本

#### eBPF REPLSYNC executor 化

| 改造项 | 复杂度 | 风险 | 预估工时 |
|--------|--------|------|---------|
| REPLSYNC 握手保持同步，不访问 store | 低 | 低 | 0.5 天 |
| 后续复制命令构造 owned command_task + 深拷贝 argv | 低 | 低 | 0.5 天 |
| 设置 task->sync_context = 2, task->is_replication = 1 | 低 | 低 | 0.2 天 |
| executor 执行时临时 set/get_sync_context | 中 | 中 | 0.5 天 |
| Response 回投：复制连接不需要 response（send_buf->size = 0） | 低 | 低 | 0.2 天 |
| 回归测试（eBPF sync 功能 + 性能） | 中 | 中 | 1 天 |
| **小计（reactor + proactor 各一份）** | | | **~3-4 天** |

#### Ntyco executor 化

| 改造项 | 复杂度 | 风险 | 预估工时 |
|--------|--------|------|---------|
| heap-alloc task + copy_argv | 低 | 低 | 0.5 天 |
| executor submit 路径 | 中 | 低 | 0.5 天 |
| response 回调/通知机制 | **高** | **高** | 2-3 天 |
| 协程 yield/resume 跨线程 | **很高** | **很高** | 3-5 天 |
| replication 路径 | 中 | 中 | 1 天 |
| 测试 + 回归 | 高 | 高 | 2-3 天 |
| **小计** | | | **9-13 天** |

ntyco 的核心风险在于 NtyCo 库可能不支持跨线程 coroutine resume。如果支持，改造可行但成本高；如果不支持，需要更换协程库或重写 ntyco 网络栈，成本会更高。

### 7.4 风险

1. **NtyCo 库跨线程支持风险**：如果 NtyCo 不支持跨线程 resume，ntyco executor 化可能变成重写 ntyco 网络栈，成本远超预估。

2. **eBPF sync_context 语义保持风险**：当前 `sync_context` 是 thread-local 变量。executor 执行 eBPF 复制命令时需要正确设置/恢复，否则从节点只读检查可能错误拒绝或接受命令。

3. **eBPF 复制 response 语义风险**：当前 eBPF 复制连接不产生 response（`send_buf->size = 0`）。改为 executor 后，response_task 需要正确处理空响应。

4. **性能退化风险**：eBPF 复制路径从零拷贝（直接引用 parser->tokens）变为深拷贝 argv + 跨线程提交，可能增加复制延迟。

### 7.5 阶段拆分建议

```
Stage 10.12: eBPF REPLSYNC executorization（reactor + proactor）
  - REPLSYNC 握手仍同步返回 +OK（不访问 store，不依赖 rwlock）
  - 复制命令封装为 owned command_task，提交 executor
  - executor 内设置 sync_context = 2 保持复制语义
  - Response 回投正确处理空响应

Stage 10.13: Ntyco executorization 或 permanent exclusion 决策
  - 先审计 NtyCo 库跨线程 resume 能力
  - 如果可行：ntyco async executor 改造
  - 如果不可行：ntyco 标记为 legacy/no-rwlock-removal mode（方案 B 对 ntyco 的部分）

Stage 10.14: executor-only build profile audit
  - 审计 reactor/proactor + TCP replication + snapshots + TTL scanner 组合
  - 确认所有 shared state 访问点已收敛

Stage 11: rwlock weakening/removal（在 Stage 10.12 完成后）
```

### 7.6 是否应推迟 Stage 11 rwlock weakening

**是。** 在 eBPF REPLSYNC 路径 executor 化完成之前，`g_kv_data_rwlock` 不能弱化或移除。因为 reactor epoll 线程和 proactor io_uring loop 线程仍可通过 eBPF replication fallback 直接执行写命令和 lazy expire——这些操作与 executor 内的操作并发时需要 rwlock 保护。

## 8. 方案 B：Owner-safe Profile + Legacy Isolation 分析

### 8.1 含义

只在 reactor/proactor normal async owner-safe profile 下保证 executor-owned。eBPF replication fallback / ntyco 作为 legacy/no-rwlock-removal profile 长期存在。

在此方案下，`g_kv_data_rwlock` 可以弱化为：
- reactor/proactor normal async profile：executor 内不再需要 rwlock（因为 executor 是唯一 owner），可以替换为 owner assertion 或 no-op。
- eBPF replication / ntyco legacy profile：保留完整 rwlock 保护。

### 8.2 是否符合原始 Route C 文档

**不完全符合。** 原始文档的要求是"所有 live state 访问都能归属到 executor 或明确 snapshot owner"，这是一个全局条件，不区分 profile。方案 B 引入的 profile-based 分层在原始文档中没有对应概念。

具体偏离点：

1. 原始文档的 rwlock removal criteria 是逐路径的全局条件，不是 per-profile 的条件。
2. 原始文档的"短期 bug fix → 长期 executor 化"路径暗示所有绕锁路径最终都要收敛，方案 B 将某些路径标记为"永久 legacy"。
3. 原始文档没有"owner-safe profile"这个概念。

但需承认：原始文档对 eBPF REPLSYNC 没有显式覆盖，对 ntyco 只有技术判断没有策略结论。在这个意义上，方案 B 是在原始设计空白处的务实填充，而不是对已有明确结论的否定。

### 8.3 是否会偏离用户"所有路径统一执行器"的最终目标

**会偏离。** 用户明确倾向是"所有 shared state 访问路径最终都接入统一执行器"。方案 B 将 eBPF replication 和 ntyco 永久隔离在 executor 之外，与用户目标矛盾。

但方案 B 可以是阶段性方案而非最终方案：
- 短期：隔离 eBPF/ntyco，推进 reactor/proactor 主线的 rwlock weakening。
- 长期：在主线稳定后回头改造 eBPF/ntyco。

这种"分阶段收敛"的策略在原始路线图中也有先例（如阶段 0.5 的短期锁修复 → 长期 executor 化）。

### 8.4 对 rwlock removal 的影响

如果采用方案 B，rwlock removal 面临以下选择：

**选择 B1：Profile-conditional rwlock removal**

- reactor/proactor normal async 路径：executor 内不再持 rwlock。
- eBPF replication / ntyco 路径：保留完整 rwlock 保护。
- 三种 network_mode 互斥运行，同时只有一个 profile 生效。
- 在编译或启动时选择 profile，启用对应的锁策略。

风险：
- 代码中出现 `if (profile == OWNER_SAFE) { /* no lock */ } else { /* take lock */ }` 的分支。
- 测试矩阵扩大（每个 profile 需要独立验证）。
- 如果未来 eBPF/ntyco 也要去锁，之前的"去锁"代码可能需要再次重构。

**选择 B2：暂不进行任何 rwlock removal，只在文档层面隔离**

- 保持 `g_kv_data_rwlock` 在所有路径中生效。
- 在文档中声明 reactor/proactor normal async 路径已"executor-safe"，但锁仍保留作为安全网。
- 等 eBPF/ntyco 改造完成后再统一去锁。

这是最保守但最安全的选项。

### 8.5 是否只是局部 profile 的 rwlock weakening，而不是全局架构完成

**是。** 方案 B 本质上是"在主流路径上提前收获性能收益，但暂不完成全局架构"。这是一个工程权衡：用架构完整性换取更早的性能验证。

利弊：
- 利：可以更早验证 rwlock removal 对 reactor/proactor 的性能收益。
- 弊：如果 eBPF/ntyco 长期不能去锁，系统将长期存在两套并行的锁策略，增加维护复杂度。

## 9. 两种方案对比总结

| 维度 | 方案 A: Full Executor Coverage | 方案 B: Owner-safe Profile |
|------|-------------------------------|--------------------------|
| 与原始 Route C 文档一致性 | **高** — 符合"所有路径 executor 化" | **中** — 偏离了"所有路径"的全局语义 |
| 与用户"所有路径统一执行器"目标一致性 | **高** | **低**（但可作为阶段性方案） |
| eBPF 路径处理 | executor 化（3-4 天） | 长期 legacy 隔离 |
| ntyco 路径处理 | executor 化或废弃（9-13 天） | 长期 legacy 隔离 |
| rwlock removal 时机 | eBPF executor 化后即可推进 | reactor/proactor 路径可先行 |
| 架构完整性 | 最终所有路径统一 | 长期存在两套锁策略 |
| 工程风险 | eBPF: 中, ntyco: 高 | 低（不改动 legacy 路径） |
| 回归测试范围 | 全路径 | 仅 reactor/proactor normal async |

## 10. 当前剩余 Blocker 汇总

### Active Blocker（阻止 rwlock removal）

| # | Blocker | 路径 | 是否访问 shared state | 当前保护 | 推荐 Stage |
|---|---------|------|----------------------|---------|-----------|
| 1 | Reactor eBPF REPLSYNC fallback | `reactor.c` sync handler | 是（写命令+TTL+vector） | `g_kv_data_rwlock` | 10.12 |
| 2 | Proactor eBPF REPLSYNC fallback | `proactor.c` sync handler | 是（写命令+TTL+vector） | `g_kv_data_rwlock` | 10.12 |
| 3 | Ntyco sync mode | `ntyco.c` coroutine | 是（全部命令） | `g_kv_data_rwlock` | 10.13 决策 |

### Resolved Blocker（已不是 blocker）

| # | 原 Blocker | 解决阶段 | 解决方式 |
|---|-----------|---------|---------|
| 1 | Master full sync 遍历 live store | 10.5A | executor snapshot_task |
| 2 | RDB save 遍历 live store | 10.6 | executor snapshot_task |
| 3 | AOF rewrite 遍历 live store | 10.7 | executor snapshot_task |
| 4 | TCP slave apply 直接修改 store | 10.3 | replicated_command_task |
| 5 | TTL active expire 直接扫描 store | 10.10 | executor scan task |
| 6 | TTL scanner 读 live store | 10.10 | 完全 executor-owned |
| 7 | Proactor async failure fallback | 10.8A | fail-fast close |
| 8 | Shutdown owner boundary | 10.9 | 顺序收敛 |
| 9 | Slave connect thread owner | 10.9A | joinable |

### Mechanical Blocker（锁仍保留的 executor 内 lock sites）

这些不是 owner bypass 问题，而是 future mechanical replacement：

| # | Lock site | 当前状态 | 未来处理 |
|---|-----------|---------|---------|
| 1 | `kvs_command_task_lock` (normal cmd) | executor 内 rdlock/wrlock | Stage 11 替换 |
| 2 | `kvs_command_task_execute_sync` (lazy expire retry) | executor 内 rdlock→wrlock 升级 | Stage 11 替换 |
| 3 | `kvs_command_executor_execute_repl` | executor 内 wrlock | Stage 11 替换 |
| 4 | `kvs_command_executor_execute_ttl` | executor 内 wrlock | Stage 11 替换 |
| 5 | `kvs_command_executor_execute_ttl_scan` | executor 内 wrlock | Stage 11 替换 |
| 6 | Full sync / RDB / AOF snapshot builders | executor 内 rdlock | Stage 11 替换 |
| 7 | `dest_kvengine` destroy | shutdown 路径 wrlock | Stage 11 替换 |

## 11. 推荐结论

### 11.1 最终结论

**结论 1：原始 Route C 明确要求 Full Executor Coverage。**

原始文档在 00、07、10、11、13、14 中一致的用词和逻辑表明：最终目标是将所有 shared state 访问路径收敛到 single command executor。"所有"、"必须"、"不得"、"唯一"等修饰词在全套文档中保持一致，从未出现 per-profile 例外。

**但建议在推进策略上采用务实的分阶段方法**：

1. **先做 eBPF executor 化（Stage 10.12）**：这是成本可控（3-4 天）、风险中等、且原始文档精神明确要求的改造。
2. **ntyco 分决策（Stage 10.13）**：先审计 NtyCo 库跨线程 resume 能力，再决定是 executor 化还是 permanent exclusion。
3. **eBPF 完成后再进入 Stage 11 rwlock weakening**：不建议在 eBPF 路径仍绕过 executor 时弱化 rwlock。

### 11.2 不建议的路线

- ❌ 在 eBPF REPLSYNC 路径仍绕过 executor 的情况下进入 Stage 11 rwlock weakening（方案 B 的选择 B1）。
- ❌ 将 eBPF 和 ntyco 一次性全部 executor 化（成本过高，应分阶段）。

### 11.3 推荐下一阶段路线

```
Stage 10.12: eBPF REPLSYNC executorization
  - Reactor eBPF REPLSYNC → owned command_task → executor
  - Proactor eBPF REPLSYNC → owned command_task → executor
  - REPLSYNC 握手仍同步返回 +OK
  - sync_context 通过 task flag 传递
  - 完成后：eBPF 路径不再是 rwlock removal blocker

Stage 10.13: Ntyco final strategy decision
  - 审计 NtyCo 库跨线程 coroutine resume 能力
  - 如果可行：ntyco async executor 改造
  - 如果不可行：ntyco permanent legacy + 文档明确排除在 rwlock removal 外
  - 如果是 permanent legacy：ntyco 启动时要求 g_kv_data_rwlock 存在

Stage 10.14: Pre-Stage-11 final readiness audit
  - 全路径 owner 收敛确认
  - executor 内 lock sites mechanical replacement plan
  - compile/runtime guard 设计方案

Stage 11: rwlock weakening / removal
  - 仅在 Stage 10.12 完成 + Stage 10.13 决策后进入
  - executor 内 lock sites 替换为 owner assertion
  - 保留 rwlock 回滚开关
```

### 11.4 是否建议进入 Stage 11

**当前不建议。** 需要先完成 Stage 10.12（eBPF executor 化），然后再评估。

具体条件：
- Stage 10.12 完成后：如果 ntyco 也已经有明确策略（即使是被排除），可以进入 Stage 11 的 reactor/proactor profile rwlock weakening。
- 如果希望 Full Executor Coverage 的完整架构完成后再去锁：需要等 Stage 10.12 + 10.13 全部完成。

### 11.5 对 eBPF replication fallback 的后续建议

**推荐方案 A（executor 化）**，理由：

1. 原始 Route C 文档精神要求所有 shared state 路径 executor-owned。
2. 改造成本可控（3-4 天），风险中等。
3. eBPF 复制命令语义与 TCP slave apply 高度相似（都是复制命令 apply），TCP slave apply 已在 Stage 10.3 成功 executor 化，eBPF 路径可以复用相同模式。
4. `sync_context` 的 thread-local 语义可以封装为 task flag，executor 在执行时设置/恢复。
5. eBPF 复制连接不需要 response（`send_buf->size = 0`），这简化了 response 回投逻辑。

### 11.6 对 ntyco 的后续建议

**推荐先审计 NtyCo 库再做决策**，理由：

1. Stage 10.4 的成本估算（9-13 天）高度依赖 NtyCo 库的跨线程 resume 能力。
2. 如果 NtyCo 支持跨线程 resume，ntyco executor 化的实际成本可能低于预期（可以复用现有 command_task + executor 基础设施）。
3. 如果 NtyCo 不支持，需要在"重写 ntyco 网络栈"和"永久 legacy isolation"之间选择。
4. 用户最终倾向是"所有路径统一执行器"，但如果 NtyCo 库技术限制无法逾越，permanent legacy exclusion 是可接受的务实选择。

**不论最终决策如何，ntyco 的定位需要在文档中明确，且不能阻塞 reactor/proactor 主线的 rwlock weakening 推进。**

## 12. 验证

### 12.1 是否修改生产代码

**否。** 本阶段没有修改任何 `src/` 或 `include/` 下的生产代码。只新增了审计文档。

### 12.2 文件变更

```text
新增：
  docs/route_c_perf_arch/57_stage10_12_pre_route_c_original_plan_reconciliation.md
```

### 12.3 是否新增测试脚本

**否。** 本阶段是纯分析审计阶段，不新增测试脚本。

### 12.4 静态检查

```bash
git status --short
git diff --check
```

预期输出：
- 仅显示新增的 `57_*.md` 文件。
- `git diff --check` 无警告。

## 13. 建议 commit message

```
复盘路线C原始执行器目标

Stage 10.12-pre：回到 00-14 号原始 Route C 设计文档，逐项对照当前
Stage 9-10.11 的完成情况，回答核心争议问题：

1. 原始 Route C 最终目标是否要求所有 shared state path 进入
   single command executor？
   → 是。原始文档的用词始终是"所有"、"必须"、"不得"、"唯一"，
     没有 per-profile 例外。

2. 原始文档是否允许长期存在 legacy sync path？
   → 不允许。阶段 0.5 的绕锁修复被明确定义为"短期 bug fix"，
     长期都要求 executor 化。

3. 原始文档对 ntyco/eBPF 是否有明确处理策略？
   → 原始文档承认 ntyco"不适合作为路线 C 基础"，但对 eBPF
     REPLSYNC 路径存在设计空白。

4. 当前 Stage 10.11 的结论是否与原始目标一致？
   → 部分一致。正确识别了剩余 blocker，但引入了原始文档中
     不存在的"profile-based readiness"概念。

推荐结论 1：原始 Route C 明确要求 Full Executor Coverage。
后续应先完成 Stage 10.12（eBPF executor 化），再评估是否进入
Stage 11 rwlock weakening。

Co-Authored-By: Claude <noreply@anthropic.com>
```

## 14. 附录：原始文档关键语句索引

| 关键语句 | 来源文档 | 章节 |
|---------|---------|------|
| "commit executor，唯一访问 store/TTL/HNSW/AOF/RDB/replication" | 00_route_c_overview.md | §3 三层所有权模型 |
| "把所有后台 store 访问路径 executor 化" | 07_background_tasks_executor_design.md | §1 本阶段目标 |
| "持久化和主从同步不是附属模块，必须纳入同一条命令状态流" | 11_persistence_replication_under_route_c.md | 结论 |
| "slave apply 不得绕过 executor" | 11_persistence_replication_under_route_c.md | §3 设计方案 |
| "master full sync 不允许 sync thread 直接遍历 live store" | 11_persistence_replication_under_route_c.md | §3 设计方案 |
| "删除锁是最后一步，不是优化起点" | 10_rwlock_removal_criteria.md | §3 设计方案 |
| "只有当所有 live state 访问都能归属到 executor 或明确 snapshot owner，才能移除全局读写锁" | 10_rwlock_removal_criteria.md | §6 正确性边界 |
| "最终删除或弱化 g_kv_data_rwlock：合理，但只能在所有 live state 访问收敛后做" | 14_route_c_design_review.md | §9 对长期路线 C 的审批 |
| "ntyco 当前实现中每连接协程使用阻塞 poll/recv/send，不适合作为路线 C 基础" | 08_multi_poller_queue_design.md | §2 当前代码现状 |
| "第一版不支持：proactor、ntyco" | 06_single_command_executor_design.md | §4 不修改范围（阶段限制，非永久排除） |
