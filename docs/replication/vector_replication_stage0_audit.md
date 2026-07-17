# Replication Stage 0 / 0.5: 主从同步现状审计与向量复制方案冻结

> Stage 0.5 修订说明：第 1～8 节为 Stage 0 的现状审计事实，经本轮源码复核（`ms_op_t`、post-commit 分支、eBPF filter、RDB 向量段、manager delta 接口）全部成立，原则上保留。第 9 节起为设计部分，本轮做了实质重写：精简 mutation 字段、明确复制状态机与 offset/ACK 语义、划定 TCP/eBPF 职责、把路线收敛为四个可验收阶段。凡与源码相关的结论均在正文给出证据。本轮仍不修改生产代码、测试、公开协议或持久化格式，不提交 commit。

## 1. 范围、基线与修改

本阶段遵守根目录 `AGENTS.md`，只审计当前 replication、RDB/AOF、Route C 和向量恢复实现并冻结后续语义。未修改生产代码、测试、公开协议、RDB/AOF 格式、HNSW 参数或同步配置。

本阶段新增本报告。进入本阶段前已有且原样保留：

```text
 M README.md
?? docs/vector_command_functionality_audit.md
?? tests/vector_command_persistence_recovery_test.sh
```

主要审计对象：

- replication：`src/distributed/distributed.c`、`src/distributed/sync_backend_tcp.c`、`src/core/kvs_replication_fallback.c`、`src/network/{reactor,proactor,ntyco}.c`、`src/eBPF/{ebpf_syncd,ebpf_kern}.c`。
- command/executor：`src/core/kvs_command_task.c`、`src/core/kvs_command_executor.c`、`src/core/kvs_operation.c`、`include/{distributed,kvs_command_executor,master_slave_proto}.h`。
- persistence/vector：`src/persistence/{kvs_rdb,kvs_aof,kvs_aof_rewrite}.c`、`src/vector/kvs_vector.c`。
- 当前测试：`tests/ttl/test_ttl_tcp_replication.sh`、`tests/ttl/test_ttl_ebpf_replication.sh`、`tests/testcase.c` 及现有 vector persistence/snapshot tests。
- 文档：根 README、`docs/vector_command_functionality_audit.md`、Route C Stage 10.3/10.5A/10.6/10.7/10.8/10.12/10.14 与 final/cleanup 文档、HNSW snapshot/rebuild 报告。

## 2. 当前主从同步架构

### 2.1 TCP 增量路径

```text
master public write command
  -> command task prepare/execute/commit
  -> kvs_command_task_post_commit()
  -> master_sync_command(cmd, key, value)
  -> command name -> ms_op_t
  -> ms_encode_message(magic/version/op/key/value)
  -> process-local msg_queue
  -> TCP backend worker broadcasts to connected slaves

slave TCP receive thread
  -> ms_decode_message()
  -> kvs_command_executor_submit_replicated()
  -> single executor FIFO apply to ordinary KV/TTL
  -> append slave-local AOF
```

该协议是 KVLite 私有二进制 op/key/value 流，不是原始 RESP、AOF 文件或 RDB。当前 `ms_op_t` 只包含 array/rbtree/hash/skiptable/PAPER/TTL；没有 vector op。

### 2.2 TCP 所谓 full sync

```text
master accepts slave
  -> executor builds kvs_full_sync_snapshot_t
  -> snapshot contains ordinary KV + expire_at_ms only
  -> each entry converted to SET/RSET/HSET/SSET + optional PEXPIREAT
  -> master_sync_command()
  -> same global broadcast queue
```

这不是 RDB 传输。`kvs_full_sync_snapshot_t` 没有 vector section；包含完整 vector records/embedding 的是独立 `kvs_rdb_snapshot_t`，当前 replication 没有使用它。

此外 full snapshot 不是发送到单一新 slave，而是通过全局 broadcast 发送。新 slave full sync 时，已连接 slave 也可能收到重复 snapshot 写入。snapshot entry 与在线增量写还可能在全局队列中交错，没有“snapshot offset -> backlog catch-up -> ready”的切换屏障。

### 2.3 eBPF 路径

