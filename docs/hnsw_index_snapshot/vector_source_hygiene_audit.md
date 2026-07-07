# CODE-HYGIENE-1: src/vector source-by-source audit and profiling cleanup

## AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`。本阶段与 AGENTS 的稳定边界无冲突。

本阶段只做 `src/vector` source-by-source 审计、profiling 归类和低风险说明/注释 cleanup；未进入 HNSW-S1，未实现 snapshot、manifest、background rebuild，也未修改 RDB/AOF、VSEARCH response、`kvs_vector_index_ops_t` 签名或 `core_hnsw_v2` graph/search 算法。

## 本阶段目标

- 逐个阅读并审计 `src/vector` 及相关 include 文件，而不是只做 `rg` 扫描。
- 建立 source inventory，标记 product-main、anncore-v2、fallback-flat、optional backend、rebuild skeleton、utility、compatibility、test-support、profiling-heavy。
- 审计 profiling 是否过度侵入主路径。
- 只执行低风险 cleanup：删除过时注释、收口 optional hnswlib 说明、删除已无意义的注释块。
- 保持默认 ANN backend 为 `core_hnsw_v2`。
- 保持 explicit `core_hnsw` 兼容行为为 `fallback_to_core_flat`。

## 路径说明

任务描述中的部分路径与当前仓库实际路径不同：

- `src/vector/kvs_vector_heap.c` 实际为 `src/vector/core/kvs_vector_heap.c`。
- `src/vector/kvs_vector_id_slot_map.c` 实际为 `src/vector/core/kvs_vector_id_slot_map.c`。
- `include/vector/kvs_vector_distance.h` 实际为 `include/kvs_vector_distance.h`。

这些实际文件均已纳入本轮审计。

## 是否修改生产代码

生产代码有改动，但为低风险 cleanup / 注释说明 / legacy 兼容面收口，不改变对外协议、默认 backend、search correctness、RDB/AOF 或 `kvs_vector_index_ops_t`。

本轮明确相关改动：

- `include/kvs_vector_hnswlib_backend.h`：补充 hnswlib optional baseline 说明。
- `src/vector/kvs_vector_hnswlib_ops.c`：补充 hnswlib optional baseline 说明。
- `src/vector/kvs_vector_index.c`：清理重复/过时注释和已注释掉的 obsolete placeholder。
- `src/vector/kvs_vector.c`：清理已注释掉的 obsolete helper / debug 注释块；保留 explicit `core_hnsw -> core_flat` 兼容行为说明。

注意：当前 `git diff` 中还包含上一阶段 CODE-HYGIENE-0 的 legacy `core_hnsw` 删除和兼容修改，本报告不把这些历史删除都归为 CODE-HYGIENE-1 新增 cleanup。

## Source Inventory

