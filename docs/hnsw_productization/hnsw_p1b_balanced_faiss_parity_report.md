# HNSW-P1b-B: 50K balanced-only FAISS parity long-run

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

本阶段只补 HNSW-P1b 未完成的 50K balanced FAISS direct parity。未修改生产代码，未修改 `src/`、`include/`、VSEARCH response、RDB/AOF、`kvs_vector_index_ops_t`、默认 backend 或默认 HNSW 参数；未接 hnswlib / USearch；未做 SIMD/layout/reverse_prune cache 优化；未让 v2 core 访问 manager-owned state。

未发现需要停止执行的冲突。`AGENTS.md` 的历史 stage report 模板默认指向 `docs/vector_core_reset/`，但本任务明确属于 `docs/hnsw_productization/` 的 `HNSW-P*` 产品化路线，因此报告写入本目录，同时继续遵守 manager/vector_core 边界。

## 修改/新增文件

新增：

- `docs/hnsw_productization/hnsw_p1b_balanced_faiss_parity_report.md`

修改：

- `tests/vector_core_hnsw_faiss_parity_test.cpp`
- `docs/hnsw_productization/README.md`
- `docs/hnsw_productization/index.md`
- `docs/hnsw_productization/hnsw_p1b_faiss_parity_report.md`

## 是否修改生产代码

未修改生产代码。

`tests/vector_core_hnsw_faiss_parity_test.cpp` 只新增 test-only `HNSW_P1B_MODE=balanced-only` 分支，复用既有 FAISS direct harness 和 v2 bridge。

## Harness 变化

新增模式：

```bash
HNSW_P1B_MODE=balanced-only tests/vector_core_hnsw_faiss_parity_test.sh
```

该模式只运行：

| item | value |
|---|---:|
| count | 50K |
| dim | 512 |
| queries | 200 |
| topK | 10 |
| metric | IP normalized vectors |
| v2 M/M0 | 48 / 96 |
| FAISS M | 48 |
| efConstruction | 512 |
| efSearch | 1024 |
| backends | `core_hnsw_v2`, `faiss_hnsw` |

该模式未重复 10K sanity 和 50K baseline。

## Oracle

本阶段继续复用 P1/P1b 的 shared oracle 设计：

- direct exact scan
- `core_flat` cross-check
- fixed seed dataset/query

结果：

| count | dim | queries | topK | mismatch | oracle_us |
|---:|---:|---:|---:|---:|---:|
| 50K | 512 | 200 | 10 | 0 | 7,886,927 |

`oracle_mismatch = 0`，因此本次 parity 判断有效。

## Balanced 结果

| backend | count | M | M0 | efConstruction | efSearch | build_us | search_avg_us | search_p50_us | search_p95_us | recall@10 | memory_bytes | visited_avg | distance_avg |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|
| core_hnsw_v2 | 50K | 48 | 96 | 512 | 1024 | 1,198,073,777 | 10,723.89 | 10,724 | 10,853 | 0.9975 | 425,597,248 | 41,624.05 | 41,767.86 |
| FAISS HNSW | 50K | 48 | N/A | 512 | 1024 | 277,934,315 | 9,736.41 | 9,724 | 9,851 | 0.9945 | N/A | N/A | N/A |

v2 graph stats：

| max_level | entry_point_level | level0_degree_avg | upper_degree_avg | isolated | duplicate_edge | degree_overflow | selected | reverse_prune |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 3 | 3 | 96.00 | 47.46 | 0 | 0 | 0 | 4,841,443 | 4,835,520 |

## Recall gap

判断阈值：

```text
FAISS recall@10 - v2 recall@10 >= 0.03
```

本次：

| profile | efSearch | v2 recall | FAISS recall | FAISS - v2 | significant |
|---|---:|---:|---:|---:|---|
| balanced | 1024 | 0.9975 | 0.9945 | -0.0030 | no |

结论：

```text
graph_quality_gap = no
```

在 50K balanced 参数下，FAISS direct HNSW 没有显著高于 `core_hnsw_v2`。本次不能支持“v2 graph-quality 明显落后 FAISS”的判断。

## Build / search cost gap

| metric | core_hnsw_v2 | FAISS HNSW | v2 / FAISS |
|---|---:|---:|---:|
| build_us | 1,198,073,777 | 277,934,315 | 4.31x |
| search_avg_us | 10,723.89 | 9,736.41 | 1.10x |
| search_p95_us | 10,853 | 9,851 | 1.10x |

判断：

