# HNSW-P1b: FAISS HNSW direct parity baseline

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

本阶段只新增 test-only FAISS direct C++ parity harness、测试脚本、Makefile 测试接线和 HNSW 产品化文档。未修改生产代码，未修改 VSEARCH response，未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未切换默认 backend，未修改默认 HNSW 参数，未删除 `core_flat` 或 legacy `core_hnsw`，未让 v2 core 访问 manager-owned state。

未发现需要停止执行的冲突。`AGENTS.md` 的历史 stage report 模板默认指向 `docs/vector_core_reset/`，但本任务明确属于 `docs/hnsw_productization/` 的 `HNSW-P*` 产品化路线，因此报告写入本目录，同时继续遵守 manager/vector_core 边界和验证要求。

## 修改/新增文件

新增：

- `tests/vector_core_hnsw_faiss_parity_test.cpp`
- `tests/vector_core_hnsw_faiss_parity_bridge.h`
- `tests/vector_core_hnsw_faiss_parity_bridge.c`
- `tests/vector_core_hnsw_faiss_parity_test.sh`
- `docs/hnsw_productization/hnsw_p1b_faiss_parity_report.md`

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
- 现有 FAISS 产品 adapter
- 现有 FAISS C ABI wrapper

`Makefile` 仅新增 test-only FAISS parity 二进制接线。

## Harness 设计

新增测试：

```bash
tests/vector_core_hnsw_faiss_parity_test.sh
```

默认模式：

```bash
HNSW_P1B_MODE=quick
```

覆盖：

- 10K sanity：`M=32 / M0=64 / efConstruction=256 / efSearch=256,512`
- 50K baseline：`M=32 / M0=64 / efConstruction=256 / efSearch=512,1024`

`full` 模式保留：

- 10K sanity
- 50K baseline
- 50K balanced：`M=48 / M0=96 / efConstruction=512 / efSearch=1024`
- 可选 100K smoke-plus：设置 `HNSW_P1B_100K=1`

本次执行的是默认 `quick`。原因是 P1 中 50K balanced v2 build 已约 1344s，本阶段先完成必须的 50K baseline FAISS direct parity，不把默认必跑脚本扩成不可控 long-run。

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
- `core_flat` 对同一 dataset/query 交叉验证。
- 输出 `oracle mismatch`。

本次结果：

| count | queries | direct exact | core_flat cross-check | mismatch | oracle_us |
|---:|---:|---|---|---:|---:|
| 10K | 100 | yes | yes | 0 | 858,398 |
| 50K | 200 | yes | yes | 0 | 8,039,389 |

Oracle 可信度：10K/50K 为 high。100K 本次未跑，仍不能写成生产 SLA。

## FAISS direct harness

FAISS direct C++ harness 已完成，直接使用：

```cpp
faiss::IndexHNSWFlat index(dim, M, faiss::METRIC_INNER_PRODUCT);
index.hnsw.efConstruction = efConstruction;
index.hnsw.efSearch = efSearch;
```

该 harness 只用于测试，不进入产品代码，不修改现有 FAISS adapter，不修改 FAISS C ABI wrapper。

注意：FAISS 的 HNSW 参数语义和 v2 未必数学等价。本报告只做同名参数下的工程 parity baseline。

## Baseline profile parity

Baseline：

```text
M=32
M0=64 for core_hnsw_v2
efConstruction=256
```

### 10K sanity

| backend | count | queries | efSearch | build_us | search_avg_us | search_p50_us | search_p95_us | recall@10 | memory_bytes | visited_avg | distance_avg |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|
| core_hnsw_v2 | 10K | 100 | 256 | 62,539,186 | 1,059.51 | 1,059 | 1,258 | 0.9800 | 64,104,384 | 7,897.14 | 7,986.75 |
| FAISS HNSW | 10K | 100 | 256 | 12,582,505 | 845.66 | 850 | 941 | 0.9740 | N/A | N/A | N/A |
| core_hnsw_v2 | 10K | 100 | 512 | 62,539,186 | 1,581.26 | 1,586 | 1,703 | 0.9990 | 64,104,384 | 9,517.79 | 9,607.40 |
| FAISS HNSW | 10K | 100 | 512 | 12,582,505 | 1,351.11 | 1,353 | 1,456 | 0.9970 | N/A | N/A | N/A |

10K gap：

| efSearch | v2 recall | FAISS recall | FAISS - v2 | significant >= 0.03 |
|---:|---:|---:|---:|---|
| 256 | 0.9800 | 0.9740 | -0.0060 | no |
| 512 | 0.9990 | 0.9970 | -0.0020 | no |

### 50K baseline

