
理解修正后的目标：你要的不是“所有模块都 lock-free”，而是沿路线 C 的思路，把命令执行及数据库状态提交收敛到单一串行执行域，从而让主命令路径不需要锁；复制线程、后台任务和低频控制面内部可以继续使用锁。

下面这段可直接交给 Codex。

项目目录：

/home/zhengpan/share/KVLite/9.1-kvstore
请执行一次严格的只读架构审计。

任务名称：

Route C Main-Path Lock Elimination Audit
路线 C 串行命令执行与主路径去锁审计
建议模型级别：

GPT-5.6 High
一、先纠正本次目标定义
本次目标不是：

让主从同步无锁
让所有线程无锁
让 HNSW 内部完全 lock-free
让所有后台任务无锁
追求 wait-free/lock-free progress guarantee
真正目标是参考路线 C 的设计初衷：

网络线程只负责收发和任务投递
所有会修改数据库逻辑状态的命令进入统一执行域
命令按照确定顺序串行执行
数据库可变状态只有一个 owner
命令执行过程中避免为数据库状态引入 mutex/rwlock
后台线程不直接并发修改主状态
目标主路径：

网络收包
-> 解析命令
-> command task
-> 串行 executor
-> storage/vector/TTL/persistence/replication propagation
-> response task
-> 网络线程发送响应
这里的“去锁”应理解为：

通过串行执行、单写者和所有权转移消除命令执行路径中的锁
而不是：

把锁替换成更复杂的原子操作
或强行把所有组件改造成 lock-free 数据结构
主从同步模块自身可以使用锁，前提是：

不把复制内部锁扩散到命令执行主路径；

不允许复制线程直接并发修改数据库主状态；

slave apply 最终也进入统一串行提交域；

复制队列、连接、ACK、backlog 等内部同步不影响数据库状态所有权。

二、核心问题
请基于当前真实源码判断：

为了实现“命令串行执行、主路径无锁”，应该：

A. 先完成全部主从同步，再统一做主路径去锁
B. 暂停主从同步，现在先完整重构串行执行模型
C. 在 Replication Stage 1A/1B 前先冻结串行执行和所有权边界，
   然后继续主从同步，过程中禁止引入新的主路径锁
D. 其他更合理的顺序
必须给出明确结论，不得只回答“分阶段实施”。

重点判断：

现在是否已经具备串行命令执行基础
当前哪些锁在串行执行模型下属于冗余锁
哪些锁实际上保护后台线程并发，不能直接删除
哪些模块破坏了单 owner 假设
主从同步是否会扩大这些并发边界
Stage 1A 是否应该顺便冻结主路径去锁设计
三、重新审计路线 C
重新读取并审计：

docs/interview-prep/route_c/
同时搜索源码和历史提交中的：

route_c
executor
command task
response task
single writer
owner thread
weak-lock
real-lock
fallback
worker
task queue
response queue
Reactor
Proactor
NtyCo
shutdown
snapshot
RCU
COW
batch
不要只总结文档，必须对照当前源码和 git 历史。

重点提取路线 C 原始设计：

网络线程是否只负责 I/O；

命令在哪里执行；

数据库状态由哪个线程修改；

task 的创建、转移和销毁位置；

response 如何回到网络线程；

weak-lock 和 real-lock fallback 为什么存在；

哪些锁原本就是为了兼容多执行模式；

Reactor、Proactor、NtyCo 是否共享同一执行语义；

worker pool 是否曾导致数据库状态多写者；

路线 C 是否真正实现了“串行命令执行”，还是只实现了部分任务串行化。

必须说明路线 C 的哪些设计可以直接用于当前版本，哪些已经因 vector、async rebuild、snapshot、replication 而失效。

四、定义目标并发模型
请评估以下目标模型是否适合 KVLite。

4.1 网络层
Reactor/Proactor/NtyCo
只允许：

接收字节
协议解析
构造 command task
投递 executor queue
接收 response task
发送响应
连接生命周期管理
禁止直接：

访问 storage
执行 TTL 删除
修改 vector records
写 HNSW
追加 AOF
修改 replication offset
执行 slave mutation apply
4.2 命令执行层
设置唯一逻辑执行域：

command execution owner
它负责串行执行：

普通 KV 写命令
普通 KV 读命令
TTL/PERSIST
VSET/VGET/VSEARCH/VDEL/VINDEX/VINFO
AOF 提交顺序
复制 mutation 生成顺序
slave mutation apply
需要确定顺序的管理命令
请判断这个 owner 应该是：

单 executor thread
固定分片的多个 single-writer executor
当前 executor 模型的严格串行模式
其他模型
当前项目目标优先保证简单、可证明的命令顺序，不要为了吞吐提前设计复杂分片。

4.3 数据所有权
目标原则：

storage records/maps/index/TTL metadata
在命令执行期间只由 command execution owner 修改
后台线程可以拥有自己的私有对象，但不能直接修改 active 主状态。

跨线程通信使用：

消息传递
所有权转移
不可变快照
结果回传
由 owner 执行最终 publish/commit
而不是让多个线程同时进入主数据结构后再加锁。

五、画出当前真实线程拓扑
至少覆盖：

Reactor
Proactor
NtyCo
executor
command task
response queue
四种 storage backend
TTL
RDB
AOF
AOF rewrite
vector manager
embedding prepare
core_flat
core_hnsw_v2
async rebuild worker
snapshot worker
TCP replication master
TCP replication slave
eBPF daemon
shutdown/control path
对于每个模块输出：

模块
线程
读取的共享状态
修改的共享状态
当前 owner
是否跨线程访问
当前同步方式
是否处于命令执行主路径
是否破坏单写者
必须明确回答：

当前是否存在多个 executor 并发执行数据库命令；

三种网络模式是否有不同命令执行方式；

storage backend 是否假设自己会被多线程调用；

VSEARCH 是否与 VSET/VDEL 并发执行；

TTL 删除由命令 owner 还是后台线程执行；

RDB/AOF 是否直接遍历可变数据库；

async rebuild 是否直接读取或修改 active records/index；

snapshot worker 是否直接读取正在修改的 HNSW；

replication slave 是否绕过 executor 直接 apply；

shutdown 是否可能与命令执行并发销毁对象。

六、审计命令主路径中的全部锁
跟踪以下调用链：

客户端请求
-> RESP parser
-> task 创建
-> executor queue
-> executor
-> command dispatch
-> storage/vector manager
-> persistence
-> replication propagation
-> response build
-> response queue
-> socket send
搜索并列出主路径中的：

pthread_mutex
pthread_rwlock
spinlock
atomic_flag
condition variable
semaphore
weak-lock
real-lock
fallback lock
形成表格：

锁或同步对象
所在文件/函数
调用线程
保护的数据
保护的不变量
命令主路径是否会获取
单写者模型下是否仍有必要
能否直接删除
删除前必须满足的条件
将锁分成四类：

第一类：串行执行后明确冗余
例如只用于防止两个命令同时修改同一数据库对象，但所有命令已经由唯一 owner 串行执行。

第二类：保护后台线程与命令线程并发
例如：

async rebuild
snapshot
AOF rewrite
RDB save
replication full sync
这类不能只因为命令串行就直接删除，必须先改变后台任务的数据访问方式。

第三类：保护对象生命周期
例如：

shutdown
active/shadow/retired
connection
task
snapshot buffer
replication session
这类需要通过所有权、引用计数、join 或显式阶段收敛，不能机械删除。

第四类：模块内部锁
例如 replication 内部：

connection
send queue
ACK
backlog
state machine
只要不进入数据库命令执行主路径，可以保留。

七、普通 KV 主路径去锁
重新跟踪普通 KV：

SET
GET
DEL
EXPIRE
PERSIST
过期删除
Pipeline
RDB
AOF
AOF rewrite
FULLDUMP
slave apply
回答：

四种 storage backend 是否都能在唯一 owner 下无锁使用；

backend 内部是否存在为多线程准备但当前不必要的锁；

GET 和 SET 是否都进入同一个串行 executor；

是否需要让只读命令也串行；

Pipeline 中的多个命令是否保持确定顺序；

lazy expire 是否在 owner 中完成；

active expire 是否有独立线程直接删除数据；

AOF append 是否在 owner 中完成；

RDB/AOF rewrite 如何在不锁住主路径的情况下获取一致视图；

FULLDUMP 是否会要求长时间持有数据库锁。

对于当前阶段，优先保证：

所有命令统一串行
不要提前引入并行读、分片执行或多 writer。

八、向量命令主路径去锁
覆盖：

VSET 新增
VSET 覆盖
VDEL
VGET
VSEARCH
VINDEX
VINFO
TTL 触发向量删除
RDB/AOF 恢复
slave mutation apply
跟踪：

embedding prepare
record allocation
logical-key lookup
record_id allocation
record store
logical-key map
record-id map
active index
shadow index
rebuild delta
AOF
future replication mutation
response
为每一步标记：

执行线程
owner
是否在串行执行域
是否跨线程
是否持锁
锁保护什么
能否通过 owner 模型去锁
重点回答：

