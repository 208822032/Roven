# HNSW-R3: product active/shadow/retired safe publish report

## 1. AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`，并读取：

- `docs/hnsw_rebuild_productization/README.md`
- `docs/hnsw_rebuild_productization/index.md`
- `docs/hnsw_rebuild_productization/hnsw_r_execution_plan.md`
- `docs/hnsw_rebuild_productization/hnsw_r1_sync_rebuild_state_machine_hardening_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r2_reader_safety_primitive_report.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；HNSW-R0 路线要求 HNSW-R 阶段报告写入 `docs/hnsw_rebuild_productization/`。本报告按 HNSW-R0 路线落盘，同时继续遵守 AGENTS 的硬边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不改变默认 backend / 默认 HNSW 参数、不让 vector core 访问 manager-owned 语义状态。

## 2. 与 HNSW-R0 / R1 / R2 的一致性

本阶段只执行 HNSW-R3：product active/shadow/retired safe publish。

已遵守 R3 边界：

- 复用 R2 的 manager-owned rwlock，publish / retired cleanup 在 write lock 下执行。
- `VSEARCH` 仍只通过 product published active wrapper 读取，不直接读取 rebuild shadow。
- shadow 只有在 `READY_TO_SWAP` 且 publish 成功后才成为 product active。
- publish 失败保持 old product active 不变。
- old product active 在 publish 成功后进入 retired，并通过 reader-safe cleanup 释放。
- 未引入 background worker。
- 未改变 delta capture 产品语义。
- 未修改 RDB/AOF、`VSEARCH` response、`kvs_vector_index_ops_t`、默认 backend 或默认 HNSW 参数。

R1 state machine hardening 和 R2 reader safety primitive 保持原有语义，本阶段只在其上补齐产品 active 发布 ownership。

## 3. Read-only audit 摘要

当前 product active owner：

```text
g_vector_state.current_index
  -> manager-side ops wrapper
  -> impl = &g_vector_state.core_hnsw_v2_index 或 &g_vector_state.core_flat_index
```

当前 rebuild context owner：

```text
g_vector_state.rebuild.active_index
g_vector_state.rebuild.shadow_index
g_vector_state.rebuild.retired_index
```

R2 之后的 reader safety：

```text
VSEARCH:
  kvs_vector_index_search_current()
    acquire read lock
    search g_vector_state.current_index
    release read lock

publish / cleanup:
  acquire write lock
  mutate product active / retired
  release write lock
```

R3 前缺口：

- `kvs_vector_rebuild_promote_shadow()` 只交换 rebuild context pointer。
- product `g_vector_state.current_index` 仍指向旧 `core_hnsw_v2_index`。
- `VSEARCH` 不会看到 promoted shadow，因此 skeleton publish 不是产品级 publish。

## 4. R3 实际修改内容

修改文件：

- `src/vector/kvs_vector_rebuild_shadow.c`
- `tests/vector_index_snapshot_shadow_publish_test.c`
- `docs/hnsw_rebuild_productization/hnsw_r3_active_shadow_retired_publish_report.md`

进入本轮前已有未提交修改：

- `docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md`
- `include/vector/kvs_vector_index_internal.h`
- `include/vector/kvs_vector_manager.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_rebuild.c`
- `tests/vector_core_hnsw_v2_product_integration_test.c`
- `tests/vector_rebuild_state_machine_test.c`
- `docs/hnsw_rebuild_productization/`

这些属于前序 HNSW-S / R1 / R2 修改，本轮未做无关重写。

## 5. Product active ownership 变化

R3 后 v2 publish 语义：

```text
before publish:
  product active = g_vector_state.core_hnsw_v2_index
  rebuild shadow = heap allocated kvs_hnsw_v2_index_t

publish success under write lock:
  allocate retired holder
  retired = old g_vector_state.core_hnsw_v2_index
  g_vector_state.core_hnsw_v2_index = shadow graph state
  clear/free shadow owner shell
  rebuild.active_index = &g_vector_state.current_index
  rebuild.retired_index = retired old active
  rebuild.shadow_index = NULL

after publish:
  VSEARCH still reads g_vector_state.current_index only
  current_index.impl still points to &g_vector_state.core_hnsw_v2_index
```

实现要点：

- shadow v2 初始化继承当前 product v2 的 `M / M0 / ef_construction / ef_search`，避免 publish 后改变 active 参数。
- product active wrapper 不暴露 rebuild shadow pointer。
- old active 不立即释放，而是进入 retired。

非 v2 backend：

- `core_flat` 路径保持 context-only promote 行为，不把 v2 shadow 发布成 flat product active。

## 6. Shadow publish 成功 / 失败语义

成功：

```text
READY_TO_SWAP
  -> acquire write lock
  -> move old product active to retired
  -> move shadow into product active storage
  -> clear shadow owner
  -> state returns IDLE
  -> best-effort HNSW-S snapshot writer
  -> release write lock before snapshot write remains unchanged by this stage
