# CODE-HYGIENE-2: hnsw_v2 header relocation

## AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`。本阶段只做 `core_hnsw_v2` helper header 迁移、include 路径更新和文档报告更新；未修改 RDB/AOF、VSEARCH response、`kvs_vector_index_ops_t`、默认 backend、core_flat、FAISS/hnswlib optional baseline。

本阶段报告位置沿用当前 HNSW snapshot hygiene 路线的 `docs/hnsw_index_snapshot/`；AGENTS 的 manager/vector_core 边界和验证要求已遵守，无实质冲突。

## 本阶段目标

让 `src/vector/core/hnsw_v2/` 下不再放置 header 文件：

- `src/vector/core/hnsw_v2/` 只保留 `.c` 源文件。
- v2 helper headers 迁移到 `include/vector/`。
- `include/vector/kvs_vector_hnsw_v2.h` 保持原位，不移动。
- 不进入 HNSW-S1，不实现 snapshot / manifest / background rebuild。

## 迁移前头文件清单

迁移前 `src/vector/core/hnsw_v2/` 下只有两个 helper header：

- heap helper header
- score cache helper header

`include/vector/kvs_vector_hnsw_v2.h` 已存在并保持在 `include/vector/`。

## 逐文件判断

heap helper header：

- 原 guard：使用 `#pragma once`，无传统 macro guard。
- 新 guard：`KVS_VECTOR_HNSW_V2_HEAP_H`。
- include 依赖：`vector/kvs_vector_hnsw_v2.h`、`stddef.h`、`stdint.h`。
- 内容：candidate heap / bounded top heap 的 `static inline` helper。
- 只被 `core_hnsw_v2` 内部使用，不访问 manager-owned semantic state。

score cache helper header：

- 原 guard：使用 `#pragma once`，无传统 macro guard。
- 新 guard：`KVS_VECTOR_HNSW_V2_SCORE_CACHE_H`。
- include 依赖：`vector/kvs_vector_hnsw_v2.h`、`stddef.h`、`stdint.h`。
- 内容：reverse_prune local score cache 的 `static inline` helper。
- 只被 `core_hnsw_v2` 内部使用，不访问 manager-owned semantic state。

## 迁移后头文件清单

- `include/vector/kvs_vector_hnsw_v2_heap.h`
- `include/vector/kvs_vector_hnsw_v2_score_cache.h`
- `include/vector/kvs_vector_hnsw_v2.h` 保持不变

两个迁移后的 helper 文件顶部均补充说明：

```text
Internal helper for core_hnsw_v2; stored under include/vector only to keep headers out of src/.
```

这表示它们只是为了源码目录结构一致而放在 `include/vector/`，仍不是 KVManager/user-facing public API。

## include 引用更新

源文件改为使用：

```c
#include "vector/kvs_vector_hnsw_v2_heap.h"
#include "vector/kvs_vector_hnsw_v2_score_cache.h"
```

更新点：

- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c`
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c`
- `Makefile`

旧头文件名和 hnsw_v2 source-directory header 路径引用已清空。

## 行为边界

未修改：

- hnsw_v2 graph/search/add/neighbor_select 算法逻辑
- 默认 M/M0/efConstruction/efSearch 参数
- RDB/AOF
- VSEARCH response
- `kvs_vector_index_ops_t`
- core_flat implementation
- FAISS/hnswlib optional baseline
- manifest / snapshot / background rebuild

本阶段 production code changed = yes，因为 `.c` include 路径和 Makefile 依赖发生变化；但 graph/search algorithm touched = no。

## 结构检查

```bash
find src/vector/core/hnsw_v2 -maxdepth 1 -name '*.h' -type f | sort
```

结果：无输出。

```bash
test -f include/vector/kvs_vector_hnsw_v2_heap.h && echo "moved heap header"
test -f include/vector/kvs_vector_hnsw_v2_score_cache.h && echo "moved score cache header"
test -f include/vector/kvs_vector_hnsw_v2.h && echo "public hnsw_v2 header still exists"
```

结果：

```text
moved heap header
moved score cache header
public hnsw_v2 header still exists
```

## 引用检查

```bash
old hnsw_v2 helper header reference grep
```

结果：无输出。

```bash
rg -n "kvs_vector_hnsw_v2_heap\.h|kvs_vector_hnsw_v2_score_cache\.h" include src tests Makefile docs || true
```

结果：仅命中新路径 include、Makefile 依赖和相关文档说明，引用合理。

## boundary grep

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2*.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2*.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2*.h || true
```

结果：均无输出。未发现 `core_hnsw_v2` 或迁移后的 helper header 访问 manager-owned semantic state。

## 测试结果

```bash
make -j"$(nproc)"
```

结果：PASS。

因为本阶段修改了 Makefile header dependency 和 include 路径，额外执行：

```bash
make clean && make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：PASS。

```bash
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_flat_correctness_test.sh
```

结果：全部 PASS。

关键 recall smoke：

```text
1K/512d ef=256 recall@10 = 1.0000
5K/512d ef=256 recall@10 = 1.0000
10K/512d ef=64 recall@10 = 0.7780
10K/512d ef=128 recall@10 = 0.9580
10K/512d ef=256 recall@10 = 0.9980
10K/512d ef=512 recall@10 = 1.0000
10K/512d ef=1024 recall@10 = 1.0000
```

## git diff --check

```bash
git diff --check
```

结果：PASS。

## git status --short

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
 M docs/vector_core_reset/stage9e1a_core_hnsw_v2_candidate_quality_attribution.md
 M docs/vector_core_reset/stage9e2_core_hnsw_v2_bounded_heap_optimization.md
 M docs/vector_core_reset/stage9e3c_core_hnsw_v2_reverse_prune_score_reuse.md
 M docs/vector_core_reset/stage9g2_default_backend_switch_hardening.md
 M docs/vector_core_reset/stage9g3_scale_stability_final_consolidation.md
 M include/kvs_config.h
 M include/kvs_vector.h
 M include/kvs_vector_hnswlib_backend.h
 D include/vector/kvs_vector_hnsw.h
RM hnsw_v2 heap helper header -> include/vector/kvs_vector_hnsw_v2_heap.h
RM hnsw_v2 score cache helper header -> include/vector/kvs_vector_hnsw_v2_score_cache.h
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
 M src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c
 M src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c
 M src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_faiss_ops.c
 M src/vector/kvs_vector_hnswlib_ops.c
 M src/vector/kvs_vector_index.c
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
