# KVLite 秋招简历源码事实审计

> 审计日期：2026-08-18
> 审计对象：当前工作区源码、可执行测试、现存原始结果与报告、构建系统、Git 历史
> 审计原则：当前源码 > 当前可执行测试 > 原始结果/报告 > 构建系统 > 设计文档 > README > 注释 > Commit Message

## 0. 执行摘要

KVLite 最有简历价值的不是“功能多”，而是以下四条真实工程主线：

1. 三种网络后端最终收敛到单一 Command Executor，并通过 response task、eventfd 通知和 session 身份校验把网络所有权与数据库提交顺序分离。
2. Vector Manager 将 record store 作为事实源，自研 Flat 精确索引与 HNSW v2 派生索引；HNSW 候选必须回 Manager 做 record_id、槽位复用、状态、scope 和 threshold 校验。
3. HNSW 图快照被明确设计为可删除缓存，缺失或损坏可从 records 重建；后台重建实现 active/shadow/delta replay/validation/publish/retire 生命周期。
4. TTL、RDB、AOF rewrite、复制 apply 等后台或跨线程工作逐步收敛到 executor/command-owner 边界，形成了可讲述的所有权重构过程。

但当前源码也存在不能回避的 correctness 边界：

- HNSW 已填充索引的扩容 reserve 会重置图，VSET/VDEL 尚未达到 lossless prepare → no-fail commit。
- embedding prepare 失败或 prepared task 回投 executor 失败时，任务可能无人释放且连接永久保持 single_inflight。
- AOF 普通 append/rewrite 增量路径和 RDB load 不是端到端 binary-safe；EVERYSEC 每秒只 fwrite/fflush，不调用 fsync。
- 复制没有 offset、ACK、backlog、partial resync、ready gate，也不支持 vector；复制队列的单 producer 假设被 full-sync 线程打破，slave 生命周期还有 join、close/fd reuse 风险。
- 当前若干历史测试已与配置或实现漂移：HNSW product integration、TTL/RDB/AOF/TCP replication 门禁在本次执行中不是绿色。

因此，KVLite 足以作为优秀秋招系统项目，但不应包装成生产级数据库或高可用复制系统。建议停止横向增加 Feature，下一阶段优先修复 task ownership、HNSW 无损扩容/失败原子性、binary persistence 和 replication lifecycle。

---

# 1. 审计范围、证据边界与工作区基线

## 1.1 本次读取范围

- 根目录、Makefile、include、src、tests、scripts、config、results、docs。
- 启动/配置、网络、RESP、session、executor、四种 KV 结构、TTL、AOF/AOF rewrite、RDB、TCP replication、eBPF、Vector/Embedding/Flat/HNSW、snapshot/rebuild。
- Git log、关键提交的演进关系和现有报告。

仓库使用 Makefile；ONNX、FAISS、hnswlib 是可选构建能力。FAISS/hnswlib 在当前产品边界中仅用于 benchmark/reference，不是产品默认后端。默认向量后端为 core_hnsw_v2，core_flat 是精确 oracle 与显式 fallback。

## 1.2 审计前工作区

审计开始时已有：

~~~text
?? config/kvstore-ai-chat-vector.conf
~~~

该文件属于用户既有未跟踪工作，本次未读取后修改、未删除、未暂存。测试曾产生根目录 vector_index.manifest 和 vector_index.snapshot，确认是本次测试产物后已删除；没有改动源码、测试、配置、README、Makefile 或已有文档。

## 1.3 证据限制

- results/raw.csv 与 results/vector_rw_raw.csv 是仓库内原始结果，可信度高于二次汇总文档，但缺少完整机器与服务启动参数，量化等级定为 B。
- T2Ranking 10K/100K 报告记录了原始 artifact 的外部绝对路径；仓库中保留的是 report，不是全部原始 artifact，因此定为 B。
- eBPF 的主要数字来自历史 README/commit 文本，当前仓库没有对应原始输出，定为 C，不能作为核心量化成果。
- “有测试脚本”不等于“本次测试通过”。本报告单独标记本次实际执行结果。

---

# 2. 完整项目地图

| 层次 | 主要目录/文件 | 当前职责 |
| --- | --- | --- |
| 入口与生命周期 | src/core/kvstore.c | 配置合并、恢复、组件启动、网络选择、shutdown |
| 配置 | src/config、include/kvs_config.h | descriptor/schema 合并、来源追踪、能力校验 |
| 网络 | src/network/reactor.c、proactor.c、ntyco.c、net_session.c | epoll、io_uring、协程网络 owner；收发与 session 生命周期 |
| 协议 | RESP parser/command task 相关源码 | 显式长度 argv 解析、命令任务构造 |
| 执行 | src/core/kvs_command_executor.c | 单一数据库可见提交顺序、多任务优先队列、response handoff |
| 后台 embedding | src/core/kvs_embedding_worker_pool.c | task-private embedding prepare，再回投 executor |
| 普通 KV | Array、Hash、RBTree、SkipTable 源码 | 四种后端与 TTL 字段 |
| TTL | src/core/kvs_ttl_background.c、executor TTL handler | lazy/active expiration、确定性复制命令 |
| AOF | src/persistence/kvs_aof.c、kvs_aof_rewrite.c | append、flush、replay、rewrite |
| RDB | src/persistence/kvs_rdb.c | KV/vector record 快照、加载恢复 |
| TCP replication | src/distributed、src/core/kvs_replication_fallback.c | 普通 KV/TTL full-sync 与实时命令传输 |
| Vector Manager | src/vector/kvs_vector.c、kvs_vector_index.c | record store、ID/map、命令语义、结果 resolve |
| Vector Core | src/vector/core/flat、src/vector/core/hnsw_v2 | Flat exact 与 HNSW 图状态 |
| Snapshot/Rebuild | src/vector/kvs_vector_index_snapshot.c、kvs_vector_rebuild*.c | 图缓存校验、shadow rebuild/publish |
| Embedding | src/embedding、src/vector/kvs_vector_embedding.c | hash/ONNX provider、归一化与模型兼容 |
| eBPF | src/eBPF | 可选 observe/experimental mirror，不是可靠复制主路径 |
| 测试/压测 | tests、scripts/benchmark、results | shell E2E、C gate、benchmark harness 与原始 CSV |

---

# 3. 启动、恢复与 shutdown

## 3.1 实际启动顺序

src/core/kvstore.c 中的实际主链路可还原为：

~~~text
解析 CLI / 主配置文件
→ 合并 built-in < file < explicit CLI
→ validate effective kvs_config_t
→ 日志、角色和信号处理
→ 内存后端与 g_kv_data_rwlock
→ Array / RBTree / Hash / SkipTable
→ 可选 Vector Manager/Core
→ RDB load
→ AOF init + AOF replay
→ vector rebuild/load-after-records
→ distributed/replication init
→ Command Executor start
→ TTL background start
→ Reactor / Proactor / NtyCo 之一
~~~

关键事实：

- RDB/AOF 恢复发生在运行期 executor 与网络启动之前，因此属于显式 startup owner。
- RDB 与 AOF 保存的是 vector records，不保存 HNSW 图；records 恢复后再加载合法 snapshot 或重建图。
- RDB load 和 AOF load 的返回值在 main 中没有形成强制启动失败边界；损坏或部分恢复不能等同于 fail-stop recovery。
- Vector rebuild 失败会影响启动，而普通 RDB/AOF load 错误处理更宽松，三者错误策略不一致。

## 3.2 正常与异常网络退出

网络主循环返回后：

~~~text
停止 TTL background
→ 正常路径：AOF force fsync + RDB save
→ executor stop/drain
→ shutdown owner
→ distributed shutdown
→ AOF free
→ 持写锁销毁普通 KV 与 Vector
→ memory/log cleanup
~~~

源码把“网络运行正常返回”和“网络初始化/运行失败”区分开：失败路径不主动做最终 RDB save。需要注意，aof_free 内部仍可能 force flush，所以不能简单宣称“异常退出完全不落 AOF”。

当前边界：

- graceful shutdown 的组件顺序已经经过收敛，并有历史 ASan UAF 修复案例。
- 启动中段失败仍有 cleanup 路径不完全对称的工程债。
- 主进程 shutdown 对阻塞中的 slave receiver/join 缺少可靠唤醒，复制开启时可能挂住。

