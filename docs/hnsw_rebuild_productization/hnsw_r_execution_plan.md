# HNSW-R0: product background rebuild execution plan

## 1. Background

已读取并遵守仓库根目录 `AGENTS.md`。

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；本任务明确要求为 HNSW-R 新建独立目录 `docs/hnsw_rebuild_productization/`。本计划按任务指定目录落盘，同时继续遵守 AGENTS 的硬边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不改变默认 backend / 默认 HNSW 参数、不让 vector_core 访问 `g_vector_state`、manager `record_id_map`、scope、TTL、metadata、question、answer 或 response assembly。

本阶段是 docs-only 的路线冻结，不写生产代码、不新增测试、不开线程、不实现 publish。

## 2. Why HNSW-R after HNSW-S

HNSW-S 已完成 `core_hnsw_v2` index snapshot 最小闭环：

```text
manifest read/write/validate: yes
anncore snapshot serialize/deserialize: yes
startup load snapshot: yes
invalid snapshot fallback rebuild: yes
rebuild success write snapshot: yes
VINDEX REBUILD write snapshot: yes
VINFO snapshot status: yes
shadow publish skeleton write snapshot: yes
```

HNSW-S 已证明 valid snapshot cold-start 收益：

```text
100K / 512d / balanced default:
  records_only_rebuild_us = 3,011,810,795
  snapshot_load_us_avg = 5,923,515.60
  snapshot_load_us_p50 = 6,050,485
  snapshot_load_us_p95 = 6,803,620
  speedup = 508.45x
  cold_start_saved_time_percent = 99.80%
  VINFO_loaded_from_snapshot = 1
  fallback_rebuild_count = 0
```

结论：

```text
valid snapshot 正常启动路径已经可接受
HNSW-R 不以 background snapshot load 为第一目标
HNSW-R 的核心目标是 product-safe background rebuild
```

valid snapshot 解决的是有 cache 时的 cold-start；HNSW-R 解决的是 snapshot missing / invalid、在线参数变更、tombstone cleanup、bulk import 后 rebuild 等场景中，如何不阻塞 `VSEARCH` 并安全发布新 active index。

## 3. Why not background snapshot load first

```text
background snapshot load is not HNSW-R first priority
```

理由：

- valid snapshot load 当前 100K 已经约 5 到 7 秒量级。
- 将 snapshot load 后台化会提前引入 reader lifetime、active publish、degraded serving、fallback policy 等复杂问题。
- 这些复杂度与 HNSW-R product publish / reader safety 重叠。
- 相比收益，当前优先级低。

当前策略：

```text
snapshot valid:
  records restore
  validate manifest + snapshot
  deserialize graph
  bind current index
  allow synchronous blocking for now

snapshot invalid/missing:
  records restore
  enter degraded / flat fallback / no-ann state according to later design
  start background shadow rebuild
  replay delta
  safe publish
  best-effort write new snapshot
```

## 4. Product rebuild target architecture

目标架构：

```text
current active index:
  继续服务 VSEARCH
  只通过 product published active pointer 读取

shadow index:
  background rebuild worker 构建
  从 manager active records 扫描 baseline
  replay VSET/VDEL delta
  final validation 通过前不可被 VSEARCH 读取

retired index:
  publish 成功后保存 old active
  reader-safe 延迟回收
```

目标调用关系：

```text
VSEARCH:
  acquire reader safety
  read product published active
  search
  release reader safety

background rebuild:
  start generation boundary
  build shadow from active records
  replay captured deltas
  validate shadow
  acquire publish safety
  swap product active
  put old active into retired
  release publish safety
  best-effort write snapshot
```

## 5. HNSW-S / HNSW-R boundary

HNSW-S 负责：

```text
records restore 后加载 valid graph cache
snapshot invalid/missing/corrupt 时 fallback
rebuild / VINDEX REBUILD / shadow publish skeleton 成功后 best-effort 写 snapshot
VINFO 暴露 snapshot load/write/fallback 状态
```

HNSW-S 不负责：

```text
降低 anncore build CPU 成本
background rebuild worker
reader safety
active/shadow/retired 产品级发布
delta capture/replay 的产品级并发语义
snapshot GC
mmap serving
```

HNSW-R 负责：