| file | lines | current role | category | depends on manager semantic state? | contains profiling code? | cleanup action | risk | notes |
|---|---:|---|---|---|---|---|---|---|
| `src/vector/kvs_vector.c` | 4450 | manager 主入口，VSET/VSEARCH/VDEL/VINDEX/VINFO，backend dispatch，core_flat/core_hnsw_v2 adapter | product-main, compatibility, profiling-heavy | yes | heavy | comment cleanup; split later | high | 大文件，包含 VSEARCH/VINFO/VSET profiling 和 backend glue；默认 backend 仍为 `core_hnsw_v2` |
| `src/vector/kvs_vector_index.c` | 4322 | 旧 index abstraction / compatibility path，保留 legacy flat/SQ8/ANN-lite 结构 | compatibility | yes | light | comment cleanup; defer | high | 仍偏大，适合后续独立兼容层收口，不在本阶段拆 |
| `src/vector/kvs_vector_embedding.c` | 1620 | embedding provider/cache/runtime 管理 | product-main, profiling-heavy | yes | heavy | defer; split later | medium | 与 vector index backend 解耦，不应混入 HNSW profile |
| `src/vector/kvs_vector_distance.c` | 224 | scalar distance utility | utility | no | none | keep | low | 纯距离计算，不访问 manager state |
| `src/vector/kvs_vector_distance_avx2.c` | 176 | AVX2 distance utility | utility | no | none | keep | low | 纯距离计算，不承载 profiling schema |
| `src/vector/core/kvs_vector_heap.c` | 210 | 通用 heap utility | utility | no | none | keep | low | 不绑定 backend profile |
| `src/vector/core/kvs_vector_id_slot_map.c` | 326 | core-local record_id -> slot map helper | utility | no | none | keep | medium | 这是 index-local slot map，不是 manager-owned `record_id_map` |
| `src/vector/kvs_vector_rebuild.c` | 505 | manager-owned rebuild skeleton/control plane | rebuild-skeleton | yes | light | defer | medium | backend-agnostic；未实现 snapshot/manifest |
| `src/vector/kvs_vector_rebuild_shadow.c` | 464 | shadow replay/swap harness skeleton | rebuild-skeleton, test-support | yes | light | defer | medium | 访问 manager-owned `g_vector_state` 是 manager harness 语义，不是 vector_core 越界 |
| `src/vector/kvs_vector_faiss_ops.c` | 194 | FAISS optional baseline ops wrapper | optional-faiss | yes | light | keep | low | adapter 会调用 manager resolve；按 AGENTS 归为 adapter debt，不作为 core 模板 |
| `src/vector/kvs_vector_faiss_backend.cpp` | 208 | FAISS C++ backend wrapper | optional-faiss | no | none | keep | low | 默认构建不强制链接；不属于产品默认 backend |
| `src/vector/kvs_vector_hnswlib_ops.c` | 226 | hnswlib optional baseline ops wrapper | optional-hnswlib | yes | light | comment cleanup | low | 已补充说明：不是 removed legacy `core_hnsw`，不是默认 backend |
| `src/vector/kvs_vector_hnswlib_backend.cpp` | 218 | hnswlib C++ backend wrapper | optional-hnswlib | no | none | keep | low | optional baseline，保留 parity/benchmark 用途 |
| `src/vector/core/flat/kvs_flat_index.c` | 227 | core_flat lifecycle/add/delete/reset | fallback-flat | no | none | keep | low | exact oracle / fallback；无 legacy hnsw 残留 |
| `src/vector/core/flat/kvs_flat_search.c` | 197 | core_flat exact search | fallback-flat | no | none | keep | low | 保留 correctness baseline |
| `src/vector/core/flat/kvs_flat_storage.c` | 163 | core_flat storage/reserve | fallback-flat | no | none | keep | low | 不访问 manager semantic state |
| `src/vector/core/flat/kvs_flat_info.c` | 5 | core_flat info shim | fallback-flat | no | none | keep | low | 极小文件，无 cleanup |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c` | 1572 | v2 add/build path，layer search，neighbor select 调用，reverse link/prune | anncore-v2, profiling-heavy | no | heavy | defer | high | graph/search implementation 禁止本阶段修改 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c` | 572 | v2 query search | anncore-v2, profiling-heavy | no | heavy | defer | high | 保留 search stats；未改算法 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c` | 495 | v2 lifecycle/add/search/delete/reset public core API | anncore-v2 | no | light | keep | medium | core-local state only |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_graph.c` | 318 | v2 graph helpers/stats refresh | anncore-v2 | no | light | keep | medium | graph stats 属 core-local profiling |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c` | 211 | v2 neighbor selection helper | anncore-v2 | no | light | keep | medium | 不在本阶段调 neighbor_select |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_context.c` | 58 | v2 search context buffers | anncore-v2 | no | light | keep | low | 不做 context/layout 优化 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_info.c` | 79 | v2 info export | anncore-v2 | no | light | keep | low | core-local stats export |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_random.c` | 81 | v2 random level helper | anncore-v2 | no | none | keep | low | deterministic seed path 保留 |
| `src/vector/core/hnsw_v2/kvs_hnsw_v2_storage.c` | 7 | v2 storage placeholder/shim | anncore-v2 | no | none | keep | low | 未改 |
| `include/vector/kvs_vector_hnsw_v2_heap.h` | 248 | v2-only heap helper | anncore-v2 | no | light | keep | medium | Stage 9E2 产物；CODE-HYGIENE-2 迁移到 `include/vector/`，仍是 internal helper |
| `include/vector/kvs_vector_hnsw_v2_score_cache.h` | 163 | v2 score cache helper | anncore-v2 | no | light | keep | medium | Stage 9E3c 产物；CODE-HYGIENE-2 迁移到 `include/vector/`，仍是 internal helper |
| `include/kvs_vector.h` | 689 | public vector API/info/stats declarations | product-main, compatibility, profiling-heavy | yes | heavy | split later | high | VINFO/VSEARCH/VSET/embedding stats 集中，后续可拆 stats header |
| `include/kvs_vector_distance.h` | 91 | distance API | utility | no | none | keep | low | 实际距离头文件路径 |
| `include/kvs_vector_hnswlib_backend.h` | 78 | hnswlib optional baseline C ABI | optional-hnswlib | no | light | comment cleanup | low | 已补充 optional baseline 边界说明 |
| `include/vector/kvs_vector_core.h` | 184 | vector_core shared types | utility | no | light | keep | low | 只承载 record_id/vector/score/core-local contracts |
| `include/vector/kvs_vector_flat.h` | 99 | core_flat API | fallback-flat | no | light | keep | low | exact baseline/fallback API |
| `include/vector/kvs_vector_heap.h` | 78 | generic heap API | utility | no | none | keep | low | 无 backend-specific profile |
| `include/vector/kvs_vector_id_slot_map.h` | 64 | core-local id-slot map API | utility | no | none | keep | medium | 与 manager `record_id_map` 名称接近，报告中标明边界 |
| `include/vector/kvs_vector_hnsw_v2.h` | 514 | v2 public core API + core-local build/search/profile stats | anncore-v2, profiling-heavy | no | heavy | defer | high | 不扩大 public API，不改 recall params |
| `include/vector/kvs_vector_index_internal.h` | 720 | internal index contracts / compatibility declarations | compatibility | yes | light | defer | high | 含 `kvs_vector_index_ops_t`，本阶段禁止签名变更 |
| `include/vector/kvs_vector_manager.h` | 329 | manager-owned vector state | product-main | yes | light | keep | high | record store、record_id_map、backend instances 属 manager |
| `include/vector/kvs_vector_rebuild.h` | 116 | rebuild control plane API | rebuild-skeleton | yes | light | keep | medium | 不实现 background worker |
| `include/vector/kvs_vector_rebuild_shadow.h` | 38 | shadow rebuild harness API | rebuild-skeleton, test-support | yes | light | keep | medium | 不实现 snapshot/manifest |
| `include/vector/kvs_vector_record.h` | 531 | manager record layout / semantic fields | product-main | yes | light | keep | high | scope/TTL/metadata/question/answer 属 manager |

