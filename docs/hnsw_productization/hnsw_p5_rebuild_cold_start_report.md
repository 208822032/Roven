# HNSW-P5: rebuild / cold-start 产品化策略报告

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

本阶段只做 rebuild / cold-start 产品化策略文档和现有 harness 验证。未修改生产代码，未修改测试，未修改 Makefile，未修改默认 HNSW 参数，未修改 VSEARCH response、RDB/AOF、`kvs_vector_index_ops_t`，未引入 background thread，未做 graph refactor、SIMD/layout/reverse_prune cache 微优化。

未发现硬边界冲突。`AGENTS.md` 的历史 stage 模板要求报告写入 `docs/vector_core_reset/`，本任务属于 HNSW 产品化路线并明确要求写入 `docs/hnsw_productization/`；本报告按任务指定目录输出，同时继续遵守 manager/vector_core 边界和持久化边界。

## 是否修改生产代码

否。

本阶段没有修改：

- `src/`
- `include/`
- `tests/`
- `Makefile`
- VSEARCH response
- RDB/AOF
- `kvs_vector_index_ops_t`
- 默认 HNSW 参数

## 修改/新增文件

新增：

- `docs/hnsw_productization/hnsw_p5_rebuild_cold_start_strategy.md`
- `docs/hnsw_productization/hnsw_p5_rebuild_cold_start_report.md`

本轮未处理既有无关 README.md，也未回退工作区中已有 P1b/P2 相关未提交改动。

## 当前 build / cold-start 风险

HNSW-P2-final balanced 默认参数：

```text
M = 48
M0 = 96
efConstruction = 512
efSearch = 1024
```

100K balanced synthetic gate：

```text
count = 100000
dim = 512
queries = 100
topK = 10
metric = IP normalized
recall@10 = 0.9740
oracle_mismatch = 0
duplicate_edge = 0
degree_overflow = 0
build_us = 3,011,810,795
memory_bytes = 851,194,400
```

结论：

- recall gate 通过，balanced default 有数据支撑。
- build 约 50.2 分钟，不能包装成生产 cold-start SLA。
- RDB/AOF 不保存 graph，load/replay 后仍需要从 active records 重建 derived graph。
- 对 100K balanced 默认参数，同步 rebuild 是主要产品化风险。

## 是否适合继续同步 rebuild

同步 rebuild 可以继续保留为：

- correctness baseline。
- 小规模数据 fallback。
- 手动维护路径。
- shadow/background rebuild 失败时的最后恢复手段。

但它不适合作为 100K balanced 默认生产 cold-start / online maintenance 策略。原因：

- 100K build 约 50 分钟，恢复时间过长。
- rebuild 期间用户可见阻塞风险高。
- 大规模 tombstone cleanup 如果同步执行，会把维护成本暴露给前台。
- 失败时必须保证旧 active index 不受影响，并给出可观测 failure reason。

## bulk import 建议

当前系统下建议：

```text
1. 先导入 records。
2. import 期间避免每批频繁 VINDEX REBUILD。
3. import 完成后执行一次 VINDEX REBUILD。
4. 大规模场景后续切换到 background shadow rebuild。
```

不建议：

- 每导入一批就 rebuild。
- 将 100K synthetic gate 通过解释为 bulk import / cold-start 已经生产就绪。
- 为了冷启动速度直接降低默认 HNSW 参数。

## tombstone rebuild policy

建议策略：

```text
deleted_count / max(size, 1) >= 0.30
  -> recommend rebuild

deleted_count / max(size, 1) >= 0.50
  -> strong recommend rebuild
```

策略解释：

- 30% 是建议 rebuild 阈值。
- 50% 是强建议 rebuild 阈值，应提示 search degradation / memory waste 风险。
- rebuild 后 `deleted_count` 应重置为 0。
- 对 100K+ 场景，不应在达到阈值时直接同步 rebuild；应由 background shadow rebuild 承担主要维护路径。

## 9F skeleton 可复用能力

### state machine

已有：

- `IDLE`
- `BUILDING`
- `REPLAYING_DELTA`
- `READY_TO_SWAP`
- `SWAPPING`
- `FAILED`
- `CANCELLED`

已有合法转移、非法转移保护、失败/取消 cleanup、`progress_total` / `progress_done`、`error_code` / `error_message`。

### delta capture

已有：

- manager-owned UPSERT / DELETE delta。
- VSET/VDEL 成功 commit 后 capture。
- generation 单调递增。
- capture inactive 时不影响写路径。
- append failure 将 rebuild context 标记 FAILED，不回滚已成功主写入。

### shadow replay

已有：

- test/internal shadow `core_hnsw_v2` index。
- 从 active records 构建 baseline。
- 按 generation replay delta。
- UPSERT latest generation wins。
- DELETE missing no-op。
- replay failure 进入 FAILED 并清理 shadow。

### swap skeleton

已有：

- active / shadow / retired opaque pointer lifecycle。
- `READY_TO_SWAP -> SWAPPING -> IDLE` no-fail pointer move skeleton。
- delayed retired cleanup helper。
- forced pre-publication failure test。
- VSEARCH 当前仍不读取 rebuild context active/shadow，产品行为未变。

