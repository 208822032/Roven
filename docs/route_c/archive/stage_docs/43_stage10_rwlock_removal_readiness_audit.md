# Stage 10: RWLock Removal Readiness Audit

## 背景

本阶段对应 `10_rwlock_removal_criteria.md`，目标是评估当前路线 C 是否已经具备删除或弱化
`g_kv_data_rwlock` 的条件。本阶段只做审计和文档输出，不删除、不弱化、不替换全局读写锁。

当前路线 C 已完成的关键前置包括：

- single command executor 已接入 reactor/proactor 的普通异步命令路径。
- owner-aware response 回投、single-inflight、send_buf gating 已稳定。
- VSET/VSEARCH prepared data 已进入 `command_task` 生命周期。
- Stage 9 embedding prepare worker pool 已接入，worker 只执行 prepare，commit/search 仍回到 single executor。
- Stage 9.1 已补测 disconnect、shutdown、fd reuse、sanitizer 异常路径；未修改生产代码。

## 为什么本阶段不直接删除 `g_kv_data_rwlock`

`10_rwlock_removal_criteria.md` 的硬条件要求所有 live store/vector/TTL/AOF/RDB/replication 访问都归属到
single executor 或明确 snapshot owner。当前代码只满足普通 async client command 的部分收敛：

- reactor/proactor 普通命令会构造 `command_task` 并提交 single executor。
- embedding worker 不访问 store/HNSW/TTL/AOF/RDB/replication。
- 但 TTL active expire、RDB save、AOF rewrite、master full sync、slave apply、shutdown destroy 仍直接访问 live state。
- reactor/proactor 的 REPLSYNC/replication fallback 以及 ntyco 网络模式仍直接调用 `kvs_filter_protocol()`。

因此本阶段结论不能是 READY。

## 共享状态清单

| 状态 | 主要位置 | 当前保护/owner | 去锁风险 |
|---|---|---|---|
| 普通 KV store | `global_array/global_rbtree/global_hash/global_skiptable` | `g_kv_data_rwlock` + command executor 串行普通 async 命令 | 后台/复制/同步 fallback 仍会直接访问 |
| TTL metadata | 普通 KV 节点内 `expire_at_ms` | `g_kv_data_rwlock` | active expire 后台线程直接扫描/删除 |
| vector records | `g_vector_state.store` | V 命令经 executor + `g_kv_data_rwlock` | RDB/AOF rewrite 遍历，RDB load/reset 修改 |
| vector id map/question+scope map | `g_vector_state.id_map` | VSET/VDEL 在 executor 锁内修改 | restore/rebuild/load 路径仍不是 executor task |
| HNSW/current index | `g_vector_state.ann_index/current_index` | VSET/VSEARCH/VINDEX 在 executor 锁内访问 | RDB load/rebuild、VINDEX rebuild 仍依赖外层串行/锁 |
| flat/sq8 flat index | `g_vector_state.flat_index/sq8_flat_index` | VSET/VSEARCH/VINFO 在 executor 锁内访问 | rewrite/save/info 读路径依赖锁边界 |
| AOF append state | `aof_state` | AOF mutex/data_rwlock，append 由 post_commit 或 replication apply 调用 | append 顺序基本在 commit 后，但 slave apply 仍绕 executor |
| AOF rewrite state | `src/persistence/kvs_aof_rewrite.c` | `aof_state.data_rwlock` + `g_kv_data_rwlock` 读锁 | rewrite 线程/子进程仍遍历 live store |
| RDB save/load state | `src/persistence/kvs_rdb.c` | save 用 `g_kv_data_rwlock` 读锁；load 启动期直接修改 | 在线 save 不是 executor snapshot |
| replication state | `global_dist_config`、sync backend queue | slave list mutex + sync queue | full sync/ slave apply 仍直接访问 store |
| background task state | TTL thread、AOF fsync/rewrite thread、sync backend thread | 各自 mutex/flag + 全局锁 | 未统一进入 executor |
| stats state | executor/vector/worker stats | atomic builtin 或单线程更新 | 非去锁主要 blocker，但需继续 TSAN 覆盖 |

## `g_kv_data_rwlock` 使用点清单

本次源码搜索范围为 `src/` 与 `include/`。当前共有 23 个源码级引用点，其中 18 个是 lock/unlock 操作点，
5 个是声明、定义、初始化或销毁点。

