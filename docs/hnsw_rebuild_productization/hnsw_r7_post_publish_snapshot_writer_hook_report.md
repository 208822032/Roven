# HNSW-R7: post-publish snapshot writer hook report

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`，并读取：

- `docs/hnsw_rebuild_productization/README.md`
- `docs/hnsw_rebuild_productization/index.md`
- `docs/hnsw_rebuild_productization/hnsw_r_execution_plan.md`
- `docs/hnsw_rebuild_productization/hnsw_r1_sync_rebuild_state_machine_hardening_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r2_reader_safety_primitive_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r3_active_shadow_retired_publish_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r4_delta_generation_boundary_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r5_shadow_replay_final_validation_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r6_background_worker_async_rebuild_report.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；HNSW-R0 路线要求 HNSW-R 阶段报告写入 `docs/hnsw_rebuild_productization/`。本报告按 HNSW-R0 路线落盘，同时继续遵守 AGENTS 的硬边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不改变默认 backend / 默认 HNSW 参数、不让 vector core 访问 manager-owned 语义状态。

## 2. 与 HNSW-R0-R6 的一致性

本阶段只执行 HNSW-R7：`post-publish snapshot writer hook`。

已遵守 R7 边界：

- 真实 async worker product publish 成功后触发 best-effort snapshot write。
- 复用 HNSW-S 既有 snapshot writer，没有重新设计 snapshot format。
- snapshot write failure 不回滚 publish、不 fail rebuild、不影响 active search。
- snapshot write 不在 product active publish write-lock critical section 内执行。
- 未修改 RDB/AOF、`VSEARCH` response、`kvs_vector_index_ops_t`、默认 backend 或默认 HNSW 参数。
- 未修改 R6 background worker 控制面语义。

## 3. Read-only audit 摘要

R6 worker 成功路径：

```text
kvs_vector_rebuild_async_run_once()
  -> kvs_vector_rebuild_shadow_v2_init()
  -> kvs_vector_rebuild_shadow_v2_build_from_active_records()
  -> kvs_vector_rebuild_shadow_v2_replay_deltas()
  -> kvs_vector_rebuild_publish_ready_shadow()
  -> kvs_vector_rebuild_retire_cleanup_safe()
```

R7 前缺口：

```text
kvs_vector_rebuild_publish_ready_shadow()
  -> product active publish success
  -> no snapshot write in real worker path
```

旧 test-only 路径：

```text
kvs_vector_rebuild_swap_ready_shadow_for_test()
  -> kvs_vector_rebuild_publish_ready_shadow()
  -> kvs_vector_index_snapshot_write_hnsw_v2_index()
```

这个路径已经能写 snapshot，但只用于既有 HNSW-S / shadow publish test harness，不是 R6 真实 async worker 产品路径。

可复用 HNSW-S writer：

```text
kvs_vector_index_snapshot_write_current()
  -> kvs_vector_index_snapshot_write_core_hnsw_v2()
  -> serialize vector_index.snapshot
  -> write vector_index.manifest
  -> update g_vector_state.snapshot_status
```

VINFO 已有字段：

```text
snapshot_write_attempted
snapshot_written
snapshot_last_write_error
snapshot_last_write_started_ms
snapshot_last_write_finished_ms
snapshot_last_bytes
snapshot_last_checksum
```

## 4. R7 实际修改内容

本轮直接修改：

- `src/vector/kvs_vector.c`
- `tests/vector_rebuild_async_worker_test.c`
- `docs/hnsw_rebuild_productization/hnsw_r7_post_publish_snapshot_writer_hook_report.md`

进入本轮前已有未提交修改：

- `docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md`
- R1-R6 相关 include / src / tests 文件
- `docs/hnsw_rebuild_productization/`
- `tests/vector_rebuild_async_worker_test.c`
- `tests/vector_rebuild_async_worker_test.sh`

这些属于前序阶段或既有脏状态，本轮未做无关回退。

## 5. Post-publish snapshot writer hook 设计

R7 后真实 async worker 成功路径：

```text
final validation ok
  -> kvs_vector_rebuild_publish_ready_shadow()
     -> acquire active-index write lock
     -> shadow becomes product active
     -> old active enters retired
     -> release active-index write lock
  -> kvs_vector_index_snapshot_write_best_effort_after_rebuild("async_rebuild_publish")
  -> kvs_vector_rebuild_retire_cleanup_safe()
```

关键点：