```text
ebpf_syncd startup
  -> connect slave public port + REPLSYNC
  -> request FULLDUMP from master public port
  -> broadcast concatenated RESP to slaves
  -> attach tcp_recvmsg probes
  -> ring buffer receives master request bytes
  -> parse/normalize supported ordinary write commands
  -> per-slave queue/batcher sends raw RESP

slave REPLSYNC connection
  -> mark connection as replication context
  -> recognized ordinary op: replicated executor task
  -> other normal command: command task with sync_context=2
```

`FULLDUMP` 只遍历四种普通 KV store，不包含 vector records。`ebpf_syncd::is_replicated_write_command()` 也没有 `VSET/VDEL`，所以正常 eBPF sender 会过滤掉向量写。

### 2.4 backlog、offset、ACK、重连

当前不存在 Redis 式 replication backlog：

- TCP/eBPF 的 `msg_queue` 是进程内待发送队列，没有持久序号、offset 范围或断线回放能力。
- 没有 `PSYNC`、`REPLCONF ACK`、master offset、replica applied offset 或成功标记。
- TCP slave `slave_connect_thread` 只 sleep 后连接一次；断线后线程退出，不自动重连。
- eBPF sender 能重新连接并再次 `REPLSYNC`，但不会在重连时重新 FULLDUMP，也没有缺口检测；队列满会丢消息。
- eBPF `REPLSYNC` 的 `+OK` 只是握手响应，replicated apply 的 `+OK` 在 submit 后返回，均不是 offset/application ACK。

## 3. 当前完成度矩阵

状态严格区分“已有代码且有测试”“已有代码但没有测试”“只有文档设计”“尚未实现”。

| 能力 | 状态 | 当前边界 |
| --- | --- | --- |
| TCP 普通 KV 增量同步 | 已有代码且有测试 | TTL TCP suite 覆盖四种 backend 的 set/update/delete/TTL |
| TCP 普通 KV full snapshot | 已有代码且有测试 | 新 slave/rejoin 测试；不是 RDB，且只含普通 KV |
| TCP vector 增量/full sync | 尚未实现 | 协议、snapshot、slave apply 都没有 vector 类型 |
| eBPF 普通 KV FULLDUMP/增量 | 已有代码且有测试 | 有环境依赖的 TTL eBPF suite；本阶段未运行内核路径 |
| eBPF vector FULLDUMP/增量 | 尚未实现 | FULLDUMP 和 sender filter 都排除 V 命令 |
| RDB 文件传输/安装 | 尚未实现 | RDB 仅用于节点本地持久化 |
| AOF 文件传输 | 尚未实现 | slave apply 可写本地 AOF，但不传 AOF 文件 |
| 私有 op command propagation | 已有代码且有测试 | 普通 KV/TTL only |
| executor replicated FIFO apply | 已有代码且有测试 | 当前普通 TCP/eBPF tests 间接覆盖；Route C 专项脚本已删除，仅保留历史报告 |
| replication backlog | 尚未实现 | 内存发送队列不能作为 backlog |
| offset/ACK/partial resync | 尚未实现 | 协议和状态中均无 offset |
| TCP 自动重连 | 尚未实现 | 一次连接线程 |
| eBPF sender 自动重连 | 已有代码但没有测试 | 仅重连当前流，不证明无缺口追赶 |
| slave client write protection | 已有代码且有测试 | `write_command` 在普通 slave client 上返回 read-only；replication context 可 apply |
| role/peer/lag 可观测性 | 尚未实现 | 配置和日志可见；没有 public replication status/lag/offset |
| 多 slave full-sync 隔离 | 已有代码但没有测试 | 当前 full sync 走 global broadcast，存在重复写风险 |

README 的“TCP 全量/增量同步和 eBPF 流量同步基础设施”可视为普通 KV 基础设施描述，不能解释为向量复制、RDB full sync、backlog 或故障恢复已完成。

## 4. V 族命令当前复制状态