| backend | count | queries | efSearch | build_us | search_avg_us | search_p50_us | search_p95_us | recall@10 | memory_bytes | visited_avg | distance_avg |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|
| core_hnsw_v2 | 50K | 200 | 512 | 489,575,673 | 6,123.97 | 6,114 | 6,252 | 0.8690 | 319,997,248 | 23,196.15 | 23,327.15 |
| FAISS HNSW | 50K | 200 | 512 | 144,485,304 | 4,871.73 | 4,849 | 5,018 | 0.8360 | N/A | N/A | N/A |
| core_hnsw_v2 | 50K | 200 | 1024 | 489,575,673 | 9,835.69 | 9,832 | 9,976 | 0.9775 | 319,997,248 | 35,055.01 | 35,186.01 |
| FAISS HNSW | 50K | 200 | 1024 | 144,485,304 | 8,389.17 | 8,344 | 8,622 | 0.9635 | N/A | N/A | N/A |

50K gap：

| efSearch | v2 recall | FAISS recall | FAISS - v2 | significant >= 0.03 |
|---:|---:|---:|---:|---|
| 512 | 0.8690 | 0.8360 | -0.0330 | no |
| 1024 | 0.9775 | 0.9635 | -0.0140 | no |

50K baseline 结论：

- FAISS direct 同参 recall 没有显著高于 v2。
- 在本 dataset / query / metric / topK 下，baseline profile 未观察到成熟库明显领先 v2 的 graph-quality parity gap。
- v2 build 明显慢于 FAISS：50K baseline 约 489.6s vs FAISS 约 144.5s。
- v2 search 也慢于 FAISS，但差距小于 build 差距。

## Balanced profile parity

Balanced：

```text
M=48
M0=96 for core_hnsw_v2
efConstruction=512
efSearch=1024
```

P1b 本次未运行；后续 P1b-B 已补完 balanced-only long-run。

P1b 当时未运行原因：

- P1 已证明 v2 50K balanced build_us 约 1,344s。
- P1b 默认 quick 先补必须的 50K baseline FAISS direct parity。
- 没有 balanced FAISS direct parity 时，不能把 P1b 结论升级成最终 P2/P3 判断。

P1b-B 结果见：

```text
docs/hnsw_productization/hnsw_p1b_balanced_faiss_parity_report.md
```

P1b-B 关键结论：

- 50K balanced `M=48 / M0=96 / efConstruction=512 / efSearch=1024`。
- oracle mismatch=0。
- v2 recall@10=0.9975。
- FAISS recall@10=0.9945。
- FAISS - v2 = -0.0030，significant=no。
- balanced profile 下未观察到 FAISS 显著高于 v2 的 graph-quality parity gap。
- v2 build 明显慢于 FAISS，build/cold-start 风险仍为 high。

## 100K smoke-plus

本次未运行。

原因：100K smoke-plus 是可选项，不应阻塞 P1b baseline parity。当前 100K 仍不能写成生产 SLA。

## v2 与 FAISS 成本差距

| count | profile | v2 build_us | FAISS build_us | FAISS / v2 build | v2 search_avg_us ef=1024 | FAISS search_avg_us ef=1024 | FAISS / v2 search |
|---:|---|---:|---:|---:|---:|---:|---:|
| 10K | baseline | 62,539,186 | 12,582,505 | 0.20x | 1,581.26 | 1,351.11 | 0.85x |
| 50K | baseline | 489,575,673 | 144,485,304 | 0.30x | 9,835.69 | 8,389.17 | 0.85x |

成本判断：

- v2 build 成本显著高于 FAISS。
- v2 search 成本高于 FAISS，但在 baseline profile 下不是数量级差异。
- 这强化了 rebuild/cold-start 风险：即使 recall 通过，生产化仍需要处理同步 build 成本。

## Graph-quality parity gap 判断

本次 baseline profile 判断：

```text
graph_quality_gap_baseline = no
```

依据：

- 10K/50K baseline 下，FAISS recall 没有高于 v2。
- 使用阈值 `FAISS recall@10 - v2 recall@10 >= 0.03` 判定显著 gap，本次所有 baseline gap 均小于阈值，且为负数。

整体判断：

```text
graph_quality_gap = unknown
```

原因：

- balanced profile parity 未完成。
- 100K smoke-plus 未完成。
- FAISS 参数语义和 v2 M0 / reverse prune 语义不完全等价。

## 参数不足判断

```text
parameter_insufficient = yes
```

依据：

- P1 已证明 50K baseline efSearch 从 512 提升到 1024 后，v2 recall@10 从 0.8690 提升到 0.9775。
- P1 已证明 balanced 参数下 50K efSearch=1024 可达到 0.9975。
- P1b baseline FAISS direct 没有显示成熟库同参数显著高于 v2，因此当前证据更支持“baseline 参数不足 + build 成本风险”，而不是“v2 baseline graph-quality 明显落后 FAISS”。

