# HNSW-P1: parity baseline 与参数敏感性补测报告

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

本阶段只新增 index-only 测试 harness、测试脚本、Makefile 测试接线和 HNSW 产品化文档。未修改生产代码，未修改 VSEARCH response，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未切换默认 backend，未修改默认参数，未让 v2 core 访问 manager-owned state。

未发现需要停止执行的冲突。`AGENTS.md` 的历史 stage report 默认要求写入 `docs/vector_core_reset/`；本任务明确属于 `docs/hnsw_productization/` 的 `HNSW-P*` 产品化路线，因此报告写入本目录，同时继续遵守 manager/vector_core 边界和验证要求。

## 修改/新增文件

新增：

- `tests/vector_core_hnsw_v2_parity_baseline_test.c`
- `tests/vector_core_hnsw_v2_parity_baseline_test.sh`
- `docs/hnsw_productization/hnsw_p1_parity_baseline_report.md`

修改：

- `Makefile`
- `docs/hnsw_productization/README.md`
- `docs/hnsw_productization/index.md`

## 是否修改生产代码

未修改生产代码。

未修改：

- `src/`
- `include/`
- VSEARCH response
- RDB/AOF
- `kvs_vector_index_ops_t`
- 默认 backend
- 默认 HNSW 参数

`Makefile` 仅新增 P1 测试二进制接线。

## Harness 设计

新增测试：

```bash
tests/vector_core_hnsw_v2_parity_baseline_test.sh
```

默认模式：

```bash
HNSW_P1_MODE=quick
```

支持：

```bash
HNSW_P1_MODE=smoke
HNSW_P1_MODE=quick
HNSW_P1_MODE=full
HNSW_P1_EXTERNAL=1
```

默认 `quick` 覆盖：

- 10K / 512d / 100 queries / baseline profile
- 50K / 512d / 200 queries / baseline profile
- 50K / 512d / 200 queries / balanced profile

`full` 预留 100K smoke-plus 和 50K high_recall，但本次未运行。原因是 50K balanced 单次 build 已约 1344s，full 矩阵不适合作为默认必跑测试。

## Dataset / query / oracle

固定设置：

| item | value |
|---|---|
| metric | IP |
| normalization | base/query 均为 unit vector |
| dim | 512 |
| topK | 10 |
| seed | fixed deterministic seed |
| record_id | `1000000 + i` |

Oracle：

- direct exact scan 计算 topK。
- `core_flat` exact search 对同一 dataset/query 交叉验证。
- 输出 `exact oracle mismatch`。

本次结果：

| count | queries | direct exact | core_flat cross-check | mismatch | oracle_us |
|---:|---:|---|---|---:|---:|
| 10K | 100 | yes | yes | 0 | 784,664 |
| 50K | 200 | yes | yes | 0 | 7,815,406 |

Oracle 可信度：10K/50K 本次为 high。100K 本次未跑，100K 仍为 low/unknown，不能写成 SLA。

## Baseline 参数结果

Baseline:

```text
M=32
M0=64
efConstruction=256
efSearch=256,512,1024
```

| count | queries | efSearch | recall@10 | search_avg_us | search_p50_us | search_p95_us | visited_avg | distance_avg |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10K | 100 | 256 | 0.9800 | 1,277.05 | 1,265 | 1,405 | 7,897.14 | 7,986.75 |
| 10K | 100 | 512 | 0.9990 | 1,857.31 | 1,846 | 1,986 | 9,517.79 | 9,607.40 |
| 10K | 100 | 1024 | 1.0000 | 2,665.92 | 2,652 | 2,846 | 9,973.68 | 10,063.29 |
| 50K | 200 | 256 | 0.6620 | 3,471.76 | 3,462 | 3,599 | 13,761.03 | 13,892.03 |
| 50K | 200 | 512 | 0.8690 | 6,088.53 | 6,083 | 6,196 | 23,196.15 | 23,327.15 |
| 50K | 200 | 1024 | 0.9775 | 9,773.25 | 9,762 | 9,937 | 35,055.01 | 35,186.01 |

