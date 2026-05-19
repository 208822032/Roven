# ef_search 一致率对比报告

> 基准 ef_search=256 vs 测试 ef_search=128
> 数据量: 100000 (实际查询 1000 条)
> topK: 10

## 结果指标

| metric | value |
|---|---:|
| query_count | 1000 |
| top1_same_rate | 93.2% |
| top3_overlap_rate | 95.3% |
| top10_overlap_rate | 96.5% |
| avg_missing_rate | 3.5% |
| avg_rank_shift | 0.10 |

## 结论

ef_search=128 与 ef_search=256 的结果一致性较高，**可以作为轻量默认配置**。

> 注意：本测试比较的是 ef_search 参数变化导致的结果一致率。
> embedding_provider=hash 时不代表真实语义召回质量。
> 真正的 recall@K 需要 flat 精确搜索作为 ground truth。
