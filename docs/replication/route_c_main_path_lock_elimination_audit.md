# 路线 C 串行命令执行与主路径去锁审计

## 1. Executive conclusion

本次为严格只读架构审计。生产代码、测试、Makefile、配置、公开协议、RDB/AOF 格式和 `kvs_vector_index_ops_t` 均未修改。

明确选择 **路线 C**：

```text
先冻结唯一 command owner、线性化点和后台 publish 边界
-> 继续 Replication Stage 1A/1B
-> slave apply 必须进入同一 command owner
-> 保留现有 correctness locks
-> 按普通 KV、vector、后台 snapshot/rebuild 分阶段删除已证明冗余的主状态锁
```

当前已经具备单 command executor 的主要基础：Reactor、Proactor、NtyCo、TTL scan、普通 TCP slave apply、RDB/AOF/full-sync snapshot creation 都已收敛到 `g_executor`。但当前还不能宣称数据库状态完全单 owner：async vector rebuild worker 直接读取 manager records、推进 rebuild context、发布 active/shadow、清理 retired，并在 worker 线程序列化 active HNSW snapshot。因此现在不能全面删除 vector correctness locks。

本报告中的“主路径去锁”只表示：

```text
数据库命令在单一串行执行域内不再为 storage/vector 主状态互相加锁。
```

它不要求 command/response queue、AOF、allocator、replication connection、worker lifecycle 或 HNSW 内部具有 lock-free progress guarantee。

## 2. “主路径去锁”目标定义

目标并发模型：

```text
Network owner
  -> parse/copy command task
  -> cross-thread queue

Single command owner
  -> serial linearization
  -> storage/TTL/vector commit or read
  -> AOF order
  -> replication mutation creation/order
  -> response task

Prepare workers
  -> immutable embedding/shadow/snapshot preparation only
  -> completion task back to command owner

Replication subsystem
  -> owns connection/backlog/ACK/internal locks
  -> never reads or mutates live database state directly
```

必须区分：

| 概念 | 本阶段含义 |
| --- | --- |
| 串行命令执行 | 一个 owner 在任意时刻只执行一个数据库操作 |
| 数据库单写者 | storage、TTL metadata、vector records/maps/active-index publish 只有 owner 可修改 |
| 主路径去锁 | owner 内部访问主状态不需要数据库 mutex/rwlock |
| 无锁队列 | 非必要目标；跨线程队列可暂时用 mutex/cond |
| RCU/COW | 后续隔离后台 reader/publish 的工具，不是当前前置条件 |
| 完全 lock-free/wait-free | 非目标；I/O、allocator、ONNX 和 shutdown 均可阻塞 |
| replication 内部无锁 | 非目标；只要求其锁不成为数据库所有权锁 |

当前优先选择一个 executor，不采用分片 executor 或并行 reads。分片会立即重新引入跨 shard order、multi-key、TTL、AOF、replication sequence 和 vector index owner 问题，当前没有必要的吞吐证据。

## 3. 路线 C 原始设计复核

附件指向的 `docs/interview-prep/route_c/` 当前不存在；实际历史资料位于 `docs/route_c/`。源码与历史文档不一致时以当前源码为准。

### 3.1 历史演进

关键提交：

| commit | 当前相关意义 |
| --- | --- |
| `5b64d56` | Proactor 命令异步执行闭环 |
| `93e2e7b` | embedding prepare worker pool |
| `13a629a` | TTL scan 收敛到 executor |
| `6da5e83` | 普通 TCP slave apply 接入 executor |
| `efe23bc` | full-sync 使用 executor snapshot |
| `f466ae7` | RDB save 使用 executor snapshot |
| `ec7c1cc` | AOF rewrite 使用 executor snapshot |
| `b851923` | NtyCo 和 eBPF fallback 接入 executor |
| `b1feec1` | owner wrapper、weak-lock 与 real-lock fallback 验证 |
| `01b50c1` | Route C closeout；默认 weak-lock |
| `15deef1` / `74ec6af` | Route C 后新增 HNSW snapshot/async rebuild，重新引入 vector 后台 owner 边界 |

历史 readiness 文档曾列出 NtyCo、TTL、RDB/AOF、full-sync 和 slave apply 直接访问 live state；上述提交已修正这些路径。历史测试后来被清理，当前报告把提交和文档作为演进证据，不把已删除测试脚本当作当前可执行证明。

### 3.2 可继续复用的设计

