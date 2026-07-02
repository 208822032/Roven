# core_hnsw_v2 后续实施方案摘要

## 1. 背景

当前 KVLite 向量模块的目标是保持 `KVmanager` 与 `Vector_core` 分离：

```text
KVmanager：
  负责 V 族命令、record 管理、key/scope、TTL、metadata、AOF/RDB、replication、rebuild、response assembly。

Vector_core：
  只负责 record_id + vector -> record_id + score。
```

`record store` 是唯一事实来源，`core_flat` / `core_hnsw` / 后续 `core_hnsw_v2` 都是可重建的派生索引状态。HNSW graph 不进入 RDB/AOF，不作为持久化事实来源。

当前定位调整如下：

```text
core_flat：
  exact recall baseline / correctness oracle。
  不再追求 FAISS IndexFlat 性能对标。

current core_hnsw：
  legacy backend / regression reference / fallback。
  不再作为主要优化对象。

core_hnsw_v2：
  新的主生产候选 ANN backend。
  目标是可用、可产品化，并以 FAISS / hnswlib 作为性能、召回率和稳定性标尺。

FAISS / hnswlib：
  只作为 baseline，不作为 KVLite 架构替代。
```

## 2. 当前问题判断

已复测确认 current `core_hnsw` 的 10K / 512d 低召回是真问题：

```text
core_hnsw 1K / 512d:   recall@10 = 1.0000
core_hnsw 5K / 512d:   recall@10 = 0.8400
core_hnsw 10K / 512d:  recall@10 = 0.6240
```

对照 FAISS：

```text
FAISS HNSW 10K / 512d, M=32 efConstruction=128 efSearch=256:
recall@10 = 0.9740
```

临时诊断显示：

```text
core_hnsw 10K / 512d, efSearch=1024:
recall@10 = 0.9960
```

说明 ground truth、维度、manager 回表、距离函数大概率不是主因。核心问题是 current `core_hnsw` 的 graph quality / construction / search semantics / parameter scaling 与 FAISS/hnswlib 不等价，导致默认搜索宽度下召回率不可用。

Stage 9A1 已验证 `accepted_ba` 连边逻辑确实存在语义问题，但解除该门槛后：

```text
10K recall@10: 0.6240 -> 0.6380
```

改善很小，因此它不是 10K 低召回主因。该问题可作为 v2 设计经验吸收，但不建议继续在 current `core_hnsw` 上扩大修补。

## 3. 路线结论

后续不继续围绕 current `core_hnsw` 做局部优化或原地大改。

采取新路线：

```text
冻结 current core_hnsw。
新建独立 core_hnsw_v2。
先 index-only 验证 graph quality。
召回率达标后再做性能优化。
最后再接入现有 kvs_vector_index_ops_t 和产品路径。
```

核心原因：

```text
1. current core_hnsw 已经过多轮局部优化，收益接近上限。
2. 低召回问题体现为图构建和搜索语义的系统性问题。
3. 原地重构 current core_hnsw 风险高，容易污染已有生产结构。
4. 新建 v2 可以隔离风险，保留旧实现作为 fallback 和 regression reference。
```

## 4. 硬边界

后续所有阶段必须继续遵守：

```text
1. 不修改 RDB/AOF 格式。
2. 不修改 kvs_vector_index_ops_t 签名。
3. 不修改 VSEARCH response 格式。
4. 不让 vector_core 访问 g_vector_state。
5. 不让 vector_core 处理 scope / TTL / metadata / question / answer。
6. 不把 HNSW graph 持久化为事实来源。
7. 不删除 current core_hnsw、core_flat、FAISS、hnswlib。
8. core_hnsw_v2 未通过 recall gate 前不接管默认 backend。
```

## 5. 分阶段实施路径

### Stage 9B：core_hnsw_v2 index-only skeleton

目标：

```text
新增独立 core_hnsw_v2 index-only skeleton。
只实现生命周期、内存结构和 info/memory accounting。
不实现 add/search。
不接入 VINDEX/VSEARCH。
不修改 current core_hnsw。
```

主要内容：

```text
include/vector/kvs_vector_hnsw_v2.h
src/vector/core/hnsw_v2/
  kvs_hnsw_v2_index.c
  kvs_hnsw_v2_storage.c
  kvs_hnsw_v2_context.c
  kvs_hnsw_v2_info.c
```

实现：

```text
init / reserve / reset / destroy / info
aligned vector storage
slot_to_record_id
record_id_to_slot
levels
deleted_flags
visited_tags
level0 graph placeholder
upper graph placeholder
search context placeholder
memory accounting
```

验收重点：

```text
1. index object 生命周期完整。
2. reserve 后内存区符合 capacity / dim / M / M0 / ef 参数。
3. vectors 指针满足 32-byte 或 64-byte alignment。
4. reset 不释放 capacity，但清空 size / entry_point / max_level / deleted / visited 状态。
5. info 能报告 memory breakdown。
6. v2 目录中无 manager 语义字段。
7. current core_hnsw 既有测试不受影响。
```

### Stage 9C：core_hnsw_v2 graph construction prototype

目标：

```text
实现 v2 add 和 graph construction。
不实现 search。
重点验证 graph 结构健康。
```

实现：

```text
random level
entry_point / max_level
slot 分配
vector 写入
level0 / upper graph adjacency
construction layer search
neighbor selection
bidirectional link
reverse edge pruning
duplicate edge 检查
graph quality stats
```

必须输出 graph stats：

