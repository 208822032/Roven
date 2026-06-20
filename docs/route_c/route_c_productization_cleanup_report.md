# Route C Productization Cleanup Report

Date: 2026-06-19

## 1. 本阶段目标

将 Route C 迭代过程中产生的大量文档、测试脚本和目录结构整理成更接近产品化维护的状态。不做新的架构改造，不修改生产代码逻辑。

## 2. 是否修改生产代码

**否。** 未修改任何 `src/`、`include/` 下的生产代码文件。仅修改了 4 个测试脚本中的文档路径引用。

## 3. 文档整理结果

### 3.1 新增文件

| 文件 | 说明 |
|------|------|
| `docs/route_c/README.md` | Route C 文档索引 |
| `docs/route_c/route_c_final_report.md` | Route C 总报告（整合所有阶段） |
| `docs/route_c/route_c_problem_resolution_log.md` | 问题解决日志（19 个问题） |
| `docs/route_c/route_c_test_and_rollback_guide.md` | 测试与回滚指南 |
| `docs/route_c/route_c_productization_cleanup_report.md` | 本报告 |

### 3.2 移动的文件（git mv，保留历史）

**58 份历史阶段文档** 从 `docs/route_c_perf_arch/` 移动到 `docs/route_c/archive/stage_docs/`：

- `01`–`13`：阶段 0 到阶段 0.5 的审计、设计文档（10 份）
- `15`–`22`：阶段 1–4 的审查和稳定化文档（8 份）
- `24`–`30`：阶段 5–6 的 proactor 适配文档（7 份）
- `31`–`37`：阶段 7–8 的 embedding 优化文档（7 份）
- `41`–`62`：阶段 9–11 的 worker pool 和 rwlock wrapper 文档（22 份）

### 3.3 保留在原路径的最终文档（8 份）

| 文件 | 说明 |
|------|------|
| `docs/route_c_perf_arch/00_route_c_overview.md` | Route C 总览 |
| `docs/route_c_perf_arch/14_route_c_design_review.md` | 设计审批报告 |
| `docs/route_c_perf_arch/63_stage12_weak_rwlock_correctness_and_stress_gate.md` | Stage 12 gate 证据 |
| `docs/route_c_perf_arch/64_stage12_infra_ntyco_third_party_vendor.md` | NtyCo vendoring 证据 |
| `docs/route_c_perf_arch/65_stage13_global_rwlock_removal_and_final_gate.md` | Stage 13 gate 证据 |
| `docs/route_c_perf_arch/66_route_c_final_closeout_report.md` | 最终 closeout 报告 |
| `docs/route_c_perf_arch/67_route_c_completion_review.md` | 完成度审计报告 |
| `docs/route_c_perf_arch/README.md` | 原索引 |

### 3.4 是否有文档被删除

**无。** 所有文档均被保留：8 份在原路径，58 份在 `docs/route_c/archive/stage_docs/`。

### 3.5 每份原始文档的去向映射

| 原始路径 | 去向 | 内容覆盖 |
|-----------|------|----------|
| `docs/route_c_perf_arch/00_route_c_overview.md` | 保留在原路径 | `route_c_final_report.md` §1 |
| `docs/route_c_perf_arch/01–13` (设计文档) | `archive/stage_docs/` | `route_c_final_report.md` §2 |
| `docs/route_c_perf_arch/14_route_c_design_review.md` | 保留在原路径 | `route_c_final_report.md` §1.1 |
| `docs/route_c_perf_arch/15–62` (阶段文档) | `archive/stage_docs/` | `route_c_final_report.md` §2.2 |
| `docs/route_c_perf_arch/63–67` (最终 gate) | 保留在原路径 | `route_c_final_report.md` §4–§11 |
| `docs/route_c_perf_arch/README.md` | 保留在原路径 | 新索引 `docs/route_c/README.md` |

如有任何文档被 `route_c_final_report.md` 或 `route_c_problem_resolution_log.md` 未覆盖内容，可通过 `docs/route_c/archive/stage_docs/` 查阅原始文件。