| 文件:行 | 函数/位置 | 行为 | 锁类型 | 保护对象 | 调用线程 | 是否在 executor 内 | 是否可未来删除 | 删除前置条件 |
|---|---|---|---|---|---|---|---|---|
| `include/kvs_globals.h:11` | extern | 声明全局锁 | - | 全局共享状态 | 全部 | - | 最后删除 | 所有引用消失 |
| `src/core/kvstore.c:18` | global | 定义全局锁 | - | 全局共享状态 | 全部 | - | 最后删除 | 所有引用消失 |
| `src/core/kvstore.c:240` | `main` | 初始化锁 | init | 全局共享状态 | main | 否 | 可删 | 不再需要全局锁 |
| `src/core/kvstore.c:244` | init failure | 销毁锁 | destroy | 全局共享状态 | main | 否 | 可删 | 不再需要全局锁 |
| `src/core/kvstore.c:332` | shutdown | 销毁锁 | destroy | 全局共享状态 | main | 否 | 可删 | 不再需要全局锁 |
| `src/core/kvs_command_task.c:192` | `kvs_command_task_destroy` | 异常兜底释放 task 持有的锁 | unlock | command handler 状态访问 | executor 或同步 handler | 部分 | 可删 | executor 内不再持锁且错误路径无残留锁 |
| `src/core/kvs_command_task.c:438` | `kvs_command_task_lock` | 读命令加锁 | rdlock | KV/vector/HNSW/TTL 读路径 | executor 或同步 handler | 部分 | 最后删 | 所有读写状态访问无并发破坏 |
| `src/core/kvs_command_task.c:449` | `kvs_command_task_lock` | 写命令加锁 | wrlock | KV/vector/HNSW/TTL 写路径 | executor 或同步 handler | 部分 | 最后删 | 所有写路径收敛到 single executor |
| `src/core/kvs_command_task.c:475` | `kvs_command_task_unlock` | 释放 command 锁 | unlock | command handler 状态访问 | executor 或同步 handler | 部分 | 最后删 | 同上 |
| `src/core/kvs_command_task.c:542` | `kvs_command_task_execute_sync` | GET/EXIST lazy expire 从读锁升级写锁 | wrlock | TTL lazy expire 删除 | executor 或同步 handler | 部分 | 最后删 | lazy expire 只在 executor 内且无同步 fallback |
| `src/core/kvs_ttl_background.c:199` | `ttl_scan_once` | active expire 扫描/删除加写锁 | wrlock | 普通 KV + TTL metadata | TTL background thread | 否 | 不能删 | TTL active expire executor 化 |
| `src/core/kvs_ttl_background.c:218` | `ttl_scan_once` | 释放 active expire 锁 | unlock | 普通 KV + TTL metadata | TTL background thread | 否 | 不能删 | TTL active expire executor 化 |
| `src/core/kvstore.c:126` | `dest_kvengine` | destroy 前加写锁 | wrlock | KV store + vector state | main shutdown | 否 | 暂不可删 | shutdown 顺序证明 executor/background/replication 全部 drain |
| `src/core/kvstore.c:143` | `dest_kvengine` | destroy 后释放锁 | unlock | KV store + vector state | main shutdown | 否 | 暂不可删 | 同上 |
| `src/distributed/distributed.c:133` | `slave_sync_thread` | master full sync 遍历 live store 前加读锁 | rdlock | 普通 KV + TTL metadata | per-slave sync thread | 否 | 不能删 | full sync 改为 executor snapshot_task |
| `src/distributed/distributed.c:196` | `slave_sync_thread` | full sync 释放读锁 | unlock | 普通 KV + TTL metadata | per-slave sync thread | 否 | 不能删 | full sync 改为 snapshot |
| `src/distributed/distributed.c:567` | `slave_connect_master` | slave apply 每条复制消息加写锁 | wrlock | 普通 KV + TTL metadata | slave sync thread | 否 | 不能删 | replicated_command_task 进入 executor |
| `src/distributed/distributed.c:664` | `slave_connect_master` | slave apply 释放写锁 | unlock | 普通 KV + TTL metadata | slave sync thread | 否 | 不能删 | replicated_command_task 进入 executor |
| `src/persistence/kvs_aof_rewrite.c:690` | `aof_rewrite_acquire_store_read_lock` | rewrite live traversal 加读锁 | rdlock | KV store + vector store | AOF rewrite thread/parent | 否 | 不能删 | AOF rewrite 改为 executor snapshot |
| `src/persistence/kvs_aof_rewrite.c:699` | `aof_rewrite_release_store_read_lock` | rewrite 释放读锁 | unlock | KV store + vector store | AOF rewrite thread/parent | 否 | 不能删 | AOF rewrite 改为 snapshot |
| `src/persistence/kvs_aof_rewrite.c:883` | fork child | 子进程释放继承的全局读锁 | unlock | fork/COW snapshot 边界 | AOF rewrite child | 否 | 不能删 | 不再 fork live store snapshot |
| `src/persistence/kvs_rdb.c:521` | `kvs_rdb_save` | save live traversal 加读锁 | rdlock | KV store + vector store | main shutdown 或命令触发路径 | 否 | 不能删 | RDB save 改为 executor snapshot_task |
| `src/persistence/kvs_rdb.c:529` | `kvs_rdb_save` | save 释放读锁 | unlock | KV store + vector store | main shutdown 或命令触发路径 | 否 | 不能删 | RDB save 改为 snapshot |