---

# 4. 网络模型与真实异步闭环

## 4.1 公共主链路

Reactor 与 Proactor 的完整链路是：

~~~text
Client socket
→ network owner 接收
→ net_session recv buffer
→ RESP parser
→ deep-copy argv 的 command task
→ Command Executor
→ command handler / post_commit
→ response task
→ async response queue
→ eventfd 仅通知 owner
→ network owner drain response queue
→ session identity/closing 校验
→ send buffer
→ partial send / completion
→ socket
~~~

NtyCo 也提交到同一 executor，但 coroutine 以约 1ms 轮询 owner response，而不是 Reactor/Proactor 的 eventfd wakeup/ref-transfer 路径。

## 4.2 Reactor

- 使用 epoll ET。
- accept 和 recv 均循环到 EAGAIN；不是“一次 EPOLLIN 只读一次”。
- send 维护 buffer offset；短写或 EAGAIN 后打开 EPOLLOUT，后续继续发送。
- 一个 session 同一时刻只允许一个 command inflight。已经读入 recv buffer 的 pipeline 命令会在上一响应闭环后继续解析，保证单连接顺序。
- recv 对 EINTR 的处理偏严格，可能把可重试中断当作连接失败，属于 P1 健壮性问题。

## 4.3 Proactor

- io_uring 接收/发送完成事件仍由网络 owner 处理。
- send completion 根据 offset 继续提交剩余区间，能够处理 partial completion。
- executor 不直接提交 socket send；response 仍通过 async adapter 回到 Proactor owner。

## 4.4 NtyCo

- coroutine 自己持有连接并等待 executor 结果。
- 仍共享 Command Executor，因此数据库提交顺序不是另一套实现。
- 它没有完全复用 Reactor/Proactor 的 eventfd 与 session ref-transfer 模型，生命周期证明更依赖 coroutine 不在响应到达前释放上下文。

## 4.5 session 字段解决的问题

| 字段 | 源码作用 |
| --- | --- |
| session_id | 区分逻辑连接，不只依赖可能复用的 fd |
| generation | 同一 session 槽/对象代际校验，拒绝旧 response |
| refcount | command/response 跨线程期间保持 session 存活 |
| closing | close 开始后禁止新 response 落入发送路径 |
| owner_poller_id | 确保 response 回到拥有该连接的网络 owner |
| single_inflight | 单连接同一时刻只提交一个命令，保序并限制生命周期复杂度 |
| recv/send offset | 支持 pipeline 缓冲和 partial write |

eventfd 中承载的是“有 response 可取”的计数通知，不是 response payload。真正 payload 在受保护的 response queue 中。

网络线程与 executor 的边界是本项目最成熟的所有权设计之一：executor 不调用 send、close、epoll_ctl 或 io_uring socket 操作；网络 owner 不直接修改数据库。

---

# 5. Command Executor 源码事实审计

## 5.1 为什么引入 Executor

它实际解决的不是笼统“异步”，而是四个具体问题：

1. 将 Reactor、Proactor、NtyCo 的协议接入统一到同一数据库执行路径。
2. 给数据库可见 mutation、record_id 分配、复制 apply 和 TTL 删除建立单一顺序。
3. 让网络连接只能由 network owner 操作，跨线程只传 immutable command/response work。
4. 把 RDB snapshot、AOF rewrite snapshot、full-sync snapshot、embedding prepared commit 等跨线程工作拉回一致的锁与提交边界。

普通命令执行仍使用 g_kv_data_rwlock；executor 的存在不等于“主路径锁已完全消除”。异步 HNSW rebuild worker 目前还是受保护的第二 publisher。

## 5.2 真实任务类型与严格优先级

按 dequeue 逻辑，优先级是：

~~~text
1. prepared embedding commit
2. replicated apply
3. TTL expire candidate
4. TTL scan
5. normal command
6. full-sync snapshot
7. RDB snapshot
8. AOF rewrite snapshot
~~~

队列由 mutex 保护，调用模型能够容纳多 producer，不是 lock-free queue。严格优先级没有 aging/fairness；持续 prepared/replication/TTL 任务可以推迟普通命令或 snapshot，属于 tail latency 与可观测性风险。

## 5.3 普通网络任务生命周期

~~~text
network owner 解析 RESP
→ 按 argv_len 深拷贝 command/argv
→ 标记 single_inflight，并为跨线程引用 retain session
→ submit executor
→ executor 持 read/write lock 调 handler
→ post_commit 做 AOF/replication 派生工作
→ 构造 response_task，转移 response buffer 与 session metadata
→ 销毁 command task/argv
→ response queue + eventfd
→ network owner 校验 fd/session_id/generation/owner/closing
→ 拷入 send buffer并释放 response_task
→ response 完成后 release session、清 single_inflight
~~~

submit 失败路径会撤销 inflight/ref 并释放 task/response，这一段 ownership 是闭合的。argv 使用 pointer + explicit length 深拷贝，避免 parser/recv buffer 被复用后悬空。

## 5.4 Embedding prepared task 生命周期

~~~text
executor 收到需要 embedding 的 VSET/VSEARCH
→ 转交 embedding worker queue
→ worker 在 task-private buffer 生成 embedding
→ 构造 prepared state
→ submit prepared queue
→ executor 重新验证并 commit / search
→ 统一 response handoff
~~~

Embedding worker 只准备私有数据，不分配 record_id，不修改 Manager/Core，不产生复制序号，符合目标 owner 模型。

但存在高风险失败路径：worker prepare 失败，以及 submit_prepared 失败时，当前 worker 主循环只记统计，没有完成 response、释放 task/session ref 或清 single_inflight。结果可能是任务泄漏与连接永久等待。历史存在名为异常路径的测试/提交，不能替代对当前控制流的判断。

## 5.5 Backpressure

- embedding_worker_queue_capacity 被读取并输出，但 submit 没有按它拒绝入队。
- 每次统计 queue depth 通过链表遍历，入队成本可能从 O(1) 退化。
- executor task queue、async response queue 也没有明确总量上限。
- single_inflight 只限制单连接，不能限制多连接聚合压力。

结论：可以写“统一 executor 与 ownership handoff”，不能写“实现了完善的有界任务调度/backpressure”。

---

# 6. 普通 KV 数据结构

## 6.1 四种结构的真实定位

- Array：稠密元素数组，同时有辅助 hash 索引。
- Hash：开放寻址。
- RBTree：有序树。
- SkipTable：跳表有序结构。

默认 SET/GET 等命令路由到配置选择的主结构；R/H/S 前缀命令显式进入其他结构。四种结构主要提供算法实现、统一协议接入和对照 benchmark，不应作为简历第一亮点。

## 6.2 TTL、二进制与线程安全

- TTL timestamp 保存在各后端 record/node 中，而不是独立 timing wheel。
- live command 路径多数使用 key/value pointer + explicit length；SET/GET/MOD/DEL/EXIST 能处理嵌入 NUL。
- test_binary_key.c 覆盖四种结构的嵌入 NUL 基本命令，但 GET 只校验 bulk length，没有逐字节比较返回内容，也没有经过 AOF/RDB 重启。
- executor 对读命令加 g_kv_data_rwlock 读锁，对写命令加写锁。部分 RANGE/SORT 因实现会修改内部状态而走写锁。
- lazy expiration 在读锁下只观察，不删除；由 executor 释放读锁后以写锁重试整个命令。

因此：“协议和 live KV API 使用显式长度”可以成立；“KVLite 端到端 binary-safe”不成立。

## 6.3 仓库内普通 KV Benchmark

results/raw.csv 的同一表中记录了 KVLite 和 Redis，均为 50 clients、pipeline=1、100K requests、32-byte value、keyspace 100K：

