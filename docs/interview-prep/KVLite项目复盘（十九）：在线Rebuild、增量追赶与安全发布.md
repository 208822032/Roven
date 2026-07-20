# 第十九章 在线 Rebuild、增量追赶与安全发布

## 19.1 为什么 snapshot 仍不能解决在线 rebuild

第十八章把 HNSW graph 从“每次启动都必须重建的派生状态”变成了“校验通过即可加载的派生 cache”。但 snapshot 解决的是已有可信图的恢复，不解决运行中的图为什么、何时以及怎样被替换。

同步 `VINDEX REBUILD` 的根本问题不是结果不正确，而是构图期间服务线程要承担完整扫描和 HNSW add 成本。与此同时，简单地把这段代码搬到后台线程也不成立：旧图仍有 reader，records 仍可能变化，新图在构建完成时未必对应当前 records，发布后旧图也不能在 reader 尚未退出时立即释放。

本章解决的唯一问题是：

```text
active HNSW 继续承担读请求时，
如何从 records 构建不可见 shadow，记录 baseline 之后的 mutation，
按序 replay 并完成一致性校验，再在 reader-safe 门禁下发布，
失败时始终保留旧 active。
```

必须先限定“在线”的证据边界：[`R6 报告`](../hnsw_rebuild_productization/hnsw_r6_background_worker_async_rebuild_report.md) 明确说明当前 one-shot worker 允许 `VSEARCH` 继续服务，但为规避 manager record store 数据竞争，worker 整轮持有 rebuild worker read guard，部分 `VSET/VDEL` 会等待写 guard。因而这一版完成的是**查询在线、发布安全的最小产品闭环**，不是写路径完全无阻塞的最终在线重建。

## 19.2 Git 范围、聚合提交与源码导航

本章范围只有一个大型聚合提交：

```text
前一提交：26ebf99（第十八章 cold-start benchmark）
本章提交：74ec6af（29 files changed, 5920 insertions, 83 deletions）
后一提交：fd439e9（真实 embedding benchmark 起点）
Git inclusive range：74ec6af^..74ec6af
```

单提交不等于单阶段。`74ec6af` 把此前工作区内逐步完成的 HNSW-R1～R7 一次聚合入库；必须借助分阶段报告还原真实顺序：

| 工程阶段 | 当时真正完成的能力 | 当时尚不能声称 |
| --- | --- | --- |
| R1 | 同步 rebuild 状态机和失败原因 | 后台并发安全 |
| R2 | manager-owned `rwlock` reader guard | 产品 active 发布 |
| R3 | active/shadow/retired 产品 publish | delta 最终一致性 |
| R4 | generation boundary 与提交后 delta capture | final validation |
| R5 | baseline + ordered replay + publish gate | 后台 worker |
| R6 | one-shot worker 与 start/status/cancel/retry | 写路径无阻塞 |
| R7 | publish 后 best-effort snapshot | snapshot 失败回滚 publish |

最终能力矩阵以 [`R-final 汇总`](../hnsw_rebuild_productization/hnsw_r_final_product_rebuild_consolidation_report.md) 为准，而每一步的中间边界分别见 [`R1`](../hnsw_rebuild_productization/hnsw_r1_sync_rebuild_state_machine_hardening_report.md)、[`R2`](../hnsw_rebuild_productization/hnsw_r2_reader_safety_primitive_report.md)、[`R3`](../hnsw_rebuild_productization/hnsw_r3_active_shadow_retired_publish_report.md)、[`R4`](../hnsw_rebuild_productization/hnsw_r4_delta_generation_boundary_report.md)、[`R5`](../hnsw_rebuild_productization/hnsw_r5_shadow_replay_final_validation_report.md)、[`R6`](../hnsw_rebuild_productization/hnsw_r6_background_worker_async_rebuild_report.md) 和 [`R7`](../hnsw_rebuild_productization/hnsw_r7_post_publish_snapshot_writer_hook_report.md)。

核心源码入口：

