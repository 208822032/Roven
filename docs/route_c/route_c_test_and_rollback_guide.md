# Route C Test and Rollback Guide

## 1. 测试脚本状态

Route C 阶段性测试脚本（`tests/route_c/`、`tests/archive/route_c/` 及 `tests/` 根目录下的兼容 wrapper）已于 2026-06-19 最终清理中全部删除。

**不再有可运行的 Route C 专项测试脚本。** 历史测试结果、TSAN/mixed stress/correctness/benchmark 证据保留在以下文档中：

- `docs/route_c/route_c_final_report.md` — 总报告，含各阶段测试结论
- `docs/route_c/route_c_problem_resolution_log.md` — 每个问题的根因、修复、验证方式
- `docs/route_c/archive/stage_docs/67_route_c_completion_review.md` — 完成度审计，含所有测试脚本的 PASS/FAIL 状态
- `docs/route_c/archive/stage_docs/63_stage12_weak_rwlock_correctness_and_stress_gate.md` — Stage 12 TSAN/mixed stress 证据
- `docs/route_c/archive/stage_docs/65_stage13_global_rwlock_removal_and_final_gate.md` — Stage 13 最终 gate 证据
- `docs/route_c/archive/stage_docs/66_route_c_final_closeout_report.md` — 最终 closeout 性能基线

## 2. 日常 Smoke

最小验证——确认构建和基本功能：

```bash
make clean && make embedding ENABLE_EBPF=0
./kvstore --config config/kvstore.conf &
sleep 1
# 基本 KV 操作
python3 scripts/benchmark/kvstore_resp_bench.py --host 127.0.0.1 --port 9300 --clients 1 --requests 100
kill %1
```

## 3. 历史测试覆盖摘要

以下是 Route C 阶段测试曾覆盖的内容（测试脚本已删除，覆盖范围记录如下供参考）：

| 类别 | 曾用脚本（已删除） | 覆盖内容 |
|------|---------------------|----------|
| Final gate | `route_c_final_closeout_stage13.sh` | weak-lock + real-lock benchmark、RDB/AOF/replication 耗时、owner stats |
| Correctness | `weak_rwlock_correctness_stage12.sh` | 17 项子测试：rwlock gate、executor coverage、ntyco/eBPF audit、TTL scanner、shutdown、slave connect、replication fallback、replicated command task、TTL executor、lazy expire、master full sync、RDB snapshot、AOF rewrite snapshot |
| Mixed stress | `weak_rwlock_mixed_stress_stage12.sh` | reactor/proactor/ntyco、c4/c8/c16、KV/TTL/Vector/HNSW/AOFREWRITE/RDB/AOF/replication |
| TSAN | `weak_rwlock_tsan_stage12.sh` | Clang TSAN、reactor stress + persistence/replication sub-suite |
| NtyCo | `ntyco_third_party_vendor_stage12_infra.sh` | NtyCo vendoring、executor、mixed shutdown |
| 去锁验证 | `rwlock_removal_final_stage13.sh` | TTL 全部后端回归 |
| 产品 gate | `route_c_final_product_gate.sh` | 聚合入口：构建 + correctness + mixed stress + TSAN + NtyCo + 去锁 |

## 4. 回滚命令

### 4.1 运行时回滚到真实锁

```bash
# 标准配置
KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore.conf

# Embedding 配置
KVLITE_ROUTE_C_WEAK_RWLOCK=0 ./kvstore --config config/kvstore-embedding.conf
```

### 4.2 代码调用回滚

```c
kvs_owner_lock_set_weak_mode(0);
```

### 4.3 源码回滚

```bash
# 通过 git revert 恢复原始 pthread_rwlock_t
git revert <stage13_commit>
```

### 4.4 恢复历史测试脚本

如需恢复已删除的 Route C 测试脚本：

```bash
# 从 tag 恢复所有测试脚本
git checkout route-c-final-weak-lock-ready -- tests/route_c/ tests/archive/route_c/ tests/*stage*.sh tests/*route_c*.sh tests/*rwlock*.sh tests/*weak_rwlock*.sh

# 或通过 git revert 恢复本次清理 commit
git revert <removal_commit>
```

## 5. Tag 前检查

打 tag 前必须确认：

- [ ] `git status --porcelain` 输出为空
- [ ] `make clean && make embedding ENABLE_EBPF=0` 成功
- [ ] 产品级通用功能测试通过（`tests/ttl/test_ttl_all_backends.sh` 等）