```text
level distribution
max_level
entry_point level
level0 degree min/avg/p50/p95/max
upper degree distribution
duplicate edge count
reciprocal edge ratio
isolated / unreachable 近似统计，如实现成本可控
neighbor selection prune/reject count
```

验收重点：

```text
1. graph degree 不超 M/M0。
2. 不出现重复边。
3. level 分布合理。
4. entry_point / max_level 更新正确。
5. 构图过程可重复、可观测。
```

### Stage 9D：core_hnsw_v2 search and recall gate

目标：

```text
实现 v2 search。
用 core_flat exact ground truth 做 recall gate。
```

实现：

```text
ef_search search
candidate heap
top heap
visited tag
deleted skip
score polarity 统一
record_id + score 导出
miss-case analysis
```

召回率 gate：

```text
1K / 512d recall@10 >= 0.99
10K / 512d recall@10 >= 0.95
10K / 512d 目标 recall@10 >= 0.98
efSearch 增大时 recall 不下降
efConstruction 增大时 graph quality 不恶化
```

重要规则：

```text
如果 recall gate 不通过，不进入性能优化阶段。
先分析 graph construction / search semantics / score polarity / heap / visited 是否有 bug。
```

### Stage 9E：core_hnsw_v2 performance pass

前提：

```text
Stage 9D recall gate 已通过。
```

优化方向：

```text
shared AVX2 distance backend
metric-specialized fast path
search context pool
hot path no malloc/free
lightweight heap
visited tag 优化
memory layout 优化
bulk rebuild
必要时考虑 parallel build
```

性能 gate：

```text
1. build/search 不能数量级落后 FAISS/hnswlib。
2. 必须报告与 FAISS/hnswlib 的相对倍数。
3. 每次只优化一个明确瓶颈，不做无数据支撑的大改。
```

### Stage 9F：core_hnsw_v2 ops integration

目标：

```text
把 v2 接入现有 kvs_vector_index_ops_t。
不改 ops 签名。
不改 RDB/AOF。
不改 VSEARCH response。
```

实现：

```text
reserve
add
search
mark_delete
reset
info
destroy
VINDEX 显式选择 core_hnsw_v2
VINFO 输出 v2 stats
manager rebuild path 调用 v2 add
```

注意：

```text
Stage 9F 不把 v2 设为默认 backend。
只允许显式选择。
```

### Stage 9G：product hardening

目标：

```text
验证 v2 能作为生产候选 backend。
```

覆盖：

```text
VSET
VSEARCH
VDEL
VINDEX rebuild
VINFO
RDB load + rebuild
AOF replay + rebuild
TTL 过期
scope 隔离
tombstone rebuild policy
stress tests
rollback config
```

上线前要求：

```text
1. v2 recall / build / search / memory 数据稳定。
2. core_flat 可继续作为 exact baseline。
3. current core_hnsw 可作为 fallback。
4. RDB/AOF 无迁移成本。
5. 可以通过配置或 VINDEX 切回 core_flat / current core_hnsw。
```

## 6. v2 设计关键原则

### 6.1 graph quality 优先

v2 第一目标不是降低 `level0_us`，而是让 graph quality 稳定达标。

```text
先保证 10K / 512d recall。
再谈 SIMD、heap、layout、parallel。
```

### 6.2 score polarity 统一

内部必须明确：

```text
exported score 永远 larger-is-better。
IP / cosine：score 越大越好。
L2：内部可用 distance，但导出统一为 -l2。
```

以下模块必须使用一致的比较方向：

```text
candidate heap
top heap
neighbor selection
reverse pruning
lower_bound
result export
recall compare
```

### 6.3 HNSW 连边语义

v2 不应继承 current core_hnsw 中 “A->B 正向边依赖 B->A 反向接受” 的语义。

推荐语义：

```text
新节点 A 选择旧邻居 B：
  A->B 正向边保留。

旧邻居 B 是否接纳 A：
  B->A 尽力添加。
  如果 B 满，则按 neighbor selection / prune 规则决定替换或拒绝。
  B 拒绝 A 不反向取消 A->B。
```

但该语义应在 v2 graph construction 中通过 recall 和 graph stats 验证。

### 6.4 删除策略克制

第一版只做：

```text
mark_delete(record_id)
search skip deleted slot
tombstone ratio 超阈值后 rebuild
```

不做：

```text
physical compact
online edge rewrite
slot relocation
graph snapshot persistence
```

### 6.5 index-only 先行

v2 在 index-only 阶段完成前，不接入 VINDEX/VSEARCH。

顺序必须是：

```text
index object 生命周期
graph construction
search recall
性能优化
ops integration
产品硬化
```

## 7. 风险与回滚

主要风险：

```text
1. v2 graph construction recall 不达标。
2. score polarity 或 heap 语义出现隐蔽 bug。
3. memory layout 过早复杂化。
4. 过早接入产品路径导致调试困难。
5. 与 current core_hnsw 的历史改动混淆。
```

控制方式：

```text
1. v2 独立目录。
2. v2 初期 index-only。
3. current core_hnsw 保留。
4. core_flat 保留为 exact baseline。
5. FAISS/hnswlib 保留为外部 benchmark baseline。
6. v2 未通过 recall gate 前不接管默认 backend。
7. 失败时直接停止 v2 或回退到 current core_hnsw / core_flat，无需 RDB/AOF 迁移。
```

## 8. 当前下一步

下一步执行：

```text
Stage 9B: core_hnsw_v2 index-only skeleton
```

该阶段只建立 v2 的内存和生命周期底座，不实现 add/search，不接产品路径。

Stage 9B 完成后，再进入 Stage 9C 实现 graph construction。