1. 网络线程只复制 command，并以 task ownership handoff 进入 executor。
2. executor 是唯一普通命令执行线程；command task 完成后生成 response task 回原网络 owner。
3. session 通过 identity/generation、single-inflight 和引用/同步等待维持生命周期。
4. TTL scanner 只产生 scan task，不直接删除 store。
5. RDB、AOF rewrite 和 full sync 由 executor 创建 immutable snapshot，再在调用线程执行 I/O。
6. embedding worker 只做 tokenizer/ONNX prepare，最终 VSET/VSEARCH 在 executor 执行。
7. weak-lock + real-lock fallback 是迁移护栏，不是数据库并发设计本身。

### 3.3 已失效或不完整的部分

- Route C closeout 早于 async rebuild；现在 rebuild worker 是 active vector state 的第二个 publisher。
- current command、prepared、replication、TTL 和 snapshot 使用独立队列与固定优先级，不存在统一 admission sequence。
- worker prepared queue 按完成顺序入队，没有全局 reorder buffer。
- `g_kv_data_rwlock` 在 executor 上虽为 no-op，但每次 wrapper 都获取 `g_owner_lock_stats_mu`，所以“weak-lock”仍不等于调用链零 mutex。
- Reactor/Proactor 源文件仍保留未被正式异步循环调用的同步 session handler；AOF load 在启动期仍直接调用 `kvs_filter_protocol()`。这些是兼容/启动入口，不应误报为当前网络 runtime 第二 executor。

## 4. 当前命令执行与线程拓扑

| 模块 | 线程 | 读取/修改的共享状态 | 当前 owner与同步 | 主路径 | 是否破坏单写者 |
| --- | --- | --- | --- | --- | --- |
| Reactor | epoll owner | session/parser/buffers；不执行 DB handler | task submit + response eventfd；session ref | 是 | 否 |
| Proactor | io_uring owner | CQE/session/buffers；不执行 DB handler | task submit + response eventfd/ref | 是 | 否 |
| NtyCo | cooperative scheduler/coroutine | session/buffers；同步等待自己的 response | task submit，等待 owner-filtered response | 是 | 否 |
| command executor | 单 pthread | 所有普通 KV、TTL、client V command、普通 slave apply、snapshot copy | `cmd_mu/cv` ingress；执行时单 owner | 是 | 目标 owner |
| command task | network创建，executor/worker持有 | argv/context/prepared result/response link | ownership transfer | 是 | 否 |
| response queue | executor producer，活动网络 owner consumer | response/session identity | `resp_mu` + eventfd | 是 | 不接触 DB |
| 四 storage backend | 无独立线程 | records/nodes/TTL metadata | runtime handler 由 executor调用；backend本身无 pthread data lock | 是 | 否 |
| TTL scanner | 1 pthread | 只读配置/role；不读写 store | atomic stop，提交 TTL scan task | 调度路径 | 否 |
| RDB save | main/control caller + executor snapshot | caller写 snapshot file；executor复制 live DB | per-task mutex/cond | 低频 | 否 |
| AOF append/fsync | executor + AOF worker | AOF buffer/fd/rewrite state | AOF mutex/cond/data rwlock | post-commit | 不写 DB，但锁可阻塞 owner |
| AOF rewrite | rewrite caller/worker + executor snapshot | immutable rewrite snapshot、增量 buffer | snapshot task + AOF locks | 低频 | 否 |
| vector manager | executor + async rebuild worker | records/maps/index/rebuild/status | vector rwlocks/mutex | 是 | **是，后台边界未收敛** |
| embedding prepare | N workers | task私有 text/embedding、runtime state | worker queue mutex/cond | prepare | 否 |
| core_flat | executor调用 | index arrays/deleted flags | active/rebuild guards | 是 | 同步命令内否 |
| core_hnsw_v2 | executor；rebuild worker持 private shadow/发布 | active graph、shadow、search scratch | active rwlock + rebuild rwlock | 是 | worker publish导致例外 |
| async rebuild | 1 pthread | 直接读 active records/rebuild ctx；publish active；写 snapshot/status | rebuild rwlock/mutex + active rwlock | 会阻塞/影响主路径 | **是** |
| vector snapshot | 无独立专用线程 | sync rebuild时 executor写；async时 rebuild worker直接序列化 current index | 依赖 rebuild/active稳定性 | 低频但可长耗时 | async路径越过 owner |
| TCP master | accept/per-slave + sender | slave list、immutable full-sync snapshot、transient send queue | `slave_mutex` + atomic ring | post-commit handoff | 不写 DB |
| TCP slave | receive thread | wire frame；构造 replicated task | executor repl queue | 是 | 否；普通 KV apply已收敛 |
| eBPF daemon | 独立进程/target workers | captured bytes/target queues | daemon内部锁；通过公开连接进入 server | 实验 | server apply仍进 executor |
| shutdown/control | main + background threads | stop flags、snapshot、executor、store destruction | stop/join/fallback lock | 控制面 | 顺序仍需加固 |

