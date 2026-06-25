# Stage 3 core_hnsw closeout correctness report

## 1. Stage 3I 已完成说明

Stage 3I 已完成 core_hnsw 接入真实 V 族命令路径：

- 已支持 `KVS_VECTOR_INDEX_CORE_HNSW`。
- 已支持配置文件 `vector_index_type = core_hnsw`。
- 已支持命令行 `--vector_index_type core_hnsw`。
- manager state 已持有 `core_hnsw_index`。
- 已通过 old-ops adapter 接入现有 VSET / VSEARCH / VINDEX REBUILD 路径。
- VSET 写入 core_hnsw。
- VSEARCH 通过 core_hnsw 返回 `record_id / score`，再由 manager resolve record 并组装旧响应格式。
- VDEL 调用 `kvs_hnsw_index_mark_delete()`。
- VINDEX REBUILD 从 active records 重建 core_hnsw。
- RDB reload 后 `kvs_vector_rebuild_after_load()` 已覆盖 core_hnsw rebuild。
- VINFO 已输出 core_hnsw 相关字段。

## 2. 新增正式测试脚本

本轮新增：

- `tests/vector_core_hnsw_correctness_test.sh`
- `tests/vector_core_hnsw_rebuild_test.sh`
- `tests/vector_core_hnsw_rdb_reload_test.sh`
- `tests/vector_core_hnsw_aof_replay_test.sh`

测试脚本均使用临时目录、随机端口、独立配置文件和 RESP 客户端，不依赖默认数据目录。

## 3. correctness 测试结果

执行：

```sh
tests/vector_core_hnsw_correctness_test.sh
```

结果：

```text
vector_core_hnsw_correctness_test PASS
```

覆盖结果：

- 配置文件 `vector_index_type = core_hnsw` 可启动。
- 命令行 `--vector_index_type core_hnsw` 可覆盖配置文件。
- 默认 index type 未被改成 core_hnsw。
- VINFO 显示 `index_type:core_hnsw`。
- VSET 多条记录后 VSEARCH 保持原有响应格式。
- 响应中未暴露 `record_id` / `slot`。
- scopeA / scopeB 隔离正确。
- threshold 过滤仍由 manager 层完成。
- 同 key/scope update 后 answer 正确更新。
- update 后不产生重复可见结果。
- VDEL 后不返回 deleted record。
- repeated delete 不崩溃，不重复增加 `core_hnsw_deleted_count`。
- VINFO 中 `core_hnsw_size / core_hnsw_capacity / core_hnsw_m / core_hnsw_m0 / core_hnsw_ef_construction / core_hnsw_ef_search / core_hnsw_max_level / core_hnsw_deleted_count` 字段存在且参数值合理。

## 4. rebuild 测试结果

执行：

```sh
tests/vector_core_hnsw_rebuild_test.sh
```

结果：

```text
vector_core_hnsw_rebuild_test PASS
```

覆盖结果：

- VSET 多条 scopeA / scopeB 记录。
- VDEL 删除部分记录。
- VINDEX REBUILD 成功。
- rebuild 后 active records 仍可查。
- deleted records 不可见。
- `core_hnsw_deleted_count` rebuild 后回到 `0`。
- VINFO 仍显示 `index_type:core_hnsw`。
- scope 过滤仍正确。
- graph 不作为 source of truth，只从 record store active records rebuild。

## 5. RDB reload 测试结果

执行：

```sh
tests/vector_core_hnsw_rdb_reload_test.sh
```

结果：

```text
vector_core_hnsw_rdb_reload_test PASS
```

覆盖结果：

- core_hnsw 启动后写入多条 VSET。
- update 一条记录。
- delete 一条记录。
- SIGTERM 优雅退出生成 RDB。
- 使用同一 RDB 重新启动。
- reload 后 VGET active record 正常。
- reload 后 VSEARCH active records 正常。
- reload 后 updated answer 正确。
- reload 后 deleted record 不可见。
- reload 后 VINDEX REBUILD 成功。
- reload + rebuild 后 `core_hnsw_deleted_count` 为 `0`。
- RDB 不保存 HNSW graph，core_hnsw 从 record store rebuild。

## 6. AOF replay 测试结果

执行：

```sh
tests/vector_core_hnsw_aof_replay_test.sh
```

结果：

```text
vector_core_hnsw_aof_replay_test PASS
```

覆盖结果：

- 启动 kvstore 并启用 AOF。
- 配置 `vector_index_type = core_hnsw`。
- VSET 多条记录。
- update 一条记录。
- VDEL 一条记录。
- SIGTERM 优雅退出。
- 使用同一 AOF replay 重新启动。
- VGET active record 正常。
- VSEARCH active records 正常。
- updated answer 正确。
- deleted record 不可见。
- VINDEX REBUILD 成功。
- replay 后 core_hnsw 可从 restored records rebuild。
- 未修改 AOF 格式。

