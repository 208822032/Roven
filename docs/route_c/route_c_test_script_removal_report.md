# Route C Test Script Removal Report

Date: 2026-06-19

## 1. 本阶段目标

删除 KVLite 项目中所有 Route C 阶段性测试脚本，仅保留文档证据。

- 不归档、不移动、不保留 `tests/archive/route_c/`
- Route C 的历史证据、测试结果、TSAN、mixed stress、final closeout 结论只保留在 `docs/route_c/` 和相关文档中

## 2. 是否修改生产代码

**否。** 未修改任何 `src/`、`include/` 下的生产代码文件。

## 3. 删除测试脚本总数

**46 个**（不含兼容 wrapper）：

- `tests/route_c/`：26 个活跃阶段测试脚本
- `tests/archive/route_c/`：20 个历史阶段测试脚本

另有 `tests/` 根目录下约 25 个兼容 wrapper（`exec bash "${ROOT_DIR}/tests/route_c/..."` 重定向脚本）一并清理。

## 4. 删除脚本完整列表

### 4.1 tests/route_c/（26 个）

| # | 文件 | 说明 |
|---|------|------|
| 1 | `route_c_final_product_gate.sh` | 产品级聚合 gate 入口 |
| 2 | `route_c_final_closeout_stage13.sh` | Stage 13 最终 closeout |
| 3 | `rwlock_removal_final_stage13.sh` | Stage 13 去锁验证 |
| 4 | `weak_rwlock_correctness_stage12.sh` | Stage 12 正确性回归（17 项子测试） |
| 5 | `weak_rwlock_mixed_stress_stage12.sh` | Stage 12 混合压力 |
| 6 | `weak_rwlock_tsan_stage12.sh` | Stage 12 TSAN gate |
| 7 | `rwlock_weakening_gate_stage11.sh` | Stage 11 gate 验证 |
| 8 | `rwlock_wrapper_stage11_0.sh` | Stage 11 wrapper 验证 |
| 9 | `full_executor_coverage_closeout_stage10_14.sh` | Stage 10 executor coverage closeout |
| 10 | `ntyco_executor_stage10_13.sh` | Stage 10 NtyCo executor convergence |
| 11 | `ebpf_replication_executor_stage10_12.sh` | Stage 10 eBPF replication executor |
| 12 | `rwlock_readiness_stage10_11.sh` | Stage 10 rwlock removal readiness |
| 13 | `ttl_scanner_owner_stage10_10.sh` | Stage 10 TTL scanner owner convergence |
| 14 | `shutdown_owner_stage10_9.sh` | Stage 10 shutdown owner boundary audit |
| 15 | `slave_connect_owner_stage10_9a.sh` | Stage 10 slave connect thread owner audit |
| 16 | `replication_fallback_stage10_8.sh` | Stage 10 replication fallback audit |
| 17 | `aof_rewrite_snapshot_stage10_7.sh` | Stage 10 AOF rewrite snapshot |
| 18 | `rdb_snapshot_stage10_6.sh` | Stage 10 RDB save snapshot |
| 19 | `master_full_sync_stage10_5.sh` | Stage 10 master full sync snapshot |
| 20 | `replicated_command_task_stage10_3.sh` | Stage 10 replicated command task |
| 21 | `lazy_expire_stage10_2.sh` | Stage 10 lazy expire audit |
| 22 | `ttl_executor_stage10_1.sh` | Stage 10 TTL ownership convergence |
| 23 | `ntyco_mixed_shutdown_stage12.sh` | Stage 12 NtyCo mixed shutdown |
| 24 | `ntyco_third_party_vendor_stage12_infra.sh` | Stage 12 NtyCo third party vendor |
| 25 | `shutdown_flag_atomic_stage12.sh` | Stage 12 shutdown flag atomic audit |
| 26 | `aofrewrite_cow_snapshot_stage12.sh` | Stage 12 AOF rewrite COW |

### 4.2 tests/archive/route_c/（20 个）

| # | 文件 | 说明 |
|---|------|------|
| 1 | `embedding_worker_pool_stage7.sh` | Stage 7 embedding worker pool |
| 2 | `embedding_worker_pool_perf_stage7.sh` | Stage 7 worker pool perf |
| 3 | `embedding_per_thread_runtime_stage8.sh` | Stage 8 per-thread runtime |
| 4 | `embedding_per_thread_runtime_perf_stage8.sh` | Stage 8 per-thread runtime perf |
| 5 | `embedding_runtime_tuning_stage8_1.sh` | Stage 8.1 runtime tuning |
| 6 | `embedding_runtime_tuning_perf_stage8_1.sh` | Stage 8.1 tuning perf |
| 7 | `embedding_runtime_bottleneck_stage8_2.sh` | Stage 8.2 bottleneck analysis |
| 8 | `embedding_batch_stage8_3.sh` | Stage 8.3 batch test |
| 9 | `embedding_batch_perf_stage8_3.sh` | Stage 8.3 batch perf |
| 10 | `vset_commit_breakdown_stage8_4.sh` | Stage 8.4 commit breakdown |
| 11 | `vset_commit_breakdown_perf_stage8_4.sh` | Stage 8.4 commit perf |
| 12 | `embedding_batch_policy_stage8_5.sh` | Stage 8.5 batch policy |
| 13 | `embedding_batch_policy_perf_stage8_5.sh` | Stage 8.5 policy perf |
| 14 | `embedding_worker_pool_stage9.sh` | Stage 9 worker pool |
| 15 | `embedding_worker_pool_stage9_1.sh` | Stage 9.1 worker pool shutdown |
| 16 | `embedding_worker_pool_perf_stage9.sh` | Stage 9 worker pool perf |
| 17 | `proactor_executor_perf_compare_6d.sh` | Stage 6d proactor perf |
| 18 | `proactor_executor_stability_6d.sh` | Stage 6d proactor stability |
| 19 | `ntyco_stage10_4.sh` | Stage 10.4 ntyco readiness |
| 20 | `repro_shutdown_segfault.sh` | Historical segfault repro (fixed) |

