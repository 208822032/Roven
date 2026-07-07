# HNSW-S snapshot / manifest 设计

## 设计目标

`core_hnsw_v2` 的 HNSW graph 是 records 派生出来的 runtime index。当前 RDB/AOF 只保存 records，load / replay 后需要重新从 active records 构建 graph。100K balanced 默认参数下，构图约 50.2 分钟，因此需要一个独立的 index snapshot cache 来加速 cold-start。

本设计不改变 records source-of-truth 事实，不改变 RDB/AOF 主格式，不改变 VSEARCH response，不改变 `kvs_vector_index_ops_t`。snapshot 是可丢弃 cache，任何校验失败都必须走 rebuild。

## 文件模型

推荐第一版文件模型：

```text
records.rdb
records.aof

vector_index.manifest
vector_index.snapshot
```

含义：

```text
records.rdb / records.aof:
  用户数据 source of truth，保存 vector records、embedding、TTL、generation 等记录状态。

vector_index.manifest:
  snapshot 元信息、参数、数据版本和校验信息。

vector_index.snapshot:
  anncore graph cache，只保存可从 records 重建出来的 index-local graph / vector / slot state。
```

`vector_index.snapshot` 不应嵌入 question、answer、scope、TTL、metadata 或 response 字段。它最多保存 anncore 恢复 graph 需要的 record_id、vector、slot、level、neighbor、deleted flag、entry point、graph stats 等 index-local 状态。

## S2 anncore snapshot payload

HNSW-S2 已把 `core_hnsw_v2` 的 graph cache 序列化能力放入独立源码目录：

```text
src/vector/core/hnsw_v2_snapshot/
```

该目录只处理 v2 index-local graph cache，不接 KVManager startup、不管理正式文件生命周期、不修改 RDB/AOF，也不改变 VSEARCH response。公开 API 放在：

```text
include/vector/kvs_vector_hnsw_v2_snapshot.h
```

私有 binary header 放在：

```text
src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_format.h
```

S2 binary header 覆盖：

```text
magic
format_version
header_size
dim / metric
M / M0 / efConstruction / efSearch
size / capacity / active_count / deleted_count
entry_point / entry_level / max_level
rng_state
payload_size
payload_checksum
```

S2 payload 按固定顺序写入：

```text
slot_to_record_id[0..size)
vectors[slot][0..dim)
levels[0..size)
deleted_flags[0..size)
level0_counts[0..size)
level0_neighbors[slot][0..M0)
upper_counts[slot][level=1..MAX_LEVEL)
upper_neighbors[slot][level=1..MAX_LEVEL][0..M)
```

`record_id -> slot` hash map 不直接持久化，而是在 deserialize 时从 `slot_to_record_id` 和 slot 顺序重建。这样可以避免把 hash table bucket layout 固化为文件格式，同时保留同 record tombstone + re-add 后“最新 slot 覆盖旧 slot”的语义。

S2 payload checksum 使用 FNV-1a 64-bit，仅用于 cache corruption detection。deserialize 使用临时 index：header、payload、checksum、bounds、basic graph validate 全部成功后，才替换目标 index；失败时目标 index 不被发布或污染。

注意：`vector_index.manifest` 是 S1 的外层元信息文件，使用文本 `key=value`；S2 binary header 是 `vector_index.snapshot` 的 anncore payload header。两者不要混淆。

## minimal manifest

第一阶段必须有 minimal manifest，不推荐裸 snapshot。裸 snapshot 无法可靠判断 backend、参数、记录版本和文件完整性是否匹配，容易把旧 graph 绑定到新 records。

建议 minimal manifest 字段：

```text
magic
manifest_version
snapshot_format_version
backend
dim
metric
M
M0
efConstruction
efSearch
active_count
deleted_count
record_generation 或 fallback record_id_checksum / vector_checksum
snapshot_size
snapshot_checksum
created_at
```

说明：

- 完整 manifest 不是第一阶段必须。
- minimal manifest 是必须的。
- `record_generation` 可以优先复用 manager 侧 generation 语义；如果当前 generation 无法覆盖所有 load / replay 场景，则用 `record_id_checksum` 和 `vector_checksum` 兜底。
- `snapshot_checksum` 至少覆盖 snapshot 文件内容；manifest 自身也可以后续增加 checksum。
- manifest 不属于 RDB/AOF 主格式，作为 snapshot cache 元信息独立存在。

## 校验规则

启动时只要任一校验失败，就丢弃 snapshot 并 rebuild：

```text
if manifest missing:
  rebuild

if magic/version/backend mismatch:
  discard snapshot and rebuild

if dim/metric/params mismatch:
  discard snapshot and rebuild

if active_count / generation mismatch:
  discard snapshot and rebuild

if checksum mismatch:
  discard snapshot and rebuild

if deserialize failed:
  discard snapshot and rebuild
```

校验顺序建议：

