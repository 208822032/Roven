# HNSW-P0: 100K 召回归因审阅报告

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

本阶段只做 HNSW 产品化路线下的审阅、归因和下一步执行方向设计，未修改生产代码，未修改测试，未修改 Makefile，未修改 VSEARCH response，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未切换默认 backend，未让 v2 core 访问 manager-owned state。

未发现需要停止执行的冲突。`AGENTS.md` 的历史 stage report 约束默认指向 `docs/vector_core_reset/`，但本任务明确开启非 Stage 9G4/9H0 的 `HNSW-P*` 产品化路线，并指定报告落在 `docs/hnsw_productization/`。本报告按用户指定的新路线写入该目录，同时继续遵守 `AGENTS.md` 的架构和边界约束。

## 审阅范围

本次审阅覆盖：

- Stage 9G3 10K/50K/100K scale gate 数据。
- `docs/hnsw_productization/` 路线文档。
- `core_hnsw_v2` 最终架构、默认 backend 和面试复盘文档。
- v2 scale / recall 测试实现。
- v2 默认参数、level generation、neighbor_select、reverse_prune、entry point / add-time layer search 相关代码。
- 既有 FAISS / hnswlib parity 文档和测试接线。

本阶段为审阅复盘，未重新跑完整 50K/100K scale test，复用 Stage 9G3 数据。

## 是否修改代码

未修改生产代码。

未修改：

- `src/`
- `include/`
- `tests/`
- `Makefile`
- VSEARCH response
- RDB/AOF
- `kvs_vector_index_ops_t`

本阶段新增/更新的文件仅限 `docs/hnsw_productization/`。

## Stage 9G3 关键数据

| count | dim | mode | queries | ef_search | recall@10 | build | duplicate/overflow |
|---:|---:|---|---:|---:|---:|---:|---:|
| 10K | 512 | full | 50 | 256 | 0.9860 | ~= 61s | 0 / 0 |
| 10K | 512 | full | 50 | 512 | 1.0000 | ~= 61s | 0 / 0 |
| 50K | 512 | sampled | 20 | 256 | 0.6650 | ~= 465s | 0 / 0 |
| 50K | 512 | sampled | 20 | 512 | 0.8600 | ~= 465s | 0 / 0 |
| 100K | 512 | smoke | 5 | 256 | 0.5400 | ~= 1034s | 0 / 0 |
| 100K | 512 | smoke | 5 | 512 | 0.7200 | ~= 1034s | 0 / 0 |

当前结论：

- 10K gate 通过。
- 50K/100K recall 未达到生产要求。
- duplicate/overflow 为 0 只说明图结构合法，不说明图导航质量足够。
- 当前不能宣称 `core_hnsw_v2` 已具备 100K 生产可用召回。

## Benchmark 可信度判断

结论：

```text
benchmark可信度: medium
是否需要补测: yes
```

可信的部分：

- 数据生成使用固定 seed，结果具备可复现性。
- scale test 使用单位向量和 IP metric，exact 侧同样按 IP score 降序计算 topK。
- 10K gate 使用 50 queries，且 recall gate 在独立 search recall test 中持续通过。
- ef_search sensitivity 单调不下降，说明测试能捕捉搜索探索强弱变化。
- duplicate/overflow 检查能证明基本图结构合法。

置信度不足的部分：

- 50K 只有 20 queries，足以作为趋势和风险证据，不足以作为 SLA。
- 100K 只有 5 queries，只能作为 smoke risk signal。
- exact oracle 是测试内 direct exact scan，不是通过统一的 `core_flat` shared harness 输出；这本身不是错误，但 P1 需要统一 oracle 和 dataset。
- 当前 scale test 只覆盖 IP / normalized random vectors，不能代表全部实际 embedding 分布。
- 还没有同一 dataset / query / metric / topK 下的 `core_hnsw_v2`、FAISS、hnswlib、USearch parity 对照。

补测建议：

```text
query_count:
  50K: 200-500
  100K: 200-500

seed:
  至少 3 组 dataset seed + 3 组 query seed

exact_oracle:
  core_flat shared-dataset exact oracle + direct exact scan cross-check

metric:
  IP normalized vectors 为主；如产品使用 cosine/L2，也必须补对应 metric

output_fields:
  recall@1/10
  build_us
  search_avg/p50/p95/p99
  memory_bytes
  visited_avg/p95
  distance_avg/p95
  level_distribution
  level0/upper degree distribution
  duplicate/overflow/isolated
  selected/pruned/reverse_prune counters
```