## 访问路径分类

### 1. 已收敛到 single command executor 的路径

- reactor 普通命令：`reactor.c` 构造 heap `command_task`，复制 argv，提交 `kvs_command_executor_submit()`。
- proactor 普通命令：`proactor.c` 同样构造 task 并提交 executor。
- VSET/VSEARCH worker pool enabled 时：executor 只把需要 prepare 的 task 投递 worker；worker 调用
  `kvs_command_task_prepare()`；prepared task 回到 executor 后执行 `execute_sync/post_commit/finalize`。
- AOF append 和 `master_sync_command()` 对普通 client 写命令在 `kvs_command_task_post_commit()` 中执行，顺序跟随 executor commit。

### 2. 仍由 network poller 或网络线程直接访问的路径

- reactor/proactor 的 REPLSYNC 或已标记 replication connection 会走 `net_session_process_resp()` 和同步 handler。
- proactor async task 分配/构建失败时会 fallback 到 `p_process_received_data()` 同步 handler。
- ntyco 模式仍通过 `net_session_process_resp()` 直接调用 `kvs_handler()`，没有纳入 single command executor。
- 这些同步路径最终进入 `kvs_filter_protocol()`，仍依赖 `kvs_command_task_execute_sync()` 内部的全局锁。

### 3. 仍由 TTL/background thread 访问的路径

- `kvs_ttl_background_start()` 创建 TTL scan thread。
- `ttl_scan_once()` 在线程中直接扫描普通 KV，删除过期 key，并在 master TCP sync 模式下调用
  `master_sync_command("DEL", ...)`。
- 该路径不是 executor task，是删除/弱化 `g_kv_data_rwlock` 的 blocker。

### 4. 仍由 AOF/RDB/persistence thread 访问的路径

- AOF fsync thread 只处理 AOF 文件状态，不读 store；它不是去锁 blocker。
- AOF rewrite thread/fork child 仍遍历 live KV/vector store，并用 `g_kv_data_rwlock` 读锁保护。
- RDB save 仍直接遍历 live KV/vector store，并用 `g_kv_data_rwlock` 读锁保护。
- RDB load 和 AOF load 在启动期网络未启动时直接修改 store，当前可接受；若未来支持在线 replay，必须变成 executor task。

### 5. 仍由 replication/sync thread 访问的路径

- master full sync per-slave thread 直接遍历 live KV store 并发送数据。
- slave apply thread 解码复制消息后直接调用底层 set/del/mod 和 TTL apply。
- TCP sync backend worker 主要发送已编码消息，不读写 store；但它依赖 master/slave 直接访问路径产生的数据。

### 6. 测试代码或工具路径

- vector benchmark/testcase 会通过 RESP 或测试入口验证语义，不作为生产 owner。
- eBPF sync daemon 是独立同步路径，当前未纳入本次去锁实现范围；若启用并直接影响 store apply，也需要单独审计。

### 7. 不确定路径

- `FULLDUMP` 命令在命令表中是写锁，实际遍历普通 KV 生成响应；如果作为在线 snapshot 能力使用，未来应纳入 snapshot_task。
- `VINDEX rebuild` 当前是写锁命令，在 executor 内运行；去锁前需要确认所有 rebuild 调用只来自 executor 或启动期。

## Store 访问路径审计