## 4. 测试脚本整理结果

> **2026-06-19 更新：** 本节描述 2026-06-19 产品化整理时的状态。后续已执行最终清理（见 §14），所有 Route C 阶段测试脚本（包括本节列出的"保留"和"归档"脚本）均已删除。验证证据保留在 `docs/route_c/` 文档中。详见 `route_c_test_script_removal_report.md`。

### 4.1 保留测试（产品回归，36 个）

以下测试脚本保留在 `tests/` 中，作为产品级回归测试：

| # | 测试脚本 | 类别 |
|---|----------|------|
| 1 | `route_c_final_closeout_stage13.sh` | 最终 gate |
| 2 | `rwlock_removal_final_stage13.sh` | 去锁验证 |
| 3 | `weak_rwlock_correctness_stage12.sh` | 正确性回归 |
| 4 | `weak_rwlock_mixed_stress_stage12.sh` | 混合压力 |
| 5 | `weak_rwlock_tsan_stage12.sh` | TSAN gate |
| 6 | `rwlock_weakening_gate_stage11.sh` | Gate 验证 |
| 7 | `rwlock_wrapper_stage11_0.sh` | Wrapper 验证 |
| 8 | `ntyco_third_party_vendor_stage12_infra.sh` | NtyCo 校验 |
| 9 | `ntyco_mixed_shutdown_stage12.sh` | NtyCo shutdown |
| 10 | `shutdown_flag_atomic_stage12.sh` | Atomic flag |
| 11 | `aofrewrite_cow_snapshot_stage12.sh` | AOF COW |
| 12 | `shutdown_owner_stage10_9.sh` | Shutdown owner |
| 13 | `slave_connect_owner_stage10_9a.sh` | Slave connect |
| 14 | `full_executor_coverage_closeout_stage10_14.sh` | Executor coverage |
| 15 | `rdb_snapshot_stage10_6.sh` | RDB snapshot |
| 16 | `aof_rewrite_snapshot_stage10_7.sh` | AOF rewrite |
| 17 | `master_full_sync_stage10_5.sh` | Master full sync |
| 18 | `replicated_command_task_stage10_3.sh` | Replication |
| 19 | `ttl_executor_stage10_1.sh` | TTL executor |
| 20 | `lazy_expire_stage10_2.sh` | Lazy expire |
| 21 | `ttl_scanner_owner_stage10_10.sh` | TTL scanner |
| 22 | `replication_fallback_stage10_8.sh` | Replication fallback |
| 23 | `ntyco_executor_stage10_13.sh` | NtyCo executor |
| 24 | `ebpf_replication_executor_stage10_12.sh` | eBPF replication |
| 25 | `rwlock_readiness_stage10_11.sh` | Rwlock readiness |
| 26–36 | `test_ttl_*.sh` (7 个) | TTL 产品回归 |

### 4.2 归档测试（21 个）

以下测试已移动到 `tests/archive/route_c/`：