## 5. 删除理由

1. **Route C 已完成：** 结论为 `ROUTE_C_COMPLETE_READY_TO_TAG` / `FINAL_ROUTE_C_READY`，Route C 不再作为活跃开发路线继续推进
2. **测试证据已固化：** 所有测试结果、TSAN、mixed stress、correctness、benchmark 证据已保存在 `docs/route_c/` 文档中
3. **避免混淆：** 保留 Route C 阶段脚本会让人误以为这些脚本仍需要运行作为日常回归
4. **产品化方向：** 未来回归应通过产品级通用功能测试覆盖，而非 Route C 阶段专用脚本
5. **减少维护负担：** Route C 阶段脚本包含硬编码路径、特定阶段假设，维护成本高于其剩余价值

## 6. 文档证据保留位置

以下文档保留了 Route C 的完整测试证据：

| 文档 | 保留的证据 |
|------|-----------|
| `docs/route_c/route_c_final_report.md` | 总报告，各阶段测试结论 |
| `docs/route_c/route_c_problem_resolution_log.md` | 每个问题的根因、修复、验证方式 |
| `docs/route_c/archive/stage_docs/67_route_c_completion_review.md` | 完成度审计，所有测试脚本 PASS/FAIL 状态 |
| `docs/route_c/archive/stage_docs/63_stage12_weak_rwlock_correctness_and_stress_gate.md` | Stage 12 TSAN/mixed stress 证据 |
| `docs/route_c/archive/stage_docs/65_stage13_global_rwlock_removal_and_final_gate.md` | Stage 13 最终 gate 证据 |
| `docs/route_c/archive/stage_docs/66_route_c_final_closeout_report.md` | 最终 closeout 性能基线 |

## 7. 引用更新

以下文件已更新以反映测试脚本删除：

| 文件 | 更新内容 |
|------|----------|
| `README.md` | Route C gate 引用改为指向文档 |
| `docs/route_c/README.md` | 移除 tests/ 目录结构，更新为"如何回滚"，添加删除说明 |
| `docs/route_c/route_c_test_and_rollback_guide.md` | 重写：标记所有测试脚本为"已删除"，测试覆盖改为历史摘要 |
| `docs/route_c/route_c_productization_cleanup_report.md` | §4 添加"2026-06-19 更新"说明，新增 §14 最终清理章节 |
| `docs/operations/README.md` | Route C gate 引用改为文档入口 |
| `tests/README.md` | 移除 `route_c/` 和 `archive/route_c/` 目录描述，添加删除说明 |
| `docs/project_layout_cleanup_report.md` | 验证项中移除 Route C 测试命令 |

**未修改的文档：** `docs/route_c/archive/stage_docs/*.md` — 这些是历史阶段文档，其中对测试脚本的引用是历史记录，不作为当前可执行命令。

## 8. 当前 tests 目录剩余内容

```
tests/
├── README.md
├── ttl/                        ← TTL 后端矩阵与链路验证
│   ├── test_ttl_all_backends.sh
│   ├── test_ttl_aof.sh
│   ├── test_ttl_ebpf_replication.sh
│   ├── test_ttl_mvp.sh
│   ├── test_ttl_qps.sh
│   ├── test_ttl_rdb.sh
│   └── test_ttl_tcp_replication.sh
├── vector/                     ← 向量/embedding 语义测试
│   └── semantic_onnx_test.sh
├── benchmark/                  ← 测试侧 benchmark shell
│   └── test_ttl_expired_path_profile.sh
├── *.c                         ← 编译型测试源码
├── testcase                    ← 编译型测试二进制
├── vector_testcase             ← 向量测试二进制
├── ... (其他编译型测试二进制)
└── (部分 TTL/vector shell 脚本的旧路径 untracked 残留 — 非 Route C)
```

## 9. 为什么不再保留 Route C 测试

