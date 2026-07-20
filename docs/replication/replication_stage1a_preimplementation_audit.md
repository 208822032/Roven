# Replication Stage 1A 前置审计

## 1. 结论与基线

本次为只读实现审计。生产代码、测试、Makefile、配置、公开协议和持久化格式均未修改。

开始状态：

```text
date = 2026-07-20
HEAD = 7cd837f 修改：统一配置文件
worktree = clean
git diff --check = PASS
Unified Configuration Stage 0-4 = 已形成提交边界
```

核心结论：

1. 普通 KV TCP 复制是“连接内、无 offset 的全量命令快照 + 增量命令队列”，不是 RDB 传输，也没有 ACK、backlog、partial resync、自动重连或 READY 状态。
2. `VSET/VDEL` 虽然属于 write command，会经过通用 post-commit，但 `master_sync_command()` 没有 vector op，且通用三参数载荷无法表达 scope、embedding、record ID 等信息，因此实际不复制。
3. 当前 `core_hnsw_v2` 存在确定性的 destructive reserve：对非空索引扩大 capacity 时，成功返回会清空全部旧图。
4. 当前新增、v2 覆盖更新和删除均未达到完整失败原子性；尤其 v2 覆盖更新和 VDEL 会在仍可能失败的步骤之前修改 record/index。
5. Stage 1A 应采用局部的 lossless allocate-copy-publish 扩容，并把 VSET/VDEL 改成“完整准备和容量预留 -> 无失败 commit -> 派生 post-commit”。不需要图布局或 HNSW 算法重构。
6. Stage 1A 完成门禁后可以直接进入 Replication Stage 1 的向量增量复制；公开协议、RDB/AOF 格式和 `kvs_vector_index_ops_t` 均无需修改。

## 2. 审计范围与证据

主要源码：

```text
src/core/kvs_command_task.c
src/core/kvs_command_executor.c
src/distributed/distributed.c
src/distributed/sync_backend_tcp.c
src/utils/msg_queue.c
src/persistence/kvs_aof.c
src/persistence/kvs_rdb.c
src/vector/kvs_vector.c
src/vector/kvs_vector_index.c
src/vector/kvs_vector_rebuild.c
src/vector/kvs_vector_rebuild_shadow.c
src/vector/core/flat/*
src/vector/core/hnsw_v2/*
src/vector/core/hnsw_v2_snapshot/*
```

现行测试和报告用于确认已有证据边界；本审计没有把“代码分支存在”升级为“测试已验证”。重点检查了 TCP TTL replication、vector persistence/rebuild/snapshot、HNSW skeleton/graph/product tests，以及 Stage 0、Configuration Stage 3/4 和现有产品化报告。

证据分类：

```text
T = 当前可执行测试直接覆盖
B = 只有代码分支或正常路径测试
F = 缺少确定性 fault hook
H = 失败后存在半提交风险
```

## 3. 普通 KV TCP 复制现状

### 3.1 增量调用链

```text
客户端 RESP
-> kvs_command_task_build()
-> kvs_command_task_prepare()
-> kvs_command_task_execute_sync()
   -> g_kv_data_rwlock write lock
   -> storage handler
   -> command_committed = true
   -> capture post state
   -> unlock
-> kvs_command_task_post_commit()
   -> aof_append()
   -> master_sync_command()
   -> ms_encode_message()
   -> sync_backend_broadcast_owned()
   -> g_tcp_queue
-> TCP backend worker
-> slave receive/decode
-> kvs_command_executor_submit_replicated()
-> replica executor apply under write lock
-> replica AOF（sync_context 阻止再次向下游传播）
-> response finalize/send
```

关键点位：`kvs_command_task_execute_sync()` 和 `kvs_command_task_post_commit()` 位于 `src/core/kvs_command_task.c:599/643`；response 在 post-commit 之后才 finalize/push，见 `src/core/kvs_command_executor.c:2793-2848`。

当前 mapper 覆盖 SET/RSET/HSET/SSET、对应 MOD/DEL、PAPERSET/PAPERMOD/PAPERDEL、PEXPIREAT 和 PERSIST。EXPIRE 在 post-commit 规范化为绝对 `PEXPIREAT`，已过期时传播 DEL；惰性和后台过期另有 DEL propagation hook。四 backend 的普通写入、覆盖、删除、TTL 和新 slave 全量数据由 `tests/ttl/test_ttl_tcp_replication.sh` 覆盖。

### 3.2 全量、线程、队列和缺失语义

