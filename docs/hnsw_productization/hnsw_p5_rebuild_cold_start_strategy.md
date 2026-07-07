# HNSW-P5: rebuild / cold-start 产品化策略

## 定位

HNSW-P2-final 已将 `core_hnsw_v2` 默认参数固化为 balanced profile：

```text
M = 48
M0 = 96
efConstruction = 512
efSearch = 1024
```

100K/512d synthetic balanced gate 的 recall 达标：

```text
recall@10 = 0.9740
oracle_mismatch = 0
duplicate_edge = 0
degree_overflow = 0
build_us = 3,011,810,795
memory_bytes = 851,194,400
```

这说明 balanced 参数有 100K recall gate 数据支撑，但不等于完整生产 SLA。build 约 50.2 分钟，说明 rebuild / cold-start 已经成为产品化主风险。本阶段不继续提升 recall，不做 SIMD/layout/reverse_prune cache 微优化，不实现后台线程，只定义 rebuild / cold-start 策略。

## 当前 rebuild 路径

当前产品路径仍以 record store 为 source of truth，HNSW graph 是 runtime derived state。

同步 rebuild 的逻辑模型是：

```text
VINDEX REBUILD
  -> manager 扫描 active records
  -> reset 当前 derived index
  -> reserve active capacity
  -> 将 active record_id + vector 重新 add 到 backend
  -> rebuild 后 tombstone graph slot 被清除
```

这个路径的优点是语义简单、正确性容易验证、不会改变 RDB/AOF 格式，也不会引入并发发布问题。缺点是对大规模 HNSW v2 balanced 参数非常慢。100K build 约 50 分钟时，同步 rebuild 不能作为最终生产级 cold-start / online maintenance 策略。

## 当前 cold-start 路径

当前 cold-start 仍遵守项目持久化边界：

```text
RDB/AOF load/replay
  -> 恢复 records
  -> 从 active records 重建 derived index
```

也就是说，cold-start 不是直接加载 HNSW graph，而是从 record store 重新构建图。这个策略保证了：

- RDB/AOF 只保存用户记录，不保存 index 内部拓扑。
- HNSW graph 可以随实现升级重建，不绑定持久化格式。
- `core_flat` 和 `core_hnsw_v2` 都保持 derived state 语义；legacy `core_hnsw` 源码删除后，只剩配置兼容面解析到 `core_flat` fallback。

风险是：当 `core_hnsw_v2` 使用 balanced 默认参数并达到 100K 规模时，cold-start rebuild 可能成为服务恢复的主要阻塞项。

## 为什么 RDB/AOF 不保存 graph

本阶段不建议做 graph persistence。

原因：

- HNSW graph 是 derived runtime state，record store 才是 source of truth。
- 持久化 graph 会把内部 layout、level 分布、neighbor pruning 细节写入 RDB/AOF，扩大格式兼容风险。
- 一旦 graph 格式进入 RDB/AOF，后续算法修复、layout 调整和参数 profile 变更都需要迁移逻辑。
- 当前主要风险是 rebuild/cold-start 的发布方式，而不是 graph 无法从 records 正确重建。
- 9F skeleton 已经提供 shadow rebuild / delta replay / safe swap 的基础，应该优先沿这条路线产品化。

结论：graph persistence 当前不是必要条件，也不是 P5 的推荐方向。

## 100K build 风险解释

100K balanced gate 的 build 成本：

```text
build_us = 3,011,810,795
约 50.2 分钟
memory_bytes = 851,194,400
```

这带来几个用户可见风险：

- `VINDEX REBUILD` 同步执行时会长时间占用 rebuild 路径。
- cold-start 需要等待 derived graph 重建完成，服务恢复时间不可接受。
- rebuild 期间内存峰值可能接近 active index + shadow/index scratch 的叠加。
- 如果 rebuild 中途失败，需要明确保留旧 active index，失败信息必须可观测。
- 如果用户在 bulk import 中频繁触发 rebuild，总耗时会被重复放大。

因此，100K balanced gate 只能证明 recall gate 通过，不能包装成 “100K 生产 cold-start SLA”。

## bulk import 推荐流程

当前系统下，bulk import 应优先使用以下流程：

```text
1. 先导入 records。
2. import 期间避免每批频繁 VINDEX REBUILD。
3. import 完成后执行一次 VINDEX REBUILD。
4. 大规模场景后续切到 shadow/background rebuild。
```

原因：

- 每批 rebuild 会重复扫描和构图，成本近似按批次数放大。
- record store 是 source of truth，先导入 records 不破坏持久化语义。
- 一次性 rebuild 可以清除 tombstone 并生成完整 graph。
- 对 100K+ 或更大规模，最终应由 background shadow rebuild 接管，避免同步阻塞。

## VINDEX REBUILD 风险提示

本阶段不新增用户命令，也不修改 VSEARCH response。后续 P5-impl 应考虑让 `VINDEX REBUILD` 或 `VINFO` 暴露风险信息：

- backend name 和参数 profile。
- active record count。
- tombstone ratio。
- last rebuild duration。
- current rebuild state。
- progress done / total。
- failure code / reason。
- 是否处于 strong rebuild recommendation。

在 100K balanced 参数下，`VINDEX REBUILD` 应被视为高成本维护操作，而不是轻量在线命令。

## tombstone ratio policy

`core_hnsw_v2` 使用 tombstone delete。delete 后旧 graph slot 保留为 routing node，但不会作为 search result 返回。高 tombstone ratio 会增加内存和搜索噪声，需要 rebuild policy。

建议策略：

```text
deleted_count / max(size, 1) >= 0.30
  -> recommend rebuild

deleted_count / max(size, 1) >= 0.50
  -> strong recommend rebuild
```

策略含义：