## Build / cold-start 风险

```text
build_cost_risk = high
```

依据：

- v2 50K baseline build 约 489.6s。
- P1 v2 50K balanced build 约 1344s。
- 9G3 100K sync build 约 1034s。
- RDB/AOF 仍保存 records，不保存 graph；cold start 需要 rebuild derived graph。

## 不建议继续做的方向

当前不建议：

- 直接做 SIMD/layout/reverse_prune cache 微优化。
- 直接修改默认 HNSW 参数。
- 直接宣称 50K/100K 生产 SLA。
- 直接进入 graph construction refactor。
- 删除 legacy `core_hnsw`。
- 修改 VSEARCH response。
- 修改 RDB/AOF graph persistence。
- 接 hnswlib 或 USearch 到本阶段测试。

## 面试可讲结论

可以这样讲：

- P1 先用 direct exact scan + `core_flat` cross-check 确认 oracle，避免把 ANN 召回问题和 manager 语义混在一起。
- P1b 绕开产品 FAISS adapter，直接用 FAISS C++ `IndexHNSWFlat` 设置 `efConstruction/efSearch`，补齐成熟库同参 baseline。
- 在 50K baseline 下，FAISS 同参 recall 没有高于 v2；这说明不能简单把 50K 低召回归因为 v2 graph-quality 明显落后 FAISS。
- 50K recall 能随 efSearch 和 balanced 参数明显恢复，说明参数不足是明确因素。
- 但 build 成本非常高，说明产品化不能只调参数，还必须处理 rebuild / cold-start 策略。
- balanced FAISS parity 未完成前，结论保持审慎，不能直接进入最终参数发布或 graph refactor。

## 验证结果

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

Required tests：

```text
tests/vector_core_hnsw_faiss_parity_test.sh PASS
tests/vector_core_hnsw_v2_parity_baseline_test.sh PASS
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
```

P1 baseline regression snapshot：

| case | recall@10 | build_us | duplicate/overflow |
|---|---:|---:|---:|
| 10K baseline ef=256 | 0.9800 | 63,414,257 | 0 / 0 |
| 10K baseline ef=512 | 0.9990 | 63,414,257 | 0 / 0 |
| 10K baseline ef=1024 | 1.0000 | 63,414,257 | 0 / 0 |
| 50K baseline ef=256 | 0.6620 | 491,304,731 | 0 / 0 |
| 50K baseline ef=512 | 0.8690 | 491,304,731 | 0 / 0 |
| 50K baseline ef=1024 | 0.9775 | 491,304,731 | 0 / 0 |
| 50K balanced ef=512 | 0.9575 | 1,343,584,213 | 0 / 0 |
| 50K balanced ef=1024 | 0.9975 | 1,343,584,213 | 0 / 0 |

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
?? docs/hnsw_productization/hnsw_p1b_faiss_parity_report.md
?? tests/vector_core_hnsw_faiss_parity_bridge.c
?? tests/vector_core_hnsw_faiss_parity_bridge.h
?? tests/vector_core_hnsw_faiss_parity_test.cpp
?? tests/vector_core_hnsw_faiss_parity_test.sh
?? tests/vector_core_hnsw_v2_parity_baseline_test.c
?? tests/vector_core_hnsw_v2_parity_baseline_test.sh
```

## 下一步判断

本阶段 baseline parity 没有支持“FAISS 同参明显高于 v2”。但因为 balanced FAISS direct parity 未完成，不能直接进入 HNSW-P2/P3/P5 的最终路线判断。

建议下一步：

1. 先补 `redo-P1b` 的 balanced-only long-run：
   - 50K / 512d / q=200
   - M=48 / M0=96 for v2
   - efConstruction=512
   - efSearch=1024
   - backends=core_hnsw_v2, FAISS HNSW
2. 如果 balanced 下 FAISS 和 v2 recall 仍接近：进入 HNSW-P2 参数硬化，并同步规划 HNSW-P5 rebuild/cold-start。
3. 如果 balanced 下 FAISS 显著高于 v2：进入 HNSW-P3 图构建质量修复。

P1b-B 已在后续补完 balanced-only long-run。最新路线判断见：

```text
docs/hnsw_productization/hnsw_p1b_balanced_faiss_parity_report.md
```

## P1b decision

```text
P1b decision:
  faiss_parity = partial
  parameter_insufficient = yes
  graph_quality_gap = unknown
  build_cost_risk = high
  next_stage = redo-P1b
```

## P1b-B follow-up decision

```text
P1b-B decision:
  faiss_balanced_parity = pass
  parameter_insufficient = yes
  graph_quality_gap = no
  build_cost_risk = high
  next_stage = HNSW-P2 + HNSW-P5
```
