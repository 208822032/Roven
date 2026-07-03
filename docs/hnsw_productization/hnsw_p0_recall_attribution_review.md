# HNSW-P0: 100K 召回归因审阅

## 目标

HNSW-P0 用于重新审阅 Stage 9G3 的 scale gate 数据，并判断 KVLite 在 10K gate 之外还需要证明什么，才能宣称更大规模的生产可用性。

这是 audit / 产品化风险判定阶段，不应从改代码开始。第一步应该验证 benchmark、exact oracle、参数空间和 graph-quality 指标是否足够可信。

## Stage 9G3 关键数据

```text
10K / 512d:
  ef=256 recall@10 = 0.9860
  ef=512 recall@10 = 1.0000
  build ~= 61s
  duplicate/overflow = 0

50K / 512d:
  ef=256 recall@10 = 0.6650
  ef=512 recall@10 = 0.8600
  build ~= 465s
  duplicate/overflow = 0

100K / 512d:
  ef=256 recall@10 = 0.5400
  ef=512 recall@10 = 0.7200
  build ~= 1034s
  duplicate/overflow = 0
```

## 当前结论

- 10K 已通过当前默认 gate。
- 50K/100K recall 未达到生产预期。
- `duplicate_edge=0` 和 `degree_overflow=0` 只能证明结构合法。
- 结构合法不等于图导航质量足够。
- 当前 v2 不能宣称 100K 生产可用。

## 审阅问题

Benchmark 可信度：

- 测试数据是否 deterministic，是否具备代表性？
- exact oracle 是否正确，是否独立于 ANN backend？
- sampled query 数量是否足以支撑 50K/100K 结论？
- exact 和 ANN 路径的 metric polarity、score ordering、normalization 是否一致？
- deleted / tombstoned records 是否被一致排除？

参数覆盖：

- ef_search 对 50K/100K 是否过低？
- ef_construction 对目标 recall 是否过低？
- 100K / 512d 下 M / M0 是否不足？
- low_latency / balanced / high_recall profile 分别能接受什么 latency / memory tradeoff？

Graph quality：

- neighbor_select 是否提供了足够多样性？
- reverse_prune 是否过度剪掉了有用的长距离连接？
- level distribution 是否接近期望？
- entry point 是否漂移到不理想区域？
- upper-layer degree 和 level0 degree 是否健康，而不只是没有 overflow？
- graph navigation 是否因为 candidates 过于局部而停滞？

Parity baseline：

- 在相同 dataset、metric、M/M0、ef_construction、ef_search 下，`core_hnsw_v2` 和 FAISS 差距多大？
- 和 hnswlib 差距多大？
- 如果可用，和 USearch 差距多大？
- 外部 baseline 是否需要不同参数 profile 才能达到目标 recall？

## 产出要求

HNSW-P0 应产出：

- recall 归因报告。
- 参数建议矩阵。
- FAISS / hnswlib / USearch parity baseline 方案。
- 下一步选择建议：进入 HNSW-P1、HNSW-P2，还是 HNSW-P3。

报告必须区分：

- benchmark 正确性问题。
- 参数不足问题。
- graph construction 质量问题。
- rebuild / cold-start 产品风险。

## 退出标准

HNSW-P0 完成时，必须能给出可辩护的回答：

```text
100K recall gap 主要来自测试方法、参数、图质量，还是实现 bug？
```

如果答案还不清楚，就继续 attribution。不要用 SIMD/layout/reverse_prune cache 工作代替召回归因。