| Backend | Op | KVLite QPS | KVLite P95 ms | KVLite P99 ms | Redis QPS | Redis P95 ms | Redis P99 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Array | SET | 115,340 | 0.447 | 0.775 | 105,708 | 0.455 | 0.615 |
| Array | GET | 108,696 | 0.559 | 0.807 | 94,877 | 0.527 | 0.855 |
| Hash | SET | 107,296 | 0.487 | 0.783 | 117,096 | 0.439 | 0.631 |
| Hash | GET | 115,340 | 0.415 | 0.583 | 121,507 | 0.343 | 0.543 |
| RBTree | SET | 114,548 | 0.583 | 0.807 | 108,225 | 0.447 | 0.679 |
| RBTree | GET | 115,473 | 0.567 | 0.767 | 114,025 | 0.407 | 0.567 |
| SkipTable | SET | 102,775 | 0.711 | 1.103 | 123,001 | 0.271 | 0.511 |
| SkipTable | GET | 105,820 | 0.671 | 0.983 | 117,371 | 0.303 | 0.591 |

这组数字不能概括为“全面超过 Redis”：不同结构/操作各有输赢。脚本没有封装并记录两端完整服务器启动参数与机器信息，不能证明 AOF、replication 等开关完全对齐，量化等级 B。

---

# 7. TTL 三条路径

## 7.1 TTL 命令与确定性派生

EXPIRE/PEXPIRE 等相对时间在提交后被规范化为绝对 PEXPIREAT，再交给 AOF/replication；已经消失或需要删除时派生 DEL。PERSIST 只在实际清除 TTL 后产生持久化/复制 mutation。

价值在于 replica/replay 不重新从各自时钟计算相对 TTL，避免把传输与恢复耗时累加为新的生命周期。

## 7.2 Lazy expiration

~~~text
read command + read lock
→ 查到 record 已过期
→ 返回 retry-with-write-lock
→ executor 释放读锁
→ write lock 下重跑命令
→ 二次确认后删除
→ post_commit 派生 DEL
~~~

不能直接在读锁内删除，因为那会同时破坏 rwlock 语义和各数据结构的并发读假设；也不能做隐式锁升级，否则容易死锁或破坏执行顺序。

Slave 的 lazy path 可以把过期 key 当作不存在，但不会主动删除；删除由 master 的确定性 mutation 驱动。

## 7.3 Active expiration

~~~text
master-only TTL background thread
→ submit TTL scan task
→ executor write lock 扫描
→ 最多收集 32 个 candidate，并受 budget 控制
→ candidate 携带 observed_expire_at
→ TTL expire task 二次核对当前 expire_at
→ 相等且已过期才删除
~~~

observed_expire_at 防止扫描后用户延长 TTL，而旧 candidate 误删新值。当前扫描没有成熟 cursor，倾向从结构前部重新扫描；TTL scan/expire 又高于 normal command，过期风暴可能影响尾延迟。

当前 TTL shell tests 存在，但本次在新临时目录下因未创建配置要求的 vector data 路径而在业务断言前失败；TCP replication TTL 测试的 master probe 也未通过。因此这里只把控制流视为源码事实，不把现有脚本存在当作当前绿色 E2E。

---

# 8. AOF 与 AOF Rewrite

## 8.1 Append 与三种策略的真实语义

~~~text
write commit
→ post_commit
→ RESP serialization
→ AOF user-space buffer
→ background/foreground flush
~~~

| 策略 | 当前源码语义 |
| --- | --- |
| always | append 后 fwrite + fflush + fsync |
| everysec | 后台约每秒调用 flush(false)：fwrite + fflush，不调用系统 fsync |
| no | 缓冲区达到阈值或 shutdown 时才 flush；shutdown 会 force fsync |

因此配置名 EVERYSEC 不能描述成“每秒 fsync”。fflush 只把 stdio buffer 推到内核页缓存，不等价于介质持久化。

数据库 commit 后，post_commit 没有把 aof_append 失败回滚为命令失败；这符合“先 commit、后派生”的结构，但当前缺少进入只读/故障状态等一致性策略。

## 8.2 Binary safety

普通 append 虽然用 arg_lens 生成 RESP length header，body 却经过 %s/snprintf；嵌入 NUL 会导致 body 截断、header/body 不一致。大命令直接 fwrite 的分支更安全，但不能挽救普通命令。

rewrite：

- base snapshot 的普通 KV key/value 多数使用显式长度；
- TTL helper 和 rewrite 期间的增量 buffer 仍有 strlen/%s；
- vector AOF 记录 public text VSET/VDEL，恢复时重新做 embedding，而不是持久化已提交 float32 embedding。

当前没有覆盖“binary write → AOF → shutdown → restart → binary GET 且逐字节比较”的测试。故端到端 binary AOF 属于 P0 correctness 风险。

另一个风险是普通 append 对剩余 buffer capacity 的证明不足：snprintf 返回“本应写入长度”，若发生截断仍可能按该值增加 buf_len。

## 8.3 Replay

AOF parser 遇到格式错误时会尝试跳字节/继续，残留不完整数据可能被丢弃；main 又不强制检查 aof_load 失败。这不是经过 offset/checksum 边界保护的 crash recovery，不能宣传“可靠崩溃恢复”。

## 8.4 Reactor/NtyCo 与 Proactor Rewrite

共同步骤：

~~~text
executor 中申请 rewrite snapshot
→ 持 read lock 深拷贝内存快照
→ 解锁
→ 临时文件写 snapshot
→ fflush/fsync
→ old → backup，temp → active rename
→ reopen AOF
~~~

差异：

- Reactor/NtyCo 路径 fork child 写文件，parent 等待 child。
- Proactor 路径创建 rewrite thread，调用者等待 thread。

关键边界：rewrite handler 本身在单 executor 上等待 child/thread 完成，所以命令提交在整个 rewrite 期间被阻塞。当前并不是 Redis 式“fork 后父进程继续接受写入并用 rewrite buffer 追平”。源码虽然存在 rewrite increment buffer，但在当前串行等待结构下无法证明它承担了真正并发写追平。

snapshot 已在 fork 前深拷贝，磁盘 IO 在全局数据锁外；因此 COW 不是这里主要的共享内存一致性机制。文件切换有 fsync + rename，但没有明确 parent directory fsync，不能宣传完整断电原子性。

---

# 9. RDB

## 9.1 Save 链路

~~~text
request RDB save
→ executor snapshot task
→ g_kv_data_rwlock read lock
→ 深拷贝四种 KV + TTL + vector records
→ unlock
→ 临时文件写入
→ fflush + fsync
→ rename
~~~

锁只保护内存 snapshot 构建，磁盘 IO 在锁外，这一设计值得面试讲。RDB 包含 vector record 与已提交 embedding，不包含 HNSW 图；load 后图由合法 graph snapshot 加速或从 records rebuild。

Replication full snapshot 是另一种内存结构与传输协议，只包含普通 KV/TTL，不是 RDB 文件，也不含 vector，二者不能混称。

## 9.2 Load 与恢复边界

- TTL 使用保存的绝对 timestamp，恢复后按当前时间判断。
- 二进制数据虽然在文件中带长度，load 应用时部分调用 legacy C-string setter，嵌入 NUL 会截断。
- Array load 路径释放临时 key/value 后仍把 key 传给额外 expire helper，存在 use-after-free/read-after-free 风险。
- load 在完整验证前会逐步替换/填充内存结构；格式/IO 错误可能留下部分状态。
- 格式依赖 native endian/time_t，缺少全文件 checksum/versioned compatibility 的生产级边界。
- temp fsync + rename 后也没有明确 directory fsync。

因此可以写“实现 executor snapshot + 锁外 RDB IO + vector record recovery”，不能写“二进制安全、跨平台、崩溃一致的 RDB”。

---

# 10. TCP Replication

## 10.1 实际主路径

~~~text
master command-owner commit 普通 KV/TTL
→ post_commit 确定性编码
→ global TCP queue
→ replication worker
→ 遍历 slave 发送

slave receiver
→ decode explicit-length mutation
→ submit replicated task
→ executor apply
→ 成功后本地 AOF
~~~

复制 apply 回到 executor，而不是网络线程直接改数据，这是正确的 owner 边界。普通 KV mutation 使用显式长度，TTL 使用 PEXPIREAT/DEL 的确定性语义。

## 10.2 Full sync

新 slave 线程通过 executor 申请普通 KV immutable snapshot，然后调用 full-sync 发送。但当前实现有三项关键限制：