直接回答当前拓扑：

1. 当前只有一个 `g_executor.thread`，不存在多个 executor 并发执行数据库命令（`src/core/kvs_command_executor.c:25-59,2888-2995`）。
2. 三种正式网络模式都走 `kvs_command_executor_submit()`；差异仅在 I/O/session lifecycle。NtyCo build 使用 `src/network/ntyco.c`，不是遗留 `ntyco_defult.c`。
3. 四种 store 没有自己的数据锁；它们依赖上层 owner。配置为 pool allocator 时，分配器内部锁仍会被调用。
4. client VSEARCH/VSET/VDEL 不会彼此并发执行；但 VSEARCH 可与 async rebuild worker并存，依赖 vector locks。
5. active TTL scan只提交 task，删除由 executor执行（`src/core/kvs_ttl_background.c:11-31`；executor `2607-2635`）。
6. RDB/AOF rewrite不再直接遍历 live DB；executor先创建快照（executor `2661-2707`；`src/persistence/kvs_rdb.c:522-534`）。
7. async rebuild worker直接读 active records并直接 publish active/shadow；不是纯 completion producer。
8. async rebuild后的 vector snapshot在 worker线程直接序列化 current HNSW；没有独立稳定 generation image。
9. 普通 TCP slave receive通过 `kvs_command_executor_submit_replicated()`，不直接 apply（executor `3280-3346`）。
10. shutdown先停 TTL并通过仍在运行的 executor做 RDB snapshot，随后 stop executor，最后在 `dest_kvengine()` 中停 distributed/AOF并销毁 store；replication producer停止晚于 executor，是需要后续 lifecycle gate 的风险（`src/core/kvstore.c:424-440`）。

## 5. 当前数据库状态 owner

### 5.1 已验证边界

- 普通 client GET/SET/DEL/EXPIRE/PERSIST/FULLDUMP：executor。
- 四 backend的 lazy expire retry：同一 executor内由 read语义切为 write语义。
- TTL active scan/delete：executor。
- 普通 TCP slave apply：executor。
- client VSET/VGET/VSEARCH/VDEL/VINDEX/VINFO：executor。
- RDB/AOF load：网络和 executor启动前的 startup owner；不是 runtime writer。
- shutdown destroy：executor停止后的 main/control owner，依赖 stop/join顺序。

### 5.2 第二 owner/待收敛路径

`kvs_vector_rebuild_async_worker_main()` 直接执行：

```text
read active records
-> build/replay private shadow
-> publish ready shadow
-> serialize current index snapshot
-> retire cleanup
-> update rebuild/snapshot status
```

证据见 `src/vector/kvs_vector.c:222-330`。worker持 `rebuild_worker_rwlock` read覆盖整个流程，而 VSET/VDEL持 write（`3999-4003,4429-4432`），因此当前正确性通过长时间阻塞产品写维持。它不是目标“后台私有 prepare -> owner publish”模型。

所以：普通 KV runtime mutation 已单 owner；整个数据库（含 vector publish/status）尚未完全单 owner。

## 6. 命令主路径锁清单

### 6.1 分类表