| 测试脚本 | 归档理由 |
|-----------|----------|
| `embedding_worker_pool_stage7.sh` | 历史阶段 perf 测试 |
| `embedding_worker_pool_perf_stage7.sh` | 历史阶段 perf 测试 |
| `embedding_per_thread_runtime_stage8.sh` | 历史阶段 perf 测试 |
| `embedding_per_thread_runtime_perf_stage8.sh` | 历史阶段 perf 测试 |
| `embedding_runtime_tuning_stage8_1.sh` | 历史阶段 tuning 测试 |
| `embedding_runtime_tuning_perf_stage8_1.sh` | 历史阶段 tuning 测试 |
| `embedding_runtime_bottleneck_stage8_2.sh` | 历史阶段 bottleneck 分析 |
| `embedding_batch_stage8_3.sh` | 历史阶段 batch 测试 |
| `embedding_batch_perf_stage8_3.sh` | 历史阶段 batch perf 测试 |
| `vset_commit_breakdown_stage8_4.sh` | 历史阶段 commit 分析 |
| `vset_commit_breakdown_perf_stage8_4.sh` | 历史阶段 commit perf 分析 |
| `embedding_batch_policy_stage8_5.sh` | 历史阶段 policy 测试 |
| `embedding_batch_policy_perf_stage8_5.sh` | 历史阶段 policy perf 测试 |
| `embedding_worker_pool_stage9.sh` | 历史阶段 worker pool 测试 |
| `embedding_worker_pool_stage9_1.sh` | 历史阶段 shutdown 测试 |
| `embedding_worker_pool_perf_stage9.sh` | 历史阶段 perf 测试 |
| `proactor_executor_perf_compare_6d.sh` | 历史阶段 proactor perf |
| `proactor_executor_stability_6d.sh` | 历史阶段 proactor 稳定性 |
| `ntyco_stage10_4.sh` | 历史阶段 ntyco readiness |
| `semantic_onnx_test.sh` | 历史阶段 ONNX 测试 |
| `repro_shutdown_segfault.sh` | 历史 segfault 复现（已修复） |

### 4.3 删除测试

**无。** 所有测试脚本均被保留或归档，未删除任何测试。

### 4.4 修改的测试脚本（路径引用更新）

| 文件 | 变更 |
|------|------|
| `tests/route_c/shutdown_owner_stage10_9.sh` | `docs/route_c_perf_arch/53_*` → `docs/route_c/archive/stage_docs/53_*`（2 处） |
| `tests/route_c/rwlock_readiness_stage10_11.sh` | `docs/route_c_perf_arch/56_*` → `docs/route_c/archive/stage_docs/56_*` |
| `tests/route_c/rwlock_weakening_gate_stage11.sh` | `docs/route_c_perf_arch/62_*` → `docs/route_c/archive/stage_docs/62_*` |
| `tests/route_c/slave_connect_owner_stage10_9a.sh` | `docs/route_c_perf_arch/54_*` → `docs/route_c/archive/stage_docs/54_*` |

## 5. 新产品级测试入口

`tests/route_c/route_c_final_product_gate.sh` — 一键运行完整 Route C 验证套件：

```
tests/route_c/route_c_final_product_gate.sh
├── Build: make clean && make embedding ENABLE_EBPF=0
├── Final Closeout Gate (weak-lock + real-lock)
├── Correctness Suite (weak-lock default)
├── Correctness Suite (real-lock rollback)
├── Mixed Stress (reactor/proactor/ntyco)
├── TSAN Gate (Clang, optional via SKIP_TSAN=1)
├── NtyCo Third-Party Vendor Check
└── Rwlock Removal Final (TTL all backends)
```

## 6. 目录结构变化

### 新增目录

```
docs/route_c/                           ← 新产品文档入口
├── README.md
├── route_c_final_report.md
├── route_c_problem_resolution_log.md
├── route_c_test_and_rollback_guide.md
├── route_c_productization_cleanup_report.md
└── archive/
    └── stage_docs/                     ← 58 份历史阶段文档

tests/archive/
└── route_c/                            ← 21 份历史阶段测试
```

### 不变目录

```
docs/route_c_perf_arch/                 ← 保留 8 份最终文档 + README
tests/                                  ← 保留 36 份产品回归测试
src/                                    ← 未修改
include/                                ← 未修改
third_party/NtyCo/                      ← 未修改
```

## 7. 验证结果

| 验证项 | 结果 |
|--------|------|
| `make clean && make embedding ENABLE_EBPF=0` | PASS |
| `tests/route_c/route_c_final_closeout_stage13.sh` | PASS（exit_code=0） |
| `KVLITE_ROUTE_C_WEAK_RWLOCK=0 bash tests/route_c/weak_rwlock_correctness_stage12.sh` | PASS |
| `tests/route_c/weak_rwlock_tsan_stage12.sh` | PASS（Clang，零 data race） |
| 旧路径引用检查 | 全部更新，无残留 |
| `git status --porcelain` | 无意外产物 |