## 参数风险判断

结论：

```text
parameter risk: high
最可疑参数: ef_search / ef_construction / M-M0 / unknown graph quality interaction
```

当前 v2 index-only 默认参数：

| parameter | value |
|---|---:|
| M | 32 |
| M0 | 64 |
| ef_construction | 256 |
| ef_search | 256 |
| random level threshold | 1 / M |

判断：

- 10K 下 ef=256 已可达 recall@10 0.9860，ef=512 达 1.0000。
- 50K/100K 下 ef 从 128 到 512 单调提升，但仍明显不足，说明 `ef_search=256/512` 不是 100K 生产级默认的充分设置。
- 50K/100K level0 degree avg 已满 64，degree 容量看起来用满，但 recall 仍低，说明“边数量足够”不等于“边质量足够”。
- `ef_construction=256` 对 10K 有效，但对 50K/100K 可能不足，需要和 M/M0 一起补测。
- 产品配置路径仍保留 legacy `vector_hnsw_ef_construction=128` 配置值；v2 配置函数会按 v2 最小默认提升到 256，但 P2 仍需要把用户可见参数 profile 和实际 v2 参数语义写清楚。

候选参数 profile，只作为 P1/P2 补测建议，不改代码：

| profile | M | M0 | ef_construction | ef_search | 目标 |
|---|---:|---:|---:|---:|---|
| low_latency | 32 | 64 | 256 | 256-512 | 延迟优先，适合 <=10K 或可接受较低召回的场景 |
| balanced | 48 | 96 | 512 | 768-1024 | 50K/100K 默认候选，验证能否接近生产召回 |
| high_recall | 64 | 128 | 1024 | 1024-2048 | 召回优先，用于 100K production recall gate 前置验证 |

## 图质量风险判断

结论：

```text
graph quality risk: high
primary suspected cause:
  search_ef_insufficient
  ef_construction_or_M_M0_insufficient
  neighbor_select_diversity_or_reverse_prune_quality_unknown
```

依据：

- 50K/100K ef sensitivity 单调上升，说明更大搜索探索能改善 recall。
- 但 ef=512 仍只有 50K recall@10 0.8600、100K recall@10 0.7200，不能只归因为查询时 ef_search 太小。
- level0 degree avg=64 且 duplicate/overflow=0，说明结构合法且度数饱和，但导航质量不足。
- `neighbor_select` 有多样性启发式，但当前 P0 尚未证明它在 50K/100K 上和 FAISS / hnswlib / USearch 等价。
- `reverse_prune` 是 best-effort 反向边维护，已有 score reuse 优化，但 P0 没有证明它不会在大规模随机插入下剪掉关键跨簇边。
- random level 使用 `1/M` threshold，理论上合理，但需要在 50K/100K 输出完整 level distribution 和 entry point drift 指标。
- 当前 100K queries=5，不能排除 sampling 对 graph-quality 判断的影响。

需要补的 graph-quality 指标：

- level distribution 与理论分布差异。
- entry point score ratio / drift。
- level0 bounded candidates 与 exact top-M0 overlap。
- neighbor_select selected/pruned 中 exact-neighbor 命中率。
- reverse_prune rejected/pruned 是否命中 exact-neighbor。
- upper-layer degree 分布和 isolated / weakly connected 情况。
- insertion-order sensitivity。
- tombstone ratio 对 recall 的影响曲线。

## Parity baseline readiness

结论：

```text
parity baseline readiness: partial
```

已有能力：

- `core_flat` 是 exact baseline / correctness oracle。
- 旧 Stage 8A 已有 FAISS index-only baseline 和 server-path backend 对比。
- Makefile 中已有 `faiss_baseline` target。
- 项目保留 FAISS / hnswlib adapters，可作为 baseline 来源。

缺口：