新 TCP slave 连接后，master executor 在读锁下复制普通 KV 到 `kvs_full_sync_snapshot_t`，再逐条编码成 SET/RSET/HSET/SSET 和可选 PEXPIREAT。它不是 RDB，也不包含 vector records。发送完成后才进入该连接的在线接收循环。

master 侧包含接入/每 slave 线程和独立 TCP backend 发送线程；slave 侧连接线程解码后提交到单 command executor。`g_tcp_queue` 是容量固定的进程内发送队列，只保存待发消息，不是可定位历史的复制积压日志。

当前没有：

```text
global replication offset
replica applied offset
ACK
backlog range
partial resync
slave automatic reconnect loop
SYNCING/CATCHING_UP/READY
断线缺口检测与补发
```

Configuration Stage 4 已使 master/slave 只从 effective `g_cfg` 读取 transport、master endpoint 和 split sync ports；TCP 与 `experimental_mirror` 互斥，observe 不产生复制写入。

## 4. 向量事务调用链

### 4.1 VSET prepare

```text
kvs_command_task_prepare()
-> kvs_vector_set_prepare()
   -> duplicate question/answer/scope
   -> generate or copy float32 embedding
-> prepared task queue
```

该阶段在产品写锁外完成 tokenizer/ONNX。失败不会修改 record、map 或 index。`normalized_question` 的额外副本仍在 commit helper 中分配，不是完整 prepare。

### 4.2 新增 VSET 的当前顺序

```text
PREPARE: question/answer/scope/embedding
WRITE LOCK
-> recheck question+scope
-> kvs_vector_find_free_slot()
   -> pop deleted slot，或 next_slot++（必要时先扩 store）
-> kvs_vector_record_take_prepared_fields()
   -> allocate normalized copy
   -> destroy old slot contents
   -> next_id++，发布 ACTIVE record
-> logical question+scope map put
-> record_id map put
-> kvs_vector_index_add_current()
   -> v2 ensure capacity -> add
-> active/deleted/total counters
-> generation++
-> capture rebuild upsert delta（返回值忽略）
COMMIT RESULT
UNLOCK
POST-COMMIT: textual AOF -> generic replication hook -> response
```

当前真正的成功 commit point 是 current index add、maps、record 和 counters 全部完成后，即 `kvs_vector_set_commit_prepared()` 返回 OK。问题是此前步骤已经逐步发布，失败回滚不完整：slot/free-list、`next_slot`、`next_id` 和 HNSW 邻接修改没有统一 rollback。

### 4.3 覆盖 VSET

非 v2 backend 保持历史语义：同 question+scope 只先分配新 answer，再替换旧 answer；record ID、embedding 和 index 不变。该局部字符串替换是 allocate-before-free。

`core_hnsw_v2` 使用不同路径 `kvs_vector_core_hnsw_v2_update_existing_record()`：

```text
mark_delete(old_record_id) in active HNSW
-> replace entire manager record and allocate new record_id
-> remove old record_id map
-> put new record_id map
-> ensure HNSW capacity
-> add new record_id/vector
-> generation++
-> capture upsert delta（ignored）
```

这是明确的 H 风险：从第一次 mark-delete 起即不可按当前代码回滚。后续 normalized copy、map reserve、HNSW reserve/add 任一步失败，都可能留下旧 record 与旧 index 状态不一致，或新 record 已发布但新 index node 缺失。

### 4.4 VDEL

```text
WRITE/REBUILD GUARD
-> find active record
-> active index write lock
-> mark manager record DELETED
-> mark index node deleted（not-found/error 只置 needs_rebuild）
-> unlock active index
-> remove logical map
-> remove record_id map
-> free-list push（可能分配并失败）
-> update active/deleted/total counters
-> generation++
-> capture delete delta（ignored）
-> POST-COMMIT AOF / generic replication
```

VDEL 在 free-list push 之前已修改 record、index 和 maps。free-list allocation 或异常 map 分支失败时，handler 返回失败但状态已经部分改变，属于 H/F。

### 4.5 TTL、恢复与 rebuild

