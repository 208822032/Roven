# HNSW-R5: shadow replay final validation report

## 1. AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`，并读取：

- `docs/hnsw_rebuild_productization/README.md`
- `docs/hnsw_rebuild_productization/index.md`
- `docs/hnsw_rebuild_productization/hnsw_r_execution_plan.md`
- `docs/hnsw_rebuild_productization/hnsw_r1_sync_rebuild_state_machine_hardening_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r2_reader_safety_primitive_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r3_active_shadow_retired_publish_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r4_delta_generation_boundary_report.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；HNSW-R0 路线要求 HNSW-R 阶段报告写入 `docs/hnsw_rebuild_productization/`。本报告按 HNSW-R0 路线落盘，同时继续遵守 AGENTS 的硬边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不改变默认 backend / 默认 HNSW 参数、不让 vector core 访问 manager-owned 语义状态。

## 2. 与 HNSW-R0-R4 的一致性

本阶段只执行 HNSW-R5：`shadow full scan + delta replay + final validation`。

已遵守 R5 边界：

- 复用 R4 的 generation boundary 和 delta append order。
- 复用 R3 的 product active publish helper，但本轮不改变 publish 语义。
- 复用 R2 的 reader safety guard，但本轮不新增 reader primitive。
- 未引入 background worker、async command、retry/cancel control plane。
- 未修改 snapshot format、RDB/AOF、`VSEARCH` response 或 `kvs_vector_index_ops_t`。
- 未把 snapshot write success 当作 rebuild correctness；validation 发生在进入 `READY_TO_SWAP` 之前。

## 3. Read-only audit 摘要

当前 shadow full scan 入口：

```text
kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test()
  -> scan manager active records
  -> add record_id + embedding into shadow core_hnsw_v2
  -> BUILDING -> REPLAYING_DELTA
```

当前 delta replay 入口：

```text
kvs_vector_rebuild_shadow_v2_replay_deltas_for_test()
  -> iterate delta_log[0..delta_count)
  -> require generation append order
  -> UPSERT mark old shadow id deleted then add latest vector
  -> DELETE missing is non-fatal no-op
```

R5 前缺口：

```text
delta replay complete
  -> direct enter READY_TO_SWAP
```

也就是说 replay 成功后尚未强制校验 shadow 是否与 manager latest records 对齐。

可复用能力：

- `kvs_hnsw_v2_index_validate_basic()` 可做 shadow graph basic validation。
- manager record store 是 source of truth，可扫描 active/deleted records。
- `kvs_hnsw_v2_slot_vector()` 可比较 shadow slot vector 与 manager record embedding。
- `kvs_hnsw_v2_index_search()` 可做 post-replay search sanity。

## 4. R5 实际修改内容

本轮直接修改：

- `src/vector/kvs_vector_rebuild_shadow.c`
- `tests/vector_rebuild_shadow_replay_test.c`
- `tests/vector_rebuild_shadow_swap_test.c`
- `docs/hnsw_rebuild_productization/hnsw_r5_shadow_replay_final_validation_report.md`

进入本轮前已有未提交修改：

- `docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md`
- `include/vector/kvs_vector_index_internal.h`
- `include/vector/kvs_vector_manager.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_rebuild.c`
- `tests/vector_core_hnsw_v2_product_integration_test.c`
- `tests/vector_index_snapshot_shadow_publish_test.c`
- `tests/vector_rebuild_delta_capture_test.c`
- `tests/vector_rebuild_state_machine_test.c`
- `docs/hnsw_rebuild_productization/`

这些属于 HNSW-S / R1 / R2 / R3 / R4 前序修改或既有脏状态，本轮未做无关重写。

## 5. Final validation 设计

`kvs_vector_rebuild_shadow_v2_replay_deltas_for_test()` 现在在 replay 完成后执行：

```text
1. shadow graph basic validation
2. manager active/deleted count summary
3. shadow active id -> manager active record 回表校验
4. shadow vector bytes == manager embedding bytes
5. order-independent record_id checksum
6. order-independent vector checksum
7. record_generation summary stability check
8. shadow search sanity
9. manager summary second pass，确认 validation 期间未变更
10. validation ok -> READY_TO_SWAP
```

checksum 采用 order-independent 摘要，因为 shadow slot 顺序可能与 manager store slot 顺序不同。重复 UPSERT 会在 shadow 内留下 tombstone，因此不要求 shadow internal `deleted_count` 等于 manager deleted record count；R5 校验的是：

- manager active/deleted count 与 manager store counters 自洽；
- shadow `deleted_count` 与 shadow deleted flags 自洽；
- shadow visible active records 与 manager active records 对齐。

## 6. Validation 成功 / 失败语义

成功：

```text
REPLAYING_DELTA
  -> replay all deltas in append order
  -> final validation succeeds
  -> enter READY_TO_SWAP