### cleanup / failure

已有：

- FAILED / CANCELLED cleanup。
- shadow destroy callback。
- delta log cleanup。
- retired cleanup。
- reset-for-load / destroy cleanup。

## 仍缺的产品化能力

仍缺：

- real background worker。
- production integration。
- VINFO 或 admin 观测面的 rebuild status / progress / failure reason。
- cancellation。
- memory budget。
- delta log backpressure / compaction。
- concurrency safety。
- rebuild throttling。
- reader safety：RW lock、refcount 或 epoch/RCU。
- product current index safe publication。

这些属于 P5-impl，不在本阶段实现。

## background shadow rebuild 最小设计

P5-impl 应复用 9F skeleton，最小设计为：

```text
1. manager 进入 BUILDING，并启用 delta capture。
2. 记录 rebuild_generation / stable scan boundary。
3. background worker 创建 shadow core_hnsw_v2。
4. 扫描 active records 并写入 shadow。
5. 进入 REPLAYING_DELTA。
6. 按 generation 顺序 replay delta log。
7. 做 shadow sanity check。
8. 进入 READY_TO_SWAP。
9. 在 reader safety 保护下 no-fail publish。
10. old active 进入 retired，延迟释放。
11. 清理 delta log，记录状态和耗时。
```

必须保持：

- record store 是 source of truth。
- HNSW graph 是 derived runtime state。
- 不持久化 graph。
- 不修改 VSEARCH response。
- 不修改 RDB/AOF。
- 不修改 `kvs_vector_index_ops_t`。
- vector_core 不访问 manager 语义字段。

## 需要但本阶段不做

本阶段不做：

- background thread。
- VINDEX REBUILD 后台化。
- VINFO rebuild status 字段。
- cancellation command。
- memory budget / throttling。
- reader refcount / epoch。
- graph persistence。
- legacy `core_hnsw` 删除。
- 1M sprint。
- SIMD/layout/reverse_prune cache 微优化。

## 测试结果

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

Rebuild harness：

```text
tests/vector_rebuild_state_machine_test.sh
PASS

tests/vector_rebuild_delta_capture_test.sh
PASS

tests/vector_rebuild_shadow_replay_test.sh
PASS

tests/vector_rebuild_shadow_swap_test.sh
PASS
```

v2 policy / product tests：

```text
tests/vector_core_hnsw_v2_tombstone_policy_test.sh
PASS

tests/vector_core_hnsw_v2_default_backend_test.sh
PASS

tests/vector_core_hnsw_v2_product_integration_test.sh
PASS
```

Tombstone policy 关键输出：

```text
hnsw_v2_tombstone_policy stage=recommend size=1000 deleted=300 active=700 ratio=0.3000 recommend=1 strong=0
hnsw_v2_tombstone_policy stage=strong size=1000 deleted=500 active=500 ratio=0.5000 recommend=1 strong=1
hnsw_v2_tombstone_policy stage=after_rebuild size=500 deleted=0 active=500 ratio=0.0000 recommend=0 strong=0 info_deleted=0 memory_bytes=6606864
vector_core_hnsw_v2_tombstone_policy_test PASS
```

## 静态边界检查

均无输出：

```text
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output
```

## git diff --check

```text
PASS
```

## git status --short

```text
 M Makefile
 M docs/hnsw_productization/README.md
 M docs/hnsw_productization/hnsw_p1b_faiss_parity_report.md
 M docs/hnsw_productization/index.md
 M docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md
 M include/vector/kvs_vector_hnsw_v2.h
 M tests/vector_core_hnsw_faiss_parity_test.cpp
?? docs/hnsw_productization/hnsw_p1b_balanced_faiss_parity_report.md
?? docs/hnsw_productization/hnsw_p2_100k_balanced_default_report.md
?? docs/hnsw_productization/hnsw_p5_rebuild_cold_start_report.md
?? docs/hnsw_productization/hnsw_p5_rebuild_cold_start_strategy.md
?? tests/vector_core_hnsw_v2_100k_balanced_gate_test.c
?? tests/vector_core_hnsw_v2_100k_balanced_gate_test.sh
```

其中 P5 本阶段新增：

- `docs/hnsw_productization/hnsw_p5_rebuild_cold_start_strategy.md`
- `docs/hnsw_productization/hnsw_p5_rebuild_cold_start_report.md`

其余未提交改动为进入本阶段前已有的 P1b/P2 相关工作区状态，本阶段未回退、未处理。

## 下一步判断

建议进入 HNSW-P5-impl，但 P5-impl 的目标应是 background/shadow rebuild 产品化，不是 graph persistence、graph refactor 或 SIMD/layout 微优化。

P5-impl 的首要设计选择是 reader safety：

- RW lock：实现简单，但 read-heavy 场景可能有竞争。
- refcount：生命周期明确，但要求所有 reader 严格 acquire/release。
- epoch/RCU：读路径开销低，但实现复杂。

## P5 decision

```text
P5 decision:
  sync_rebuild_as_default = risky
  background_rebuild_needed = yes
  graph_persistence_needed = no
  next_stage = HNSW-P5-impl
```