| 类别 | 锁/同步对象 | 文件/函数 | 主路径调用线程 | 保护对象/不变量 | single owner下必要性 | 能否现在删除 / 前置条件 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 冗余候选 | `g_kv_data_rwlock` owner wrapper | `kvs_command_task_lock/unlock()`；TTL/repl/snapshot executor sites | executor | 历史多 handler并发；shutdown fallback | executor内逻辑冗余，默认已no-op | 否；先证明所有runtime DB入口和shutdown owner |
| 1 冗余候选 | `g_owner_lock_stats_mu` | `kvs_owner_lock_record_call()` | 每次owner wrapper | profiling counters一致 | 不应为DB正确性存在 | 否；先用owner-local/atomic stats替代并保留fallback观测 |
| 2 后台并发 | `active_index_rwlock` | `kvs_vector_active_index_acquire_*()` | VSET/VDEL/VSEARCH + rebuild worker | active graph不在search/add/swap/serialize中被替换 | client命令之间冗余；对worker仍必要 | 否；worker只建private shadow，publish/snapshot回owner |
| 2 后台并发 | `rebuild_worker_rwlock` | VSET/VDEL/VSEARCH/rebuild worker | executor + rebuild worker | records/delta/rebuild流程稳定 | 当前必要且会长时间阻塞写 | 否；冻结immutable baseline、owner-only delta/publish |
| 2 后台并发 | AOF mutex/data rwlock | `aof_append()` | executor + fsync/rewrite | buffer/fd/rewrite增量顺序 | 不是DB状态锁，但在post-commit主路径 | 否；owner-only append buffer或有界handoff后再评估 |
| 3 lifecycle | `cmd_mu/cmd_cv` | executor submit/pop/start/stop | network/workers/TTL/repl/control | task只入队一次，stop/drain完整 | 跨线程必需 | 否；无锁queue不是当前目标 |
| 3 lifecycle | `resp_mu` + eventfd | push/drain response | executor + network owner | response/session ownership不丢 | 跨线程必需 | 否；按poller分区后才可考虑SPSC |
| 3 lifecycle | embedding pool mutex/cond | worker pool | executor + N workers | job owner、stop/join、queue lifecycle | 必需 | 否 |
| 3 lifecycle | rebuild worker mutex | async start/cancel/status/shutdown | executor + rebuild worker | thread状态、cancel、join | 必需 | 保留低频控制锁 |
| 3 lifecycle | snapshot task mutex/cond | executor snapshot builders | caller + executor | immutable snapshot完成通知 | 必需 | 保留或换future，不属于DB锁 |
| 3 lifecycle | `slave_mutex` | distributed/TCP sender | accept/send/stop threads | connection fd/list生命周期 | replication内部必要 | 保留 |
| 4 module internal | mempool locks | `src/storage/kvs_mempool.c` | 所有分配线程 | allocator free list/large blocks | worker并发分配时必要 | 保留；不等于store data lock |
| 4 module internal | AOF rewrite mutex/cond | AOF subsystem | control/bg worker | rewrite只能一个、buffer/fd安全 | 必要 | 保留低频内部锁 |
| 4 module internal | log mutex | `src/utils/kvs_log.c` | 所有线程 | sink/file lifecycle | 必要 | 通过异步日志隔离，不作为DB owner gate |
| 4 module internal | TCP/eBPF queue/connection locks | distributed/daemon | replication threads | transport object lifecycle | 允许 | 只禁止进入live DB或vector critical section |

### 6.2 解释

默认 weak mode 下，executor调用 `g_kv_data_rwlock` 不进入 pthread rwlock，但会在 `kvs_owner_lock_record_call()` 中获取 stats mutex（`src/core/kvs_owner_lock.c:37-59,89-140`）。因此它是架构上已经弱化、实现上仍有小锁的过渡层。

`cmd_mu`、`resp_mu`、AOF、allocator 和 replication locks 不应因“single command owner”被机械删除：它们保护跨线程对象或模块私有状态，而不是允许第二线程写数据库。

本次在正式数据库主路径中未发现 `pthread_spinlock`、`atomic_flag` 或 semaphore。现有 atomics主要用于stop/running、session refcount、queue index和统计，不应与“数据库主状态无锁”混为一谈。

## 7. 普通 KV 串行执行分析

- SET/GET/DEL、四 backend命令以及读命令全部通过一个 executor；backend本身可在唯一 owner下无数据锁运行。
- 当前阶段应让 reads继续串行。并行GET会立即要求store reclamation、lazy expire和snapshot一致性的新协议。
- 每个连接有 `single_inflight`，网络层一次解析/提交一个命令；同连接 Pipeline按输入/响应顺序串行，但吞吐也受这一门禁限制。
- 不同客户端的 command queue是FIFO append，但没有跨producer“网络到达时间”的可复现定义；真正线性化点是owner执行handler/commit。
- lazy expire发生在executor：read handler返回retry后，同一个task切到write语义并删除（`src/core/kvs_command_task.c:599-630`）。
- active expire由TTL线程调度、executor扫描/删除；slave role不主动扫描。
- AOF append在owner的post-commit顺序调用，但会获取AOF内部锁；大命令还可能直接写文件/fsync（`src/persistence/kvs_aof.c:181-314`）。这不会产生第二DB writer，但可能阻塞所有命令。
- RDB、AOF rewrite、TCP full sync都由owner复制immutable snapshot后再I/O，不需要长持有DB rwlock。
- `FULLDUMP` public命令仍在executor中同步遍历并组装大response，会造成owner head-of-line blocking；它不需要DB锁，但长期应复用snapshot image而不是长时间占用owner。
- startup RDB/AOF replay可作为“服务未发布前的startup owner”保留；若未来在线load/replay，必须转为owner task。