```text
snapshot invalid/missing 时避免启动同步卡死在 full rebuild
online rebuild 不阻塞 VSEARCH
current active 持续服务
shadow index 后台构建
VSET/VDEL append delta
shadow replay delta
final consistency validation
reader-safe publish
retired index 延迟回收
publish 成功后复用 HNSW-S snapshot writer
```

## 6. Current skeleton audit summary

```text
current_rebuild_skeleton_found = yes
product_background_worker_exists = no
product_reader_safety_exists = no
product_active_shadow_publish_exists = no
delta_capture_product_integrated = partial
```

当前事实：

- 现有 rebuild context 已包含 state machine、generation、delta log、active / shadow / retired opaque pointer skeleton。
- 现有 shadow replay harness 可以从 manager active records 构建 `core_hnsw_v2` shadow，并按 append order replay delta。
- 现有 shadow publish 主要是 rebuild context 内部 pointer swap。
- 当前 product `VSEARCH` 仍读 `g_vector_state.current_index`。
- 当前没有 product-safe active pointer publish。
- 当前没有 refcount / epoch / rwlock / hazard pointer。
- 当前 VSET/VDEL capture wrapper 存在，但产品路径没有 background rebuild 激活入口。
- 当前 shadow publish skeleton 成功后可以 best-effort 复用 HNSW-S snapshot writer，但这仍不是完整 background rebuild 产品路径。

## 7. Stage roadmap R0-R7

### HNSW-R0: 路线冻结 / execution plan

目标：

```text
docs-only
明确 reader safety 选择
明确 active/shadow/retired ownership
明确 delta capture generation boundary
明确 HNSW-S 复用边界
明确 VINFO 字段
明确后续 R1-R7 顺序
```

允许修改范围：

```text
docs/hnsw_rebuild_productization/*
```

禁止：

```text
不写生产代码
不新增测试
不开线程
不实现 publish
```

验收标准：

```text
README.md / index.md / hnsw_r_execution_plan.md 落盘
HNSW-S / HNSW-R 边界明确
background snapshot load 非第一优先级明确
R1-R7 顺序明确
最终 decision block 明确 ready_for_HNSW_R1 = yes
```

风险：

```text
路线文档不能替代后续并发正确性测试
后续实现必须重新验证 reader safety 和 publish/free 顺序
```

### HNSW-R1: 同步 rebuild 状态机硬化

目标：

```text
围绕现有 explicit VINDEX REBUILD / rebuild context
硬化 state machine、error path、cancel/fail 状态
仍不引入 background worker
仍不做 product active publish
```

允许修改范围：

```text
include/vector/kvs_vector_rebuild.h
src/vector/kvs_vector_rebuild.c
tests/vector_rebuild_state_machine_test.c
tests/vector_rebuild_state_machine_test.sh
docs/hnsw_rebuild_productization/hnsw_r1_sync_rebuild_state_machine_hardening_report.md
```

禁止：

```text
不改变 VSEARCH response
不改变 RDB/AOF
不改变 kvs_vector_index_ops_t
不引入 background thread
不发布 product active/shadow swap
```

验收标准：

```text
state transition deterministic
failure reason 可观测
重复 rebuild / failed rebuild / cancel 状态明确
不影响当前同步 VINDEX REBUILD 行为
```

风险：

```text
状态机硬化若混入 publish，会提前触发 reader lifetime 风险
错误状态 cleanup 必须不释放 product current_index
```

### HNSW-R2: reader safety primitive

目标：

```text
为 VSEARCH 读取 product active index 和 publish/retired cleanup 建立第一版 reader safety primitive
```

第一版建议：

```text
rwlock
```

方案比较：

| 方案 | 优点 | 代价 | R2 结论 |
| --- | --- | --- | --- |
| rwlock | 实现简单，reader 持读锁，publish / retired cleanup 持写锁，正确性容易验证 | 高 QPS VSEARCH 下可能有锁竞争，publish 可能等待长查询 | 第一版采用 |
| refcount | old active 可在 refcount 到 0 后释放，读路径可比 rwlock 更细粒度 | 每个 reader 必须严格 acquire/release，错误路径必须无遗漏 | 暂缓 |
| epoch / RCU | read-heavy 场景开销低 | 实现复杂，需要 epoch advance、quiescent state 和 retired list 管理 | 暂缓 |

建议结论：