- snapshot writer 复用 HNSW-S `kvs_vector_index_snapshot_write_current()`。
- snapshot 写入发生在 `kvs_vector_rebuild_publish_ready_shadow()` 返回之后，因此不在 product active publish write lock 内。
- worker 仍持有 R6 的 rebuild worker read guard，避免 snapshot summary 与 VSET/VDEL 并发写入发生数据竞争。
- snapshot write 的返回值只用于日志和 `snapshot_status`，不覆盖 worker rebuild 返回值。
- 旧 `kvs_vector_rebuild_swap_ready_shadow_for_test()` 的 test-only snapshot 行为保持不变。

## 6. Snapshot write success / failure 语义

成功：

```text
publish succeeds
snapshot write succeeds
snapshot_status.snapshot_write_attempted = 1
snapshot_status.snapshot_written = 1
VINFO exposes write bytes/checksum
worker last_status remains success
active search uses newly published product active
```

失败：

```text
publish succeeds
snapshot write fails
snapshot_status.snapshot_write_attempted = 1
snapshot_status.snapshot_written = 0
snapshot_status.last_snapshot_write_error records reason
worker last_status remains success
active search uses newly published product active
```

snapshot write failure does not:

- rollback product active publish
- fail async rebuild
- affect `VSEARCH`
- change snapshot format
- change RDB/AOF

## 7. VINFO / snapshot status 影响

R7 没有新增 VINFO 字段。复用 HNSW-S 已有 snapshot status 字段。

新增 async worker 测试覆盖：

- async publish 成功后 `snapshot_write_attempted = 1`。
- async publish 成功后 `snapshot_written = 1`。
- async publish 写失败后 `snapshot_write_attempted = 1`。
- async publish 写失败后 `snapshot_written = 0`。
- `kvs_vector_info()` 可观察相同状态。

## 8. 未修改 snapshot format / RDB/AOF / VSEARCH response

本阶段没有修改：

- `vector_index.manifest` 格式。
- `vector_index.snapshot` 序列化格式。
- snapshot load / deserialize 实现。
- RDB/AOF 主格式。
- `VSEARCH` response。
- `kvs_vector_index_ops_t`。
- 默认 backend。
- 默认 HNSW 参数。

## 9. 测试结果

构建：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

必跑测试：

```text
tests/vector_rebuild_async_worker_test.sh
PASS

tests/vector_index_snapshot_shadow_publish_test.sh
PASS

tests/vector_rebuild_shadow_replay_test.sh
PASS

tests/vector_rebuild_shadow_swap_test.sh
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

补充 snapshot 测试：

```text
tests/vector_index_snapshot_manifest_test.sh
PASS

tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh
PASS

tests/vector_index_snapshot_load_after_records_test.sh
PASS

tests/vector_index_snapshot_write_after_rebuild_test.sh
PASS

tests/vector_index_snapshot_vinfo_status_test.sh
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

- snapshot write 仍是同步 best-effort IO，会增加 async worker 完成时间，但不会延长 product active publish write-lock critical section。
- worker 仍按 R6 设计持有 rebuild worker read guard，VSET/VDEL 写路径会等待 worker 完成。
- snapshot write 成功只表示 graph cache 已写入，不代表 rebuild correctness；rebuild correctness 仍由 R5 final validation 保证。

下一步建议：

```text
HNSW-R-final: product rebuild consolidation
```

R-final 应整合 R1-R7 的最终状态、确认边界未破坏，并给出是否进入后续线上化 / 压测 / cleanup 的结论。

## 11. Git checks

`git diff --check`：

```text
PASS
```

`git status --short`：

```text
 M Makefile
 M docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_index_internal.h
 M include/vector/kvs_vector_manager.h
 M include/vector/kvs_vector_rebuild_shadow.h
 M src/core/kvs_operation.c
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
?? tests/vector_rebuild_async_worker_test.c
?? tests/vector_rebuild_async_worker_test.sh
```

说明：`docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md` 是进入 HNSW-R 系列前已有修改；R1-R6 相关文件是前序阶段未提交修改。本轮 R7 只追加本报告、`src/vector/kvs_vector.c` 中 async worker post-publish snapshot hook，以及 `tests/vector_rebuild_async_worker_test.c` 中 focused 覆盖。

## 12. Decision

```text
HNSW-R7 decision:
  production_code_changed = yes
  tests_changed = yes
  makefile_changed = no
  docs_changed = yes
  post_publish_snapshot_hook_added = yes
  hnsw_s_snapshot_writer_reused = yes
  snapshot_write_best_effort = yes
  snapshot_write_failure_rolls_back_publish = no
  snapshot_write_failure_fails_rebuild = no
  snapshot_write_failure_affects_active_search = no
  snapshot_format_changed = no
  rdb_aof_changed = no
  vsearch_response_changed = no
  index_ops_changed = no
  background_worker_semantics_changed = no
  ready_for_HNSW_R_final = yes
  next_stage = HNSW-R-final
```