结论：ordinary KV runtime serial execution成立；去掉wrapper前仍需验证所有配置/网络模式、direct test入口和shutdown fallback。

## 8. 向量命令串行执行分析

| 步骤 | 当前线程/owner | 串行域/跨线程 | 当前锁 | owner模型下结论 |
| --- | --- | --- | --- | --- |
| embedding prepare | worker或executor | task私有，跨线程 | worker queue/runtime locks | 保留prepare并行，不得写manager |
| logical-key lookup | executor | 串行域 | rebuild rwlock | records/maps唯一owner后无需DB锁 |
| record/ID allocation | executor | 串行域 | rebuild rwlock | Stage 1A改为prepare/reserve后无失败commit |
| record store/maps | executor | 串行域 | rebuild rwlock | owner-only mutation |
| active flat/HNSW add/delete | executor | 串行域 | active + rebuild rwlock | client命令间无需锁；worker隔离前不能删 |
| VSEARCH active index/read resolve | executor | 串行域 | active read + rebuild read | 当前目标与writes串行，不做无锁并行search |
| rebuild delta append | executor commit path | 串行域，但worker replay读取 | rebuild rwlock/context | delta只能owner append；worker使用冻结batch/private copy |
| shadow build/replay | rebuild worker | 跨线程private对象 | rebuild locks | 只允许private shadow；禁止改active/records |
| active/shadow swap | 当前rebuild worker | 跨线程 | active write lock | 必须改为completion task回owner publish |
| retired cleanup | 当前rebuild worker | 跨线程 | guards | owner在确认worker不再借用后回收；低频barrier可用 |
| snapshot serialize | sync owner或async worker | async跨线程读current graph | rebuild稳定性 | owner生成stable image/private generation后worker I/O |
| AOF/future mutation | executor post-commit | 串行域到模块handoff | AOF/queue内部同步 | 生成顺序必须等于owner commit顺序 |
| response | executor -> network owner | 跨线程 | response queue | commit/post-commit完成后再响应 |

`core_flat` 在所有操作只由command owner调用、后台只持private copy后，不需要主路径data lock。`core_hnsw_v2`也不需要让原地图add/delete与VSEARCH并发；当前产品目标是三者在owner内串行。HNSW内部无需完全lock-free。

当前 vector serial execution只能评为部分成立：client command是串行的，但 async rebuild publish/snapshot不在owner。

## 9. Embedding prepare/commit 顺序

当前 Route C 已实现方案 2：worker只执行 `kvs_command_task_prepare()`，prepared task按完成顺序回到 `prepared_head/tail`，executor优先消费prepared queue（`src/core/kvs_embedding_worker_pool.c:70-151`；executor `912-925,2931-2952,3161-3186`）。

因此：

```text
client A 的 VSET 开始 prepare
client B 的 VSET 开始 prepare
B 先完成
=> B 当前可以先回队并先 commit
```

这不产生多writer。协议已有 `single_inflight`，所以同一连接的命令不会发生这种重排；跨客户端调用时间重叠，本来就没有公开“socket到达先后”保证。推荐不引入全局reorder buffer，而冻结以下语义：

1. 同一连接严格按request/response顺序。
2. 跨客户端重叠命令以command owner实际commit为唯一全局线性化顺序。
3. replication sequence在无失败commit边界由owner分配，必须与实际commit顺序一致。
4. prepare完成顺序可影响谁先进入commit，但worker不得分配record ID、sequence或修改任何manager状态。
5. 若未来产品明确要求“首次executor dequeue顺序”，再增加admission sequence + reorder buffer；这会造成慢ONNX全局队头阻塞，不应作为复制正确性的隐含要求。

当前缺口不是多writer，而是该线性化规则尚未作为正式契约和测试冻结。

## 10. 后台任务与主路径隔离

