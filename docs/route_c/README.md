# Route C — 所有权模型收敛（已完成）

Route C 是 KVLite 的架构演进路线，目标是将同步 handler 模型演进为"网络并发、embedding 并发、状态串行提交"的分层架构。

**状态：ROUTE_C_COMPLETE_READY_TO_TAG / FINAL_ROUTE_C_READY**

## 推荐阅读顺序

### 快速了解（15 分钟）

1. **[route_c_final_report.md](route_c_final_report.md)** — 路线 C 总报告，包含目标、设计、迭代、架构、结论
2. **[route_c_test_and_rollback_guide.md](route_c_test_and_rollback_guide.md)** — 测试证据位置与回滚方式

### 深入理解（45 分钟）

3. **[route_c_problem_resolution_log.md](route_c_problem_resolution_log.md)** — 每个问题的根因、修复、验证
4. **[route_c_productization_cleanup_report.md](route_c_productization_cleanup_report.md)** — 产品化整理报告
5. **[route_c_test_script_removal_report.md](route_c_test_script_removal_report.md)** — 测试脚本最终清理报告

### 原始证据

6. 最终 gate 文档（在 `archive/stage_docs/` 中）：
   - `63_stage12_weak_rwlock_correctness_and_stress_gate.md` — Stage 12 TSAN/mixed stress 证据
   - `65_stage13_global_rwlock_removal_and_final_gate.md` — Stage 13 最终 gate
   - `66_route_c_final_closeout_report.md` — 最终 closeout 性能基线
   - `67_route_c_completion_review.md` — 完成度审计

## 目录结构

```
docs/route_c/
├── README.md                                  ← 本文件
├── route_c_final_report.md                    ← 路线 C 总报告（整合所有阶段）
├── route_c_problem_resolution_log.md          ← 问题解决日志
├── route_c_test_and_rollback_guide.md         ← 测试证据位置与回滚指南
├── route_c_productization_cleanup_report.md   ← 产品化整理报告
├── route_c_test_script_removal_report.md      ← 测试脚本最终清理报告
└── archive/
    └── stage_docs/                            ← 历史阶段文档（60+ 份）
        ├── 00_route_c_overview.md
        ├── 01_lock_and_shared_state_audit.md
        ├── ...
        └── 67_route_c_completion_review.md
```

## 测试脚本状态

Route C 阶段���测试脚本（`tests/route_c/`、`tests/archive/route_c/` 及其兼容 wrapper）已于 2026-06-19 最终清理中全部删除。

- 删除报告：`docs/route_c/route_c_test_script_removal_report.md`
- 测试证据：保留在 final report、completion review、problem resolution log 等文档中
- 日常回归：通过产品级通用功能测试覆盖（`tests/ttl/test_ttl_all_backends.sh` 等）

## 如何回滚

```bash
# 运行时回滚到真实锁模式
KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore.conf

# Embedding 配置
KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore-embedding.conf

# 源码回滚：通过 git revert 恢复原始 pthread_rwlock_t
git revert <stage13_commit>
```

## 历史文档定位

`docs/route_c_perf_arch/` 仅保留兼容说明；正式入口和历史阶段文档已经迁移到 `docs/route_c/` 与 `docs/route_c/archive/stage_docs/`。

历史阶段文档（Stage 0-13 的每阶段设计、实现、审查、修复文档）已归档到 `docs/route_c/archive/stage_docs/`，可通过 `docs/route_c/archive/stage_docs/README.md` 了解各阶段顺序。

## Route C 后续

Route C 在 Stage 13 已达成所有原始架构目标。不建议继续做 Stage 14。后续如需进一步工作（多 executor、性能优化等），建议作为独立路线启动。