1. 新 slave 在 full snapshot 完成前已加入全局 slave 数组。
2. full-sync helper 最终通过全局 broadcast 路径，不是只写目标 slave fd。
3. 没有 snapshot offset/barrier，full snapshot 与实时 incremental mutation 可以交错。

所以“支持有限 full-sync 语义”准确；“一致全量同步 + 无缝增量追平”不准确。

## 10.3 当前支持矩阵

| 能力 | 当前状态 |
| --- | --- |
| 普通 KV 实时复制 | 已实现基础路径 |
| TTL 确定性复制 | 已实现基础路径 |
| 普通 KV full snapshot | 有限实现，缺 offset barrier/target isolation |
| Vector VSET/VDEL | 未实现；编码表无确定性 vector mutation |
| Replica ACK | 未实现 |
| 全局 replication offset | 未实现 |
| durable/bounded backlog | 未实现 |
| partial resync | 未实现 |
| disconnect/reconnect | slave 仅启动后单次连接，无可靠重连协议 |
| promotion recovery | 未实现 |
| READY/VSEARCH gate | 未实现 |
| gap/checksum handling | 未实现 |

现有瞬时 send queue 不是 backlog。

## 10.4 Replication Queue 并发模型

queue push 使用单 producer 的 tail 发布模型，pop 使用 CAS 容纳 consumer；它本质上是 SPMC 假设，不是 MPSC/MPMC。

实际 producers 至少包括：

- 单 Command Executor 的 post_commit；
- 每个 slave full-sync 线程通过同一 master_sync/broadcast queue 发 snapshot。

调用侧存在多 producer，违反 queue 实现假设。结论必须写为：

> 当前实现存在并发正确性风险，不应在简历中包装成生产级无锁复制队列。

## 10.5 Slave 生命周期风险

- heap slave 对象先复制进全局数组，之后 pthread_create 才把 thread id 写回 heap 对象；数组中的 thread id 可能未初始化，shutdown join 目标不可靠。
- slave thread close fd 后再按 fd remove；broadcaster 在锁内复制 fd、解锁后 send，close/remove/fd reuse 之间存在 stale fd race。
- shutdown join 前未稳定 shutdown 所有阻塞 recv fd，可能永久等待。
- 一个 worker 对多个 slave 使用 blocking send，慢 slave 可阻塞其他 slave。
- Replica 没有 READY 状态，full sync 中也可能服务读/VSEARCH。

本次 TTL TCP replication E2E 未通过 master probe，因此不能把当前复制称为已完成、当前测试绿色的模块。

---

# 11. Vector / Embedding / HNSW

## 11.1 三层事实边界

| 层 | 地位 | 内容 |
| --- | --- | --- |
| Vector Record Store / Manager | source of truth | record_id、question/answer/scope/TTL/metadata、ACTIVE/deleted、slot/map |
| core_flat | derived exact index | record_id、vector、精确 score；正确性 oracle/fallback |
| core_hnsw_v2 | derived ANN index | internal slot/id map、deleted flag、graph/search state |

HNSW graph 与 snapshot 都不是数据真相；删除它们后应能由 records 恢复。

## 11.2 VSET

~~~text
RESP request
→ network command task
→ executor 识别需要 embedding
→ embedding worker 生成 task-private embedding
→ prepared task 回投 executor
→ executor 重新验证 key/state
→ Manager 分配 record_id/record slot
→ Flat/HNSW derived add
→ post_commit AOF
→ response task 回 network owner
~~~

worker 不持 Manager/Core 写权。HNSW 同 record 更新采用“旧 record_id tombstone + 新 record_id/vector append/re-add”，不是在原 internal slot 上无损重连。

## 11.3 VSEARCH

~~~text
query embedding
→ active Flat/HNSW search
→ HNSW overfetch candidate record_id + score
→ Manager record_id_map resolve
→ slot range
→ record exists
→ record->id == candidate.record_id
→ ACTIVE && !deleted
→ scope + threshold
→ TopK response
~~~

record->id 二次校验是关键：Manager slot 和 HNSW internal slot 可按不同生命周期复用；旧图命中不能解析成新 record。候选回表验证是非常适合源码追问的差异化亮点。

## 11.4 VDEL

Manager 先改变 record 状态/映射/空闲槽，Core 标记 index-local tombstone，图边不做物理全清理；rebuild 才是压缩恢复路径。VDEL 目前仍有多个可能失败的 map/core/delta 步骤，尚未形成严格 no-fail commit。

Vector VSET/VDEL 没有 TCP replication codec；当前 post_commit 即使尝试普通 command 名，也无法复制稳定 record_id 与 committed float32 embedding。不能写“向量主从复制”。

## 11.5 Embedding Worker Pool

- worker 数量可配置，每个 worker 可持有独立 runtime。
- 支持 task-private hash/ONNX embedding prepare。
- queue_capacity 目前只进入配置/日志，没有 enforce，不能称为有界队列/backpressure。
- submit 深度统计遍历链表，深队列时成本增长。
- prepare failure 与 submit_prepared failure 的 ownership 缺口是 P0。
- Vector AOF replay 重新执行 public VSET embedding；模型版本、归一化、维度变化会改变恢复结果，虽然统一配置试图形成兼容边界，但 AOF 格式没有持久化 committed embedding。

---

# 12. HNSW v2 深入审计

## 12.1 图构建与搜索

- level：按与 M 相关的几何分布随机生成层级。
- 插入：从 entry point 的高层 greedy descent，目标层用 efConstruction best-first search 收集候选。
- neighbor selection：带 diversity/距离启发式选择，不只是取最近 M 个。
- 连接：新节点与邻居双向连边；超过上限时 reverse prune。
- M/M0：上层使用 M，level 0 使用更大的 M0。
- efSearch：查询时从高层 greedy 到 level 0，再做 bounded best-first expansion。
- tombstone：deleted 节点不进入最终结果，但边仍可参与图连通；物理清理由 rebuild 完成。

当前默认参数由统一配置提供，现行测试期望 core_hnsw_v2 默认，典型默认 M=48、M0=96、efConstruction=512、efSearch=1024。

## 12.2 Flat oracle 与 recall

core_flat 使用同一 record/vector 语义计算精确 TopK；HNSW recall gate 用 Flat truth 比较 ANN 结果。这比只测 QPS 更可信。真实 T2Ranking benchmark 也先生成 exact embedding nearest-neighbor truth；它测的是向量近邻召回，不等于 qrels 语义相关性。

## 12.3 Capacity 与 failure atomicity：当前最重要 blocker

ensure_add_capacity 会调用 core_hnsw_v2 reserve。当前 reserve 在已填充索引上分配新存储后释放旧存储并把 size/graph 重置，属于 destructive reserve，不是 allocate-copy-validate-publish。

VSET 在 Manager record/map 已变化后才可能触发 core add；HNSW add 又可能已经修改旧节点 adjacency。后续 rollback 无法可靠恢复旧图。因此：

- 不能写“动态扩容无损”。
- 不能写“VSET/VDEL 失败原子”。
- 这是 vector replication 之前必须先解决的 Stage 1A blocker。

100K product benchmark 曾通过预设足够 initial capacity 绕开该路径；那是 benchmark precondition，不是源码问题已修复。

## 12.4 Snapshot

snapshot/manifest 校验包括：

- magic/version/backend；
- dimension、metric、M/M0、efConstruction、efSearch；
- record/vector 数量、generation；
- record ID 与 vector fingerprint/checksum；
- snapshot length/checksum；
- model identity/兼容字段；
- 反序列化后的图边、slot、level 等结构有效性。

合法 snapshot 可加速启动；缺失、不兼容或损坏会回退 records-only rebuild。snapshot 写失败不回滚已成功 publish。它是 cache，不进入 RDB/AOF truth。

## 12.5 Async rebuild

~~~text
active 继续服务 search
→ worker 从 guarded record view 构建 shadow
→ VSET/VDEL commit 后记录 delta
→ replay delta 到 shadow
→ 校验 records/vector/graph/generation/search
→ publish swap active/shadow
→ old active 进入 retired
→ reader safety 后 reclaim
→ best-effort snapshot writer
~~~

失败或取消保留 old active，shadow cleanup 不应暴露。该实现已超过普通学生“重建时停服”的简单方案。

