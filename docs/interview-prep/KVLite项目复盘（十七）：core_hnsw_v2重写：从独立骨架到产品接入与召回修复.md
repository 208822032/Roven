# 第十七章 core_hnsw_v2：从独立 ANN 内核到可验证默认 Backend

## 17.1 第一版 core_hnsw 已经独立，为什么还要重写

第十六章解决了向量模块的职责边界：record store 是 source of truth，manager 拥有 question、answer、scope、TTL、持久化和响应语义，vector core 只处理 `record_id + vector -> record_id + score`。第一版 `core_hnsw` 也已经证明，HNSW 可以脱离 `g_vector_state` 独立构建、搜索和回归。

但“独立”不等于“值得继续原地演进”。到 `a473bea`，第一版实现已经出现三类结构性信号：

```text
内存：upper graph 按 capacity * MAX_LEVEL * M 固定预分配；
构建：layer search、neighbor select、双向连接成为反复出现的热点；
正确性：10K/512d 召回低，局部修正 forward/reverse coupling 也只带来很小改善。
```

更关键的是，Stage 7I/7J 的 locality prototype 没有改善主热点，Stage 7K 又把它们删除；Stage 7E/7G 已经拿掉了较明显的重复计算和距离调用开销，Stage 7L 看到的仍是 dense level0 expansion 与 visited pressure。[`Stage 8A architecture audit`](../vector_core_reset/stage8a_core_flat_hnsw_faiss_parity_architecture_audit.md) 因此把后续路线从“继续加局部补丁”转成“冻结 v1，旁路实现 v2”。

这次重写的目标也需要说准确：

```text
不是重做 manager/core contract；
不是把 FAISS/hnswlib 搬进产品；
不是一开始就解决在线 rebuild；

而是在不破坏 v1 回归基线的前提下，
重做一套可独立构图、搜索、归因和调参的 HNSW core，
再通过产品 adapter 验证它能否接住 V 族命令。
```

本章唯一要回答的问题是：如何把进入局部优化低收益区的第一版 `core_hnsw`，替换为一个可独立验证、可接入产品，并在规模召回门禁下成为默认 ANN backend 的 `core_hnsw_v2`。

本章只复盘这条闭环。`2f1fc7f` 中虽然已经混入 manager rebuild skeleton、delta capture、shadow replay 和 pointer lifecycle 的阶段文件，但这些控制面尚未成为产品在线重建；快照缓存从下一提交 `15deef1` 才开始。本章不展开它们。

## 17.2 Git 范围、真实演进主线与核心源码导航

### Git 范围

第十六章结束在 `a473bea`。审计 `a473bea..7e89a93` 后确认：`a473bea` 的下一提交就是 `2f1fc7f`，中间没有遗漏的过渡提交；`7e89a93` 的下一提交 `15deef1` 已经进入 snapshot cache。

所以本章的真实范围是：

```text
起点：2f1fc7f
终点：7e89a93
Git inclusive range：2f1fc7f^..7e89a93
```

候选范围端点正确。需要修正的不是端点，而是对提交粒度的理解：四个提交都是聚合提交，不能把阶段报告中的每一个 Stage 伪装成独立 commit snapshot。

```text
2f1fc7f
→ 聚合 v1 后期审计、v2 设计、skeleton、构图、搜索、性能归因与 tombstone gate；
  Git 只能恢复聚合后的完整 v2，不能恢复 9B/9C/9D 的逐阶段源码快照。

07fdc6c
→ 聚合 9G0 experimental integration、cleanup、9G2 默认切换和 9G3 scale gate；
  同一提交内先实验接入，终态却已经切为默认 backend。

e016678
→ 增加 direct exact + core_flat oracle、参数敏感性和 FAISS direct parity harness；
  没有修改生产代码和默认参数。

7e89a93
→ 补 50K balanced FAISS parity、100K balanced gate，
  再把 v2 默认参数固化为 M=48、M0=96、efConstruction=512、efSearch=1024。
```

范围内还混有 v1 Stage 7 的审计、旧 profile harness 清理、路线文档整理，以及 9F rebuild control-plane skeleton。前者只作为重写动因，后者只作为下一阶段触发条件；它们不被包装成第十七章的产品能力。

### 核心源码导航