| 命令 | command 属性 | TCP 当前行为 | eBPF 当前行为 | 当前结论 | 推荐语义 |
| --- | --- | --- | --- | --- | --- |
| `VSET` | write | post-commit 会调用 `master_sync_command("VSET", ...)`，但 op mapper 返回失败且调用方忽略 | syncd filter 不广播 | 不复制 | 复制主节点 commit 后的确定性 UPSERT/REPLACE mutation |
| `VDEL` | write | 同样因无 vector op 被丢弃 | syncd filter 不广播 | 不复制 | 复制确定性 DELETE mutation |
| `VGET` | read | 不进入 post-commit | 不广播 | 不复制 | 节点本地读取 |
| `VSEARCH` | read | 不进入 post-commit | 不广播 | 不复制 | 节点本地查询 |
| `VINFO` | read | 不进入 post-commit | 不广播 | 不复制 | 节点本地状态；未来增加 replication status 应另设字段/命令 |
| `VINDEX REBUILD` | `write_command=0` | 不传播，slave 可本地执行 | 不传播 | 不复制 | 保持节点本地派生索引控制 |

`VINDEX` 虽然会改变本地派生索引，但不改变 record source of truth，因此当前“不复制”的方向正确。backend/metric/HNSW 配置变化不应伪装成 `VINDEX REBUILD` 复制；应由配置兼容门禁单独处理。

## 5. ONNX、record ID、顺序和幂等语义

### 5.1 从节点是否重新运行 ONNX

正常 TCP/eBPF sender 当前不发送 VSET，所以实际结果是“向量缺失”，而不是“向量已复制但不重跑 ONNX”。

存在一个明确的潜在重执行路径：在 eBPF `REPLSYNC` 连接上，`VSET` 会被 classifier 归为 normal command，并以 `sync_context=2` 提交普通 command task。该 task 仍执行 `kvs_vector_set_prepare()`，使用 `local_c` 时会重新运行 tokenizer/ONNX。AOF 文本 VSET replay 也有相同语义。

因此后续复制不能简单广播公开文本 VSET；必须复制主节点已生成的 float32 embedding。

### 5.2 record ID

新 public VSET 在 commit 时使用本地 `store.next_id++`。core_hnsw_v2 的覆盖更新还会删除旧 record ID 并分配新 ID。若从节点重放文本命令，record ID 由从节点本地状态重新分配；只在初始状态和命令序列完全一致时才可能偶然一致，不是复制契约。

RDB 恢复不同：RDB 已保存 record index、record ID、embedding、generation 和 `next_id`，`kvs_vector_restore_record_with_embedding()` 会原值恢复且按需扩容，不运行 ONNX。

### 5.3 插入顺序和幂等

- TCP connected-stream 的单个队列维持实际 enqueue 顺序，但 full snapshot 与在线增量可能交错，不能证明全局 commit 顺序。
- eBPF 对单条 TCP request stream 顺序解析并进入 per-slave queue，但 ring buffer/queue drop 后没有 offset 检测。
- question+scope 的重复 VSET 在逻辑记录层可覆盖，重复 VDEL 返回 not-found；这不等于 replication mutation 幂等，因为 record ID、generation、HNSW tombstone 和更新顺序仍可能不同。
- 必须用 master sequence/offset + mutation generation 做 duplicate/stale rejection，而不是依赖文本命令“看起来可重复”。

## 6. 全量同步与索引恢复

### 6.1 当前真实流程

新 TCP slave 当前流程是：

```text
slave loads its own local RDB/AOF
  -> local vector snapshot load or records rebuild
  -> starts one-shot connection to master
  -> receives master's ordinary KV snapshot + future ordinary KV ops
  -> no master vector records arrive
```

eBPF 也是普通 KV FULLDUMP，不传 master RDB 或 vector section。因此任务中期望的：

```text
master RDB -> replica records/embedding -> local index -> backlog catch-up -> ready
```

当前尚未实现。

### 6.2 RDB 与 ONNX

现有 RDB vector section 已具备未来 full sync 所需的大部分数据：question/answer/scope、normalized question、float32 embedding、record ID/index、state/deleted、timestamps、generation、model version 和 next_id。若从节点安装该 RDB section，恢复不需要 ONNX。

### 6.3 index readiness

节点本地启动时，RDB/AOF load 和 `kvs_vector_rebuild_after_load()` 发生在 public network listener 前，已有本地 records 的 index 会先恢复。但 replication 没有 `SYNCING/CATCHING_UP/READY` 状态：未来 full sync 若在服务运行期间安装 vectors，当前没有门禁阻止 VSEARCH 在部分 records/index 上响应。

### 6.4 rebuild delta 与 replication

