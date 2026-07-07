# CODE-HYGIENE-0: legacy core_hnsw 删除收尾报告

## AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`。

本阶段目标与 AGENTS 的稳定边界没有实质冲突：AGENTS 要求保持 manager / vector_core 边界、不修改 RDB/AOF、不修改 VSEARCH response、不修改 `kvs_vector_index_ops_t`、不删除 core_flat。本阶段删除的是用户明确授权的 legacy `src/vector/core/hnsw` 实现，并保留 `core_hnsw_v2` 默认主线与 `core_flat` fallback。

## 本阶段实际目标

一次性完成 legacy `core_hnsw` 删除收尾：

- 删除 `src/vector/core/hnsw/`
- 删除 `include/vector/kvs_vector_hnsw.h`
- 清理 legacy `core_hnsw` 构建、测试和默认产品路径引用
- 保持默认 ANN backend 为 `core_hnsw_v2`
- 保持 explicit `core_hnsw` 请求 fallback 到 `core_flat`

## 删除的文件清单

- `include/vector/kvs_vector_hnsw.h`
- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/core/hnsw/kvs_hnsw_context.c`
- `src/vector/core/hnsw/kvs_hnsw_delete.c`
- `src/vector/core/hnsw/kvs_hnsw_graph.c`
- `src/vector/core/hnsw/kvs_hnsw_index.c`
- `src/vector/core/hnsw/kvs_hnsw_info.c`
- `src/vector/core/hnsw/kvs_hnsw_random.c`
- `src/vector/core/hnsw/kvs_hnsw_search.c`
- `tests/vector_core_hnsw_aof_replay_test.sh`
- `tests/vector_core_hnsw_correctness_test.sh`
- `tests/vector_core_hnsw_rdb_reload_test.sh`
- `tests/vector_core_hnsw_rebuild_test.sh`

## 修改的文件清单

- `Makefile`
- `include/kvs_config.h`
- `include/vector/kvs_vector_index_internal.h`
- `include/vector/kvs_vector_manager.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_faiss_ops.c`
- `src/vector/kvs_vector_hnswlib_ops.c`
- `tests/README.md`
- `tests/vector_core_hnsw_v2_default_backend_test.c`
- `tests/vector_core_hnsw_v2_default_backend_test.sh`
- `tests/vector_manager_resolve_fastpath_test.sh`
- `tests/vector_vinfo_backend_stats_test.sh`
- `tests/vector_vsearch_profile_breakdown_test.sh`
- `docs/hnsw_index_snapshot/codebase_hygiene_before_s1_report.md`

## 构建系统清理

`Makefile` 已不再编译或链接 `src/vector/core/hnsw/*.c`。`make` 链接输出中只包含：

- `src/vector/core/flat/*.o`
- `src/vector/core/hnsw_v2/*.o`
- HNSWLIB optional baseline objects

未再出现 legacy `src/vector/core/hnsw/kvs_hnsw_*.o`。

保留内容：

- `HNSW_V2_SRCS` / `HNSW_V2_OBJS`
- HNSWLIB optional baseline
- FAISS parity
- `core_flat`

## 产品路径行为

默认 backend 仍为 `core_hnsw_v2`。

explicit `core_hnsw` 请求的新行为：

```text
vector_index_type = core_hnsw
  -> kvs_vector_index_type_from_config()
  -> log: core_hnsw has been removed; falling back to core_flat
  -> KVS_VECTOR_INDEX_CORE_FLAT
```

因此 legacy `core_hnsw` 不再可创建，也不会进入已删除实现。兼容配置字符串保留，fallback 为 `core_flat`。

## 删除或移除的 legacy 测试

已删除 legacy-only shell 测试：

- `tests/vector_core_hnsw_correctness_test.sh`
- `tests/vector_core_hnsw_rebuild_test.sh`
- `tests/vector_core_hnsw_rdb_reload_test.sh`
- `tests/vector_core_hnsw_aof_replay_test.sh`

已更新 default-backend 测试：

- `tests/vector_core_hnsw_v2_default_backend_test.c`：explicit `core_hnsw` 断言改为 fallback 到 `core_flat`
- `tests/vector_core_hnsw_v2_default_backend_test.sh`：explicit `core_hnsw` smoke 改为期望 `core_flat`

已从以下测试矩阵移除 legacy `core_hnsw`：

- `tests/vector_manager_resolve_fastpath_test.sh`
- `tests/vector_vinfo_backend_stats_test.sh`
- `tests/vector_vsearch_profile_breakdown_test.sh`

保留 `tests/vector_core_hnsw_faiss_parity_*`，因为它是 FAISS parity / `core_hnsw_v2` 对照测试，不依赖已删除的 legacy `src/vector/core/hnsw` 实现。

## 保留的 core_hnsw_v2 主线

保留且未重构：

- `include/vector/kvs_vector_hnsw_v2.h`
- `src/vector/core/hnsw_v2/`
- `tests/vector_core_hnsw_v2_*`

`core_hnsw_v2` 仍是默认 ANN backend。`core_flat` 仍是 exact baseline / fallback。

## 保留的非 legacy HNSW 内容

- `HNSW_V2`
- HNSWLIB optional baseline
- FAISS parity
- `hnsw_v2` benchmark / gate / product integration 测试
- 旧 native/adapter 统计字段 `hnsw_search_core_*` / `hnsw_search_export_*`，它们属于旧 native adapter 统计，不依赖已删除的 `src/vector/core/hnsw`

## 未触碰边界

未触碰：

- RDB/AOF 格式
- VSEARCH response
- `kvs_vector_index_ops_t`
- `core_hnsw_v2` graph/search implementation
- snapshot / manifest
- background rebuild
- SIMD / layout 优化

## 测试结果

```bash
make -j"$(nproc)"
```

结果：PASS。

```bash
tests/vector_core_hnsw_v2_default_backend_test.sh
```

结果：PASS。关键语义：默认选择 `core_hnsw_v2`，explicit `core_hnsw` fallback 到 `core_flat`。

```bash
tests/vector_core_hnsw_v2_product_integration_test.sh
```

结果：PASS。

```bash
tests/vector_core_hnsw_v2_search_recall_test.sh
```

结果：PASS。关键输出：

```text
1K/512d ef=256 recall@10 = 1.0000
5K/512d ef=256 recall@10 = 1.0000
10K/512d ef=64  recall@10 = 0.7780
10K/512d ef=128 recall@10 = 0.9580
10K/512d ef=256 recall@10 = 0.9980
10K/512d ef=512 recall@10 = 1.0000
10K/512d ef=1024 recall@10 = 1.0000
```

```bash
tests/vector_core_flat_correctness_test.sh
```

结果：PASS。

## v2 boundary grep 结果

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
```

结果：均无输出。未发现 `core_hnsw_v2` 访问 manager-owned semantic state。

## legacy 残留审计

```bash
rg -n "src/vector/core/hnsw|core/hnsw|kvs_vector_hnsw\.h|kvs_vector_core_hnsw_|kvs_hnsw_|core_hnsw_ops|HNSW_CORE_SRCS|HNSW_CORE_OBJS" \
  include src tests Makefile \
  --glob '!src/vector/core/hnsw_v2/**' \
  --glob '!include/vector/kvs_vector_hnsw_v2.h' || true
```

结果：无 legacy implementation / header / build object 引用；输出仅为 `hnsw_v2` 与保留的 parity/v2 测试符号。

仍有 `core_hnsw` 字符串残留，分类如下：

- `include/kvs_vector.h`：public enum `KVS_VECTOR_INDEX_CORE_HNSW` 和 legacy VINFO struct fields，保留为 API / compatibility residue，不链接已删除实现。
- `src/vector/kvs_vector.c`：配置字符串 fallback 分支，保留用于 explicit `core_hnsw -> core_flat`。
- `src/core/kvs_operation.c` / `src/core/kvs_command_executor.c`：dead compatibility display branches for public enum，当前产品路径不会进入。
- `include/kvs_config.h` / `tests/README.md`：说明 legacy 已删除、explicit fallback 到 `core_flat`。
- `tests/vector_core_hnsw_faiss_parity_*`：FAISS parity / `core_hnsw_v2` 对照测试文件名，保留。

## git diff --check

```bash
git diff --check
```

结果：PASS，无输出。

## git status --short

```text
 M Makefile
 M include/kvs_config.h
 D include/vector/kvs_vector_hnsw.h
 M include/vector/kvs_vector_index_internal.h
 M include/vector/kvs_vector_manager.h
 D src/vector/core/hnsw/kvs_hnsw_add.c
 D src/vector/core/hnsw/kvs_hnsw_context.c
 D src/vector/core/hnsw/kvs_hnsw_delete.c
 D src/vector/core/hnsw/kvs_hnsw_graph.c
 D src/vector/core/hnsw/kvs_hnsw_index.c
 D src/vector/core/hnsw/kvs_hnsw_info.c
 D src/vector/core/hnsw/kvs_hnsw_random.c
 D src/vector/core/hnsw/kvs_hnsw_search.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_faiss_ops.c
 M src/vector/kvs_vector_hnswlib_ops.c
 M tests/README.md
 D tests/vector_core_hnsw_aof_replay_test.sh
 D tests/vector_core_hnsw_correctness_test.sh
 D tests/vector_core_hnsw_rdb_reload_test.sh
 D tests/vector_core_hnsw_rebuild_test.sh
 M tests/vector_core_hnsw_v2_default_backend_test.c
 M tests/vector_core_hnsw_v2_default_backend_test.sh
 M tests/vector_manager_resolve_fastpath_test.sh
 M tests/vector_vinfo_backend_stats_test.sh
 M tests/vector_vsearch_profile_breakdown_test.sh
?? docs/hnsw_index_snapshot/
?? docs/hnsw_productization/hnsw_p5_build_rebuild_bottleneck_analysis.md
```

## CODE-HYGIENE-0 deletion decision

```text
src_vector_core_hnsw_deleted = yes
production_code_changed = yes
default_backend_still_core_hnsw_v2 = yes
explicit_core_hnsw_behavior = fallback_to_core_flat
legacy_hnsw_build_refs_removed = yes
legacy_hnsw_default_tests_removed = yes
core_hnsw_v2_touched = no
core_flat_touched = no
rdb_aof_touched = no
vsearch_response_touched = no
kvs_vector_index_ops_t_touched = no
boundary_violation_found = no
build_passed = yes
core_tests_passed = yes
safe_to_continue_next_file = yes
```

## Legacy core_hnsw compatibility cleanup after deletion

### AGENTS.md 遵守情况

本轮再次读取并遵守根目录 `AGENTS.md`。本任务与 AGENTS 的稳定边界无冲突：本轮只收口 legacy `core_hnsw` 删除后的配置、说明、测试期望和兼容显示；未修改 RDB/AOF、VSEARCH response、`kvs_vector_index_ops_t`，未触碰 `src/vector/core/hnsw_v2/` graph/search implementation，也未进入 HNSW-S1 snapshot / manifest 实现。

### 本轮是否修改生产代码

是。修改仅限 legacy `core_hnsw` 删除后的兼容面：

- `src/vector/kvs_vector.c`：`vector_index_type=core_hnsw` 统一 fallback 到 `core_flat`；`KVS_VECTOR_INDEX_CORE_HNSW` type name 显示为 `core_hnsw_removed`；删除 legacy `core_hnsw` wrapper / info / memory 代码。
- `src/core/kvs_operation.c`、`src/core/kvs_command_executor.c`：dead compatibility display branch 不再显示可用的 `core_hnsw`，改为 `core_hnsw_removed`；补齐 `core_hnsw_v2` 显示分支。
- `include/kvs_config.h`、`include/kvs_vector.h`、`include/vector/kvs_vector_manager.h`：注释与 manager state 对齐删除后的真实结构；保留 public enum / VINFO 字段作为兼容残留，不链接已删除实现。
- `src/vector/kvs_vector_faiss_ops.c`、`src/vector/kvs_vector_hnswlib_ops.c`：注释中不再把 `core_hnsw` 写成当前 overfetch 参考。

### 本轮修改文件列表

- `include/kvs_config.h`
- `include/kvs_vector.h`
- `include/vector/kvs_vector_index_internal.h`
- `include/vector/kvs_vector_manager.h`
- `src/core/kvs_command_executor.c`
- `src/core/kvs_operation.c`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_faiss_ops.c`
- `src/vector/kvs_vector_hnswlib_ops.c`
- `tests/README.md`
- `tests/vector_core_hnsw_v2_default_backend_test.c`
- `tests/vector_core_hnsw_v2_default_backend_test.sh`
- `tests/vector_manager_resolve_fastpath_test.sh`
- `tests/vector_vinfo_backend_stats_test.sh`
- `tests/vector_vsearch_profile_breakdown_test.sh`
- `docs/hnsw_index_snapshot/codebase_hygiene_before_s1_report.md`
- `docs/hnsw_productization/README.md`
- `docs/hnsw_productization/hnsw_p0_recall_attribution_report.md`
- `docs/hnsw_productization/hnsw_p2_100k_balanced_default_report.md`
- `docs/hnsw_productization/hnsw_p5_rebuild_cold_start_report.md`
- `docs/hnsw_productization/hnsw_p5_rebuild_cold_start_strategy.md`
- `docs/hnsw_productization/interview_narrative.md`
- `docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md`
- `docs/vector_core_reset/core_hnsw_v2_final_architecture.md`
- `docs/vector_core_reset/core_hnsw_v2_interview_review.md`
- `docs/vector_core_reset/stage9g2_default_backend_switch_hardening.md`
- `docs/vector_core_reset/stage9g3_scale_stability_final_consolidation.md`

### core_hnsw 残留字符串分类

- compatibility enum：`KVS_VECTOR_INDEX_CORE_HNSW` 保留在 public enum 中，避免 API 编号兼容风险；当前创建路径不会返回该类型。
- compatibility config string：配置文件和 CLI 仍接受 `core_hnsw`，但只作为旧配置兼容入口。
- fallback behavior：`core_hnsw` 统一解析为 `KVS_VECTOR_INDEX_CORE_FLAT`，并记录日志 `core_hnsw has been removed; falling back to core_flat`。
- historical docs：旧阶段报告中涉及 legacy `core_hnsw` 的当前状态说明已补充 superseded / removed 语义；历史阶段数据本身保留。
- removed tests/build refs：`Makefile` 不再编译 `src/vector/core/hnsw/*.c`；legacy-only `tests/vector_core_hnsw_*` shell tests 已删除；服务级测试矩阵不再期待 legacy `core_hnsw` backend 可用。

### 当前行为

- explicit `core_hnsw` 当前行为：`fallback_to_core_flat`。
- default backend 当前行为：未配置 `vector_index_type` 时默认 `core_hnsw_v2`。
- `core_flat` fallback 当前行为：`flat` / `core_flat` / removed `core_hnsw` 兼容入口都可以得到 `core_flat` baseline / fallback。
- 已删除实现引用：未发现生产代码调用 `kvs_vector_core_hnsw_*`、`kvs_hnsw_*` legacy implementation，未发现 `include/vector/kvs_vector_hnsw.h` 引用，未发现 `src/vector/core/hnsw` 构建引用。

### 文档和测试说明更新

- `tests/README.md` 已改为说明 legacy `core_hnsw` implementation 已删除，显式 `core_hnsw` 仅作为 `core_flat` fallback 兼容入口。
- `docs/hnsw_productization/*` 和关键 `docs/vector_core_reset/*` 当前状态说明已更新为：legacy `core_hnsw` source removed；compat enum/config string may remain；explicit `core_hnsw` resolves to `core_flat` fallback；`core_hnsw_v2` remains default ANN backend。

### 执行命令与结果

```bash
make -j"$(nproc)"
```

结果：PASS。

为运行覆盖 FAISS backend 的服务级测试矩阵，又执行：

```bash
make clean && make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：PASS。说明：不 clean 时已有对象不会因 `ENABLE_FAISS` 切换自动重编，服务级 FAISS 矩阵会降级到 `flat`；干净构建后 FAISS / HNSWLIB / core_flat 矩阵均通过。

```bash
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_flat_correctness_test.sh
tests/vector_manager_resolve_fastpath_test.sh
tests/vector_vinfo_backend_stats_test.sh
tests/vector_vsearch_profile_breakdown_test.sh
```

结果：全部 PASS。

`tests/vector_core_hnsw_v2_search_recall_test.sh` 关键输出：

```text
1K/512d ef=256 recall@10 = 1.0000
5K/512d ef=256 recall@10 = 1.0000
10K/512d ef=64  recall@10 = 0.7780
10K/512d ef=128 recall@10 = 0.9580
10K/512d ef=256 recall@10 = 0.9980
10K/512d ef=512 recall@10 = 1.0000
10K/512d ef=1024 recall@10 = 1.0000
```

### v2 boundary grep 结果

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
```

结果：均无输出。

### legacy implementation 引用检查

```bash
rg -n "src/vector/core/hnsw|core/hnsw|kvs_vector_hnsw\.h|kvs_vector_core_hnsw_|kvs_hnsw_|core_hnsw_ops|HNSW_CORE_SRCS|HNSW_CORE_OBJS" \
  include src tests Makefile \
  --glob '!src/vector/core/hnsw_v2/**' \
  --glob '!include/vector/kvs_vector_hnsw_v2.h' || true
```

结果：无已删除 legacy implementation / header / build object 引用。命中项均为 `core_hnsw_v2` 路径、v2 测试、v2 product integration / rebuild shadow 调用，属于保留主线。

### git diff --check

```bash
git diff --check
```

结果：PASS，无输出。

### git status --short

```text
 M Makefile
 M docs/hnsw_productization/README.md
 M docs/hnsw_productization/hnsw_p0_recall_attribution_report.md
 M docs/hnsw_productization/hnsw_p2_100k_balanced_default_report.md
 M docs/hnsw_productization/hnsw_p5_rebuild_cold_start_report.md
 M docs/hnsw_productization/hnsw_p5_rebuild_cold_start_strategy.md
 M docs/hnsw_productization/interview_narrative.md
 M docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md
 M docs/vector_core_reset/core_hnsw_v2_final_architecture.md
 M docs/vector_core_reset/core_hnsw_v2_interview_review.md
 M docs/vector_core_reset/stage9g2_default_backend_switch_hardening.md
 M docs/vector_core_reset/stage9g3_scale_stability_final_consolidation.md
 M include/kvs_config.h
 M include/kvs_vector.h
 D include/vector/kvs_vector_hnsw.h
 M include/vector/kvs_vector_index_internal.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_command_executor.c
 M src/core/kvs_operation.c
 D src/vector/core/hnsw/kvs_hnsw_add.c
 D src/vector/core/hnsw/kvs_hnsw_context.c
 D src/vector/core/hnsw/kvs_hnsw_delete.c
 D src/vector/core/hnsw/kvs_hnsw_graph.c
 D src/vector/core/hnsw/kvs_hnsw_index.c
 D src/vector/core/hnsw/kvs_hnsw_info.c
 D src/vector/core/hnsw/kvs_hnsw_random.c
 D src/vector/core/hnsw/kvs_hnsw_search.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_faiss_ops.c
 M src/vector/kvs_vector_hnswlib_ops.c
 M tests/README.md
 D tests/vector_core_hnsw_aof_replay_test.sh
 D tests/vector_core_hnsw_correctness_test.sh
 D tests/vector_core_hnsw_rdb_reload_test.sh
 D tests/vector_core_hnsw_rebuild_test.sh
 M tests/vector_core_hnsw_v2_default_backend_test.c
 M tests/vector_core_hnsw_v2_default_backend_test.sh
 M tests/vector_manager_resolve_fastpath_test.sh
 M tests/vector_vinfo_backend_stats_test.sh
 M tests/vector_vsearch_profile_breakdown_test.sh
?? docs/hnsw_index_snapshot/
?? docs/hnsw_productization/hnsw_p5_build_rebuild_bottleneck_analysis.md
```

## CODE-HYGIENE-0 compatibility cleanup decision

```text
legacy_core_hnsw_source_removed = yes
legacy_core_hnsw_header_removed = yes
production_code_changed = yes
default_backend_still_core_hnsw_v2 = yes
explicit_core_hnsw_behavior = fallback_to_core_flat
core_hnsw_remaining_refs_are_compat_or_docs_only = yes
legacy_hnsw_build_refs_removed = yes
legacy_hnsw_default_tests_removed = yes
core_hnsw_v2_graph_search_touched = no
core_flat_implementation_touched = no
rdb_aof_touched = no
vsearch_response_touched = no
kvs_vector_index_ops_t_touched = no
boundary_violation_found = no
build_passed = yes
core_tests_passed = yes
safe_to_start_HNSW_S1 = yes
```

# CODE-HYGIENE-1: src/vector source-by-source audit and profiling cleanup

## 报告位置

本阶段详细审计报告已新增：

- `docs/hnsw_index_snapshot/vector_source_hygiene_audit.md`

## AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`。本阶段与 AGENTS 的稳定边界无冲突。

## 本阶段目标

CODE-HYGIENE-1 没有进入 HNSW-S1，也没有实现 snapshot、manifest、background rebuild。实际目标是逐个审计 `src/vector` 和相关 include 文件，识别 large-file / profiling-heavy 候选，并只执行低风险 cleanup。

## 审计结论摘要

逐文件审计已覆盖：

- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_index.c`
- `src/vector/kvs_vector_embedding.c`
- `src/vector/kvs_vector_distance*.c`
- `src/vector/core/kvs_vector_heap.c`
- `src/vector/core/kvs_vector_id_slot_map.c`
- `src/vector/kvs_vector_rebuild*.c`
- `src/vector/kvs_vector_faiss_*`
- `src/vector/kvs_vector_hnswlib_*`
- `src/vector/core/flat/*`
- `src/vector/core/hnsw_v2/*`
- `include/kvs_vector.h`
- `include/kvs_vector_distance.h`
- `include/kvs_vector_hnswlib_backend.h`
- `include/vector/*.h`

识别出的主要 large-file / profiling-heavy 候选：

- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_index.c`
- `src/vector/kvs_vector_embedding.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c`
- `include/kvs_vector.h`
- `include/vector/kvs_vector_hnsw_v2.h`

## 本阶段 cleanup

已执行的低风险 cleanup：

- `src/vector/kvs_vector_hnswlib_ops.c`：补充 optional baseline 说明，避免与已删除 legacy `core_hnsw` 混淆。
- `include/kvs_vector_hnswlib_backend.h`：补充 hnswlib C ABI optional baseline 边界说明。
- `src/vector/kvs_vector_index.c`：清理重复/过时注释和已注释掉的 obsolete placeholder。
- `src/vector/kvs_vector.c`：清理已注释掉的 obsolete helper/debug 注释块。

暂缓的 cleanup：

- 不拆 `src/vector/kvs_vector.c`。
- 不拆 `src/vector/kvs_vector_index.c`。
- 不拆 `src/vector/kvs_vector_embedding.c`。
- 不改 `include/kvs_vector.h` stats schema。
- 不改 `include/vector/kvs_vector_index_internal.h` 的 `kvs_vector_index_ops_t`。
- 不改 `core_hnsw_v2` graph/search/add/neighbor_select。

## 边界状态

- 默认 backend 仍为 `core_hnsw_v2`。
- explicit `core_hnsw` 行为仍为 `fallback_to_core_flat`。
- `core_flat` 仍保留为 exact baseline / fallback。
- FAISS / hnswlib 仍保留为 optional baseline。
- RDB/AOF 未触碰。
- VSEARCH response 未触碰。
- `kvs_vector_index_ops_t` 签名未触碰。
- `core_hnsw_v2` graph/search implementation 未触碰。

## 验证结果

构建和测试已通过：

```bash
make -j"$(nproc)"
make clean && make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_flat_correctness_test.sh
tests/vector_manager_resolve_fastpath_test.sh
tests/vector_vinfo_backend_stats_test.sh
tests/vector_vsearch_profile_breakdown_test.sh
```

结果：PASS。

`core_hnsw_v2` boundary grep 无输出，未发现 manager-owned semantic state 越界。

`git diff --check`：PASS。

## CODE-HYGIENE-1 decision

```text
source_by_source_audit_done = yes
only_rg_scan = no
production_code_changed = yes
default_backend_still_core_hnsw_v2 = yes
explicit_core_hnsw_behavior = fallback_to_core_flat
legacy_core_hnsw_impl_refs_removed = yes
profiling_heavy_files_identified = yes
profiling_cleanup_done = yes
large_split_deferred = yes
core_hnsw_v2_graph_search_touched = no
core_flat_implementation_touched = no
faiss_hnswlib_optional_baseline_preserved = yes
rdb_aof_touched = no
vsearch_response_touched = no
kvs_vector_index_ops_t_touched = no
boundary_violation_found = no
build_passed = yes
core_tests_passed = yes
safe_to_start_HNSW_S1 = yes
```

# CODE-HYGIENE-2: hnsw_v2 header relocation

## 报告位置

本阶段详细报告已新增：

- `docs/hnsw_index_snapshot/hnsw_v2_header_relocation_report.md`

## AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`。本阶段与 manager/vector_core 边界无冲突。

## 本阶段目标

只做 `core_hnsw_v2` helper header relocation：

- 将 hnsw_v2 heap helper header 迁移到 `include/vector/kvs_vector_hnsw_v2_heap.h`
- 将 hnsw_v2 score cache helper header 迁移到 `include/vector/kvs_vector_hnsw_v2_score_cache.h`
- 更新 source include 和 Makefile header dependency
- 确认 `src/vector/core/hnsw_v2/` 下不再有 header 文件

这两个 header 仍是 v2 internal helper，只是为了目录卫生放在 `include/vector/`，不代表成为 user-facing public API。

## 边界状态

- 默认 backend 仍为 `core_hnsw_v2`。
- explicit `core_hnsw` 行为仍为 `fallback_to_core_flat`。
- 未修改 hnsw_v2 graph/search/add/neighbor_select 算法逻辑。
- 未修改 RDB/AOF。
- 未修改 VSEARCH response。
- 未修改 `kvs_vector_index_ops_t`。
- 未触碰 core_flat implementation。
- 未进入 HNSW-S1。

## 验证结果

执行并通过：

```bash
make -j"$(nproc)"
make clean && make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_flat_correctness_test.sh
```

`core_hnsw_v2` boundary grep 无输出。`git diff --check`：PASS。

## CODE-HYGIENE-2 decision

```text
hnsw_v2_headers_moved = yes
src_vector_core_hnsw_v2_has_headers = no
moved_headers_under_include_vector = yes
include_references_updated = yes
production_code_changed = yes
graph_search_algorithm_touched = no
default_backend_still_core_hnsw_v2 = yes
explicit_core_hnsw_behavior = fallback_to_core_flat
core_flat_touched = no
faiss_hnswlib_optional_baseline_preserved = yes
rdb_aof_touched = no
vsearch_response_touched = no
kvs_vector_index_ops_t_touched = no
boundary_violation_found = no
build_passed = yes
core_tests_passed = yes
safe_to_start_HNSW_S1 = yes
```

# CODE-HYGIENE-4B: external backend removal update

本阶段之后，CODE-HYGIENE-0 / 1 / 2 中提到的 FAISS / hnswlib optional baseline 已不再保留为当前代码路径。

当前状态：

- `src/vector/kvs_vector_faiss_ops.c` 已删除。
- `src/vector/kvs_vector_faiss_backend.cpp` 已删除。
- `include/kvs_vector_faiss_backend.h` 已删除。
- `src/vector/kvs_vector_hnswlib_ops.c` 已删除。
- `src/vector/kvs_vector_hnswlib_backend.cpp` 已删除。
- `include/kvs_vector_hnswlib_backend.h` 已删除。
- FAISS / hnswlib Makefile 编译选项、objects、bench/parity targets 已移除。
- FAISS / hnswlib direct parity / smoke / benchmark 测试已移除。
- 旧配置名 `faiss` / `faiss_flat` / `faiss_hnsw` / `hnsw` / `hnswlib` / `hnswlib_hnsw` 统一 fallback 到 `core_flat`。
- 默认 backend 仍为 `core_hnsw_v2`。
- `core_flat` 仍为 exact baseline / fallback。

保留项：

- public enum 编号保留，避免破坏 API / command-layer schema。
- VINFO 中 FAISS / hnswlib 字段保留，运行时固定 disabled/0。
- 历史 docs 中的 FAISS parity 数据保留为 historical evidence，不代表当前仍有可构建 external backend。

```text
external_faiss_removed = yes
external_hnswlib_removed = yes
faiss_hnswlib_optional_baseline_preserved = no
```