```text
第一版使用 rwlock
VSEARCH 获取 read lock
publish / retired cleanup 获取 write lock
先保证正确性，再评估读路径开销
```

允许修改范围：

```text
manager-owned active pointer access layer
VSEARCH active acquire/release path
publish/cleanup locking helper
focused reader-safety tests
```

禁止：

```text
不要直接上 epoch / RCU，除非已有读路径开销数据证明 rwlock 不可接受
不改变 VSEARCH response
不改变 RDB/AOF
不改变 kvs_vector_index_ops_t
```

验收标准：

```text
VSEARCH acquire/release 成对
publish/cleanup 在 write lock 下执行
old active 不会在 reader 持有期间释放
故障路径也 release read lock
```

风险：

```text
reader lock 开销可能影响高 QPS VSEARCH
错误路径漏 release 会导致 publish 永久阻塞
```

### HNSW-R3: product active/shadow/retired safe publish

目标：

```text
真正定义 product active pointer ownership
shadow publish 成功后替换 g_vector_state.current_index 或等价 product active owner
old active 进入 retired
reader-safe cleanup
```

必须明确：

```text
VSEARCH 只能读 product published active
VSEARCH 不能读 rebuild context shadow
shadow 只有 publish 成功后才成为 product active
publish 失败不影响 current active
retired cleanup 不得破坏正在读的 VSEARCH
```

允许修改范围：

```text
manager-owned active index ownership helper
rebuild publish helper
retired cleanup helper
focused active/shadow/retired tests
```

禁止：

```text
不改变 response format
不改变 index ops signature
不把 shadow 暴露给 VSEARCH
不在 validation 前 publish
```

验收标准：

```text
publish 成功：new shadow becomes product active
publish 失败：old current active remains serving
old active enters retired after successful publish
retired cleanup waits for reader safety
VSEARCH never sees half-published index
```

风险：

```text
publish/free 顺序错误可能导致 use-after-free
双份 active + shadow 内存峰值需要观测
```

### HNSW-R4: delta capture generation boundary

目标：

```text
定义 rebuild start generation
定义 full scan cutoff
定义何时开启 delta capture
定义 VSET/VDEL commit 后 append delta
定义 append failure 语义
```

必须写清楚：

```text
用户写入提交成功后再 append delta
delta append failure marks rebuild failed
delta append failure 不回滚用户写入
delta replay 按 append order
同一 record 多次 update 以 latest manager record 为最终校验标准
delete missing in shadow 是非致命 no-op
```

允许修改范围：

```text
manager write-path capture wrapper
generation boundary helper
delta append failure handling
delta capture tests
```

禁止：

```text
不把 scope/TTL/metadata/question/answer 写入 delta
不回滚已成功用户写入
不改变 RDB/AOF
```

验收标准：

```text
capture start 与 full scan cutoff 无丢写窗口
append order == replay order
append failure 让 rebuild failed 且不影响用户写入返回语义
repeated UPSERT latest manager record wins
missing DELETE no-op
```

风险：

```text
delta capture boundary 错误可能导致 shadow 丢写入
append failure 不能回滚用户写入，只能 fail rebuild
```

### HNSW-R5: shadow full scan + delta replay + final validation

目标：

```text
基于 manager active records 构建 shadow
replay captured deltas
final validation 对齐 manager latest records
validation 成功才允许进入 READY_TO_SWAP / publish
```

必须包含：

```text
active_count / deleted_count validation
record_id checksum validation
vector checksum validation
record_generation validation
shadow graph basic validation
search sanity validation
```

允许修改范围：

```text
shadow build/replay harness productization
final validation helpers
validation tests
```

禁止：

```text
validation 未通过不得 publish
不把 snapshot write 成功当作 rebuild correctness
不修改 graph serialization format
```

验收标准：

```text
shadow visible records match manager active records
delta replay completes in generation order
final validation mismatch enters FAILED
READY_TO_SWAP only after validation succeeds
```

风险：

```text
校验过弱会发布不完整 shadow
校验过强可能因非语义差异误报失败
```

### HNSW-R6: background worker / async rebuild command

目标：

```text
在 R2/R3/R4/R5 完成后才引入 background worker
支持 async rebuild start / status / cancel / retry
支持 progress
```

禁止：

```text
不要在 reader safety 和 product publish 之前开 background thread
```