| 后台任务 | 当前方式 | 目标方式 | 是否允许锁/短barrier |
| --- | --- | --- | --- |
| TTL active expire | scanner只提交task，owner扫描/删除 | 保持 | scanner stop/join允许 |
| RDB save | owner复制immutable snapshot，caller写文件 | 保持；限制单次copy预算或分批 | task cond允许；无需DB lock |
| AOF rewrite | owner复制snapshot，rewrite处理文件+增量buffer | 保持；AOF内部锁隔离 | 允许AOF locks；避免长I/O在owner |
| TCP full sync | owner复制snapshot，per-slave线程发送 | 保持数据隔离；未来绑定base sequence | connection/queue locks允许 |
| HNSW rebuild | worker读active records并持长rwlock，直接publish | owner创建immutable record baseline；worker只建private shadow；completion回owner publish | lifecycle mutex允许；publish是短owner barrier |
| HNSW snapshot | async worker直接序列化current active | owner/private shadow生成stable snapshot image，worker只写文件 | file/status锁允许；不长持active lock |
| observability | atomic counters +若干mutex stats | owner-local counters或atomic snapshot | 允许；不得为了stats让每命令取全局mutex |

如果复制stable snapshot成本无法一次性承受，可在owner中分批复制，但每一批必须有generation校验并在最后验证；不能让worker裸读正在修改的records/graph。低频shutdown可以stop intake、drain executor、join workers后直接销毁，这是一种明确stop-the-world，不需要伪装成lock-free。

## 11. Replication 对单 owner 模型的影响

### 11.1 主节点

推荐边界：

```text
command owner prepare mutation并预留reliable stream/AOF所需容量
-> no-fail database commit + sequence assignment + 发布到已预留slot
-> wake/notify replication owner（允许重复或丢wake，但mutation不得丢）
-> response
```

- replication thread不得读取records/maps/index，只读取自包含immutable mutation。
- transient TCP send queue与reliable mutation backlog必须分离；当前 `g_tcp_queue`不是backlog。
- executor是唯一producer时，handoff适合bounded SPSC/owner append接口。replication内部可以用mutex，但command owner不得持vector/store lock再获取connection/ACK/backlog mutex。
- queue/reservation/allocator failure必须在可失败prepare阶段暴露；commit后handoff只能消费已预留资源，不得再失败。Stage 1A/1B需要冻结“失败不commit、不分配成功sequence、不响应成功”。
- 主节点不等待slave ACK；本地可靠stream是否接受是成功边界的一部分。
- 当前 `master_sync_command()` 的返回在post-commit中未形成可回滚错误，而且atomic ring的push只支持单producer，却会被normal post-commit和full-sync threads共同调用。这是现有有限复制风险，不应复制到future mutation接口。

### 11.2 从节点

当前普通KV路径已经满足基本模型：receive/decode线程调用 `kvs_command_executor_submit_replicated()`，executor串行apply并在成功后append AOF（`src/core/kvs_command_executor.c:2270-2313,3280-3346`）。future vector mutation必须复用这一owner边界，而不是在TCP thread调用VSET/VDEL/backend。

未来可并行decode/checksum/embedding-copy prepare，但apply、record ID、HNSW add/delete和`applied_offset`成功推进必须在owner定义的sequence顺序内完成。replication status/ACK可由replication线程拥有，owner只返回apply completion。

### 11.3 对主路径锁的结论

主从同步内部不要求无锁。允许connection、send、ACK、backlog和state machine使用锁，只要：

1. 不成为第二数据库writer；
2. 不把其mutex嵌套进storage/vector commit；
3. 主路径只执行有界handoff/backpressure；
4. slave apply进入统一owner。

## 12. 三方案比较

| 路线 | 优点 | 缺点和返工 | 结论 |
| --- | --- | --- | --- |
| A 先完成复制 | 快速推进功能 | 容易让TCP thread直接apply、让sequence脱离commit、把replication mutex嵌进VSET；后续去锁需重写wire/lifecycle | 不推荐 |
| B 暂停复制全面去锁 | 可一次清理旧wrapper | 会把async rebuild、AOF、queue、shutdown一起扩大；缺future mutation/full-sync真实约束；阻塞Stage 1A | 不推荐 |
| C 先冻结owner再继续 | 只冻结入口、线性化、handoff和publish；暂留locks保证正确性 | 短期存在过渡锁和已知阻塞 | **推荐** |

## 13. Stage 1A 应冻结的设计边界

Stage 1A继续，但实现前必须把以下规则视为硬接口：

```text
VSET/VDEL唯一commit线程 = command owner
embedding worker = immutable prepare only
records/logical map/record-id map唯一writer = command owner
HNSW active add/delete唯一caller = command owner
rebuild delta唯一append writer = command owner
shadow build = worker private state
active/shadow publish = command owner completion task
future mutation唯一生成/sequence点 = owner no-fail commit boundary
response = commit + required local durability/handoff完成后
```

