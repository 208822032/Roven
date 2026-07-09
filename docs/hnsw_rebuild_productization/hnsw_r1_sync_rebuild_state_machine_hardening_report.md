# HNSW-R1: sync rebuild state machine hardening report

## 1. AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`，并读取：

- `docs/hnsw_rebuild_productization/README.md`
- `docs/hnsw_rebuild_productization/index.md`
- `docs/hnsw_rebuild_productization/hnsw_r_execution_plan.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；HNSW-R0 路线明确要求 HNSW-R 阶段报告写入 `docs/hnsw_rebuild_productization/`。本报告按 HNSW-R0 路线落盘，同时继续遵守 AGENTS 的硬边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不改变默认 backend / 默认 HNSW 参数、不让 vector core 访问 manager-owned 语义状态。

## 2. 与 HNSW-R0 文档的一致性

本阶段只执行 HNSW-R1：同步 rebuild state machine hardening。

已遵守 HNSW-R0 的 R1 边界：

- 未引入 background worker。
- 未引入 reader safety primitive。
- 未实现 product active/shadow/retired reader-safe publish。
- 未修改 delta capture 产品语义。
- 未修改 VSEARCH response。
- 未修改 RDB/AOF。
- 未修改 `kvs_vector_index_ops_t`。
- 未修改 Makefile。

## 3. 修改文件列表

- `src/vector/kvs_vector_rebuild.c`
- `tests/vector_rebuild_state_machine_test.c`
- `docs/hnsw_rebuild_productization/hnsw_r1_sync_rebuild_state_machine_hardening_report.md`

进入本轮前已有修改：

- `docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md`

该文件不属于 HNSW-R1，本轮未做无关修改。

## 4. 当前 state machine audit 摘要

现有 rebuild context 状态：

```text
IDLE
BUILDING
REPLAYING_DELTA
READY_TO_SWAP
SWAPPING
FAILED
CANCELLED
```

现有主路径：

```text
begin:
  IDLE -> BUILDING

baseline build complete:
  BUILDING -> REPLAYING_DELTA

delta replay complete:
  REPLAYING_DELTA -> READY_TO_SWAP

explicit swap path:
  READY_TO_SWAP -> SWAPPING -> IDLE

promote shadow helper:
  READY_TO_SWAP -> SWAPPING -> IDLE

error:
  active states -> FAILED

cancel:
  active states -> CANCELLED

cleanup:
  FAILED / CANCELLED / IDLE -> IDLE
```

审计发现的 R1 缺口：

- 非法 transition 只返回 `KVS_VECTOR_ERR`，没有稳定写入可观测原因。
- 重复 begin、terminal failed/cancelled 后继续推进的行为缺少显式测试。
- cancel 状态没有可观测 reason。
- cleanup 对 borrowed product current placeholder 和 owned rebuild pointer 的 ownership 规则缺少单元测试。

已有覆盖：

- 基本合法 transition。
- fail / cancel 后 reset。
- shadow replay / shadow swap harness 验证当前 product `VSEARCH` 仍读 `g_vector_state.current_index`。
- forced pre-publication swap failure 不污染 product current index。

## 5. R1 实际补强内容

`src/vector/kvs_vector_rebuild.c`：

- 新增内部 helper `kvs_vector_rebuild_reject_transition()`。
- 非法 transition 统一写入：

```text
error_code = KVS_VECTOR_ERR
error_message = "invalid transition: <operation> from <state>"
```

- `begin()` 在非 `IDLE` 状态下写入明确 reason。
- `begin()` 在 generation overflow 时写入明确 reason。
- `enter_replaying_delta()` / `enter_ready_to_swap()` / `enter_swapping()` / `finish_swap()` / `promote_shadow()` 在非法状态下写入明确 reason。
- `promote_shadow()` 对 missing shadow 和 retired already present 写入明确 reason。
- 成功的合法 transition 会清理之前的 transient transition error。
- `cancel()` 进入 `CANCELLED` 时写入 `rebuild cancelled` reason。
- repeated `fail()` / repeated `cancel()` 在 terminal 状态返回错误但不覆盖已有 terminal reason。
- `reset_idle()` 在 active 状态下拒绝 cleanup，并写入明确 reason。

`tests/vector_rebuild_state_machine_test.c`：

- 验证非法 transition 记录 reason 且不改变状态。
- 验证合法 transition 会清理 transient error。
- 验证 repeated begin while building 被拒绝且状态保持。
- 验证 repeated fail 保留第一次 failure reason。
- 验证 begin from failed 必须先 reset。
- 验证 cancel 写入 reason。
- 验证 repeated cancel 保留 cancel reason。
- 验证 cancelled 状态下继续 progress 被拒绝且状态保持。
- 验证 borrowed active pointer cleanup 不调用 destroy。
- 验证 owned active pointer cleanup 按 destroy function 释放一次。

## 6. 未进入 R2/R3/R4/R5/R6/R7 的说明

本阶段没有加入 reader lock、rwlock、refcount、epoch、background thread、async rebuild command、product active publish、delta generation 产品语义、shadow full scan + delta replay 产品入口、post-publish snapshot hook。

当前仍是同步 rebuild state machine hardening；HNSW-R2 才处理 reader safety primitive。

## 7. 测试结果

构建：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

必跑测试：

```text
tests/vector_rebuild_state_machine_test.sh
PASS

tests/vector_rebuild_delta_capture_test.sh
PASS

tests/vector_rebuild_shadow_replay_test.sh
PASS

tests/vector_rebuild_shadow_swap_test.sh
PASS

tests/vector_core_hnsw_v2_product_integration_test.sh
PASS

tests/vector_index_snapshot_shadow_publish_test.sh
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

## 8. 风险与后续阶段

当前风险：

- R1 只定义同步状态机行为，不能证明 background rebuild 并发安全。
- product `VSEARCH` 仍读当前产品 index；reader lifetime 问题尚未处理。
- active/shadow/retired 的产品级 publish/free 顺序仍需 HNSW-R2/R3 验证。

下一步建议：

```text
HNSW-R2: reader safety primitive
```

第一版按 HNSW-R0 路线使用 `rwlock`，先证明 reader / publish / cleanup 生命周期正确，再考虑更复杂的 refcount 或 epoch。

## 9. git checks

`git diff --check`：

```text
PASS
```

`git status --short`：

```text
M docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md
M src/vector/kvs_vector_rebuild.c
M tests/vector_rebuild_state_machine_test.c
?? docs/hnsw_rebuild_productization/
```

说明：`docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md` 是进入 HNSW-R1 前已有修改，不属于本轮任务。

## 10. Decision

```text
HNSW-R1 decision:
  production_code_changed = yes
  tests_changed = yes
  makefile_changed = no
  docs_changed = yes
  background_worker_added = no
  reader_safety_added = no
  product_active_publish_added = no
  delta_capture_semantics_changed = no
  state_machine_hardened = yes
  failure_reason_observable = yes
  repeated_rebuild_behavior_defined = yes
  cancel_failed_behavior_defined = yes
  current_index_ownership_unchanged = yes
  ready_for_HNSW_R2 = yes
  next_stage = HNSW-R2
```
