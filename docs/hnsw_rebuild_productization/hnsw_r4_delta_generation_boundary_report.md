# HNSW-R4: delta capture generation boundary report

## 1. AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`，并读取：

- `docs/hnsw_rebuild_productization/README.md`
- `docs/hnsw_rebuild_productization/index.md`
- `docs/hnsw_rebuild_productization/hnsw_r_execution_plan.md`
- `docs/hnsw_rebuild_productization/hnsw_r1_sync_rebuild_state_machine_hardening_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r2_reader_safety_primitive_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r3_active_shadow_retired_publish_report.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；HNSW-R0 路线要求 HNSW-R 阶段报告写入 `docs/hnsw_rebuild_productization/`。本报告按 HNSW-R0 路线落盘，同时继续遵守 AGENTS 的硬边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不改变默认 backend / 默认 HNSW 参数、不让 vector core 访问 manager-owned 语义状态。

## 2. 与 HNSW-R0 / R1 / R2 / R3 的一致性

本阶段只执行 HNSW-R4：delta capture generation boundary。

已遵守 R4 边界：

- 未修改 RDB/AOF。
- 未修改 `VSEARCH` response。
- 未修改 `kvs_vector_index_ops_t`。
- 未修改默认 backend 或默认 HNSW 参数。
- 未改 product active/shadow/retired publish 语义。
- 未引入 background worker。
- 未实现 R5 final validation。
- 未实现 R7 post-publish snapshot writer 新语义。

R1 状态机、R2 reader safety、R3 product active publish 均保持前序语义。本轮审计确认 R4 需要的生产语义已存在，因此只补 focused tests 和本报告，没有重复重写生产代码。

## 3. Read-only audit 摘要

当前 rebuild context 已包含 generation / delta log：

```text
kvs_vector_rebuild_context_t:
  state
  current_generation
  rebuild_generation
  delta_count
  delta_capacity
  delta_log[]
```

delta entry 只包含 R4 允许字段：

```text
record_id
vector
dim
op
generation
```

未包含 scope / TTL / metadata / question / answer / response 字段。

append 入口：

```text
kvs_vector_manager_capture_upsert_delta()
  -> kvs_vector_rebuild_append_upsert_delta()

kvs_vector_manager_capture_delete_delta()
  -> kvs_vector_rebuild_append_delete_delta()
```

VSET / VDEL 调用位置：

- `kvs_vector_core_hnsw_v2_update_existing_record()`：record / id map / active index 更新成功后递增 manager generation，再 append UPSERT delta。
- `kvs_vector_set()` 新增记录：record store / id map / record_id_map / current index 更新成功后递增 manager generation，再 append UPSERT delta。
- `kvs_vector_set_commit_prepared()` 新增记录：同样在提交成功后 append UPSERT delta。
- `kvs_vector_del()`：record mark-delete、backend mark-delete、id map / record_id_map 删除、free list 和计数更新成功后递增 manager generation，再 append DELETE delta。

generation 来源和规则：

- `kvs_vector_rebuild_begin()` 在进入 `BUILDING` 时递增 `ctx->current_generation`，并把该值记录到 `ctx->rebuild_generation`。
- capture active 状态为 `BUILDING` / `REPLAYING_DELTA` / `READY_TO_SWAP`。
- 每个 delta append 使用 `ctx->current_generation + 1`，append 成功后推进 `ctx->current_generation`。
- append 失败不推进 `ctx->current_generation`，不增加 `delta_count`。
- `ctx->rebuild_generation` 在本轮 rebuild 内保持稳定，用作 `rebuild_generation_start`。

rebuild start / capture / full scan / replay 顺序：

```text
shadow init:
  kvs_vector_rebuild_begin()
  -> state BUILDING
  -> capture active
  -> record rebuild_generation_start

full scan:
  kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test()
  -> scan current manager active records
  -> state REPLAYING_DELTA

replay:
  iterate delta_log[0..delta_count)
  -> require monotonic generation
  -> replay in append order
  -> missing DELETE is no-op
  -> state READY_TO_SWAP
```

测试缺口：

- full scan 前、capture 已开启后提交的写入没有独立用例证明不会丢。
- append failure 已有底层 append overflow 测试，但没有产品写路径用例证明 VSET/VDEL 成功后 append failure 只 fail rebuild、不回滚用户写入。
- rebuild_generation_start 稳定性缺少明确断言。

## 4. R4 实际修改内容

本轮直接修改：

- `tests/vector_rebuild_delta_capture_test.c`
- `tests/vector_rebuild_shadow_replay_test.c`
- `docs/hnsw_rebuild_productization/hnsw_r4_delta_generation_boundary_report.md`

本轮没有修改生产代码。

进入本轮前已有未提交修改：

- `docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md`
- `include/vector/kvs_vector_index_internal.h`
- `include/vector/kvs_vector_manager.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_rebuild.c`
- `src/vector/kvs_vector_rebuild_shadow.c`
- `tests/vector_core_hnsw_v2_product_integration_test.c`
- `tests/vector_index_snapshot_shadow_publish_test.c`
- `tests/vector_rebuild_state_machine_test.c`
- `docs/hnsw_rebuild_productization/`

这些属于 HNSW-S / R1 / R2 / R3 前序修改或既有脏状态，本轮未做无关重写。

## 5. Generation boundary 设计

R4 后固定的 generation boundary：

```text
rebuild_generation_start = ctx->rebuild_generation
```

它由 `kvs_vector_rebuild_begin()` 记录，并在 delta append 期间保持不变。

capture 与 full scan 顺序：

```text
begin:
  state = BUILDING
  capture active

