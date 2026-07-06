# HNSW 产品化路线

这个目录用于记录 KVLite HNSW 后续产品化路线。

此前的 `docs/vector_core_reset/` 主要记录架构重置、vector manager / vector_core 边界拆分，以及 `core_hnsw_v2` 从原型、优化到接入默认 ANN backend 的过程。Stage 9G3 之后，这条线已经到达新的边界：`core_hnsw_v2` 已经接入并成为默认 ANN backend，但 50K/100K scale gate 暴露了召回不足和同步 rebuild 风险。这些风险不能靠继续堆局部优化阶段来掩盖。

因此，从这里开始，文档从 reset 历史线切换到独立的产品化设计线。后续重点不再是“继续优化直到数字变好”，而是生产级召回、参数 profile、图质量、rebuild / cold-start 策略，以及是否可以对外承诺某个规模的 SLA。

## 当前状态

- `core_hnsw_v2` 已经成为默认 ANN backend。
- `core_flat` 保留为 exact baseline、正确性 oracle 和 fallback。
- legacy `core_hnsw` 保留为显式 fallback 和 regression reference。
- 10K/512d gate 已通过：
  - ef=256 recall@10 = 0.9860
  - ef=512 recall@10 = 1.0000
- baseline/default 50K/100K gate 曾暴露召回不足。
- HNSW-P2-final 已用 balanced 参数完成 100K single gate：
  - `M=48 / M0=96 / efConstruction=512 / efSearch=1024`
  - 100K/512d, 100 queries, recall@10 = 0.9740
  - oracle mismatch = 0，duplicate/overflow = 0
- 同步 rebuild 和 cold start 仍然是产品风险。
- RDB/AOF 仍然只保存 records，不保存 HNSW graph topology。
- VSEARCH response format 保持不变。

当前 100K balanced gate 可以支持将 `core_hnsw_v2` 默认参数切到 balanced，但它仍不是完整生产 SLA。build/cold-start 风险仍然很高，必须单独进入 HNSW-P5。

## 路线定位

新的路线是：

```text
HNSW 产品化设计线
```

这条线优先回答产品问题：

- 50K/100K 需要达到什么 recall 目标？
- 哪些 latency / memory tradeoff 可以作为可发布参数 profile？
- 在等价参数下，当前图质量和 FAISS / hnswlib / USearch 的差距在哪里？
- 低召回来自参数不足、graph construction、entry point / level distribution，还是 benchmark 方法？
- rebuild 和 cold-start 到什么程度才可以对外承诺？

## HNSW-P 路线

```text
HNSW-P0: 100K 召回归因审阅与产品化风险判定
HNSW-P1: FAISS / hnswlib / USearch parity baseline 与参数敏感性补测
HNSW-P2: 参数硬化，输出 low_latency / balanced / high_recall profile
HNSW-P3: 图构建质量修复，只处理被证明有问题的 neighbor_select / reverse_prune / level search / entry point
HNSW-P4: 100K production recall gate
HNSW-P5: rebuild / cold-start 产品化策略
```

## 当前不建议继续做

没有新的归因报告前，不建议直接启动：

- SIMD/layout 微优化。
- reverse_prune cache。
- 删除 legacy `core_hnsw`。
- RDB/AOF graph persistence。
- VSEARCH response 改造。
- 无归因的 1M sprint。

下一步应该执行 HNSW-P5：rebuild / cold-start 产品化策略。不要把 100K balanced gate 通过包装成完整生产 SLA，也不要在没有新归因的情况下直接进入 graph refactor 或微优化。

## 当前 P1 状态

HNSW-P1 quick parity baseline 已完成并记录在：

```text
docs/hnsw_productization/hnsw_p1_parity_baseline_report.md
```

结论摘要：

- 50K 低召回在 200 queries 下复现。
- baseline 参数下，50K efSearch=1024 可将 recall@10 提升到 0.9775。
- balanced 参数 `M=48 / M0=96 / efConstruction=512` 下，50K efSearch=1024 recall@10 达 0.9975。
- balanced build 成本约 1344s，不能直接视为默认生产参数。
- FAISS / hnswlib mature parity 仍为 partial，需要单独 long-run 或 test-only direct C++ harness。

因此，当前还不能直接宣称 100K 生产可用；下一步应补完 mature library parity，或在明确标注 partial 的前提下进入 HNSW-P2 参数 profile 草案。

## 当前 P1b 状态

HNSW-P1b FAISS direct parity baseline 已完成 quick baseline，并记录在：

```text
docs/hnsw_productization/hnsw_p1b_faiss_parity_report.md
```

结论摘要：

- 新增 test-only FAISS C++ direct harness，可直接设置 `IndexHNSWFlat::hnsw.efConstruction` 和 `efSearch`。
- 10K/50K 使用同一 fixed seed dataset、query、IP normalized metric、topK=10 和 exact oracle。
- 10K/50K oracle mismatch 均为 0。
- 50K baseline `M=32 / efConstruction=256` 下，FAISS 同参 recall 没有高于 v2：
  - efSearch=512: v2 0.8690, FAISS 0.8360
  - efSearch=1024: v2 0.9775, FAISS 0.9635
- baseline profile 未观察到 FAISS 显著领先 v2 的 graph-quality parity gap。
- build/cold-start 风险仍然是 high。

HNSW-P1b-B balanced-only FAISS direct parity 已完成，并记录在：

```text
docs/hnsw_productization/hnsw_p1b_balanced_faiss_parity_report.md
```

结论摘要：

- 50K balanced `M=48 / M0=96 / efConstruction=512 / efSearch=1024` 下，oracle mismatch=0。
- v2 recall@10 = 0.9975，FAISS recall@10 = 0.9945。
- FAISS - v2 = -0.0030，未达到显著 graph-quality gap 阈值。
- v2 build 约 1198s，FAISS build 约 278s，v2 build 约为 FAISS 的 4.31x。
- 当前判断应转向 HNSW-P2 参数硬化，并同步规划 HNSW-P5 rebuild/cold-start；不建议直接进入 HNSW-P3 graph refactor。

## 当前 P2-final 状态

HNSW-P2-final 100K balanced gate 与默认参数固化已完成，并记录在：

```text
docs/hnsw_productization/hnsw_p2_100k_balanced_default_report.md
```

结论摘要：

- 100K/512d/100 queries，balanced 参数 `M=48 / M0=96 / efConstruction=512 / efSearch=1024`。
- `oracle_mismatch=0`，direct exact 与 `core_flat` 一致。
- `core_hnsw_v2 recall@10=0.9740`，达到 `>=0.95` gate，通过 100K balanced gate。
- `duplicate_edge=0`，`degree_overflow=0`。
- `core_hnsw_v2` 默认参数已固化为 balanced：
  - `M=48`
  - `M0=96`
  - `efConstruction=512`
  - `efSearch=1024`
- build 约 3,011,810,795us，约 50.2 分钟，build/cold-start 风险仍为 high。

当前判断：

- balanced default 有 100K gate 数据支撑。
- 不能把该 gate 写成完整生产 SLA。
- 不建议进入 HNSW-P3 graph refactor。
- 下一步应进入 HNSW-P5 rebuild / cold-start 产品化策略。
