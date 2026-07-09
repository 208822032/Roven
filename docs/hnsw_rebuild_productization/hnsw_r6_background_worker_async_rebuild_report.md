# HNSW-R6: background worker / async rebuild command report

## 1. AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`，并读取：

- `docs/hnsw_rebuild_productization/README.md`
- `docs/hnsw_rebuild_productization/index.md`
- `docs/hnsw_rebuild_productization/hnsw_r_execution_plan.md`
- `docs/hnsw_rebuild_productization/hnsw_r1_sync_rebuild_state_machine_hardening_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r2_reader_safety_primitive_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r3_active_shadow_retired_publish_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r4_delta_generation_boundary_report.md`
- `docs/hnsw_rebuild_productization/hnsw_r5_shadow_replay_final_validation_report.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；HNSW-R0 路线要求 HNSW-R 阶段报告写入 `docs/hnsw_rebuild_productization/`。本报告按 HNSW-R0 路线落盘，同时继续遵守 AGENTS 的硬边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不改变默认 backend / 默认 HNSW 参数、不让 vector core 访问 manager-owned 语义状态。

## 2. 与 HNSW-R0-R5 的一致性

本阶段只执行 HNSW-R6：background worker / async rebuild command。

已遵守 R6 边界：

- 引入 one-shot background rebuild worker。
- 新增 async start / status / cancel / retry 控制面。
- worker 串联 R2/R3/R4/R5 已有安全门禁。
- 未修改 `VSEARCH` response。
- 未修改 RDB/AOF。
- 未修改 `kvs_vector_index_ops_t`。
- 未修改 snapshot format。
- 未进入 R7 的 post-publish snapshot writer 新 hook。

## 3. Read-only audit 摘要

R1-R5 后可串联入口：

```text
kvs_vector_rebuild_shadow_v2_init()
kvs_vector_rebuild_shadow_v2_build_from_active_records()
kvs_vector_rebuild_shadow_v2_replay_deltas()
kvs_vector_rebuild_publish_ready_shadow()
kvs_vector_rebuild_retire_cleanup_safe()
```

当前同步 `VINDEX REBUILD` 入口：

```text
handle_vindex_command()
  -> kvs_vector_index_rebuild()
```

R6 前缺口：

- 没有 background worker lifecycle。
- 没有 async start/cancel/retry/status API。
- `VINDEX` 只有同步 `REBUILD`。
- worker shutdown 与 vector destroy/reset 没有 join/cancel cleanup。

## 4. R6 实际修改内容

本轮直接修改：

- `include/kvs_vector.h`
- `include/vector/kvs_vector_manager.h`
- `include/vector/kvs_vector_rebuild_shadow.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_rebuild_shadow.c`
- `src/core/kvs_operation.c`
- `Makefile`
- `tests/vector_rebuild_async_worker_test.c`
- `tests/vector_rebuild_async_worker_test.sh`
- `docs/hnsw_rebuild_productization/hnsw_r6_background_worker_async_rebuild_report.md`

进入本轮前已有未提交修改：

- `docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md`
- R1-R5 相关 include/src/tests 文件
- `docs/hnsw_rebuild_productization/`

这些属于前序阶段或既有脏状态，本轮未做无关回退。

## 5. Worker lifecycle 设计

新增 manager-owned worker 状态：

```text
rebuild_worker_mu
rebuild_worker_rwlock
rebuild_worker_thread
rebuild_worker_running
rebuild_worker_cancel_requested
rebuild_worker_job_id
rebuild_worker_last_status
rebuild_worker_started_at_ms
rebuild_worker_finished_at_ms
rebuild_worker_last_error
```

worker 主流程：

```text
async start
  -> create one-shot worker thread
  -> return immediately

worker:
  -> acquire manager-owned rebuild_worker read lock
  -> shadow init / rebuild begin
  -> build from manager active records
  -> replay deltas
  -> final validation
  -> product publish under R2/R3 active-index write guard
  -> retired cleanup under R2/R3 active-index write guard
  -> update async status
```

`kvs_vector_destroy()` 和 `kvs_vector_reset_for_load()` 会先 shutdown async worker：请求 cancel、join thread，再释放 current/shadow/retired/index state。

## 6. Async start/status/cancel/retry 语义

新增 async control API：

```text
kvs_vector_index_rebuild_async_start()
kvs_vector_index_rebuild_async_cancel()
kvs_vector_index_rebuild_async_retry()
kvs_vector_index_rebuild_async_status()
```

新增 internal test helper：

```text
kvs_vector_index_rebuild_async_wait_for_test()
```

新增命令控制面：

```text
VINDEX REBUILD ASYNC
VINDEX REBUILD STATUS
VINDEX REBUILD CANCEL
VINDEX REBUILD RETRY
```

保留既有同步语义：

```text
VINDEX REBUILD
  -> kvs_vector_index_rebuild()