普通 KV handler (`SET/GET/DEL/MOD/EXIST/RANGE/SORT/FULLDUMP` 等) 通过 command table 的 lock mode 进入
`kvs_command_task_execute_sync()`。在 reactor/proactor 普通 async 路径中，handler 已由 single executor 调用。

但 store 写路径没有全部收敛：

- TTL active expire 线程直接删除 key。
- slave apply 线程直接 set/del/mod/TTL apply。
- RDB load 启动期直接重置和加载 store。
- shutdown destroy 直接释放 store。
- ntyco 和 replication fallback 可以在网络线程同步执行 handler。

结论：普通 client async command 部分满足，但全局 store owner 仍不是 single executor。

## TTL 访问路径审计

TTL 有两类路径：

- lazy expire：`GET/EXIST` 等 read path 先在读锁下检测，遇到过期返回 `KVS_CMD_RETRY_WITH_WRITE_LOCK`，再加写锁删除。
- active expire：TTL background thread 每秒扫描普通 KV，并在写锁内删除过期 key。

当前 active expire 仍是直接后台写路径，不是 executor task。TTL 删除只影响普通 KV，不直接删除 vector/id_map/HNSW；但它与普通 KV 写、RDB/AOF rewrite、slave apply、shutdown 仍有并发风险，必须保留全局锁。

结论：TTL 是 Stage 10 去锁 blocker。

## Vector / HNSW 访问路径审计

VSET/VSEARCH/VDEL/VGET/VINFO/VINDEX 在命令表中分别使用读/写锁。当前正常 async client 路径由 executor 串行调用：

- VSET prepare 可在 worker 执行；commit 由 executor 调用 `kvs_vector_set_commit_prepared()`，修改 store/id_map/flat/HNSW。
- VSEARCH prepare 可在 worker 执行；search 由 executor 调用 `kvs_vector_search_with_prepared()`，读取 index/store。
- VDEL 由 executor 调用 `kvs_vector_del()`，标记 record deleted 并更新 id_map/store counters。
- VINFO 在读锁内读取 `g_vector_state`。
- embedding worker 只调用 `kvs_command_task_prepare()`，不访问 vector mutable state。

仍需保留锁的原因：

- RDB save/AOF rewrite 会读取 vector live store。
- RDB load/reset/rebuild 在启动期直接修改 vector state。
- ntyco 和同步 fallback 仍可能同步调用 V 命令 handler。

结论：V 命令 normal async path 部分满足，vector/HNSW 全局 owner 仍未完全收敛。

## AOF/RDB/Replication 访问路径审计

- AOF append：普通 client 写命令已在 executor `post_commit` 中执行；slave apply 后的 AOF append 仍来自 replication thread。
- AOF rewrite：仍由 rewrite thread/fork path 读取 live KV/vector store，依赖 `g_kv_data_rwlock` 读锁。
- RDB save：仍直接遍历 live KV/vector store，依赖 `g_kv_data_rwlock` 读锁；shutdown 中 save 发生在 TTL stop 前。
- RDB load：当前在网络启动前执行，可以作为启动期单线程路径保留。
- master full sync：sync thread 直接遍历 live store，依赖读锁。
- slave apply：sync thread 直接修改 live store，依赖写锁。
- `master_sync_command()`：普通 command post_commit 中调用是 executor-owned；TTL active expire 和 master full sync 中调用不是 executor-owned。

结论：persistence/replication 尚未满足 rwlock removal criteria。

## Network Poller 结论

reactor/proactor 普通 client 命令已经主要通过 executor；但 network poller/网络线程仍有直接共享状态访问入口：

- reactor/proactor replication fallback 同步执行 handler。
- proactor async 构建失败 fallback 同步执行 handler。
- ntyco 模式未纳入 executor，仍同步执行 handler。

结论：仍存在 network-side 绕过 executor 的 handler 路径。

## Background Thread 结论

当前仍存在多个后台线程直接或间接接触 live state：

- TTL scan thread：直接扫描和删除普通 KV。
- AOF rewrite thread/child：遍历 live KV/vector store。
- distributed master full sync thread：遍历 live KV store。
- distributed slave apply thread：直接修改 live KV/TTL metadata。
- AOF fsync thread、TCP sync backend worker 不直接读写 store，但仍属于 shutdown/ordering 审计范围。

结论：background thread 仍访问共享状态，是去锁 blocker。

## RWLock Removal Criteria 对照