- 公开 V 族命令当前没有设置 vector TTL 的入口；新 record 的 `expire_at_ms` 固定为 no-expire。RDB 保留该字段，但普通 KV TTL scanner 不驱动 vector VDEL。因此当前不存在“TTL 导致向量删除”的生产调用链。
- RDB 按原值恢复 question/answer/scope/normalized text/embedding/record ID/timestamps/generation，重建 manager maps，然后加载有效 snapshot 或从 active records 重建 index，不运行 ONNX。
- AOF 仍保存公开文本 VSET/VDEL；VSET replay 经过正常 prepare，因此 `local_c` 会重新运行 tokenizer/ONNX。Stage 1A 不修改这一既有格式。
- 同步 rebuild 先 reset active HNSW，再按 active count reserve 和逐条 add；它主动清空图，因此此处 reserve 的 destructive 语义不是额外数据丢失，但任一 add 失败会留下 needs_rebuild 的部分派生图。
- async rebuild 创建独立 shadow，预留至少 `2 * store.capacity`，baseline 后重放 delta，最终 checksum/search validate 后 swap。delta apply 失败使 shadow FAILED，旧 active index 保留。
- 产品写在 async rebuild 期间仍先更新 active record/index，再 append delta；append 失败只将 rebuild 标记 FAILED，不回滚已成功的产品写。

## 5. HNSW capacity 审计

### 5.1 初始容量和自动扩容

`vector_hnsw_initial_capacity` 同时进入 manager store 初次 reserve 和 active v2 index 初次 reserve（`src/vector/kvs_vector.c:815, 1967-1978`）。active add 在 `size == capacity` 时调用 `kvs_vector_core_hnsw_v2_ensure_add_capacity()`，目标通常为两倍容量。

### 5.2 destructive reserve 的确定证据

`kvs_hnsw_v2_index_reserve()`（`src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c:331-379`）执行：

```text
allocate completely empty storage(new_capacity)
-> on allocation failure: free temporary storage, return error
-> on success: free old index storage
-> install new empty arrays/map/context
-> size = 0
-> deleted_count = 0
-> entry_point/has_entry/max_level = empty
-> reset RNG/visit state
```

它没有复制 nodes、vectors、edges、deleted flags、record ID/slot map、entry point、max level 或 graph/build state。因此：

| 情况 | 结果 | 风险 |
| --- | --- | --- |
| 空图初次 reserve | 正常 | 低 |
| 非空图，new allocation 失败 | 旧图保持完整 | 中；写失败路径仍需 manager 回滚 |
| 非空图，new allocation 成功 | 旧图全部丢失，reserve 却返回 OK | 严重 |
| active add 跨 capacity | 清空旧图后只加入当前新 record，命令可返回成功 | 严重 |
| snapshot load 后 size == capacity 再写 | 下一次写触发同一清图 | 严重 |

snapshot deserialize 先在临时空 index 中按 header capacity reserve，再读 payload，最后替换 active，因此加载本身是 transactional；风险发生在加载后的后续增长。

### 5.3 add 失败行为

`kvs_hnsw_v2_index_add()` 会先写 pending slot 和 ID map，再搜索/连接多层图。初始 workspace allocation 或 ID map put 失败时 pending slot 可清理；但连接过程会逐层改写新节点和旧邻居 adjacency。若后续层返回错误，cleanup 只清 pending vector/ID map，不恢复已写入的 forward/reverse edge、reverse prune 结果或 RNG。现有 reverse-edge 错误还被 connect layer忽略，缺少可注入、可断言的 add transaction boundary。

### 5.4 shadow、slot reuse 和 100K 为何未暴露

- shadow 初次 reserve 发生在空图，且留出至少 2 倍 store capacity；正常测试 delta 不跨该边界。shadow replay 本身不自动 reserve，容量不足会令 shadow FAILED 并保留 active。
- manager slot reuse 不复用 record ID，但 v2 graph append 新 slot；tombstone 多时 `size` 仍增长，因此 records 有 free slot 不代表 HNSW 有 capacity。
- B-scale-7 正式 100K load 把 initial capacity 直接设为 100000，恰好没有跨扩容点。现有 skeleton 的 larger reserve 也发生在 reset/空图后；graph test 只验证满容量拒绝。它们均未证明非空 reserve 保留图。

风险等级：active non-empty reserve 为 P0 correctness blocker；add rollback、v2 overwrite 和 VDEL 半提交为进入复制 apply 前的 P0 correctness blocker。

## 6. 失败原子性矩阵