then:
  full scan manager active records
```

因此 full scan 前已经没有未捕获写入窗口。若写入发生在 capture begin 之后、full scan 之前，可能同时出现在 baseline scan 和 delta log 中；replay UPSERT 会先删除 shadow 中同 record_id 的旧条目再追加新条目，因此该重复是幂等可接受的，不会丢写。

R5 才会记录 `rebuild_generation_end` 并做 final validation；R4 不提前实现 final validation。

## 6. VSET / VDEL delta append 语义

R4 固定语义：

```text
VSET / VDEL 用户写入先提交
提交成功后 append delta
append 返回值不回滚用户写入
append 失败只让 rebuild 进入 FAILED
```

本轮测试覆盖：

- VSET 已提交后触发 append failure，VSET 仍返回成功，记录仍可被产品搜索命中。
- VDEL 已提交后触发 append failure，VDEL 仍返回成功，记录已不可通过 VGET 找到。

## 7. Append failure 语义

append failure 通过 manager capture wrapper 进入：

```text
kvs_vector_manager_mark_rebuild_capture_failed()
  -> kvs_vector_rebuild_fail()
  -> state FAILED
  -> error_message records append failure reason
```

本轮用 generation overflow 人工触发 append failure，验证：

- `delta_count` 不增加。
- rebuild state 进入 `FAILED`。
- error reason 可观测。
- 用户写入结果不回滚。

## 8. Repeated UPSERT / DELETE missing 行为

已有并保留的 replay 语义：

- delta replay 按 `delta_log` append order 顺序执行。
- repeated UPSERT 对同一 record_id 会先 mark-delete shadow 中旧条目，再 append 新向量；最新 append 在搜索可见层面生效。
- repeated record_id 的最终一致性仍以 R5 final validation 对齐 manager latest record。
- DELETE missing in shadow 返回 `KVS_VECTOR_OK`，并计入 missing delete 统计，不 fail rebuild。

本轮没有把 scope / TTL / metadata / question / answer 写入 delta。

## 9. 未进入 R5 / R6 / R7 的说明

本阶段没有实现：

- shadow full scan + delta replay 的最终产品 validation。
- active_count / deleted_count / checksum / record_generation final validation。
- background worker / async rebuild command。
- retry / cancel / progress control plane。
- post-publish snapshot writer 新 hook。
- snapshot format 变更。
- snapshot GC / mmap。
- reader epoch / RCU / refcount。

## 10. 测试结果

构建：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

必跑测试：

```text
tests/vector_rebuild_delta_capture_test.sh
PASS

tests/vector_rebuild_shadow_replay_test.sh
PASS

tests/vector_rebuild_state_machine_test.sh
PASS

tests/vector_rebuild_shadow_swap_test.sh
PASS

tests/vector_index_snapshot_shadow_publish_test.sh
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

## 11. 风险与后续阶段

当前风险：

- R4 只固定 delta capture boundary 和 append failure 语义，仍不能证明最终 shadow 与 manager latest records 完全一致。
- full scan 前写入可能同时进入 baseline 和 delta；当前 replay 幂等处理该情况，但 R5 仍需 final validation。
- append failure 不回滚用户写入是正确产品语义，但意味着本次 rebuild 必须失败并由后续 retry / background worker 处理。
- R4 仍没有 background worker，因此不能宣称 online rebuild 已产品化完成。

下一步建议：

```text
HNSW-R5: shadow full scan + delta replay + final validation
```

R5 应基于本阶段 generation boundary，补齐 active_count / deleted_count / record_id checksum / vector checksum / record_generation / graph sanity / search sanity validation，并确保 validation 失败不得 publish。

## 12. Git checks

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
M tests/vector_rebuild_state_machine_test.c
?? docs/hnsw_rebuild_productization/
```

说明：`vector_index.manifest` 和 `vector_index.snapshot` 是测试生成的未跟踪产物，本轮已清理。

## 13. Decision

```text
HNSW-R4 decision:
  production_code_changed = no
  tests_changed = yes
  makefile_changed = no
  docs_changed = yes
  delta_capture_semantics_changed = no
  generation_boundary_defined = yes
  capture_enable_before_full_scan_cutoff = yes
  vset_vdel_append_after_commit = yes
  append_failure_marks_rebuild_failed = yes
  append_failure_rolls_back_user_write = no
  replay_order_is_append_order = yes
  repeated_upsert_latest_record_wins = yes
  delete_missing_is_noop = yes
  product_active_publish_changed = no
  background_worker_added = no
  snapshot_format_changed = no
  ready_for_HNSW_R5 = yes
  next_stage = HNSW-R5
```