vector manager 是否应由 command execution owner 直接拥有；

records、logical-key map、record-id map 是否应只由 owner 修改；

core_flat 是否可在串行命令下完全不需要主路径锁；

core_hnsw_v2 的 add/delete/search 是否都由同一 owner 调用；

如果 VSEARCH 也串行执行，是否还需要 HNSW reader lock；

当前 HNSW 锁是否只因 async rebuild/snapshot 并发而存在；

async rebuild 能否改成私有 shadow graph 构建；

shadow 构建完成后是否应向 owner 提交 publish 请求；

delta 是否只能由 owner 追加和 replay；

active/shadow/retired 的 swap 是否只能由 owner 执行；

retired graph 是否能在确认无后台使用者后由 owner 回收；

snapshot 是否应从稳定 generation 或私有副本生成；

是否完全没有必要让 HNSW 原地图修改与 VSEARCH 并发。

本项目当前目标不是“无锁并行搜索”，而是：

VSET/VDEL/VSEARCH 等命令按统一顺序串行执行，
因此不需要在命令之间互相加锁。
九、embedding prepare 的边界
embedding 计算耗时较长，需要特别判断。

比较以下方案：

方案 1：全部在串行 owner 中执行
VSET/VSEARCH
-> tokenizer
-> ONNX
-> commit/search
优点是语义简单，但可能长时间阻塞所有命令。

方案 2：后台 prepare，owner 串行 commit
owner 接收命令
-> 生成 prepare job
-> embedding worker 只计算不可变 embedding
-> 结果回到 owner
-> owner 重新校验
-> 串行 commit/search
请判断路线 C 是否已有类似 prepare/commit 分离。

如果使用后台 prepare，必须分析命令顺序：

VSET A 开始 prepare
VSET B 开始 prepare
B 先完成
是否允许 B 先 commit，还是必须保持原始命令顺序。

请给出建议：

严格按接收顺序提交
允许 prepare 并行但 commit reorder buffer 保序
暂时全部串行执行
其他方案
当前优先保证：

命令执行语义清晰
提交顺序确定
复制 sequence 与提交顺序一致
不能因为 embedding worker 引入多 writer。

十、后台任务如何避免把锁重新带回主路径
重点审计：

RDB save
AOF rewrite
HNSW rebuild
HNSW snapshot
TTL active expire
replication full sync
observability
对于每类后台任务，判断应采用：

owner 创建不可变快照
owner 复制必要元数据
copy-on-write
私有 shadow state
后台只做 prepare
结果回到 owner publish
短暂 owner barrier
低频控制面锁
请明确哪些任务可以做到完全不获取数据库主路径锁，哪些任务可能仍需：

短时间 stop-the-world
短临界区
real-lock fallback
低频后台路径允许保留锁，但不能让普通命令长期等待后台任务持有的锁。

十一、主从同步与主路径去锁的关系
不要分析“如何让 replication 本身无锁”。

只分析 replication 是否会破坏串行命令 owner 模型。

主节点
目标：

owner 串行完成 manager/storage commit
-> owner 生成确定性 mutation
-> owner 分配 sequence 或调用唯一 sequence allocator
-> mutation 交给 replication subsystem
-> 返回客户端成功
复制模块可以自行使用锁。

必须回答：

mutation enqueue 是否会在命令主路径获取 replication mutex；

是否应使用有界队列或单向交接接口隔离复制内部锁；

enqueue 失败时主写语义是什么；

mutation 对象在何处完成所有权转移；

replication thread 是否允许读取 records/maps/index；

replication thread 是否只能读取 mutation 自包含数据；

TCP send queue 是否与可靠 mutation backlog 分离；

复制内部阻塞是否可能反压并阻塞所有命令。

从节点
目标：

replication 网络线程接收 mutation
-> 解码为 apply task
-> 投递统一 command execution owner
-> owner 按 sequence 串行 apply
-> apply 完成后反馈 replication 状态
禁止：

replication 网络线程直接修改 storage/vector/HNSW
必须判断当前普通 KV slave apply 是否已经满足该模型。

关键判断
主从同步不要求无锁，但必须满足：

复制线程不成为第二个数据库 writer
复制 apply 不绕过串行 executor
复制内部锁不扩散到数据库数据结构
十二、比较实施顺序
请重点比较以下三种路线。

路线 A：先完成主从同步，再处理主路径锁
分析是否会导致：

slave apply 形成第二套执行入口
replication thread 直接访问数据库
主路径新增 replication mutex
mutation sequence 与 executor 顺序脱节
后续去锁需要重构复制链路
路线 B：暂停主从同步，先做完整主路径去锁重构
分析是否会：

扩大当前阶段范围
阻塞 Replication Stage 1A
在复制 apply/full sync 尚未落地前过度设计
一次修改过多模块
增加回归风险
路线 C：现在先冻结串行 owner 边界，再继续复制
建议重点评估：

1. 先确认所有普通命令和向量命令的唯一执行入口
2. 禁止后台线程和 replication thread 直接修改数据库
3. 冻结 prepare/commit/publish 模型
4. 暂时保留现有 correctness locks
5. 实现 Replication Stage 1A/1B
6. 确认 slave apply 也进入同一 owner
7. 按模块移除已证明冗余的命令路径锁
8. 最后处理后台 snapshot/rebuild/RDB/AOF 的锁
必须判断路线 C 是否最符合当前项目。

十三、Stage 1A 与主路径去锁
当前下一阶段：

Replication Stage 1A
HNSW 写入、容量与失败原子性前置审计
请判断 Stage 1A 是否应继续。

重点判断 Stage 1A 内是否必须提前冻结：

VSET 唯一执行线程
VSET prepare/commit/post-commit
records/maps/index 的唯一 writer
HNSW add/delete 的唯一调用 owner
rebuild delta 的唯一 writer
active/shadow publish 的唯一执行 owner
future replication mutation 的唯一生成点
client response 与 commit 的顺序
Stage 1A 不需要立即删除所有锁，但实现方案不得新增：

VSET 全局 mutex
vector manager 大锁
HNSW add 与 records commit 之间的嵌套锁
replication mutex 进入 vector commit 临界区
十四、给出建议目标架构
至少给出以下对象的 owner 表：

对象
唯一 owner
其他线程如何访问
跨线程消息
生命周期结束位置
是否允许内部锁
至少覆盖：

database/storage
TTL metadata
vector records
logical-key map
record-id map
active HNSW
shadow HNSW
rebuild delta
snapshot image
AOF state
RDB snapshot
replication mutation
replication backlog
slave apply task
command task
response task
建议使用类似结构：

Network thread
  -> command task queue

Command owner
  -> serial execute
  -> storage/vector mutation
  -> AOF ordering
  -> replication mutation creation
  -> response task

Embedding/rebuild/snapshot workers
  -> private prepare only
  -> completion task

Replication subsystem
  -> owns connection/backlog/ACK
  -> never mutates database directly

Slave replication receiver
  -> apply task queue
  -> command owner serial apply
十五、给出分阶段实施路线
输出可执行路线，建议考虑：

Main-Path Lock Stage 0
重新审计路线 C 和当前执行拓扑

Main-Path Lock Stage 1
冻结唯一 command owner 和所有权规则

Replication Stage 1A
HNSW 容量与失败原子性

Replication Stage 1B
向量 mutation，主节点 post-commit 交付

Replication Stage 2
full sync，slave apply 进入 command owner

Main-Path Lock Stage 2
删除普通 KV 命令路径冗余锁

Main-Path Lock Stage 3
删除 vector records/maps/index 命令路径冗余锁

Main-Path Lock Stage 4
重构 rebuild/snapshot/RDB/AOF 后台访问

Replication Stage 3
offset/ACK/backlog/reconnect

Main-Path Lock Stage 5
shutdown、fallback 和残余锁收敛
阶段可以调整，但必须说明：

目标
修改范围
前置条件
完成门禁
测试门禁
是否阻塞 replication
十六、测试门禁
请提出能证明串行执行和主路径去锁正确性的测试，而不仅是性能测试。

至少包括：

不同网络模式命令顺序一致
Pipeline 顺序一致
多个客户端并发请求最终串行提交
VSET 覆盖与 VDEL 顺序确定
TTL 删除与用户写命令顺序确定
embedding prepare 乱序完成但 commit 保序
async rebuild 完成时 publish 由 owner 执行
slave apply 与本地命令不并发修改数据库
shutdown 不与 command owner 并发销毁状态
TSAN 无数据库主状态 data race
fault injection 下 owner 状态不半提交
性能测试重点观察：

命令主路径 lock_wait
executor queue latency
embedding prepare latency
AOF 延迟
replication enqueue 延迟
HNSW rebuild/snapshot 对普通命令的阻塞
十七、输出文件
本次只允许新增：

docs/replication/route_c_main_path_lock_elimination_audit.md
允许更新：

docs/development/kvstore_development_checklist.md
开发清单只能记录：