| 失败点 | 当前返回/状态 | records | maps | index | delta/AOF | 回滚与证据 | 风险 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| prepared question/answer/scope/embedding allocation | VSET error | 不变 | 不变 | 不变 | 无 | prepare cleanup；B，缺逐点 fault | 低/F |
| commit normalized copy allocation | VSET error | 新增通常未发布；deleted slot 可能被取走且计数被改；v2 overwrite 的旧 index 已 delete | 未变 | overwrite 已变 | 无 | 无完整 rollback | 高/F/H |
| record store records allocation | error | 不变 | 不变 | 不变 | 无 | temp allocation | 低/F |
| store growth 后 record_id map allocation | error | records 数组已换址且 capacity 已发布，数据仍在 | 旧 map 保留 | 不变 | 无 | 可重试但不是原子 reserve | 中/F |
| logical key map grow/put | error | candidate 已写，随后 destroy；slot/id 消耗不恢复 | grow alloc 失败旧表保留 | 不变 | 无 | 部分 cleanup | 中/F/H |
| manager record_id map grow/put | error | candidate destroy；slot/id 消耗不恢复 | logical map remove | 不变 | 无 | 部分 cleanup | 中/F/H |
| HNSW reserve allocation failure | VSET error | 新增 cleanup；v2 overwrite 已替换 | 新增 cleanup；overwrite 可能已换 | 旧图完整但 overwrite old node deleted | 无 | reserve 本身保旧，事务不保旧 | 高/F/H |
| HNSW reserve success on non-empty graph | VSET 可成功 | 全部 records 保留 | manager maps 保留 | 旧图清空 | delta/AOF 可记录成功写 | 无保护；代码确定 | 严重/B/H |
| HNSW add workspace/ID map failure before edge mutation | error | caller 部分 cleanup | caller 部分 cleanup | pending slot大致清理，RNG/diag 可变 | 无 | 无 fault hook | 中/F |
| HNSW add/link failure after edge mutation | error 或内部忽略 | caller cleanup/overwrite 已变 | caller cleanup/overwrite 已变 | adjacency/prune 无 rollback | 无 | 无 fault hook | 严重/F/H |
| flat reserve/add failure | error | caller cleanup但 slot/id 可消耗 | caller cleanup | reserve allocate-copy；pending slot cleanup | 无 | 正常路径测试，无 fault hook | 中/F |
| VDEL free-list allocation | VDEL error | 已标 DELETED，计数可能仍 active | 两 map 已删除 | 已标 delete | 无 | 无 rollback | 严重/F/H |
| rebuild delta vector/log allocation或上限 | 产品命令仍成功，rebuild -> FAILED | active write 已提交 | 已提交 | active 已提交，shadow 废弃 | delta 缺失；AOF继续 | 设计上 fail shadow，不回滚 active；无 allocator fault | 中/F |
| AOF append/write/fsync failure | API 为 void，handler success 不变 | 已提交 | 已提交 | 已提交 | AOF 可能缺 mutation | post-commit，不可回滚；无错误上送 | 高/F/H，Stage 1A 外 |
| 当前 generic TCP enqueue allocation/full | 返回 -1但 post-commit 忽略 | 已提交 | 已提交 | 已提交 | mutation 丢失 | 普通 KV 既有行为；无 vector op | 高/F/H |
| future vector enqueue failure | 尚未实现 | 待冻结 | 待冻结 | 待冻结 | 不得静默丢失 | Stage 1B 使用本报告语义 | blocker |

## 7. Stage 1A 冻结方案

### 7.1 HNSW 扩容选择

选择 **A：同一 index 对象内的 lossless allocate-copy-publish growth**。

实现要求：

1. 分配完整新 storage；任何分配失败都销毁 temporary，旧 index byte-for-byte 保持。
2. 复制 vectors、record IDs、levels、deleted flags、level0/upper counts 和 neighbors；重建或复制 ID/slot map。
3. 保留 size、deleted count、entry point、has_entry、max level、RNG、参数和需要保留的 stats；visited tags/context scratch 可安全清零重建。
4. 全部复制和校验成功后，在 active index write lock 下单点发布新 storage，再释放旧 storage。
5. 对 shadow/空图、snapshot-loaded index 和 direct index tests 使用同一函数，不新增第二套增长逻辑。

选择理由：当前存储已经是多组 capacity-sized arrays，现 reserve 也已有 old+new 峰值；在此函数内补复制和最终 swap 是最小局部修复。固定容量拒绝虽然实现更小，但会把已存在的自动增长产品语义变成硬容量上限，并使 tombstone-heavy workload 和 replica apply 频繁停机。每次写整图 copy-and-swap 则峰值和写放大不可接受。

### 7.2 VSET/VDEL 事务顺序

冻结目标：

