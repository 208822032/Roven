# HNSW-R-final: product rebuild consolidation report

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
- `docs/hnsw_rebuild_productization/hnsw_r7_post_publish_snapshot_writer_hook_report.md`

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；HNSW-R0 路线和本任务要求 HNSW-R 报告写入 `docs/hnsw_rebuild_productization/`。本报告按 HNSW-R 路线落盘，同时继续遵守 AGENTS 的硬边界：不修改 RDB/AOF 主格式、不修改 `VSEARCH` response、不修改 `kvs_vector_index_ops_t`、不改变默认 backend / 默认 HNSW 参数、不让 vector core 访问 manager-owned 语义状态。

## 2. HNSW-R-final 范围说明

本阶段只做 consolidation / audit / docs update：

- 汇总 R1-R7 已完成能力。
- 复核 HNSW-S / HNSW-R 边界。
- 复核禁止修改的接口和格式。
- 更新 HNSW-R README / index。
- 新增本 final 报告。

本阶段没有修改生产代码、测试或 Makefile，也没有实现新功能。

## 3. R1-R7 完成矩阵

| Stage | 目标 | 完成状态 | 关键结论 |
| --- | --- | --- | --- |
| R1 | sync rebuild state machine hardening | done | 状态转换确定；failure / cancel reason 可观测；重复 begin / failed / cancel 行为明确。 |
| R2 | reader safety primitive | done | 第一版使用 manager-owned `rwlock`；`VSEARCH` read guard 与 publish / cleanup write guard 成对。 |
| R3 | product active / shadow / retired publish | done | shadow 通过安全 publish 成为 product active；old active 进入 retired；publish failure 保持 old active。 |
| R4 | delta capture generation boundary | done | capture 在 full scan 前开启；VSET / VDEL commit 后 append delta；append failure fail rebuild 不回滚用户写入。 |
| R5 | shadow full scan + delta replay + final validation | done | READY_TO_SWAP 必须通过 graph / record / vector / checksum / generation / search sanity validation。 |
| R6 | background worker / async rebuild command | done | 新增 one-shot async worker 和 start / status / cancel / retry 控制面；复用 R2-R5 safety gate。 |
| R7 | post-publish snapshot writer hook | done | async worker publish 成功后复用 HNSW-S best-effort snapshot writer；write failure 不回滚 publish。 |

R1-R7 decision block 前后一致：每个阶段都只推进当前目标，下一阶段标记均为 ready，最终 R7 标记 `ready_for_HNSW_R_final = yes`。

## 4. 当前 product rebuild 能力矩阵

当前最小产品闭环：

```text
VINDEX REBUILD ASYNC
  -> start one-shot worker
  -> shadow init / rebuild begin
  -> full scan manager active records
  -> capture and replay VSET / VDEL deltas
  -> final validation
  -> reader-safe product publish
  -> old active retired cleanup
  -> best-effort snapshot write
  -> async status observable
```

已具备：

- 同步 `VINDEX REBUILD` 行为保留。
- `VSEARCH` 继续读 product published active，不读 rebuild shadow。
- async worker 成功路径发布新 active 后可触发 snapshot write。
- async worker cancel / retry / failure cleanup 行为明确。
- snapshot write failure 可通过既有 snapshot status / VINFO 字段观察。

当前仍是最小产品化版本：

- worker 是 one-shot thread，不是长期 worker pool。
- 为避免 manager record store 数据竞争，R6 设计会让 VSET / VDEL 写路径等待 worker guard；`VSEARCH` 仍可继续服务。
- reader safety 第一版是 `rwlock`，后续是否替换为 refcount / epoch 需要压测依据。

## 5. HNSW-S / HNSW-R 边界复核

HNSW-S 边界保持：

- snapshot manifest / snapshot serialize / deserialize / validate 仍由 HNSW-S 实现。
- startup valid snapshot load 和 invalid / missing / corrupt fallback 语义不在 R-final 修改。
- snapshot 仍是 derived graph cache，不是 source of truth。
- RDB/AOF 仍保存 records，不保存 HNSW graph。

HNSW-R 边界保持：

- HNSW-R 只负责 product-safe background rebuild：reader safety、active/shadow/retired publish、delta capture/replay、final validation、async worker、post-publish snapshot writer hook。
- R7 复用 HNSW-S writer，不重新设计 snapshot format。
- 本轮 static boundary grep 未发现 vector core 访问 `g_vector_state`、`record_id_map`、scope / TTL / metadata / question / answer。