路线 C 主路径去锁实施顺序审计完成
主路径去锁实现未开始
Replication Stage 1A 实现未开始
不得修改：

生产代码
测试代码
Makefile
配置
公开协议
RDB/AOF 格式
kvs_vector_index_ops_t
不得提交 commit。

十八、最终报告结构
报告至少包含：

1. Executive conclusion
2. “主路径去锁”目标定义
3. 路线 C 原始设计复核
4. 当前命令执行与线程拓扑
5. 当前数据库状态 owner
6. 命令主路径锁清单
7. 普通 KV 串行执行分析
8. 向量命令串行执行分析
9. embedding prepare/commit 顺序
10. 后台任务与主路径隔离
11. replication 对单 owner 模型的影响
12. 先复制/先去锁/先冻结 owner 三方案比较
13. Stage 1A 应冻结的设计边界
14. 推荐目标架构
15. 分阶段实施路线
16. 测试和 profiling 门禁
17. 风险与待决问题
18. 最终 flags
十九、最终必须明确回答
当前目标是否应定义为“串行命令执行下的主路径去锁”：yes/no
是否要求 replication 内部无锁：yes/no
是否要求 HNSW 内部完全 lock-free：yes/no
是否应继续 Replication Stage 1A：yes/no
是否应在 Stage 1A 前冻结唯一 command owner：yes/no
是否应现在全面删除现有锁：yes/no
是否应先禁止第二数据库 writer：yes/no
所有数据库命令是否应进入统一执行域：yes/no
VSEARCH 当前是否应与写命令串行：yes/no
slave apply 是否必须进入统一执行域：yes/no
replication thread 是否可以直接修改数据库：yes/no
后台 rebuild 是否只能构建私有 shadow：yes/no
active/shadow publish 是否应由 command owner 完成：yes/no
低频 replication/control path 是否允许使用锁：yes/no
主路径 correctness lock 是否只能在所有权证明后删除：yes/no
最终 flags：

route_c_reaudited=yes/no
main_path_lock_elimination_goal_correctly_defined=yes/no
current_command_execution_topology_mapped=yes/no
database_single_owner_verified=yes/no
second_writer_paths_identified=yes/no
main_path_lock_inventory_completed=yes/no
ordinary_kv_serial_execution_verified=yes/no
vector_serial_execution_verified=yes/no
embedding_prepare_commit_order_evaluated=yes/no
background_worker_isolation_evaluated=yes/no
replication_database_writer_risk_evaluated=yes/no
slave_apply_owner_model_proposed=yes/no
stage1a_should_continue=yes/no
command_owner_should_be_frozen_before_stage1a=yes/no
locks_should_be_removed_immediately=yes/no
ownership_first_then_replication_recommended=yes/no
main_path_lock_elimination_roadmap_actionable=yes/no
ready_for_replication_stage1a_implementation=yes/no
二十、硬约束
本次必须基于当前源码，而不是复述历史设计。

关键结论尽量附：

文件路径
函数名
行号
相关 git commit
如果历史路线 C 文档与当前源码不一致，以当前源码为准。

不得把以下概念混为一谈：

串行命令执行
数据库单写者
主路径不加锁
无锁队列
RCU
完全 lock-free
replication 内部无锁
本次最重要的判断是：

如何通过统一串行执行域和严格数据所有权，
让普通 KV、向量命令以及未来 slave apply
在执行数据库命令时不需要互相加锁，
同时允许 replication、网络、后台构建和控制面
在各自私有状态内部继续使用锁。
更符合你当前目标的预期结论应当是：现在先冻结路线 C 的“唯一命令执行域”和“数据库单写者”边界，但不要立即大规模删锁；随后继续 Stage 1A/1B，并强制未来 slave apply 也进入同一串行执行域。等所有第二写者路径被清除后，再逐项删除主路径上的 correctness locks。



粘贴的 markdown (1)。md(61)
文件
已完成 Route C 主路径去锁审计。

报告：[route_c_main_path_lock_elimination_audit.md](/home/zhengpan/share/KVLite/9.1-kvstore/docs/replication/route_c_main_path_lock_elimination_audit.md)

清单已更新：[kvstore_development_checklist.md](/home/zhengpan/share/KVLite/9.1-kvstore/docs/development/kvstore_development_checklist.md:568)

核心结论：

- 选择路线 C：先冻结唯一 command owner，再继续 Replication Stage 1A/1B，最后逐段移除主状态锁。
- Reactor、Proactor、NtyCo、TTL scan、普通 TCP slave apply 已进入单 executor。
- 普通 KV runtime 已基本实现单 owner 串行执行。
- 当前数据库尚非完整单 owner：async rebuild worker直接读取 records、发布 active/shadow、清理 retired 并写 HNSW snapshot。
- VSEARCH 当前应继续与 VSET/VDEL 串行，不需要实现 HNSW 并行无锁读取。
- embedding 可并行 prepare；同连接严格保序，跨客户端以 owner 实际 commit 顺序作为线性化和 replication sequence 顺序。
- g_kv_data_rwlock 在 executor 中默认 no-op，但 stats mutex仍进入每次 wrapper 调用。
- command/response queue、AOF、allocator、replication 和 lifecycle 内部锁允许保留。
- future mutation 必须在 commit 前完成容量预留，commit 后发布不得失败。

验证结果：

text
git diff --check = PASS
required answers/flags = complete
trailing whitespace = none
build/tests = not run（只读审计阶段）
commit = none


当前工作树：

text
 M docs/development/kvstore_development_checklist.md
?? docs/replication/replication_lockfree_sequencing_audit.md
?? docs/replication/replication_stage1a_preimplementation_audit.md
?? docs/replication/route_c_main_path_lock_elimination_audit.md


最终状态：

text
database_single_owner_verified=no
ordinary_kv_serial_execution_verified=yes
vector_serial_execution_verified=no
stage1a_should_continue=yes
command_owner_should_be_frozen_before_stage1a=yes
locks_should_be_removed_immediately=no
ready_for_replication_stage1a_implementation=yes


分析内容 是否合理 下一步应该做什么内容

思考了 2m 18s
结论
审计的主方向是合理的，核心判断基本正确：

先冻结唯一 command owner 和线性化规则
-> 保留 correctness locks
-> 继续 Replication Stage 1A/1B
-> 禁止复制和后台线程成为新的数据库 writer
-> 最后逐段删除已经失去作用的主状态锁
它准确区分了“数据库主路径去锁”和“所有模块完全 lock-free”，也正确识别出当前真正破坏单 owner 的主要路径不是普通 KV、网络线程或 slave apply，而是 async rebuild worker 对 records、active/shadow publish、retired cleanup 和 snapshot 的直接操作。


不过报告内部还有几处需要在进入代码实现前修正。

一、正确且应保留的结论
1. 路线 C 的方向正确
普通 KV、TTL、三种网络模式和普通 TCP slave apply 已经基本收敛到单 executor。继续围绕单 command owner 扩展，比现在重做无锁队列、RCU 或并行读模型更合理。

你的目标应继续定义为：

所有数据库命令串行线性化
数据库主状态只有一个 writer
后台线程只处理私有状态或不可变快照
跨线程通过任务和所有权转移协作
而不是“任何地方都没有 mutex”。

2. VSEARCH 暂时继续串行是正确的
当前完全没有必要为了 VSEARCH 并行读取，提前引入：

epoch
hazard pointer
RCU
active graph reader reference
retired generation 延迟回收
VSEARCH 与 VSET/VDEL 串行，会让 HNSW 的读写、生命周期和复制顺序都更容易证明。

3. embedding 的顺序定义合理
报告提出：

同连接严格保序
跨连接以 command owner 实际 commit 顺序线性化
replication sequence 与 commit 顺序一致
这是正确的。

没有必要为了不同客户端 ONNX prepare 的开始顺序，引入全局 reorder buffer。否则一个慢 embedding 会阻塞所有其他连接。

4. 不立即删除锁是正确的
目前 vector locks 仍然保护真实存在的跨线程关系，而不仅仅是历史遗留：

executor
<-> async rebuild worker
现在删除 active_index_rwlock 或 rebuild_worker_rwlock，会直接产生数据竞争或生命周期问题。

二、报告中需要修正的四个问题
1. ready_for_replication_stage1a_implementation=yes 存在歧义
报告同时写了：

Main-Path Lock Stage 1 是 Stage 1A 的阻塞前置
以及：

ready_for_replication_stage1a_implementation=yes
这两个结论严格来说不完全自洽。

当前更准确的状态应为：

ready_for_main_path_owner_contract_implementation=yes
ready_for_replication_stage1a_implementation=no
或者解释为：

完成 owner contract freeze 后，可以立即进入 Stage 1A
现在不建议直接开始 Stage 1A 生产代码修改，应先完成一个很小的 owner contract 阶段。

2. “第二 writer fail-fast”与保留 async rebuild 现状矛盾
报告的路线图要求 Main-Path Lock Stage 1：

第二 writer fail-fast
但又把 async rebuild worker 收敛到 owner 的工作放到了 Main-Path Lock Stage 3。

