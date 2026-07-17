# V 族命令功能覆盖审计与持久化正确性验证

## 1. 范围与修改

本轮遵守根目录 `AGENTS.md`，只审计 V 族命令、持久化恢复和派生索引恢复证据。未修改公开协议、`VSEARCH` 返回格式、HNSW 算法/参数、RDB/AOF 格式、同步逻辑或 benchmark 数据。

修改文件：

- `tests/vector_command_persistence_recovery_test.sh`：补充 RDB-only、纯 AOF、RDB+AOF 和 records-only fallback 的产品进程集成测试。
- `README.md`：区分 RDB 的记录/embedding 持久化与 AOF 的命令重放语义。
- `docs/vector_command_functionality_audit.md`：记录覆盖矩阵、证据和剩余风险。

生产代码未修改。

## 2. 当前调用链

```text
RESP VSET/VGET/VSEARCH/VDEL/VINDEX/VINFO
  -> command parse / arity validation
  -> vector manager
       -> record store / scope / state / record_id map
       -> current core_hnsw_v2 or core_flat derived index
       -> manager filtering / result assembly

startup:
  RDB load and restore records/embeddings
  -> optional AOF command replay
  -> rebuild runtime record_id map
  -> validate/load index snapshot
  -> invalid or missing snapshot: rebuild index from active records
```

## 3. 功能覆盖矩阵

状态只使用“已由测试验证”“只有代码实现但没有测试”“只有文档声明”“尚未实现”。“已由测试验证”不表示覆盖了所有故障注入点。

| 功能 | 状态 | 证据/边界 |
| --- | --- | --- |
| VSET 新增 | 已由测试验证 | `vector_core_flat_correctness_test.sh`、product integration、新增恢复测试 |
| VSET 覆盖更新、重复写入 | 已由测试验证 | `vector_testcase.c`、product integration、新增恢复测试；重启后仍为新值 |
| VSET 不同 scope | 已由测试验证 | `vector_testcase.c`、manager fastpath、product integration |
| VSET 参数失败不产生记录 | 已由测试验证 | 新增恢复测试发送 arity 错误请求并检查 active/backend count 不变 |
| VSET 分配/index commit 失败回滚 | 只有代码实现但没有测试 | add/update rollback 分支存在，但没有直接 fault-injection 测试 |
| rebuild delta append 失败时的 VSET/VDEL 语义 | 已由测试验证 | `vector_rebuild_delta_capture_test.c`；产品写入保持提交，本次 rebuild 失败 |
| VGET 存在/不存在 | 已由测试验证 | `vector_testcase.c`、新增恢复测试 |
| VGET scope 隔离 | 已由测试验证 | `vector_testcase.c`、新增恢复测试 |
| VGET 删除后读取 | 已由测试验证 | `vector_testcase.c`、新增恢复测试 |
| VSEARCH topK | 已由测试验证 | `vector_testcase.c`、core correctness/product integration |
| VSEARCH threshold/空结果 | 已由测试验证 | `vector_testcase.c`、`vector_manager_resolve_fastpath_test.sh` |
| VSEARCH scope | 已由测试验证 | core correctness、manager fastpath、product integration |
| VSEARCH 删除过滤 | 已由测试验证 | core correctness、product integration、新增恢复测试 |
| VSEARCH 更新后结果 | 已由测试验证 | core correctness、manager fastpath、product integration |
| VDEL 正常/重复删除 | 已由测试验证 | `vector_testcase.c`、core correctness、product integration |
| VDEL tombstone 与同步重建清理 | 已由测试验证 | core tombstone/rebuild tests、`vector_testcase.c` |
| VINDEX 同步重建 | 已由测试验证 | core flat/v2 product tests |
| VINDEX async 状态/取消/重试 | 已由测试验证 | 新增测试覆盖公开 `ASYNC/STATUS/RETRY` 和 idle cancel；`vector_rebuild_async_worker_test.c` 覆盖成功取消、失败清理和重试 |
| VINFO records/index count | 已由测试验证 | core correctness、新增恢复测试 |
| VINFO backend memory/capacity | 已由测试验证 | `vector_vinfo_backend_stats_test.sh`，含 memory source 口径 |
| VINFO search profiling | 已由测试验证 | `vector_vsearch_profile_breakdown_test.sh` |
| VINFO rebuild/snapshot state | 已由测试验证 | async worker、snapshot VINFO tests |
| RDB 向量恢复 | 已由测试验证 | core RDB tests、新增 RDB-only restart |
| 纯 AOF 向量恢复 | 已由测试验证 | 新增测试在首次停止后删除 RDB/快照，只保留 AOF 再启动 |
| RDB+AOF 向量恢复 | 已由测试验证 | 新增测试保留两者，检查无重复 active record、更新和删除最终态 |
| 服务重启后 VGET/VSEARCH/VINFO | 已由测试验证 | 新增三类恢复测试及既有 product/RDB tests |
| 有效索引快照加载 | 已由测试验证 | `vector_index_snapshot_vinfo_status_test.sh` 执行真实 server restart |
| 快照缺失/损坏/不兼容 fallback | 已由测试验证 | `vector_index_snapshot_load_after_records_test.c` 覆盖缺失、参数不匹配、checksum、截断/损坏/反序列化失败 |
| 快照与 records 不匹配 | 已由测试验证 | manifest generation、record ID/vector checksum mismatch 测试 |
| 从 active records 重建索引 | 已由测试验证 | 新增测试删除快照后重启，日志确认 fallback；core rebuild/snapshot tests |
| 恢复后的 update/delete 正确性 | 已由测试验证 | 新增 RDB、纯 AOF、RDB+AOF 三种场景 |
| AOF 直接复用持久化 embedding、不重跑 ONNX | 尚未实现 | AOF append/rewrite 最终重放文本 `VSET`，经过 embedding prepare；`local_c` 会再次运行 tokenizer/ONNX |