```

取消语义：

- `cancel` 只设置 request flag，不在调用线程释放 shadow。
- worker 在 checkpoint 中进入 `CANCELLED`，并销毁 shadow。
- 已完成 worker 不能再 cancel，返回明确错误。

retry 语义：

- running 状态拒绝 retry。
- `FAILED` / `CANCELLED` 先 reset 到 `IDLE`，再启动新 worker。
- `IDLE` 状态允许 retry 作为新的 async rebuild。

status 语义：

- C API 返回 running、cancel request、job id、last status、rebuild state、progress、delta count、error。
- `VINDEX REBUILD STATUS` 返回新命令自己的 bulk string。
- `VINFO` 仍保留既有 `rebuild_state` / `rebuild_delta_count` / error / shadow / retired 状态字段。

## 7. Failure / shutdown cleanup 语义

失败：

```text
build / replay / validation / publish 任一步失败
  -> R1/R5 failure path
  -> state = FAILED
  -> shadow destroyed
  -> old product active unchanged
  -> worker last_status = -1
```

取消：

```text
cancel requested
  -> worker checkpoint
  -> state = CANCELLED
  -> shadow destroyed
  -> old product active unchanged
  -> retry may reset and restart
```

成功：

```text
final validation ok
  -> product publish
  -> old active retired
  -> retired cleanup
  -> state = IDLE
  -> worker last_status = 1
```

并发策略：

- `VSEARCH` 通过 R6 manager read guard + R2 active-index read guard 继续读取 current active。
- async worker 使用 manager read guard，避免与 VSET/VDEL 的 manager write path 数据竞争。
- 本版为了安全保守地串行化 VSET/VDEL 与 worker full rebuild；高写入并发下的更细粒度 delta replay 窗口可作为后续 follow-up，但不在 R6 扩大实现。

## 8. 未进入 R7 的说明

本阶段没有新增 post-publish snapshot writer hook。

`kvs_vector_rebuild_publish_ready_shadow()` 只做 product publish，不写 snapshot。旧测试路径 `kvs_vector_rebuild_swap_ready_shadow_for_test()` 仍保留 HNSW-S 既有 best-effort snapshot 行为，避免改变前序测试语义。

本阶段没有修改：

- snapshot format
- RDB/AOF 主格式
- `VSEARCH` response
- `kvs_vector_index_ops_t`
- 默认 backend
- 默认 HNSW 参数
- graph construction / SIMD / layout

## 9. 测试结果

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

tests/vector_index_snapshot_shadow_publish_test.sh
PASS

tests/vector_core_hnsw_v2_product_integration_test.sh
PASS

tests/vector_core_hnsw_v2_default_backend_test.sh
PASS
```

新增 R6 测试：

```text
tests/vector_rebuild_async_worker_test.sh
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

- R6 worker 是 one-shot 线程，不是长期 worker pool。
- 为避免 manager store 数据竞争，本版 async rebuild 会阻塞 VSET/VDEL 写路径；VSEARCH 仍可继续读 current active。
- `VINDEX REBUILD STATUS` 是新增命令自己的 bulk string，不改变既有 `VINDEX REBUILD` OK/ERR，也不改变 `VSEARCH` response。
- progress 在 rebuild context 中更新，但小数据集 rebuild 很快，运行中观测窗口很短。
- R7 仍需定义真实 product publish 后的 best-effort snapshot writer hook。

下一步建议：

```text
HNSW-R7: post-publish snapshot writer hook
```

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

## 12. Decision

```text
HNSW-R6 decision:
  production_code_changed = yes
  tests_changed = yes
  makefile_changed = yes
  docs_changed = yes
  background_worker_added = yes
  async_rebuild_start_added = yes
  async_status_added = yes
  cancel_retry_defined = yes
  worker_reuses_reader_safety = yes
  worker_reuses_product_publish = yes
  worker_reuses_delta_boundary = yes
  worker_reuses_final_validation = yes
  vsearch_response_changed = no
  rdb_aof_changed = no
  index_ops_changed = no
  snapshot_format_changed = no
  ready_for_HNSW_R7 = yes
  next_stage = HNSW-R7
```