| 条件 | 当前状态 | 结论 |
|---|---|---|
| 普通 KV handler 不再被网络线程直接调用 | reactor/proactor 普通路径满足；ntyco/replication/fallback 不满足 | 未满足 |
| Vector handler 不再被网络线程直接调用 | reactor/proactor 普通路径满足；ntyco/replication/fallback 不满足 | 未满足 |
| TTL active expire 不再直接访问 store | 仍直接访问 | 未满足 |
| lazy expire 只在 executor 内完成 | 普通 async 满足；同步 fallback 不满足 | 未满足 |
| RDB save 不直接遍历 live store | 仍直接遍历，读锁保护 | 未满足 |
| AOF rewrite 不直接遍历 live store | 仍直接遍历，读锁保护 | 未满足 |
| master full sync 不直接遍历 live store | 仍直接遍历，读锁保护 | 未满足 |
| AOF append 顺序由 executor 产生 | 普通 client 写满足；slave apply 不满足 | 部分满足 |
| slave apply 通过 `replicated_command_task` | 仍由 sync thread 直接 apply | 未满足 |
| shutdown/destroy 在 executor drain 后执行 | Stage 9 已 drain worker/executor，但 shutdown save/TTL stop/destroy 仍分散 | 部分满足 |
| vector store/id_map/HNSW 所有访问有明确 owner | V 命令普通 async 满足；persistence/load/fallback 未满足 | 部分满足 |
| TSAN 或等价竞态检测通过 | Stage 9.1 无 worker 新竞态，但有既有 shutdown/background race | 部分满足 |

## BLOCKER 列表

1. TTL active expire 仍由 `src/core/kvs_ttl_background.c` 后台线程直接扫描和删除普通 KV。
2. master full sync 仍由 `src/distributed/distributed.c::slave_sync_thread` 直接遍历 live KV store。
3. slave apply 仍由 `src/distributed/distributed.c::slave_connect_master` 直接修改普通 KV 和 TTL metadata。
4. AOF rewrite 仍由 `src/persistence/kvs_aof_rewrite.c` 线程/fork path 直接遍历 live KV/vector store。
5. RDB save 仍由 `src/persistence/kvs_rdb.c::kvs_rdb_save` 直接遍历 live KV/vector store。
6. ntyco 模式和 reactor/proactor replication/fallback 路径仍可同步调用 `kvs_filter_protocol()`，没有全部经 executor。
7. shutdown 顺序仍包含 RDB save、TTL stop、destroy 等直接 live state 操作，尚未统一为 executor-drained snapshot/destroy 边界。

## Non-Blocking Issue 列表

1. AOF fsync thread 不读 store，但 shutdown/stop 标志已有 TSAN 背景告警，需要在后续 shutdown hardening 中处理。
2. vector VINFO/stats 在读锁内读取 `g_vector_state`，本身不是 blocker；去锁前需保证只在 executor 或 immutable snapshot 中读取。
3. RDB/AOF load 当前是启动期路径，可暂时保留；若未来支持在线 replay，必须封装为 executor task。
4. Stage 9.1 已知 LSAN shutdown leaks 和 TSAN background races 不属于 worker pool 新问题，但会影响最终去锁验收。
5. `FULLDUMP`/snapshot-like 命令当前依赖写锁和同步响应缓冲，未来若作为在线 snapshot，应进入 snapshot_task 设计。

## 审计结论

结论：NOT_READY。

当前 architecture 只对 reactor/proactor 普通 client command 达到“commit 串行”的部分目标。所有 store/vector/TTL/AOF/RDB/replication 写路径尚未全部收敛到 single executor 或 snapshot owner。存在多个绕过 executor 的后台/复制写路径，因此不能删除或弱化 `g_kv_data_rwlock`。

## 下一阶段建议

不建议进入删除/弱化 `g_kv_data_rwlock` 的实现阶段。建议下一阶段继续做 shared state ownership 收敛，优先顺序：

1. TTL active expire executor task 化，过期 DEL 的 AOF/replication 顺序跟随 commit 流。
2. slave apply 封装为 `replicated_command_task`，由 single executor 串行 apply。
3. master full sync 改为 executor snapshot_task，sync thread 只发送 snapshot。
4. RDB save 和 AOF rewrite 改为 executor snapshot_task + IO thread 写 snapshot。
5. 收口 ntyco 与 replication/fallback 同步 handler，保证所有在线 handler 进入 executor。
6. 重新跑 correctness、long-run、ASAN/UBSAN/LSAN、TSAN，再评估 PARTIAL_READY/READY。