本次审计未发现仍应保留为“只有文档声明”的 README 功能项；不准确的 AOF 表述已修正。

## 4. README 声明与证据

| README 声明 | 结论 | 证据 |
| --- | --- | --- |
| RDB/AOF 持久化产品记录 | 需拆分表述 | RDB 保存完整记录和 embedding；AOF 保存并重放 `VSET`/`VDEL` 命令 |
| 服务重启后恢复记录 | 已验证 | 新增三类 restart 测试 |
| 索引可由记录重新构建 | 已验证 | 缺失快照 restart 日志、snapshot fallback tests |
| 有效快照可以加载 | 已验证 | snapshot VINFO server restart |
| 快照缺失/损坏时回退重建 | 已验证 | missing/corrupt/incompatible/checksum tests |
| VSET/VDEL 更新在恢复后正确 | 已验证 | 三类恢复均检查 update 新值、deleted absence 和计数一致 |

此前证据不足的是“纯 AOF”与“RDB+AOF 不重复/最终态”两项：既有启用 AOF 的测试在优雅退出后保留了 RDB，不能单独证明纯 AOF。新增测试通过删除 RDB 和索引快照补齐该证据。

## 5. 新增持久化测试

`tests/vector_command_persistence_recovery_test.sh` 对每种模式使用独立临时 cwd、端口、RDB、AOF、快照和日志：

1. 写入两个 scope 的记录，覆盖并重复写入一条，删除并重复删除一条。
2. 发送 arity 错误的 `VSET`，确认记录和 backend count 不变。
3. 优雅停止后按场景保留 RDB、AOF 或两者；显式删除索引快照。
4. 重启后验证 `VGET`、`VSEARCH`、scope、update/delete 最终态。
5. 验证 `active_records=index_size=backend_size=core_hnsw_v2_active_count=3`。
6. 从 reload 日志确认 `manifest missing` 触发 fallback rebuild，并确认新快照生成。
7. 通过公开 RESP 验证 `VINDEX REBUILD ASYNC/STATUS/RETRY`，并验证 idle 状态拒绝 cancel；成功取消由 deterministic manager API 测试覆盖。

结果：RDB-only、AOF-only、RDB+AOF 全部 PASS。测试使用 hash embedding 和小规模确定性数据，不运行 ONNX 或 benchmark。