```

失败：

```text
REPLAYING_DELTA
  -> replay or final validation fails
  -> rebuild state = FAILED
  -> shadow destroyed
  -> product active unchanged
  -> READY_TO_SWAP not entered
  -> publish helper cannot publish
```

新增负向测试覆盖 fake delta record_id：delta 可以进入 shadow，但 final validation 发现该 record_id 不在 manager source of truth，rebuild 进入 `FAILED`，shadow 被销毁，product active 仍可搜索原记录。

## 7. READY_TO_SWAP 进入条件

R5 后 `READY_TO_SWAP` 的必要条件为：

```text
state == REPLAYING_DELTA
delta generation order valid
all delta replay operations succeeded
shadow graph basic validation passed
shadow active ids are exactly manager active ids
shadow vectors match manager embeddings
record_id/vector checksum match
record_generation summary stable during validation
search sanity passed
```

validation 未通过不得 publish。

## 8. 未进入 R6/R7 的说明

本阶段没有实现：

- background worker。
- async rebuild start/status/cancel/retry。
- progress control plane。
- post-publish snapshot writer 新 hook。
- snapshot format 变更。
- snapshot GC / mmap。
- reader epoch / RCU / refcount。
- VINFO 新字段。

R7 的 snapshot writer 仍只复用既有 HNSW-S / R3 shadow publish 路径，本轮没有改变其语义。

## 9. 测试结果

构建：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

必跑测试：

```text
tests/vector_rebuild_shadow_replay_test.sh
PASS

tests/vector_rebuild_shadow_swap_test.sh
PASS

tests/vector_index_snapshot_shadow_publish_test.sh
PASS

tests/vector_rebuild_delta_capture_test.sh
PASS

tests/vector_rebuild_state_machine_test.sh
PASS

tests/vector_core_hnsw_v2_product_integration_test.sh
PASS

tests/vector_core_hnsw_v2_default_backend_test.sh
PASS
```

静态边界检查：

```text
grep -RIn "g_vector_state" src/vector/core include/vector/kvs_vector_hnsw_v2.h
no output

grep -RIn "record_id_map" src/vector/core include/vector/kvs_vector_hnsw_v2.h
no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core include/vector/kvs_vector_hnsw_v2.h
no output
```

测试生成的根目录 `vector_index.manifest` / `vector_index.snapshot` 已清理。

## 10. 风险与后续阶段

当前风险：

- search sanity 是轻量抽样，不是 100K recall SLA。
- final validation 当前是同步执行；R6 引入 background worker 后需要重新验证并发窗口。
- validation 期间如果 manager records 变化，当前设计会通过 second summary pass 检测并 fail rebuild；R6/R7 需要结合 worker retry 策略处理。
- active + shadow + retired 仍会形成短期内存峰值。

下一步建议：

```text
HNSW-R6: background worker / async rebuild command
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
M tests/vector_rebuild_delta_capture_test.c
M tests/vector_rebuild_shadow_replay_test.c
M tests/vector_rebuild_shadow_swap_test.c
M tests/vector_rebuild_state_machine_test.c
?? docs/hnsw_rebuild_productization/
```

## 12. Decision

```text
HNSW-R5 decision:
  production_code_changed = yes
  tests_changed = yes
  makefile_changed = no
  docs_changed = yes
  shadow_full_scan_productized = yes
  delta_replay_productized = yes
  final_validation_added = yes
  validation_failure_blocks_publish = yes
  ready_to_swap_requires_validation = yes
  product_active_publish_changed = no
  background_worker_added = no
  snapshot_format_changed = no
  ready_for_HNSW_R6 = yes
  next_stage = HNSW-R6
```