注：Route C 专项测试脚本已删除。验证证据已固化在 `docs/route_c/` 文档中。后续 Route C 行为验证应通过产品级通用测试覆盖。

## 6. 产物清理

测试产生的临时文件：

```bash
rm -rf /tmp/kvlite-*
rm -rf /tmp/kvlite-stage*
```

`.gitignore` 已覆盖的产物（不需要手动清理）：
- `workspace/`、`logs/`
- `third_party/NtyCo/objs/`、`third_party/NtyCo/libntyco.a`
- `*.o`、`*.a`

## 7. 历史测试依赖关系（仅供参考）

以下为 Route C 测试脚本删除前的依赖关系树。保留在此仅供理解测试覆盖范围：

```
route_c_final_product_gate.sh (已删除)
├── route_c_final_closeout_stage13.sh (已删除)
│   ├── weak_rwlock_correctness_stage12.sh (已删除)
│   │   ├── rwlock_weakening_gate_stage11.sh (已删除)
│   │   ├── full_executor_coverage_closeout_stage10_14.sh (已删除)
│   │   ├── ntyco_executor_stage10_13.sh (已删除)
│   │   ├── ebpf_replication_executor_stage10_12.sh (已删除)
│   │   ├── ttl_scanner_owner_stage10_10.sh (已删除)
│   │   ├── shutdown_owner_stage10_9.sh (已删除)
│   │   ├── slave_connect_owner_stage10_9a.sh (已删除)
│   │   ├── shutdown_flag_atomic_stage12.sh (已删除)
│   │   ├── replication_fallback_stage10_8.sh (已删除)
│   │   ├── replicated_command_task_stage10_3.sh (已删除)
│   │   ├── ttl_executor_stage10_1.sh (已删除)
│   │   ├── lazy_expire_stage10_2.sh (已删除)
│   │   ├── master_full_sync_stage10_5.sh (已删除)
│   │   ├── rdb_snapshot_stage10_6.sh (已删除)
│   │   ├── aof_rewrite_snapshot_stage10_7.sh (已删除)
│   │   ├── aofrewrite_cow_snapshot_stage12.sh (已删除)
│   │   └── ntyco_mixed_shutdown_stage12.sh (已删除)
│   └── weak_rwlock_mixed_stress_stage12.sh (已删除)
│       ├── rdb_snapshot_stage10_6.sh (已删除)
│       ├── aof_rewrite_snapshot_stage10_7.sh (已删除)
│       ├── master_full_sync_stage10_5.sh (已删除)
│       └── replicated_command_task_stage10_3.sh (已删除)
├── rwlock_removal_final_stage13.sh (已删除)
│   └── test_ttl_all_backends.sh (仍在 tests/ttl/ 中)
├── weak_rwlock_tsan_stage12.sh (已删除)
│   └── weak_rwlock_mixed_stress_stage12.sh (已删除)
└── ntyco_third_party_vendor_stage12_infra.sh (已删除)
    ├── ntyco_executor_stage10_13.sh (已删除)
    ├── ntyco_mixed_shutdown_stage12.sh (已删除)
    ├── rwlock_weakening_gate_stage11.sh (已删除)
    └── shutdown_flag_atomic_stage12.sh (已删除)
```

## 8. 历史阶段测试（已删除）

以下测试脚本曾归档到 `tests/archive/route_c/`，已于 2026-06-19 最终清理中删除：

- `embedding_worker_pool_stage7.sh` / `embedding_worker_pool_perf_stage7.sh`
- `embedding_per_thread_runtime_stage8.sh` / `embedding_per_thread_runtime_perf_stage8.sh`
- `embedding_runtime_tuning_stage8_1.sh` / `embedding_runtime_tuning_perf_stage8_1.sh`
- `embedding_runtime_bottleneck_stage8_2.sh`
- `embedding_batch_stage8_3.sh` / `embedding_batch_perf_stage8_3.sh`
- `vset_commit_breakdown_stage8_4.sh` / `vset_commit_breakdown_perf_stage8_4.sh`
- `embedding_batch_policy_stage8_5.sh` / `embedding_batch_policy_perf_stage8_5.sh`
- `embedding_worker_pool_stage9.sh` / `embedding_worker_pool_stage9_1.sh` / `embedding_worker_pool_perf_stage9.sh`
- `proactor_executor_perf_compare_6d.sh` / `proactor_executor_stability_6d.sh`
- `ntyco_stage10_4.sh`
- `repro_shutdown_segfault.sh`

验证证据保留在 `docs/route_c/` 相关文档中。