当前 product VSET/VDEL 通过 manager mutation path，可在 async rebuild 时 capture delta。未来 replica mutation apply 必须复用同一 manager-owned commit/delta boundary；直接调用 load-only restore helper 或直接写 core index，会绕过 rebuild generation、record map 和 rollback 语义。

full-sync snapshot 必须绑定一个 replication offset。snapshot 之后的 VSET/VDEL 只能由大于该 offset 的 backlog 追赶一次；不能同时被 full snapshot 和 rebuild delta 当成两个独立来源重复 apply。

### 6.5 index snapshot 与图一致性

当前 replication 不传 `vector_index.snapshot`。Stage 1-3 也不应把 HNSW 图作为复制真相：

- source of truth 是 records/embedding/ID/generation。
- replica 默认本地增量更新或从 records rebuild index。
- 不要求主从 HNSW graph byte-identical；要求 records mutation/offset 一致、active/index count 一致、索引可验证且查询满足同一配置下的正确性门禁。
- 跨节点复用图快照只能作为可选加速：必须 backend/dim/metric/M/M0/ef 参数和 record/vector checksum 全部匹配，失败时回退本地 rebuild。

## 7. 当前测试覆盖

| 测试/证据 | 已证明 | 未证明 |
| --- | --- | --- |
| `tests/ttl/test_ttl_tcp_replication.sh` | 四种普通 KV 的增量 set/delete/overwrite、TTL、new slave ordinary full snapshot | vector、offset、断线自动追赶、多 slave 隔离 |
| `tests/ttl/test_ttl_ebpf_replication.sh` | 环境允许时的普通 KV FULLDUMP/增量/TTL | vector、offset、内核事件丢失恢复 |
| `tests/testcase.c` master-slave modes | 大批普通 KV set/mod/del 后主从响应比较 | 自动化 gate、vector、fullsync boundary |
| Route C Stage 10.x reports | executor ownership、ordinary replicated task、snapshot task 历史结果 | 当前专项脚本已删除，不能当作现行可执行 vector test |
| vector persistence/snapshot tests | RDB 含 embedding、AOF 重放、index snapshot/fallback | master->slave 传输和 catch-up |

当前没有任何 VSET/VDEL replication、replica ONNX counter、record ID parity、vector full sync、promotion 或 vector reconnect 测试。

## 8. 已知缺口和主要风险

1. **向量完全未复制**：master 与 slave records/index 会直接分叉。
2. **潜在 ONNX 重执行**：若以后直接放行 raw VSET，replica 会走正常 embedding prepare；模型/tokenizer 不一致会生成不同向量。
3. **record ID 不确定**：文本重放会使用 replica `next_id`，v2 update 还会生成新 ID。
4. **无正式 commit 顺序**：没有 sequence/offset，snapshot 与 live write 可交错。
5. **没有 backlog/partial resync**：队列丢失或断线后无法定位缺口，只能设计为 full resync。
6. **TCP full sync 广播**：新 slave snapshot 可能重复写到已有 slave；更新与旧 snapshot entry 交错可产生 stale overwrite。
7. **eBPF full sync 有界响应**：FULLDUMP 受单响应上限约束，缺少长度/checksum/completion frame，不能承载大规模 vector。
8. **rebuild 并发**：future replicated mutation 必须进入 manager delta capture；否则可能丢失或重复。
9. **capacity/resource failure**：vector store/HNSW 可动态扩容，初始 capacity 不一致本身不是逻辑错误；但 OOM/超限后当前没有 ACK/offset/resync，不能静默继续。
10. **RDB 启动重复恢复**：RDB vector load 内部和主启动流程都会调用 `kvs_vector_rebuild_after_load()`，结果正确但增加成本并覆盖首次 fallback 状态。
11. **无 replica readiness**：没有 syncing/catching-up/ready gate，未来在线 full sync 时 VSEARCH 可能看到部分状态。
12. **可观测性不足**：没有 role/peer/master offset/applied offset/backlog range/lag/fullsync status 和 vector parity 状态。

## 9. 冻结的确定性 vector mutation

### 9.1 原则

```text
复制 manager commit 之后的确定性 record mutation，
不复制公开 VSET 文本作为 apply 载荷，
不在 replica 重新运行 tokenizer/ONNX 生成 embedding，
不复制 HNSW graph（图是从节点本地派生索引）。
```

