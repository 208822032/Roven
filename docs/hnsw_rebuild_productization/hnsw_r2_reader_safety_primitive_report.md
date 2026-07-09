# HNSW-R2: reader safety primitive report

## 1. AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`，并读取：

- `docs/hnsw_rebuild_productization/README.md`
- `docs/hnsw_rebuild_productization/index.md`
- `docs/hnsw_rebuild_productization/hnsw_r_execution_plan.md`
- `docs/hnsw_rebuild_productization/hnsw_r1_sync_rebuild_state_machine_hardening_report.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；HNSW-R0 路线明确要求 HNSW-R 阶段报告写入 `docs/hnsw_rebuild_productization/`。本报告按 HNSW-R0 路线落盘，同时继续遵守 AGENTS 的硬边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不改变默认 backend / 默认 HNSW 参数、不让 vector core 访问 manager-owned 语义状态。

## 2. 与 HNSW-R0 / R1 的一致性

本阶段只执行 HNSW-R2：建立 reader safety primitive。

已遵守 R2 边界：

- 使用第一版 `rwlock` reader safety primitive。
- `VSEARCH` 读取 product active index 时通过 acquire/release helper 持有 read lock。
- publish / cleanup 侧新增并使用 write lock helper skeleton。
- 未引入 background worker。
- 未实现 product active/shadow/retired 产品级发布。
- 未改变 delta capture 语义。
- 未修改 Makefile、RDB/AOF、`VSEARCH` response 或 `kvs_vector_index_ops_t`。

R1 state machine hardening 保持不变，本阶段只在其上增加 reader lifetime 基础。

## 3. Read-only audit 摘要

当前 VSEARCH 读路径：

```text
kvs_vector_search()
  -> kvs_vector_index_search_current()
     -> g_vector_state.current_index.ops->search()
        -> core_flat / core_hnsw_v2 search
        -> manager resolve/filter/response data assembly input

kvs_vector_search_with_prepared()
  -> kvs_vector_index_search_current()
```

当前 product active owner：

```text
g_vector_state.current_index
```

它仍是 manager-owned 统一 ops 包装对象，实际 backing state 仍是：

```text
g_vector_state.core_flat_index
g_vector_state.core_hnsw_v2_index
```

当前写 / cleanup 相关路径：

- `kvs_vector_index_bind_current()`
- `kvs_vector_index_add_current()`
- `kvs_vector_index_rebuild_current()`
- `kvs_vector_core_hnsw_v2_update_existing_record()`
- `kvs_vector_del()` 中的 current backend mark-delete
- `kvs_vector_reset_for_load()`
- `kvs_vector_destroy()`
- `kvs_vector_rebuild_swap_ready_shadow_for_test()`
- `kvs_vector_rebuild_retire_cleanup_for_test()`

R2 最小结论：

```text
rwlock 放在 kvs_vector_state_t
read acquire/release 放在 kvs_vector_index_search_current()
write helper 用于 current_index mutation / cleanup / shadow publish skeleton
```

没有使用 `kvs_owner_rwlock_*` weak wrapper，因为该 wrapper 在 executor thread 下可能 no-op，不适合作为 HNSW-R 的 active index reader lifetime primitive。

## 4. R2 实际修改内容

修改文件：

- `include/vector/kvs_vector_index_internal.h`
- `include/vector/kvs_vector_manager.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_rebuild_shadow.c`
- `tests/vector_core_hnsw_v2_product_integration_test.c`
- `docs/hnsw_rebuild_productization/hnsw_r2_reader_safety_primitive_report.md`

进入本轮前已有修改：

- `docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md`
- `src/vector/kvs_vector_rebuild.c`
- `tests/vector_rebuild_state_machine_test.c`
- `docs/hnsw_rebuild_productization/`

其中 R1 相关文件属于上一阶段；本轮未重写 R1 状态机语义。

## 5. Acquire / release 设计

新增 manager-owned state：

```text
kvs_vector_state_t.active_index_rwlock
kvs_vector_state_t.active_index_rwlock_initialized
```

新增 helper：

```text
kvs_vector_active_index_acquire_read()
kvs_vector_active_index_release_read()
kvs_vector_active_index_acquire_write()
kvs_vector_active_index_release_write()
```

读路径：

```text
kvs_vector_index_search_current()
  acquire_read
  read g_vector_state.current_index
  call ops->search
  release_read on success and error paths
