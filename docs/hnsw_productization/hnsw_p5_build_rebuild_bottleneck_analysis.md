# HNSW-P5-analysis: build / rebuild 瓶颈与 KVManager-anncore 分离可行性分析

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

本阶段只新增分析报告，不修改生产代码、不修改测试、不修改 Makefile、不修改 VSEARCH response、不修改 RDB/AOF、不修改 `kvs_vector_index_ops_t`、不修改默认 HNSW 参数、不引入 background thread、不做 graph persistence、graph construction refactor、SIMD/layout/reverse_prune cache 微优化。

未发现硬边界冲突。`AGENTS.md` 的历史 stage 模板要求报告写入 `docs/vector_core_reset/`，本任务属于 HNSW 产品化路线并明确要求新增 `docs/hnsw_productization/hnsw_p5_build_rebuild_bottleneck_analysis.md`；本报告按任务指定目录输出，同时继续遵守 manager/vector_core 边界和持久化边界。

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

- `docs/hnsw_productization/hnsw_p5_build_rebuild_bottleneck_analysis.md`

## 当前 call chain

当前产品路径仍是同步派生索引模型：

```text
VSET / VDEL / VSEARCH / VINDEX
  -> KVManager / current_index wrapper
  -> selected backend ops
  -> core_hnsw_v2 add/search/rebuild/info
```

cold-start 仍是：

```text
RDB/AOF load/replay
  -> restore records
  -> rebuild manager id_map
  -> bind current backend
  -> current_index.ops->rebuild()
  -> anncore add active record_id + vector
```

当前同步 `VINDEX REBUILD` 仍是：

```text
kvs_vector_index_rebuild()
  -> compact record store
  -> rebuild manager id_map
  -> rebuild exact flat / selected backend
  -> for core_hnsw_v2: kvs_vector_core_hnsw_v2_ops_rebuild()
  -> scan active records
  -> kvs_hnsw_v2_index_add(record_id, vector, dim)
```

9F skeleton 另外已有 test/internal control-plane：

```text
manager rebuild context
  -> BUILDING
  -> delta capture
  -> shadow core_hnsw_v2 build from active records
  -> replay deltas
  -> READY_TO_SWAP
  -> active/shadow/retired pointer lifecycle skeleton
```

但它还没有成为产品 VSEARCH/VINDEX 的后台 rebuild 发布路径。

## 概念区分

### build

`build` 是 anncore 从空 index 插入 N 个向量并构造 HNSW graph 的 CPU/内存成本。

对 `core_hnsw_v2` 来说，核心成本在：

```text
kvs_hnsw_v2_index_add()
  -> random level
  -> vector write / id-slot map
  -> HNSW-style layer search
  -> neighbor_select
  -> forward link
  -> reverse link / reverse prune
```

### rebuild

`rebuild` 是 KVManager 的产品化流程：

```text
scan active records
  -> 调用 anncore build/add 构建新的 derived index
  -> 处理 tombstone cleanup
  -> 处理 failure / cancel / progress
  -> publish 或保留旧 active index
```

它包含 anncore build，但还包含 manager-owned orchestration。

### cold-start

`cold-start` 是 RDB/AOF 恢复 records 后重新构建 derived graph 的过程。

当前设计中：

```text
record store 是 source of truth
HNSW graph 是 runtime derived state
RDB/AOF 保存 records，不保存 graph topology
```

因此 cold-start 必然需要某种 rebuild，除非未来另开 graph snapshot/persistence 路线。

### bulk import

`bulk import` 是大量 records 导入后构建 ANN index 的过程。

推荐语义是：

```text
先导入 records
避免每批频繁同步 rebuild
导入结束后执行一次 rebuild
大规模场景最终切换到 background shadow rebuild
```

## 核心判断

```text
build 慢是 anncore 构图成本问题。
rebuild / cold-start 不可用是产品化 orchestration 问题。
两者相关，但不是同一个问题。
```

KVManager / anncore 分离不能直接降低单次 `anncore add` 的 CPU 成本；但它能让 rebuild / cold-start 从前台同步阻塞，演进为 manager-owned background shadow rebuild、delta replay、安全发布和失败可观测。

## 当前 build 瓶颈分析

HNSW-P2-final balanced 默认参数：

```text
M = 48
M0 = 96
efConstruction = 512
efSearch = 1024
```

100K balanced gate：

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

`build_us = 3,011,810,795`，约 50.2 分钟。

当前 build 时间主要来自 `core_hnsw_v2` 循环插入：

```text
for each active record:
  kvs_hnsw_v2_index_add(record_id, vector, dim)
```

balanced 参数相比旧默认提高了：

- `M`
- `M0`
- `efConstruction`
- candidate exploration
- neighbor_select 输入规模
- reverse link / reverse prune 工作量
- 图内存和访问量

因此 build 慢的直接来源是 HNSW graph construction 的 CPU 和内存访问成本，而不是：

- KVManager / anncore 分离本身；
- VSEARCH response formatting；
- RDB/AOF serialization；
- scope / TTL / metadata filtering；
- manager record resolve；
- 是否保存 question / answer。