证据：`ms_op_t`（`include/master_slave_proto.h`）无向量 op，`master_sync_command("VSET", …)` 只传 `argv[1]/argv[2]`（`src/core/kvs_command_task.c:628`），连 answer/scope 都会丢；因此文本 VSET 在现协议下既无 op、又不完整，不能作为复制载荷。record 的稳定标识与 embedding 已在 record store 中（`include/vector/kvs_vector_record.h`），mutation 直接搬运这些已提交字段即可。

### 9.2 精简后的 mutation 最小字段

只复制“无法在从节点确定性重建”的字段；一切派生量都不上线。

```text
schema_version        u16    版本协商，破坏兼容时递增
op                    u8     UPSERT=1 | DELETE=2      （仅两种）
sequence              u64    全局复制 offset，主节点按 commit 顺序分配
record_id             u64    本条 mutation 对应记录的稳定 id
prev_record_id        u64    被本次覆盖的旧记录 id；无覆盖为 0（断言 + 精确 supersede）
question_len + question            bytes
scope_len   + scope               bytes
answer_len  + answer              bytes         （DELETE 省略）
norm_len    + normalized_question bytes         （DELETE 省略）
embedding_dim         u16                          （DELETE 为 0）
embedding             embedding_dim × float32      （DELETE 省略）
embedding_model_version u32
created_at_ms / updated_at_ms / expire_at_ms  i64 ×3
checksum              u32/u64  覆盖上述 payload
```

length-delimited、versioned，是 manager/replication 内部二进制对象，不改变公开 RESP `VSET/VDEL`。

### 9.3 相对 Stage 0 草案删除/收敛的字段（附理由）

- `operation = UPSERT | REPLACE | DELETE` → 收敛为 **UPSERT / DELETE 两种**。覆盖更新就是“对同一 logical key 的 UPSERT，携带新 `record_id` 与被替换的 `prev_record_id`”，无需独立 REPLACE op，apply 分支更少。
- `logical_key` 独立字段 → 删除。logical key 即 `question + scope`，已在 payload 中携带，重复字段只会带来不一致风险。
- `record generation` **不上线**。generation 是从节点本地 rebuild/snapshot 一致性用的 per-record 版本号（`kvs_vector_record.h:78`，RDB 持久化于 `kvs_rdb.c:182`），与全局复制 offset 是两个维度；把主节点 generation 灌进从节点会把从节点的 rebuild 内部实现钉死在主节点上。从节点 apply 时本地自增自己的 generation。offset 负责“顺序/去重”，generation 负责“本地重建版本”，二者严格分离。
- `question_hash` → 删除，可由 question 确定性重算（`kvs_vector_record.h:39`）。
- `state / deleted` → 删除，op 本身已表达：UPSERT ⇒ ACTIVE 且未删除，DELETE ⇒ 逻辑删除；不存在“复制一条已删除记录的创建”。

### 9.4 Apply 规则（从节点）

- `sequence <= applied_offset`：duplicate/stale，幂等跳过，不报错、不回退。
- `sequence == applied_offset + 1`：正常 apply。
- `sequence > applied_offset + 1`：检测到 gap，**拒绝 apply**、不推进 offset，转入全量重同步（见第 12 节）。
- UPSERT：按 logical key 定位从节点当前记录；若存在且其 id 与 `prev_record_id` 不一致，说明发生分叉，转 resync；一致（或 `prev_record_id==0`）则 mark-delete 旧记录并以主节点给定的 `record_id` 插入新记录。
- DELETE：按 `record_id` 定位；命中即 mark-delete，slot 复用不复用 id（`kvs_vector_record.h:37`），因此 stale hit 天然可辨。
- 一次 apply 必须原子提交：record store、question/scope key map、record_id map、当前 index、rebuild delta（复用 `kvs_vector_manager_capture_upsert_delta / capture_delete_delta`，`kvs_vector_manager.h:342/348`）。**任一步失败都不得推进 `applied_offset`、不得发送 ACK。**
- `embedding_dim` / `embedding_model_version` 与从节点 backend 不兼容：fail-fast，进入 BLOCKED，触发全量重同步或人工介入，不静默跳过。
- capacity 是可扩容资源，不进入 mutation；扩容失败按“apply 失败”处理（停 apply、不推进 offset、不 ACK），不是丢 mutation 后继续。