Stage 1A不得新增VSET全局mutex、vector manager大锁、records/HNSW嵌套锁层级或replication mutex进入vector commit。lossless reserve和failure atomicity应通过allocate/copy/validate后owner单点publish实现。

## 14. 推荐目标架构

| 对象 | 唯一owner | 其他线程访问方式 | 跨线程消息/结束位置 | 内部锁 |
| --- | --- | --- | --- | --- |
| database/storage | command owner | immutable snapshot | command task；shutdown owner在drain后销毁 | 主状态目标无锁 |
| TTL metadata | command owner | scanner只提交tick/task | owner删除；shutdown join scanner | scanner lifecycle可有锁/atomic |
| vector records | command owner | immutable rebuild/RDB image | owner commit/destroy | 主状态目标无锁 |
| logical-key map | command owner | 不直接共享 | owner | 无需 |
| record-id map | command owner | 不直接共享 | owner | 无需 |
| active HNSW/flat | command owner | query也在owner；worker只见private copy | owner publish/retire | publish收敛前保留fallback |
| shadow HNSW | rebuild worker | owner只接completion descriptor | 失败worker销毁；成功owner接管 | worker私有无需data lock |
| rebuild delta | command owner append | worker消费冻结batch/copy | replay completion回owner | handoff可有短锁/queue |
| snapshot image | command owner生成 | serializer只读 | serializer完成销毁 | completion锁允许 |
| AOF state | AOF subsystem | owner按commit order append/handoff | AOF shutdown | 允许内部锁 |
| RDB snapshot | command owner生成 | save caller只读 | save caller销毁 | task cond允许 |
| replication mutation | command owner生成/sequence | replication owner只读 | backlog安全水位后回收 | immutable；subsystem可锁 |
| replication backlog | replication owner或冻结的单append协议 | owner只做有界accept | replication shutdown/recovery | 允许内部锁，不接触DB |
| slave apply task | replication receiver创建 | command owner接管并commit | apply completion/destroy | ingress queue可锁 |
| command task | network owner创建 | worker仅prepare，ownercommit | executor销毁或response转移 | queue可锁 |
| response task | command owner创建/完成 | network owner发送 | network owner销毁 | queue可锁 |

## 15. 分阶段实施路线

| 阶段 | 目标/修改范围 | 前置与完成/测试门禁 | 是否阻塞复制 |
| --- | --- | --- | --- |
| Main-Path Lock Stage 0（本审计） | 当前topology/locks/owner审计 | 文档、flags、diff check | 不阻塞 |
| Main-Path Lock Stage 1 | 冻结single executor、commit linearization、prepare/publish/handoff assertions；不删锁 | 三网络模式入口静态门禁；第二writer fail-fast；顺序测试设计 | **是，作为1A实现前契约** |
| Replication Stage 1A | HNSW lossless growth、VSET/VDEL失败原子性；不实现wire | fault injection、graph checksum、失败无AOF/mutation/response success | 主线，继续 |
| Replication Stage 1B | deterministic vector mutation、本地可靠accept、slave task prepare | no-ONNX、ID/order parity、backpressure/alloc fault | 主线 |
| Replication Stage 2 | immutable full-sync snapshot、base sequence、slave owner apply、READY | concurrent writes、atomic install、READY query gate | 主线 |
| Main-Path Lock Stage 2 | 删除普通KV executor内wrapper/stats mutex；保留startup/shutdown fallback | real/weak parity、TTL/slave/Pipeline/TSAN | 不阻塞Stage 3 |
| Main-Path Lock Stage 3 | vector worker只private build；publish回owner；删除client command中的active/rebuild data locks | VSET/VDEL/VSEARCH/rebuild/snapshot race/fault gates | 不阻塞Stage 3 |
| Main-Path Lock Stage 4 | snapshot/RDB/AOF/FULLDUMP访问全部基于immutable image；降低owner HOL | persistence parity、long snapshot mixed workload | 不阻塞 |
| Replication Stage 3 | offset/ACK/backlog/reconnect；内部锁允许 | gap/overrun/reconnect/multi-replica | 主线 |
| Main-Path Lock Stage 5 | shutdown顺序、fallback和残余lock收敛 | stop producers -> drain owner -> join -> destroy；TSAN/ASan | 最终门禁 |

不需要新增“全面无锁化”大阶段，也不在Stage 1A重写队列或HNSW layout。

## 16. 测试和 profiling 门禁

### 16.1 correctness