## Per-file Responsibility Analysis

`src/vector/kvs_vector.c` 是当前最大职责聚合点：负责 manager 主路径、backend dispatch、core_flat/core_hnsw_v2 adapter、optional backend glue、VINFO/VSEARCH/VSET profiling、初始化/销毁/rebuild 调用。它仍是后续拆分候选，但本阶段不做大拆。

`src/vector/kvs_vector_index.c` 是兼容层聚合点，包含旧 index abstraction、legacy flat/SQ8/ANN-lite 路径和部分测试/兼容 helper。当前没有已删除 legacy `src/vector/core/hnsw` 的 build dependency；后续如要收口，应单独开 compatibility cleanup。

`src/vector/kvs_vector_embedding.c` 负责 embedding runtime、provider、cache 和统计。它是 product-main，但不属于 ANN core；本阶段只标记为 large/profiling-heavy，不做拆分。

`src/vector/kvs_vector_distance*.c`、`src/vector/core/kvs_vector_heap.c`、`src/vector/core/kvs_vector_id_slot_map.c` 是 utility。它们不应承载 manager semantic state，也不应混入 backend profile。本轮未发现需要立即 cleanup 的死分支。

`src/vector/kvs_vector_rebuild*.c` 是 manager-owned rebuild / shadow harness。它们可以访问 manager-owned 状态，但不能被复制到 vector_core。当前未实现 snapshot/manifest/background worker，本阶段保持不动。

FAISS / hnswlib 文件是 optional baseline adapter。`*_ops.c` 会调用 manager resolve，属于 AGENTS 中允许隔离记录的 adapter debt；`*_backend.cpp` 只包装外部库对象。hnswlib 已补充说明，避免与已删除 legacy `core_hnsw` 混淆。

`src/vector/core/flat/*` 是 exact baseline / fallback。未发现 legacy hnsw 残留，未触碰实现。

`src/vector/core/hnsw_v2/*` 是默认 ANN backend 的核心实现。本阶段只审计并记录职责，未修改 graph/search/add/neighbor_select/layout/recall 参数。