### 9.5 与 AOF 的关系

该 mutation 概念上是 replication、未来 AOF 与内部恢复的 canonical 格式，但 **Replication-1～4 期间保持与 AOF 解耦**：不改 AOF 格式、不要求 AOF 立即采用此格式。AOF 迁移是独立后续工作，必须保留旧 RESP AOF replay 兼容或提供显式版本迁移，不得破坏现有文件（`AGENTS.md` 硬边界）。

## 10. 冻结的命令复制矩阵

| 命令 | 是否复制 | 冻结语义 |
| --- | --- | --- |
| `VSET` | 是 | master 完成 embedding 与 manager commit 后复制确定性 UPSERT/REPLACE mutation |
| `VDEL` | 是 | 复制带 record ID/generation/sequence 的确定性 DELETE mutation |
| `VGET` | 否 | replica 本地只读 |
| `VSEARCH` | 否 | replica 本地 index 查询；readiness 未完成时拒绝或明确不可用 |
| `VINFO` | 否 | 节点本地观测；replication 状态是本地字段，不作为命令复制 |
| `VINDEX REBUILD` | 否 | 节点本地派生索引控制；配置兼容性由 full sync/启动 gate 校验 |

## 11. 复制状态机、offset 与 ACK 语义

### 11.1 各标识的职责（不可混用）

| 标识 | 归属 | 职责 | 覆盖更新时 |
| --- | --- | --- | --- |
| `logical_key`（question+scope） | 业务 | 记录的业务身份，决定覆盖目标 | 不变 |
| `record_id` | manager/core 之间稳定标识 | 一条不可变记录版本；索引 add/delete、stale-hit 防护 | v2 覆盖会删旧 id、发新 id（`kvs_vector_index.c:297/407`） |
| `generation` | 从节点本地 | rebuild/snapshot 版本一致性 | 本地自增，不上复制线 |
| `sequence/offset` | 复制流 | 全局顺序、去重、backlog 定位、ACK | 每条 mutation 单调 +1 |

结论：**全局 replication offset 与 record generation 必须分离**。offset 是流位置，generation 是记录版本，二者服务不同不变量。

### 11.2 主节点 commit / 复制入队 / 客户端响应顺序

```text
1. manager commit（record store + index + delta 原子生效）
2. 在与 commit 同一顺序点上分配 sequence，并把 mutation 入复制流
3. 向客户端返回成功
```

采用 **异步复制**：客户端成功不等待从节点 ACK（KVLite 定位为 AI Chat 缓存，可接受从节点滞后与故障切换时丢失少量未 ACK mutation）。ACK 只用于 backlog 裁剪与 lag 观测，不作为客户端写入的门禁。语义半同步是可选的后续项，不在四阶段内。

### 11.3 从节点复制状态机

```text
DISCONNECTED
  → CONNECTING
  → FULL_SYNC      安装 records/embedding/next_id @ snapshot offset O；本地建/载索引
  → CATCHING_UP    重放 backlog (O, master_current]
  → READY          apply 在线 mutation；仅此状态允许 VSEARCH
  → BLOCKED/FAILED  config/checksum/apply 失败：停止推进 offset，等待重同步或人工介入
```

Ready 门禁：只有进入 `READY` 才允许对外返回 `VSEARCH`。`FULL_SYNC/CATCHING_UP` 期间 `VSEARCH` 返回明确“replica 未就绪”，不返回部分结果（对应第 6.3 节缺失的 gate）。

### 11.4 offset/ACK 不变量

- 主节点维护 `master_offset = 最后分配的 sequence`。
- 从节点维护 `applied_offset`，仅在一条 mutation 完整原子 apply 成功后推进。
- 从节点周期性 ACK `applied_offset`（信息用途，非客户端门禁）。
- snapshot 绑定 offset O；full sync 后只重放 `(O, current]` 一次，不得让 full snapshot 与 backlog 对同一 mutation 双重 apply（对应第 6.4 节）。

## 12. 失败语义