当前 async rebuild 本身就是已知第二 publisher。如果立刻对所有第二 writer 做 fail-fast，async rebuild 会直接不可用。

因此应改成：

禁止新增第二 writer
对现有 async rebuild 第二 publisher 建立显式 legacy exception
记录唯一允许的例外入口
后续单独消除该例外
而不是现在就宣称完整数据库单 owner。

3. mutation 可靠接收与 AOF 失败语义不能混在一起
报告提出：

prepare mutation 并预留资源
-> no-fail database commit
-> 发布 mutation
-> response
这个方向适合未来 replication mutation。

但当前 AOF：

post-commit append
接口返回 void
可能涉及文件写入或 fsync
错误不能回滚数据库 commit
所以目前不能笼统定义为：

所有本地 durability 都在 response 前无失败完成
建议明确区分三个概念：

logical commit
replication reliable-stream accept
AOF durability policy
Stage 1B 可以要求 mutation stream 的容量在 commit 前预留，但不应顺便把现有 AOF 改造成事务日志，否则范围会大幅膨胀。

4. “实际 commit 顺序”还需要精确到函数级线性化点
报告已经定义跨客户端按照 owner commit 顺序排序，但还需要进一步明确：

到底在哪一条操作之后算 commit？
不能只写成“executor 执行顺序”。

以 VSET 为例，应精确冻结为类似：

所有可能失败的 allocation/reserve/validation 完成
-> records/maps/index/delta 的 no-fail 发布区间
-> commit sequence 生效
-> AOF/mutation post-commit
-> response
Stage 1A 的核心工作正是把这个区间压缩成一个可以证明不会失败的提交段。

三、下一步不应直接做什么
现在不要：

删除 g_kv_data_rwlock
删除 active_index_rwlock
删除 rebuild_worker_rwlock
重写 command/response queue
让 GET/VSEARCH 并行
引入 RCU、epoch 或 hazard pointer
让 replication thread 直接访问 records/HNSW
同时重构 AOF、shutdown、FULLDUMP 和 async rebuild
尤其不要把“owner 已经确定”理解为“所有锁现在都可以删”。

四、当前最合理的下一阶段
下一阶段建议命名为：

Route C Re-entry Stage 1
Command Owner Contract Freeze
这是一个很小的代码与测试阶段，位于 Replication Stage 1A 之前。

目标不是删除锁，而是把所有权规则变成可执行约束。

必须完成的内容
第一，增加 command owner 身份判断：

kvs_command_executor_is_owner_thread()
kvs_command_executor_assert_owner_thread()
在 debug/test 构建下，对以下运行期入口增加断言：

普通 KV runtime mutation
TTL apply
vector records/maps mutation
active HNSW add/delete
rebuild delta append
slave apply
future active/shadow publish API
启动恢复和 shutdown drain 后的销毁路径应列为显式例外，不能被误判为 runtime 第二 writer。

第二，冻结允许的线程边界：

network thread
  只能 submit task

embedding worker
  只能写 task-private prepared result

replication receiver
  只能构造 replicated task

TTL scanner
  只能提交 scan task

async rebuild worker
  当前为唯一已知 legacy exception
  禁止扩展其权限
第三，冻结线性化规则：

同连接：严格顺序
跨连接：owner 实际 commit 顺序
record_id：只能由 owner 分配
replication sequence：只能在 owner commit 边界分配
prepare worker：不能分配 record_id 或 sequence
第四，增加当前树可执行测试：

Reactor/Proactor/NtyCo 都进入同一 executor
TTL 删除由 executor 执行
普通 slave apply 由 executor 执行
embedding worker 不能调用 manager commit
非 owner 调用 runtime mutation API 时 fail-fast
startup replay 和 shutdown destroy 的例外可控
第五，暂时不改变行为：

不删除锁
不修改 HNSW 算法
不实现 mutation
不修改复制 wire
不重构 async rebuild
五、完成 owner contract 后进入 Stage 1A
Stage 1A 随后只处理：

HNSW 容量安全
VSET/VDEL prepare、commit、post-commit
records/maps/index 失败原子性
rebuild delta append 失败语义
allocator/index-add fault hooks
未来 mutation post-commit 扩展点
Stage 1A 的理想提交模型：

PREPARE
  record/string/embedding allocation
  map/index capacity reserve
  HNSW capacity validation
  delta capacity validation
  future mutation capacity contract validation

COMMIT
  不执行内存分配
  不执行 destructive reserve
  不进入 replication mutex
  不发生可恢复失败
  串行发布 records/maps/index/delta

POST-COMMIT
  AOF
  future mutation publish
  response
其中 future mutation 在 Stage 1A 只冻结接口和容量语义，不实现 codec、backlog 或 TCP wire。

六、文档工作树也应先收敛
当前存在三个未跟踪审计文档：

replication_lockfree_sequencing_audit.md
replication_stage1a_preimplementation_audit.md
route_c_main_path_lock_elimination_audit.md
进入代码前应明确各自职责：

route_c_main_path_lock_elimination_audit.md
  主路径所有权和去锁总原则

replication_stage1a_preimplementation_audit.md
  Stage 1A 容量与失败原子性的实现依据

replication_lockfree_sequencing_audit.md
  若内容与前两者重复，应合并、标记 superseded 或删除
不建议带着三份可能互相覆盖的审计文档直接开始实现，否则后续 Codex 容易引用不同版本的结论。

推荐执行顺序
1. 合并并冻结三份审计文档
2. 单独提交审计文档
3. Route C Re-entry Stage 1：owner contract freeze
4. 运行三网络模式、TTL、slave apply、TSAN 基础门禁
5. Replication Stage 1A：容量与失败原子性
6. Replication Stage 1B：确定性 mutation 和本地可靠接收
7. Replication Stage 2：full sync、READY、slave vector apply
8. 再收敛 async rebuild publish
9. 最后删除 vector 和普通 KV 主状态冗余锁
一句话判断：

审计方向正确，但现在还不应直接“去锁”，也不应直接进入大规模 Stage 1A 修改。

下一步应先完成一个小范围的 command owner 契约冻结阶段，
把单 owner 从设计结论变成运行期断言和测试约束，
随后立即继续 Replication Stage 1A。

将该阶段的统一约束生成一个文档 我放在docs/replication下 避免每次生成prompt都是完整的重复的约束 

思考了 18s

生成文档计划
我会把路线 C 的主路径去锁原则、owner 契约、阶段边界和后续 Codex 引用方式合并成一份长期约束文档。

已生成统一约束文档：

route_c_main_path_owner_constraints.md

建议放置到：

docs/replication/route_c_main_path_owner_constraints.md
文档已经统一冻结：

主路径去锁的准确目标；

唯一 command owner 契约；

数据对象 owner 矩阵；

embedding prepare 与跨客户端线性化规则；

async rebuild、snapshot、TTL、RDB/AOF 后台边界；

主节点 mutation 和从节点 apply 约束；

VSET/VDEL 的 PREPARE、COMMIT、POST-COMMIT 模型；

当前不得删除的锁及删除前置条件；

async rebuild 的 legacy second-publisher 例外；

Owner Contract、Replication Stage 1A-4 与后续去锁顺序；

每阶段通用测试、报告和硬边界；

后续 Codex 最小 Prompt 模板。

以后给 Codex 的阶段 Prompt 只需保留：

开始前必须读取并遵守：

docs/replication/route_c_main_path_owner_constraints.md
docs/replication/route_c_main_path_lock_elimination_audit.md
docs/replication/replication_stage1a_preimplementation_audit.md
docs/development/kvstore_development_checklist.md
然后补充该阶段的目标、允许修改范围和测试门禁即可。 


库
/
route_c_main_path_owner_constraints.md


# Route C 主路径单 Owner 与去锁统一约束

> 建议路径：
>
> ```text
> docs/replication/route_c_main_path_owner_constraints.md
> ```
>
> 本文是 KVLite 后续主从同步、向量写入、后台任务收敛和主路径去锁阶段的统一架构约束。
> 后续阶段 Prompt 应优先引用本文，不再重复完整背景与约束。
>
> 当本文与当前源码事实冲突时，以当前源码为准，并先更新本文后再继续实现。

---

## 1. 文档目的

KVLite 当前长期目标不是让所有模块实现完全 lock-free，而是沿 Route C 的设计思路：

```text
通过统一串行命令执行域、数据库单写者和跨线程所有权转移，
避免在数据库命令执行主路径中为主状态引入 mutex/rwlock。
```

本文统一约束以下后续工作：

```text
Replication Stage 1A-4
普通 KV 主路径去锁
向量命令主路径去锁
async rebuild owner 收敛
HNSW snapshot/publish 收敛
slave apply
RDB/AOF/full sync snapshot
shutdown 与故障恢复
```

---

## 2. 目标定义

### 2.1 本项目所称“主路径去锁”