边界：

- worker 当前仍直接 publish，是受锁保护的第二 publisher，不是完整 command-owner completion。
- rebuild 与 VSET/VDEL 有保守串行区，不能宣称重建期间写延迟完全不受影响。
- reader safety 使用 rwlock，active + shadow + retired 会叠加内存。
- delta append 失败会让 rebuild 失败，但不会回滚已经成功的产品写入。
- snapshot/rebuild 测试较丰富，但没有足够 sanitizer 并发 race 证明。

## 12.6 测试实际覆盖

| 测试族 | 实际验证 |
| --- | --- |
| flat correctness/rebuild/RDB reload | exact add/search/delete/rebuild 与 record 恢复基线 |
| HNSW skeleton/graph construction | 生命周期、图规模、level、重复/overflow 基本不变量 |
| search recall | 对 Flat exact truth 的 synthetic recall；本次只观察到 1K 子阶段通过，整脚本未取得最终完成证据 |
| tombstone/policy | 删除结果过滤与 tombstone 策略 |
| snapshot manifest/roundtrip/fallback | 格式校验、序列化一致性、损坏/缺失回退 |
| shadow publish/write/VINFO | snapshot 与 active/shadow publish 状态 |
| rebuild state/delta/replay/swap/async worker | 状态机、delta generation、最终校验、异步 worker |
| product integration | Manager/Core/RDB 产品接入；本次因 reload fixture norm_sq=0.3125 被当前 normalization gate 拒绝而失败 |

测试存在不等于所有 gate 当前绿色；product integration fixture 与现行 normalization contract 已漂移，应在下一轮修正或明确迁移。

---

# 13. eBPF / Batch Replication 结论

## 13.1 当前主路径

默认配置关闭。运行模式是 disabled / observe / experimental_mirror；通过外部 daemon 和内核 hook 观察 master 收到的 TCP payload，再由用户态解析/转发。可靠 TCP replication 仍是应用层主路径。

experimental mirror 在数据库 handler 成功前就可能观察并转发请求，无法证明 failed command、权限/语义失败不会被镜像；ring buffer 丢失也没有 offset/ACK/full resync 修复。

## 13.2 Benchmark 归因

历史材料中出现约 15K–16K QPS、send_all 从 8K–13K 次降到 3–5 次等数字。对应优化主体是 tx_batcher 将小块合并为约 128KiB 发送，不是 eBPF 本身减少应用发送系统调用。当前没有保留完整环境与原始输出，不能把 QPS 提升归因给 eBPF。

## 13.3 简历建议

**三选一：删除。**

原因：它是 optional/experimental path，可靠性语义弱于主 TCP path，数字证据只有 C 级且核心收益来自 batching。若面试官深挖“失败命令、TCP 分段、ring loss、重连、顺序和 ACK”，当前实现容易被击穿。可作为复盘中的探索经历，不应占秋招简历主 bullet。

---

# 14. Benchmark 真实性表

| Benchmark | Workload | Dataset | Clients | Pipeline | QPS / 延迟 | 对照组 | 证据文件 | 等级与边界 |
| --- | --- | ---: | ---: | ---: | --- | --- | --- | --- |
| 普通 KV vs Redis | SET/GET，100K req，value=32B，keyspace=100K | 100K | 50 | 1 | 见第 6.3 节；KVLite 约 102.8K–115.5K QPS | Redis 同表约 94.9K–123.0K | results/raw.csv、scripts/benchmark/bench_all.sh | B；缺完整机器/服务开关记录，不能概括全面胜出 |
| Vector synthetic latency | VSET/VGET/VSEARCH/VDEL | 100K | 1 | 1 | VSEARCH 962.48 QPS，P95 1.49ms，P99 2.02ms | 无 | results/vector_rw_raw.csv | B；synthetic hash embedding，不是 ONNX |
| Vector synthetic throughput | 同上 | 100K | VSET 50；VGET 100；VSEARCH 50；VDEL 50 | 1/16/4/1 | VSEARCH 1,133 QPS，P95 235ms，P99 272ms；其余约 133K–165K QPS | 无 | results/vector_rw_raw.csv | B；高 pipeline，尾延迟高，非 SLA |
| T2Ranking 10K index-only | 512d normalized IP，200 query，Top10，单线程 | 10K | 1 | N/A | core ef64 recall@10=.999，avg 222.952us，P95 304us；build 37.56s | FAISS HNSW ef64 recall=.996，avg157.255us，build6.459s | docs/vector_benchmark_readiness/b_scale_3_t2ranking_10k_hnsw_faiss_benchmark_report.md | B；原 artifact 在仓库外；是 exact-NN recall，不是 semantic qrels |
| T2Ranking 100K index-only | 512d normalized IP，200 query，Top10，单线程 | 100K | 1 | N/A | core build 678.326s；ef64 recall=.991，avg650.426us，P95 967us，P99 1156us；ef128 recall=.9985 | FAISS build215.897s | docs/vector_benchmark_readiness/b_scale_6_t2ranking_100k_hnsw_faiss_index_only_report.md | B；单 seed/单线程；peak RSS 是进程口径 |
| T2Ranking 100K product prepared | 已准备 embedding 的 VSEARCH 产品路径 | 100K | 1 | 1 | ef128 recall=.9985，avg1.373ms，P95 1.933ms，P99 2.148ms，约728 QPS | exact truth | docs/vector_benchmark_readiness/b_scale_7_t2ranking_100k_product_e2e_report.md | B；AOF disabled、无 replica、单客户端 |
| T2Ranking 100K public text | ONNX embedding + product VSEARCH | 100K | 1 | 1 | ef128 avg6.018ms，P95 7.473ms，P99 8.496ms，约166 QPS | prepared path | 同上 | B；明确包含 embedding，但不是并发 SLA |
| 100K snapshot startup | synthetic graph snapshot load，5 次 | 100K | 1 | N/A | avg5.923s、P50 6.050s、P95 6.804s；历史 records rebuild 3011.8s，约508x | 复用的旧 rebuild baseline | docs/hnsw_index_snapshot/hnsw_s_final_consolidation_report.md | B；baseline 非同轮重跑，synthetic，不宜作为核心数字 |
| eBPF/batch | replication tx batching | 小规模历史压测 | 不完整 | batch | 约15K–16K QPS；send_all 8K–13K→3–5 | unbatched | 历史 README/commit | C；无当前 artifact，收益主要来自 batch |

量化使用建议：

- 优先使用 B 级 T2Ranking 100K prepared/text 的 recall + latency，且必须写“单客户端、AOF 关闭、无复制”。
- 普通 KV 与 Redis 只适合说“完成同 workload 对照，结果互有胜负”，不适合写“超过 Redis X%”。
- snapshot 508x 可以作为面试补充实验，不建议放主简历数字。
- eBPF 数字不使用。

---

# 15. 测试体系审计

| 分类 | 现有证据 | 覆盖强度 | 主要缺口 |
| --- | --- | --- | --- |
| Protocol | RESP、pipeline、binary key 基本测试 | E2E/smoke | binary 返回内容逐字节、malformed/fuzz、超大 pipeline |
| Storage | 四结构功能与 benchmark | E2E + benchmark | failure injection、内存分配失败、一致 differential test |
| TTL | all-backends、AOF、RDB、TCP replication 脚本 | E2E 脚本存在 | 本次临时 data_dir 漂移导致失败；过期风暴/公平性 |
| Persistence | RDB/AOF/rewrite/vector recovery | E2E + C tests | binary restart、corrupt/truncated recovery、dir fsync、load atomicity |
| Replication | ordinary KV/TTL TCP scripts | E2E 脚本存在 | MPSC race、fd reuse、blocking shutdown、offset/gap、vector |
| Network | Reactor/Proactor/NtyCo 集成与历史 Route C 测试 | 集成/历史 | EINTR、fd reuse stress、response queue overload、sanitizer race |
| Vector | command/persistence/manager resolve | E2E + unit | current product integration fixture 漂移、failure atomicity |
| HNSW | graph/recall/tombstone/snapshot/rebuild | 较强 unit/integration | growth fault injection、长期并发 rebuild/write/search |
| Embedding | hash/ONNX/worker tests | unit/smoke | prepare failure 与回投失败的 response/owner 闭环 |
| Config | stage1–4、runtime、vector-disabled | 强 schema/runtime gates | 并行脚本会互相 make clean；build-mode stale object |
| Shutdown | 历史 ASan UAF 修复与脚本清理报告 | 历史证据 | 当前 replication blocking shutdown、全模式 sanitizer |
| Failure path | snapshot corrupt fallback、rebuild cancel/fail | Vector 较强 | AOF/RDB corruption、allocation failure、queue saturation |
| Concurrency | executor、async rebuild 相关测试 | 部分 | TSan/ASan 主程序 race、replication producer/lifetime |
| Benchmark | KV、synthetic vector、real embedding/index/product | 多层次 | 统一 artifact、机器元数据、高并发 SLA、重复 seed |