原因：

```text
unsafe part is not the thread; unsafe part is reader lifetime and publish semantics
```

允许修改范围：

```text
background worker lifecycle
async rebuild command/control plane
status/cancel/retry plumbing
worker tests
```

禁止：

```text
不改变 VSEARCH response
不改变 RDB/AOF
不改变 kvs_vector_index_ops_t
不绕过 R2/R3/R4/R5 的 safety gate
```

验收标准：

```text
async rebuild start returns quickly
VSEARCH continues using current active
cancel/retry/failure deterministic
progress fields updated
worker exit and shutdown cleanup safe
```

风险：

```text
background worker 过早引入会放大并发问题
shutdown/cancel 与 publish race 需要专项测试
```

### HNSW-R7: post-publish snapshot writer hook

目标：

```text
real product publish 成功后复用 HNSW-S snapshot writer
```

必须明确：

```text
snapshot writer 是 best-effort
snapshot write failure 不回滚 publish
snapshot write failure 不 fail rebuild
snapshot write failure 不影响 active search
HNSW-R 不重新设计 snapshot format
HNSW-R 不重复实现 snapshot serialize/deserialize
```

允许修改范围：

```text
post-publish snapshot writer hook
snapshot status update after product publish
focused tests
```

禁止：

```text
不重新设计 snapshot format
不把 snapshot 变成 source of truth
不修改 RDB/AOF 主格式
```

验收标准：

```text
publish success triggers best-effort snapshot write
snapshot write failure leaves active search usable
VINFO write status reflects outcome
startup snapshot loader remains HNSW-S implementation
```

风险：

```text
snapshot write 成功不能代表 rebuild correctness，必须先通过 final validation
snapshot write IO 不能延长 publish critical section
```

## 8. Reader safety decision

第一版选择：

```text
reader_safety_first_choice = rwlock
```

设计原则：

- `VSEARCH` 进入 active index search 前获取 read lock，离开时 release。
- publish 和 retired cleanup 获取 write lock。
- write lock 内只做 no-fail pointer publication / retired pointer move / safe cleanup，不做耗时 build。
- 后续用 VSEARCH profile 判断 rwlock 是否成为瓶颈。

暂不直接使用 epoch / RCU 的原因：

- 当前还没有读路径锁开销数据证明 rwlock 不可接受。
- epoch / RCU 会引入 retired list、epoch advance、quiescent state 和 shutdown cleanup 复杂度。
- HNSW-R 第一版目标是产品正确性，不是低开销 reader lifetime 的最终形态。

## 9. Active/shadow/retired ownership

ownership 规则：

```text
product active:
  manager-owned product published pointer
  only reader-safe acquire path exposes it to VSEARCH

rebuild shadow:
  rebuild-owned opaque index
  visible only to rebuild worker / validation
  cannot be searched by product VSEARCH

retired:
  old product active after successful publish
  destroyed only after reader safety confirms no in-flight reader
```

publish 规则：

```text
validation succeeds
  -> acquire write lock
  -> move product active to retired
  -> move shadow to product active
  -> clear shadow owner
  -> update epochs / observability
  -> release write lock
  -> best-effort snapshot writer

validation fails or publish precondition fails
  -> product active unchanged
  -> shadow destroyed or retained for diagnostics according to R-stage policy
  -> rebuild state FAILED
```

## 10. Delta capture and generation boundary

generation 规则：

```text
rebuild start:
  increment / record rebuild_generation_start
  enable capture before full scan window can miss committed writes

full scan:
  scan manager active records at defined cutoff
  build shadow baseline

write path:
  VSET/VDEL user commit succeeds
  append UPSERT/DELETE delta with monotonic generation
  append failure marks rebuild failed
  append failure does not roll back user write

replay:
  replay delta append order
  repeated record_id uses latest manager record during final validation
  DELETE missing in shadow is non-fatal no-op

final:
  record rebuild_generation_end
  validate shadow against manager latest records
```

delta entry 仍只允许包含：

```text
record_id
vector
dim
op
generation
```

不得包含 scope / TTL / metadata / question / answer / response fields。

## 11. Snapshot writer reuse rule

HNSW-R 复用 HNSW-S snapshot writer，不重新设计 snapshot format。

规则：