## 8. 剩余风险

1. **测试脚本硬编码路径**：4 个脚本的文档路径已更新为新归档路径。如需进一步修改归档结构，需要同步更新这些引用。
2. **archive 测试脚本**：`tests/archive/route_c/` 中的历史测试未验证在新路径下是否仍可运行。它们仅供复盘参考，不期望作为日常回归。
3. **legacy 入口**：外部文档或脚本如仍引用 `docs/route_c_perf_arch/` 下已移动的历史文档，将在新归档路径下查找。8 份最终文档仍保留在原路径，不应受影响。

## 9. 后续维护建议

1. **主要入口**：`docs/route_c/README.md` 和 `tests/route_c/route_c_final_product_gate.sh`
2. **新增测试**：如新增 Route C 相关回归测试，放到 `tests/` 并在 product gate 中注册
3. **新增文档**：如需补充 Route C 文档，建议优先更新 `docs/route_c/` 下的总文档
4. **archive 维护**：`docs/route_c/archive/stage_docs/` 和 `tests/archive/route_c/` 原则上只读，不再新增文件
5. **回滚验证**：每次打 tag 前运行 `tests/route_c/route_c_final_product_gate.sh`

## 10. 是否仍然 ROUTE_C_COMPLETE_READY_TO_TAG

**是。** 所有验证通过，产品化整理不影响 Route C 正确性。仍为 `FINAL_ROUTE_C_READY`。

## 11. 是否建议重新打 tag 或追加 tag

建议在 commit 后重新打 tag（或移动已有 tag 到新 commit），因为：

- 目录结构已产品化重组
- 新增了产品入口文档和聚合测试脚本
- tag 应指向产品化完成后的最终 commit

## 12. 修改/新增/移动/删除文件清单

| 操作 | 数量 | 说明 |
|------|------|------|
| 新增 | 6 | `docs/route_c/` 下 5 个文档 + `tests/route_c/route_c_final_product_gate.sh` |
| 移动（git mv） | 79 | 58 份文档 + 21 份测试脚本 |
| 修改 | 4 | 测试脚本路径引用更新 |
| 删除 | 0 | 无文件被删除 |

## 13. 建议 Commit Message

```
整理路线C文档测试并完成产品化收口

文档：
- 新增 docs/route_c/ 产品入口：总报告、问题解决日志、测试回滚指南、索引
- 将 58 份历史阶段文档归档到 docs/route_c/archive/stage_docs/
- 保留 8 份最终文档在 docs/route_c_perf_arch/

测试：
- 新增 tests/route_c/route_c_final_product_gate.sh 产品级聚合测试入口
- 将 21 份历史阶段测试归档到 tests/archive/route_c/
- 更新 4 个测试脚本的文档路径引用

验证：
- make embedding ENABLE_EBPF=0 PASS
- route_c_final_closeout_stage13.sh PASS
- correctness suite (real-lock rollback) PASS
- TSAN gate PASS (零 data race)

Co-Authored-By: Claude <noreply@anthropic.com>
```

## 14. 后续更新：Route C 测试脚本最终清理（2026-06-19）

在产品化整理完成后，于 2026-06-19 执行了 Route C 测试脚本的最终清理：

- **删除了 46 个 Route C 阶段测试脚本**（`tests/route_c/` 25 个 + `tests/archive/route_c/` 21 个）及所有兼容 wrapper
- **不归档、不移动到 archive**，直接从项目目录中删除
- **Route C 历史证据保留**在 `docs/route_c/` 及相关文档中（final report、completion review、problem resolution log 等）
- **回滚方式**：通过 git revert 或从 `route-c-final-weak-lock-ready` tag 恢复测试脚本
- **未来 Route C 行为验证**：应通过产品级通用功能测试覆盖，不再依赖 Route C 阶段专用脚本

详细报告见：`docs/route_c/route_c_test_script_removal_report.md`