| 情况 | 检测 | 处理 |
| --- | --- | --- |
| duplicate（seq ≤ applied） | sequence 比较 | 幂等跳过 |
| gap（seq > applied+1） | sequence 比较 | 停 apply、不推进 offset、转全量重同步 |
| checksum 失败 | payload checksum | 拒绝该 mutation、不推进 offset、转全量重同步 |
| backlog overrun（请求 offset 早于 backlog 起点） | 与 backlog range 比较 | 直接全量重同步 |
| logical-key 分叉（`prev_record_id` 不匹配） | apply 断言 | 转全量重同步 |
| config 不兼容（dim/model/metric） | 握手 + apply 校验 | 进入 BLOCKED，报明确错误 |
| capacity/resize 失败、OOM | apply 返回值 | 按 apply 失败：停推进 offset、不 ACK，不静默继续 |

原则：**apply 失败绝不推进 offset、绝不发送成功 ACK**；无法安全增量追赶时一律退化为一次全量重同步，而不是设计复杂的部分修复。

## 13. TCP 与 eBPF 职责划分（冻结）

证据：eBPF 走 kprobe 抓 `tcp_recvmsg` 的原始 RESP 字节，`is_replicated_write_command()`（`src/eBPF/ebpf_syncd.c:412`）不含 `VSET/VDEL`，`FULLDUMP` 只遍历四种普通 KV，且响应无 framing/长度/checksum、队列满即丢。eBPF 层根本看不到“主节点 commit 后生成的 float32 embedding mutation”——那是内部对象，不在网络字节流上。

冻结结论：

- **向量可靠复制只走 TCP 应用层 post-commit mutation 路径**。
- **eBPF 不承担向量可靠复制职责**：它至多作为普通 KV 流量镜像的实验/旁路能力，向量复制显式排除在外。
- **统一复制协议以 TCP 为准**（offset/ACK/backlog/checksum 都在 TCP 侧定义）；不得把任何 transport 的内存发送队列当作 backlog。

## 14. 后续实现路线（四阶段，含验收门禁）

### Replication-1：可靠的向量增量复制

- **输入**：现有单机向量 commit/delta 路径、已冻结的 mutation schema（第 9 节）。
- **输出**：TCP 上 `VSET/VDEL` 的确定性增量复制，从节点 apply 不跑 tokenizer/ONNX。
- **修改范围**：新增 versioned vector mutation op/codec；主节点 post-commit 捕获最终 record/embedding/record_id/prev_record_id；从节点 manager apply API（复用 delta 边界）。
- **不做**：不改 AOF 格式；不做全量同步；不做 backlog/断线追赶；不改 eBPF。
- **正确性不变量**：从节点 ONNX/tokenizer 调用计数为 0；record_id 与主节点逐条一致；apply 失败不推进 offset；duplicate 幂等；gap 触发失败而非静默。
- **最小测试**：ID/顺序 parity、重复 mutation、update/delete、rebuild 并发下 delta 捕获、ONNX-zero-rerun 计数。
- **进入下一阶段门禁**：上述测试全绿；从节点在纯增量场景与主节点 active/index count 一致。

### Replication-2：带 offset 的全量同步与索引就绪

- **输入**：Replication-1 的 mutation apply；现有 RDB 向量段（records/embedding/`next_id`/generation，`kvs_rdb.c`）。
- **输出**：master→replica 的 records/embedding/`next_id` 安装流程（带 framing/checksum/完成帧），安装后本地建/载索引，`SYNCING→…→READY` 状态机 + VSEARCH ready 门禁。
- **修改范围**：full-sync 安装通道、replica 状态机、ready gate。snapshot 绑定 offset O。
- **不做**：不传 HNSW 图作为真相；不实现 backlog 追赶（在线并发写用显式 barrier 或标注该阶段不具备在线 catch-up 完整性）；不改 AOF。
- **正确性不变量**：full sync 完成后从节点 records/embedding 与主节点快照点一致；ready 前不返回部分 VSEARCH；安装失败进入 FAILED 且不对外可查。
- **最小测试**：新从节点冷启动全量、快照点一致性、ready 门禁拒绝 in-flight VSEARCH、安装 checksum 失败回退。
- **门禁**：全量 + 后续增量能达到与主节点一致的可查询状态。

### Replication-3：backlog、断线重连与部分重同步