Build / memory / graph:

| count | build_us | memory_bytes | max_level | level0_degree_avg | upper_degree_avg | duplicate | overflow | isolated |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10K | 63,736,339 | 64,104,384 | 2 | 64.00 | 31.58 | 0 | 0 | 0 |
| 50K | 487,794,820 | 319,997,248 | 3 | 64.00 | 31.98 | 0 | 0 | 0 |

结论：

- 50K 低召回在 q=200 下复现。
- efSearch 从 512 提升到 1024 后，50K recall@10 从 0.8690 提升到 0.9775。
- 参数不足是明确因素。
- duplicate/overflow/isolated 均为 0，只说明结构合法，不说明图质量已经和成熟库 parity。

## Balanced 参数结果

Balanced:

```text
M=48
M0=96
efConstruction=512
efSearch=512,1024
```

| count | queries | efSearch | recall@10 | search_avg_us | search_p50_us | search_p95_us | visited_avg | distance_avg |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 50K | 200 | 512 | 0.9575 | 7,826.31 | 7,819 | 7,940 | 30,200.39 | 30,344.21 |
| 50K | 200 | 1024 | 0.9975 | 11,512.98 | 11,499 | 11,660 | 41,624.05 | 41,767.86 |

Build / memory / graph:

| count | build_us | memory_bytes | max_level | level0_degree_avg | upper_degree_avg | duplicate | overflow | isolated |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 50K | 1,344,046,475 | 425,597,248 | 3 | 96.00 | 47.46 | 0 | 0 | 0 |

结论：

- 提高 M/M0 + efConstruction 能显著恢复 50K recall。
- 50K balanced ef=1024 recall@10 达 0.9975。
- 代价也非常明显：build_us 约 1344s，约为 baseline 50K build 的 2.76x；memory 从约 320MB 增加到约 426MB。
- 这更像 high-cost balanced/high_recall 候选，不能直接变成默认参数。

## High-recall 参数结果

High-recall:

```text
M=64
M0=128
efConstruction=1024
efSearch=1024,2048
```

本次未运行。

原因：

- 50K balanced 单次 build 已约 1344s。
- high_recall 预计更重。
- 本阶段默认 quick 不应变成不可控长跑。

后续如需要验证 high_recall，应显式运行 `HNSW_P1_MODE=full` 或新增单独 long-run job。

## FAISS / hnswlib parity

当前状态：

```text
parity_baseline: partial
```

FAISS：

- 编译时 `ENABLE_FAISS=1` 可用。
- 现有 FAISS C ABI wrapper 缺少 efSearch setter。
- 如果直接使用现有 wrapper，无法保证同一 `efSearch=512/1024` 口径，输出会误导。
- 因此本次没有输出 FAISS HNSW recall 数字。

hnswlib：

- 编译时 `ENABLE_HNSWLIB=1` 可用。
- Harness 支持 `HNSW_P1_EXTERNAL=1` 进入 hnswlib 分支。
- 一次 50K hnswlib baseline 对照运行在 build 阶段耗时明显超出 default quick 预算后被中断，未采纳为结果。
- 默认 quick 改为 `HNSW_P1_EXTERNAL` 显式 opt-in，避免必跑命令不可控。

下一步 mature parity 必须单独完成：

- 新增 test-only FAISS direct C++ harness，直接设置 `IndexHNSWFlat::hnsw.efSearch`。
- 或扩展 benchmark-only FAISS C ABI 的 efSearch setter，但仍不得进入产品路径。
- hnswlib parity 应拆成单独 long-run job，并记录 build_us。

由于 mature library parity 未完成，当前不能判断 v2 是否存在同参数 graph-quality gap。

## Graph-quality 补充指标

本次已输出可得项：