1. 先完成 records RDB/AOF load / replay。
2. 从当前 records / config 计算 manifest 对照值。
3. 读取并校验 manifest。
4. 校验 snapshot 文件存在、大小和 checksum。
5. 调用 anncore deserialize。
6. 做 graph basic validate。
7. publish 为 active index。

## startup 流程

推荐启动流程：

```text
1. load records from RDB/AOF
2. rebuild manager record structures
3. read vector_index.manifest
4. validate manifest against current records/config
5. if valid:
     load vector_index.snapshot
     anncore deserialize graph
     bind as active index
     optional sanity search/info check
6. if invalid:
     discard snapshot
     use flat fallback or degraded mode
     trigger rebuild
```

关键点：

- records load 成功与否不依赖 snapshot。
- snapshot validate 必须发生在 records load 之后，因为 manifest 需要对照 active_count、generation、dim、metric 和 HNSW 参数。
- flat fallback / degraded mode 是 manager 决策，不是 anncore 决策。
- snapshot load 成功后仍必须保持 manager resolve / scope / threshold / response 流程不变。

## snapshot 写入流程

S4 当前实现的写入流程：

```text
1. successful records-only fallback rebuild or explicit VINDEX REBUILD
2. manager validates active core_hnsw_v2 graph
3. anncore serialize graph to vector_index.snapshot.tmp
4. manager calculates tmp snapshot size/checksum
5. manager builds manifest from current records/config
6. manager writes vector_index.manifest.tmp
7. manager backs up existing vector_index.snapshot / vector_index.manifest to .bak
8. rename temp snapshot to vector_index.snapshot
9. rename temp manifest to vector_index.manifest
10. cleanup .bak on success, rollback .bak on normal failure
```

S4 第一版没有强制 fsync。如果进程或机器在两文件发布窗口崩溃，可能留下 tmp / bak / 不匹配的正式文件；启动校验必须拒绝不完整或不匹配的 manifest/snapshot，并 fallback rebuild。records 仍是 source of truth。

写入触发点：

```text
kvs_vector_rebuild_after_load()
  -> snapshot load failed
  -> records-only rebuild success
  -> best-effort write snapshot

kvs_vector_index_rebuild()
  -> explicit VINDEX REBUILD success
  -> best-effort write snapshot

kvs_vector_rebuild_swap_ready_shadow_for_test()
  -> shadow rebuild skeleton promote success
  -> best-effort write snapshot from promoted rebuild-active v2 index
```

snapshot load 成功路径不重复写 snapshot。非 `core_hnsw_v2` backend 不写 snapshot。S6 的 shadow publish hook 只接入当前已有 test/internal skeleton 成功点，不新增 background worker，不改变 product `VSEARCH` reader path。

## 失败策略

snapshot load failed：

```text
records remain valid
snapshot discarded
rebuild path used
```

snapshot write failed：

```text
active index remains valid
old snapshot remains valid or no snapshot
report warning only
rebuild return value is unchanged
shadow publish return value is unchanged after promote succeeds
```

checksum mismatch：

```text
discard snapshot
never partially load
```

其他策略：

- deserialize 失败不得保留半初始化 graph。
- 参数不匹配不得尝试兼容加载。
- snapshot 损坏不得触发 records 清理。
- snapshot 写失败只影响下次 cold-start 速度，不影响当前 active index。

## KVManager / anncore 职责边界

anncore 可以提供：

```text
serialize_graph()
deserialize_graph()
estimate_snapshot_size()
validate_graph_basic()
```

当前 S2 已提供 test-only v2 API：

```text
kvs_hnsw_v2_index_serialize_file()
kvs_hnsw_v2_index_deserialize_file()
kvs_hnsw_v2_index_validate_basic()
```

S3 已把 deserialize 接入 KVManager records load 后路径：

```text
kvs_vector_rebuild_after_load()
  -> rebuild manager id_map
  -> bind selected backend wrapper
  -> if selected backend is core_hnsw_v2:
       read vector_index.manifest
       build expected manifest from current records/config
       validate manifest and snapshot file checksum
       deserialize vector_index.snapshot
       validate graph basic
       validate loaded record_id/vector bytes against manager records
       bind loaded v2 index
     else / failed:
       fallback to records-only rebuild
```

S4 已把 writer 接入 manager rebuild 成功路径：

```text
records-only fallback rebuild or VINDEX REBUILD success
  -> kvs_vector_index_snapshot_write_current()
       -> validate active core_hnsw_v2 graph
       -> serialize vector_index.snapshot.tmp
       -> build manifest from manager records/config
       -> write vector_index.manifest.tmp
       -> temp rename + .bak rollback
       -> update internal snapshot write status
```

S4 仍未实现 background shadow rebuild writer、snapshot GC、多版本 snapshot、startup config path 或 admin command。VINFO snapshot 字段已在 HNSW-S5 单独实现。