- 30% 是运维建议阈值，不自动触发 rebuild。
- 50% 是强建议阈值，应提示 search degradation / memory waste 风险。
- rebuild 后 `deleted_count` 应回到 0。
- 对大规模数据，不应简单在达到阈值时同步 rebuild，而应进入 background shadow rebuild。

## 9F skeleton 对接审阅

现有 9F skeleton 可复用能力如下。

### state machine

已有：

- `IDLE`
- `BUILDING`
- `REPLAYING_DELTA`
- `READY_TO_SWAP`
- `SWAPPING`
- `FAILED`
- `CANCELLED`

已有合法状态转移、非法转移保护、失败/取消 cleanup、error code/message、progress done/total 占位字段。

仍缺：

- 用户可见状态暴露。
- cancellation 的产品入口。
- rebuild lifecycle 与真实后台 worker 的锁边界。

### delta capture

已有：

- manager-owned UPSERT / DELETE delta log。
- VSET/VDEL 成功 commit 后 capture。
- generation 单调递增。
- capture inactive 时不改变写路径。
- append 失败时标记 rebuild failed，但不回滚已成功主写入。

仍缺：

- 与真实 background rebuild 的并发写锁策略。
- delta log memory budget。
- 长时间 rebuild 下的 delta log backpressure。
- TTL expiry 独立 commit 点接入，如后续产品语义需要。

### shadow replay

已有：

- test/internal shadow `core_hnsw_v2` index。
- 从 active records 构建 baseline。
- 按 generation 顺序 replay delta。
- UPSERT latest generation wins。
- DELETE missing treated as non-fatal no-op。
- replay failure 进入 FAILED 并销毁 shadow。

仍缺：

- 真实 production shadow worker。
- 大规模 replay 分批/限速。
- shadow build 与 active scan 的稳定边界协议。
- replay 后的线上一致性校验策略。

### swap skeleton

已有：

- active / shadow / retired opaque pointer lifecycle。
- `READY_TO_SWAP -> SWAPPING -> IDLE` no-fail pointer move skeleton。
- delayed retired cleanup skeleton。
- forced pre-publication failure 测试。
- VSEARCH 仍不读取 rebuild context active/shadow，产品行为未改变。

仍缺：

- VSEARCH reader safety 方案：RW lock、refcount 或 epoch/RCU。
- retired index 真正延迟释放策略。
- swap 发布到产品 current index 的正式接线。
- rollback / observability / failure reporting 的产品入口。

### cleanup / failure

已有：

- active state 可进入 FAILED / CANCELLED。
- shadow cleanup。
- delta log cleanup。
- reset-for-load / destroy cleanup。
- retired cleanup helper。

仍缺：

- 用户可见 failure reason。
- retry policy。
- cancellation command 或 admin hook。
- memory pressure 下的 fail-fast policy。

## background shadow rebuild 最小设计

P5-impl 的最小产品化路径应复用 9F skeleton，不重造 rebuild 设计。

建议流程：

```text
1. manager 进入 BUILDING，并在同一控制边界启用 delta capture。
2. 记录 rebuild_generation，建立稳定 scan boundary。
3. background worker 创建 shadow core_hnsw_v2。
4. worker 扫描 active records，写入 shadow index。
5. 进入 REPLAYING_DELTA。
6. 按 generation 顺序 replay delta log。
7. 校验 shadow visible count、deleted_count、基础 search sanity。
8. 进入 READY_TO_SWAP。
9. 在 reader safety 保护下做 no-fail pointer publish。
10. old active 进入 retired，等待安全释放。
11. 清理 delta log，记录 last rebuild duration/status。
```

必须保持：

- record store 仍是 source of truth。
- RDB/AOF 不保存 graph。
- vector_core 不访问 manager 语义字段。
- VSEARCH response 不变。
- `kvs_vector_index_ops_t` 不变。

## 状态、进度和失败信息

现有 rebuild context 已经有：

- `state`
- `progress_total`
- `progress_done`
- `error_code`
- `error_message`
- `cancel_requested`
- `current_generation`
- `rebuild_generation`
- `delta_count`

P5-impl 应将其中一部分暴露给产品观测面，优先考虑 `VINFO` 或 admin-only profile 字段，而不是修改 VSEARCH response。

建议暴露：

- rebuild state。
- progress done / total。
- delta count。
- last error code / message。
- tombstone ratio。
- last rebuild duration。
- rebuild recommendation level。

## 本阶段现在做什么

本阶段只做：

- rebuild / cold-start 风险说明。
- bulk import 建议。
- tombstone policy 与 rebuild 策略结合。
- 9F skeleton 复用审阅。
- P5-impl 最小设计边界。
- 运行现有 rebuild harness 和 v2 product/default/tombstone policy 测试。

## 留到 P5-impl 的内容

以下内容不在本阶段实现：

- real background worker。
- reader safety 方案落地。
- product current index 发布接线。
- VINFO rebuild status 字段。
- cancellation / retry / throttling。
- memory budget / backpressure。
- delta log compaction。
- rebuild progress 精准统计。
- shadow rebuild 与 VINDEX 的正式命令策略。

## 当前不建议做

在 P5-impl 前不建议直接做：

- graph persistence。
- 重新引入或继续依赖已删除 legacy `core_hnsw`。
- graph construction refactor。
- SIMD/layout 微优化。
- reverse_prune cache。
- 无 100K/real-data 风险归因的 1M sprint。

## 策略结论

```text
sync rebuild:
  保留为 correctness baseline / small-scale fallback。
  不适合作为 100K balanced 默认生产 cold-start 策略。

background shadow rebuild:
  是下一步产品化方向。
  应复用 9F state machine / delta capture / shadow replay / swap skeleton。

graph persistence:
  当前不需要。
  不应作为 P5 的优先方案。
```