## 15.1 本次实际执行

本次运行并记录：

- make clean；make -j4：通过。此前直接增量 make 因残留 embedding_runtime_onnx.o 被普通链接纳入而失败，说明切换构建模式对 stale object 不稳健。
- 通过：Route C command-owner contract；Flat correctness/rebuild/RDB reload；HNSW skeleton/graph construction；snapshot manifest/roundtrip/load fallback/write/VINFO/shadow publish；rebuild state/delta/replay/swap/async worker；统一配置 stage1、stage2、stage3、stage3 runtime、vector-disabled、stage4（有并行干扰的脚本均隔离重跑）。
- HNSW graph 当前样例：size=1000、max_level=2、duplicate=0、overflow=0。
- HNSW recall：观察到 1K 子阶段 recall=1.0、build≈8.64s；未获得整个多规模脚本最终退出状态，故不宣称本次完整通过。
- 失败：vector_core_hnsw_v2_product_integration_test.sh。reload fixture 的 norm_sq=0.3125 被当前 normalization validation 拒绝。
- 失败：test_ttl_all_backends.sh、test_ttl_aof.sh、test_ttl_rdb.sh，在业务断言前因临时配置要求的 vector data 目录未创建而失败。
- 失败：test_ttl_tcp_replication.sh，master probe 未得到预期响应。

本次没有执行外部 ONNX/FAISS/hnswlib 全构建、100K benchmark 或主程序全路径 sanitizer，因此不得写成当前重现结果。

---

# 16. Git 演进与最多 5 个面试案例

## Case 1：三种网络模型收敛到统一 Executor

- 代表提交：b6c89ba、da5699f、0db4f00、5b64d56、b851923。
- 问题：各网络后端若直接执行命令/发送响应，会重复数据库并发、socket ownership 和关闭竞态。
- 修改：抽象 command/response task 与 async adapter；Reactor、Proactor、NtyCo 分阶段接入同一 executor。
- 验证：Route C contract、网络集成和 session lifecycle 测试。
- 面试价值：可以完整讲 owner、deep copy、eventfd notification、fd reuse 防护与 single_inflight。

## Case 2：后台遍历共享数据的 race 收敛为 immutable snapshot

- 代表提交：b385149、6cc8278、90b1072、f466ae7、efe23bc、ec7c1cc。
- 问题：full sync、RDB save、AOF rewrite 在后台直接遍历全局结构，与 mutation 并发。
- 修改：由 executor 在读锁内深拷贝 snapshot，随后锁外做 IO/网络工作。
- 验证：RDB/AOF rewrite/full-sync 与 Route C snapshot tests。
- 面试价值：能够讲“锁持有时间、内存放大、snapshot 一致点、为什么 IO 必须锁外”。

## Case 3：TTL 从数据结构副作用收敛到 Command Owner

- 代表提交：13a629a、63bd9e2。
- 问题：读路径/后台线程直接删除会打乱提交顺序，复制相对 TTL 会受到延迟影响。
- 修改：lazy retry-with-write-lock；active scan/candidate 经 executor；post_commit 规范化 PEXPIREAT/DEL。
- 验证：TTL 多后端、AOF、RDB、replication 脚本；但当前脚本存在环境漂移。
- 面试价值：适合讲锁升级、TOCTOU、observed_expire_at 和确定性复制。

## Case 4：自研 HNSW v2 从可运行到可校验

- 代表提交：2f1fc7f、7e89a93、4600dd5。
- 问题：早期自研 HNSW 在高维/100K 召回偏低，单纯 QPS 无法判断正确性。
- 修改：独立 core 边界、Flat exact oracle、图构建/邻居选择/反向剪枝、与 FAISS 同参数对照、真实 T2Ranking truth。
- 验证：graph、recall、tombstone、10K/100K benchmark。
- 面试价值：可以讲算法、数据布局、recall/latency tradeoff 与 benchmark attribution，而不只是“调用库”。

## Case 5：HNSW 冷启动与在线重建产品化

- 代表提交：15deef1、74ec6af、26ebf99。
- 问题：100K records-only rebuild 成本高，删除/tombstone 后需要图压缩，又不能暴露半成品图。
- 修改：强校验 snapshot cache；active/shadow/delta replay/final validation/publish/retire；失败保留 old active。
- 验证：snapshot roundtrip/corrupt fallback、shadow publish、rebuild delta/swap/async worker。
- 面试价值：体现 derived state、发布原子性、reader safety 与 failure cleanup。

补充但未列入前五：a99a1e9 是一次 shutdown use-after-free 经 ASan 定位并修复的真实 bug；当前对应历史脚本已清理，适合作为故障排查备选故事，不宜作为核心量化成果。

---

# 17. KVLite 当前实现边界与风险

## P0 / correctness

1. **Embedding task owner 丢失**：prepare failure 或 submit_prepared failure 没有统一 complete/free，可能泄漏 task/session ref 并永久卡住 single_inflight。
2. **HNSW destructive growth**：已填充索引 reserve 重置图；VSET/VDEL 不能保证 Manager、map、graph、delta、AOF、response 一致。
3. **Binary persistence**：AOF %s/strlen 路径、RDB legacy setter 都可能截断 NUL；没有 binary restart E2E。
4. **RDB load UAF/部分恢复**：Array TTL apply 使用已释放 key；load 非先全量校验再 publish。
5. **Replication queue producer 假设错误**：SPMC push 被 executor 与多个 full-sync thread 并发调用。
6. **Slave lifetime**：thread id 写入时序、阻塞 recv join、unlock 后 send 与 close/fd reuse race。
7. **Full sync 无一致性边界**：不是 target-only，且 snapshot/incremental 无 offset barrier。
8. **AOF recovery 宽松**：corruption 后可能跳过并继续，main 未建立 fail-stop/error state。

## P1 / engineering

1. embedding/executor/response queue 缺少总量 backpressure；配置 capacity 未 enforce。
2. strict priority 没有 fairness，TTL/replication/prepared 工作可影响 normal tail latency。
3. EVERYSEC 只 fflush，不是每秒 fsync；命名与常见预期不一致。
4. AOF rewrite 在 executor 上同步等待，长时间阻塞命令。
5. snapshot/rewrite/RDB rename 缺 directory fsync。
6. Reactor EINTR、固定 session/fd 容量等健壮性边界。
7. build-mode 切换可被 stale optional object 污染；部分 shell test 不创建新配置所需 data_dir，并行测试会 make clean 互扰。
8. 缺少 queue depth、oldest task age、replication lag、rebuild memory peak 等系统化观测。
9. blocking broadcaster 使慢 slave 拖累全部 slave。
10. current product integration/TTL replication gates 不是绿色。

## P2 / productionization

1. replication offset、ACK、bounded durable backlog、partial resync、gap/checksum、自动 reconnect。
2. replica READY gate、promotion/restart offset persistence、vector deterministic mutation codec。
3. HNSW growth allocate-copy-validate-publish 与 command-owner no-fail commit。
4. 完整 crash consistency、versioned portable RDB/AOF、corruption policy。
5. TSan/ASan 长时间并发网络/复制/vector rebuild 测试。
6. 高并发服务 SLA、多 seed/多数据集、明确资源 accounting。
7. eviction/cluster/分片等数据库生产能力。

P2 不代表秋招项目失败；相反，这些边界说明项目已经到达需要收敛 correctness 与 productionization，而不是继续堆 Feature 的阶段。

---

