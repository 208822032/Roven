# 第十八章 派生 HNSW 图的 Snapshot、校验与冷启动

## 18.1 为什么 records-only 恢复不再够用

第十七章结束时，`core_hnsw_v2` 已经成为默认 ANN backend，并在固定 synthetic 口径下通过 100K recall gate。但它留下了一个直接影响启动和故障恢复的问题：record store 虽然能完整恢复业务数据，HNSW graph 却必须从 active records 重新构建。

[`P2 100K 报告`](../hnsw_productization/hnsw_p2_100k_balanced_default_report.md) 记录的 records-only build 为 `3,011,810,795 us`，约 50.2 分钟。这个结果不否定“records 是 source of truth”，反而暴露了该设计的运行代价：正确恢复路径存在，但每次 cold start 都同步重走昂贵构图，产品可能长时间无法提供向量检索。

本章解决的唯一问题是：

```text
在 RDB/AOF records 仍是业务真值的前提下，
把构建成本很高的 core_hnsw_v2 graph 保存为可丢弃 cache，
启动时先用 records/config 校验，再快速加载；
cache 缺失、过期、损坏或不兼容时，安全退回 records-only rebuild。
```

它不重新解释 v2 的构图、搜索、neighbor selection、FAISS parity 或 balanced defaults，也不把 snapshot 写成新的真值。`74ec6af` 才开始的在线 rebuild、reader safety、active/shadow/retired、delta replay 和后台 worker属于第十九章；真实 T2Ranking benchmark 属于第二十章。

## 18.2 Git 范围、演进主线与源码导航

### Git 范围

第十七章结束在 `7e89a93`。它的下一提交是 `15deef1`；`26ebf99` 只增加 cold-start benchmark；再下一提交 `74ec6af` 已进入 HNSW-R 在线 rebuild。因此本章范围核验为：

```text
起点：15deef1
终点：26ebf99
Git inclusive range：15deef1^..26ebf99
```

两个提交不是两个孤立故事：

```text
15deef1
→ 聚合代码清理、manifest、graph roundtrip、startup load/fallback、
  rebuild 后写入、VINFO 状态和 snapshot 路线收口；

26ebf99
→ 不改生产代码，只增加 10K quick / 100K load-only benchmark，
  量化 valid snapshot 绕过 records-only rebuild 的收益和文件成本。
```

`15deef1` 还是一个大型聚合提交。它同时删除 legacy `core_hnsw`、FAISS/hnswlib 产品 adapter及旧 harness，并迁移 v2 helper header。[`CODE-HYGIENE 报告`](../hnsw_index_snapshot/codebase_hygiene_before_s1_report.md) 说明这些清理把当前产品 backend 收敛为 `core_hnsw_v2` 与 `core_flat`；它们是 snapshot 格式边界的前置整理，不是 snapshot 加载能力本身。本章只在这里交代混合 diff，不展开旧 backend 清理史。

### 核心源码导航