## Large-file Candidates

超过 1000 行的文件：

- `src/vector/kvs_vector.c`：4450 行，product-main + compatibility + profiling-heavy。
- `src/vector/kvs_vector_index.c`：4322 行，compatibility 聚合。
- `src/vector/kvs_vector_embedding.c`：1620 行，embedding runtime/cache + profiling。
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`：1572 行，v2 build/add hot path。

接近 large-file 或 header 聚合候选：

- `include/vector/kvs_vector_index_internal.h`：720 行，compatibility / internal contract 聚合。
- `include/kvs_vector.h`：689 行，public API + profiling stats 聚合。
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c`：572 行，v2 search + stats。
- `include/vector/kvs_vector_hnsw_v2.h`：514 行，v2 API + build/search/profile stats。

本阶段没有拆分这些文件，因为拆分会影响 include dependency、build graph、public stats schema 或 hot path ownership，超出 CODE-HYGIENE-1 的低风险 cleanup 范围。

## Profiling-heavy Candidates

- `include/kvs_vector.h`：VINFO backend stats、VSEARCH breakdown、embedding runtime stats。
- `src/vector/kvs_vector.c`：VSEARCH/VINFO profiling capture 和 backend info 汇总。
- `src/vector/kvs_vector_embedding.c`：embedding runtime/cache/provider stats。
- `include/vector/kvs_vector_hnsw_v2.h`：v2 build/search/graph/diagnostic stats。
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c`：build/add-time profiling。
- `src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c`：query search profiling。
- `tests/vector_core_hnsw_v2_*profile*_test.*`：benchmark/test-only profile harness。

## Profiling 分类

Product observability，保留：

- VINFO backend stats：backend name/size/capacity/dim/metric/memory source/deleted/rebuild flags。
- VSEARCH breakdown：total/backend search/manager resolve/response assembly/raw/resolved/filtered counts。
- embedding runtime stats：provider/cache/runtime health。

Optional backend stats，保留但不作为新 core 模板：

- FAISS adapter add/search count、ntotal。
- hnswlib adapter ntotal/capacity/M/ef/add/search count。
- optional adapter 内 manager resolve 属兼容 adapter debt，已在报告中隔离说明。

Benchmark/test-only，保留在 tests：

- v2 performance profile / distance profile / reverse prune profile / scale stability harness。
- skiptable / TTL / vector store benchmark timers。

Legacy removed residue：

- 已删除 legacy `src/vector/core/hnsw` implementation/header/build refs。
- public enum / config string `core_hnsw` 保留为 compatibility residue，行为为 fallback 到 `core_flat`。
- `core_hnsw` 旧 VINFO 字段仍属于 public compatibility residue，不能在本阶段删除 schema。

Deferred profiling cleanup：

- `include/kvs_vector.h` 可后续拆出 stats-specific header，但会影响 API include 面。
- `src/vector/kvs_vector.c` 中 profile aggregation 可后续分离为 manager observability helper，但本阶段不动 schema。
- v2 build/search stats 仍留在 core-local header；若后续产品化稳定，可单独讨论 internal/public stats 边界。

## 已执行 Cleanup

- 在 `src/vector/kvs_vector_hnswlib_ops.c` 顶部补充说明：hnswlib 是 optional baseline，不是已删除的 legacy `core_hnsw`，不是默认 backend。
- 在 `include/kvs_vector_hnswlib_backend.h` 补充同样的 C ABI 边界说明。
- 清理 `src/vector/kvs_vector_index.c` 中重复/过时注释和已注释掉的 obsolete placeholder。
- 清理 `src/vector/kvs_vector.c` 中已注释掉的 obsolete `kvs_vector_info_reset` 和 noisy debug 注释块。

## Deferred Cleanup

- 不拆 `src/vector/kvs_vector.c`：需要单独设计 manager API/observability/helper 边界。
- 不拆 `src/vector/kvs_vector_index.c`：需要先冻结旧 compatibility path 是否继续保留。
- 不拆 `src/vector/kvs_vector_embedding.c`：embedding runtime/cache 是独立产品域，应另开阶段。
- 不改 `include/kvs_vector.h` profile schema：会影响 public info/stats 面。
- 不改 `include/vector/kvs_vector_index_internal.h` 中 `kvs_vector_index_ops_t`。
- 不改 v2 build/search profiling counters：属于 anncore-v2 core-local profiling，且 graph/search implementation 本阶段禁止修改。
- 不删除 FAISS/hnswlib optional baseline。
- 不删除 core_flat。

## 为什么没有做大规模拆分

CODE-HYGIENE-1 的目标是 source-by-source audit 和低风险 cleanup。大规模拆分会同时触及 build graph、public header、tests、manager/core ownership 和 profiling schema，容易把本阶段扩大成结构重构。当前更合理的处理是先把职责、风险和 profiling-heavy 文件明确记录，为 HNSW-S1 前建立干净边界。

## Backend / Fallback 状态

- 默认 backend：仍为 `core_hnsw_v2`。
- explicit `core_hnsw`：仍为 `fallback_to_core_flat`。
- `core_flat`：保留为 exact baseline / fallback。
- FAISS / hnswlib：保留为 optional baseline / parity / benchmark adapter。
- legacy `src/vector/core/hnsw` implementation/header/build refs：已删除，不在默认构建中使用。

## Legacy 残留审计

执行：

```bash
rg -n "src/vector/core/hnsw|core/hnsw|kvs_vector_hnsw\.h|kvs_vector_core_hnsw_|kvs_hnsw_|core_hnsw_ops|HNSW_CORE_SRCS|HNSW_CORE_OBJS" \
  include src tests Makefile \
  --glob '!src/vector/core/hnsw_v2/**' \
  --glob '!include/vector/kvs_vector_hnsw_v2.h' || true