# 18. 简历候选亮点（最多 6 项）

## Candidate #1

### 技术主题

三网络后端统一 Command Executor 与连接所有权闭环

### 真实解决的问题

将 epoll Reactor、io_uring Proactor、NtyCo coroutine 从数据库执行和 socket 跨线程操作中解耦，建立单一提交顺序，同时保持响应回到连接 owner。

### 源码实现

显式长度 argv deep copy、single_inflight、session retain/release、response_task pointer transfer、eventfd notification、fd/session_id/generation/closing 校验、partial send。

### 关键文件 / 函数

src/core/kvs_command_executor.c、src/network/net_session.c、reactor.c、proactor.c、ntyco.c，以及 async network adapter。

### 测试证据

Route C owner contract、三网络模型历史集成测试、当前默认构建。

### Benchmark 证据

普通 KV raw.csv 提供端到端 QPS/latency，但不能单独归因给 executor。

### 面试价值

**S**

### 面试风险

中：需要主动说明 g_kv_data_rwlock 仍存在、NtyCo response 模型不同、队列未有界、embedding failure owner 缺口。

### 推荐写法方向

突出“统一提交顺序 + network owner response handoff + fd reuse/lifecycle 防护”，不要泛写“基于异步多线程提升性能”。

## Candidate #2

### 技术主题

Vector Manager/Core 分层与自研 HNSW v2 正确性闭环

### 真实解决的问题

把 records 作为事实源，把 Flat/HNSW 作为可重建索引；解决 record slot 与 HNSW internal slot 生命周期不同导致的 stale hit 风险。

### 源码实现

稳定 record_id、record_id_map、record->id 二次验证、状态/scope/threshold 过滤；HNSW level、efConstruction、heuristic neighbor selection、bidirectional link/reverse prune、efSearch、tombstone。

### 关键文件 / 函数

src/vector/kvs_vector.c、kvs_vector_index.c、src/vector/core/flat、src/vector/core/hnsw_v2。

### 测试证据

Flat exact、HNSW graph/recall/tombstone、manager resolve fastpath；当前 product integration fixture 有漂移失败。

### Benchmark 证据

100K T2Ranking index-only ef64 recall@10=.991、avg 650us；产品 prepared ef128 recall=.9985、avg1.373ms，均为 B 级单线程/单客户端证据。

### 面试价值

**S**

### 面试风险

高：必须主动说明 destructive growth 与 VSET/VDEL failure atomicity blocker，且 ANN recall 不等于 semantic qrels。

### 推荐写法方向

突出“事实源/派生索引边界、候选回表校验、Flat oracle、真实 100K recall-latency”，不要写“生产级亿级向量数据库”。

## Candidate #3

### 技术主题

HNSW snapshot cache 与 active/shadow 在线重建

### 真实解决的问题

降低大索引冷启动成本，并在 tombstone/图退化后重建而不暴露未验证 shadow。

### 源码实现

manifest/fingerprint/checksum/compatibility 校验；missing/corrupt fallback；delta capture/replay；final record/vector/graph/generation/search validation；publish/retire/reader safety；写快照 best effort。

### 关键文件 / 函数

src/vector/kvs_vector_index_snapshot.c、src/vector/core/hnsw_v2_snapshot、src/vector/kvs_vector_rebuild.c、kvs_vector_rebuild_shadow.c。

### 测试证据

manifest、roundtrip、load fallback、shadow publish、delta replay、swap、async worker tests 本次多数通过。

### Benchmark 证据

100K synthetic snapshot load 平均 5.923s，对复用历史 records rebuild baseline 约 508x；B 级且不建议作为主数字。

### 面试价值

**A**

### 面试风险

中高：worker 仍是 guarded second publisher；写路径部分串行；active+shadow+retired 内存叠加。

### 推荐写法方向

突出“派生缓存可删除、校验失败回退、shadow 发布失败保留 active”，不要写“完全无锁/写入零抖动”。

## Candidate #4

### 技术主题

Executor immutable snapshot 与锁外持久化 IO

### 真实解决的问题

消除 RDB/full-sync/AOF rewrite 后台直接遍历可变结构的 race，并缩短全局数据锁覆盖磁盘 IO 的时间。

### 源码实现

executor read-lock 下深拷贝 snapshot，unlock 后临时文件写入、fsync、rename；vector records 进入 RDB、graph 不进入事实格式。

### 关键文件 / 函数

src/persistence/kvs_rdb.c、kvs_aof.c、kvs_aof_rewrite.c、src/core/kvs_command_executor.c。

### 测试证据

RDB/vector reload、snapshot task、AOF/rewrite 脚本与历史 Route C tests。

### Benchmark 证据

无可安全归因的独立持久化吞吐数据。

### 面试价值

**A**

### 面试风险

高：binary persistence、RDB load UAF/部分恢复、EVERYSEC、rewrite 阻塞、dir fsync 都会被追问。

### 推荐写法方向

只写“快照一致点与锁外 IO 重构”，不要写“实现生产级 AOF/RDB 崩溃一致性”。

## Candidate #5

### 技术主题

TTL 的 lazy/active/持久化复制一致性

### 真实解决的问题

在读锁下发现过期时安全删除，避免 TTL 更新后的 stale candidate 误删，并使恢复/复制使用确定性绝对时间。

### 源码实现

retry-with-write-lock、master-only active scan、budget/candidate、observed_expire_at、post_commit PEXPIREAT/DEL。

### 关键文件 / 函数

src/core/kvs_command_executor.c、src/core/kvs_ttl_background.c、四种存储 TTL API。

### 测试证据

多后端、AOF、RDB、TCP replication E2E 脚本存在；本次因 test/config 漂移或 probe 失败未获得绿色证据。

### Benchmark 证据

有 expired-path profile 脚本，无适合作为简历量化的当前 artifact。

### 面试价值

**A**

### 面试风险

中：扫描 cursor/fairness、当前 E2E 漂移、slave 依赖复制删除。

### 推荐写法方向

突出 TOCTOU 与确定性 mutation，不要写“高精度时间轮”或“过期删除无尾延迟影响”。

## Candidate #6

### 技术主题

统一配置与运行期兼容边界

### 真实解决的问题

替代模块内 getenv/硬编码分叉，让默认值、配置文件、CLI override 按统一优先级合并，并拒绝未编译能力与不兼容 vector 模型参数。

### 源码实现

共享 descriptor/schema、来源 metadata、unknown/duplicate/invalid reject、config_root/data_dir 路径解析、compiled capability validation。

### 关键文件 / 函数

src/config、include 中的 kvs_config_t/schema，以及各模块消费点。

### 测试证据

unified config stage1–4、runtime、vector-disabled tests 本次隔离执行通过。

### Benchmark 证据

不适用。

### 面试价值

**B**

### 面试风险

低：技术亮度弱于 executor/HNSW，但边界完整。

### 推荐写法方向

作为主 bullet 的支撑子句或工程化补充，不单独占最大篇幅。

---

# 19. 最终排行榜

## A. 最值得写进简历的技术点

### S

1. 三网络模型统一 Command Executor、response handoff 与 session 生命周期。
2. Vector Manager/Core 事实源分层、自研 HNSW v2、候选回表校验和 Flat recall oracle。

### A

3. HNSW snapshot cache + active/shadow/delta replay/publish/retire。
4. Executor immutable snapshot 与 RDB/AOF rewrite 锁外 IO 重构。
5. TTL lazy/active/PEXPIREAT 确定性提交链路。

### B

6. schema-driven 统一配置与模型/后端兼容校验。

## B. 最值得面试重点准备的模块

| 模块 | 面试价值 | 深挖难度 | 是否写进简历 |
| --- | --- | --- | --- |
| Command Executor / task ownership | S | 高 | 是 |
| Reactor/Proactor/NtyCo response closure | S | 高 | 是 |
| Vector Manager/Core boundary | S | 高 | 是 |
| HNSW v2 graph/search/recall | S | 高 | 是 |
| HNSW snapshot/rebuild | A | 高 | 是 |
| RDB/AOF snapshot 与恢复 | A | 高 | 谨慎写 |
| TTL owner convergence | A | 中高 | 是 |
| Replication queue/slave lifetime | A（风险复盘） | 高 | 不作为完成能力 |
| Embedding worker ownership | A（风险复盘） | 高 | 作为 executor 边界补充 |
| Unified configuration | B | 中 | 可写为工程化支撑 |