1. [`15deef1` 的 manifest schema](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/include/vector/kvs_vector_index_snapshot.h#L6-L36)
2. [`15deef1` 的 binary snapshot header](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_format.h#L9-L38)
3. [`15deef1` 的 graph serialize](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_serialize.c#L9-L65)
4. [`15deef1` 的临时 index deserialize/publish](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_deserialize.c#L258-L390)
5. [`15deef1` 的 graph basic validation](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_validate.c#L126-L305)
6. [`15deef1` 的两文件发布与回滚](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/kvs_vector.c#L1845-L1932)
7. [`15deef1` 的 snapshot writer](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/kvs_vector.c#L2186-L2334)
8. [`15deef1` 的 startup load/fallback](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/kvs_vector.c#L2440-L2652)
9. [`15deef1` 的 VINFO snapshot 字段](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/core/kvs_operation.c#L2630-L2742)
10. 当前仍保留的 [`snapshot startup/fallback test`](../../tests/vector_index_snapshot_load_after_records_test.c)
11. [`26ebf99` 的 cold-start harness](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/26ebf99/tests/vector_index_snapshot_cold_start_bench.c#L591-L710)
12. [`10K/100K cold-start 报告`](../hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md)

## 18.3 graph 是派生状态，snapshot 是可丢弃 cache

这一阶段没有改变上一章建立的真值关系：

```text
RDB / AOF records
  -> question / answer / scope / TTL / metadata / embedding / record_id
  -> 业务真值

vector_index.snapshot
  -> record_id / vector / graph-local slot / level / neighbor / tombstone
  -> 可删除、可过期、可重建的派生 cache
```

RDB/AOF 没有开始保存 HNSW graph。启动必须先恢复 records，并重建 manager 的 key map 与 `record_id_map`，之后才有资格判断 snapshot 是否属于这批 records。snapshot 自己不能恢复 question、answer、scope、TTL，也不能独立组装 VSEARCH response。

这带来一个重要故障隔离结果：

```text
snapshot missing / invalid / corrupt
!= records 丢失

snapshot write failed
!= rebuild 失败

snapshot checksum mismatch
!= RDB/AOF records 损坏
```

代价也很明确：只要 cache 不可信，就必须接受 records-only rebuild 的 CPU 和启动时间。snapshot 优化的是“已有可信 cache 时跳过重建”，没有降低构图算法本身的成本。

## 18.4 manifest 与 snapshot 的两层数据契约

这一版没有把所有信息塞进一个二进制文件，而是分成外层 manifest 和内层 graph snapshot。

### manifest：回答“这个文件能否用于当前 records/config”

[`kvs_vector_index_snapshot_manifest_t`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/include/vector/kvs_vector_index_snapshot.h#L10-L36) 是文本 `key=value` 文件，字段全部来自实际代码：

| 类别 | 字段 | 作用 |
| --- | --- | --- |
| 格式身份 | `magic`、`manifest_version`、`snapshot_format_version` | 拒绝错误文件和不支持的版本 |
| backend/config | `backend`、`dim`、`metric` | 绑定当前 backend、维度和距离语义 |
| HNSW 参数 | `hnsw_m`、`hnsw_m0`、`hnsw_ef_construction`、`hnsw_ef_search` | 参数不同即拒绝复用 |
| records 摘要 | `active_count`、`deleted_count`、`record_generation` | 检查数量和最大 record generation |
| records 指纹 | `record_id_checksum`、`vector_checksum` | 检查 active ID、维度和 embedding bytes |
| 文件摘要 | `snapshot_size`、`snapshot_checksum` | 检查实际 graph 文件大小和内容 |
| 观测 | `created_at_ms` | 记录生成时间，不参与业务真值 |

[`manifest validate`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/kvs_vector_index_snapshot.c#L481-L598) 对上述兼容字段逐项比较。`record_generation` 使用 active records 的最大 `record->generation`，不是简单相信恢复后的 manager 全局 generation；真正的 records 一致性还依赖 active/deleted count、record ID checksum 和 vector checksum共同约束。

checksum 是 FNV-1a 64-bit。它适合检测偶发损坏与不一致，不是 cryptographic integrity，也不能防恶意构造。

### binary header：回答“这个 graph 文件能否安全反序列化”

[`kvs_hnsw_v2_snapshot_header_t`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_format.h#L14-L38) 记录：

```text
magic / format_version / header_size
dim / metric / M / M0 / efConstruction / efSearch
size / capacity / active_count / deleted_count
entry_point / entry_level / max_level
rng_state
payload_size / payload_checksum
```

manifest 面向 manager records/config，binary header 面向 anncore 内部布局。两层都通过并不重复：前者防止“把别的数据或配置的图加载进来”，后者防止“按错误长度和结构读取二进制 payload”。

当前格式采用精确版本匹配，没有跨版本迁移或兼容读取分支。header 和数组以本机 C 布局/基础类型直接写入，也没有显式 endian 转换。因此它是同版本、本地部署语境中的 cache 格式，不是可长期承诺的跨架构交换格式。

## 18.5 graph serialization：保存了什么，重建了什么

[`serialize payload`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_serialize.c#L67-L130) 按固定顺序写入：

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

vector 只写 `dim * sizeof(float)`，不写 aligned stride padding。`upper_offsets`、`record_id_to_slot`、visited tags、search context、graph/memory stats 也不直接持久化：offset 可以由固定布局恢复，id map 在加载后重建，临时查询状态重新初始化，stats 重新计算。

反序列化不是直接覆盖 active index，而是：

```text
读取并校验 header/file size
-> init temp index
-> 按 header 参数 reserve temp capacity
-> 读取 payload 并验证 checksum
-> 恢复 entry/rng/size/deleted 状态
-> 重建 record_id_to_slot
-> 刷新 graph/memory stats
-> basic graph validation
-> 全部成功后 destroy target 并 move temp
```

这条 temp-then-publish 路径意味着 bad magic、截断、payload checksum mismatch、分配失败或 graph validation 失败不会把半初始化 graph 发布给查询，也不会破坏原目标 index。

### basic validation 能证明什么

[`kvs_hnsw_v2_index_validate_basic()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_validate.c#L126-L305) 检查：

```text
ready / dim / M / M0 / size / capacity / stride
entry point / max level / node level
deleted flag 与 deleted_count
degree 上限
neighbor slot 范围
self edge / duplicate edge
upper neighbor level
duplicate active record_id
```

这证明结构在当前不变量下可安全遍历，不证明 graph 具有全局连通性、最佳导航质量或目标 recall。文件兼容性校验和 basic graph validation 都不能替代规模检索验证。

## 18.6 snapshot writer 与两文件发布边界

snapshot 不是每次 VSET 后同步写。`15deef1` 把 writer 接在两个成功点：records-only fallback rebuild 完成后，以及显式 `VINDEX REBUILD` 完成后。valid snapshot load 成功不会重复 rewrite。

writer 的实际顺序是：

```text
validate active core_hnsw_v2
-> serialize graph 到 snapshot temp
-> 计算 temp snapshot size/checksum
-> 从 manager records/config 构造 manifest
-> 写 manifest temp
-> 旧 snapshot/manifest rename 为 .bak
-> temp snapshot rename 为正式 snapshot
-> temp manifest rename 为正式 manifest
-> 成功后删除 .bak
-> 失败则清 temp，并尽量恢复 .bak
```

代码证据见 [`kvs_vector_index_snapshot_write_core_hnsw_v2()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/kvs_vector.c#L2186-L2334) 与 [`kvs_vector_snapshot_publish_files()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/vector/kvs_vector.c#L1845-L1932)。

这里的“发布”需要保守理解：单个 rename 提供文件名切换，但 snapshot 与 manifest 是两个文件，不可能靠两次 rename 形成真正的跨文件原子事务。第一版也没有 `fsync`/directory `fsync`。进程或机器在两次 rename 之间崩溃，仍可能留下 tmp、bak 或互不匹配的正式文件。

正常错误路径会尽量 rollback；崩溃后的最终防线不是“写入绝不出错”，而是下次启动重新检查 manifest、size 和 checksum，不可信就 rebuild。因此这一版实现了 temp/backup/rename 的 best-effort 发布和清理，没有证明完整 crash consistency。

snapshot write failure 不改变成功 rebuild 的返回值，不替换 active index，也不影响当前 search。代价是下一次启动可能再次承担完整 rebuild。100K snapshot 超过 800 MB，但本范围没有独立的 writer wall-time 结果；不能从 create-fixture build 时间反推出纯写盘成本。

## 18.7 startup validation 与加载调用链

产品恢复顺序是：

```text
RDB load / AOF replay
-> records 与 embedding 恢复
-> kvs_vector_rebuild_after_load()
   -> rebuild manager id map
   -> bind configured backend shell
   -> core_hnsw_v2 才尝试 snapshot
      -> read manifest
      -> scan restored records，构造 expected manifest
      -> compare versions/backend/dim/metric/HNSW params/count/generation/checksums
      -> deserialize graph into temp index
      -> basic graph validation
      -> active record_id/vector bytes 再与 manager records逐项核对
      -> bind loaded index
      -> mark snapshot_loaded / loaded_from_snapshot
   -> load 失败则 records-only rebuild
```

manager-side loaded-record validation 逐个检查 active graph slot 能否通过 manager `record_id_map` 找到 active record、embedding dimension 是否一致、snapshot vector bytes 是否与 record embedding完全一致，并检查 active slot 数等于 manager active records。

这说明加载不是“文件 checksum 对了就信任”。但也要注意启动边界：`kvs_vector_rebuild_after_load()` 返回成功只说明 vector index 恢复步骤成功；服务还要完成其他模块初始化并开始监听，才是完整 server ready。snapshot loader 本身没有定义 replica READY、网络 READY 或整个进程的健康状态。

## 18.8 missing、invalid 与 incompatible snapshot 如何回退

实际 fallback 条件包括：

```text
manifest missing / unreadable / malformed
manifest or snapshot format version mismatch
backend / dim / metric / HNSW parameter mismatch
active/deleted count mismatch
record_generation mismatch
record_id or vector checksum mismatch
snapshot file missing / size mismatch / checksum mismatch
bad magic / truncated payload / deserialize failure
graph basic validation failure
loaded active record/vector 与 manager records 不一致
bind failure
```

这些错误不会删除或回滚已经恢复的 records。`kvs_vector_rebuild_after_load()` 增加 `fallback_rebuild_count`、写日志，然后调用原来的 `kvs_vector_index_rebuild_current()`；重建成功后再 best-effort 写出新 snapshot。

这里没有一个独立、长期保持的 snapshot `DEGRADED` 状态机。实际状态是“最近一次 load 失败原因 + fallback 计数 + rebuild 返回值”：

```text
load failed + rebuild succeeded
-> records/index 恢复成功，cache 被重建

load failed + rebuild failed
-> kvs_vector_rebuild_after_load 返回错误，不能宣称 vector ready
```

另一个源码细节是，`snapshot_valid` 在 manifest 对比通过后、binary deserialize 前就被置为 1；因此它更接近“外层 manifest 已通过”，不是完整加载成功的充分条件。判断最终是否走 snapshot 必须看 `snapshot_loaded` 与 `loaded_from_snapshot`，不能只看 `snapshot_valid`。

## 18.9 VINFO 与日志如何区分 load、fallback 和 write

[`kvs_vector_snapshot_status_t`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/include/vector/kvs_vector_manager.h#L69-L85) 把 load 与 writer 状态分开：

```text
load:
  snapshot_load_attempted
  snapshot_loaded
  snapshot_valid
  loaded_from_snapshot
  fallback_rebuild_count
  last_snapshot_error
  last_snapshot_manifest_path
  last_snapshot_path

write:
  snapshot_write_attempted
  snapshot_written
  last_snapshot_write_error
  last_snapshot_write_started_ms
  last_snapshot_write_finished_ms
  last_snapshot_bytes
  last_snapshot_checksum
```

S5 将它们映射到 VINFO；[`历史 response assembly`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/15deef1/src/core/kvs_operation.c#L2630-L2742) 只增加观测字段，没有改变 VSEARCH response。

日志则记录 snapshot loaded、fallback reason、writer success/skip。它们能回答“最近一次尝试走了哪条路径”，不能证明 snapshot 长期可靠、磁盘已经持久化到介质，也不能替代启动 readiness 或 recall gate。

## 18.10 10K/100K cold-start benchmark：收益与一次口径陷阱

`26ebf99` 没有修改生产代码，只增加 [`cold-start benchmark`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/26ebf99/tests/vector_index_snapshot_cold_start_bench.c#L761-L1050)。它有三种模式：

```text
create-fixture
-> 生成 synthetic records、构图并写 records/manifest/snapshot fixture

load-only
-> 恢复 records fixture，只做 manifest validate + deserialize + record validation + bind
-> snapshot 无效直接失败，禁止 fallback rebuild 污染计时

quick
-> 临时目录内跑一轮 10K create-fixture + load-only
```

### 10K quick：机制验证，不是正式 speedup

[`cold-start 报告`](../hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md) 记录的 10K/512d balanced 结果是：build `126,327,129 us`；snapshot 文件 `84,580,136` bytes；manifest `402` bytes；records fixture `20,640,096` bytes；records restore `52,909 us`；snapshot total load `374,815 us`；其中 core load `321,906 us`。

报告曾用 100K records-only rebuild baseline 除以这次 10K snapshot load，得到 `8,035.46x`。这不是同规模 apples-to-apples 对比，只能验证 harness 和 load-only 机制，不能作为 10K 或 100K 正式收益。保留这个数字的价值在于暴露 benchmark 口径问题，而不是把它包装成成果。

### 100K load-only：同规模，但 old path 是复用历史结果

100K fixture 使用 synthetic normalized IP、512d、M=48、M0=96、efConstruction=512、efSearch=1024、50 queries。报告记录：

```text
fixture build           2,933,596,834 us
snapshot file           845,800,136 bytes
manifest                403 bytes
records fixture         206,400,096 bytes
index memory_bytes      851,194,400 bytes
post-build topK sanity  50 / 50
```

随后 load-only 连跑 5 次，结果为：

```text
records restore avg       895,253.80 us
snapshot total load avg   5,923,515.60 us
snapshot total load p50   6,050,485 us
snapshot total load p95   6,803,620 us
snapshot core load avg    5,028,261.80 us
manifest/file validate    1,793,127.60 us
deserialize avg           3,235,132.80 us
bind avg                   0.80 us
post-load sanity          50 / 50
fallback rebuild count    0
```

同一报告用 P2 的 100K records-only `3,011,810,795 us` 历史 baseline 计算约 `508.45x`。规模和参数已对齐，但 old path 没有在 `26ebf99` 同次运行重测；所以这是“本次 load-only + 历史同口径 rebuild”的阶段性比较，不是同机同进程交替运行的严格 A/B。

这些结果已经证明 valid snapshot 在该 synthetic fixture 上可以把分钟级/小时级构图替换为秒级加载，也证明实际走了 `loaded_from_snapshot=1` 且没有 fallback。它没有证明完整 server startup、真实 embedding、并发查询、跨机器缓存复用或长期磁盘可靠性。

## 18.11 load、rebuild、startup、文件大小与内存不能混写

本章至少有五种不同量：

| 指标 | 实际包含 | 不包含 |
| --- | --- | --- |
| `fixture_create_build_us` | 进程内 synthetic graph fixture 构建 | 完整 server startup；纯 snapshot writer 时间 |
| `records_restore_us` | benchmark fixture 恢复 manager records | RDB/AOF 解析、真实 ONNX、网络初始化 |
| `snapshot_core_load_us` | manifest/file validate、deserialize、record validation、bind | records restore |
| `snapshot_load_us` | records restore + core load | 完整 KVLite startup 的其他模块 |
| records-only rebuild baseline | P2 同参数 100K graph build | 本次 `26ebf99` 重跑的 old path |

因此：

```text
load-only benchmark != full server startup
snapshot deserialize != load total
load total != product ready
historical rebuild baseline != same-run A/B
```

文件大小和内存也不能互换。100K snapshot 的 `845,800,136` bytes 是 serialized graph payload/header 文件；`851,194,400` bytes 是 v2 自己统计的 index allocation。二者数值接近是因为当前 fixed-array layout 被近似原样展开，不代表 serialized size 就是 runtime RSS。加载还会分配 temp index、id map、context 和进程其他状态；反序列化期间的峰值内存也没有在本次报告中独立测量。

## 18.12 测试、失败路径与证据等级

本章没有重新执行历史 benchmark。以下 PASS 来自对应阶段报告，不是本次文章修订的执行结果。

| 证据 | 历史执行证据 | 已证明 | 未证明 |
| --- | --- | --- | --- |
| [`manifest test`](../../tests/vector_index_snapshot_manifest_test.c) | [`S1 report`](../hnsw_index_snapshot/hnsw_s1_minimal_manifest_report.md) 记录 PASS | schema roundtrip、各 mismatch、missing/malformed、checksum helper | graph 可加载、startup fallback |
| [`graph roundtrip test`](../../tests/vector_core_hnsw_v2_snapshot_roundtrip_test.c) | [`S2 report`](../hnsw_index_snapshot/hnsw_s2_core_hnsw_v2_snapshot_roundtrip_report.md) 记录 PASS | empty/small graph、tombstone/update、bad magic、truncate、failure 不污染 target | manager records 对照、规模 cold start |
| [`startup/fallback test`](../../tests/vector_index_snapshot_load_after_records_test.c) | [`S3 report`](../hnsw_index_snapshot/hnsw_s3_snapshot_load_fallback_report.md) 记录 PASS | valid load；missing、param/checksum mismatch、corrupt/truncate/text file fallback | 完整 server ready、崩溃恢复 |
| [`writer test`](../../tests/vector_index_snapshot_write_after_rebuild_test.c) | [`S4 report`](../hnsw_index_snapshot/hnsw_s4_snapshot_write_after_rebuild_report.md) 记录 PASS | rebuild/VINDEX 后写；失败不阻断 search；旧 snapshot 尽量保留 | fsync、断电、两文件原子事务 |
| [`VINFO test`](../../tests/vector_index_snapshot_vinfo_status_test.c) | [`S5 report`](../hnsw_index_snapshot/hnsw_s5_vinfo_snapshot_status_report.md) 记录 PASS | load/fallback/write 状态可观测 | 外部健康检查或 READY 状态机 |
| [`cold-start harness`](../../tests/vector_index_snapshot_cold_start_bench.c) | [`S-bench report`](../hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md) 记录 quick/100K PASS | synthetic 10K/100K load-only、sanity、VINFO path | 真实文本、完整 startup、并发 SLA |

目标 Git 范围内没有提交 snapshot benchmark 的原始 JSON/CSV/log；100K fixture 也在测试后清理。因此所有耗时、大小和内存数字只能追溯到阶段报告，没有 commit-pinned raw artifact。

按证据等级总结：

**已经证明：** manifest 与 graph payload 可以 roundtrip；valid snapshot 可在 records 恢复后加载并绑定；常见 missing/mismatch/corrupt/truncate 会 fallback；writer 失败不改变 records 真值；100K synthetic load-only 有明确收益。

**基本证明：** 在阶段测试环境中，fallback rebuild 后能继续 search，并可 best-effort 生成下一份 snapshot；VINFO 能区分主要 load/write 路径。

**暂时观察到：** [`S-bench 报告`](../hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md) 中 100K load-only 平均约 5.92 秒，相对复用的 P2 records-only baseline 约 508.45 倍；该结论受 synthetic fixture、历史 baseline 和 load-only 边界限制。

**尚未证明：** 完整 server cold-start SLA、真实 embedding cold start、跨版本/跨架构兼容、断电一致性、并发 reader、后台 rebuild、安全在线发布和长期文件生命周期。

## 18.13 工程取舍、真实失败与当前缺陷

### 为什么不把 graph 放进 RDB/AOF

RDB/AOF 是业务数据兼容边界；HNSW graph 布局和参数会演进。把 graph 嵌入主格式会把 ANN 内部结构变成长期业务契约，并让损坏直接污染恢复。独立 cache 文件允许删除、失效和回退，代价是 cache miss 时仍要 rebuild。

### 为什么需要 manifest，而不是只信 binary header

binary header 能证明文件内部自洽，却不知道当前 manager records 是否还是同一批。manifest 把 records generation/count/checksum 和当前 backend/config 绑定进来；加载后再逐 record 比较 vector bytes，形成三道防线。

### 为什么不做 mmap/GC/多版本

这一版先闭合 serialize、validate、load、fallback 和 write。mmap 会改变内存所有权；GC/多版本会引入保留策略和并发文件生命周期；admin command 会扩大公开协议。这些都不是最小 cold-start cache 闭环。

### 必须保留的失败与返工

1. [`S-bench 报告`](../hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md) 中 10K quick 的 `8035.46x` 混用了 100K rebuild 与 10K load，只能作为错误口径示例。
2. 同一报告的 100K old path 没有同次重跑，`508.45x` 依赖 P2 历史 baseline。
3. manifest 通过不等于 deserialize/graph/records 校验通过；`snapshot_valid` 也不是最终 loaded 状态。
4. 文件损坏不会神奇修复；它触发昂贵 rebuild，并在成功后覆盖生成新 cache。
5. writer 有 temp/backup/rename，但无 fsync，两个文件也不是崩溃原子发布。

### 当前缺陷

1. target range 的默认路径是进程工作目录下 `vector_index.manifest` / `vector_index.snapshot`。
2. 格式只支持 exact version，没有 migration、endianness 或跨 ABI 兼容层。
3. FNV-1a 不是安全哈希。
4. basic graph validation 不证明连通性和 recall。
5. 没有 snapshot GC、多版本、mmap/view 或磁盘空间策略。
6. 没有独立 writer latency、deserialize 峰值内存和完整 server startup 测量。
7. [`100K load-only 报告`](../hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md) 中 snapshot 约 845.8 MB，会引入显著磁盘、IO 和加载期内存压力。
8. S6 只把 writer 接到既有 shadow publish skeleton/test hook；它不是在线 rebuild、reader safety 或后台 worker完成证据。

第 8 项说明了下一阶段触发条件，但本章不展开其状态机和并发设计。

## 18.14 Agent 辅助开发边界

本范围两个提交没有 `Co-Authored-By` trailer。Git 能确认 author、diff、测试资产和阶段报告，不能确认逐行由人还是 Agent 编写。

面试中可防守的责任边界是：

```text
我定义的问题：
records-only 恢复正确但 100K 构图太慢，需要 cache 而不是改变真值。

我冻结的不变量：
records 先恢复；snapshot 只在校验后加载；任何失败都可 rebuild；
snapshot write failure 不回滚 records/rebuild。

我选择的验证：
manifest/config/records 指纹 + binary header/payload checksum +
graph basic validation + manager active record/vector byte对照。

我识别的口径问题：
10K load 不能除 100K rebuild；load-only 不能写成完整 server startup；
serialized bytes 不能写成 runtime memory。

Agent 可以辅助：
生成 codec/helper、failure fixture、重复测试和报告表格。

我必须自己核验：
写入/rename/fsync 边界、temp publish safety、fallback 是否真的 rebuild、
status 字段何时置位、benchmark 是否同规模同口径。
```

真正掌握这章，应能画出 records、manifest、snapshot 三者的信任方向，并解释为什么 cache 校验失败时系统宁可慢，也不能加载可疑 graph。

## 18.15 面试表达与本章小结

### 1 分钟版本

“上一阶段 `core_hnsw_v2` 在 100K synthetic gate 下达到召回下限，但 [`P2 报告`](../hnsw_productization/hnsw_p2_100k_balanced_default_report.md) 的 records-only rebuild 约 50.2 分钟。record store 仍必须是真值，所以我没有把 graph 塞进 RDB/AOF，而是增加独立的 manifest + snapshot cache。

启动先恢复 records，再用 backend、dim、metric、M/M0/ef、active/deleted count、record generation、record ID/vector checksum 和 snapshot size/checksum 校验 manifest。通过后把 binary graph 读进临时 index，检查 entry、level、degree、neighbor、重复边和 active ID，再逐条对照 manager record/vector；全部成功才 bind。任何 missing、mismatch、truncate、corrupt 或 deserialize failure 都回到 records-only rebuild。

写入采用 temp、backup 和 rename，失败不回滚 rebuild，但没有 fsync 和两文件崩溃原子性。[`S-bench 报告`](../hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md) 中 100K synthetic load-only 5 次平均约 5.92 秒，p95 约 6.80 秒；对照的 50.2 分钟 rebuild 来自历史 P2，不是同次 A/B。snapshot 文件约 845.8 MB，也不能当作 runtime memory。这个阶段证明了可丢弃 cache 的 cold-start 收益，没有证明完整 server ready、真实 embedding SLA 或在线 rebuild。”

### 3～5 分钟版本

“第十七章的架构选择是 records 为 source of truth，graph 是 derived state。优点是 backend 可替换、图坏了能重建；缺点是 [`P2 报告`](../hnsw_productization/hnsw_p2_100k_balanced_default_report.md) 的 balanced 100K graph 构建约 50 分钟。第十八章没有推翻这个边界，而是在旁边增加一个可删除 cache。

cache 分两层。外层 manifest 是文本，绑定 manifest/snapshot version、backend、dim、metric、M/M0/efConstruction/efSearch、active/deleted count、最大 record generation、active record ID/vector checksum，以及 snapshot 文件 size/checksum。内层 binary header 保存 graph 参数、size/capacity、entry/max level、rng state、payload size/checksum；payload 才是 record IDs、vectors、levels、deleted flags 和 level0/upper neighbors。

serialize 前先做 basic graph validation。deserialize 不直接覆盖 active index，而是建 temp index，读 payload、验 checksum、重建 id map、刷新 stats，再检查 degree、neighbor range、self/duplicate edge、upper level 和 duplicate active ID。失败只销毁 temp，旧 target 不动。

产品启动必须先恢复 RDB/AOF records 和 manager map。之后读 manifest，用当前 records/config 构造 expected 值逐项比较；再 deserialize；最后逐 active slot 回 manager 查 record，比较 embedding bytes。全部通过才 bind并设置 loaded_from_snapshot。manifest missing、参数/generation/checksum mismatch、截断、损坏、graph/record mismatch 都记 reason 并 fallback rebuild。这里没有持久 DEGRADED 状态；重建也失败就返回错误，不能宣称 ready。

writer 在 fallback rebuild 或显式 VINDEX REBUILD 成功后 best-effort 执行。它写 temp，备份旧文件，再依次 rename snapshot 和 manifest；普通失败尽量 rollback。两次 rename 不是跨文件事务，而且没有 fsync，所以断电一致性尚未证明。正确性靠下次启动拒绝不匹配 cache兜底。

benchmark 也有一次值得保留的口径问题。[`S-bench 报告`](../hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md) 的 10K quick load 约 375ms，但用 100K rebuild 算出 8035 倍，这不能作为正式结果。随后 100K synthetic load-only 重复 5 次，平均约 5.92 秒、p95 约 6.80 秒，snapshot 约 845.8MB；和 P2 同规模历史 rebuild 比约 508 倍。它证明 load-only 机制有数量级收益，但不是完整 server startup、真实文本、并发 SLA，也没有测 writer latency和加载峰值内存。”

### 常见追问与回答要点

**为什么不能只保存 graph，不保存 vector？**

当前 graph slot 的搜索距离需要 vector，snapshot 又要逐 slot 与 manager embedding 对照。只存边会在启动时重新组织 vector storage，并扩大格式和恢复复杂度。

**manifest 和 binary header 为什么都要参数？**

manifest 用当前 records/config 判断 cache 是否适用；binary header保护文件内部反序列化。前者是跨层兼容门禁，后者是 anncore 文件安全门禁。

**generation 一样是否就能加载？**

不能。generation 只是一项粗粒度证据；还要比较 active/deleted count、record ID checksum、vector checksum、snapshot size/checksum，并在加载后逐 record核对。

**checksum 通过为什么还要 graph validation？**

checksum 只能说明 bytes 与写入时一致，不能说明这些 bytes 在当前代码下满足 entry、degree、neighbor 和 ID 不变量，也不能说明属于当前 records。

**graph validation 通过是否证明 recall？**

不证明。它检查结构安全，不检查全局连通性和近邻质量。benchmark 的 50-query topK sanity 也只是有限门禁。

**为什么 invalid snapshot 不直接让启动失败？**

因为 snapshot 是 cache，records 才是真值。拒绝 cache 后 rebuild能恢复正确 graph；只有 records 或 rebuild 本身失败，才应阻断 vector ready。

**两文件发布是原子的吗？**

不是完整事务。每次 rename 是单文件切换，但 snapshot 和 manifest 依次发布；没有 fsync/dir fsync。崩溃后可能不匹配，靠 startup validation 拒绝并 rebuild。

**5.92 秒是否等于 KVLite 冷启动？**

不是。它是 benchmark fixture records restore + snapshot core load，不含完整 RDB/AOF、其他模块初始化、网络监听和服务 readiness。

**845.8 MB snapshot 是否等于索引内存？**

不是。它是 serialized file size。runtime index memory、进程 RSS和 deserialize 峰值是不同口径。

### 容易夸大的内容

不能说：

```text
snapshot 成为新的业务真值；
HNSW graph 已进入 RDB/AOF；
checksum 通过就证明 graph 语义和 recall 正确；
snapshot load 成功就代表整个 server ready；
10K quick 证明 8035x 的正式 100K speedup；
100K 508x 来自同次 old/new A/B；
serialized size 就是 runtime memory；
temp + rename 已经提供完整 fsync/crash consistency；
S6 shadow hook 等于在线 rebuild 完成；
本章已经覆盖真实 T2Ranking benchmark。
```

### 本章小结

这一阶段形成的闭环是：

```text
100K records-only rebuild 成本过高
-> 保持 records/RDB/AOF 真值不变
-> manifest 绑定 records/config/file identity
-> binary snapshot 保存 graph-local state
-> temp deserialize + graph/record validation 后才 bind
-> missing/mismatch/corrupt 安全 fallback rebuild
-> rebuild 后 best-effort 写下一份 cache
-> VINFO/log 暴露 load/fallback/write
-> 10K quick 暴露口径陷阱
-> 100K load-only 证明秒级加载的阶段性收益
```

已经证明的是：同版本、同配置、同 records 的 valid snapshot 可以安全跳过昂贵构图；cache 不可信时 records 不丢、系统能回到 rebuild。

仍未解决的是：完整 crash consistency、跨版本/跨架构兼容、GC/mmap、真实 server cold-start、真实 embedding SLA，以及在线构建期间如何继续服务查询并追赶写入。

下一篇只进入“第十九章：在线 Rebuild、增量追赶与安全发布”，候选范围 `74ec6af^..74ec6af`。