```text
所有会读取或修改数据库逻辑状态的命令，
由唯一 command owner 按确定顺序执行。

在该串行执行域内部，
storage、TTL metadata、vector records/maps、
active index 和复制 mutation 顺序
不再依赖数据库级 mutex/rwlock 保证命令间互斥。
```

### 2.2 本项目不追求

```text
所有线程无锁
所有队列无锁
replication 内部无锁
HNSW 内部完全 lock-free
wait-free progress guarantee
任意线程并行修改数据库
VSEARCH 与 VSET/VDEL 并行执行
```

### 2.3 允许保留锁的模块

以下锁可以保留，只要不承担数据库主状态的多 writer 协调：

```text
command/response queue
embedding worker queue
replication connection/backlog/ACK/state machine
AOF buffer、fd、rewrite state
allocator
日志
worker lifecycle
snapshot completion
shutdown/control path
低频 fallback
```

---

## 3. 统一目标架构

```text
Network owner
  -> 收包和协议解析
  -> 创建 command task
  -> 投递 command executor
  -> 接收 response task
  -> 发送响应

Single command owner
  -> 串行执行所有数据库命令
  -> 决定全局线性化和 commit 顺序
  -> 修改 storage/TTL/vector 主状态
  -> 生成 AOF 顺序
  -> 生成 replication mutation 和 sequence
  -> 处理 slave apply
  -> 执行 active/shadow publish
  -> 创建 response task

Prepare workers
  -> 只处理 task-private 或 worker-private 状态
  -> embedding prepare
  -> private shadow index build
  -> snapshot file I/O
  -> 完成后向 command owner 提交 completion task

Replication subsystem
  -> 拥有 connection/backlog/ACK/offset/state machine
  -> 可以使用内部锁
  -> 不读取或修改 live database state
  -> 从节点接收后只生成 slave apply task
```

---

## 4. 唯一 Command Owner 契约

### 4.1 唯一执行域

运行期以下操作必须由唯一 command owner 执行：

```text
普通 KV GET/SET/DEL
EXPIRE/PERSIST
lazy expire 删除
active TTL scan/delete
Pipeline 中的数据库命令
VSET/VGET/VSEARCH/VDEL/VINDEX/VINFO
vector records/maps 修改
record_id 分配
active flat/HNSW add/delete/search
rebuild delta append
active/shadow publish
普通及向量 slave mutation apply
AOF 命令顺序确定
replication sequence 分配
replication mutation 生成
客户端成功响应判定
```

### 4.2 允许的非 runtime owner

以下场景可以成为显式 owner，但不得与 runtime command owner 并发：

```text
服务启动前的 RDB/AOF load
executor 停止并完成 drain 后的 shutdown destroy
离线工具或只读测试中的显式单线程入口
```

这些入口必须作为明确例外记录，不能成为运行期第二 writer。

### 4.3 禁止的第二 writer

运行期以下线程不得直接修改数据库主状态：

```text
Reactor/Proactor/NtyCo 网络线程
embedding worker
TTL scanner thread
replication receive/send thread
full-sync sender
async rebuild worker
snapshot writer
AOF rewrite worker
eBPF daemon
observability thread
```

---

## 5. 线性化与命令顺序

### 5.1 同一连接

```text
同一连接的命令、Pipeline 和响应必须严格保序。
```

### 5.2 不同连接

不同客户端之间不承诺 socket 到达时间顺序。

全局顺序定义为：

```text
command owner 实际执行并完成 commit 的顺序
```

### 5.3 Embedding prepare

允许：

```text
多个 embedding worker 并行 tokenizer/ONNX prepare
prepared task 按完成顺序返回 command owner
```

必须保证：

```text
worker 只写 task-private prepared result
worker 不分配 record_id
worker 不分配 replication sequence
worker 不访问或修改 vector manager 主状态
worker 不决定 commit
```

跨客户端 prepare 完成顺序可以改变 commit 顺序；最终以 command owner 实际 commit 顺序为准。

当前不引入全局 admission reorder buffer，避免慢 embedding 阻塞所有连接。

---

## 6. 数据所有权矩阵

| 对象 | 唯一 owner | 其他线程访问方式 | 回收/结束位置 | 是否允许内部锁 |
| --- | --- | --- | --- | --- |
| database/storage | command owner | immutable snapshot | owner 或 shutdown owner | 主状态目标无锁 |
| TTL metadata | command owner | scanner 只提交 task | owner | scanner lifecycle 可用 |
| vector records | command owner | immutable rebuild/snapshot image | owner | 主状态目标无锁 |
| logical-key map | command owner | 不共享 live map | owner | 不需要 |
| record-id map | command owner | 不共享 live map | owner | 不需要 |
| active flat/HNSW | command owner | VSEARCH 也由 owner 执行 | owner publish/retire | 过渡期可保留 fallback |
| shadow index | rebuild worker 私有 | completion descriptor 交给 owner | 失败由 worker，成功由 owner | worker 私有无需 data lock |
| rebuild delta | command owner append | worker 读取冻结副本/batch | owner | handoff 可有内部锁 |
| RDB snapshot | command owner 创建 | 保存线程只读 | 保存线程 | completion 同步允许 |
| AOF state | AOF subsystem | owner 按 commit 顺序 append/handoff | AOF shutdown | 允许 |
| vector snapshot image | owner 或 private builder 创建 | writer 只读并写文件 | writer | completion/status 锁允许 |
| replication mutation | command owner 创建 | replication owner 只读 | backlog 安全回收 | immutable |
| replication backlog | replication subsystem | command owner 仅执行可靠 accept | replication subsystem | 允许 |
| slave apply task | replication receiver 创建 | command owner 接管 | owner/completion | ingress queue 可锁 |
| command task | network owner 创建 | prepare worker/command owner 接管 | executor 或 network | queue 可锁 |
| response task | command owner 创建 | network owner 发送 | network owner | queue 可锁 |

---

## 7. 后台任务统一约束

### 7.1 通用规则

后台任务必须遵循：

```text
owner 创建不可变输入或冻结视图
-> worker 只处理私有状态
-> worker 返回 completion
-> command owner 执行最终 publish/commit
```

后台线程不得通过持有数据库大锁长期读取 live state。

### 7.2 Async rebuild

目标模型：

```text
command owner
  -> 创建 immutable record baseline
  -> 冻结 rebuild generation
  -> 启用 owner-only delta append

rebuild worker
  -> 构建 private shadow
  -> replay 冻结 delta batch
  -> 返回 publish completion

command owner
  -> 校验 generation
  -> replay 最终 delta
  -> active/shadow 原子 publish
  -> retired 进入可控回收
```

强制约束：

```text
worker 不直接发布 active
worker 不直接修改 records/maps
worker 不分配 record_id
worker 不直接清理仍可能被使用的 retired
worker 不直接把 live active graph 当作稳定 snapshot 输入
```

### 7.3 RDB/AOF rewrite/full sync

必须由 command owner 创建一致的 immutable snapshot，再由其他线程执行文件或网络 I/O。

禁止后台线程直接遍历正在修改的 live database。

### 7.4 TTL

TTL scanner 只负责定时和投递任务。

TTL scan、expire 判定和删除最终由 command owner 执行。

### 7.5 Snapshot

snapshot writer 只处理稳定 snapshot image 或 worker-private graph。

禁止在后台线程中长时间持有 active index lock 后直接序列化 live active graph。

---

## 8. 主从同步约束

### 8.1 主节点

可靠向量复制的主路径：

```text
command owner prepare
-> 预留所有可能失败的本地资源
-> no-fail database commit
-> 分配 sequence
-> 发布 immutable mutation 到本地可靠 mutation stream
-> 通知 replication subsystem
-> 客户端成功
```

必须区分：

```text
manager/database commit
local reliable mutation stream accept
TCP transient send queue
slave ACK
AOF durability policy
```

强制规则：

```text
不等待 slave ACK
不等于允许本地 mutation 丢失

TCP send queue 不是可靠 backlog

replication thread 不读取 records/maps/index

replication mutex 不得嵌入 storage/vector commit 临界区

mutation 必须自包含，replication thread 只读 mutation
```

### 8.2 从节点

```text
replication receiver
-> 解码和校验
-> 创建 slave apply task
-> 投递 command owner
-> command owner 按 sequence 串行 apply
-> 成功后推进 applied_offset
-> 返回 completion 给 replication subsystem
```

禁止 replication 网络线程直接调用：

```text
storage set/delete
VSET/VDEL
vector manager commit
HNSW add/delete
AOF apply
```

### 8.3 Sequence

```text
sequence 必须由 command owner 在最终 commit 顺序中分配。
```

prepare worker、replication thread 和网络线程不得分配 sequence。

### 8.4 READY

READY/BLOCKED/FAILED 等复制状态由 replication subsystem 拥有。

涉及数据库可见性的状态切换，必须以 command owner apply/full-sync install completion 为依据。

---

## 9. VSET/VDEL 提交模型

### 9.1 PREPARE

所有可能失败的操作应尽可能在 PREPARE 完成：

