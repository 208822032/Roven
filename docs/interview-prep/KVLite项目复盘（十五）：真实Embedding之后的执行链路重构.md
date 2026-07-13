# 第十五章 真实 Embedding 之后的执行链路重构：锁外 Prepare、单写者 Commit 与所有权收敛

## 15.1 上一阶段留下的问题：模型已经接进来，但执行链路还没准备好

第十四章解决的是“模型如何接进来”。

那一阶段把 hash embedding 的工程占位推进到了可选 ONNX 本地推理闭环：C ABI、默认 mock runtime、可选 ONNX Runtime、最小 tokenizer、三输入 tensor、`OrtRun`、`sentence_embedding` 输出校验和 semantic smoke 都已经落地。这些内容在第十五章只作为背景，不再重复展开。

它同时留下了一个更现实的问题：真实 embedding 不再是便宜的 hash 计算。

在 `b43ec18` 这个时间点，命令执行链路仍然是旧模型。直接证据是 [`b43ec18` 时的 `kvs_filter_protocol()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/b43ec18/src/core/kvstore.c#L49-L125)：函数根据 `cmd->lock_mode` 先拿 `g_kv_data_rwlock`，再调用 `cmd->handler(&ctx, response)`。

```text
网络线程 / 协程
→ RESP parse
→ kvs_filter_protocol()
→ 先拿 g_kv_data_rwlock
→ cmd->handler()
→ VSET / VSEARCH 内部同步生成 embedding
→ 修改 record / index 或执行 search
→ 释放锁
→ AOF / replication / response
```

VSET 的 embedding 也确实发生在 handler 里面。[`b43ec18` 时的 `kvs_vector_record_set_fields()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/b43ec18/src/vector/kvs_vector_index.c#L218-L305) 会在初始化 record 字段时调用 `kvs_vector_embedding_generate()`，随后才写入 `record->id`、`question`、`answer`、`embedding` 等可见状态。VSEARCH 同样在 [`b43ec18` 时的 `kvs_vector_search()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/b43ec18/src/vector/kvs_vector.c#L1324-L1395) 里同步生成 query embedding，然后进入索引搜索。

这意味着真实 ONNX 推理接入后，命令锁里混进了这些成本：

```text
tokenizer
tensor 创建
OrtRun
output copy
L2 normalize
record / id_map / HNSW mutation
response assembly
```

第十五章的核心问题就从“能不能跑 ONNX”变成了：

```text
真实模型推理变慢后，
怎样把纯计算从全局状态锁里拆出去，
同时保证 record / TTL / AOF / RDB / replication / HNSW
仍然由一个清楚的状态所有者顺序提交？
```

本章对应的 Git 范围是：

```text
b43ec18..33a886f
```

其中核心实现闭环到 `01b50c1`，`33a886f` 主要是 Route C 文档归档、Route C 专项测试脚本清理和目录整理。[`route_c_final_report.md`](../route_c/route_c_final_report.md) 是阶段总报告，[`route_c_test_script_removal_report.md`](../route_c/route_c_test_script_removal_report.md) 明确说明 Route C 专项脚本已经删除、验证证据保留在 `docs/route_c/` 文档中。因此本章引用测试时必须区分“历史脚本曾存在并通过”和“当前 HEAD 仍存在的测试文件”。

## 15.2 Git 范围与 Route C 真实演进主线

这一章不能按 commit 拆。`b43ec18` 之后的连续提交，表面上有锁补洞、prepare/commit、executor、proactor、worker pool、batch、TTL、复制、RDB/AOF、shutdown、weak-lock 等很多 Stage，但它们实际在回答同一个工程问题：

```text
谁可以访问全局状态？
哪些工作可以锁外做？
哪些工作必须回到单写者顺序提交？
后台任务和网络线程能不能绕开这个顺序？
```

Route C 原始架构在 [`00_route_c_overview.md`](../route_c/archive/stage_docs/00_route_c_overview.md) 中已经定好方向：

```text
network poller
→ RESP parse
→ command_task
→ classify/router
→ embedding worker pool / commit executor
→ response_task
→ original poller send
```

完整执行路线可以压缩成这样：

```text
00ce1dd/b385149/6cc8278/2dec88d/90b1072
→ 先补 master full sync、RDB、AOF rewrite 遍历全局存储的锁边界

c6615c4
→ VSET / VSEARCH 拆出 prepare，embedding 生成移出全局数据锁

36a4c38/31ab477/827c0f5/e4b02cb
→ command_task / response_task / session 生命周期抽象

b6c89ba/da5699f/354d8ce/1861867
→ reactor 单线程 executor 和异步响应适配层稳定

77375e1/0db4f00/5b64d56/47705cd
→ proactor 接入 executor eventfd 唤醒和异步执行闭环

93e2e7b/6338e9f/f57dd5e/2db1f4b/4e9ff06/a350b1f/b1ac888
→ embedding prepare worker pool、per-thread runtime、batch 尝试、瓶颈观测和异常路径测试

13a629a/6da5e83/efe23bc/f466ae7/ec7c1cc/ac05a9e/b6de93d/63bd9e2/b851923
→ TTL、复制、full sync、RDB、AOF rewrite、shutdown、NtyCo、eBPF fallback 路径收敛到 executor

b1feec1/b067970/01b50c1
→ owner rwlock wrapper、weak-lock gate、NtyCo vendoring、TSAN / mixed stress / final closeout

33a886f
→ Route C 文档、测试脚本删除报告和目录整理；不引入新的执行链路能力
```

所以本章不是“worker pool 优化篇”，也不是“TTL executor 篇”。它的主题是 Route C：把状态访问从网络线程和后台线程里收回来，形成一个以 executor 为中心的所有权模型。

阶段前后的主调用链差异可以这样概括。

早期：

```text
network poller / coroutine
→ RESP parse
→ kvs_filter_protocol()
→ g_kv_data_rwlock
→ cmd->handler()
→ VSET / VSEARCH 同步 embedding
→ record / index / TTL / AOF / replication
→ response
```

后期：

```text
network poller / coroutine
→ RESP parse
→ kvs_command_task_build()
→ command executor queue
→ 需要 embedding 的任务先进入 prepare
→ embedding worker pool 做纯计算
→ prepared task 回投 executor
→ executor 单线程 commit / search / TTL / AOF / RDB / replication
→ response_task 回到网络层发送
```

[`route_c_final_report.md`](../route_c/route_c_final_report.md) 对最终架构的描述也是这个方向：网络层只负责 socket / recv / send / RESP parse，计算层负责 embedding prepare，状态层由 commit executor 作为访问 store、TTL、HNSW、AOF/RDB、replication 的 owner。

## 15.3 第一版修补：先承认旧链路有绕锁路径

Route C 不是一上来就写 executor。

最早几次提交先处理的是更基础的问题：有些后台路径会遍历全局存储，但没有统一进入 `kvs_filter_protocol()` 的锁边界。这个问题在 [`01_lock_and_shared_state_audit.md`](../route_c/archive/stage_docs/01_lock_and_shared_state_audit.md) 和 [`02_existing_lock_bypass_fix_design.md`](../route_c/archive/stage_docs/02_existing_lock_bypass_fix_design.md) 里被单独列出。

实际提交对应如下：

- `00ce1dd` 和 `b385149` 修改 [`distributed.c`](../../src/distributed/distributed.c)，围绕 master full sync 遍历全局存储补锁和重构遍历逻辑。
- `6cc8278` 修改 [`kvs_rdb.c`](../../src/persistence/kvs_rdb.c)，处理 RDB 保存遍历全局存储的锁边界。
- `2dec88d` 和 `90b1072` 修改 [`kvs_aof_rewrite.c`](../../src/persistence/kvs_aof_rewrite.c)，处理 AOF rewrite 遍历全局存储的锁边界。

这些提交很容易被忽略，因为它们还不是最终设计。但它们说明当时已经发现一个关键事实：

```text
全局数据锁不是只服务普通命令。
RDB save、AOF rewrite、master full sync 也会读 live store。
如果这些路径绕过统一锁，就算 VSET 自己加锁也不够。
```

这一步的意义不是“加几把锁就完成了”，而是把问题从“VSET 里有 ONNX 推理”扩大到了“系统里到底有哪些状态访问者”。只有先找到所有访问者，后面才可能谈单写者模型。

它也暴露了旧方案的局限：

```text
继续给每条路径补 pthread_rwlock
→ 短期能修安全洞，但状态所有权仍然分散。

把所有状态访问收敛到 executor
→ 改动更大，但每条路径可以用同一个顺序解释。
```

后续 Route C 选择了第二条路。

## 15.4 第一次关键拆分：VSET/VSEARCH 的 Prepare 与 Commit

`c6615c4` 是本章最关键的转折点。

这个提交没有直接引入 worker pool，而是先把 VSET/VSEARCH 的纯计算阶段拆了出来。[`c6615c4` 时的 `include/kvs_vector.h`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/c6615c4/include/kvs_vector.h#L150-L203) 新增了两个 prepared 结构：

```c
kvs_vector_set_prepared_t
kvs_vector_search_prepared_t
```

同时新增了几组接口：

```c
kvs_vector_set_prepare()
kvs_vector_set_commit_prepared()
kvs_vector_search_prepare()
kvs_vector_search_with_prepared()
kvs_vector_set_prepared_destroy()
kvs_vector_search_prepared_destroy()
```

当前 HEAD 中这些接口仍在 [`include/kvs_vector.h`](../../include/kvs_vector.h) 和 [`src/vector/kvs_vector.c`](../../src/vector/kvs_vector.c)，但理解历史时要看 `c6615c4` 的第一版 diff，而不是用后续 HNSW v2 代码倒推。

这个拆分的边界很重要。

prepare 做的是：

```text
复制 question / answer / scope
生成 embedding
把结果放入 prepared 对象
不访问 vector store
不修改 id_map
不插入 flat / HNSW
不生成 record_id
```

commit 做的是：

```text
在锁内重新检查 question + scope
如果记录已经存在，只更新 answer 并丢弃 prepared embedding
如果不存在，分配或复用 record slot
生成 record_id / timestamp
更新 id_map
插入 flat 或当前 HNSW index
更新 active/deleted/total 统计
```

直接证据是 [`c6615c4` 时的 `kvs_vector_set_prepare()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/c6615c4/src/vector/kvs_vector.c#L1106-L1178) 和 [`c6615c4` 时的 `kvs_vector_set_commit_prepared()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/c6615c4/src/vector/kvs_vector.c#L1488-L1622)。前者复制字符串并调用 `kvs_vector_embedding_generate()`；后者才调用 `kvs_vector_find_record()`、`kvs_vector_find_free_slot()`、`kvs_vector_id_map_put()`、`kvs_vector_flat_index_add()` 和 `kvs_vector_index_add_current()`。

这一版还保留在同步 `kvs_filter_protocol()` 里执行，只是把 embedding 生成放到了拿锁之前：

```text
kvs_filter_protocol()
→ VSET/VSEARCH prepare
→ 拿 g_kv_data_rwlock
→ commit/search with prepared
→ unlock
```

这一步看起来小，但它确定了后面所有异步化的基本不变量：

```text
embedding 可以锁外计算；
prepare 结果不产生可见状态；
record_id / timestamp / slot / id_map / index 只能在 commit 时生成或修改；
commit 前必须重新检查 question + scope；
prepare 失败不能留下半提交 record；
prepared 对象必须有明确 destroy 路径。
```

如果没有这个拆分，后面直接上 worker pool 就会变成“多线程同时改 vector store / HNSW”，正确性边界会更糟。

## 15.5 command_task 和 response_task：先把命令生命周期装进对象

prepare/commit 拆出来后，下一步不是马上开线程，而是先把命令执行过程从栈上局部变量变成可转移的任务对象。

`36a4c38` 先在 [`kvstore.c`](../../src/core/kvstore.c) 里抽象同步命令任务上下文。`31ab477` 又把它拆成 [`kvs_command_task.h`](../../include/kvs_command_task.h) 和 [`kvs_command_task.c`](../../src/core/kvs_command_task.c)。`827c0f5` 引入 [`kvs_response_task_t`](../../include/kvs_response_task.h)，`e4b02cb` 补 [`net_session_t`](../../include/net_session.h) 的生命周期边界。这些阶段分别有 [`16_stage2_command_task_review.md`](../route_c/archive/stage_docs/16_stage2_command_task_review.md)、[`17_stage3_response_task_review.md`](../route_c/archive/stage_docs/17_stage3_response_task_review.md) 和 [`18_session_lifecycle_before_executor.md`](../route_c/archive/stage_docs/18_session_lifecycle_before_executor.md) 作为审计证据。

[`kvs_command_task_t`](../../include/kvs_command_task.h) 能看到这一阶段的设计意图。它不是只保存 `argv`，而是承载完整生命周期：

```text
cmd / cmd_index / argc / argv / argv_lens
response_task / response buffer
build_status / status / command_committed
owns_argv
is_prepared / prepare_dispatched / prepare_completed / prepare_status
prepared_kind / prepared_set / prepared_search
lock wait / lock hold / VSET side-effect timings
expire_at_ms / has_expire_at / need_del_sync / need_persist_sync
session / fd / session_id / generation / request_seq / owner info
sync_context / is_replication
```

这里有两个关键取舍。

第一，argv 所有权必须明确。早期同步路径里，`argv` 指向 RESP parse buffer，不能跨出当前函数。后续 executor / worker pool 需要让任务跨线程、跨队列移动，所以 [`kvs_command_task_copy_argv()`](../../src/core/kvs_command_task.c) 会复制参数，并重新解析 `command_context_t`。

第二，response 不能继续只是一个栈上 buffer。[`kvs_response_task_t`](../../include/kvs_response_task.h) 负责响应 buffer、session 引用、fd、generation、request_seq 和 poller owner。这样 prepared task 完成后，executor 可以把 response 投回原网络层，而不是在 worker 线程里直接写 socket。

这一步解决的不是性能，而是生命周期。

没有 command_task，worker 做完 embedding 后不知道把结果交给谁；没有 response_task，executor 做完 commit 后也不知道该把响应送回哪个 session。真正难的不是起线程，而是对象在队列之间移动时“谁还拥有它”。

## 15.6 Reactor、Proactor、NtyCo：三条网络路径不是同一种接入

`b6c89ba` 开始把 reactor 接到单线程命令执行器，`da5699f` 稳定 reactor 边界。之后 `354d8ce` 抽出 [`kvs_async_net_adapter.c`](../../src/network/kvs_async_net_adapter.c)，`1861867` 补强 adapter 边界。`77375e1`、`0db4f00`、`5b64d56`、`47705cd` 又把 proactor 的连接生命周期、eventfd 唤醒和异步执行闭环补上。这部分不能只写成“reactor/proactor/ntyco 都接入 executor”，因为三条网络模型的适配边界不同。

Reactor 是最直接的一条路径。[`reactor.c`](../../src/network/reactor.c) 在解析出一个 RESP 命令后构造 `kvs_command_task_t`，复制 argv，记录 session identity / generation / request_seq，然后调用 `kvs_command_executor_submit()`。响应由 executor 放入 response queue 后，reactor 通过 executor eventfd 唤醒，在 reactor owner loop 中取回属于自己的 response 并 append 到 session send buffer。这个阶段对应 [`20_stage4b_executor_mvp_review.md`](../route_c/archive/stage_docs/20_stage4b_executor_mvp_review.md)、[`21_stage4c_reactor_executor_stabilization.md`](../route_c/archive/stage_docs/21_stage4c_reactor_executor_stabilization.md) 和 [`22_stage4c_reactor_executor_stabilization_review.md`](../route_c/archive/stage_docs/22_stage4c_reactor_executor_stabilization_review.md)。

Proactor 复杂一些，因为它基于 io_uring，响应必须回到拥有该 fd 的 ring / owner 线程。[`5b64d56` 时的 `proactor_submit_async_command()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/5b64d56/src/network/proactor.c#L469-L790) 展示了这个闭环：提交 task 时保存 owner type、poller id、generation、request_seq；executor 完成后通过 eventfd 产生 CQE；proactor owner 再调用 owner-aware polling。对应的设计文档是 [`26_stage6_proactor_io_uring_adapter_design.md`](../route_c/archive/stage_docs/26_stage6_proactor_io_uring_adapter_design.md)、[`28_stage6b_proactor_executor_eventfd_wake.md`](../route_c/archive/stage_docs/28_stage6b_proactor_executor_eventfd_wake.md)、[`29_stage6c_proactor_executor_async_closure.md`](../route_c/archive/stage_docs/29_stage6c_proactor_executor_async_closure.md) 和 [`30_stage6d_proactor_executor_stabilization_review.md`](../route_c/archive/stage_docs/30_stage6d_proactor_executor_stabilization_review.md)。

NtyCo 又是另一种约束。Route C 审计文档 [`47_stage10_4_ntyco_executor_readiness.md`](../route_c/archive/stage_docs/47_stage10_4_ntyco_executor_readiness.md) 记录了早期问题：协程路径仍可能沿旧同步 handler 执行，而且没有证明跨线程安全唤醒协程的机制。后续 [`b851923` 时的 `ntyco_submit_command_task()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/b851923/src/network/ntyco.c#L95-L348) 采用了更保守的方式：协程提交 task 到 executor，然后在协程侧轮询自己的 response。这个选择牺牲了性能优雅性，但避免了把未证明的跨线程 coroutine wake 写进正确性主路径。最终收敛记录在 [`59_stage10_13_ntyco_executor_convergence.md`](../route_c/archive/stage_docs/59_stage10_13_ntyco_executor_convergence.md)。

三条路径共同的不变量是：

```text
网络层可以 parse 和持有 socket；
网络层不能直接执行会访问 shared state 的 handler；
response 必须用 owner_type / owner_poller_id / generation / request_seq 回到原 owner；
断连或 fd 复用时，response drop 不能写错 socket；
单连接内仍保持 single in-flight，避免乱序响应。
```

## 15.7 单执行线程：状态顺序从锁转移到 executor

最终在 [`kvs_command_executor.c`](../../src/core/kvs_command_executor.c) 里，核心结构是一个全局 executor。当前 HEAD 仍能看到这些队列：

```text
cmd queue
prepared queue
repl queue
ttl expire queue
ttl scan queue
full sync snapshot queue
rdb snapshot queue
aof rewrite snapshot queue
response queue
```

执行线程入口 [`kvs_command_executor_thread_main()`](../../src/core/kvs_command_executor.c) 启动后会调用：

```c
kvs_owner_lock_set_executor_thread();
```

这一步把“状态 owner”从抽象说法变成了运行时事实：executor 线程可以被识别，owner lock wrapper 可以统计调用是否发生在 executor 线程内。

主循环的优先级也能看出设计意图。它优先处理 prepared task，然后是 replication、TTL、普通命令和 snapshot 类任务：

```text
prepared
→ replicated
→ ttl expire
→ ttl scan
→ normal command
→ full sync snapshot
→ rdb snapshot
→ aof rewrite snapshot
```

这不是为了证明这个优先级一定最优，而是说明 Route C 已经把这些任务放进同一个调度点。这样 AOF append、`master_sync_command()`、TTL lazy expire、VSET commit、RDB snapshot 都能用同一个“executor 顺序”解释。

[`kvs_command_executor_execute()`](../../src/core/kvs_command_executor.c) 是主调用链的核心：

```text
response_task 绑定 session / fd / seq
→ 如果 build 阶段已失败，直接 finalize
→ 如果 prepare 已完成，执行 kvs_command_task_execute_sync()
→ 执行 kvs_command_task_post_commit()
→ kvs_response_task_finalize()
→ kvs_command_executor_push_response()
```

对 VSET 来说，[`kvs_command_task_execute_sync()`](../../src/core/kvs_command_task.c) 内部会调用 `kvs_vector_set_commit_prepared()`；对 VSEARCH 来说，则通过 `kvs_vector_search_prepared_to_resp()` 进入 `kvs_vector_search_with_prepared()`。

这说明 executor 不是只包了一层队列。它承担了最终状态 mutation、读写锁 wrapper、post-commit side effect 和 response ownership。提交顺序在 executor 中确定，AOF append 和 replication post-commit 跟随这个顺序；prepare 完成顺序只决定 prepared task 何时入队，不等于多个请求可以并行修改 record/id_map/HNSW。

## 15.8 worker pool、per-thread runtime 与 batch：慢计算被移出 executor，但收益没有被包装成结论

在 executor 跑通后，才有条件引入 embedding worker pool。

`93e2e7b` 引入 [`kvs_embedding_worker_pool.h`](../../include/kvs_embedding_worker_pool.h) 和 worker pool 配置；`6338e9f` 做 per-thread runtime 方向；`f57dd5e` 增加 runtime tuning 和瓶颈观测；`2db1f4b` 尝试 batch embedding prepare；`4e9ff06` 增加 VSET commit breakdown 和 batch policy 测试，并在提交信息里明确“批量处理 VSET 后性能仍然不理想”；`a350b1f` 收敛到 embedding prepare 工作池；`b1ac888` 补 worker pool 异常路径测试。这些阶段对应 [`31_stage7_embedding_worker_pool.md`](../route_c/archive/stage_docs/31_stage7_embedding_worker_pool.md)、[`32_stage8_per_thread_embedding_runtime.md`](../route_c/archive/stage_docs/32_stage8_per_thread_embedding_runtime.md)、[`33_stage8_1_embedding_runtime_tuning.md`](../route_c/archive/stage_docs/33_stage8_1_embedding_runtime_tuning.md)、[`34_stage8_2_embedding_runtime_bottleneck_tuning.md`](../route_c/archive/stage_docs/34_stage8_2_embedding_runtime_bottleneck_tuning.md)、[`35_stage8_3_batch_embedding_prepare.md`](../route_c/archive/stage_docs/35_stage8_3_batch_embedding_prepare.md)、[`36_stage8_4_vset_commit_breakdown.md`](../route_c/archive/stage_docs/36_stage8_4_vset_commit_breakdown.md)、[`37_stage8_5_command_level_batch_policy.md`](../route_c/archive/stage_docs/37_stage8_5_command_level_batch_policy.md)、[`41_stage9_embedding_prepare_worker_pool.md`](../route_c/archive/stage_docs/41_stage9_embedding_prepare_worker_pool.md) 和 [`42_stage9_1_worker_pool_shutdown_stabilization.md`](../route_c/archive/stage_docs/42_stage9_1_worker_pool_shutdown_stabilization.md)。

最终 worker pool 的职责在 [`kvs_embedding_worker_pool.c`](../../src/core/kvs_embedding_worker_pool.c) 里很清楚：

```text
worker 从 prepare queue 取 kvs_command_task_t
→ 调用 kvs_command_task_prepare(task)
→ 只生成 prepared_set / prepared_search
→ 成功后调用 kvs_command_executor_submit_prepared(task)
→ 由 executor 执行 commit/search 和 response
```

也就是说，worker 不访问 store，不修改 TTL，不 append AOF，不做 master_sync，不直接写 socket。它只是计算层。

配置也说明这条路径是显式开关。`01b50c1` 时的 `config/kvstore-embedding.conf` 中，`embedding_worker_pool_enabled`、`embedding_per_thread_runtime_enabled` 和 `embedding_batch_enabled` 都是保守配置。当前 HEAD 的 [`config/kvstore-embedding.conf`](../../config/kvstore-embedding.conf) 仍然能看出这些开关是可配置项。因此本阶段不能说“默认已经开启高并发 embedding worker pool”。更准确的说法是：代码具备了可选 worker pool、per-thread runtime、batch prepare 和 profiling 入口，但默认配置和结果结论都偏保守。

结果文件也要谨慎使用。历史结果 [`01b50c1:results/stage7_worker_pool_rerun/reactor_threads4.log`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/results/stage7_worker_pool_rerun/reactor_threads4.log#L1-L26) 记录了 ONNX 本地模型加载、`worker_threads=4`，并显示：

```text
submitted=1560
completed=1560
failed=0
queue_max=16
prepare_avg_us=5743
prepare_max_us=87435
```

历史结果 [`01b50c1:results/stage7_worker_pool_rerun/proactor_threads4.log`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/results/stage7_worker_pool_rerun/proactor_threads4.log#L1-L29) 也记录了：

```text
submitted=1560
completed=1560
failed=0
queue_max=14
prepare_avg_us=5943
prepare_max_us=108838
```

这些数字能证明 worker pool 路径在这组脚本里确实跑过、任务完成数和失败数可观测；但不能直接证明整体 QPS 提升，也不能证明生产并发稳定性。尤其 `f57dd5e` 和 `4e9ff06` 的历史说明，当时观察到瓶颈仍然主要在模型计算，batch VSET 的收益也不理想。

这类失败或低收益尝试必须写出来。它说明这次迭代不是“加 worker pool 就结束”，而是在真实推理成本下不断确认：系统瓶颈不只在锁，也在 ORT runtime、tensor、batch 语义和 commit side effect。

## 15.9 TTL、复制、RDB/AOF、Full Sync 与 shutdown：为什么不能只优化 VSET

如果只把 VSET/VSEARCH 接到 executor，Route C 仍然是不完整的。

原因是 KVLite 里有很多不是客户端命令直接触发的状态访问：

```text
TTL active scan
GET/EXIST 的 lazy expire
slave apply
master full sync
RDB save
AOF rewrite
shutdown destroy
NtyCo coroutine path
eBPF replication fallback
```

这些路径只要还有一个能绕过 executor，weak-lock 或单写者模型就不能成立。[`43_stage10_rwlock_removal_readiness_audit.md`](../route_c/archive/stage_docs/43_stage10_rwlock_removal_readiness_audit.md) 到 [`60_stage10_14_full_executor_coverage_closeout_and_stage11_gate.md`](../route_c/archive/stage_docs/60_stage10_14_full_executor_coverage_closeout_and_stage11_gate.md) 的核心工作，就是把这些路径逐步收回来。

TTL 的收敛分两层。`13a629a` 让主动扫描进入 executor task，当前 [`kvs_ttl_background.c`](../../src/core/kvs_ttl_background.c) 只提交扫描任务，不再自己直接扫 live store；[`44_stage10_1_ttl_ownership_convergence.md`](../route_c/archive/stage_docs/44_stage10_1_ttl_ownership_convergence.md) 记录了这个边界。懒过期则在 [`45_stage10_2_lazy_expire_and_sync_fallback_audit.md`](../route_c/archive/stage_docs/45_stage10_2_lazy_expire_and_sync_fallback_audit.md) 中单独审计：读命令触发删除时必须在 executor 内做 read-lock 到 write-lock 的重试，不能让网络线程直接删除节点。

复制的收敛也不是复制协议 v2。`6da5e83` 和后续提交把 slave apply 封装为 [`kvs_replicated_command_task_t`](../../include/kvs_command_executor.h)，由 executor 应用；[`46_stage10_3_replicated_command_task.md`](../route_c/archive/stage_docs/46_stage10_3_replicated_command_task.md) 记录了 slave apply 的任务所有权。[`52_stage10_8_replication_fallback_audit.md`](../route_c/archive/stage_docs/52_stage10_8_replication_fallback_audit.md) 和 [`58_stage10_12_ebpf_replication_fallback_executorization.md`](../route_c/archive/stage_docs/58_stage10_12_ebpf_replication_fallback_executorization.md) 则把 reactor/proactor/eBPF fallback 路径做了分类：`REPLSYNC` 是协议握手，不直接访问 shared state；真正 apply 的复制写入要走 `kvs_command_executor_submit_replicated()`。这仍然没有补齐 repl_id、offset、ACK、backlog、断线续传或向量增量复制。

snapshot 路径也做了收敛。`efe23bc` / `f466ae7` / `ec7c1cc` 分别把 master full sync、RDB save、AOF rewrite 变成 executor snapshot task，当前 [`kvs_command_executor.h`](../../include/kvs_command_executor.h) 中有 `kvs_full_sync_snapshot_t`、`kvs_rdb_snapshot_t` 和 `kvs_aof_rewrite_snapshot_t`，执行入口在 [`kvs_command_executor_build_full_sync_snapshot()`](../../src/core/kvs_command_executor.c)、[`kvs_command_executor_build_rdb_snapshot()`](../../src/core/kvs_command_executor.c) 和 [`kvs_command_executor_build_aof_rewrite_snapshot()`](../../src/core/kvs_command_executor.c)。对应文档是 [`48_stage10_5_master_full_sync_snapshot_task.md`](../route_c/archive/stage_docs/48_stage10_5_master_full_sync_snapshot_task.md)、[`49_stage10_5a_master_full_sync_snapshot_implementation.md`](../route_c/archive/stage_docs/49_stage10_5a_master_full_sync_snapshot_implementation.md)、[`50_stage10_6_rdb_save_snapshot_task.md`](../route_c/archive/stage_docs/50_stage10_6_rdb_save_snapshot_task.md) 和 [`51_stage10_7_aof_rewrite_snapshot_task.md`](../route_c/archive/stage_docs/51_stage10_7_aof_rewrite_snapshot_task.md)。

这一步的工程取舍是：snapshot 构建顺序正确，不等于大规模 snapshot 性能已经解决。它避免了 RDB/AOF/full sync 直接遍历 live store 并和命令 mutation 交错，但会带来内存拷贝、executor 排队和构建期间读边界成本。

shutdown 和生命周期同样是 Route C 的一部分。[`53_stage10_9_shutdown_owner_boundary_audit.md`](../route_c/archive/stage_docs/53_stage10_9_shutdown_owner_boundary_audit.md)、[`54_stage10_9a_slave_connect_thread_owner_audit.md`](../route_c/archive/stage_docs/54_stage10_9a_slave_connect_thread_owner_audit.md) 和 [`55_stage10_10_ttl_scanner_runtime_owner_convergence.md`](../route_c/archive/stage_docs/55_stage10_10_ttl_scanner_runtime_owner_convergence.md) 分别处理 executor stop/drain、slave connector、TTL scanner 的生命周期。后续 [`63_stage12_weak_rwlock_correctness_and_stress_gate.md`](../route_c/archive/stage_docs/63_stage12_weak_rwlock_correctness_and_stress_gate.md) 又记录了真实问题：`g_shutdown_flag` data race、AOF rewrite COW snapshot 失败、NtyCo mixed shutdown hang、TTL scanner stop/running race、AOF stop race、NtyCo `init_hook` race。最终 [`route_c_problem_resolution_log.md`](../route_c/route_c_problem_resolution_log.md) 按问题而不是按 Stage 归档了这些修复和 watch item。

面试里可以这样讲：

```text
VSET 异步化只是入口。
如果 RDB save 或 master full sync 还在另一个线程里直接扫 live store，
那状态所有权仍然不成立。
所以我把后台读写也抽象成 executor task 或 snapshot task，
让它们和普通命令共享同一个状态顺序。
```

这比单独说“我加了一个线程池”更接近真实工程难点。

## 15.10 weak-lock：不是不要锁，而是给所有权模型加 gate

本阶段最后一段是 owner rwlock wrapper 和 weak-lock gate。

`b1feec1` 引入 [`kvs_owner_lock.h`](../../include/kvs_owner_lock.h) 和 [`kvs_owner_lock.c`](../../src/core/kvs_owner_lock.c)，`01b50c1` 完成 final gate。当前 `kvs_owner_lock.c` 里可以看到：

```text
kvs_owner_lock_set_executor_thread()
kvs_owner_lock_is_executor_thread()
kvs_owner_rwlock_rdlock()
kvs_owner_rwlock_wrlock()
kvs_owner_rwlock_unlock()
```

weak-lock 的语义不是“系统完全没有并发保护”，而是：

```text
如果当前线程是 executor 线程，
  rwlock lock/unlock 可以是 no-op；
如果不是 executor 线程，
  仍然 fallback 到真实 pthread_rwlock，并记录 warning / stats。
```

`KVLITE_ROUTE_C_WEAK_RWLOCK` 是回滚开关。当前 [`kvs_owner_lock.c`](../../src/core/kvs_owner_lock.c) 中默认值已经设为 weak mode；如果设置为 `0`，则回到真实锁模式。这个默认切换的完成证据在 [`65_stage13_global_rwlock_removal_and_final_gate.md`](../route_c/archive/stage_docs/65_stage13_global_rwlock_removal_and_final_gate.md) 和 [`66_route_c_final_closeout_report.md`](../route_c/archive/stage_docs/66_route_c_final_closeout_report.md)。

这个设计的关键在于可验证性。

如果所有状态访问都应该在 executor 线程里发生，那么正常运行时应该看到：

```text
non_executor_thread_calls = 0
weak_fallback_calls = 0
strict_assertion_failures = 0
```

[`route_c_final_report.md`](../route_c/route_c_final_report.md) 把这件事作为 Route C 的 closeout 标准之一：`g_kv_data_rwlock` 不再是正确性主路径，weak-lock 默认启用，但真实锁回滚路径仍然保留。

这个取舍很务实：

```text
直接删除 g_kv_data_rwlock
→ 架构看起来干净，但回滚和审计风险太高。

继续所有路径都用真实 rwlock
→ 改动风险低，但无法证明单写者所有权已经收敛。

引入 owner wrapper + weak-lock gate
→ 保留回滚，同时用统计和测试验证是否还有非 executor 访问。
```

本阶段选择了第三种。

## 15.11 测试、benchmark 和证据边界

这一阶段的证据要分层看。

从 `33a886f` 开始，Route C 专项测试脚本已被清理，当前 HEAD 下不再有 `tests/route_c/`。所以本章不能写“当前可以运行 `tests/route_c/xxx.sh` 证明通过”。正确说法是：历史脚本在 `01b50c1` 前后存在并被 Route C 文档记录；当前可点击的证据主要是 Route C 文档和少量保留的产品级测试，例如 [`tests/ttl/test_ttl_all_backends.sh`](../../tests/ttl/test_ttl_all_backends.sh) 与 [`tests/vector/semantic_onnx_test.sh`](../../tests/vector/semantic_onnx_test.sh)。

| 证据 | 类型 | 能证明 | 不能证明 |
| --- | --- | --- | --- |
| [`01b50c1:tests/proactor_executor_stability_6d.sh`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/tests/proactor_executor_stability_6d.sh)、[`01b50c1:tests/proactor_executor_perf_compare_6d.sh`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/tests/proactor_executor_perf_compare_6d.sh)、[`stage6d/proactor.log`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/results/stage6d/proactor.log#L1-L19) | 历史 executor smoke / stability / perf harness + 结果日志 | proactor executor 事件唤醒和响应回投路径可跑 | 不能证明真实 ONNX 场景吞吐提升 |
| [`01b50c1:tests/embedding_worker_pool_stage7.sh`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/tests/embedding_worker_pool_stage7.sh)、[`01b50c1:tests/embedding_worker_pool_stage9.sh`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/tests/embedding_worker_pool_stage9.sh)、[`reactor_threads4.log`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/results/stage7_worker_pool_rerun/reactor_threads4.log#L1-L26) | 历史 worker pool correctness / abnormal path + ONNX 观测 | worker pool、断连、shutdown 等路径被脚本覆盖，ONNX prepare 数量可观测 | 不能证明所有取消语义完善，也不能证明队列容量策略合理 |
| [`01b50c1:tests/ttl_executor_stage10_1.sh`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/tests/ttl_executor_stage10_1.sh)、[`01b50c1:tests/master_full_sync_stage10_5.sh`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/tests/master_full_sync_stage10_5.sh)、[`01b50c1:tests/rdb_snapshot_stage10_6.sh`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/tests/rdb_snapshot_stage10_6.sh)、[`01b50c1:tests/aof_rewrite_snapshot_stage10_7.sh`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/tests/aof_rewrite_snapshot_stage10_7.sh)、[`01b50c1:tests/replicated_command_task_stage10_3.sh`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/tests/replicated_command_task_stage10_3.sh) | 历史 correctness / end-to-end | TTL、full sync、RDB、AOF rewrite、replication 进入 executor 后有回归覆盖 | 不能证明大数据量 snapshot 成本 |
| [`63_stage12_weak_rwlock_correctness_and_stress_gate.md`](../route_c/archive/stage_docs/63_stage12_weak_rwlock_correctness_and_stress_gate.md)、[`67_route_c_completion_review.md`](../route_c/archive/stage_docs/67_route_c_completion_review.md) | Route C 阶段报告 | weak-lock / real-lock、TSAN、mixed stress、竞态修复和 closeout gate 记录 | 报告不是源码本身，需要和 diff / 当前源码一起看 |
| [`66_route_c_final_closeout_report.md`](../route_c/archive/stage_docs/66_route_c_final_closeout_report.md) | Route C 最终性能基线 | weak-lock vs real-lock 的 GET/SET/VSET/VSEARCH/TTL 样本和 owner stats | 不能当作生产 SLA；closeout 文档本身也说明 weak-lock 不是绝对性能提升 |
| [`route_c_test_script_removal_report.md`](../route_c/route_c_test_script_removal_report.md) | 文档清理证据 | 解释为什么当前 HEAD 不再保留 Route C 专项脚本 | 不能替代历史测试结果 |

按证据强度，本阶段已经证明：

- VSET/VSEARCH 的 embedding prepare 可以从全局状态锁内拆出。
- prepared task 可以回投 executor，由 executor 做最终 commit/search。
- reactor/proactor/ntyco 路径在 Route C 后都接入统一执行器。
- TTL、replication、full sync、RDB、AOF rewrite、shutdown 路径形成了 executor ownership 收敛。
- weak-lock 和 real-lock 都有回滚/验证入口。

基本证明但需要谨慎表达：

- worker pool 在结果日志中完成了固定数量的 ONNX prepare 任务，且没有失败；但这只是特定脚本和参数下的结果。
- TSAN/mixed stress 说明当时的 Route C closeout 通过了竞态和压力 gate；但不能替代长期线上 soak。
- weak-lock 证明了所有权模型可以运行；但 final closeout 也记录了 GET/VSET/VSEARCH 在部分指标上并非绝对更快。

尚未证明：

- 多 executor 线程下的正确性。
- worker queue 满时的完整 backpressure 策略。
- prepare 任务取消、超时和模型慢请求隔离。
- 真实 10K/100K ONNX embedding 下的端到端吞吐、P95、P99。
- batch embedding 的稳定收益。

这些边界不能在面试里夸大。

从本章开始，后续复盘文章的链接规范也应固定下来：首次引用源码、头文件、测试、结果和 Route C/阶段文档时使用 Markdown 链接；当前 HEAD 文件用相对链接，历史实现和历史结果用 commit-pinned GitLab 链接；没有原始结果文件的数字不写；测试通过结论必须同时说明脚本状态和结果证据；链接用于回看源码，不能替代正文解释。

## 15.12 工程取舍：为什么不是直接并行修改 record/index

这一阶段最核心的取舍是并发模型。

候选方案可以分成几类。

第一种，把整个 VSET/VSEARCH 都放在命令锁内执行。

```text
优点：实现简单，旧代码改动少。
缺点：真实 ONNX 推理会长期占用全局锁，其他命令、TTL、RDB/AOF、复制都可能被拖住。
```

这个方案是 `b43ec18` 附近的状态。hash provider 时代还勉强可接受，真实模型接入后就不够了。

第二种，让多个线程直接并行修改 record/id_map/HNSW。

```text
优点：理论上并发度最高。
缺点：record slot、id_map tombstone、HNSW add、AOF 顺序、replication 顺序、TTL 删除都要重新设计同步协议。
```

这个方案被放弃，因为它会把问题从“推理慢”变成“所有状态结构都要变成并发安全结构”。对 KVLite 这个阶段来说风险太高。

第三种，prepare/commit 两阶段。

```text
优点：把纯计算和状态 mutation 分开；worker 可以并发做 embedding；commit 仍然单写者。
缺点：需要复制参数，需要 prepared 对象生命周期，需要 commit 前重新检查状态。
```

Route C 采用的是这个方案。

第四种，每个请求创建一个线程。

```text
优点：实现直观。
缺点：线程创建成本高，无法控制并发，shutdown 和取消更难做。
```

所以后续使用固定 worker pool，而不是 per-request thread。

第五种，模型级 batch。

```text
优点：理论上能提高 ORT 吞吐。
缺点：需要等待窗口、batch shape、任务拆分、response 顺序、VSET/VSEARCH 混排策略。
```

`2db1f4b` 和 `4e9ff06` 已经尝试 batch prepare 和 batch policy，但历史上明确出现了“批量处理 VSET 后性能仍然不理想”的转折。因此本章只能把它写成尝试和观察，不能写成已完成的性能突破。

第六种，重新回到远程模型服务。

```text
优点：模型计算和 KV 进程隔离。
缺点：第十四章已经说明过，gRPC/Python/C++ bridge 在这个项目里被撤回；远程方案还会引入 RPC 失败、部署和协议依赖。
```

所以这一阶段的合理结论是：先用本地 worker pool + executor ownership 把正确性边界收住，再考虑真正的 batch、模型服务化或多 executor。

## 15.13 当前缺陷与重新设计条件

Route C 完成的是所有权模型收敛，不是所有性能问题的终点。

当前缺陷要主动讲清楚。

第一，worker pool 队列容量策略不完整。[`kvs_embedding_worker_pool_submit()`](../../src/core/kvs_embedding_worker_pool.c) 保存了 `queue_capacity`，但 submit 路径主要是入队和统计 max depth，并没有形成严格的满队列拒绝或 backpressure 语义。

第二，任务取消仍然有限。断连场景在历史 [`embedding_worker_pool_stage9_1.sh`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/01b50c1/tests/embedding_worker_pool_stage9_1.sh) 中覆盖 response drop 和 shutdown，但 worker 已经拿到的慢任务无法细粒度取消。模型慢请求仍可能占用 worker。

第三，batch 不是最终答案。虽然引入了 batch API 和策略测试，但历史提交已经记录 batch VSET 性能不理想。当前不能把它讲成成熟模型级 batching。

第四，weak-lock 假设单 executor。[`route_c_final_report.md`](../route_c/route_c_final_report.md) 的剩余风险明确写了：如果未来引入多 executor，需要重新启用真实锁或设计 executor-internal 同步。

第五，snapshot 有成本。RDB/AOF/full sync 通过 executor snapshot 统一边界，但这会带来内存复制和执行器排队成本。当前阶段证明的是一致性边界，不是大规模 snapshot 性能。

第六，复制仍不是完整协议。Route C 收敛的是现有 slave apply、fallback 和 full sync 所有权；它没有补齐 repl_id、offset、ACK、backlog、partial resync、向量全量同步、向量增量复制或副本本地 HNSW rebuild。

第七，仍缺少真实中文 10K/100K 端到端证据。后续 HEAD 已经出现 T2Ranking、真实 embedding cache、exact ground truth 和 FAISS/core_hnsw 对比方向，但这些属于第十六章之后的向量 benchmark / HNSW 路线，不能提前写成本章成果。

重新设计的触发条件也很清楚：

```text
如果 worker queue 经常堆积
→ 需要 backpressure、拒绝策略或 admission control。

如果 OrtRun 长尾主导 P99
→ 需要模型级 batch、超时、取消或模型服务隔离。

如果要多 executor
→ 必须重新设计 record/id_map/HNSW/AOF/replication 的并发协议。

如果 snapshot 阻塞 executor
→ 需要增量 snapshot、copy-on-write 或更细粒度的后台快照机制。

如果要生产级语义缓存
→ 必须补真实数据集、recall、端到端延迟和长稳测试。
```

## 15.14 Agent 辅助开发边界

这一章比第十四章更需要把 Agent 辅助边界讲清楚，因为 Route C 的代码量和文档量都很大。

能从 Git 直接确认的是：本章这些提交有不同 author 邮箱记录，且 `docs/route_c` 下保留了大量阶段设计、审计和 closeout 报告。部分提交信息或文档中存在 `Co-Authored-By` 记录时，可以陈述协作事实；但不能仅凭提交速度、文档风格或代码风格推断每一行是谁写的。

面试里更稳妥的表达是：

```text
我提出并坚持的工程问题：
真实 embedding 进入 VSET/VSEARCH 后，不能让模型推理长期占用全局状态锁。

我设定的边界：
worker 只能做 prepare；record/id_map/HNSW/TTL/AOF/RDB/replication 必须由 executor 顺序提交。

我做出的关键决策：
采用 prepare/commit 两阶段；
采用单执行线程 owner 模型；
保留 real-lock fallback；
用 weak-lock stats 验证是否还有非 executor 状态访问。

Agent 可以辅助：
生成阶段脚手架、补测试脚本、整理审计报告、扩展统计字段。

我必须自己掌握：
prepared 对象所有权、commit 前重查、response_task 生命周期、
worker 回投 executor、snapshot task 边界、weak-lock 回滚和测试证据。
```

如果面试官追问“没有 Agent 你能不能维护”，回答重点不应该是“我用了什么工具”，而应该是能画出调用链：

```text
RESP parse
→ command_task
→ embedding prepare
→ prepared queue
→ executor commit
→ post_commit AOF/replication
→ response_task
→ network send
```

并且能说清每个对象在哪里分配、在哪里转移所有权、失败时由谁释放。

## 15.15 面试中怎么讲这一章

### 1 分钟版本

“第十四章把真实 ONNX embedding 接进来了，但这马上暴露了一个执行链路问题：原来 VSET/VSEARCH 是在 `g_kv_data_rwlock` 内同步跑 handler，而 handler 里会生成 embedding。hash provider 时代这个成本不明显，ONNX 接入后，tokenizer、tensor、`OrtRun` 都可能长期占锁。

我后面做了 Route C 重构。第一步不是直接上线程池，而是把 VSET/VSEARCH 拆成 prepare/commit。prepare 只复制参数并生成 embedding，不碰 record/id_map/HNSW；commit 回到锁内重新检查 question+scope，再分配 record、更新 id_map 和索引。

之后我把命令封装成 `kvs_command_task_t`，响应封装成 `kvs_response_task_t`，引入单线程 command executor 作为状态 owner。需要 embedding 的任务会先投到 worker pool，worker 只做 prepare，完成后回投 executor，由 executor 做最终 commit 和 response。

最后 TTL、复制、RDB、AOF rewrite、shutdown、NtyCo 路径也收敛到 executor。weak-lock 不是删除锁，而是用 owner wrapper 验证状态访问是否都发生在 executor 线程内，并保留 real-lock 回滚。这个阶段证明的是所有权模型收敛，不是证明所有性能问题都解决。”

### 3～5 分钟版本

“真实 embedding 接入后，系统从功能问题变成了执行链路问题。`b43ec18` 时，`kvs_filter_protocol()` 会先按命令类型拿 `g_kv_data_rwlock`，然后执行 handler。VSET 的 `kvs_vector_record_set_fields()` 和 VSEARCH 的 `kvs_vector_search()` 都会在 handler 里同步调用 `kvs_vector_embedding_generate()`。这意味着 ONNX 推理成本会落在命令锁里。

我的第一步是拆 prepare/commit。`c6615c4` 增加了 `kvs_vector_set_prepared_t` 和 `kvs_vector_search_prepared_t`。prepare 阶段复制 question/answer/scope 并生成 embedding，不访问 store；commit 阶段重新检查 question+scope，存在就只更新 answer，不存在才生成 record_id、更新时间戳、更新 id_map 和 flat/HNSW index。这个不变量保证了 worker 未来可以并发 prepare，但状态 mutation 仍然单写者。

第二步是把命令生命周期对象化。`kvs_command_task_t` 里保存 argv、context、prepared_set/search、lock timing、post-commit 状态、session 引用和 request_seq。`kvs_response_task_t` 保存响应 buffer 和 session owner。这样任务才能从网络线程转移到 executor，再转移到 worker，最后回到 executor 和网络层。

第三步是单线程 executor。executor 有 command queue、prepared queue、TTL queue、replication queue、snapshot queue 和 response queue。它启动后标记自己是 owner thread。普通命令进入 executor；需要 embedding 的任务如果 worker pool 开启，就先交给 worker。worker 只调用 `kvs_command_task_prepare()`，成功后 `kvs_command_executor_submit_prepared()` 回投。executor 再调用 `kvs_command_task_execute_sync()`，里面对 VSET 走 `kvs_vector_set_commit_prepared()`，对 VSEARCH 走 `kvs_vector_search_with_prepared()`。

第四步是后台路径收敛。只异步化 VSET 不够，因为 TTL scan、lazy expire、slave apply、master full sync、RDB save、AOF rewrite、shutdown 都可能访问全局状态。所以 Route C 后半段把这些都变成 executor task 或 snapshot task。最后用 owner rwlock wrapper 做 weak-lock gate：executor 线程内 lock/unlock 可以 no-op，非 executor 线程 fallback 到真实锁并记录统计。这样既能验证所有权，又能通过环境变量回滚到 real-lock。

验证上，我会分层表达。stage6d 的历史结果日志证明 reactor/proactor executor 闭环跑过；stage7 worker pool rerun 日志证明 ONNX prepare worker 完成了固定数量任务；Route C final report 记录了 weak-lock/real-lock、TSAN 和 mixed stress gate。但这些都不能外推成生产 SLA，也不能说 batch 已经解决性能。当前缺陷包括 worker queue backpressure 不完整、慢任务取消有限、batch 收益没有证明、多 executor 需要重新设计。”

### 面试官追问方向

**为什么不能直接在锁内跑 ONNX？**

因为 ONNX 推理包含 tokenizer、tensor 创建、`OrtRun` 和输出处理，耗时和长尾都比 hash provider 大得多。放在全局数据锁内，会把普通 KV、TTL、RDB/AOF、复制和其他 V 命令都拖住。

**prepare 阶段为什么不能生成 record_id？**

record_id 依赖当前 store 状态和 slot 分配。prepare 是锁外纯计算阶段，期间可能有其他命令提交，所以 record_id、slot、timestamp、id_map、index mutation 都必须在 commit 阶段生成或修改。

**如果两个 VSET 同时 prepare 同一个 question+scope 怎么办？**

commit 阶段会重新查 question+scope。如果第一个已经提交，第二个 commit 时就只更新 answer，并销毁自己准备好的 embedding，保持旧语义。

**worker pool 会不会并发改 HNSW？**

不会。worker 只调用 prepare，生成 prepared embedding。HNSW add/search、record/id_map mutation 和 response finalize 都回到 executor。

**weak-lock 是不是不安全？**

weak-lock 的前提是所有状态访问都收敛到 executor 线程。它不是无条件删除锁，而是通过 owner wrapper 检查当前线程；非 executor 线程会 fallback 到真实锁并记录统计。并且保留 `KVLITE_ROUTE_C_WEAK_RWLOCK=0` 回滚到 real-lock。

**这个阶段性能提升了吗？**

不能简单说全面提升。Route C final closeout 里也记录了 weak-lock 在部分 GET/VSET/VSEARCH 指标上有退化。这个阶段的主结论是所有权模型收敛和锁边界清晰，不是所有性能问题终结。

**为什么 batch VSET 没有作为最终成果强调？**

因为 Git 历史里 `4e9ff06` 明确是“批量处理 VSET 后性能仍然不理想”。所以它只能作为一次尝试和转折，不能包装成成熟优化。

## 15.16 本章小结

第十五章的核心不是“给 embedding 加了线程池”，而是 KVLite 在真实模型成本进入命令链路后，被迫重构了状态所有权。

这一阶段已经完成的关键变化是：

```text
VSET/VSEARCH 从同步锁内 embedding
→ 拆成锁外 prepare 和锁内 commit/search

网络线程直接执行 handler
→ command_task 投递到 executor

worker 直接参与状态修改
→ worker 只做 embedding prepare，prepared task 回投 executor

TTL/RDB/AOF/replication/shutdown 分散访问状态
→ 收敛为 executor task 或 snapshot task

g_kv_data_rwlock 作为主正确性机制
→ owner wrapper + weak-lock gate + real-lock fallback
```

这个阶段真正值得面试展开的是几个不变量：

```text
prepare 不产生可见状态；
commit 是唯一可见 mutation 点；
record_id / slot / timestamp 只能在 commit 时生成；
worker 不能修改 record/id_map/HNSW；
AOF append 和 replication 跟随 commit 顺序；
response 必须带 session generation / request_seq 回到原网络 owner；
weak-lock 只能建立在单 executor 所有权成立之后。
```

当前还不能夸大的内容也很明确：

```text
worker pool 不等于生产级 backpressure；
batch 尝试没有证明稳定收益；
weak-lock 不适用于未来多 executor；
snapshot 一致性不等于大规模 snapshot 性能；
Route C 不是完整复制协议 v2；
Route C 不证明真实中文 10K/100K 召回和端到端 SLA。
```

所以本章的阶段性结论是：KVLite 从“网络线程持锁同步执行真实 embedding”演进到了“计算锁外 prepare、状态单写者 commit、后台任务 executor 化”的所有权模型。它为后续向量内核重构、HNSW v2 和真实 benchmark 打下了执行链路基础，但没有提前消费那些后续主题。