1. [`rebuild state/delta/context`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/include/vector/kvs_vector_rebuild.h#L9-L56)
2. [`manager active lock 与 worker lifecycle`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/include/vector/kvs_vector_manager.h#L100-L155)
3. [`状态转换与 delta append`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild.c#L138-L277)
4. [`shadow init/baseline build`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild_shadow.c#L678-L818)
5. [`ordered replay 与 final validation`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild_shadow.c#L825-L944)
6. [`reader-safe publish/retire cleanup`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild_shadow.c#L1030-L1111)
7. [`one-shot async worker`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector.c#L221-L327)
8. [`VINDEX async control plane`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/core/kvs_operation.c#L3325-L3387)

## 19.3 状态机不是装饰，而是发布协议

[`kvs_vector_rebuild_state_t`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/include/vector/kvs_vector_rebuild.h#L9-L17) 定义：

```text
IDLE
  -> BUILDING
  -> REPLAYING_DELTA
  -> READY_TO_SWAP
  -> SWAPPING
  -> IDLE

任一活动阶段 -> FAILED
可取消阶段   -> CANCELLED
FAILED/CANCELLED 经显式 reset/retry -> IDLE
```

R1 的价值不是增加枚举，而是让非法 transition 可拒绝、terminal reason 可保留、重复 begin/cancel 不会悄悄覆盖状态。[`状态机实现`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild.c#L292-L527) 使“能否继续推进”成为显式协议。

三条最重要的不变量是：

```text
BUILDING 完成 != READY_TO_SWAP
READY_TO_SWAP != 已对 reader 可见
SWAPPING 完成 != retired 可无条件立即销毁
```

这也是为什么接口骨架或状态枚举存在不能算在线 rebuild 完成。R1 只把同步控制流钉牢；直到 R2～R7 补齐 reader、publish、delta、validation、worker 和 snapshot hook，才形成最小产品闭环。

## 19.4 baseline capture：先开边界，再扫 records

records 始终是业务真值，shadow 只消费 `record_id + embedding`。[`shadow init`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild_shadow.c#L678-L742) 先创建并 reserve 独立 v2 index，再调用 `kvs_vector_rebuild_begin()`：

```text
allocate shadow
-> copy active HNSW parameters
-> reserve capacity
-> begin(state=BUILDING, rebuild_generation=current_generation)
-> delta capture active
-> scan manager active embeddings into shadow
```

“先 begin、后 full scan”是关键边界。若先扫描再打开 capture，二者之间提交的写入既不在 baseline，也不在 delta，会永久漏进新图。现在 capture 在 scan 前开启，写入即使同时落入 baseline 和 delta，也会在 replay UPSERT 时以同一 stable `record_id` 覆盖到最新版本。

[`baseline iterator`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild_shadow.c#L783-L818) 只遍历 active embedding，不把 scope、TTL、metadata、question、answer 搬进 vector core。manager/core 边界没有因为 rebuild 被打破。

## 19.5 active、shadow、retired 的所有权

三个名字对应三个不同生命周期：

```text
active  : current_index 指向的 reader-visible 产品索引
shadow  : rebuild context 独占，发布前 VSEARCH 不可见
retired : publish 前的 old active，已不可被新 reader 获取，但尚待安全回收
```

R2 早期的 test-only swap 只能证明 context 指针移动，不能证明产品查询切换。R3 才在 [`product publish`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild_shadow.c#L158-L238) 中检查 backend、shadow destroy function、old retired ownership，并把 shadow 内容移动到产品 `core_hnsw_v2_index`。

这里的“atomic publish”不是无锁 CPU 原子指针交换，而是 active-index write lock 保护下的不可分割可见性变化：reader 要么在锁前看到旧 active，要么在锁后看到新 active，不应看到半移动对象。

发布前分配 retired holder。如果分配失败、类型不符、shadow 缺失或状态不为 ready，旧 active 保持不变。只有进入 `SWAPPING` 后才移动结构所有权；成功后 old active 留在 `retired_index`，由独立 cleanup 回收。

## 19.6 mutation delta：记录的是顺序，不是业务副本

[`kvs_vector_rebuild_delta_entry_t`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/include/vector/kvs_vector_rebuild.h#L19-L30) 只有：

```text
generation
op = UPSERT | DELETE
record_id
vector + dim（仅 UPSERT）
```

它没有 key、scope、TTL、metadata、question 或 answer。delta 的任务不是复制 record store，而是让派生 graph 从 baseline 状态追到 manager 已提交状态。

[`append_delta`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild.c#L138-L196) 在成功 append 时才推进 `current_generation`，每项 generation 严格递增；UPSERT 深拷贝 vector，避免调用方 buffer 生命周期泄漏到 replay。`rebuild_generation` 在一轮任务内保持为 baseline 起点。

产品写路径采用 commit-after-capture 语义：record 和 active index 的写入先成功，再调用 [`capture_upsert_delta`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector.c#L106-L158) 或 DELETE capture。append 失败不能回滚已经对用户成功的写入，而是把 rebuild 标为 `FAILED`，禁止这份 shadow 发布。

这是一项有意取舍：业务真值优先于派生任务。代价是 rebuild 失败后需要 retry，而不是假装 delta 已经完整。

## 19.7 same-record update、tombstone 与有序 replay

[`replay_deltas`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild_shadow.c#L873-L944) 严格按 append order 遍历，并拒绝 generation 非递增。顺序不能压缩成“最终操作集合”，因为同一 `record_id` 可能经历：

```text
UPSERT(v1) -> UPSERT(v2) -> DELETE -> UPSERT(v3)
```

UPSERT replay 若发现同 ID slot，先 mark-delete 旧 slot，再 add 新 vector；DELETE missing 视为幂等 no-op 并计数。这样 latest mutation 在可见结果上生效，但它仍沿用 v2 的逻辑删除模型。

因此必须准确表述：

```text
tombstone != 物理删除全部入边和出边
same-record update != 原 slot 原地改向量并完整 reconnect graph
```

重复 UPSERT 会留下 shadow 内部 tombstone，最终校验也不会强求 shadow `deleted_count` 等于 manager deleted records；它校验的是**可见 active record 集合与向量**一致。

## 19.8 reader safety：发布与销毁是两个危险点

真正的竞态不是“搜索算错一点”，而是 reader 拿到 old index 后，publisher 替换并释放它，形成 use-after-free。

R2 在 manager state 增加 `active_index_rwlock`。[`read guard`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector.c#L641-L680) 锁住 `current_index` 的获取与使用生命周期；publish 和 retired cleanup 获取 write lock。它没有把锁塞进 anncore，也没有依赖可能在 executor thread 中 no-op 的 weak owner lock wrapper。

协议是：

```text
VSEARCH: rdlock -> obtain/use active -> unlock
publish: wrlock -> move shadow to product active -> unlock
cleanup: wrlock -> destroy retired -> unlock
```

write lock 到手意味着此前持锁 reader 已退出，且新 reader 无法在移动中进入。发布成功后，新 reader只会得到新 active；cleanup 再次走 write guard，避免 retired 尚被旧 reader 使用时销毁。

第一版 `rwlock` 优先证明 lifetime correctness，代价是每次 VSEARCH 的锁开销和 writer 等待。当前没有 epoch、RCU 或 refcount 的性能证据，不能把它写成最终并发方案。

## 19.9 final validation：构建结束只是候选图

R5 把“可以搜索”与“可以发布”分开。[`validate_final`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector_rebuild_shadow.c#L560-L675) 依次检查：

1. HNSW basic graph invariants。
2. manager active/deleted counters 自洽。
3. shadow visible active IDs 能逐项回表到 manager active record。
4. shadow vector bytes 与 manager embedding 相同。
5. active count、record ID checksum、vector checksum 对齐。
6. 有限 search sanity 通过。
7. validation 前后两次 manager summary 相同。

只有全部通过，状态才从 `REPLAYING_DELTA` 进入 `READY_TO_SWAP`。fake ID、vector mismatch、generation order violation、graph invalid 或 validation 期间 records 改变都会 fail rebuild、销毁 shadow，publish helper也因状态不满足而拒绝发布。

证据边界同样重要：basic validation 证明结构可安全遍历，checksum 证明集合摘要一致，search sanity 只做有限查询；三者都不等于 100K recall gate，更不等于真实语义质量。

## 19.10 publish、retired 回收与失败原子性

最终发布链为：

```text
READY_TO_SWAP
-> acquire active write lock
-> validate publish ownership/type
-> allocate retired holder
-> enter SWAPPING
-> old active -> retired
-> shadow -> product core_hnsw_v2_index
-> finish_swap -> IDLE
-> release write lock
-> later acquire write lock
-> destroy retired
```

“失败不能破坏旧 active”主要由 publish point 之前的检查实现：validation failure、shadow missing、retired allocation failure、wrong backend 和 forced pre-publication failure 都发生在产品 active 移动前。

[`shadow swap test`](../../tests/vector_rebuild_shadow_swap_test.c) 验证发布前产品 VSEARCH 仍读旧图、发布后才读新图、retired 存在并可安全 cleanup；[`product integration test`](../../tests/vector_core_hnsw_v2_product_integration_test.c) 覆盖产品 backend 路径。它们证明已编码断言，不自动代表所有生产并发调度都已穷尽。

active + shadow + retired 短时共存会抬高峰值内存。当前选择用空间换构建隔离和回滚能力，没有给出 100K 在线重建峰值 RSS 的 commit-pinned 原始结果。

## 19.11 background worker、cancel 与 retry

R6 的 [`async run_once`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector.c#L221-L289) 复用前述闭环：

```text
acquire rebuild-worker read guard
-> shadow init / begin capture
-> cancel checkpoint
-> baseline scan
-> cancel checkpoint
-> delta replay + final validation
-> cancel checkpoint
-> reader-safe publish
-> best-effort snapshot
-> retired cleanup
-> release worker guard
```

[`start/status/cancel/retry`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector.c#L346-L500) 管理 one-shot pthread：start 拒绝重复运行和非 IDLE context；status 返回 running、state、progress、delta count、last status、cancel flag；cancel 设置请求，由阶段 checkpoint 协作响应；retry 会 join 已结束线程，将 FAILED/CANCELLED reset 为 IDLE 后再启动。

cancel 不是异步强杀。请求落在大段不可中断工作中时，要等下一 checkpoint；若发布已完成，也不能靠 cancel 免费恢复旧 active。shutdown 还必须请求停止并 join worker，避免线程继续访问已销毁 manager state。

更关键的是，当前 worker guard 会让写路径等待，因此真实 async 产品路径并没有充分利用 delta 去承接持续并发写流。delta 边界、顺序和产品 capture 已被单测验证，为下一步放开锁粒度提供正确性协议；但“VSET/VDEL 全程不停顿”仍是当前缺陷。

## 19.12 V 族命令接入与可观测性

[`handle_vindex_command`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/core/kvs_operation.c#L3325-L3387) 保留同步 `VINDEX REBUILD`，并新增：

```text
VINDEX REBUILD ASYNC / START
VINDEX REBUILD STATUS
VINDEX REBUILD CANCEL
VINDEX REBUILD RETRY
```

STATUS 是该命令自己的 bulk string，不改变 VSEARCH response，字段包括 running、rebuild state、progress done/total、delta count、last status 和 cancel requested。manager info 还暴露 rebuild state/error、shadow/retired presence 和 snapshot write status。

可观测性不能替代正确性：看到 `worker_running=1` 只证明线程已启动；看到 `BUILDING` 只证明进入 baseline 阶段；看到 `READY_TO_SWAP` 才证明门禁通过；只有 publish 成功后 reader 才切换到新 active。

VSET/VDEL capture 点位于各提交路径之后。[`delta capture test`](../../tests/vector_rebuild_delta_capture_test.c) 特别覆盖 same-record UPSERT、DELETE、duplicate DELETE、generation monotonic、VSEARCH 不产生日志，以及 append failure 不回滚已提交用户写入。

## 19.13 publish 后 snapshot 是旁路 cache 更新

R7 没有修改第十八章的 manifest 或 binary format。成功路径在 publish 返回之后调用 [`best-effort writer`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/74ec6af/src/vector/kvs_vector.c#L2960-L2995)：

```text
new active published
-> write current HNSW snapshot
-> record status/log
-> cleanup retired
```

writer 不在 active publish write lock 内，但 worker 仍持有 rebuild worker guard，避免 snapshot summary 与产品写发生数据竞争。snapshot 写失败只留下 warning 和 `snapshot_status`，不覆盖 rebuild 成功，不回滚已经对 reader 可见的新 active。

[`snapshot-after-publish test`](../../tests/vector_index_snapshot_shadow_publish_test.c) 覆盖 shadow publish 后 cache 行为；[`async worker test`](../../tests/vector_rebuild_async_worker_test.c) 还人为阻断 snapshot temp path，验证 writer 失败后 worker 仍报告 publish 成功且产品搜索可用。

这不意味着 snapshot 可靠性问题被本章重做：格式、启动校验、损坏 fallback 和 cold-start benchmark 已在第十八章结束，本章只讲 publish 后更新派生 cache 的时序。

## 19.14 测试矩阵、历史 PASS 与证据等级

| 证据 | 主要证明 | 不能证明 |
| --- | --- | --- |
| [`state machine test`](../../tests/vector_rebuild_state_machine_test.c) | transition、reason、cancel/reset | 后台并发安全 |
| [`delta capture test`](../../tests/vector_rebuild_delta_capture_test.c) | generation、commit 后 capture、append failure | 无限写流下无阻塞追赶 |
| [`shadow replay test`](../../tests/vector_rebuild_shadow_replay_test.c) | scan 前边界、ordered replay、validation failure | 规模 recall |
| [`shadow swap test`](../../tests/vector_rebuild_shadow_swap_test.c) | old/new 可见性、retired、forced failure | 所有 reader 调度交错 |
| [`async worker test`](../../tests/vector_rebuild_async_worker_test.c) | start/status/cancel/retry、snapshot failure | 长期 worker pool 稳定性 |
| [`snapshot publish test`](../../tests/vector_index_snapshot_shadow_publish_test.c) | publish 后写 cache | 完整 crash consistency |
| [`R-final 报告`](../hnsw_rebuild_productization/hnsw_r_final_product_rebuild_consolidation_report.md) | 当时构建、回归与静态检查记录为 PASS | 本次重新执行、生产长期可靠性 |

R1～R7 报告均记录了当时各自要求的 build、focused tests、core regressions 和 static boundary checks。这里应写“阶段报告记录 PASS”，不能写成“测试文件存在所以一定通过”，也不能冒充本次文章审计重新跑过全部测试。

本提交没有在线 rebuild 的时延、写阻塞分布、峰值 RSS、100K recall 或真实 embedding 原始结果。因此本章不提供性能倍率和 SLA 数字。机制测试与阶段报告足以证明最小控制面闭环，不足以证明生产负载下的性能闭环。

## 19.15 失败路径、工程取舍与当前缺陷

### 失败与回滚矩阵

| 失败点 | 处理 | old active |
| --- | --- | --- |
| shadow init/build 失败 | `FAILED`，销毁 shadow | 继续服务 |
| delta append/replay 失败 | `FAILED`，禁止 ready/publish | 继续服务 |
| final validation 失败 | 不进入 `READY_TO_SWAP` | 继续服务 |
| publish 前类型/内存检查失败 | 不移动产品对象 | 继续服务 |
| cancel checkpoint 命中 | `CANCELLED`，清理后可 retry | 继续服务 |
| worker 失败 | last status/error 可见 | 继续服务 |
| snapshot writer 失败 | warning/status，publish 不回滚 | 新 active 继续服务 |

### 当前缺陷

1. worker 是 one-shot thread，不是持久 worker pool，也没有任务队列。
2. `rwlock` 是第一版 reader lifetime primitive，高 QPS 成本未量化。
3. worker guard 对部分 VSET/VDEL 形成等待，尚未实现真正持续写入下的无阻塞 delta catch-up。
4. cancel 是 checkpoint 协作式，不保证任意点即时停止。
5. active + shadow + retired 与 snapshot serialization 可能造成显著峰值内存，缺少规模结果。
6. final search sanity 不是 recall gate；真实 T2Ranking 未进入本章。
7. snapshot writer 失败不影响在线正确性，但会让下次冷启动失去最新 cache。
8. 当前 async worker 仍是受 guard 保护的第二 publisher；尚不能据此宣称未来 single-command-owner 或主路径锁消除已经完成。

这些限制不是闭环失败，而是准确界定闭环：本章证明“旧 active 不被破坏、shadow 不提前可见、门禁后安全发布”，没有证明“所有读写负载下零停顿、低内存、低尾延迟”。

## 19.16 Agent 辅助边界

Agent 在这一阶段适合做的是机械但高风险覆盖广的工作：枚举状态转换、生成 failure injection case、核对每个 lock acquire/release、追踪 shadow 所有权、补齐 delta 顺序断言、比较报告与最终源码。

不能交给 Agent 自行决定的，是一致性语义本身：records 是否仍是真值、用户写入与 delta 谁先提交、append failure 是否回滚业务写、publish point 在哪里、snapshot failure 是否回滚 active。这些必须由工程不变量先定，再让 Agent 辅助验证。

本次复盘也遵循同一边界：以 `74ec6af` 历史源码、测试断言和 R1～R7 报告交叉核验；没有因为最终代码存在 worker 就倒推每个中间 skeleton 当时已经具备产品能力，也没有把报告中的历史 PASS 写成本次重新执行结果。

## 19.17 面试表达与本章小结

### 1 分钟版本

“同步 HNSW rebuild 会阻塞服务，直接放到线程里又会产生三类问题：baseline 之后的写会丢、发布时 reader 可能看到半成品、旧图可能在 reader 使用中被释放。我们把重建拆成 active/shadow/retired 生命周期和 `BUILDING -> REPLAYING_DELTA -> READY_TO_SWAP -> SWAPPING` 状态机。capture 在 full scan 前开启，VSET/VDEL 提交后按 generation 记录 UPSERT/DELETE；shadow 顺序 replay 后做 graph、record ID、vector checksum和有限搜索校验。只有 gate 通过才在 manager rwlock 写侧发布，旧 active 延迟回收；失败和 cancel 都保留旧 active。最后 one-shot worker 接入 VINDEX start/status/cancel/retry，publish 后 best-effort 写 snapshot。当前 VSEARCH 可在线，但 worker guard仍会让部分写等待，所以这不是最终的写路径零停顿方案。”

### 3～5 分钟版本

1. 背景：snapshot 解决冷启动 cache，不解决运行中替换；同步 rebuild 的构图成本会阻塞调用线程。
2. 一致性边界：records 是 source of truth；先 begin capture 再扫 baseline，避免扫描窗口漏写。
3. 增量语义：delta 只含 generation/op/record_id/vector；用户写先提交，append 失败 fail rebuild 而不回滚业务。
4. shadow 收敛：按 generation replay；same-ID UPSERT 逻辑删除旧 slot 再 add，DELETE missing 幂等；tombstone 不等于物理清边。
5. 发布门禁：graph basic validation、active ID/vector checksum、回表、search sanity、manager double-summary 全通过才 ready。
6. reader lifetime：VSEARCH read lock，publish/cleanup write lock；shadow 发布前不可见，old active 进入 retired 后再安全释放。
7. 产品控制面：one-shot worker + ASYNC/STATUS/CANCEL/RETRY；失败保留 old active，snapshot writer 失败不回滚 publish。
8. 边界：阶段报告记录测试 PASS，但没有规模性能结果；VSEARCH 在线，部分 VSET/VDEL 仍被 worker guard 串行化。

### 常见追问

**为什么 capture 必须早于 full scan？**

否则 start-scan 之间存在漏写窗口。提前 capture 最多造成 baseline 与 delta 重复，UPSERT replay 可以按 stable record ID 收敛；漏记则无法恢复。

**为什么不在写入前先 append delta？**

那会记录最终未提交成功的 mutation。当前选择业务先提交，delta 失败使 rebuild 作废并 retry，保证 records 真值不被派生任务绑架。

**构建完成为什么不能直接 swap？**

因为 delta 可能未追平，graph 可能结构异常，shadow active IDs/vectors 可能与 records 不一致。`READY_TO_SWAP` 是验证结果，不是 build 的别名。

**rwlock 如何避免 use-after-free？**

reader 持读锁覆盖 index 使用期；publisher/cleanup 只有拿到写锁才能移动或销毁对象，因此必须等已有 reader 退出。

**publish 后为什么不立刻 free old active？**

发布和回收是不同所有权事件。old active 先进入 retired，再在 reader-safe cleanup 中释放，协议更清晰，也能处理 publish 后旁路工作。

**delta 是否证明异步期间 VSET/VDEL 完全不停？**

不证明。语义和产品 capture 点已经实现，但 R6 worker guard 仍会让部分写等待；本阶段主要实现查询在线和安全发布。

**snapshot 写失败为什么不回滚？**

新 active 已通过 records 一致性门禁并发布；snapshot 只是派生 cache。回滚正确索引去迁就 cache 失败会颠倒真值优先级。

### 本章结论

```text
同步 rebuild 阻塞且不能安全热替换
-> 状态机把 build/replay/ready/publish 分开
-> capture-before-scan 关闭 baseline 漏写窗口
-> ordered delta replay 追到 manager 最新可见状态
-> final validation 才授予 READY_TO_SWAP
-> rwlock 保护 reader/publish/cleanup 生命周期
-> active/shadow/retired 支持失败保留旧图
-> one-shot worker 接入 cancel/retry/status
-> publish 后 best-effort 更新 snapshot
```

已经证明的是：在当前 guard 和测试口径下，shadow 不会提前服务，validation 失败不会发布，publish 对 reader 具有互斥可见性，worker/cancel/retry 和 snapshot failure 路径都有明确状态，旧 active 在失败时继续服务。

尚未证明的是：持续并发 VSET/VDEL 下的无阻塞追赶、100K 在线 rebuild 尾延迟与峰值内存、完整并发调度空间、真实 embedding recall，以及长期 worker/单 owner 架构。

下一篇只进入“第二十章：真实 T2Ranking 的 10K/100K 证据闭环”，候选范围 `fd439e9^..3b247a3`。