```text
embedding prepare
record/string/embedding allocation
logical-key lookup
record_id 规划
record store capacity reserve
logical-key map capacity reserve
record-id map capacity reserve
HNSW capacity validation/reserve
flat capacity reserve
rebuild delta capacity reserve
future mutation stream capacity reserve
checksum/validation
```

PREPARE 阶段失败：

```text
不得修改 records/maps/index/delta
不得生成成功 sequence
不得发布 mutation
不得追加成功语义 AOF
不得返回客户端成功
```

### 9.2 COMMIT

COMMIT 必须尽量满足：

```text
由 command owner 独占执行
不进行新的内存分配
不执行 destructive reserve
不获取 replication connection/ACK mutex
不进行文件或网络 I/O
不发生常规可恢复失败
```

COMMIT 发布顺序必须在 Stage 1A 中通过源码和 fault injection 冻结。

### 9.3 POST-COMMIT

允许：

```text
AOF append/handoff
已预留 mutation slot 发布
replication wake/notify
dirty/statistics 更新
response 构造和投递
```

注意：

```text
POST-COMMIT 不代表所有操作都可任意失败。
```

未来可靠 mutation publish 必须在 commit 前完成容量预留，使 commit 后发布不可失败。

现有 AOF durability 语义单独处理，不得在 Replication Stage 1A 中顺便重构为完整事务日志。

---

## 10. 锁处理原则

### 10.1 当前不得立即删除

在 owner 契约和后台边界未完成前，不得直接删除：

```text
g_kv_data_rwlock fallback
active_index_rwlock
rebuild_worker_rwlock
AOF locks
command/response queue locks
allocator locks
replication locks
worker lifecycle locks
shutdown/control locks
```

### 10.2 锁删除前置条件

删除主状态 correctness lock 前必须同时满足：

```text
所有 runtime mutation 入口已进入 command owner
不存在未登记的第二 writer
后台 worker 只处理 private/immutable state
publish 只由 command owner 执行
startup/shutdown 例外已明确
TSAN 无主状态 data race
fault injection 无半提交
三种网络模式行为一致
```

### 10.3 当前已知 legacy exception

当前 async rebuild worker 仍直接：

```text
读取 live records
推进 rebuild context
发布 active/shadow
清理 retired
写 HNSW snapshot
```

在完成专门收敛前，应把它记录为：

```text
唯一已知 legacy second-publisher exception
```

约束是：

```text
禁止新增类似入口
禁止扩大其权限
暂时保留相关 correctness locks
后续通过 private build + owner publish 消除
```

---

## 11. 阶段顺序

### Stage O1：Command Owner Contract Freeze

目标：

```text
把单 owner 从文档结论变成运行期断言和可执行测试。
```

建议内容：

```text
owner thread identity API
debug/test owner assertion
runtime mutation 入口断言
startup/shutdown 显式例外
禁止新增第二 writer
当前 async rebuild legacy exception 登记
三网络模式/TTL/slave apply owner 测试
```

禁止：

```text
删除 correctness locks
重写队列
实现 replication wire
重构 HNSW 算法
```

### Replication Stage 1A

```text
HNSW 容量安全
VSET/VDEL prepare/commit/post-commit
records/maps/index 失败原子性
delta append 失败语义
allocator/index fault hooks
future mutation post-commit 扩展点
```

### Replication Stage 1B

```text
确定性 vector mutation
本地可靠 mutation stream accept
主节点 sequence
slave vector apply task
从节点不运行 ONNX
```

### Replication Stage 2

```text
full sync
base sequence
catch-up
READY/BLOCKED/FAILED
VSEARCH READY gate
```

### Main-Path Lock Stage 2

```text
删除普通 KV executor 内已证明冗余的数据库 wrapper/stats mutex
```

### Main-Path Lock Stage 3

```text
async rebuild private build
publish 返回 command owner
snapshot 输入稳定化
删除 vector client command 路径冗余 data locks
```

### Replication Stage 3

```text
offset
ACK
可靠 backlog
partial resync
reconnect
```

### Replication Stage 4 / Main-Path Lock Stage 4

```text
restart
promotion
recovery
shutdown
fallback 和残余锁收敛
```

---

## 12. 当前阶段硬边界

当前主线只围绕：

```text
Route C owner contract
Replication Stage 1A-4
随后逐段移除主状态锁
```

不得提前扩展到：

```text
并行读 executor
分片 executor
RCU/epoch/hazard pointer
HNSW 原地图并发读写
全面无锁队列
wait-free
eBPF 可靠复制
复制 HNSW graph
从节点重跑 ONNX
公开 V 命令或响应变化
RDB/AOF 格式变化
kvs_vector_index_ops_t 变化
100 万条 benchmark
```

---

## 13. 每阶段通用测试门禁

### Correctness

```text
Reactor/Proactor/NtyCo 进入同一 command owner
同连接 Pipeline 严格保序
多客户端 commit sequence 唯一、单调
TTL 与用户写命令按 owner 顺序线性化
embedding prepare 乱序完成不产生多 writer
VSET overwrite/VDEL 顺序确定
async rebuild publish 只由 owner 执行
slave apply 只由 owner 执行
shutdown 无 late submit/UAF
fault injection 无半提交
```

### Sanitizer

```text
TSAN
ASan
UBSan
```

### Profiling

```text
executor queue wait/depth
owner wrapper/stats lock wait
embedding prepare/HOL
AOF append/fsync
replication reliable accept
active/rebuild lock wait/hold
snapshot/rebuild 对 p95/p99 的阻塞
```

去锁优先依据正确性和所有权简化，不预设性能一定提升。

---

## 14. 通用 Codex 执行约束

后续 Codex 阶段 Prompt 应包含：

```text
开始前必须读取：

docs/replication/route_c_main_path_owner_constraints.md
docs/replication/route_c_main_path_lock_elimination_audit.md
docs/replication/replication_stage1a_preimplementation_audit.md
docs/development/kvstore_development_checklist.md

本文为统一架构约束。
如当前源码与本文冲突，先记录冲突，不得静默绕过。
```

所有阶段必须：

```text
重新读取当前源码
列出实际调用链和 owner
标记 PREPARE/COMMIT/POST-COMMIT
说明是否新增第二 writer
说明是否新增主路径锁
说明是否修改公开协议或持久化格式
运行与本阶段匹配的测试
执行 git diff --check
不得擅自扩大阶段范围
```

---

## 15. 后续 Prompt 最小模板

```text
项目目录：

/home/zhengpan/share/KVLite/9.1-kvstore

执行阶段：

<阶段名称>

开始前必须读取并遵守：

docs/replication/route_c_main_path_owner_constraints.md
docs/replication/route_c_main_path_lock_elimination_audit.md
docs/replication/replication_stage1a_preimplementation_audit.md
docs/development/kvstore_development_checklist.md

本阶段目标：

<只写本阶段新增目标>

允许修改：

<文件或模块范围>

禁止修改：

<阶段外范围>

完成门禁：

<测试、sanitizer、fault injection、flags>

不得提交 commit，除非另有明确指令。
```

后续 Prompt 不再重复本文中的 owner、线程、去锁、replication 和后台任务统一约束。

---

## 16. 通用阶段报告要求

每个阶段最终报告至少包含：

```text
1. 实际修改文件
2. 当前线程和 owner 变化
3. PREPARE/COMMIT/POST-COMMIT 边界
4. 新增或消除的第二 writer
5. 新增或删除的锁
6. failure atomicity
7. 对 replication sequence/order 的影响
8. 对 startup/shutdown 的影响
9. 测试、sanitizer 和 fault injection
10. git diff --check
11. 未完成事项
12. 最终 flags
```

---

## 17. 统一最终判断

```text
main_path_lock_elimination_goal =
  serial command execution + single database owner

replication_internal_lockfree_required = no
hnsw_internal_lockfree_required = no
parallel_vsearch_required = no
single_command_owner_required = yes
slave_apply_through_owner_required = yes
background_private_prepare_required = yes
owner_only_publish_required = yes
correctness_locks_removable_immediately = no
ownership_proof_before_lock_removal = yes
replication_stage1a_should_continue = yes
```

当前推荐顺序：

```text
冻结 command owner 契约
-> Replication Stage 1A
-> Replication Stage 1B
-> Replication Stage 2
-> 收敛 async rebuild publish
-> 分阶段删除普通 KV/vector 主状态冗余锁
-> Replication Stage 3/4 和 shutdown/fallback 收敛
```
库
/
route_c_main_path_owner_constraints.md


# Route C 主路径单 Owner 与去锁统一约束

> 建议路径：
>
> ```text
> docs/replication/route_c_main_path_owner_constraints.md
> ```
>
> 本文是 KVLite 后续主从同步、向量写入、后台任务收敛和主路径去锁阶段的统一架构约束。
> 后续阶段 Prompt 应优先引用本文，不再重复完整背景与约束。
>
> 当本文与当前源码事实冲突时，以当前源码为准，并先更新本文后再继续实现。

---

## 1. 文档目的

KVLite 当前长期目标不是让所有模块实现完全 lock-free，而是沿 Route C 的设计思路：