```

写路径：

```text
bind / add / rebuild / reset / destroy / delete mark / v2 update
  acquire_write
  mutate current active wrapper or backing index
  release_write
```

失败路径：

- `kvs_vector_index_search_current()` 在 missing ops / invalid search ret 等路径后释放 read lock。
- 测试验证 search error path 后 writer acquire 可以成功。
- release helper 对未持锁 guard 是 no-op，避免 cleanup path 二次释放造成破坏。

## 6. Writer-side lock helper skeleton

本阶段只建立 writer-side publish / cleanup guard skeleton：

- `kvs_vector_rebuild_swap_ready_shadow_for_test()` 在 promote shadow skeleton 时持有 write lock。
- `kvs_vector_rebuild_retire_cleanup_for_test()` 在 retired cleanup 时持有 write lock。

这仍不是产品级 active publish：

```text
product current active remains g_vector_state.current_index
shadow publish harness still swaps rebuild context pointers only
VSEARCH still never reads rebuild shadow directly
```

真正的 product active/shadow/retired ownership 发布留给 HNSW-R3。

## 7. 未进入 R3/R4/R5/R6/R7 的说明

本阶段没有实现：

- product active/shadow/retired safe publish。
- retired list / delayed free / epoch / RCU。
- background rebuild worker。
- delta generation boundary 产品语义。
- shadow full scan + delta replay 产品入口。
- final validation。
- post-publish snapshot writer hook。
- VINFO 新字段。

## 8. 测试结果

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
```

补充 VSEARCH / backend 测试：

```text
tests/vector_core_hnsw_v2_default_backend_test.sh
PASS

tests/vector_core_flat_correctness_test.sh
PASS
```

R2 新增测试覆盖：

- read guard 持有期间 writer try-lock 被阻止。
- write guard 持有期间 reader try-lock 被阻止。
- `kvs_vector_index_search_current()` 错误路径释放 read lock。
- release read guard 二次调用不破坏状态。

## 9. 风险与后续阶段

当前风险：

- 第一版 rwlock 会增加 VSEARCH read path 锁开销，高 QPS 下需要后续压测确认。
- R2 只保证 active index reader lifetime 的基础，不定义产品级 publish ownership。
- VINFO / snapshot 状态仍按现有同步路径读取；如 R3 引入真正 product active pointer swap，需要重新审计所有 product active 读者。
- 当前 shadow publish harness 仍不替换 product active，因此不能用 R2 结论证明 background rebuild 已可上线。

下一步建议：

```text
HNSW-R3: product active/shadow/retired safe publish
```

R3 应在 R2 rwlock 基础上定义真正的 product active ownership、publish 成功/失败语义、old active retired cleanup 规则，并验证 VSEARCH never sees half-published index。

## 10. Git checks

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
M tests/vector_rebuild_state_machine_test.c
?? docs/hnsw_rebuild_productization/
```

说明：`docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md` 是进入 HNSW-R1/R2 前已有修改；`src/vector/kvs_vector_rebuild.c` 和 `tests/vector_rebuild_state_machine_test.c` 是 HNSW-R1 已有修改。

## 11. Decision

```text
HNSW-R2 decision:
  production_code_changed = yes
  tests_changed = yes
  makefile_changed = no
  docs_changed = yes
  reader_safety_added = yes
  reader_safety_primitive = rwlock
  vsearch_read_guard_added = yes
  writer_publish_guard_added = yes
  product_active_publish_added = no
  background_worker_added = no
  delta_capture_semantics_changed = no
  current_index_ownership_unchanged = yes
  acquire_release_balanced = yes
  ready_for_HNSW_R3 = yes
  next_stage = HNSW-R3
```
