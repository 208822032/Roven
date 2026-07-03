# HNSW 产品化面试讲法

## 一句话版本

这不是简单“接一个 HNSW”的任务，而是先拆清 KV manager 和 vector_core 的边界，再建立 exact baseline 和 ANN backend，最后用 scale gate 判断哪些能力已经可用、哪些风险还需要归因。

## Manager / vector_core 边界

manager 负责用户可见语义：

- VSET / VSEARCH / VDEL / VINDEX / VINFO
- record store、key、scope、TTL、metadata、question、answer
- active/deleted filtering
- threshold filtering
- response formatting
- AOF/RDB/replication
- rebuild orchestration

vector_core 只负责：

- record_id
- vector
- score
- index-local graph/search state

这个边界很关键，因为 ANN search 不能意外接管 scope、TTL、metadata、response assembly 或 persistence 语义。record store 是 source of truth，HNSW graph 只是 derived runtime state。

## 为什么先做 core_flat

`core_flat` 是 exact oracle：

- 提供 deterministic recall ground truth。
- 用最简单 backend 证明 manager/vector_core 边界。
- 在 ANN 行为需要审计时提供 fallback。
- 帮助区分 ANN graph bug 和 manager resolve/filter bug。

它是正确性 baseline，不是性能目标。

## 为什么重做 core_hnsw_v2

legacy `core_hnsw` 作为第一版 in-repo HNSW backend 是有价值的，但后续 scale test 和 audit 说明，继续在旧实现上打补丁会越来越难解释和验证。

`core_hnsw_v2` 被重做成自包含 ANN backend：

- 输入：record_id + vector
- 输出：record_id + score
- 不访问 manager global state
- 不处理 scope/TTL/metadata/question/answer
- 不做 response formatting
- graph 是 derived runtime state

v2 还修正了重要的 graph-construction 语义，例如新节点 forward edge 独立保留，不再依赖旧邻居 reverse edge 是否接受。

## 10K gate

10K/512d gate 证明默认路径在当前小规模验证集上可用：

- ef=256 recall@10 = 0.9860
- ef=512 recall@10 = 1.0000
- duplicate edge = 0
- degree overflow = 0

这是当前已验证 gate，不是对所有规模的生产承诺。

## 50K/100K gate

50K/100K gate 暴露了 scale recall 问题：

- 50K ef=512 recall@10 = 0.8600
- 100K ef=512 recall@10 = 0.7200

这些数字说明当前参数或图质量在这些规模上还没有生产 ready。正确做法是归因，而不是包装说法，也不是盲目优化。

## 结构合法不等于召回质量达标

`duplicate_edge=0` 和 `degree_overflow=0` 是必要安全检查，但它们只能证明 graph 满足结构约束。

它们不能证明：

- neighbor diversity 足够好。
- long-range navigation 足够好。
- entry point 行为健康。
- upper-layer connectivity 足够。
- recall 已经和 FAISS / hnswlib / USearch parity。

面试里要强调这个区别：graph valid 不等于 graph useful。

## 为什么仿照 FAISS / USearch 仍可能失败

即使实现形态接近成熟 HNSW，召回仍可能失败，原因包括：

- ef_search 对目标规模过低。
- ef_construction 过低。
- M/M0 过低。
- neighbor_select 多样性差异。
- reverse_prune 过度激进。
- random level distribution 波动。
- entry point 质量问题。
- dataset 和 metric 敏感性。
- benchmark sampling 不充分。

下一步应该在等价设置下和外部 baseline 对比，而不是假设“形态相似就等于图质量相同”。

## 下一步归因路径

如果要推进 100K 产品化，需要审阅：

- efSearch sensitivity
- efConstruction sensitivity
- M/M0 sensitivity
- graph-quality stats
- entry point 和 level distribution
- neighbor_select 和 reverse_prune 行为
- FAISS / hnswlib / USearch parity

只有完成归因后，才能决定是调整参数、修复 graph construction，还是调整 rebuild / cold-start 策略。

## 面试中要强调的能力点

可以重点强调：

- 有 exact baseline。
- 默认 backend 切换时保留了显式 fallback。
- legacy `core_hnsw` 没有过早删除。
- VSEARCH response 和 RDB/AOF compatibility 被保留。
- 在宣称生产可用前加了 scale gate。
- 50K/100K 风险被明确记录。
- 发现 recall gap 后没有盲目优化。
- 产品化路线重新围绕 recall、参数、graph quality 和 rebuild behavior 定义。