```text
通过统一串行命令执行域、数据库单写者和跨线程所有权转移，
避免在数据库命令执行主路径中为主状态引入 mutex/rwlock。
```

本文统一约束以下后续工作：

```text
Replication Stage 1A-4
普通 KV 主路径去锁
向量命令主路径去锁
async rebuild owner 收敛
HNSW snapshot/publish 收敛
slave apply
RDB/AOF/full sync snapshot
shutdown 与故障恢复
```

---

## 2. 目标定义

### 2.1 本项目所称“主路径去锁”

```text
所有会读取或修改数据库逻辑状态的命令，
由唯一 command owner 按确定顺序执行。

在该串行执行域内部，
storage、TTL metadata、vector records/maps、
active index 和复制 mutation 顺序
不再依赖数据库级 mutex/rwlock 保证命令间互斥。
```

### 2.2 本项目不追求

```text
所有线程无锁
所有队列无锁
replication 内部无锁
HNSW 内部完全 lock-free
wait-free progress guarantee
任意线程并行修改数据库
VSEARCH 与 VSET/VDEL 并行执行
```

### 2.3 允许保留锁的模块

以下锁可以保留，只要不承担数据库主状态的多 writer 协调：

```text
command/response queue
embedding worker queue
replication connection/backlog/ACK/state machine
AOF buffer、fd、rewrite state
allocator
日志
worker lifecycle
snapshot completion
shutdown/control path
低频 fallback
```

---

## 3. 统一目标架构

```text
Network owner
  -> 收包和协议解析
  -> 创建 command task
  -> 投递 command executor
  -> 接收 response task
  -> 发送响应

Single command owner
  -> 串行执行所有数据库命令
  -> 决定全局线性化和 commit 顺序
  -> 修改 storage/TTL/vector 主状态
  -> 生成 AOF 顺序
  -> 生成 replication mutation 和 sequence
  -> 处理 slave apply
  -> 执行 active/shadow publish
  -> 创建 response task

Prepare workers
  -> 只处理 task-private 或 worker-private 状态
  -> embedding prepare
  -> private shadow index build
  -> snapshot file I/O
  -> 完成后向 command owner 提交 completion task

Replication subsystem
  -> 拥有 connection/backlog/ACK/offset/state machine
  -> 可以使用内部锁
  -> 不读取或修改 live database state
  -> 从节点接收后只生成 slave apply task
```

---

## 4. 唯一 Command Owner 契约

### 4.1 唯一执行域

运行期以下操作必须由唯一 command owner 执行：

```text
普通 KV GET/SET/DEL
EXPIRE/PERSIST
lazy expire 删除
active TTL scan/delete
Pipeline 中的数据库命令
VSET/VGET/VSEARCH/VDEL/VINDEX/VINFO
vector records/maps 修改
record_id 分配
active flat/HNSW add/delete/search
rebuild delta append
active/shadow publish
普通及向量 slave mutation apply
AOF 命令顺序确定
replication sequence 分配
replication mutation 生成
客户端成功响应判定
```

### 4.2 允许的非 runtime owner

以下场景可以成为显式 owner，但不得与 runtime command owner 并发：

```text
服务启动前的 RDB/AOF load
executor 停止并完成 drain 后的 shutdown destroy
离线工具或只读测试中的显式单线程入口
```

这些入口必须作为明确例外记录，不能成为运行期第二 writer。

### 4.3 禁止的第二 writer

运行期以下线程不得直接修改数据库主状态：

```text
Reactor/Proactor/NtyCo 网络线程
embedding worker
TTL scanner thread
replication receive/send thread
full-sync sender
async rebuild worker
snapshot writer
AOF rewrite worker
eBPF daemon
observability thread
```

---

## 5. 线性化与命令顺序

### 5.1 同一连接

```text
同一连接的命令、Pipeline 和响应必须严格保序。
```

### 5.2 不同连接

不同客户端之间不承诺 socket 到达时间顺序。

全局顺序定义为：

```text
command owner 实际执行并完成 commit 的顺序
```

### 5.3 Embedding prepare

允许：

```text
多个 embedding worker 并行 tokenizer/ONNX prepare
prepared task 按完成顺序返回 command owner
```

必须保证：

```text
worker 只写 task-private prepared result
worker 不分配 record_id
worker 不分配 replication sequence
worker 不访问或修改 vector manager 主状态
worker 不决定 commit
```

跨客户端 prepare 完成顺序可以改变 commit 顺序；最终以 command owner 实际 commit 顺序为准。

当前不引入全局 admission reorder buffer，避免慢 embedding 阻塞所有连接。

---

## 6. 数据所有权矩阵

| 对象 | 唯一 owner | 其他线程访问方式 | 回收/结束位置 | 是否允许内部锁 |
| --- | --- | --- | --- | --- |
| database/storage | command owner | immutable snapshot | owner 或 shutdown owner | 主状态目标无锁 |
| TTL metadata | command owner | scanner 只提交 task | owner | scanner lifecycle 可用 |
| vector records | command owner | immutable rebuild/snapshot image | owner | 主状态目标无锁 |
| logical-key map | command owner | 不共享 live map | owner | 不需要 |
| record-id map | command owner | 不共享 live map | owner | 不需要 |
| active flat/HNSW | command owner | VSEARCH 也由 owner 执行 | owner publish/retire | 过渡期可保留 fallback |
| shadow index | rebuild worker 私有 | completion descriptor 交给 owner | 失败由 worker，成功由 owner | worker 私有无需 data lock |
| rebuild delta | command owner append | worker 读取冻结副本/batch | owner | handoff 可有内部锁 |
| RDB snapshot | command owner 创建 | 保存线程只读 | 保存线程 | completion 同步允许 |
| AOF state | AOF subsystem | owner 按 commit 顺序 append/handoff | AOF shutdown | 允许 |
| vector snapshot image | owner 或 private builder 创建 | writer 只读并写文件 | writer | completion/status 锁允许 |
| replication mutation | command owner 创建 | replication owner 只读 | backlog 安全回收 | immutable |
| replication backlog | replication subsystem | command owner 仅执行可靠 accept | replication subsystem | 允许 |
| slave apply task | replication receiver 创建 | command owner 接管 | owner/completion | ingress queue 可锁 |
| command task | network owner 创建 | prepare worker/command owner 接管 | executor 或 network | queue 可锁 |
| response task | command owner 创建 | network owner 发送 | network owner | queue 可锁 |

---

## 7. 后台任务统一约束

### 7.1 通用规则

后台任务必须遵循：

```text
owner 创建不可变输入或冻结视图
-> worker 只处理私有状态
-> worker 返回 completion
-> command owner 执行最终 publish/commit
```

后台线程不得通过持有数据库大锁长期读取 live state。

### 7.2 Async rebuild

目标模型：

```text
command owner
  -> 创建 immutable record baseline
  -> 冻结 rebuild generation
  -> 启用 owner-only delta append

rebuild worker
  -> 构建 private shadow
  -> replay 冻结 delta batch
  -> 返回 publish completion

command owner
  -> 校验 generation
  -> replay 最终 delta
  -> active/shadow 原子 publish
  -> retired 进入可控回收
```

强制约束：

```text
worker 不直接发布 active
worker 不直接修改 records/maps
worker 不分配 record_id
worker 不直接清理仍可能被使用的 retired
worker 不直接把 live active graph 当作稳定 snapshot 输入
```

### 7.3 RDB/AOF rewrite/full sync

必须由 command owner 创建一致的 immutable snapshot，再由其他线程执行文件或网络 I/O。

禁止后台线程直接遍历正在修改的 live database。

### 7.4 TTL

TTL scanner 只负责定时和投递任务。

TTL scan、expire 判定和删除最终由 command owner 执行。

### 7.5 Snapshot

snapshot writer 只处理稳定 snapshot image 或 worker-private graph。

禁止在后台线程中长时间持有 active index lock 后直接序列化 live active graph。

---

## 8. 主从同步约束

### 8.1 主节点

可靠向量复制的主路径：

```text
command owner prepare
-> 预留所有可能失败的本地资源
-> no-fail database commit
-> 分配 sequence
-> 发布 immutable mutation 到本地可靠 mutation stream
-> 通知 replication subsystem
-> 客户端成功
```

必须区分：

```text
manager/database commit
local reliable mutation stream accept
TCP transient send queue
slave ACK
AOF durability policy
```

强制规则：

```text
不等待 slave ACK
不等于允许本地 mutation 丢失

TCP send queue 不是可靠 backlog

replication thread 不读取 records/maps/index

replication mutex 不得嵌入 storage/vector commit 临界区

mutation 必须自包含，replication thread 只读 mutation
```

### 8.2 从节点

```text
replication receiver
-> 解码和校验
-> 创建 slave apply task
-> 投递 command owner
-> command owner 按 sequence 串行 apply
-> 成功后推进 applied_offset
-> 返回 completion 给 replication subsystem
```

禁止 replication 网络线程直接调用：

```text
storage set/delete
VSET/VDEL
vector manager commit
HNSW add/delete
AOF apply
```

