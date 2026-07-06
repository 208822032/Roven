# HNSW-P2-final: 100K balanced gate 与默认参数固化

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

本阶段只做 100K balanced gate、`core_hnsw_v2` 默认参数固化和配套文档/测试接入。未修改 VSEARCH response、RDB/AOF、`kvs_vector_index_ops_t`，未删除 `core_flat` 或 legacy `core_hnsw`，未接 hnswlib / USearch，未做 FAISS parity、SIMD/layout/reverse_prune cache 微优化、graph construction refactor、background thread 或新增用户命令。

未发现需要停止执行的冲突。`AGENTS.md` 历史模板要求 stage report 放在 `docs/vector_core_reset/`，本任务明确属于 `docs/hnsw_productization/` 的 `HNSW-P*` 产品化路线，因此报告写入本目录，同时继续遵守 manager/vector_core 边界。

## 修改/新增文件

新增：

- `tests/vector_core_hnsw_v2_100k_balanced_gate_test.c`
- `tests/vector_core_hnsw_v2_100k_balanced_gate_test.sh`
- `docs/hnsw_productization/hnsw_p2_100k_balanced_default_report.md`

修改：

- `Makefile`
- `include/vector/kvs_vector_hnsw_v2.h`
- `docs/hnsw_productization/README.md`
- `docs/hnsw_productization/index.md`
- `docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md`

本轮工作区中还保留了 P1b-B 的既有未提交改动；本阶段未回退这些改动。

## 是否修改生产代码

修改了生产 header：

```c
#define KVS_HNSW_V2_DEFAULT_M 48
#define KVS_HNSW_V2_DEFAULT_M0 96
#define KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION 512
#define KVS_HNSW_V2_DEFAULT_EF_SEARCH 1024
```

这是 gate 通过后的 `core_hnsw_v2` 默认参数固化。没有修改产品命令语义、响应格式、RDB/AOF、index ops、`core_flat`、legacy `core_hnsw`、FAISS 或 hnswlib adapter。

## 100K balanced gate 参数

| item | value |
|---|---:|
| count | 100000 |
| dim | 512 |
| queries | 100 |
| topK | 10 |
| metric | IP normalized vectors |
| M | 48 |
| M0 | 96 |
| efConstruction | 512 |
| efSearch | 1024 |

本轮没有降级到 50 queries。

## Oracle

继续使用：

- direct exact scan
- `core_flat` cross-check
- fixed seed dataset/query

结果：

| count | dim | queries | topK | oracle_mismatch | oracle_us |
|---:|---:|---:|---:|---:|---:|
| 100000 | 512 | 100 | 10 | 0 | 7,974,478 |

`oracle_mismatch = 0`，因此本次 recall gate 有效。

## Gate 结果

| metric | value |
|---|---:|
| recall@10 | 0.9740 |
| build_us | 3,011,810,795 |
| memory_bytes | 851,194,400 |
| search_avg_us | 16,251.50 |
| search_p50_us | 16,205 |
| search_p95_us | 16,536 |
| visited_avg | 59,945.90 |
| distance_avg | 60,118.20 |
| level0_degree_avg | 96.00 |
| upper_degree_avg | 47.67 |
| duplicate_edge | 0 |
| degree_overflow | 0 |

Graph stats：

| max_level | entry_point_level | level0_degree_min | level0_degree_p50 | level0_degree_p95 | level0_degree_max | upper_degree_p50 | upper_degree_p95 | upper_degree_max | isolated |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 3 | 3 | 96 | 96 | 96 | 96 | 48 | 48 | 48 | 0 |

Gate 判定：

```text
oracle_mismatch = 0
recall@10 = 0.9740 >= 0.95
duplicate_edge = 0
degree_overflow = 0
=> 100k_balanced_gate = pass
```

按照任务规则，balanced 可固化为 `core_hnsw_v2` 默认参数。由于 recall@10 也超过 0.97，本轮 balanced 可作为 100K 推荐默认候选，但这仍不是完整生产 SLA。

## 默认参数固化

已固化：

| parameter | old | new |
|---|---:|---:|
| `KVS_HNSW_V2_DEFAULT_M` | 32 | 48 |
| `KVS_HNSW_V2_DEFAULT_M0` | 64 | 96 |
| `KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION` | 256 | 512 |
| `KVS_HNSW_V2_DEFAULT_EF_SEARCH` | 256 | 1024 |

说明：

- 只修改 `core_hnsw_v2` 默认参数。
- legacy `core_hnsw`、`core_flat`、FAISS adapter、hnswlib adapter 不受此默认宏影响。
- `src/vector/kvs_vector.c` 的 v2 configure path 仍使用 v2-specific default，并将 shared config 中低于 v2 default 的值提升到 v2 default。