```text
PREPARE（锁外）
  all strings + normalized text + embedding + candidate record
LOCKED PREFLIGHT
  recheck logical key
  choose slot/record_id without consuming them
  reserve store/maps/free-list/HNSW/delta capacity
  reserve all HNSW add scratch and rollback journal
INDEX PREPARE/COMMIT
  no fallible allocation after first adjacency mutation
  add failure restores graph/RNG/ID map exactly
MANAGER COMMIT POINT
  publish record + logical map + record_id map
  consume slot/next_id
  publish counters/generation
  overwrite: retire old record/node only in same no-fail commit
DERIVED POST-COMMIT
  append rebuild delta; failure fails/discards shadow, not active product state
UNLOCK
COMMAND POST-COMMIT
  textual AOF（保持现状）
  future deterministic replication mutation enqueue
  response finalize
```

具体不变量：

- 新增失败不得改变 `next_slot/next_id`、free-list、records/maps/index checksum。
- overwrite 失败必须保留旧 answer、record ID、embedding、maps 和旧 node 可查询；不得只设置 needs_rebuild 后返回。
- VDEL 在 commit 前预留 free-list，验证 record/maps/index；commit 后不再允许分配失败。
- HNSW add 必须“首次图修改之后不返回失败”或使用局部 journal 完整回滚。只清 pending slot 不足够。
- rebuild delta 是 shadow 的派生追赶日志。append 失败时保留已经一致提交的 active records/index，明确令 shadow FAILED；不让失败 shadow 反向破坏产品写。

### 7.3 唯一 replication post-commit 点

冻结为 `kvs_command_task_post_commit()` 中的 vector 专用分支：manager commit 在 task 上留下只读、task-owned deterministic mutation；post-commit 在单 executor 顺序中消费它。不得从 core、AOF文本、response builder 或 eBPF 再生成 mutation。

该点位于产品 write lock 释放后、response finalize/send 前，保持当前客户端链路。Stage 1B 需要跳过 generic `master_sync_command(argv[0], argv[1], argv[2])` 的 VSET/VDEL 分支，不能重放公开文本。

### 7.4 临时 enqueue 失败语义

- 保持异步复制：客户端成功不等待 slave ACK。
- mutation 编码和所需内存必须在 manager commit 前准备成功。
- 正常成功响应至少要求当前进程内 replication queue 接受该 mutation；queue full/allocation failure 不允许被忽略后返回普通成功。
- 在没有 backlog 的 Stage 1，queue acceptance 只证明 connected-session handoff，不提供断线恢复或 durability。不得宣称可靠复制完成。
- 若预留后仍发生 post-commit enqueue 失败，本地 write 已提交，不能伪装回滚：master 必须进入 replication DEGRADED/BLOCKED、停止继续接受需要复制的 vector writes，并返回明确错误。重复请求依赖后续 sequence/record ID 幂等规则处理。
- 真正允许“已接受 mutation 后安全返回并可断线恢复”仍需要 Replication Stage 3 的本地 backlog；Stage 1A 不实现 ACK/backlog。

## 8. Stage 1A 最小修改范围

必须实现：

1. non-empty HNSW reserve 无损增长，失败保留旧图。
2. HNSW add 的 preflight/no-fail-commit 或局部 rollback，恢复 RNG、ID map 和全部 touched adjacency。
3. VSET 新增/v2 overwrite 的 prepared candidate、slot/ID 延迟消费和失败回滚。
4. VDEL free-list 预留与 no-fail commit。
5. store growth 与两类 manager map 的 transactional reserve/publish。
6. allocator、HNSW reserve/add/link 和 rebuild delta 的确定性 test-only fault hooks。
7. task-owned post-commit mutation handoff 接口形状和唯一点位；不实现 codec/wire。

明确排除：

```text
vector mutation codec / TCP vector op / replica apply
offset / ACK / backlog / partial resync / reconnect / promotion
RDB full sync
AOF/RDB format change
public VSET/VDEL/VSEARCH response change
kvs_vector_index_ops_t signature change
HNSW layout、算法、参数、termination、neighbor heuristic 改动
graph persistence 或复制
eBPF reliable replication
性能优化和大规模 benchmark
```

预计生产修改文件（实施时再以最小 diff 收敛）：

```text
src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c
src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c
src/vector/core/hnsw_v2/kvs_hnsw_v2_graph.c       # 仅 rollback/helper 确有需要时
src/vector/kvs_vector.c
src/vector/kvs_vector_index.c
include/vector/kvs_vector_hnsw_v2.h               # 仅内部 helper/test hook
include/vector/kvs_vector_manager.h                # 仅 prepared commit result/test hook
```