### 8.3 Sequence

```text
sequence 必须由 command owner 在最终 commit 顺序中分配。
```

prepare worker、replication thread 和网络线程不得分配 sequence。

### 8.4 READY

READY/BLOCKED/FAILED 等复制状态由 replication subsystem 拥有。

涉及数据库可见性的状态切换，必须以 command owner apply/full-sync install completion 为依据。

---

## 9. VSET/VDEL 提交模型

### 9.1 PREPARE

所有可能失败的操作应尽可能在 PREPARE 完成：

```text
embedding prepare
record/string/embedding allocation
logical-key lookup
record_id 规划
record store capacity reserve
logical-key map capacity reserve
record-id map capacity reserve
HNSW capacity validation/reserve
flat capacity reserve
rebuild delta capacity reserve
future mutation stream capacity reserve
checksum/validation
```

PREPARE 阶段失败：

```text
不得修改 records/maps/index/delta
不得生成成功 sequence
不得发布 mutation
不得追加成功语义 AOF
不得返回客户端成功
```

### 9.2 COMMIT

COMMIT 必须尽量满足：

```text
由 command owner 独占执行
不进行新的内存分配
不执行 destructive reserve
不获取 replication connection/ACK mutex
不进行文件或网络 I/O
不发生常规可恢复失败
```

COMMIT 发布顺序必须在 Stage 1A 中通过源码和 fault injection 冻结。

### 9.3 POST-COMMIT

允许：

```text
AOF append/handoff
已预留 mutation slot 发布
replication wake/notify
dirty/statistics 更新
response 构造和投递
```

注意：

```text
POST-COMMIT 不代表所有操作都可任意失败。
```

未来可靠 mutation publish 必须在 commit 前完成容量预留，使 commit 后发布不可失败。

现有 AOF durability 语义单独处理，不得在 Replication Stage 1A 中顺便重构为完整事务日志。

---

## 10. 锁处理原则

### 10.1 当前不得立即删除

在 owner 契约和后台边界未完成前，不得直接删除：

```text
g_kv_data_rwlock fallback
active_index_rwlock
rebuild_worker_rwlock
AOF locks
command/response queue locks
allocator locks
replication locks
worker lifecycle locks
shutdown/control locks
```

### 10.2 锁删除前置条件

删除主状态 correctness lock 前必须同时满足：

```text
所有 runtime mutation 入口已进入 command owner
不存在未登记的第二 writer
后台 worker 只处理 private/immutable state
publish 只由 command owner 执行
startup/shutdown 例外已明确
TSAN 无主状态 data race
fault injection 无半提交
三种网络模式行为一致
```

### 10.3 当前已知 legacy exception

当前 async rebuild worker 仍直接：

```text
读取 live records
推进 rebuild context
发布 active/shadow
清理 retired
写 HNSW snapshot
```

在完成专门收敛前，应把它记录为：

```text
唯一已知 legacy second-publisher exception
```

约束是：

```text
禁止新增类似入口
禁止扩大其权限
暂时保留相关 correctness locks
后续通过 private build + owner publish 消除
```

---

## 11. 阶段顺序

### Stage O1：Command Owner Contract Freeze

目标：

```text
把单 owner 从文档结论变成运行期断言和可执行测试。
```

建议内容：

```text
owner thread identity API
debug/test owner assertion
runtime mutation 入口断言
startup/shutdown 显式例外
禁止新增第二 writer
当前 async rebuild legacy exception 登记
三网络模式/TTL/slave apply owner 测试
```

禁止：

```text
删除 correctness locks
重写队列
实现 replication wire
重构 HNSW 算法
```

### Replication Stage 1A

```text
HNSW 容量安全
VSET/VDEL prepare/commit/post-commit
records/maps/index 失败原子性
delta append 失败语义
allocator/index fault hooks
future mutation post-commit 扩展点
```

### Replication Stage 1B

```text
确定性 vector mutation
本地可靠 mutation stream accept
主节点 sequence
slave vector apply task
从节点不运行 ONNX
```

### Replication Stage 2

```text
full sync
base sequence
catch-up
READY/BLOCKED/FAILED
VSEARCH READY gate
```

### Main-Path Lock Stage 2

```text
删除普通 KV executor 内已证明冗余的数据库 wrapper/stats mutex
```

### Main-Path Lock Stage 3

```text
async rebuild private build
publish 返回 command owner
snapshot 输入稳定化
删除 vector client command 路径冗余 data locks
```

### Replication Stage 3

```text
offset
ACK
可靠 backlog
partial resync
reconnect
```

### Replication Stage 4 / Main-Path Lock Stage 4

```text
restart
promotion
recovery
shutdown
fallback 和残余锁收敛
```

---

## 12. 当前阶段硬边界

当前主线只围绕：

```text
Route C owner contract
Replication Stage 1A-4
随后逐段移除主状态锁
```

不得提前扩展到：

```text
并行读 executor
分片 executor
RCU/epoch/hazard pointer
HNSW 原地图并发读写
全面无锁队列
wait-free
eBPF 可靠复制
复制 HNSW graph
从节点重跑 ONNX
公开 V 命令或响应变化
RDB/AOF 格式变化
kvs_vector_index_ops_t 变化
100 万条 benchmark
```

---

## 13. 每阶段通用测试门禁

### Correctness

```text
Reactor/Proactor/NtyCo 进入同一 command owner
同连接 Pipeline 严格保序
多客户端 commit sequence 唯一、单调
TTL 与用户写命令按 owner 顺序线性化
embedding prepare 乱序完成不产生多 writer
VSET overwrite/VDEL 顺序确定
async rebuild publish 只由 owner 执行
slave apply 只由 owner 执行
shutdown 无 late submit/UAF
fault injection 无半提交
```

### Sanitizer

```text
TSAN
ASan
UBSan
```

### Profiling

```text
executor queue wait/depth
owner wrapper/stats lock wait
embedding prepare/HOL
AOF append/fsync
replication reliable accept
active/rebuild lock wait/hold
snapshot/rebuild 对 p95/p99 的阻塞
```

去锁优先依据正确性和所有权简化，不预设性能一定提升。

---

## 14. 通用 Codex 执行约束

后续 Codex 阶段 Prompt 应包含：

```text
开始前必须读取：

docs/replication/route_c_main_path_owner_constraints.md
docs/replication/route_c_main_path_lock_elimination_audit.md
docs/replication/replication_stage1a_preimplementation_audit.md
docs/development/kvstore_development_checklist.md

本文为统一架构约束。
如当前源码与本文冲突，先记录冲突，不得静默绕过。
```

所有阶段必须：

```text
重新读取当前源码
列出实际调用链和 owner
标记 PREPARE/COMMIT/POST-COMMIT
说明是否新增第二 writer
说明是否新增主路径锁
说明是否修改公开协议或持久化格式
运行与本阶段匹配的测试
执行 git diff --check
不得擅自扩大阶段范围
```

---

## 15. 后续 Prompt 最小模板

```text
项目目录：

/home/zhengpan/share/KVLite/9.1-kvstore

执行阶段：

<阶段名称>

开始前必须读取并遵守：

docs/replication/route_c_main_path_owner_constraints.md
docs/replication/route_c_main_path_lock_elimination_audit.md
docs/replication/replication_stage1a_preimplementation_audit.md
docs/development/kvstore_development_checklist.md

本阶段目标：

<只写本阶段新增目标>

允许修改：

<文件或模块范围>

禁止修改：

<阶段外范围>

完成门禁：

<测试、sanitizer、fault injection、flags>

不得提交 commit，除非另有明确指令。
```

后续 Prompt 不再重复本文中的 owner、线程、去锁、replication 和后台任务统一约束。

---

## 16. 通用阶段报告要求

每个阶段最终报告至少包含：

```text
1. 实际修改文件
2. 当前线程和 owner 变化
3. PREPARE/COMMIT/POST-COMMIT 边界
4. 新增或消除的第二 writer
5. 新增或删除的锁
6. failure atomicity
7. 对 replication sequence/order 的影响
8. 对 startup/shutdown 的影响
9. 测试、sanitizer 和 fault injection
10. git diff --check
11. 未完成事项
12. 最终 flags
```

---

## 17. 统一最终判断

```text
main_path_lock_elimination_goal =
  serial command execution + single database owner

replication_internal_lockfree_required = no
hnsw_internal_lockfree_required = no
parallel_vsearch_required = no
single_command_owner_required = yes
slave_apply_through_owner_required = yes
background_private_prepare_required = yes
owner_only_publish_required = yes
correctness_locks_removable_immediately = no
ownership_proof_before_lock_removal = yes
replication_stage1a_should_continue = yes
```

当前推荐顺序：

```text
冻结 command owner 契约
-> Replication Stage 1A
-> Replication Stage 1B
-> Replication Stage 2
-> 收敛 async rebuild publish
-> 分阶段删除普通 KV/vector 主状态冗余锁
-> Replication Stage 3/4 和 shutdown/fallback 收敛
```