anncore 不负责：

```text
RDB/AOF
manifest
record_generation
file path
atomic rename
fallback policy
VINFO response assembly
scope / TTL / metadata / question / answer
```

KVManager 负责：

```text
load records
validate manifest
compare generation / count / params
call anncore deserialize
discard invalid snapshot
trigger rebuild
write manifest
manage snapshot lifecycle
expose status
```

## VINFO snapshot status

HNSW-S5 将 manager-owned snapshot status 暴露到 `VINFO`，用于诊断 cold-start 是否从 snapshot 恢复、是否 fallback rebuild、以及最近一次 snapshot writer 是否成功。

`VINFO` 新增字段：

```text
snapshot_supported
snapshot_load_attempted
snapshot_loaded
snapshot_valid
loaded_from_snapshot
snapshot_fallback_rebuild_count
snapshot_last_error

snapshot_write_attempted
snapshot_written
snapshot_last_write_error
snapshot_last_write_started_ms
snapshot_last_write_finished_ms
snapshot_last_bytes
snapshot_last_checksum
```

字段语义：

```text
core_hnsw_v2:
  snapshot_supported = 1
  其他字段来自 KVManager 的 snapshot_status

core_flat / non-v2:
  snapshot_supported = 0
  load/write attempt 为 0
  error 字段为空
```

第一版不暴露完整 manifest / snapshot 路径，避免把测试或部署路径写入响应，也避免 VINFO 响应膨胀。路径仍只留在 manager 内部状态和日志中。

这些字段只用于观测，不参与 search 正确性判断，不改变 snapshot load / fallback / writer 语义，也不进入 RDB/AOF。

必须保持：

```text
不要让 core_hnsw_v2 访问 g_vector_state。
不要让 core_hnsw_v2 访问 manager record_id_map。
不要让 core_hnsw_v2 处理 scope / TTL / metadata / question / answer。
```

## 与 R / buildperf 路线关系

```text
HNSW-R:
  background shadow rebuild / online maintenance

HNSW-S:
  index snapshot / cold-start acceleration

HNSW-buildperf:
  anncore build CPU optimization
```

`HNSW-S` 不替代 `HNSW-R`。snapshot 能缩短重启后恢复时间，但在线 tombstone cleanup、参数变更 rebuild、bulk import 后 rebuild 仍需要 R 线的 shadow rebuild / safe swap。

`HNSW-S` 也不替代 `HNSW-buildperf`。如果 snapshot 不存在、失效或参数升级，系统仍要重新构图；单次 build 约 50 分钟的问题仍需要单独的 build CPU 优化路线解决。

## S-final 能力矩阵

```text
manifest read/write/validate: yes
anncore snapshot serialize/deserialize: yes
startup load snapshot: yes
invalid snapshot fallback rebuild: yes
rebuild success write snapshot: yes
VINDEX REBUILD write snapshot: yes
VINFO snapshot status: yes
shadow publish skeleton write snapshot: yes
snapshot GC: no
mmap serving: no
background worker: no
graph persistence in RDB/AOF: no
```

## S-final 不变量

必须持续成立：

```text
records are source of truth
snapshot is cache
snapshot may be deleted
snapshot may be invalid
snapshot load failure does not fail records load
snapshot write failure does not fail rebuild
snapshot does not enter RDB/AOF
VSEARCH response unchanged
kvs_vector_index_ops_t unchanged
core_hnsw_v2 does not access manager semantic state
```

如果后续任一阶段需要改变这些不变量，不能作为 HNSW-S 的小扩展处理，必须重新开独立设计和兼容性审计。

## 当前风险清单

- snapshot 文件路径仍是第一版默认策略。
- 第一版 atomic rename 没有完整 crash-consistency / fsync 保证。
- shadow publish hook 仍基于 9F skeleton / test internal，不等于完整 background rebuild。
- 没有 snapshot GC / 多版本管理。
- 没有 mmap / view。
- 没有真实 embedding 下 cold-start benchmark。
- 没有 100K snapshot load time benchmark。
- 50 分钟 build CPU 成本仍然存在，snapshot 只是在 valid cache 存在时绕过 rebuild。
- worktree 存在大量前序未提交改动，需要提交前人工确认。

## S-final 路线分流

HNSW-S-final 后不建议继续在当前路线增加功能。后续按问题域分流：

```text
HNSW-R:
  真正 background shadow rebuild
  reader safety
  active / shadow / retired 产品化
  delta replay / safe swap

HNSW-buildperf:
  bulk-build API
  parallel build
  reverse prune / candidate selection 优化
  allocator / memory layout

real/proxy embedding benchmark:
  recall@10
  QPS
  p50 / p95 / p99 latency
  snapshot load time
  cold-start no-snapshot vs with-snapshot 对比

future HNSW-S:
  snapshot GC
  multi-version snapshot
  mmap/view
  stronger fsync/crash consistency
```