| count | profile | max_level | entry_point_level | level0 avg/min/p95/max | upper avg/min/p95/max | selected | reverse_prune | duplicate | overflow | isolated |
|---:|---|---:|---:|---|---|---:|---:|---:|---:|---:|
| 10K | baseline | 2 | 2 | 64.00 / 64 / 64 / 64 | 31.58 / 3 / 32 / 32 | 646,166 | 643,552 | 0 | 0 | 0 |
| 50K | baseline | 3 | 3 | 64.00 / 64 / 64 / 64 | 31.98 / 0 / 32 / 32 | 3,245,824 | 3,242,688 | 0 | 0 | 0 |
| 50K | balanced | 3 | 3 | 96.00 / 96 / 96 / 96 | 47.46 / 0 / 48 / 48 | 4,841,443 | 4,835,520 | 0 | 0 | 0 |

未输出或仍需 P3/P1-long-run 补充：

- mature library 同口径 level/degree 对比。
- candidate_recall@M0。
- reverse_prune_removed_exact_best。
- insertion-order sensitivity。
- 100K level distribution / entry point drift。

## 关键判断

50K/100K recall gap 是否复现：

- 50K gap 复现。q=200 下 baseline ef=256/512 分别为 0.6620/0.8690，与 Stage 9G3 的 0.6650/0.8600 一致。
- 100K 本次未复测，仍不能作为 SLA 判断。

efSearch=1024 是否能恢复 recall：

- 50K baseline ef=1024 达 0.9775，明显恢复，但未达到 10K 的 1.0000。
- 50K balanced ef=1024 达 0.9975。

M/M0 + efConstruction 是否能恢复 recall：

- 在 50K 上是 yes。
- 代价是 build_us 和 memory 明显上升。

v2 与成熟库同参数差距：

- unknown。
- mature_library_parity 仍是 partial。

## 不建议继续做的方向

当前不建议：

- 直接做 SIMD/layout/reverse_prune cache 微优化。
- 直接改默认参数。
- 直接宣称 50K/100K 生产 SLA。
- 直接进入 graph construction refactor。
- 删除 legacy `core_hnsw`。
- 修改 VSEARCH response。
- 修改 RDB/AOF graph persistence。

## 面试可讲结论

可以这样讲：

- 先用 `core_flat` 和 direct exact scan 交叉验证 oracle，避免 ANN 召回问题和 manager 语义混在一起。
- 50K 低召回不是 9G3 小 query sample 偶然现象，q=200 下仍复现。
- efSearch=1024 能显著恢复 recall，说明参数不足是明确因素。
- 提高 M/M0 + efConstruction 后，50K recall 可以恢复到 0.9975，但 build 成本约 1344s，说明产品化不能只调参数，还要处理 rebuild/cold-start 成本。
- mature library parity 还没完成，因此不能武断说是 v2 graph-quality bug，也不能说已经 parity。

## 验证结果

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

Required tests：

```text
tests/vector_core_hnsw_v2_parity_baseline_test.sh PASS
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
```

Static boundary checks：

```text
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output
```

git diff --check：

```text
PASS
```

git status --short：

```text
 M Makefile
 M docs/hnsw_productization/README.md
 M docs/hnsw_productization/index.md
?? docs/hnsw_productization/hnsw_p1_parity_baseline_report.md
?? tests/vector_core_hnsw_v2_parity_baseline_test.c
?? tests/vector_core_hnsw_v2_parity_baseline_test.sh
```

## P1 decision

```text
benchmark_confidence = medium
parameter_insufficient = yes
graph_quality_gap = unknown
mature_library_parity = partial
next_stage = redo-P1
```

说明：

- 对 10K/50K v2 参数敏感性，本次置信度较高。
- 对 100K 和 mature library parity，置信度不足。
- 如果只基于 v2 内部数据，P2 参数硬化有依据。
- 但由于 mature library parity 仍是 partial，正式选择 P2/P3 前应先完成一个更窄的 P1-long-run / P1b：同一 dataset/query 下跑 FAISS/hnswlib 50K parity。