```text
publish 成功后:
  call HNSW-S best-effort snapshot writer
  update snapshot write status

snapshot write failure:
  does not roll back publish
  does not fail rebuild
  does not affect active search
  remains observable through VINFO snapshot fields
```

snapshot 仍是 derived cache，不是 source of truth。RDB/AOF 仍保存 records，load / replay / rebuild 仍以 manager record store 为事实来源。

## 12. VINFO / observability plan

建议字段：

```text
rebuild_supported
rebuild_in_progress
rebuild_state
rebuild_started_ms
rebuild_finished_ms
rebuild_progress_records
rebuild_total_records
rebuild_delta_count
rebuild_replay_count
rebuild_last_error
rebuild_generation_start
rebuild_generation_end
rebuild_publish_epoch
active_index_epoch
retired_pending_count
```

阶段归属：

| 字段 | 引入阶段 | 说明 |
| --- | --- | --- |
| rebuild_supported | R1 | 后端是否支持 product rebuild |
| rebuild_in_progress | R1 | 当前是否处于 active rebuild state |
| rebuild_state | R1 | 状态机可观测 |
| rebuild_started_ms | R1 | rebuild 开始时间 |
| rebuild_finished_ms | R1 | rebuild 结束时间 |
| rebuild_progress_records | R1/R6 | R1 可用于同步流程，R6 接 worker progress |
| rebuild_total_records | R1/R6 | 计划扫描/构建总数 |
| rebuild_delta_count | R4 | delta capture 数量 |
| rebuild_replay_count | R5 | replay 已处理数量 |
| rebuild_last_error | R1 | failure reason |
| rebuild_generation_start | R4 | capture boundary |
| rebuild_generation_end | R5 | final validation boundary |
| rebuild_publish_epoch | R3 | publish 成功 epoch |
| active_index_epoch | R3 | 当前 active index epoch |
| retired_pending_count | R3 | 待安全回收 retired index 数 |

这些字段属于 manager-owned observability，不进入 RDB/AOF，不改变 `VSEARCH` response。

## 13. Risks

至少需要跟踪以下风险：

- reader lock 开销可能影响高 QPS VSEARCH。
- publish/free 顺序错误可能导致 use-after-free。
- delta capture boundary 错误可能导致 shadow 丢写入。
- append failure 不能回滚用户写入，只能 fail rebuild。
- background worker 过早引入会放大并发问题。
- snapshot write 成功不能代表 rebuild correctness，必须先通过 final validation。
- active + shadow + retired 可能形成短期多份 index 内存峰值。
- shutdown/cancel/retry 与 publish race 需要独立测试覆盖。

## 14. Non-goals

```text
不优化 anncore build CPU
不做 HNSW buildperf
不做 snapshot mmap
不做 snapshot GC
不改变 RDB/AOF source-of-truth
不改变 VSEARCH response
不改变 kvs_vector_index_ops_t
不改变默认 HNSW 参数
不把 snapshot 变成 source of truth
不把 background snapshot load 作为第一阶段目标
不实现 admin command
不实现 HNSW-R worker in R0
```

## 15. Acceptance checklist

- [x] docs/hnsw_rebuild_productization/README.md added
- [x] docs/hnsw_rebuild_productization/index.md added
- [x] docs/hnsw_rebuild_productization/hnsw_r_execution_plan.md added
- [x] HNSW-S boundary documented
- [x] background snapshot load deprioritized
- [x] R0-R7 roadmap documented
- [x] reader safety first implementation selected
- [x] active/shadow/retired ownership documented
- [x] delta generation boundary documented
- [x] snapshot writer reuse documented
- [x] VINFO fields proposed
- [x] no production code changed
- [x] no tests changed
- [x] no Makefile changed

## 16. Final decision block

```text
HNSW-R0 execution plan decision:
  production_code_changed = no
  tests_changed = no
  makefile_changed = no
  docs_changed = yes
  docs_dir = docs/hnsw_rebuild_productization
  background_snapshot_load_first = no
  hnsw_r_primary_goal = product_safe_background_rebuild
  reader_safety_first_choice = rwlock
  product_active_publish_defined = yes
  delta_capture_boundary_defined = yes
  hnsw_s_reuse_boundary_defined = yes
  ready_for_HNSW_R1 = yes
  next_stage = HNSW-R1
```