## 6. 未修改接口 / 格式复核

本轮审计结论：

- `kvs_vector_index_ops_t` 签名未修改。
- `VSEARCH` response 未修改。
- RDB/AOF 主格式未修改。
- `vector_index.manifest` / `vector_index.snapshot` 格式未修改。
- 默认 backend 和默认 HNSW 参数未修改。
- `src/vector/core` 和 `include/vector/kvs_vector_hnsw_v2.h` 本轮无 diff。

R6 新增的是 `VINDEX REBUILD ASYNC|STATUS|CANCEL|RETRY` 控制面和 public async rebuild API；这不改变 `VSEARCH` response，也不改变 `kvs_vector_index_ops_t`。

## 7. 测试结果汇总

R7 阶段已执行并通过的构建：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

R7 阶段已执行并通过的 rebuild / product 测试：

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

R7 阶段已执行并通过的补充 snapshot 测试：

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

R7 阶段静态边界检查：

```text
grep -RIn "g_vector_state" src/vector/core include/vector/kvs_vector_hnsw_v2.h
no output

grep -RIn "record_id_map" src/vector/core include/vector/kvs_vector_hnsw_v2.h
no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core include/vector/kvs_vector_hnsw_v2.h
no output
```

R-final 本轮只改 docs，未重新运行全量测试。本轮执行：

```text
git diff --check
PASS

git status --short
见第 8 节完整输出
```

## 8. git 状态说明

进入 R-final 前已有未提交修改：

- `docs/hnsw_index_snapshot/hnsw_s_bench_snapshot_cold_start_report.md` 是进入 HNSW-R 系列前已有修改，不属于 HNSW-R-final。
- R1-R7 相关 include / src / tests / Makefile 修改仍未提交。
- `docs/hnsw_rebuild_productization/` 作为 HNSW-R 文档目录仍是 untracked。
- `tests/vector_rebuild_async_worker_test.c` 和 `.sh` 是 R6 新增测试，仍是 untracked。

R-final 本轮新增 / 修改：

- `docs/hnsw_rebuild_productization/hnsw_r_final_product_rebuild_consolidation_report.md`
- `docs/hnsw_rebuild_productization/README.md`
- `docs/hnsw_rebuild_productization/index.md`

当前 `git status --short` 仍应显示 HNSW-R 系列未提交修改和既有 HNSW-S bench 文档脏状态；提交时需要明确 stage 范围，避免无意混入或遗漏前序文件。

实际输出：

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

## 9. 当前剩余风险

- async worker 是 one-shot 线程；长期 worker pool、队列调度和高并发压力下的公平性尚未产品化。
- 第一版 reader safety 使用 `rwlock`；高 QPS `VSEARCH` 下的锁开销需要后续 benchmark 验证。
- R6 保守串行化 worker 与 VSET / VDEL 写路径；这保证安全，但不是最终高写入并发设计。
- final validation search sanity 是轻量正确性检查，不替代大规模 recall / latency benchmark。
- active + shadow + retired 在 rebuild / publish 窗口可能形成内存峰值，需要后续容量压测。
- snapshot write 是 best-effort 同步 IO；不会延长 publish write lock，但会增加 worker 完成时间。

## 10. 后续建议

建议下一步先提交 HNSW-R1-R7 + R-final 收口 diff，然后进入后续验证阶段：

- 对 async rebuild 做 10K / 100K 级别耗时、内存峰值和 `VSEARCH` 并发读压测。
- 观察 `rwlock` 对 `VSEARCH` p50 / p95 / p99 的影响。
- 明确是否需要把 one-shot worker 演进为 worker queue / scheduler。
- 评估 VSET / VDEL 与 background rebuild 的并发写入体验是否需要后续 R-followup 优化。

## 11. Final decision

```text
HNSW-R-final decision:
  production_code_changed = no
  tests_changed = no
  makefile_changed = no
  docs_changed = yes
  final_report_added = yes
  readme_index_updated = yes
  r1_r7_consolidated = yes
  hnsw_s_boundary_preserved = yes
  rdb_aof_changed = no
  vsearch_response_changed = no
  index_ops_changed = no
  snapshot_format_changed = no
  background_rebuild_product_loop_complete = yes
  ready_to_commit = yes
  recommended_next = commit
```