- recall parity 不再是 balanced 50K 下的主矛盾。
- v2 build 明显慢于 FAISS，build/cold-start 风险仍为 high。
- v2 search 也慢于 FAISS，但差距约 10%，远小于 build 差距。

## 参数不足判断

```text
parameter_insufficient = yes
```

含义：

- 对 baseline/default profile，参数不足仍成立：P1/P1b 已证明 50K baseline `M=32 / M0=64 / efConstruction=256` 在 ef=512 下 recall 只有 0.8690，ef=1024 才到 0.9775。
- 对 50K balanced profile，`M=48 / M0=96 / efConstruction=512 / efSearch=1024` 已可把 v2 recall 提升到 0.9975，且 FAISS 同参没有更高。
- 因此下一步更适合 HNSW-P2 参数 profile 硬化，而不是直接进入 HNSW-P3 graph construction refactor。

## P1 baseline regression

`tests/vector_core_hnsw_v2_parity_baseline_test.sh` 本轮通过。

关键输出：

| case | recall@10 | build_us | duplicate/overflow |
|---|---:|---:|---:|
| 10K baseline ef=256 | 0.9800 | 60,468,081 | 0 / 0 |
| 10K baseline ef=512 | 0.9990 | 60,468,081 | 0 / 0 |
| 10K baseline ef=1024 | 1.0000 | 60,468,081 | 0 / 0 |
| 50K baseline ef=256 | 0.6620 | 465,504,846 | 0 / 0 |
| 50K baseline ef=512 | 0.8690 | 465,504,846 | 0 / 0 |
| 50K baseline ef=1024 | 0.9775 | 465,504,846 | 0 / 0 |
| 50K balanced ef=512 | 0.9575 | 1,298,376,600 | 0 / 0 |
| 50K balanced ef=1024 | 0.9975 | 1,298,376,600 | 0 / 0 |

## P1b quick regression

`tests/vector_core_hnsw_faiss_parity_test.sh` 本轮通过。

关键输出：

| count | profile | efSearch | v2 recall | FAISS recall | FAISS - v2 | significant |
|---:|---|---:|---:|---:|---:|---|
| 10K | baseline | 256 | 0.9800 | 0.9740 | -0.0060 | no |
| 10K | baseline | 512 | 0.9990 | 0.9970 | -0.0020 | no |
| 50K | baseline | 512 | 0.8690 | 0.8360 | -0.0330 | no |
| 50K | baseline | 1024 | 0.9775 | 0.9635 | -0.0140 | no |

## 验证结果

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

Tests：

```text
HNSW_P1B_MODE=balanced-only tests/vector_core_hnsw_faiss_parity_test.sh PASS
tests/vector_core_hnsw_faiss_parity_test.sh PASS
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

## 不建议继续做的方向

当前不建议：

- 直接做 SIMD/layout/reverse_prune cache 微优化。
- 直接进入 HNSW-P3 graph construction refactor。
- 修改默认 backend 或默认 HNSW 参数。
- 宣称 50K/100K 已具备生产 SLA。
- 删除 legacy `core_hnsw`。
- 修改 VSEARCH response。
- 修改 RDB/AOF graph persistence。
- 本阶段继续接 hnswlib 或 USearch。

## 面试可讲结论

可以这样讲：

- P1b-B 用同一 dataset/query/oracle，把 v2 balanced 参数和 FAISS HNSW direct 同参对齐。
- balanced 下 v2 recall@10=0.9975，FAISS recall@10=0.9945，FAISS 没有显著领先。
- 这说明当前 50K 低召回主要不是 v2 graph-quality 明显落后 FAISS，而是 baseline/default 参数不足。
- 但 v2 build 比 FAISS 慢约 4.31x，所以产品化风险从 recall parity 转向参数 profile 选择和 rebuild/cold-start 策略。
- 因此下一步应进入 HNSW-P2 参数硬化，并同步规划 HNSW-P5，不应盲目做 graph refactor 或微优化。

## git diff --check

```text
PASS
```

## git status --short

```text
 M docs/hnsw_productization/README.md
 M docs/hnsw_productization/hnsw_p1b_faiss_parity_report.md
 M docs/hnsw_productization/index.md
 M tests/vector_core_hnsw_faiss_parity_test.cpp
?? docs/hnsw_productization/hnsw_p1b_balanced_faiss_parity_report.md
```

## P1b-B decision

```text
P1b-B decision:
  faiss_balanced_parity = pass
  parameter_insufficient = yes
  graph_quality_gap = no
  build_cost_risk = high
  next_stage = HNSW-P2 + HNSW-P5
```