1. Route C 是迭代开发路线，不是持续回归需求
2. 46 个阶段测试脚本 + 25 个 wrapper 增加目录噪音和认知负担
3. 测试脚本中的硬编码路径、阶段编号、互调逻辑使得非原始开发者难以理解和维护
4. 测试覆盖的功能已通过产品级通用功能测试（`tests/ttl/test_ttl_all_backends.sh` 等）覆盖
5. 所有验证证据已在文档中固化，不需要重新运行来证明

## 10. 后续 Route C 行为验证

如需要验证 Route C 相关行为（weak-lock、real-lock 回滚、executor 正确性等）：

1. **构建验证：** `make clean && make embedding ENABLE_EBPF=0`
2. **通用功能测试：** `bash tests/ttl/test_ttl_all_backends.sh 127.0.0.1 9300`
3. **回滚模式验证：**
   ```bash
   KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore.conf &
   # 运行产品级通用功能测试验证 real-lock 模式正常
   bash tests/ttl/test_ttl_all_backends.sh 127.0.0.1 9300
   ```
4. **TSAN 验证：** 如需 TSAN，用 Clang 编译后运行通用测试
5. **如有新的 Route C 回归需求：** 应编写产品级通用测试脚本，不恢复 Route C 阶段脚本

## 11. 回滚方式

如需恢复已删除的 Route C 测试脚本：

### 方式一：Git Revert

```bash
git revert <removal_commit_hash>
```

### 方式二：从 Tag 恢复

```bash
git checkout route-c-final-weak-lock-ready -- tests/route_c/ tests/archive/route_c/
git checkout route-c-final-weak-lock-ready -- tests/*stage*.sh tests/*route_c*.sh tests/*rwlock*.sh tests/*weak_rwlock*.sh tests/*owner*.sh tests/*readiness*.sh tests/*closeout*.sh tests/*executor*.sh tests/*shutdown*.sh tests/*aofrewrite*.sh tests/*replication_fallback*.sh tests/*replicated_command_task*.sh tests/*slave_connect*.sh tests/*ttl_executor*.sh tests/*ttl_scanner*.sh tests/*lazy_expire*.sh tests/*ebpf_replication*.sh tests/*master_full_sync*.sh tests/*rdb_snapshot*.sh tests/*aof_rewrite_snapshot*.sh tests/*ntyco_*.sh tests/*full_executor*.sh
```

## 12. 验证结果

| 验证项 | 结果 |
|--------|------|
| `find tests -type f` Route C 模式匹配 | 0 匹配（全部清空） |
| `tests/route_c/` 目录是否存在 | 已删除 |
| `tests/archive/route_c/` 目录是否存在 | 已删除 |
| Route C 兼容 wrapper 是否清空 | 全部删除 |
| 引用更新 | README、6 份 docs、tests/README.md 已更新 |
| 生产代码是否修改 | 否 |
| 文档是否删除 | 否（仅更新引用） |

## 13. 是否仍然 ROUTE_C_COMPLETE_READY_TO_TAG

**是。** 测试脚本删除不影响 Route C 的正确性结论。所有验证证据仍在文档中。Route C 的架构成果（weak-lock、executor、owner convergence）仍在生产代码中生效。

## 14. 是否建议重新打 tag

不建议仅为测试脚本删除重新打 tag。`route-c-final-weak-lock-ready` 或 `route-c-productized-weak-lock-ready` tag 已标记 Route C 完成状态。如果后续需要包含本次清理的 tag，建议在 commit 后视需要追加 `route-c-final-clean` 或类似 tag。

## 15. 剩余风险

1. **无生产代码风险：** 本次仅删除测试脚本和更新文档引用
2. **无人为依赖外部脚本：** 如有外部 CI/CD 脚本引用已删除的 Route C 测试脚本，将失败。这些外部引用需要更新
3. **archive/stage_docs 中的测试命令：** 历史文档中仍包含对已删除测试脚本的引用（如 `bash tests/route_c/xxx.sh`）。这些是历史记录，不作为当前可执行命令
4. **tests/ 根目录存在少量非 Route C untracked 文件：** 如 `test_ttl_*.sh`、`semantic_onnx_test.sh` — 这些是从旧路径 rename 到 `tests/ttl/`、`tests/vector/` 后的残留副本，非 Route C 相关，建议单独处理

## 16. 建议 Commit Message

```
删除路线C阶段性测试脚本并保留文档证据

删除 tests/route_c/（26个）和 tests/archive/route_c/（20个）中的所有
Route C 阶段测试脚本及兼容 wrapper。Route C 已完成，测试证据保留在
docs/route_c/ 文档中。

引用更新：
- README.md：Route C gate 引用改为文档入口
- docs/route_c/README.md：移除测试目录结构，添加删除说明
- docs/route_c/route_c_test_and_rollback_guide.md：重写为历史证据索引
- docs/route_c/route_c_productization_cleanup_report.md：追加最终清理章节
- docs/operations/README.md、tests/README.md、project_layout_cleanup_report.md

验证：make embedding ENABLE_EBPF=0 通过

Co-Authored-By: Claude <noreply@anthropic.com>
```