## C. 不建议写进简历的能力

1. **eBPF 高性能复制**：删除；实验路径、可靠性不足，收益主要来自 batch，数字 C 级。
2. **生产级主从复制/高可用**：未实现 offset、ACK、backlog、partial resync、reconnect、ready、vector；现有 queue/lifecycle 有 correctness 风险。
3. **端到端 binary-safe KV 数据库**：live path 较安全，但 AOF/RDB restart 不成立。
4. **AOF EVERYSEC 每秒 fsync**：源码不成立。
5. **无损动态 HNSW 扩容/失败原子 VSET**：源码明确不成立。
6. **完全无锁或 single-owner 数据库内核**：仍有全局 rwlock，rebuild worker 是 guarded second publisher。
7. **高并发 100K vector SLA**：现有真实产品证据是单客户端，不能外推。
8. **四种数据结构**作为主亮点：实现真实，但区分度和系统价值低于 executor/vector lifecycle。
9. **Redis 性能全面领先**：raw.csv 各项互有输赢且服务配置证据不完整。
10. **生产级 AOF/RDB crash recovery**：binary、load atomicity、corruption policy、dir fsync 均未闭合。

---

# 20. 量化结果可信度分级

## A 级

当前没有适合直接放简历且同时满足“源码 + 仓库内原始 artifact + 完整可复现命令/环境”的关键数字。

## B 级

- results/raw.csv 的普通 KV vs Redis QPS/P95/P99。
- results/vector_rw_raw.csv 的 synthetic vector endpoint 数据。
- T2Ranking 10K/100K index-only 与 100K product prepared/text 报告。
- 100K snapshot load 报告。

使用时必须连同 dataset、dimension、metric、topK、ef、query count、client/concurrency、AOF/replication 开关与 artifact 边界。

## C 级

- eBPF/batch 历史 README/commit 数字。
- 只在历史阶段报告中出现、当前脚本或 artifact 已删除的数值。

## D 级

- 无源码/测试/文件支撑的“百万级”“生产级”“零拷贝”“高可用”“线性扩展”等描述。

最终简历优先使用 B 级的 T2Ranking 100K 数据，并把条件压缩但保留；C/D 不作为核心量化成果。

---

# 21. 30 道 KVLite 项目深挖题

## Level 1：项目基本理解

1. KVLite 启动时 RDB、AOF、vector rebuild、replication、executor、TTL 和 network 的真实顺序是什么？为什么 recovery 在 executor 前？
2. 为什么 Vector Record Store 是 source of truth，而 HNSW graph/snapshot 只能是 derived cache？
3. Reactor、Proactor、NtyCo 是否各有一套命令执行逻辑？它们共享的边界在哪里？
4. eventfd 中传的是完整 response 还是通知？真正 response 存在哪里？
5. core_flat 在产品中除了 fallback 还有什么正确性价值？
6. 四种 KV 结构如何由命令路由？为什么它们不是项目最强亮点？
7. EXPIRE 为什么在 AOF/replication 中转换为 PEXPIREAT？

## Level 2：源码调用链

8. 从 Reactor 收到一条 pipeline 中的 SET，到 response 写回 socket，逐对象说明 command_task、response_task 和 session 的 owner。
9. argv 为什么必须 deep copy？如果只保存 RESP recv buffer 的指针会发生什么？
10. partial send 在 Reactor 和 Proactor 中分别如何继续？为什么 executor 不能直接 send？
11. VSET 为什么先经过 embedding worker 再回 executor？worker 为什么不能分配 record_id？
12. VSEARCH 得到 HNSW hit 后为什么还要查 record_id_map，并检查 record->id？
13. lazy expiration 为什么是 read lock 观察后 retry-with-write-lock，而不是锁升级或直接删除？
14. RDB save 为什么在读锁内 deep-copy、锁外写盘？这个方案的内存与延迟代价是什么？
15. AOF rewrite 的 Reactor/NtyCo fork 和 Proactor thread 路径有何不同？为什么当前仍会阻塞 executor？

## Level 3：并发、生命周期与持久化

16. session_id、generation、refcount、closing、owner_poller_id 各自防哪类 fd close/reuse/stale response 问题？是否有字段冗余？
17. embedding prepare 失败和 submit_prepared 失败时当前 task 谁释放？连接为什么可能永久 single_inflight？
18. Executor 严格优先级可能怎样影响普通命令 P99？你会怎样增加 fairness 而不破坏 replication apply 顺序？
19. active TTL candidate 为什么携带 observed_expire_at？给出 TTL 被延期时的竞态时间线。
20. EVERYSEC 为什么不是每秒 durability？fwrite、fflush、fsync 分别保证什么？
21. 普通 KV live path 使用显式长度，为什么仍不能说端到端 binary-safe？指出 AOF 与 RDB 的具体断点。
22. replication queue 实际是 SPSC/SPMC/MPSC/MPMC 中哪一种？full-sync thread 为什么打破 producer 假设？
23. broadcaster 解锁后 send 与 slave close/remove 之间如何触发 fd reuse race？只加 refcount 是否足够？

## Level 4：系统设计与生产化

24. 如何把 HNSW reserve 改成 lossless allocate-copy-validate-publish，并让 VSET 达到 fallible prepare → no-fail commit？
25. 如何设计 deterministic vector mutation，使 replica 不重跑 ONNX，同时携带稳定 record_id、float32 embedding 和全局 stream offset？
26. Full sync 如何绑定 snapshot offset，并与 incremental backlog 无缝衔接？Replica 何时进入 READY？
27. active/shadow rebuild publish 如何从 worker 直接发布改为 command-owner completion？如何处理 publish 前最后一批 delta？
28. 如何设计 crash-safe RDB/AOF 文件替换？为什么 temp fsync + rename 之后还需要 directory fsync？
29. 如果要证明 100K vector 是服务 SLA，你会如何设计 clients、pipeline、query mix、write/rebuild concurrency、warm/cold、RSS 和 recall 的实验矩阵？
30. eBPF mirror 为什么不能作为可靠复制 truth？若保留它，如何证明 failed command、TCP segmentation、ring loss、顺序和重连的正确性？

---

# 22. 面向简历审阅者的最终建议

建议最终简历只保留 4–5 条主线：

1. Executor + 三网络 owner/lifecycle。
2. Vector Manager/Core + HNSW v2 + Flat oracle + 100K B 级数据。
3. Snapshot/rebuild derived-state lifecycle。
4. TTL 或 immutable persistence snapshot 二选一，依据目标岗位调整。
5. 统一配置作为工程化支撑，不必独占主 bullet。

每条都应主动带一个限制：

- Executor：全局 rwlock 与 queue backpressure 尚未完全收敛。
- HNSW：动态扩容/失败原子性待修。
- Rebuild：worker 仍是 second publisher，内存与写延迟有代价。
- Persistence：不能宣称 binary-safe/生产级 crash consistency。
- TTL：当前 E2E 脚本需修复环境漂移。

不建议在最终简历中出现“生产级、高可用、全面超过 Redis、百万级、零拷贝、无锁复制、eBPF 提升 X%”等没有闭环证据的表述。

---

# 23. 项目定位（不超过 200 字）

如果我是大厂 Linux C/C++ 后端、存储或数据库面试官，我会把 KVLite 定位为“明显超过普通 CRUD/仿 Redis 作业的系统型学生项目”：最强的是跨网络模型的命令所有权收敛，以及 records/Flat/HNSW/snapshot/rebuild 的事实源设计。它适合投存储引擎、数据库内核、Linux 服务端和基础架构岗位；复制、二进制恢复、失败原子性仍非生产级。项目应停止加 Feature，优先补 correctness、故障注入和可复现实验。

---

# 24. 最终工作区说明

本次唯一计划新增文件为：

~~~text
kvlite_resume_source_audit.md
~~~

既有未跟踪文件 config/kvstore-ai-chat-vector.conf 保持不变。没有提交、暂存、修改或格式化任何源码、测试、配置、README、Makefile 与既有文档。