```

结论：未发现已删除 legacy implementation/header/build object 真实引用。输出主要为保留的 `core_hnsw_v2` 主线路径、v2 测试、v2 rebuild shadow、compatibility enum/config 字符串和 parity 测试名称。

## hnsw_v2 Boundary Grep

执行：

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
```

结果：均无输出。未发现 `core_hnsw_v2` 访问 manager-owned semantic state。

## Build / Tests

执行：

```bash
make -j"$(nproc)"
```

结果：PASS。

首次组合运行中 `tests/vector_core_hnsw_v2_product_integration_test.sh` 曾在混合/陈旧构建状态下出现 `ERR vector index not initialized`，单独复跑曾出现一次 segfault。随后执行 clean build 后全部通过；该失败未能在 clean build 下复现，归类为 stale/mixed build artifact，不作为当前代码回归结论。

执行：

```bash
make clean && make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：PASS。

clean build 后执行：

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

关键 recall 输出：

```text
1K/512d  ef=256  recall@10 = 1.0000
5K/512d  ef=256  recall@10 = 1.0000
10K/512d ef=64   recall@10 = 0.7780
10K/512d ef=128  recall@10 = 0.9580
10K/512d ef=256  recall@10 = 0.9980
10K/512d ef=512  recall@10 = 1.0000
10K/512d ef=1024 recall@10 = 1.0000
```

## git diff --check

执行：

```bash
git diff --check
```

结果：PASS，无输出。

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
 M docs/vector_core_reset/stage9g2_default_backend_switch_hardening.md
 M docs/vector_core_reset/stage9g3_scale_stability_final_consolidation.md
 M include/kvs_config.h
 M include/kvs_vector.h
 M include/kvs_vector_hnswlib_backend.h
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

# CODE-HYGIENE-4B Superseding Note

CODE-HYGIENE-4B 已改变本报告中关于 optional external backend 的状态：

- FAISS external backend source/header/build/test entries 已删除。
- hnswlib external backend source/header/build/test entries 已删除。
- `faiss` / `faiss_flat` / `faiss_hnsw` / `hnsw` / `hnswlib` / `hnswlib_hnsw` 配置名只作为 compatibility fallback 保留，实际落到 `core_flat`。
- public enum 与 VINFO schema 中的 FAISS / hnswlib 字段保留是兼容面，运行时固定 disabled/0。
- 当前允许的 vector backend 只有 `core_flat` 和 `core_hnsw_v2`。

因此，本报告早前的 `faiss_hnswlib_optional_baseline_preserved = yes` 已被 CODE-HYGIENE-4B 取代，当前应读作：

```text
faiss_hnswlib_optional_baseline_preserved = no
external_faiss_removed = yes
external_hnswlib_removed = yes
```