- 尚无统一的 50K/100K shared float dataset harness 同时跑 `core_flat`、`core_hnsw_v2`、FAISS HNSW、hnswlib HNSW。
- Stage 8A 的 parity 数据主要是旧 core_hnsw / 10K 口径，不等价于当前 v2 100K 归因。
- FAISS server adapter 曾出现过和 index-only 不一致的测量异常，不能直接拿 server-path 数字当成熟库 kernel baseline。
- USearch 暂时没有仓库内 baseline，应先作为外部设计参考，除非 P1 明确引入独立 harness。

推荐 parity test：

```text
dataset_count:
  10K, 50K, 100K

dim:
  512

query_count:
  200-500

M:
  32, 48, 64

efConstruction:
  256, 512, 1024

efSearch:
  256, 512, 768, 1024, 2048

baselines:
  core_flat exact oracle
  core_hnsw_v2
  FAISS HNSW
  hnswlib HNSW
  USearch as external reference only unless harness is explicitly added
```

## 为什么仿照 FAISS / USearch 仍可能失败

HNSW 框架容易仿，生产级图质量难仿。

即使实现包含 entry point、upper greedy、level0 ef search、neighbor_select、bidirectional link 和 reverse prune，仍可能在大规模上失败，原因包括：

- 参数语义不是名义值等价。相同 M/ef 在不同实现中可能对应不同 candidate selection 和 prune 行为。
- neighbor_select 的多样性细节会影响跨簇导航。
- reverse_prune 过度保守或过度激进都会伤害长程边。
- entry point / level distribution 的小偏差会在 100K 放大。
- insertion order 对自建图质量可能更敏感。
- exact benchmark 和 ANN benchmark 若 dataset / metric / normalization 不完全对齐，会误导判断。
- 10K 通过不能外推到 100K。

## 推荐补测矩阵

优先级按 P1 执行：

| priority | count | dim | queries | backends | parameters |
|---:|---:|---:|---:|---|---|
| 1 | 10K | 512 | 200 | core_flat, core_hnsw_v2, FAISS, hnswlib | M=32, efC=256, ef=256/512 |
| 2 | 50K | 512 | 200 | core_flat, core_hnsw_v2, FAISS, hnswlib | M=32, efC=256, ef=256/512/1024 |
| 3 | 100K | 512 | 200 | core_flat, core_hnsw_v2, FAISS, hnswlib | M=32, efC=256, ef=256/512/1024 |
| 4 | 100K | 512 | 200 | core_hnsw_v2, FAISS, hnswlib | M=48/64, efC=512/1024, ef=512/1024/2048 |

如果 100K exact oracle 成本过高，先预生成 exact topK 文件并复用，不要降低 oracle 严格性。

## 推荐下一步 HNSW-P 路线

推荐路线：

```text
1. HNSW-P1: parity baseline 与参数敏感性补测
2. HNSW-P2: 参数硬化 profile
3. HNSW-P3: 只修被 P1/P2 证明的问题
4. HNSW-P4: 100K production recall gate
5. HNSW-P5: rebuild / cold-start 产品化策略
```

当前不建议直接进入 P3，因为还不能区分参数不足、成熟库 parity 差距、benchmark sampling 不足和 graph construction 质量问题。

判断规则：

- 如果 P1 证明成熟库同参数也低 recall，优先 P2 参数 profile。
- 如果成熟库同参数显著高于 v2，进入 P3 graph construction parity audit。
- 如果 v2 通过高参数达到 recall 目标但 build/cold-start 不可接受，进入 P5。
- 如果 100K 补测仍只有 smoke 级 query 数，不得进入 P4 SLA gate。

## 面试可讲的工程决策链

可以这样复盘：

1. 先拆 manager / vector_core 边界，确保 ANN core 只处理 `record_id + vector -> record_id + score`。
2. 用 `core_flat` 建 exact oracle，避免把 ANN 召回问题和 manager resolve/filter 混在一起。
3. 当时保留 legacy `core_hnsw` 作为显式 fallback 和 regression reference；该结论已被 CODE-HYGIENE-0 覆盖，现在源码已删除，显式 `core_hnsw` 配置解析为 `core_flat` fallback。
4. 重做 `core_hnsw_v2`，修复旧实现里新节点 forward edge 依赖 reverse acceptance 的图构建语义问题。
5. 用 10K gate 验证默认 ANN backend 小规模可用。
6. 用 50K/100K scale gate 暴露召回和同步 rebuild 风险。
7. 明确 duplicate/overflow=0 只是结构合法，不代表 graph useful。
8. 发现 100K gap 后没有盲目做 SIMD/layout/cache，而是切换到 HNSW 产品化路线：先做 P0/P1 归因和 parity，再决定参数硬化或图构建修复。