- Reactor/Proactor/NtyCo对同一script产生相同命令结果和commit trace。
- 同连接Pipeline严格顺序；多客户端每次运行都有完整、唯一、单调owner commit sequence。
- VSET overwrite/VDEL冲突按owner commit顺序得到确定最终态。
- TTL expire task与SET/PERSIST/DEL的owner顺序可记录并重放。
- embedding jobs强制乱序完成；同连接不乱序，跨连接按实际owner commit sequence复制。
- async rebuild worker不得调用active publish API；completion只能由owner处理。
- slave receiver不能调用store/vector mutation；apply completion后才推进状态。
- shutdown先拒绝producer，再drain owner，join所有worker后销毁；无late submit/UAF。
- TSAN无storage/vector manager主状态race；ASan/UBSan无生命周期错误。
- allocator/index/AOF/mutation handoff fault下无半提交、无虚假success sequence。

### 16.2 profiling

后续测量：owner wrapper/stats `lock_wait`、executor各queue wait/depth、embedding prepare/HOL、AOF append/fsync、replication accept、active/rebuild lock wait/hold、snapshot/rebuild对普通命令p95/p99阻塞。Route C已有证据显示VSET lock wait约`0-2 us`，主要hold成本是HNSW add；weak-lock相对real-lock也没有稳定吞吐优势。因此去锁首先是ownership正确性和简化，不应预先宣称性能收益。

## 17. 风险与待决问题

1. async rebuild worker当前是第二publisher，也是vector lock不能删除的首要原因。
2. prepared/repl/TTL/command分队列按类别优先级消费，没有统一global admission order；需冻结“owner实际commit order”定义。
3. owner lock stats mutex仍在每次wrapper调用上，可能掩盖weak-lock的真实成本。
4. AOF append可持mutex、直接file I/O/fsync或触发rewrite，可能让所有命令停顿。
   现接口返回`void`，写入错误不能反馈给command success边界；该问题需单独冻结，但本阶段不得修改AOF格式。
5. FULLDUMP同步遍历/response assembly会长时间占用owner。
6. pool allocator锁由embedding/rebuild/network等多线程共享，不能因store单writer删除。
7. shutdown中distributed停止晚于executor stop；必须证明receiver看到shutdown后不会late submit。
8. TCP transient queue的单producer实现与现有多producer调用面冲突；未来mutation handoff不得复用该错误契约。
9. 当前Route C历史tests已删除；未来每次删锁必须建立新的current-tree executable gate。

## 18. 最终 flags

明确回答：

```text
当前目标是否应定义为“串行命令执行下的主路径去锁”：yes
是否要求 replication 内部无锁：no
是否要求 HNSW 内部完全 lock-free：no
是否应继续 Replication Stage 1A：yes
是否应在 Stage 1A 前冻结唯一 command owner：yes
是否应现在全面删除现有锁：no
是否应先禁止第二数据库 writer：yes
所有数据库命令是否应进入统一执行域：yes
VSEARCH 当前是否应与写命令串行：yes
slave apply 是否必须进入统一执行域：yes
replication thread 是否可以直接修改数据库：no
后台 rebuild 是否只能构建私有 shadow：yes
active/shadow publish 是否应由 command owner 完成：yes
低频 replication/control path 是否允许使用锁：yes
主路径 correctness lock 是否只能在所有权证明后删除：yes
```

```text
route_c_reaudited=yes
main_path_lock_elimination_goal_correctly_defined=yes
current_command_execution_topology_mapped=yes
database_single_owner_verified=no
second_writer_paths_identified=yes
main_path_lock_inventory_completed=yes
ordinary_kv_serial_execution_verified=yes
vector_serial_execution_verified=no
embedding_prepare_commit_order_evaluated=yes
background_worker_isolation_evaluated=yes
replication_database_writer_risk_evaluated=yes
slave_apply_owner_model_proposed=yes
stage1a_should_continue=yes
command_owner_should_be_frozen_before_stage1a=yes
locks_should_be_removed_immediately=no
ownership_first_then_replication_recommended=yes
main_path_lock_elimination_roadmap_actionable=yes
ready_for_replication_stage1a_implementation=yes
```

`database_single_owner_verified=no` 和 `vector_serial_execution_verified=no` 是对当前async rebuild publisher的如实判定，不否定单executor基础，也不阻塞Stage 1A；Stage 1A实现前只需冻结owner契约，不要求先完成所有去锁代码。

本次未运行build或功能测试，因为任务只允许源码/历史只读审计和文档更新。收尾只执行文档范围、空白、`git diff --check` 和`git status --short`检查。