```

失败：

- 非 `READY_TO_SWAP` 状态沿用 R1 invalid transition 行为。
- missing shadow / non-v2 shadow / retired already present / product active mismatch 会进入 FAILED。
- retired allocation failure 发生在 product active 修改前，old active 保持可服务。
- forced pre-publication failure 仍不写 snapshot、不创建 retired、不污染 product current active。

半发布可见性：

- product pointer move 在 R2 write lock 下执行，read lock 的 `VSEARCH` 不会并发看到中间态。
- 会失败的分配和 precondition 检查都在 product active 修改前完成。

## 7. Retired cleanup 语义

`kvs_vector_rebuild_retire_cleanup_for_test()` 继续在 active write lock 下调用 `kvs_vector_rebuild_retire_cleanup()`。

语义：

```text
publish success:
  old product active heap holder is retained as retired

retire cleanup:
  acquire write lock
  destroy retired old active
  clear retired pointer
  release write lock
```

因为 write lock 会等待已进入的 VSEARCH reader 释放 read lock，所以 retired cleanup 不会释放仍被 reader 持有的旧 active。

## 8. 未进入 R4/R5/R6/R7 的说明

本阶段没有实现：

- R4 delta generation boundary 产品语义。
- R5 shadow full scan + delta replay + final validation 产品化。
- R6 background worker / async rebuild command。
- R7 post-publish snapshot writer hook 新语义。
- reader epoch / RCU / refcount。
- snapshot format 变更。
- snapshot GC / mmap。
- VINFO 新字段。

当前 shadow replay / snapshot writer 仍复用 HNSW-S / 现有 test harness 能力。

## 9. 测试结果

构建：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

必跑测试：

```text
tests/vector_core_hnsw_v2_product_integration_test.sh
PASS

tests/vector_rebuild_state_machine_test.sh
PASS

tests/vector_rebuild_delta_capture_test.sh
PASS

tests/vector_rebuild_shadow_replay_test.sh
PASS

tests/vector_rebuild_shadow_swap_test.sh
PASS

tests/vector_index_snapshot_shadow_publish_test.sh
PASS

tests/vector_core_hnsw_v2_default_backend_test.sh
PASS

tests/vector_core_flat_correctness_test.sh
PASS
```

新增 R3 覆盖：

- manager record store 中存在、旧 product active 中不存在、shadow delta 中存在的 record，publish 前 `VSEARCH` 不命中。
- publish 成功后同一 record 通过 product `VSEARCH` 命中。
- publish 后 `current_index.impl == &g_vector_state.core_hnsw_v2_index`。
- publish 后 `rebuild.shadow_index == NULL`。
- publish 后 `rebuild.retired_index != NULL`。
- retired cleanup 后 retired 清空。

静态边界检查：

```text
grep -RIn "g_vector_state" src/vector/core include/vector/kvs_vector_hnsw_v2.h
no output

grep -RIn "record_id_map" src/vector/core include/vector/kvs_vector_hnsw_v2.h
no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core include/vector/kvs_vector_hnsw_v2.h
no output
```

## 10. 风险与后续阶段

当前风险：

- R3 只定义同步 publish ownership；没有 background worker。
- R3 仍依赖现有 shadow replay harness；完整 final validation 留给 R5。
- active + shadow + retired 会形成短期多份 v2 index 内存峰值。
- 第一版 rwlock 的 VSEARCH read path 开销仍需后续压测观察。
- snapshot writer 仍是 HNSW-S best-effort 复用；R7 才定义真实 post-product-publish hook。

下一步建议：

```text
HNSW-R4: delta capture generation boundary
```

## 11. Git checks

`git diff --check`：

```text
PASS
```

`git status --short`：

```text
M docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md
M include/vector/kvs_vector_index_internal.h
M include/vector/kvs_vector_manager.h
M src/vector/kvs_vector.c
M src/vector/kvs_vector_rebuild.c
M src/vector/kvs_vector_rebuild_shadow.c
M tests/vector_core_hnsw_v2_product_integration_test.c
M tests/vector_index_snapshot_shadow_publish_test.c
M tests/vector_rebuild_state_machine_test.c
?? docs/hnsw_rebuild_productization/
```

说明：`docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md` 是进入 HNSW-R1/R2/R3 前已有修改；`include/vector/kvs_vector_index_internal.h`、`include/vector/kvs_vector_manager.h`、`src/vector/kvs_vector.c`、`src/vector/kvs_vector_rebuild.c`、`tests/vector_core_hnsw_v2_product_integration_test.c`、`tests/vector_rebuild_state_machine_test.c` 主要来自 HNSW-R1/R2 前序阶段。本轮 R3 直接修改 `src/vector/kvs_vector_rebuild_shadow.c`、`tests/vector_index_snapshot_shadow_publish_test.c` 并新增本报告。

## 12. Decision

```text
HNSW-R3 decision:
  production_code_changed = yes
  tests_changed = yes
  makefile_changed = no
  docs_changed = yes
  reader_safety_reused = yes
  product_active_publish_added = yes
  shadow_to_active_publish_added = yes
  retired_cleanup_added = yes
  vsearch_reads_product_active_only = yes
  publish_failure_keeps_old_active = yes
  half_published_index_visible = no
  background_worker_added = no
  delta_capture_semantics_changed = no
  snapshot_format_changed = no
  ready_for_HNSW_R4 = yes
  next_stage = HNSW-R4
```