1. [`a473bea` 时的 v1 index 结构](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/a473bea/include/vector/kvs_vector_hnsw.h#L123-L166)
2. [`a473bea` 时的 fixed upper storage 分配](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/a473bea/src/vector/core/hnsw/kvs_hnsw_index.c#L123-L314)
3. [`2f1fc7f` 时的 v2 主结构与 API](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/include/vector/kvs_vector_hnsw_v2.h#L311-L400)
4. 当前仍保留的 [`core_hnsw_v2` 实现目录](../../src/vector/core/hnsw_v2/)
5. [`2f1fc7f` 时的 storage/reserve](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c#L108-L378)
6. [`2f1fc7f` 时的完整插入主链](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c#L1212-L1571)
7. [`2f1fc7f` 时的搜索主链](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c#L441-L572)
8. [`2f1fc7f` 时的 metric score contract](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_graph.c#L138-L155)
9. [`07fdc6c` 时的产品 adapter](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/07fdc6c/src/vector/kvs_vector.c#L1571-L1868)
10. 当前 [`v2 search recall test`](../../tests/vector_core_hnsw_v2_search_recall_test.c)
11. [`e016678` 时的 FAISS direct parity test](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/e016678/tests/vector_core_hnsw_faiss_parity_test.cpp)
12. [`100K balanced gate 报告`](../hnsw_productization/hnsw_p2_100k_balanced_default_report.md)

## 17.3 重写动因：一次局部修复为什么不够

第一版 `core_hnsw` 的问题不能概括成“性能差”。实际历史更复杂。

第一，内存布局有明确债务。v1 的 [`upper_neighbors`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/a473bea/src/vector/core/hnsw/kvs_hnsw_index.c#L159-L314) 为每个 slot 的每个可能 upper level 预留 `M` 个邻居，即使绝大多数节点只有 level 0，也承担固定空间。多条 per-slot 数组和搜索 context 又扩大了维护面。

第二，局部优化开始进入低收益区。Stage 7E 去掉 level0 worst-neighbor 的距离重算，Stage 7G 减少距离/SIMD 调用开销，但 Stage 7I/7J 的两个 locality prototype 没有改善主热点，Stage 7K 只能删除原型。继续在同一数据结构上加 mirror、counter 和局部缓存，成本越来越高，能解释的收益越来越小。

第三，召回问题不是一个开关就能修好。[`Stage 9A1 audit`](../vector_core_reset/stage9a1_core_hnsw_bidirectional_link_acceptance_audit.md) 证明 v1 确实把新节点 `A -> B` 的 forward edge 错误地依赖于 `B -> A` reverse acceptance；解除 coupling 后，10K/512d recall@10 只从 `0.6240` 到 `0.6380`。这是实 bug，但不是低召回主因。

因此工程判断不是“v1 一无是处”，而是：

```text
v1 继续作为 regression/reference；
不要在默认路径上边改结构边猜根因；
v2 从独立 index-only API 起步；
每一步先用 exact oracle 和规模 gate 证明，再接产品。
```

还必须保留一个反直觉事实：**v2 并没有立即消灭 fixed upper storage。** `2f1fc7f` 的 v2 reserve 仍然按 `capacity * (MAX_LEVEL - 1) * M` 分配 upper neighbors。重写首先改善的是实现隔离、插入/搜索路径、归因工具和参数验证能力，不是一次完成所有 layout 目标。

## 17.4 最初 skeleton：接口存在不等于 HNSW 可用

[`Stage 9A design boundary`](../vector_core_reset/stage9a_core_hnsw_v2_design_boundary.md) 先冻结了边界：v2 不注册产品 backend，不访问 manager 状态，先走 index-only。随后 [`Stage 9B report`](../vector_core_reset/stage9b_core_hnsw_v2_index_only_skeleton.md) 只建立生命周期和存储骨架：

```text
init / reserve / reset / destroy / info
aligned vector storage
slot_to_record_id
record_id_to_slot
levels / deleted_flags / visited_tags
level0 / upper graph storage placeholder
search context placeholder
memory accounting
```

这时没有完成 `add` 和 `search`，更没有 VSET/VSEARCH 产品路径。Stage 9B 的价值是先把对象生命周期、容量、内存统计和 core ownership 固定下来，而不是宣称 HNSW 已经完成。

需要特别说明 Git 证据边界：`2f1fc7f` 一次性提交了 9B 到 9F 的聚合终态，所以不能用 `git show 2f1fc7f` 伪造“9B 当时 search 返回什么”。“9B 只有 skeleton”来自阶段报告；“聚合终态已经有 add/search”来自实际 commit 源码。两类证据不能混用。

[`v2 skeleton test`](../../tests/vector_core_hnsw_v2_skeleton_test.c) 覆盖的是参数保存、aligned allocation、各数组存在、memory accounting、reset、重复 reserve 和 destroy。它证明 lifecycle contract，不证明 graph construction、recall 或产品行为。

## 17.5 数据布局：stable record_id 与 internal slot 为什么必须分开

`2f1fc7f` 的 [`kvs_hnsw_v2_index_t`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/include/vector/kvs_vector_hnsw_v2.h#L311-L354) 把状态分为几组：

```text
参数：dim / metric / M / M0 / ef_construction / ef_search
向量：aligned vectors + vector_stride
身份：slot_to_record_id + record_id_to_slot
图：levels + level0 neighbors/counts + upper offsets/neighbors/counts
导航：entry_point + has_entry + max_level
状态：deleted_flags + visited_tags + current_visit_tag
工作区：context_pool + candidate/top/scratch
观测：memory / graph / build / search stats
```

这里有两种 slot，不能混为一谈：

```text
manager record_index
→ record store 的物理槽，可在 VDEL 后复用；

v2 internal slot
→ graph 数组下标，由 v2 append 分配，不等于 manager slot。
```

稳定身份是 `record_id`。产品 adapter 收到旧 ops 的 `record_index` 后，先由 manager 取出 active record 的 `record_id`，再调用 v2 add；search 返回 `record_id + score`，manager 再用自己的 `record_id_map` 回表并检查 `record->id == hit.record_id`。这样 manager slot 被复用后，旧 hit 不会误解析成另一个新 record。

v2 core 自己也保留 `record_id_to_slot`，用途是 tombstone、重复 id 检查和 re-add 定位。这个 map 是 **index-local state**，不是 manager 的 `record_id_map`，也不会进入 RDB/AOF。

向量和 graph 都由 v2 index 拥有；record store 仍是真值。graph 损坏、参数改变或 tombstone 比例过高时，正确恢复路径是 manager 扫 active records 后 reset/reserve/add，而不是让 core 读取 question、scope 或持久化记录。

## 17.6 距离与 score contract：内部不是统一的“距离越小越好”

v2 实际比较的是 **score**，统一约定为越大越好：

```text
IP      -> dot(lhs, rhs)
L2      -> -squared_l2(lhs, rhs)
cosine  -> cosine_score(lhs, rhs)
```

代码证据是 [`kvs_hnsw_v2_metric_score()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_graph.c#L138-L155)。candidate max-heap 弹出当前最好 score，bounded top heap 的根是当前最差入选项；最终结果按 score 降序导出。相同 score 时，[`heap tie-break`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_heap.h#L8-L32) 使用更小 internal slot 保持确定性。

这和 `core_flat` 的 exact oracle 使用同一 metric polarity，才能计算 ANN recall@K。

但不能省略三个边界：

1. v2 不在 core 内做 L2 normalization。产品主路径使用上游归一化 embedding + IP；测试中的 synthetic base/query 也由 harness 显式归一化。
2. cosine 会在距离函数内计算 norm；IP 只有向量已经归一化时才可解释为 cosine similarity。
3. 该阶段 add/search 检查维度相等，却没有显式拒绝 NaN/Inf。当前证据只覆盖有限、可控的 float 输入。

所以“支持 IP/L2/cosine”是实现事实；“所有异常浮点输入都有完整 contract”尚未证明。

## 17.7 从空图到完整插入：v2 实际做了哪些步骤

[`Stage 9C report`](../vector_core_reset/stage9c_core_hnsw_v2_graph_construction.md) 把图构建补进 skeleton；后续 9E 又把全候选构建改成 HNSW-style bounded layer search。聚合终态的插入链是：

```text
kvs_hnsw_v2_index_add(record_id, vector, dim)
→ 检查 active duplicate；deleted id 才允许 re-add
→ 分配 append-only internal slot
→ random_level，概率阈值 1 / M，最大 31
→ 写 aligned vector / record_id / level / map

若为空图：
  → 该 slot 成为 entry_point，更新 max_level

否则：
  → 从旧 entry_point 开始
  → 对高于新节点 level 的 upper layers 做 greedy descent
  → 从 min(old_max_level, new_level) 逐层向下
  → bounded efConstruction layer search
  → heuristic neighbor_select
  → 写 new -> selected forward edges
  → best-effort 写 selected -> new reverse edges；满时 prune
  → 若新 level 更高，更新 entry_point / max_level
```

随机层生成见 [`kvs_hnsw_v2_random_level()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_random.c#L47-L69)，完整 connect/add 见 [`kvs_hnsw_v2_connect_new_node()` 与 `index_add()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c#L1251-L1571)。

v2 与 v1 的一个重要语义差异是 forward edge 不再依赖 reverse acceptance。新节点已经选择 `B` 时，先独立追加 `A -> B`，再尝试 `B -> A`；reverse side 满了可以 prune 或拒绝，但不会撤销 forward edge。这吸收了 Stage 9A1 发现的 coupling 问题。

不过“完整插入流程”不等于“完整失败事务”。聚合终态在 connect 中途失败时会恢复 id map 和新 slot 元数据，却没有完整回滚此前写入旧节点的 reverse edges。测试没有注入每一个分配/连接失败点。它属于当前失败清理缺口，不能写成强异常安全。

## 17.8 搜索：upper greedy、level0 efSearch 与两个堆

搜索链见 [`kvs_hnsw_v2_index_search()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c#L441-L572)：

```text
entry = entry_point
→ 从 max_level 到 level 1 做 greedy descent
→ 在 level 0 以 effective_ef 展开候选
→ candidate max-heap：下一步优先扩展分数最高节点
→ top min-heap：堆根保存当前最差入选 score
→ visited tag 去重，不为每次 query 清整张 bitmap
→ top 已满且 best candidate.score < lower_bound 时停止
→ top 降序排序
→ 导出未删除 slot 的 record_id + score
```

`effective_ef` 的规则是：调用方传 0 就用 index 默认；小于 topK 时提升到 topK；大于当前 size 时截断。`efSearch` 越大，通常探索更多节点、召回更高，但距离计算、visited 数和延迟也会上升。

为什么需要两个相反排序的堆？

```text
candidate 需要快速拿“当前最好、最值得继续扩展”的点；
top 需要 O(1) 看“当前最差入选点”，用于替换和 early stop。
```

deleted node 的语义也必须说准确：level0 中 deleted node 仍会进入 candidate heap并参与导航，但不会进入 top result；upper greedy 也没有按 deleted flag 排除邻居。最终导出再次过滤 deleted slot，manager 回表后还会检查 active/deleted/scope/threshold。这是 logical tombstone，不是物理删边。

[`v2 search recall test`](../../tests/vector_core_hnsw_v2_search_recall_test.c) 同时检查空图、维度错误、排序、删除过滤、exact direct scan recall 和 ef sensitivity。它的 ground truth 是 synthetic vector 最近邻，不是语义 qrels。

## 17.9 邻居选择与性能返工：不是一次实现就结束

v2 的演进最能说明为什么算法工程不能写成“照论文实现成功”。

### 第一版可搜，但构建方式不可持续

[`Stage 9D report`](../vector_core_reset/stage9d_core_hnsw_v2_search_recall_gate.md) 记录了 IP、512d、topK=10、M=32、M0=64、efConstruction=128 下的初始闭环：1K/5K recall@10 为 `1.0000`，10K 在 efSearch=256 时 `0.9840`、efSearch=512 时 `1.0000`；但 10K build 已接近 130 秒。这里证明的是搜索正确性下限，不是构建成本可接受。

[`Stage 9E0 attribution`](../vector_core_reset/stage9e0_core_hnsw_v2_performance_attribution.md) 进一步发现，第一版构图会为插入节点扫描过多 full candidates，10K 产生约 11.18 亿次 build distance compute，neighbor select/reverse prune 是主要成本。

### HNSW-style layer search 加速后，召回反而小幅退化

[`Stage 9E1 report`](../vector_core_reset/stage9e1_core_hnsw_v2_add_time_layer_search.md) 把 full-candidate construction 换成 bounded add-time layer search：10K build 从约 `129.1s` 降到 `48.1s`，但 ef=256 recall 从 `0.9840` 退到 `0.9780`，没有通过目标 gate。

这次转折不能抹掉。更快的 candidate construction 改变了候选质量，必须重新归因。

### 诊断没有支持“neighbor heuristic 已经坏了”

[`Stage 9E1a attribution`](../vector_core_reset/stage9e1a_core_hnsw_v2_candidate_quality_attribution.md) 用 sampled exact candidates 检查 entry quality、candidate overlap 和 reverse prune：level0 exact best 能被访问，top-M0 overlap 约 `0.9685`，neighbor-select overlap 约 `98.84%`；主要信号是 level0 entry 偏弱和 efConstruction 不足。

参数实验也不是单调神话：

```text
efConstruction=128 -> recall@10 0.9780
efConstruction=256 -> recall@10 0.9860
efConstruction=512 -> recall@10 0.9840
```

所以阶段决策是把当时默认 efConstruction 提到 256，而不是声称“参数越大一定越正确”。

### 堆优化有效，reverse prune 归因后对 cache 止损

[`Stage 9E2 report`](../vector_core_reset/stage9e2_core_hnsw_v2_bounded_heap_optimization.md) 用 candidate max-heap 与 bounded top min-heap 代替 `O(ef)` 数组维护，保持 10K ef=256 recall@10 `0.9860`。随后 [`Stage 9E3 distance attribution`](../vector_core_reset/stage9e3_core_hnsw_v2_distance_simd_attribution.md) 把 residual build cost 收敛到 reverse prune 中反复执行的距离计算；[`Stage 9E3b gate`](../vector_core_reset/stage9e3b_reverse_prune_attribution_rebuild_strategy_gate.md) 的 sampled attribution 只证明“存在复用机会”，同时把 background shadow rebuild 留作设计方向，它本身没有实现产品在线 rebuild。

[`Stage 9E3c report`](../vector_core_reset/stage9e3c_core_hnsw_v2_reverse_prune_score_reuse.md) 随后复用 direct known score，但 local pair cache 命中率只有约 `0.29%`；扩大 cache 的变体虽然保存了更多 score，却让 build 变慢，因此未保留。这个阶段留下的是窄范围 score reuse 和明确止损，不是继续扩大 graph-side cache。

这条真实路线是：

```text
先构建可用图
→ full candidate 成本失控
→ bounded layer search 加速但 recall 回退
→ 诊断 candidate quality
→ 调整 efConstruction
→ 用 heap 降低维护复杂度
→ 只保留有效的 score reuse，撤回低收益 cache
```

## 17.10 删除、更新与生命周期：tombstone 不是 reconnect

Stage 9F2 在产品接入前补了 [`kvs_hnsw_v2_index_mark_delete()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c#L456-L484)：

```text
record_id -> v2 internal slot
→ 校验 slot 范围和 slot_to_record_id
→ deleted_flags[slot] = 1
→ deleted_count++
```

重复 mark-delete 幂等；id map 和 graph edge 都保留。这样删除节点还能作为导航中间点，但不能出现在结果中。

更新不是原位改 vector。active duplicate `record_id` 会被拒绝；先 tombstone 后 re-add 才会分配新 slot并让 map 指向新 slot。[`tombstone test`](../../tests/vector_core_hnsw_v2_tombstone_test.c) 验证 old slot 保持 deleted，新 slot active，搜索返回 re-add 后的向量。

产品同 question+scope 更新时，adapter 先 tombstone 旧 record id，再让 manager 接收 prepared fields、更新 manager record-id map，最后把新 id/vector add 进 v2。历史实现见 [`core_hnsw_v2 update path`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/07fdc6c/src/vector/kvs_vector.c#L3660-L3744)。

因此当前边界是：

```text
删除 = logical tombstone，不物理断边；
更新 = old tombstone + append new slot，不原位 reconnect；
internal slot 不复用；
size 包含 tombstone，active_count = size - deleted_count；
reset/rebuild 才真正清掉 tombstone。
```

这会带来容量和图质量压力，也解释了为什么高 tombstone ratio 最终仍需要 rebuild。

## 17.11 从 experimental adapter 到默认产品路径

### 先纠正一个 Git 粒度陷阱

[`Stage 9G0 report`](../vector_core_reset/stage9g0_experimental_core_hnsw_v2_product_integration.md) 记录的中间态是：

```text
vector_index_type=core_hnsw_v2
+ vector_hnsw_v2_experimental_enabled=yes
→ 才启用 v2；默认关闭，未启用时回退 v1。
```

但 `07fdc6c` 同时聚合了 9G2。可恢复的 commit 终态已经把默认 `vector_index_type` 改为 `core_hnsw_v2`，experimental flag 只保留为 telemetry，不再 gate 选择；init 失败采取 fail-fast，不静默回退。因此准确叙述是：**产品接入先经过 experimental 中间态，随后在同一个聚合提交内完成默认切换。** 不能把 9G0 报告当成 `07fdc6c` 最终源码。

### 产品 adapter 做什么

[`g_kvs_vector_core_hnsw_v2_ops`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/07fdc6c/src/vector/kvs_vector.c#L1571-L1868) 沿用既有 `kvs_vector_index_ops_t`，没有修改公开签名：

```text
VSET new record
→ manager commit record
→ adapter 从 record_index 取 stable record_id
→ v2 add(record_id, embedding)

VSEARCH
→ embedding prepare
→ adapter 计算 raw_topk：max(topK*4, topK+32)，受 index size 限制
→ v2 search(record_id + score)
→ manager resolve_hits
→ active/deleted/id/scope/threshold 过滤
→ response assembly

VDEL
→ manager 删除 record
→ v2 mark_delete(record_id)

VINDEX REBUILD
→ reset v2
→ reserve active record count
→ manager 扫 active embeddings
→ v2 add

VINFO
→ size/capacity/M/M0/ef/deleted/active/memory/ready
```

[`kvs_vector_index_bind_current()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/07fdc6c/src/vector/kvs_vector.c#L1939-L1962) 让 `current_index` 真正绑定 v2 ops。`core_flat` 仍可显式选择；在本章历史终点，v1 `core_hnsw` 也仍是显式 fallback/reference。这里的“fallback”是可配置替代路径，不是默认 v2 失败后静默切换。

中间的 [`Stage 9G1 cleanup`](../vector_core_reset/stage9g1_cleanup_codebase_consolidation.md) 只清理过时 profile harness、整理 gate，并把 v1 降为 legacy reference；它没有切默认，也没有新增 ANN 语义。默认切换属于随后 [`Stage 9G2`](../vector_core_reset/stage9g2_default_backend_switch_hardening.md)，不能把 cleanup 写成独立产品能力。

RDB/AOF 没有保存 v2 graph。产品测试启动真实 server，经 RESP 执行 VSET/VDEL/VSEARCH/VINDEX/VINFO，再做 RDB reload 和 AOF replay；恢复后从 records 重建 v2。[`product integration script`](../../tests/vector_core_hnsw_v2_product_integration_test.sh) 是脚本资产，[`Stage 9G0 report`](../vector_core_reset/stage9g0_experimental_core_hnsw_v2_product_integration.md) 记录其当时提权后通过。graph 仍是 derived runtime state。

## 17.12 召回问题如何暴露：先确认跑的真是 v2

产品接入之后，第一轮 10K gate 看起来健康，但 9G3 把规模扩到 50K/100K 时暴露明显退化。[`Stage 9G3 report`](../vector_core_reset/stage9g3_scale_stability_final_consolidation.md) 的口径是 synthetic、512d、unit-normalized、IP、topK=10：

```text
10K / 50 queries：ef=256 recall@10 0.9860，ef=512 1.0000
50K / 20 queries：ef=256 0.6650，ef=512 0.8600
100K / 5 queries：ef=256 0.5400，ef=512 0.7200
```

这组结果首先证明“10K 不能外推 100K”，没有证明图算法必然错误。50K 只有 20 queries，100K 只有 5 queries，后两者只能作为风险信号，不是 SLA。

定位时需要依次排除：

1. **是否选中 v2。** [`default backend test`](../../tests/vector_core_hnsw_v2_default_backend_test.c) 直接检查未配置时 `index_type == core_hnsw_v2`；不能让 fallback 掩盖失败。
2. **metric/normalization 是否一致。** dataset 和 query 都由 harness unit-normalize，v2 与 exact 都用 IP、larger-is-better。
3. **oracle 是否可信。** 后续 P1 同时跑 direct exact scan 与 `core_flat`，按 record_id 检查 topK，mismatch 为 0。
4. **是否是 manager resolve/scope overfetch。** 规模 gate 和 parity 都直接调用 v2 index-only API，不经过 manager、scope、embedding 或 RESP，因此低 recall 不来自回表过滤。
5. **ef sensitivity 是否合理。** recall 随 efSearch 单调上升，说明搜索探索不足至少是一个有效因素。

所以问题被收敛成：默认 M/M0/efConstruction/efSearch 是否覆盖目标规模，以及同参下 v2 graph quality 是否明显落后成熟实现。

[`HNSW-P0 attribution review`](../hnsw_productization/hnsw_p0_recall_attribution_report.md) 在这里承担的是“先停手、再归因”：它没有修改生产代码，而是把 50K/100K 结果降级为风险证据，明确结构合法不等于导航质量足够，并要求先统一 exact oracle、query 数和成熟库 parity，再决定调参还是改图。这一步避免了在证据不足时继续做 SIMD、layout 或 cache 优化。

## 17.13 参数归因与 FAISS parity：召回修复证明了什么

### P1：把小样本风险变成 200-query 证据

[`HNSW-P1 report`](../hnsw_productization/hnsw_p1_parity_baseline_report.md) 用同一 fixed-seed dataset/query、direct exact scan 和 `core_flat` cross-check 重跑 10K/50K。50K baseline 参数是 M=32、M0=64、efConstruction=256：

```text
efSearch=256  -> recall@10 0.6620
efSearch=512  -> recall@10 0.8690
efSearch=1024 -> recall@10 0.9775
```

换成 balanced M=48、M0=96、efConstruction=512 后，50K efSearch=1024 达 `0.9975`。参数不足得到基本证明，但代价同样明确：报告中的 50K balanced build 约为 baseline 的 `2.76x`，memory 从约 320MB 增到约 426MB。

P1 还尝试 hnswlib external 分支，但 50K build 超出默认 quick 预算后中断，没有形成可采纳结果。因此本章不能写“v2 已和 hnswlib 完成同参 parity”。

### P1b：直接调用 FAISS，而不是绕产品 adapter

[`HNSW-P1b report`](../hnsw_productization/hnsw_p1b_faiss_parity_report.md) 新增 test-only C++ harness：

```cpp
faiss::IndexHNSWFlat(dim, M, faiss::METRIC_INNER_PRODUCT)
index.hnsw.efConstruction = efConstruction;
index.hnsw.efSearch = efSearch;
```

它不走 KVLite 产品 adapter，目的就是保证 FAISS 的 efSearch 真正进入运行路径。在 50K baseline、efSearch=1024 下，v2 recall@10 为 `0.9775`，FAISS 为 `0.9635`；没有观察到“成熟库同参显著高于 v2”。这不能证明两个实现算法等价，只能反驳“低 recall 必然来自 v2 图质量明显落后”的简单判断。

[`balanced FAISS parity report`](../hnsw_productization/hnsw_p1b_balanced_faiss_parity_report.md) 再补 50K balanced long-run：v2 `0.9975`，FAISS `0.9945`，但 v2 build 约为 FAISS 的 `4.31x`。召回 parity 基本通过，构建成本成为更突出的问题。

### P2：固化的是阶段默认，不是生产 SLA

[`100K balanced gate`](../hnsw_productization/hnsw_p2_100k_balanced_default_report.md) 使用 100K synthetic unit vectors、512d、100 queries、IP、topK=10：

```text
M=48
M0=96
efConstruction=512
efSearch=1024
recall@10=0.9740
direct exact vs core_flat mismatch=0
duplicate_edge=0
degree_overflow=0
```

因此 `7e89a93` 把 [`v2 defaults`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7e89a93/include/vector/kvs_vector_hnsw_v2.h#L10-L16) 从 `32/64/256/256` 固化到 `48/96/512/1024`。产品 configure path 会把低于 v2 default 的 shared config 值提升到这个下限，参数确实进入运行时。这里的 balanced defaults 是当前 100K synthetic gate 下的阶段性取舍，不是所有规模、数据分布和延迟目标下的全场景最优参数。

但同一报告记录 build 约 50.2 分钟、memory 约 851MB。它证明的是：

```text
已经证明：固定 synthetic/IP 口径下，balanced profile 通过单次 100K recall gate；
基本证明：前序低 recall 的主要因素是默认参数覆盖不足，而非已观察到的 FAISS graph-quality gap；
暂时观察到：高参数的构建和内存成本很重；
尚未证明：多 seed、真实 embedding、语义 qrels、产品 e2e、长期 SLA 和在线 rebuild。
```

“参数调高后 recall 恢复”不等于“算法和产品化都结束”。

## 17.14 测试、benchmark 与证据边界

本章修订没有在当前环境重跑历史长测。本次只允许修改现有文章，而重新执行 50K/100K benchmark 既会生成新产物，也不能替代目标提交当时的历史证据。以下“PASS”都来自对应阶段报告，不是本次执行结果。

| 证据 | 资产状态 | 历史执行证据 | 能证明 | 不能证明 |
| --- | --- | --- | --- | --- |
| [`skeleton test`](../../tests/vector_core_hnsw_v2_skeleton_test.sh) | 当前存在 | [`Stage 9B`](../vector_core_reset/stage9b_core_hnsw_v2_index_only_skeleton.md) 记录 PASS | lifecycle、reserve/reset、memory accounting | add/search/recall |
| [`graph construction test`](../../tests/vector_core_hnsw_v2_graph_construction_test.sh) | 当前存在 | [`Stage 9C`](../vector_core_reset/stage9c_core_hnsw_v2_graph_construction.md) 记录 PASS | entry/max level、degree、重复边/越界检查 | 大规模召回 |
| [`search recall test`](../../tests/vector_core_hnsw_v2_search_recall_test.sh) | 当前存在 | 9D/9E/9G 报告多次记录 PASS | direct exact recall、ef sensitivity、排序 | 语义相关性、产品 e2e |
| [`tombstone test`](../../tests/vector_core_hnsw_v2_tombstone_test.sh) | 当前存在 | [`Stage 9F2`](../vector_core_reset/stage9f2_core_hnsw_v2_mark_delete_tombstone_gate.md) 记录 PASS | 幂等 tombstone、re-add、deleted result filter | 物理删边、长期 tombstone 图质量 |
| [`product integration test`](../../tests/vector_core_hnsw_v2_product_integration_test.sh) | 当前存在 | [`Stage 9G0`](../vector_core_reset/stage9g0_experimental_core_hnsw_v2_product_integration.md) 记录提权后 PASS | VSET/VDEL/VSEARCH/VINDEX/VINFO、RDB/AOF 恢复 | 高并发、规模 SLA |
| [`default backend test`](../../tests/vector_core_hnsw_v2_default_backend_test.sh) | 当前存在 | [`Stage 9G2`](../vector_core_reset/stage9g2_default_backend_switch_hardening.md) 记录 PASS | 默认 v2、显式 flat/v1 路径 | init failure 的所有注入点 |
| [`scale stability test`](../../tests/vector_core_hnsw_v2_scale_stability_test.sh) | 当前存在 | [`Stage 9G3`](../vector_core_reset/stage9g3_scale_stability_final_consolidation.md) 记录 PASS | 10K full、50K sampled、100K smoke 风险 | 50K/100K SLA |
| [`P1 parity test`](../../tests/vector_core_hnsw_v2_parity_baseline_test.sh) | 当前存在 | [`P1 report`](../hnsw_productization/hnsw_p1_parity_baseline_report.md) 记录 PASS | direct exact/core_flat 对齐、参数敏感性 | 完整 hnswlib parity |
| [`e016678` 时的 FAISS parity test](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/e016678/tests/vector_core_hnsw_faiss_parity_test.sh) | 历史树存在，当前已清理 | [`P1b`](../hnsw_productization/hnsw_p1b_faiss_parity_report.md) / [`P1b balanced`](../hnsw_productization/hnsw_p1b_balanced_faiss_parity_report.md) 报告记录 PASS | 同 dataset/query 与显式 FAISS 参数 | 参数语义完全等价、产品 endpoint 对比 |
| [`100K balanced gate`](../../tests/vector_core_hnsw_v2_100k_balanced_gate_test.sh) | 当前存在 | [`P2 report`](../hnsw_productization/hnsw_p2_100k_balanced_default_report.md) 记录 PASS | 单次 100K synthetic recall 下限 | 多 seed、真实文本、真实 ONNX、SLA |

Git 树中没有找到这四个候选提交对应的独立 `results/*.json`、`*.log` 或 `*.csv`。所以本章所有历史数字都链接到明确记录执行口径的阶段报告；没有把测试文件存在本身写成“本次通过”，也没有构造不存在的原始结果链接。

还要严格区分三层 benchmark：

```text
index-only v2/flat/FAISS
→ 直接输入 float vector，测 build/search/recall；

product integration
→ 走 VSET/VSEARCH/manager/RESP/RDB/AOF，数据规模很小，主要验语义；

真实 embedding / 真实文本
→ 本章没有完成，不得用 synthetic 100K 替代。
```

100K balanced 的 memory 是 v2 自己按已分配数组统计的 index memory；FAISS parity 报告将 FAISS memory 标为 `N/A`。不能用 v2 measured memory 与不存在的 FAISS memory 数字做比较。

## 17.15 工程取舍、失败与当前缺陷

### 为什么不继续优化 v1

继续修补 v1 的优点是改动小、产品路径已有；缺点是 fixed upper layout、热点、召回和大量历史 profiling 状态纠缠在一起。多个 locality prototype 已失败，forward-independent 修复也没有解释低召回。旁路 v2 可以冻结回归参考，并让 skeleton/build/search/recall 分阶段 gate。

### 为什么不直接采用 hnswlib/FAISS

外部库适合做 mature baseline，但产品 adapter 仍要处理 stable record_id、scope overfetch、manager resolve、tombstone、VINDEX、RDB/AOF 与可选依赖。直接替换不能消除这些 KVLite 语义，而且 FAISS 是 C++ 可选依赖。自研 v2 的工程价值是建立一套可解释、可归因的 C core；它不意味着外部库没有产品价值，也不意味着 v2 性能已经优于成熟库。

### 为什么先 experimental，再切默认

实验开关让 VSET/VDEL/VSEARCH/VINDEX/VINFO 和恢复路径先在不影响默认行为的情况下验证。9G0 通过后，9G2 才切默认，并保留显式 flat/v1 路径。合理的产品策略是“先可选验证，再切默认”；不合理的是把聚合提交终态反写成从第一天起就默认。

### 为什么先用高参数验证召回

低 recall 可能来自 benchmark、metric、oracle、搜索探索、构图参数或算法 bug。先提高 efSearch/M/M0/efConstruction并做 FAISS同口径 parity，可以判断“现有图是否具备达到目标召回的能力”。如果高参数仍不行，才有更强理由重写 graph construction；如果能恢复，则先记录性能代价，再决定参数 profile 和构建策略。

### 当前缺陷

本阶段终点仍有明确技术债务：

1. **upper storage 仍固定展开。** v2 没有完成预期中的 compact upper layout。
2. **reserve 不是增长式迁移。** [`2f1fc7f reserve`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/2f1fc7f/src/vector/core/hnsw_v2/kvs_hnsw_v2_index.c#L329-L378) 分配新 storage 后释放旧 storage，并把 size 置 0，没有复制已有向量和 graph；产品 adapter 的 auto-grow 因而存在丢失派生索引内容的边界。现有产品小测和预 reserve 的规模测试没有充分覆盖它。
3. **partial add rollback 不完整。** 连接中途失败可能已经改写旧节点 reverse edge，当前回滚主要恢复 map 与新 slot 元数据。
4. **删除和更新只做 tombstone + append。** 不物理 reconnect，slot 不复用，长期更新会推高 size/deleted_count。
5. **search context 并发边界未证明。** 聚合终态直接使用 context pool 的第一个 context；本章没有多 reader 并发安全证据。
6. **NaN/Inf contract 缺失。** 只有维度和对象状态检查。
7. **高召回参数代价很大。** 100K 单次 gate 的 build、内存和 search 探索量都明显增加；高 recall 不等于 build/search/memory 可接受。
8. **同步 rebuild/cold start 仍不可产品化。** records-only persistence 保证正确恢复，但大规模 graph 重建会阻塞很久。
9. **hnswlib parity 未闭环。** 有 harness 尝试，没有可采纳的同口径长跑结果。
10. **真实语义证据缺失。** 本章没有真实 ONNX/text dataset、qrels 或真实 endpoint 10K/100K。

第 8 项是后续路线的触发条件，但仍要服从真实 Git 顺序：`15deef1^..26ebf99` 先处理 snapshot、校验和冷启动，`74ec6af` 才进入在线 rebuild、active/shadow/retired、delta replay、reader publication 和后台 worker。本章两者都不展开。

### 方案比较

| 方案 | 优点 | 代价与结论 |
| --- | --- | --- |
| 继续修补 v1 | 产品路径成熟、改动小 | 局部优化收益下降，结构与历史状态复杂；不再作为主路线 |
| 旁路重写 v2 | 可以 index-only 分阶段验证，v1 可回归 | 重写成本高，初版仍保留 fixed upper 等债务；本阶段采用 |
| 直接采用 FAISS/hnswlib | 成熟实现、外部性能参照 | 依赖、adapter、持久化和 manager 语义仍需处理；保留为 baseline/可选路径 |
| 原地替换默认 backend | 迁移快 | 容易让 fallback 掩盖错误；先 experimental gate 后再切默认 |
| 低参数优先延迟 | build/search 较轻 | 50K/100K recall 明显不足；不能用低 recall 换性能结论 |
| 高参数先验正确性 | 能判断图是否有达到召回下限的能力 | build/memory/search 成本上升；只能作为归因和阶段默认依据 |

## 17.16 Agent 辅助开发边界

本章四个提交没有 `Co-Authored-By` trailer。Git 能证明提交作者、实际 diff、阶段报告和测试资产，不能证明每一行代码由谁手写，也不能仅凭代码风格推断 Agent 参与比例。

面试中可防守的表达应按责任划分：

```text
我提出的问题：
v1 局部优化收益下降后，必须区分参数不足、图质量和 benchmark 口径。

我设定的边界：
v2 只拥有 record_id/vector/score/internal slot/graph；
manager record slot、scope、TTL、record resolve 和 response 不进入 core。

我选择的验证方法：
先 skeleton，再构图，再搜索；
direct exact 与 core_flat 交叉验证；
FAISS direct 显式设置 efConstruction/efSearch；
index-only recall 与产品 integration 分开。

我接受的实现：
stable record_id、append-only graph slot、upper greedy + level0 ef search、
forward independent/reverse best-effort、tombstone re-add、experimental adapter。

我否决或撤回的实现：
继续扩大 v1 locality prototype；
用大 local pair cache 包装低命中收益；
在 oracle/parity 前直接判定 graph bug；
用低参数 recall 换性能结论。

Agent 可以辅助：
生成 skeleton、测试 harness、profiling counter 和阶段报告初稿。

我必须自己核验：
实际 backend 是否选中、参数是否进入运行路径、score polarity、
record_id/slot 安全、exact oracle、FAISS 口径、失败回滚和证据等级。
```

判断自己是否真正掌握，不看能否背报告，而看能否现场画出：

```text
manager record_index
→ active record_id
→ v2 internal slot / graph
→ search hit(record_id, score)
→ manager record_id_map
→ id/active/scope/threshold check
→ response
```

并解释删除、更新、rebuild 时每一层的真值和派生状态。

## 17.17 面试表达与本章小结

### 1 分钟版本

“第十六章已经把 vector manager 和 core 拆开，但第一版 `core_hnsw` 继续演进的收益开始下降：upper graph 固定预分配，构建热点集中在 layer search、neighbor select 和双向连接，两个 locality prototype 没有效果；[`Stage 9A1`](../vector_core_reset/stage9a1_core_hnsw_bidirectional_link_acceptance_audit.md) 记录的 10K/512d recall 只有 0.624，修掉 forward edge 依赖 reverse acceptance 后也只有 0.638。

所以我冻结 v1，旁路做 `core_hnsw_v2`。v2 先只做 index-only skeleton，再补随机层、entry point、upper greedy、level0 ef search、neighbor selection、forward/reverse link 和两个堆。它用 stable record_id 对外，internal slot 只属于 graph；删除是 tombstone，更新是 tombstone 后 append re-add。

接入产品时先做 experimental adapter，验证 VSET/VDEL/VSEARCH/VINDEX/VINFO 和 RDB/AOF records-only 恢复，随后同一个聚合提交切成默认 backend。50K/100K scale gate 又暴露低召回，我们没有直接重写算法，而是用 direct exact + core_flat oracle、FAISS direct 同参 parity 做归因。结果表明主要是默认参数覆盖不足；[`P2 gate`](../hnsw_productization/hnsw_p2_100k_balanced_default_report.md) 中 balanced 参数让单次 100K synthetic recall@10 达到 0.974，但 build 约 50 分钟，所以这只是可用下限，不是生产 SLA；snapshot/cold start 与在线 rebuild/safe publication 要由后续两章分别解决。”

### 3～5 分钟版本

“这次重写不是因为第一版 core 没有分层。v1 已经符合 `record_id + vector -> record_id + score`，问题是它的实现进入低收益区：fixed upper arrays、dense level0 expansion、visited pressure 和复杂的双向连接维护。Stage 7E/7G 去掉明显重复开销后，Stage 7I/7J 的 locality prototype 仍失败；[`Stage 9A1`](../vector_core_reset/stage9a1_core_hnsw_bidirectional_link_acceptance_audit.md) 又证明 forward/reverse coupling 是 bug，但解除后 10K recall 只从 0.624 到 0.638。继续原地改 v1 很难区分结构变化和回归，所以我把它冻结为 reference，另起 v2。

v2 第一步只是 skeleton：init/reserve/reset/destroy、aligned vector、record-id/slot map、level/deleted/visited、graph storage 和 memory info。接口存在不代表可以搜索。9C 才补构图，9D 才补搜索。插入时先生成随机层；空图设置 entry point；非空图从 upper level greedy descent，再逐层用 efConstruction 搜候选、用 diversity heuristic 选邻居，先写新节点 forward edge，再 best-effort 维护 reverse edge和 prune。搜索时 upper greedy 到 level0，candidate max-heap 负责拿最好扩展点，top min-heap 负责维护最差入选下界，visited tag 去重，满足 lower-bound 条件后停止。

身份边界上，manager record slot 和 graph slot 完全不同。manager slot 可以复用；v2 slot 是 append-only 下标。两边只用 stable record_id 对接。search 返回 id+score 后，manager 还要校验 id、active、deleted、scope、threshold，再组响应。删除只 mark tombstone，节点还能导航但不能导出；更新是 old tombstone + new slot，不是原位 reconnect。

性能演进也有返工。[`Stage 9D`](../vector_core_reset/stage9d_core_hnsw_v2_search_recall_gate.md) 的 full-candidate build 10K 接近 130 秒；[`Stage 9E1`](../vector_core_reset/stage9e1_core_hnsw_v2_add_time_layer_search.md) 改成 bounded layer search 后约 48 秒，但 recall 从 0.984 掉到 0.978。诊断显示 exact-best 能访问，neighbor heuristic overlap 也高，主要问题更像 entry quality 和 efConstruction。提到 256 后 recall 回到 0.986；后面用两个堆降低 `O(ef)` 维护，[`Stage 9E3c`](../vector_core_reset/stage9e3c_core_hnsw_v2_reverse_prune_score_reuse.md) 保留窄范围 score reuse；低开销 local pair cache 的命中率只有 0.29%，扩大 cache 的变体反而让 build 变慢，因此止损。

产品接入先用 experimental flag，adapter 把旧 ops 的 record_index 转 stable record_id，VSEARCH raw hit 再交 manager resolve。VINDEX 同步从 active records 重建，RDB/AOF 不保存 graph。9G2 后 v2 成为默认；这说明 experimental 是中间态，不是最终状态。

随后 50K/100K synthetic gate 暴露低 recall。我们先验证 default backend 真选中 v2，再用 direct exact 和 core_flat 交叉验证 oracle，确保 metric、normalization、topK 一致；由于 index-only 路径不经过 manager，也排除了 scope/resolve。P1 看到 efSearch 和 M/M0/efConstruction 提升能恢复召回，P1b 又直接设置 FAISS `efConstruction/efSearch`，同参下 FAISS 没有显著领先 v2。最后 [`P2 report`](../hnsw_productization/hnsw_p2_100k_balanced_default_report.md) 的 100K balanced 单次 gate recall@10=0.974，默认参数据此固化。

我不会把它讲成生产级完成。数据是 synthetic normalized IP，不是语义 qrels；原始 result 文件没有入库，数字来自阶段报告；[`P2 report`](../hnsw_productization/hnsw_p2_100k_balanced_default_report.md) 的 100K 是单 seed gate，build 约 50 分钟、memory 约 851MB；upper storage、reserve growth、partial rollback、tombstone 累积和并发 context 仍有缺口。下一章先处理 snapshot、校验和冷启动，再后一章才处理在线 rebuild、delta replay 和安全发布。”

### 常见追问与回答要点

**HNSW 插入流程是什么？**

随机生成新节点 level；从旧 entry point 在高于新 level 的层 greedy descent；从两者较小的最高层逐层做 efConstruction 搜索；neighbor select；写 forward edge；尝试 reverse edge并在满时 prune；若新 level 更高则更新 entry point/max level。

**为什么需要分层？**

upper layer 提供长距离导航，快速进入 query 附近；level0 保存更密邻接并做 efSearch 扩展。只在 level0 扫图容易随规模增长扩大 visited 和距离计算。

**M、M0、efConstruction、efSearch 分别影响什么？**

M 是 upper layer 邻居容量，M0 是 level0 容量；efConstruction 控制插入候选探索，主要影响图质量和 build；efSearch 控制查询探索，主要影响 recall 和 search cost。它们会共同影响内存、构建、搜索，不能脱离实现语义横比。

**candidate queue 和 top queue 为什么排序相反？**

candidate 要快速弹出最好扩展点，所以是 max-heap；top 要快速知道最差入选下界，所以是 min-heap。early stop 比较最好待扩展候选与最差入选 score。

**为什么 deleted 节点仍能参与导航？**

物理删边可能切断图连通性，成本也高。tombstone 保留图拓扑，让删除节点作为中间点，但不进入结果；manager 还会做最终 active/id 校验。代价是 tombstone 积累后需要 rebuild。

**为什么 record_id 不能直接用 slot？**

manager record slot 会被 VDEL 后复用，graph slot 又是另一套 append-only布局。stable record_id 才能防止 stale hit 在 slot 复用后解析成新 record。

**召回下降如何区分构图和搜索问题？**

先固定 dataset/query/metric/normalization/oracle；扫 efSearch 看搜索探索是否有效；再扫 M/M0/efConstruction；检查 candidate overlap、entry quality、degree/duplicate/overflow；最后与 FAISS direct 同参对照。不能只看一个 recall 数字。

**参数调高后 recall 变好，为什么仍不能算完成？**

高参数增加 edge、候选和距离计算。100K balanced 虽通过 recall gate，但 build、memory、search exploration 都明显增加；还没有真实 embedding、多 seed、端到端和在线 rebuild 证据。

**为什么不直接用 hnswlib？**

hnswlib/FAISS 是成熟 baseline，但 KVLite 仍需 manager adapter、stable id、scope/TTL过滤、删除、RDB/AOF 恢复和可选依赖策略。v2 的价值是可解释的 self-contained C core；是否最终采用外部库是另一项产品取舍。

**这一阶段最大的失败或返工是什么？**

一是 full-candidate 构建正确但太慢；二是 bounded layer search 加速后 recall 退化；三是较大的 reverse-prune cache 命中低且让 build 变慢；四是默认参数在 10K 好看，却在 50K/100K 暴露低 recall。

### 容易夸大的内容

不能说：

```text
v2 一开始就是完整 HNSW；
v2 已经消灭 fixed upper storage；
experimental flag 一直是最终产品状态；
100K recall 来自产品 RESP 或真实 embedding；
synthetic nearest-neighbor recall 等于语义相关性；
FAISS 同参不领先就证明两套算法完全等价；
参数调高后 build/search/memory 也已经解决；
tombstone 等于物理删除或完整 reconnect；
RDB/AOF 已经持久化 graph；
在线 rebuild、snapshot 和 active/shadow publication 已在本章完成。
```

### 本章小结

这一阶段真正完成的闭环是：

```text
v1 局部优化收益下降
→ 冻结 reference，建立 v2 index-only skeleton
→ 完成分层构图和 efSearch 搜索
→ 用 exact recall 暴露性能/召回返工
→ 形成 bounded heap、参数归因和低收益 cache 否决
→ 先 experimental 接产品，再切默认 backend
→ 规模 gate 暴露 50K/100K 低召回
→ direct exact + core_flat + FAISS parity 排除主要口径疑点
→ balanced 参数通过单次 100K synthetic recall gate并固化默认
```

已经证明的是：v2 能独立构图、搜索、tombstone、返回 stable record_id；产品 V 族命令和 records-only RDB/AOF 恢复能走 v2；在固定 synthetic/IP 口径下，通过合理参数可以达到阶段性召回下限。

仍未解决的是：compact upper layout、增长式 reserve、强失败回滚、长期 tombstone、并发 search context、真实 embedding/语义 benchmark，以及大规模同步 rebuild/cold-start 的产品可用性。

因此第十七章的终点不是“生产级 HNSW 完成”，而是“可验证的 v2 内核进入产品并达到召回可用下限”。下一篇是“第十八章：HNSW snapshot、校验与冷启动”，候选范围 `15deef1^..26ebf99`；从 `74ec6af` 才开始的在线 rebuild、delta replay 与安全发布属于第十九章，不在下一篇提前展开。
