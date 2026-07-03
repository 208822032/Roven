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
- 50K/100K gate 未达到生产召回预期。
- 同步 rebuild 和 cold start 仍然是产品风险。
- RDB/AOF 仍然只保存 records，不保存 HNSW graph topology。
- VSEARCH response format 保持不变。

当前 50K/100K 数据只能作为风险证据，不能写成生产 SLA，也不能说明当前默认配置已经在这些规模上生产可用。

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

下一步应该执行 HNSW-P0：重新审阅 100K recall 归因，判断风险主要来自 benchmark 方法、参数覆盖、图构建质量，还是 rebuild / product readiness。