## 为什么不进入 P3 graph refactor

P1b-B 已证明 50K balanced 下 FAISS direct HNSW 没有显著高于 v2：

```text
v2 recall@10 = 0.9975
FAISS recall@10 = 0.9945
FAISS - v2 = -0.0030
```

本轮 100K balanced gate 也通过：

```text
v2 recall@10 = 0.9740
duplicate/overflow = 0
```

因此当前证据支持“baseline/default 参数不足”是前序 50K/100K 低召回主因，不支持直接进入 HNSW-P3 graph construction refactor。

## 为什么不继续 FAISS/hnswlib parity

本阶段目标是最短路径验证 100K balanced gate 并在通过后固化默认参数，不做 FAISS parity、不接 hnswlib、不跑多参数矩阵。

P1b 已补齐 FAISS direct quick baseline，P1b-B 已补齐 50K balanced FAISS direct long-run。本轮 100K gate 的阻断项是 v2 自身 recall/oracle/degree invariant，而不是 mature library parity。

## Build / cold-start 风险

本轮 build：

```text
build_us = 3,011,810,795
```

约 50.2 分钟。该成本非常高，不能包装成生产级 cold-start / synchronous rebuild SLA。

本阶段按规则只记录 build/cold-start 风险，不把它作为 balanced 默认参数的 gate 阻断项。下一步必须进入 HNSW-P5，单独设计 rebuild / cold-start 产品化策略，例如后台 rebuild、shadow rebuild、bulk import 策略、进度/失败观测和运维降级路径。

## 验证结果

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS
```

100K balanced gate：

```text
tests/vector_core_hnsw_v2_100k_balanced_gate_test.sh
PASS
```

v2 regressions：

```text
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
```

`tests/vector_core_hnsw_v2_search_recall_test.sh` 关键结果：

| count | dim | ef | recall@10 | build_us | search_avg_us |
|---:|---:|---:|---:|---:|---:|
| 1000 | 512 | 256 | 1.0000 | 4,066,195 | 217 |
| 5000 | 512 | 256 | 1.0000 | 44,271,238 | 671 |
| 10000 | 512 | 64 | 0.7780 | 121,306,529 | 894 |
| 10000 | 512 | 128 | 0.9580 | 121,306,529 | 1,292 |
| 10000 | 512 | 256 | 0.9980 | 121,306,529 | 1,707 |
| 10000 | 512 | 512 | 1.0000 | 121,306,529 | 2,118 |
| 10000 | 512 | 1024 | 1.0000 | 121,306,529 | 2,706 |

额外 long-run parity：

```text
tests/vector_core_hnsw_v2_parity_baseline_test.sh
HNSW_P1B_MODE=balanced-only tests/vector_core_hnsw_faiss_parity_test.sh
```

本轮未重复运行。原因：这两个测试包含 50K/FAISS long-run，P1b-B 已完成并记录；P2-final 任务允许在参数固化成功后只运行 default backend / product integration / search recall，并说明未重复 long-run parity。

## 静态边界检查

```text
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output
```

## git diff --check

```text
PASS
```

## git status --short

```text
 M Makefile
 M docs/hnsw_productization/README.md
 M docs/hnsw_productization/hnsw_p1b_faiss_parity_report.md
 M docs/hnsw_productization/index.md
 M docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md
 M include/vector/kvs_vector_hnsw_v2.h
 M tests/vector_core_hnsw_faiss_parity_test.cpp
?? docs/hnsw_productization/hnsw_p1b_balanced_faiss_parity_report.md
?? docs/hnsw_productization/hnsw_p2_100k_balanced_default_report.md
?? tests/vector_core_hnsw_v2_100k_balanced_gate_test.c
?? tests/vector_core_hnsw_v2_100k_balanced_gate_test.sh
```

## 面试可讲结论

可以这样讲：

- 之前 50K/100K default 低召回没有直接判定为实现失败，而是先做 exact oracle、FAISS direct parity 和参数归因。
- P1b-B 证明 50K balanced 下 v2 recall 不落后于 FAISS direct HNSW。
- P2-final 再用 100K / 512d / 100 queries 做 single balanced gate，recall@10=0.9740，oracle mismatch=0，duplicate/overflow=0。
- 因此 default profile 从低参数切到 balanced 是有数据依据的。
- 但 build 约 50 分钟，不能包装成生产 cold-start SLA；下一步必须做 HNSW-P5 rebuild/cold-start 产品化，而不是继续盲目 graph refactor 或 SIMD 微优化。

## P2-final decision

```text
P2-final decision:
  100k_balanced_gate = pass
  recall_at_10 = 0.9740
  default_param_change = yes
  new_default_profile = balanced
  build_cost_risk = high
  next_stage = HNSW-P5
```