- **输入**：Replication-2 的 offset 绑定全量。
- **输出**：主节点 bounded backlog、从节点 `applied_offset`/ACK、断线重连后 `(O, current]` 增量追赶；gap/checksum/overrun/apply 失败统一退化为全量重同步。
- **修改范围**：backlog 结构、ACK 协议、重连追赶逻辑；TCP/eBPF 共用一致 offset/ACK 语义（eBPF 仍不承载向量）。
- **不做**：不实现语义半同步；不把 transport queue 当 backlog；不改 AOF。
- **正确性不变量**：snapshot 后每条 mutation 恰好 apply 一次；backlog 越界必然触发全量而非静默丢失；重连不产生重复 active record。
- **最小测试**：断线重连追赶、backlog overrun→全量、注入 gap/checksum 失败→全量、多从节点隔离无重复写。
- **门禁**：断线/丢包注入后从节点最终与主节点一致。

### Replication-4：重启、晋升与故障恢复

- **输入**：前三阶段的 mutation/offset/backlog 语义。
- **输出**：从节点本地持久化 `applied_offset` 与去重状态；重启后正确恢复 `next_id`/offset/generation；晋升为新主节点时三者正确；旧主回归安全。
- **修改范围**：从节点持久化 offset/去重状态（此阶段才触及持久化格式，须走版本兼容迁移）、晋升流程、`next_id` 恢复。
- **不做**：不引入跨节点强一致；不破坏旧 RDB/AOF 兼容。
- **正确性不变量**：重启后不重放已 apply mutation；晋升后 `next_id` 不与历史 record_id 冲突；模型不一致时拒绝晋升/同步。
- **最小测试**：crash/restart 去重、晋升后 `next_id` 无冲突、backlog overrun、fullsync 中断恢复、模型不一致拒绝、旧 AOF 兼容迁移。
- **门禁**：故障注入矩阵全绿；晋升后新主可继续对外复制。

## 15. 验证、构建与工作树

- 生产代码修改：否。
- 测试修改：否。
- 构建/功能测试：未运行；本阶段只有只读源码审计和文档新增，没有可执行行为变化。
- 静态审计：使用 `rg`/`grep` 复核 command post-commit（`kvs_command_task.c:585-638`）、`ms_op_t`（`master_slave_proto.h`，无向量 op）、eBPF filter（`ebpf_syncd.c:412`，无 VSET/VDEL）、RDB 向量段 `next_id`/`generation`/`restore_record_with_embedding`（`kvs_rdb.c:157/182/317`）、record 字段（`kvs_vector_record.h`）、manager delta 接口（`kvs_vector_manager.h:342/348`）；结论与本报告一致。
- Stage 0.5 修订部分（第 9 节起）为设计文档变更，未引入任何可执行行为变化。
- `git diff --check`：PASS，无输出。
- 新增报告通过 `git diff --no-index --check /dev/null docs/replication/vector_replication_stage0_audit.md` 检查；退出码 `1` 仅表示文件与 `/dev/null` 不同，无 whitespace 错误输出。

最终 `git status --short`：

```text
 M README.md
?? docs/replication/
?? docs/vector_command_functionality_audit.md
?? tests/vector_command_persistence_recovery_test.sh
```

其中除 `docs/replication/vector_replication_stage0_audit.md` 外均为进入本阶段前已有改动，本阶段未处理或覆盖。

## 16. Stage 0 / 0.5 结论

Stage 0 的现状审计（第 1～8 节）经复核成立并保留；Stage 0.5 完成了设计复审与冻结（第 9～14 节），没有生产实现变化。当前普通 KV replication 可用于有限测试场景，但不存在 vector replication、RDB full sync、offset/backlog 或生产级故障恢复闭环。

```text
replication_current_state_audited=yes
vector_commands_currently_replicated=no
replica_onnx_reexecution_detected=yes
deterministic_vector_mutation_required=yes

replication_design_reviewed=yes
mutation_schema_frozen=yes
full_sync_boundary_frozen=yes
offset_ack_semantics_frozen=yes
implementation_stages_actionable=yes
ready_for_replication_stage1=yes
```

`replica_onnx_reexecution_detected=yes` 表示现有 replication-context normal VSET/AOF text replay 会走 embedding prepare 的代码事实；正常 sender 当前过滤 VSET，因此线上当前表现是“vector 不复制”，而不是已经发生了一次完整 vector replication。Stage 0.5 冻结的 mutation 路径正是为消除该重执行风险而设计（复制已生成 embedding，从节点不再 prepare）。