## 6. 恢复结果

| 场景 | 结果 |
| --- | --- |
| RDB | update/delete/scope/search 正确；records 与 index count 一致；无快照时从 records 重建 |
| AOF | 删除 RDB 后仅靠命令日志恢复成功；update/delete 最终态正确；count 一致 |
| RDB+AOF | RDB 后重放 AOF，无重复 active records；update/delete 最终态正确；count 一致 |
| 有效快照 | 加载成功，`loaded_from_snapshot=1`，查询正确 |
| 缺失/损坏/不兼容快照 | 拒绝无效快照，fallback rebuild，查询正确 |
| records/index 一致性 | 所有新增恢复场景均为 3/3；专项 snapshot tests 校验 record ID 和 vector checksum |

## 7. 缺陷与风险

未发现需要在本轮修改生产代码的持久化正确性缺陷。

两个剩余风险需要如实保留：

- `kvs_rdb_load()` 的向量段加载末尾调用一次 `kvs_vector_rebuild_after_load()`，`kvstore` 主启动流程在 AOF 后又调用一次。RDB-only 缺失快照时，第一次会 rebuild/write snapshot，第二次加载刚写出的 snapshot。最终结果正确，但会产生重复启动工作并覆盖第一次 fallback 的 VINFO 状态。本轮不做性能/启动流程重构。
- VSET commit 内部的分配失败和 index add 失败回滚只有代码路径，没有 fault-injection 测试。本轮新增的失败原子性证据只覆盖命令参数校验失败。

AOF 的 embedding 语义不是缺陷但必须明确：AOF append/rewrite 最终恢复的是文本命令，`local_c` 恢复会重新进行 ONNX 推理。若未来要求恢复时不推理，需要独立设计带 embedding 的内部持久化/重放格式；本轮不扩大协议或 AOF 格式。

## 8. 验证命令与结果

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"                         PASS
tests/vector_core_flat_correctness_test.sh                                PASS
tests/vector_core_hnsw_v2_default_backend_test.sh                         PASS
tests/vector_core_hnsw_v2_product_integration_test.sh                     PASS
tests/vector_manager_resolve_fastpath_test.sh                             PASS
tests/vector_rebuild_async_worker_test.sh                                 PASS
tests/vector_index_snapshot_load_after_records_test.sh                    PASS
tests/vector_index_snapshot_vinfo_status_test.sh                          PASS
tests/vector_vinfo_backend_stats_test.sh                                  PASS
tests/vector_vsearch_profile_breakdown_test.sh                            PASS
tests/vector_core_flat_rebuild_test.sh                                    PASS
tests/vector_core_flat_rdb_reload_test.sh                                 PASS
tests/vector_command_persistence_recovery_test.sh                         PASS
bash -n tests/vector_command_persistence_recovery_test.sh                 PASS
```

静态边界检查：

```text
grep -RIn "g_vector_state" src/vector/core include/vector
  -> 仅 include/vector/kvs_vector_manager.h 的 manager-owned extern 声明；core source 无引用

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core
  -> no output

grep -RIn "record_index" src/vector include
  -> 仅 manager/record store/index adapter 路径；src/vector/core 无引用

grep -RIn "kvs_vector_index_ops_t" include src/vector
  -> 现有声明及 core_flat/core_hnsw_v2 adapter 定义

git diff --check
  -> PASS
```

最终 `git status --short`：

```text
 M README.md
?? docs/vector_command_functionality_audit.md
?? tests/vector_command_persistence_recovery_test.sh
```

无生产代码修改，不涉及 core ownership 变化。

## 9. 结论

```text
v_command_audit_complete=yes
rdb_vector_recovery_verified=yes
aof_vector_recovery_verified=yes
snapshot_fallback_verified=yes
record_index_consistency_verified=yes
readme_claims_evidence_based=yes
ready_for_replication_audit=yes
```

`ready_for_replication_audit=yes` 只表示本轮单机命令和持久化证据已闭环，不表示 replication 已通过验证或可跳过独立审计。