测试应新增独立 Stage 1A C tests/shell wrappers；Makefile 只在注册 test binary 必需时修改。不得触碰 distributed wire、AOF/RDB 或 public command table。

## 9. Stage 1A 测试门禁

| 测试 | fault hook | 核对不变量 | Stage 1A 必须 |
| --- | --- | --- | --- |
| low initial capacity insert | 无 | 跨一次边界后全 ID、search、counts 保留 | 是 |
| multiple capacity boundaries | 无 | 每次 graph checksum/ordered hits/records-index count | 是 |
| reserve failure preserves old graph | fail Nth storage allocation | capacity/graph/RNG/queries 不变 | 是 |
| index add failure | before mutation + after selected layer/link | records/maps/index/next IDs 完全不变 | 是 |
| new VSET allocation failure | allocator tag/Nth allocation | logical miss、counts/checksum、AOF/replication 均无新增 | 是 |
| overwrite VSET failure | normalized/map/reserve/add hooks | 旧 ID/answer/vector/search 全保留 | 是 |
| VDEL failure boundary | free-list/map/index hooks | 失败时 record 仍 active、maps/search/counts 不变 | 是 |
| slot reuse | delete后失败/成功 insert | stale ID 永不解析到新 record，next ID 单调 | 是 |
| snapshot load then growth | load full-cap snapshot 后 insert | snapshot graph 保留并新增成功 | 是 |
| sync rebuild low capacity | 小 capacity + tombstones | rebuild 后全 active IDs/count/checksum | 是 |
| async rebuild low capacity | baseline + concurrent deltas | old active 保持，shadow validate/swap 一致 | 是 |
| rebuild delta failure | delta vector/log allocation/max | active write一致；shadow FAILED 且不发布 | 是 |
| ASan/UBSan | 上述 hooks | 无 UAF/double-free/leak/overflow | 是 |
| graph/records checksum | test-only stable checksum | 成功增长一致，任一失败前后相同 | 是 |
| VINFO capacity/failure | 无 | 现有 backend capacity/needs_rebuild 与实际一致 | 是；不要求新增公开字段 |
| 100K low-cap benchmark | 无 | 多次扩容的规模回归 | 否；正确性小数据门禁通过后另行执行 |

现有 tests 的缺口：skeleton larger reserve 在空图/reset 后执行；graph construction 只测 full reject；snapshot roundtrip 不做 load 后跨 capacity 写；没有 allocator/index-link fault injection。因此当前测试不能证明上述不变量。

## 10. Stage 1B 进入条件

全部满足后才进入向量增量复制：

```text
non-empty reserve preserves graph = PASS
reserve failure preserves graph = PASS
new/overwrite/delete fault matrix = PASS
slot/record ID consumption only at commit = PASS
HNSW add failure checksum rollback = PASS
snapshot/rebuild low-capacity lifecycle = PASS
rebuild delta failure keeps active state = PASS
ASan/UBSan = PASS
existing HNSW graph/search/product/rebuild/snapshot regressions = PASS
static manager/core boundary checks = PASS
git diff --check = PASS
```

Stage 1B 随后只实现 deterministic mutation codec、master enqueue 和 replica manager apply；不应重新打开 capacity 设计，也不复制 HNSW graph。

## 11. 风险与待确认事项

没有阻塞 Stage 1A 开始实施的未知架构问题。实施中需要通过测试确定的局部细节：

1. HNSW add 使用“所有连接计划先算后发布”还是 bounded adjacency journal；验收不变量相同，不能以 `needs_rebuild` 代替 rollback。
2. `record_id_to_slot` 扩容与 graph storage growth 应统一成一个 temporary publish，避免两次可失败提交。
3. build/search stats 中哪些属于失败尝试可增长的 observability，哪些必须随事务恢复；图、RNG、records/maps 和查询结果必须严格恢复。
4. AOF void-return 的 durability 缺口和普通 KV queue 丢失语义真实存在，但受阶段边界约束，不在 Stage 1A 顺带修复。
5. Stage 1B 的 connected-session queue acceptance 不是 backlog。直到 offset/ACK/backlog 完成前，最终文档必须继续标注复制能力为 partial。

## 12. 最终状态

```text
ordinary_tcp_replication_reaudited=yes
vector_write_commit_boundary_identified=yes
hnsw_capacity_risk_identified=yes
failure_atomicity_matrix_completed=yes
replication_post_commit_point_frozen=yes
stage1a_scope_minimal=yes
stage1a_test_gate_actionable=yes
public_protocol_change_required=no
ready_for_replication_stage1a_implementation=yes
```