KVManager 分离后，manager 仍然需要调用 anncore add/build。分离不会让一次 `kvs_hnsw_v2_index_add()` 的 layer search、neighbor_select 或 reverse prune 自动变快。

如果未来要降低 build CPU 成本，应另开 P5-buildperf 或 build-performance 路线，例如：

- bulk-build API；
- parallel build；
- batched distance；
- reverse prune reuse / 更好的剪枝策略；
- level build optimization；
- allocator / memory layout；
- optional graph snapshot。

这些不属于本阶段，也不应混入 P5-impl 的 background rebuild 产品化。

## KVManager / anncore 分离能否解决 rebuild 问题

明确结论：

```text
KVManager / anncore 分离不能直接解决 build CPU 慢。
KVManager / anncore 分离可以解决 rebuild / cold-start 的产品可用性问题。
```

原因是两类问题的控制点不同。

### anncore 保持职责

anncore 应继续只提供 index-local 能力：

```text
init
reserve
add(record_id, vector)
search(query) -> record_id + score
mark_delete(record_id)
reset
destroy
info
```

anncore 不应该知道：

- scope / TTL / metadata / question / answer；
- RDB/AOF；
- VSEARCH response；
- rebuild state machine；
- manager `record_id_map`；
- `g_vector_state`。

### KVManager 负责 orchestration

KVManager 应负责：

- record store source of truth；
- 扫描 active records；
- 捕获 VSET / VDEL delta；
- 创建 shadow index；
- replay delta；
- validate shadow；
- publish active pointer；
- retain retired index；
- expose rebuild status / progress / failure。

### 分离架构的价值

分离架构的价值不在于减少单次 add 的 CPU，而在于把大 rebuild 从用户前台路径中隔离出去：

1. 可以在不污染 anncore 的情况下实现 background rebuild。
2. 可以保留 `core_flat` fallback；legacy `core_hnsw` 源码删除后，旧配置字符串只作为 `core_flat` fallback 兼容入口。
3. 可以让 RDB/AOF 继续只保存 records。
4. 可以让 HNSW graph 继续作为 runtime derived state。
5. 可以在 manager 层处理 failure / cancel / progress / memory budget。
6. 可以避免 `core_hnsw_v2` 访问 manager semantic state。

## 当前实现依据

现有代码已经符合这个分离方向：

- `kvs_vector_manager_for_each_active_embedding()` 只把 `record_id + embedding + dim` 传给回调。
- `core_hnsw_v2` 默认参数位于 `include/vector/kvs_vector_hnsw_v2.h`，core 只维护 index-local graph/search state。
- `kvs_vector_rebuild_context_t` 属于 manager-owned rebuild control-plane。
- 9F delta entry 只保存 `generation / op / record_id / vector / dim`，不保存 scope、TTL、metadata、question、answer。
- 9F shadow replay 使用 test/internal shadow `core_hnsw_v2`，仍然没有改变产品 VSEARCH/VINDEX 行为。
- 9F5 active/shadow/retired 是 pointer lifecycle skeleton，还没有正式 reader safety 和产品发布接线。

## 方案比较

| 方案 | 结论 | 优点 | 缺点 |
|---|---|---|---|
| 同步 rebuild | 保留为 correctness baseline / small-scale fallback | 简单、容易验证、无并发 swap | 100K build 约 50 分钟，cold-start 阻塞，bulk import 后维护成本高，tombstone cleanup 成本暴露给前台 |
| background shadow rebuild | 当前首选产品化方案 | 旧 active index 继续服务，shadow 构建完成后 safe swap，复用 9F skeleton，不改 RDB/AOF，不破坏边界 | 需要 reader safety、delta capture/replay 正式接线、progress/status/failure、内存预算，构建期间可能 active + shadow 双份内存 |
| graph persistence | 当前不推荐 | 理论上可降低 cold-start rebuild 时间 | 引入 RDB/AOF 格式风险，绑定 graph layout 和算法版本，后续参数/算法变更需要迁移，损坏恢复复杂，与 derived state 设计冲突 |
| anncore build 优化 | 后续可以做，但不能替代 background rebuild | 能降低单次 build CPU 成本 | 即使 50 分钟降到 15 分钟，仍需要后台 rebuild、progress、failure、safe swap 和 reader safety |

## 为什么当前首选 background shadow rebuild

background shadow rebuild 正好对应当前风险：

- 100K balanced recall gate 已通过，问题已经从“能不能搜准”转为“能不能产品化 rebuild/cold-start”。
- 同步 rebuild 会把 50 分钟级别的构图成本暴露给前台。
- 9F 已经有 state machine、delta capture、shadow replay、swap skeleton，可复用，不需要重造路线。
- 旧 active index 可以继续服务，shadow 完成并验证后再发布。
- 失败可以留在 manager 层，保留旧 active index，暴露 failure reason。
- RDB/AOF 不需要持久化 graph，仍然保持 records-only source of truth。

## 为什么不是 graph persistence