## 当前不建议继续做的方向

在 P1/P2 之前，不建议：

- SIMD/layout 微优化。
- reverse_prune cache 继续扩展。
- 重新引入或继续依赖已删除 legacy `core_hnsw`。
- RDB/AOF graph persistence。
- VSEARCH response 改造。
- background rebuild thread。
- 无归因的 1M sprint。
- 直接把 50K/100K 当前结果包装成生产 SLA。

## 本阶段验证结果

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

v2 tests：

```text
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
```

本次 search recall 关键输出：

| count | dim | queries | ef_search | recall@10 | search_avg_us |
|---:|---:|---:|---:|---:|---:|
| 1K | 512 | 50 | 256 | 1.0000 | 210 |
| 5K | 512 | 50 | 256 | 1.0000 | 917 |
| 10K | 512 | 50 | 64 | 0.6800 | 837 |
| 10K | 512 | 50 | 128 | 0.8840 | 1281 |
| 10K | 512 | 50 | 256 | 0.9860 | 1859 |
| 10K | 512 | 50 | 512 | 1.0000 | 2462 |
| 10K | 512 | 50 | 1024 | 1.0000 | 3143 |

50K/100K scale test 未在本阶段重跑，原因是本阶段定位为审阅复盘，任务允许复用 Stage 9G3 数据。

## 静态边界检查结果

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output
```

`git diff --check`：

```text
PASS
```

## git status --short

```text
 M Makefile
 M "docs/vector_core_reset/\344\274\230\345\214\226\346\255\245\351\252\244.md"
 M "docs/vector_core_reset/\346\211\247\350\241\214\346\255\245\351\252\244\344\271\246.md"
 M include/kvs_config.h
 M include/kvs_vector.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_config.c
 M src/core/kvs_operation.c
 M src/vector/kvs_vector.c
 M tests/README.md
 D tests/vcmd_profile_bench.c
 D tests/vector_core_hnsw_rebuild_profile_test.sh
?? docs/hnsw_productization/
?? docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md
?? docs/vector_core_reset/core_hnsw_v2_final_architecture.md
?? docs/vector_core_reset/core_hnsw_v2_interview_review.md
?? docs/vector_core_reset/stage9g0_experimental_core_hnsw_v2_product_integration.md
?? docs/vector_core_reset/stage9g1_cleanup_codebase_consolidation.md
?? docs/vector_core_reset/stage9g2_default_backend_switch_hardening.md
?? docs/vector_core_reset/stage9g3_scale_stability_final_consolidation.md
?? tests/vector_core_hnsw_v2_default_backend_test.c
?? tests/vector_core_hnsw_v2_default_backend_test.sh
?? tests/vector_core_hnsw_v2_product_integration_test.c
?? tests/vector_core_hnsw_v2_product_integration_test.sh
?? tests/vector_core_hnsw_v2_scale_stability_test.c
?? tests/vector_core_hnsw_v2_scale_stability_test.sh
?? tests/vector_core_hnsw_v2_tombstone_policy_test.c
?? tests/vector_core_hnsw_v2_tombstone_policy_test.sh
```

说明：除 `docs/hnsw_productization/` 内新增/更新文档外，其余 dirty worktree 项为既有改动，本阶段未处理、未回退。

## 本阶段新增/更新文件

新增：

```text
docs/hnsw_productization/hnsw_p0_recall_attribution_report.md
```

更新：

```text
docs/hnsw_productization/index.md
```

## 最终判断

`accepted_ba` 类旧问题已经被 v2 设计修正，当前 100K gap 更像是参数覆盖和图质量归因问题，而不是 manager resolve、VSEARCH response、RDB/AOF、距离函数或结构合法性问题。

P0 判断：

```text
当前不能宣称 100K 生产可用。
benchmark 可作为风险证据，但 50K/100K query 数不足以支撑 SLA。
最可能原因是参数不足和 graph construction 质量尚未与成熟库 parity。
下一步应执行 HNSW-P1，而不是直接改代码。
```