## 7. VINFO core_hnsw 字段完整性

VINFO 已验证以下 core_hnsw 字段：

- `index_type:core_hnsw`
- `core_hnsw_size`
- `core_hnsw_capacity`
- `core_hnsw_m`
- `core_hnsw_m0`
- `core_hnsw_ef_construction`
- `core_hnsw_ef_search`
- `core_hnsw_max_level`
- `core_hnsw_deleted_count`

测试配置中 `vector_hnsw_m = 8`，因此 VINFO 返回 `core_hnsw_m:8`、`core_hnsw_m0:16`、`core_hnsw_ef_construction:64`、`core_hnsw_ef_search:64`。

## 8. 静态边界检查结果

执行：

```sh
grep -RIn "g_vector_state" \
  include/vector/kvs_vector_hnsw.h \
  src/vector/core/hnsw 2>/dev/null
```

结果：无输出。

执行：

```sh
grep -RIn "question\|answer\|scope\|TTL\|metadata" \
  include/vector/kvs_vector_hnsw.h \
  src/vector/core/hnsw 2>/dev/null
```

结果：

```text
include/vector/kvs_vector_hnsw.h:20: *   question / answer / scope / TTL / metadata / manager store
```

该输出只出现在边界注释中，core_hnsw 逻辑代码未处理这些业务语义。

执行：

```sh
rg -n "kvs_vector_manager|kvs_vector_search_result_t|record store|g_vector_state" \
  include/vector/kvs_vector_hnsw.h src/vector/core/hnsw
```

结果：无输出。

## 9. search 热路径无 malloc/free 检查结果

执行：

```sh
rg -n "kvs_malloc|kvs_free|malloc|free" src/vector/core/hnsw/kvs_hnsw_search.c
```

结果：无输出。

说明：core_hnsw search 热路径未出现 `kvs_malloc` / `kvs_free` / `malloc` / `free` 调用。

## 10. 全量构建结果

执行：

```sh
make clean && make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：通过。

## 11. 是否修改生产代码

本轮 Stage 3 Final Correctness Gate / Closeout 未修改生产代码。

本轮只新增正式测试脚本和 closeout 报告。

## 12. 当前 git status --short

```text
 M Makefile
 D docs/vector_core_reset/00_vector_core_slimming_report.md
 D docs/vector_core_reset/01_vector_manager_kernel_design.md
 D docs/vector_core_reset/02_third_party_vector_engine_audit.md
 D docs/vector_core_reset/03_kvlite_vector_manager_kernel_design.md
 D docs/vector_core_reset/04_vector_compute_kernel_design.md
 D docs/vector_core_reset/05_vector_backend_reselection_plan.md
 D docs/vector_core_reset/06_performance_first_self_hnsw_design.md
 D docs/vector_core_reset/07_usearch_inspired_flat_hnsw_core_execution_plan.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_command_executor.c
 M src/core/kvs_config.c
 M src/core/kvs_operation.c
 M src/vector/kvs_vector.c
?? docs/vector_core_reset/stage3_core_hnsw_closeout_correctness_report.md
?? docs/vector_core_reset/stage3abc_core_hnsw_lifecycle_context_report.md
?? docs/vector_core_reset/stage3de_core_hnsw_heap_random_report.md
?? docs/vector_core_reset/stage3f2_core_hnsw_multilayer_add_report.md
?? docs/vector_core_reset/stage3f_core_hnsw_add_report.md
?? docs/vector_core_reset/stage3gh_core_hnsw_search_delete_report.md
?? docs/vector_core_reset/stage3i_core_hnsw_integration_report.md
?? include/vector/kvs_vector_hnsw.h
?? src/vector/core/hnsw/
?? tests/vector_core_hnsw_aof_replay_test.sh
?? tests/vector_core_hnsw_correctness_test.sh
?? tests/vector_core_hnsw_rdb_reload_test.sh
?? tests/vector_core_hnsw_rebuild_test.sh
```

说明：上面的生产代码修改、core_hnsw 源码新增、Stage 3 中间报告以及旧文档删除为此前阶段已有状态。本轮未处理 unrelated docs 删除。

## 13. Stage 3 最终验收结论

Stage 3 Final Correctness Gate / Closeout 已通过。

core_hnsw 在真实 V 族命令路径下完成：

- correctness 验证。
- rebuild 验证。
- RDB reload + rebuild 验证。
- AOF replay + rebuild 验证。
- VINFO 字段完整性验证。
- core_hnsw 与 manager / record store / 业务字段边界验证。
- search 热路径无 malloc/free 验证。
- 全量构建验证。

## 14. benchmark 状态

本轮未做 benchmark。