当前不建议 graph persistence：

- 它会把 `core_hnsw_v2` 的内部 graph topology 写入持久化格式。
- graph layout、neighbor selection、reverse prune、level distribution 都会变成兼容性负担。
- 后续参数 profile 或算法修复会带来迁移/版本化问题。
- graph 文件损坏后的恢复语义更复杂。
- 当前已有 records-only rebuild 正确性路径和 9F shadow rebuild skeleton，先产品化后台 rebuild 更低风险。

graph persistence 可以作为更晚的独立路线，但不应作为 P5 的首选解法。

## 为什么不是先做 graph refactor 或 SIMD 微优化

本阶段不建议直接做 graph refactor、SIMD/layout 或 reverse_prune cache：

- P1b/P2 的数据已经说明 100K balanced recall gate 可通过，当前阻断点是 rebuild/cold-start 产品化。
- SIMD/layout 优化只能降低局部 kernel 时间，不能提供 progress、failure、cancel、safe swap。
- graph refactor 可能影响 recall/degree/duplicate 安全，需要独立归因和 gate，不应混入 rebuild 产品化。
- reverse_prune cache 等 build 优化可能降低 build_us，但不会解决 reader safety、delta replay、shadow publish 和 retired lifetime。

因此：

```text
P5-impl 解决产品可用性。
P5-buildperf 解决构图速度。
两条线可以并行规划，但不要混在一个阶段。
```

## P5-impl 推荐路线

建议拆分为：

```text
P5-impl-0:
  reader safety / active-shadow-retired ownership

P5-impl-1:
  background shadow rebuild worker skeleton
  build shadow from active records

P5-impl-2:
  delta replay / safe swap product integration

P5-impl-3:
  VINFO rebuild status / progress / failure reason

P5-impl-4:
  tombstone ratio / bulk import / rebuild recommendation 联动

P5-buildperf:
  另开 build CPU 性能优化路线
```

优先级判断：

1. 先做 `P5-impl-0`，因为没有 reader safety 就不能让 VSEARCH 读可替换 active pointer。
2. 再做 `P5-impl-1/2`，把 9F skeleton 从 test/internal 串到真实 background shadow rebuild。
3. 再做 `P5-impl-3/4`，补齐产品可观测性和运维策略。
4. `P5-buildperf` 单独开线，不阻塞 P5-impl 的控制面推进。

## 安全不变量

P5-impl 后续必须保持：

- record store 是 source of truth。
- HNSW graph 是 runtime derived state。
- RDB/AOF 不保存 graph topology。
- VSEARCH response format 不变。
- `kvs_vector_index_ops_t` 不变，除非后续任务明确要求。
- `core_hnsw_v2` 不访问 `g_vector_state`。
- `core_hnsw_v2` 不访问 manager `record_id_map`。
- `core_hnsw_v2` 不处理 scope / TTL / metadata / question / answer。
- shadow rebuild 失败不得破坏旧 active index。
- swap publication 必须是 reader-safe 的 no-fail pointer move。

## 面试可讲结论

可以这样讲：

> 我们没有把 HNSW 的慢 rebuild 简单归咎于 manager 或序列化。100K balanced 的 50 分钟 build 本质是 anncore HNSW graph construction 成本，主要来自高 M/M0/efConstruction 下的 layer search、neighbor_select 和 reverse prune。KVManager 与 anncore 分离不能让单次 add 变快，但它能把 rebuild 变成可产品化的后台流程：record store 继续作为 source of truth，manager 负责 shadow build、delta replay、safe swap、failure 和 progress，anncore 只负责 record_id/vector 到 record_id/score。这个拆分让我们不用把 scope、TTL、metadata 或 RDB/AOF 语义塞进 core，也不用急着持久化 graph。下一步应该先做 reader-safe background shadow rebuild，而不是盲目做 SIMD 或 graph refactor。

## 测试计划

本阶段只新增文档分析。按任务要求仍运行现有 build 和 harness：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_rebuild_state_machine_test.sh
tests/vector_rebuild_delta_capture_test.sh
tests/vector_rebuild_shadow_replay_test.sh
tests/vector_rebuild_shadow_swap_test.sh
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
boundary grep
git diff --check
git status --short
```

结果见本报告末尾验证记录。

## P5 bottleneck decision

```text
P5 bottleneck decision:
  build_cpu_bottleneck = yes
  rebuild_productization_bottleneck = yes
  kvmanager_anncore_separation_viable = yes
  separation_solves_build_cpu = no
  separation_solves_rebuild_productization = yes
  recommended_next = HNSW-P5-impl-0
```

## 验证记录

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

v2 product/default tests：

```text
tests/vector_core_hnsw_v2_default_backend_test.sh
PASS

tests/vector_core_hnsw_v2_product_integration_test.sh
PASS
```

静态边界检查：

```text
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output
```

`git diff --check`：

```text
PASS
```

`git status --short`：

```text
?? docs/hnsw_productization/hnsw_p5_build_rebuild_bottleneck_analysis.md
```
