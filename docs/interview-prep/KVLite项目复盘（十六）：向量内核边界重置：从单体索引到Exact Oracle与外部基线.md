# 第十六章 向量内核边界重置：从单体索引到 Exact Oracle 与外部基线

## 16.1 Route C 之后，为什么还要重置向量内核

第十五章解决的是“谁执行状态修改”。

Route C 把 embedding prepare 移到 worker，把 record、TTL、AOF/RDB、replication 和索引 mutation 收敛到 executor owner。到 `33a886f`，执行链路和状态所有权已经能用 prepare/commit、command task、response task 和 owner lock 解释。

但“谁来执行”清楚，不等于“执行的向量模块内部边界”清楚。

当时的 record store、flat、HNSW、topK、scope 过滤和 backend dispatch 仍然集中在一个大模块里。外部 FAISS benchmark 又是另一条路径。于是项目面临的下一组问题变成：

```text
旧 HNSW 慢或 recall 不稳，究竟是算法、实现还是 benchmark 口径？
flat 是产品 fallback，还是可以独立验证的 exact oracle？
FAISS / hnswlib 的数字能不能和产品 VSEARCH 直接比较？
record、scope、TTL、question/answer 应该属于 manager，还是 ANN index？
RDB/AOF 恢复的真值到底是 record，还是 HNSW graph？
```

因此本章不再展开 ONNX、Prepare / Commit、executor、worker pool 或 weak-lock。它只承接一个前提：Route C 已经把状态修改的 owner 收住；接下来要把 **KV manager 语义** 与 **vector core 计算** 拆开，并建立可以相互校验的内部 exact baseline 和外部 ANN baseline。

本章的结论也不是“新增了几个 backend”，而是：

```text
record store 成为 source of truth；
ANN index 退回为可重建的派生状态；
manager 保留业务语义；
core 只接受 record_id + vector，只返回 record_id + score；
产品命令、内部 exact oracle 和外部 baseline 开始可以放在同一证据框架里比较。
```

## 16.2 Git 范围、真实演进主线与核心源码导航

### Git 范围

第十五章结束在 `33a886f`。审计 `33a886f..a473bea` 后，没有发现夹在 `33a886f` 与 `f61d64d` 之间、应提前纳入本章的过渡提交；`a473bea` 的下一提交 `2f1fc7f` 已明确开始 `core_hnsw_v2`。

所以本章的真实端点是：

```text
起点：f61d64d
终点：a473bea
Git inclusive range：f61d64d^..a473bea
```

候选写法 `f61d64d..a473bea` 会排除起点提交本身。因此端点不变，但正文统一使用“inclusive `f61d64d` through `a473bea`”，命令范围写作 `f61d64d^..a473bea`。

九个提交不是九个孤立功能，而是一条连续路线：

```text
f61d64d
→ 接入可选 FAISS C ABI、standalone baseline 和产品兼容适配器

8afe305
→ 接入 hnswlib backend、smoke 和产品兼容适配器

1f65e1b
→ 补第三方依赖 submodule 元数据；属于构建过渡

39eb0a2
→ 审计旧路径，区分语义存储、embedding 和 vector kernel；
  建立 index-only / server-path benchmark 分层

5b9c97a
→ 拆分 monolithic header，明确 manager / record / core contract

874f3ed
→ 实现 self-contained core_flat，接入 V 族命令并补 correctness/rebuild/RDB 测试

1810908
→ 实现第一版 self-contained core_hnsw，接入 manager 并补 correctness/rebuild/RDB/AOF 测试

8922145
→ 横向 benchmark；发现 FAISS/hnswlib scope overfetch 缺陷并重跑 corrected baseline

a473bea
→ manager resolve、VINFO/profile、rebuild attribution、距离后端和高维 harness 的集中硬化；
  同时确认第一版 core_hnsw 的内存、重建和高维热点仍未结束
```

范围内有两处必须排除的混合修改：`f61d64d` 同时清理了大量上一阶段结果文件，`39eb0a2` 同时迁移或删除了无关文档。本章不把这些清理当成向量内核能力，也不处理当前 worktree 中的后续 v2 修改。

### 核心源码导航

下面这些入口足以快速还原本章，不代表正文只做文件索引：

1. [`33a886f` 时的 monolithic vector state 与旧 ops](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/33a886f/include/kvs_vector.h#L424-L671)
2. [`5b9c97a` 时的 `record_id + vector -> hit` core contract](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/5b9c97a/include/vector/kvs_vector_core.h#L27-L115)
3. [`1810908` 时的 manager backend binding](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/src/vector/kvs_vector.c#L1211-L1313)
4. 当前仍保留的 [`core_flat`](../../src/vector/core/flat/)
5. [`1810908` 时的第一版 `core_hnsw` 状态与 API](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/include/vector/kvs_vector_hnsw.h#L59-L139)
6. [`8922145` 时的 FAISS compatibility adapter](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/8922145/src/vector/kvs_vector_faiss_ops.c#L43-L177)
7. [`8922145` 时的 hnswlib compatibility adapter](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/8922145/src/vector/kvs_vector_hnswlib_ops.c#L37-L170)
8. 当前仍保留的 [`index-only external benchmark runner`](../../scripts/benchmark/run_hnswlib_vs_faiss_benchmark.py)
9. 当前 [`core_flat correctness` 测试](../../tests/vector_core_flat_correctness_test.sh)
10. [`Stage 4B corrected benchmark 报告`](../vector_core_reset/stage4b_corrected_vector_core_benchmark_report.md)
11. [`Stage 7C 512d direct-vector 原始 JSON`](../../results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_512d.json)
12. [`1810908` 时的 RDB record save/load 与 rebuild](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/src/persistence/kvs_rdb.c#L134-L367)

## 16.3 旧实现的问题：不是没有抽象，而是抽象仍把 manager 语义带进 index

第九章已经讲过旧 flat、旧 HNSW 和 V 族命令的算法与业务语义。本章不重复 HNSW 原理，只看为什么旧结构不适合继续演进。

### 一个全局状态同时放了 record 和所有索引

在 `33a886f`，[`kvs_vector_state_t`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/33a886f/include/kvs_vector.h#L648-L671) 同时拥有：

```text
store
question+scope id_map
old flat_index
old ann_index
sq8_flat_index
current_index
embedding_provider
```

同一个头文件还定义 record、store、flat、ANN node、ANN graph、topK heap 和 index ops。物理上有 `kvs_vector_index_ops_t`，但其 [`add`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/33a886f/include/kvs_vector.h#L590-L616) 输入是 `record_index`，`search` 参数直接含 `scope` 和 `threshold`，输出是已经带 question/answer/scope 的 manager result。

这说明旧 ops 抽象的是“完整 KVLite vector backend”，不是“只做 ANN 的 vector core”。

### 旧 flat 和旧 HNSW 不能脱离 manager 独立验证

旧 flat 与 ANN 算法都在 [`kvs_vector_index.c`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/33a886f/src/vector/kvs_vector_index.c#L1459-L1850) 中。旧 ANN add/search 也在同一文件里，并直接依赖全局 metric、record slot、scope 和 record 状态：[`旧 ANN add/search`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/33a886f/src/vector/kvs_vector_index.c#L3147-L3390)。

这带来三个问题：

1. 算法正确性测试必须先初始化 `g_vector_state`、record 和命令环境，难以只给一组向量验证图。
2. backend 返回的是 manager result，无法判断错误发生在 ANN candidate、record 回表、scope 过滤还是 response assembly。
3. `record_index` 是可复用 slot，不是稳定身份；旧 hit 如果跨删除/复用存活，存在误解析到新 record 的风险。

### 产品、standalone benchmark 和外部库最初不是同一路径

最早的 [`FAISS benchmark plan review`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/39eb0a2/docs/vector_native_faiss_optimization/00_benchmark_plan_review.md) 结论是 `NOT_APPROVED`。原因不是 FAISS 不能运行，而是当时混在一起的资产包括：

```text
KVLite TCP/RESP VSET/VSEARCH
进程内 vector API
standalone FAISS C++ API
随机 float 数据
hash text embedding
ONNX 专项实验
历史结果文件
```

这些路径的数据、预处理、参数和计时边界不同。看到 standalone FAISS 更快，不能判断差距来自 index kernel、embedding、manager lookup、executor、RESP 还是网络。

### 为什么不能继续在原 manager 文件里补 HNSW

继续修补旧 ANN 可以局部减少 malloc、调整邻居或提高 ef，但无法回答“core hit 是否正确”“manager filter 是否正确”“外部参数是否对齐”。每次性能变化都可能混入业务层成本，每次 recall 变化也可能只是 scope 候选不足。

因此这次重构的必要性不是代码洁癖，而是验证闭环：

```text
没有独立 core
→ 无法隔离算法正确性；

没有 exact core oracle
→ 无法可靠计算 ANN recall；

没有外部 baseline
→ 无法判断自研实现是否偏离成熟实现数量级；

没有统一产品接入
→ index-only 通过也不能证明 VSET/VSEARCH 正确。
```

## 16.4 manager / record / vector core：边界如何形成

### 目标 contract

`5b9c97a` 拆头文件后，核心约束第一次在源码里变得清楚。[`kvs_vector_hit_t`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/5b9c97a/include/vector/kvs_vector_core.h#L27-L32) 只有：

```c
uint64_t record_id;
float score;
```

[`kvs_vector_core_ops_t`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/5b9c97a/include/vector/kvs_vector_core.h#L72-L115) 的语义则是：

```text
create / reset / reserve
add(record_id, vector)
mark_delete(record_id)
search(query, topk) -> record_id + score
info / destroy
```

core 不再接收 question、answer、scope、TTL 或 manager record slot，也不组装 RESP。

### 最终职责表

| 责任 | manager / record store | vector core |
| --- | --- | --- |
| question、answer、scope | 拥有 | 不可见 |
| TTL、active/deleted 业务语义 | 拥有并最终过滤 | 只有 index-local deleted flag |
| record_id 生成与 record slot 复用 | 拥有 | 只把 record_id 当稳定 label |
| threshold、scope 过滤 | 拥有 | 不处理 |
| response assembly | 拥有 | 不处理 |
| RDB/AOF/replication | 拥有 | 不参与序列化 |
| vector、metric、topK 搜索 | 组织调用 | 执行 |
| graph、visited、candidate heap | 不拥有 | 拥有 |
| rebuild orchestration | 扫描 active record 并驱动 | reset/reserve/add |

这个边界最重要的结果是：**record store 是 source of truth，所有 index 都是 derived state**。

[`RDB save`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/src/persistence/kvs_rdb.c#L134-L186) 保存 record id、question、answer、scope、embedding 和生命周期字段；[`RDB load`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/src/persistence/kvs_rdb.c#L194-L367) 恢复 record 后调用 `kvs_vector_rebuild_after_load()`。它没有序列化第一版 core_hnsw graph。

这不是“忘记保存图”，而是有意识地选择：

```text
RDB/AOF 保存业务真值；
load/replay 恢复 active records；
manager 再驱动 selected backend rebuild。
```

### 必须承认：这时仍是两层接口过渡，不是最终 ABI

干净的 `kvs_vector_core_ops_t` 已经形成，但产品 `current_index` 仍通过旧 `kvs_vector_index_ops_t` 分派。[`kvs_vector_index_bind_current()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/src/vector/kvs_vector.c#L1211-L1256) 在一串分支中绑定 legacy flat/HNSW、core_flat、core_hnsw、hnswlib 和 FAISS；core adapter 再把旧 `record_index` 转成 `record_id`，把 raw hits 交回 manager resolve。

所以准确说法是：

```text
本阶段建立了稳定的职责边界和 core contract；
产品路径仍保留 compatibility adapter；
它不是已经冻结的最终 backend ABI。
```

外部 adapter 仍有 manager/global-state 债务，也不能作为新 core 的模板。这个债务被隔离在 adapter，而不是重新进入 `src/vector/core`。

## 16.5 core_flat：它的价值不是“另一个慢 backend”

`874f3ed` 新增了独立的 flat storage、search、info、shared heap 和 `record_id -> slot` map。当前 HEAD 仍保留这些文件：[`kvs_flat_storage.c`](../../src/vector/core/flat/kvs_flat_storage.c)、[`kvs_flat_search.c`](../../src/vector/core/flat/kvs_flat_search.c) 和 [`kvs_vector_id_slot_map.c`](../../src/vector/core/kvs_vector_id_slot_map.c)。

### 数据与生命周期

[`kvs_flat_index_add()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/874f3ed/src/vector/core/flat/kvs_flat_storage.c#L18-L120) 使用稳定 `record_id`：已有 id 更新原 slot，新 id 分配 core-local slot；[`mark_delete`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/874f3ed/src/vector/core/flat/kvs_flat_storage.c#L122-L163) 只标记 index-local deleted flag。

它不保存 scope、question 或 answer。manager record 被删除后，core 内 stale slot 即使还存在，也只能返回原 record_id；manager 仍会做最终 active/deleted/id 一致性检查。

### 距离与 TopK

[`kvs_flat_index_search()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/874f3ed/src/vector/core/flat/kvs_flat_search.c#L10-L198) 的行为是：

```text
扫描所有未删除 slot
→ IP：dot product
→ L2：negative squared distance
→ cosine：dot / (norm(lhs) * norm(rhs))
→ 固定容量 topK min-heap
→ 降序导出 record_id + score
```

复杂度是 `O(N * dim + N log K)`，额外 TopK 空间是 `O(K)`。使用固定容量 min-heap 的原因很直接：没有必要保存并排序全部 N 个候选；堆顶始终是当前最差的入选项。

core_flat 自己不做 L2 normalize。文本 embedding provider 会在上游归一化；index-only 数据生成器也必须显式记录是否 normalize。不能把“IP 与 cosine 在归一化向量上等价”误写成“core_flat 自动归一化”。

### exact oracle 的准确含义

core_flat 是 **同一批向量、同一 metric 下的精确 nearest-neighbor oracle**。ANN recall@K 可以定义为：

```text
| ANN topK ∩ exact flat topK | / K
```

它不是语义 qrels。若文本 embedding 本身不能表达语义，flat 只能证明“距离计算与排序精确”，不能证明“答案与问题语义相关”。

为什么不只用 FAISS Flat 做 oracle？

1. core_flat 与 KVLite 使用同一 `record_id + score` contract，可在没有 C++/FAISS 依赖时运行。
2. 它能直接覆盖 manager adapter、删除、rebuild 和 RDB reload。
3. FAISS Flat 是第二个独立实现，可以反向校验 core_flat，而不是成为唯一真值来源。

Stage 4A 正是靠“两个 exact 实现理论上应一致”发现 benchmark 缺陷；如果只有一个 exact 实现，异常更容易被误判成 ANN recall 问题。

### 第一轮接入并非一次成功

core_flat 最初接入后，RDB 能恢复 record，却没有重建 core index，reload 后 VSEARCH 返回 `ERR invalid vector argument`。[`Stage 2D regression`](../vector_core_reset/stage2d_core_flat_integration_regression.md) 记录了这个失败和 `kvs_vector_rebuild_after_load()` 修复；[`Stage 2E report`](../vector_core_reset/stage2e_core_flat_regression_report.md) 再覆盖 correctness、rebuild 和 RDB reload。

这次返工恰好证明 source-of-truth 边界不是口号：恢复 record 不等于恢复派生 index，load 路径必须显式触发 rebuild。

## 16.6 core_hnsw：第一版内核化已经完成什么

`1810908` 新增的 `src/vector/core/hnsw/` 不是把旧 `ann_index` 简单换文件名。它引入独立的：

```text
contiguous vectors
slot_to_record_id / record_id_to_slot
level0 与 upper neighbor arrays
deleted_flags
visited_tags
reusable search context pool
deterministic random level
add / search / mark_delete / reset / reserve / info
```

[`kvs_hnsw_index_t`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/include/vector/kvs_vector_hnsw.h#L59-L98) 不含 question、scope、TTL 或 response。搜索从 upper greedy descent 进入 level 0 candidate/top heaps，[`kvs_hnsw_index_search()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/src/vector/core/hnsw/kvs_hnsw_search.c#L199-L370) 最终只导出 hit。

相比旧 HNSW，这一版真正解决了：

1. core 可以脱离 `g_vector_state` 构图和检索。
2. graph 内部用 stable record_id，而不是把 manager slot 当长期身份。
3. search context 可复用，核心 search 路径不需要每次 malloc/free。
4. manager 可以用同一套回表、安全过滤和 response 逻辑接 core_flat/core_hnsw。
5. rebuild 由 manager 扫 active records，core 只 reset/reserve/add。

但它没有成为默认 `hnsw`。历史 [`core_hnsw correctness test`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/tests/vector_core_hnsw_correctness_test.sh#L173-L258) 甚至显式断言普通 `hnsw` 配置不是 `core_hnsw`，然后分别验证配置和 CLI 选择 `core_hnsw`。准确表述应是：**core_hnsw 已进入可配置产品命令路径，但仍与 legacy/external backends 并存。**

### 为什么这还不是后来的 core_hnsw_v2

第一版 core_hnsw 仍保留明显结构债务：

- upper neighbors 按 `capacity * KVS_HNSW_MAX_LEVEL * M` 固定展开，真实层数很低时仍预留大量空间。
- add-time layer search、neighbor selection 和 bidirectional link maintenance 成为 rebuild 热点。
- 删除是 logical mark，旧边不会物理移除。
- 同 record_id 的 vector update 不等于完整 graph reconnect。
- 产品仍通过 legacy compatibility ops 接入。

[`Stage 7A memory/layout audit`](../vector_core_reset/stage7a_core_hnsw_memory_layout_cache_audit.md) 和 [`Stage 7D high-dimensional audit`](../vector_core_reset/stage7d_highdim_neighbor_select_audit.md) 都把这些问题留到了边界之后。

因此本章不能把后续 v2 的数据结构、删除策略、重建状态或产品切换提前算在第一版 core_hnsw 上。本阶段完成的是“第一版内核化与可验证接入”，不是 HNSW 的最终实现。

## 16.7 FAISS：外部 exact/ANN 参考，不是产品架构

### 接入了什么

`f61d64d` 增加可选 `ENABLE_FAISS` 构建和 C ABI wrapper。[`kvs_faiss_index_create()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/f61d64d/src/vector/kvs_vector_faiss_backend.cpp#L31-L105) 支持：

```text
IndexFlatIP
IndexHNSWFlat(metric = inner product)
IndexIDMap2(record label)
```

[`faiss_baseline.cpp`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/f61d64d/tests/faiss_baseline.cpp#L90-L240) 是 standalone C++ harness：生成随机向量，IP 路径先 normalize，分别测 add/search，并用 `IndexFlatIP` 生成 exact ground truth。它不经过 KVLite record、manager、RESP 或网络。

同时，FAISS 还有一个 compile-time compatibility adapter，可以绑定到 KVLite `current_index`，让受控 benchmark 走相同 VSET/VSEARCH manager 路径。[`8922145` 时的 adapter](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/8922145/src/vector/kvs_vector_faiss_ops.c#L43-L177) 已经把 manager slot 转成 record_id，并在 search 后回到 manager resolve。

“能走产品命令路径”和“是产品 backend”不是一回事。源码注释、构建开关和阶段设计都把 FAISS 定位为 benchmark-only baseline；它没有成为默认索引，也没有成为新 core 架构模板。

### 它提供两类参照

```text
FAISS FlatIP
→ exact implementation cross-check

FAISS HNSWFlat
→ 成熟 ANN 在相同 M / efConstruction / efSearch / metric 下的外部参照
```

这能帮助区分：

- core_flat 与 FAISS Flat 不一致：优先查 metric、normalization、过滤和 tie-breaking。
- core_hnsw 与两个 exact flat 的 recall 都低：优先查 graph/search 或参数。
- index-only 接近，产品 VSEARCH 差距大：优先查 manager、embedding、response 和网络。

### 不能从 FAISS baseline 推导什么

1. standalone FAISS latency 不能直接等同于 KVLite endpoint latency。
2. `ru_maxrss` 是进程峰值 RSS，不是精确的单 index runtime memory。
3. Stage 4 一度把 FAISS `memory_bytes=0` 输出；这代表未实现统计，不代表零内存。
4. 本阶段没有形成统一、可信的 serialized index size 对比；KVLite RDB 保存 record，不是序列化 FAISS/HNSW graph。
5. 不同 metric、normalize、M、ef 或 query set 下的数字不能直接横比。

## 16.8 hnswlib：为什么还需要第二个外部 HNSW

`8afe305` 接入了 hnswlib `HierarchicalNSW<float>`。[`backend wrapper`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/8afe305/src/vector/kvs_vector_hnswlib_backend.cpp#L20-L200) 使用 `InnerProductSpace`，支持 add/search/reset/resize/setEf；[`smoke`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/8afe305/tests/hnswlib_smoke/hnswlib_smoke.cpp#L1-L43) 只证明 4 维小例子能返回预期 label，不能证明正式 recall 或产品接入。

同时保留 FAISS HNSW 和 hnswlib，不是重复造 baseline。二者承担不同排查价值：

```text
自研 core_hnsw 同时偏离 FAISS 和 hnswlib
→ 自研算法/实现更可疑；

FAISS 与 hnswlib 在同参数下也有差异
→ 需要看实现细节、参数语义和数据分布，不能只怪自研；

外部 backend 在产品路径也出现异常
→ benchmark harness 或 manager adapter 更可疑。
```

### hnswlib 接入也经历了失效与纠正

这条路线不是“vendor header 后一次成功”：

1. [`1K smoke report`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/39eb0a2/docs/vector_native_faiss_optimization/02_smoke_benchmark_report.md) 记录 FAISS/hnswlib 依赖目录为空，只能 SKIP，不能伪造结果。
2. [`formal report`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/39eb0a2/docs/vector_native_faiss_optimization/05_formal_benchmark_report.md) 记录一次 `hnswlib_hnsw` 配置实际落到 flat ops 的 run，被判无效并排除。
3. 初版 adapter 在每次 search 前把 ef 设成 `topK`，配置的 `efSearch=128` 没有真正体现。后续修正后 search 成本上升，旧低延迟数字不能与修正后数字混用。
4. [`reserve report`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/39eb0a2/docs/vector_native_faiss_optimization/12_p0b_hnswlib_add_reserve_report.md) 显示预留容量消除了 resize chain，却没有实质关闭 VSET gap；热点仍在 `addPoint()`。
5. [`internal profile`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/39eb0a2/docs/vector_native_faiss_optimization/14_p0d_hnswlib_internal_profile_report.md) 再把 add 成本归因到 base-layer search 和 neighbor selection。

外部库因此是“对照组和诊断工具”，不是“成熟库一定更快”的结论生成器，更不是新 `vector_core` 的设计模板。

## 16.9 core_flat/core_hnsw 如何进入产品 V 族命令路径

第一版产品接入保留 Route C 的 prepare/commit，不改 VSEARCH response 格式。

### VSET

[`kvs_vector_set_commit_prepared()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/src/vector/kvs_vector.c#L2822-L3112) 的关键顺序是：

```text
prepared question/answer/scope/vector
→ manager 查重或分配 record slot
→ record 接管 prepared 字段，生成稳定 record_id
→ question+scope id_map
→ compatibility flat state（当时仍保留）
→ kvs_vector_index_add_current(record_index, vector)
→ selected adapter 将 record_index 转为 record_id
→ core_flat/core_hnsw add(record_id, vector)
```

这也暴露了过渡期债务：manager 为兼容旧实现仍维护部分 legacy flat/index 状态，不能把本阶段描述成“一刀切删除旧内核”。

### VSEARCH

```text
VSEARCH prepare 得到 query vector
→ kvs_vector_index_search_current()
→ selected backend adapter
→ core search 返回 raw record_id + score
→ kvs_vector_manager_resolve_hits()
→ 校验 record_id、slot、ACTIVE、deleted
→ manager 做 scope / threshold
→ question/answer/scope response assembly
→ RESP
```

core adapter 的实际形状可以从 [`core_hnsw ops search`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/src/vector/kvs_vector.c#L983-L1060) 看到：先申请 raw hit buffer、调用 `kvs_hnsw_index_search()`，再交给 manager resolve。

`a473bea` 又为 manager 增加 `record_id_map`，把 resolve 从逐 hit 扫整个 record store 的 `O(raw_topk * capacity)` 降到平均 `O(raw_topk)`。[`Stage 5A report`](../vector_core_reset/stage5a_manager_resolve_fastpath_report.md) 和 [`最终 fast resolve`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/a473bea/src/vector/kvs_vector.c#L4067-L4165) 仍保留强制校验：

```text
map 中存在 record_id
record_index 在范围内
record 存在
record->id == hit.record_id
record ACTIVE
record 未删除
```

其中 `record->id == hit.record_id` 不能省略。VDEL 释放 slot 后，新 VSET 可能复用它；旧 backend hit 必须被拒绝，不能解析成新 record。

### VDEL、VINDEX、VINFO 与恢复

- VDEL 先处理 manager 的 question+scope/record 生命周期，再通知 selected core `mark_delete(record_id)`；core 不决定业务上是否存在。
- VINDEX REBUILD 由 manager 遍历 active record embedding，core 只 reset/reserve/add。
- VINFO 把 backend size/capacity/dim/metric/memory source 和 profile 汇总到 manager response；core 不直接写 RESP。
- RDB load 和 AOF replay 都从 record/write path 恢复，再重建派生 index。

这条链路证明 core_flat/core_hnsw 不是只存在于 index-only 单测里，而是通过配置进入了真实 VSET/VDEL/VSEARCH/VINDEX/VINFO 路径；但默认 backend、legacy adapter 和最终 ABI 仍未收敛。

## 16.10 Benchmark 口径：三层结果必须分开

### L1：index-only direct vector

[`vector_index_only_formal.cpp`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/39eb0a2/tests/vector_index_only_formal.cpp#L18-L267) 直接读取 float32 base/query 文件，对 hnswlib、FAISS Flat、FAISS HNSW 调 add/search。默认参数在该工具中是：

```text
metric = inner product
topK = 10
M = 32
efConstruction = 256
efSearch = 128
```

[`dataset generator`](../../scripts/benchmark/gen_vector_benchmark_dataset.py) 用固定 seed 生成 synthetic vectors，默认 L2 normalize，并把 dim、metric、normalize、topK 写入 manifest。

这一层指标的定义是：

```text
build/add time：逐 vector index add 的 wall time
search latency：单 query index search wall time
QPS：count / measured total wall time
recall@K：与同数据 direct exact_topk 的集合重叠
RSS：进程 ru_maxrss
```

它排除 embedding、record、scope、RESP、executor 和网络。

### L3：产品 text + embedding + RESP endpoint

历史 [`full e2e report`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/39eb0a2/docs/vector_native_faiss_optimization/10_full_e2e_text_embedding_benchmark_report.md) 明确把此前误称为 server-path 的路径重新命名为 full e2e：

```text
TCP
→ RESP VSET/VSEARCH text
→ hash embedding provider（64d）
→ manager / selected backend
→ record 回表与 response assembly
→ TCP response
```

它不是 raw-vector server benchmark，也不是 ONNX benchmark。其 endpoint latency 不能与 L1 index-only latency直接做倍数结论。

### Stage 4 产品横评

[`bench_vector_core_compare.py`](../../scripts/bench_vector_core_compare.py) 通过临时配置依次启动 core_flat、core_hnsw、hnswlib_hnsw、faiss_flat、faiss_hnsw，发真实 VSET/VSEARCH，并用 core_flat 产品结果计算 recall。它还测 VINDEX rebuild、RDB load+rebuild 和 correctness smoke。

这里的 `VSEARCH avg` 包含 backend search、manager resolve、response assembly、embedding 和 socket 开销。到 Stage 5B/5D 后，报告才分出：

```text
endpoint VSEARCH
estimated backend search
manager resolve
response assembly
```

[`Stage 5B`](../vector_core_reset/stage5b_vsearch_profile_breakdown_report.md) 先拆出 backend search、manager resolve 和 response assembly；[`Stage 5C`](../vector_core_reset/stage5c_vinfo_backend_stats_report.md) 再统一 backend capacity/dim/metric/memory source；[`Stage 5D`](../vector_core_reset/stage5d_vinfo_benchmark_stats_hardening_report.md) 固化 JSON/Markdown 口径，并明确注明：对 legacy FAISS/hnswlib adapter，`backend_search_us` 是 `ops_search_wall_us - manager_resolve_us` 的估算，不是纯 index kernel time。

### build、memory 与 serialized size 不能混写

本阶段至少有四种不同量：

| 字段 | 实际含义 |
| --- | --- |
| index-only add | 直接向外部 index 加点 |
| endpoint VSET | text/embedding/manager/backend/response 的完整写路径 |
| VINDEX rebuild | 从 manager active records 重建 derived index |
| RDB load+rebuild | 读 record snapshot 后重建 index |

内存也有三种来源：`measured`、`estimated`、`unsupported`。FAISS 旧字段为 0 时只能写 unsupported，不能当作零内存；RSS 也不能当作单 index 精确占用。

本阶段没有统一序列化 FAISS/hnswlib/core_hnsw graph 并比较文件大小。RDB 文件若有 size，也主要代表 record snapshot，不是 ANN serialized size。把 serialized bytes 写成 runtime memory 会得出错误结论。

## 16.11 最关键的 benchmark 返工：先修 validity，再谈排名

### 异常如何暴露

Stage 4 首轮出现一个反常结果：`faiss_flat` 是 exact `IndexFlatIP`，却明显低于 core_flat recall。两个 exact 实现使用相同数据、IP、scope 和 topK，理论上不应出现系统性差距。

[`Stage 4A audit`](../vector_core_reset/stage4a_benchmark_validity_audit_report.md) 没有接受“FAISS recall 较差”这个表面结论，而是逐 query 比较返回数量和 key。根因是 adapter 只向 FAISS 请求 `topK` 个 raw candidates，然后 manager 再做 scope/deleted 过滤；候选被过滤后已经无法补满 topK。

### 修正

`8922145` 让 FAISS 和 hnswlib adapter 都采用：

```text
raw limit = topK * 4 + 32
cap = backend index size
manager 再做 scope / deleted / threshold filtering
```

实际 FAISS 修正见 [`limit 与 resolve`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/8922145/src/vector/kvs_vector_faiss_ops.c#L91-L139)，hnswlib 修正见 [`同口径 overfetch`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/8922145/src/vector/kvs_vector_hnswlib_ops.c#L92-L140)。

这次修复没有把 scope 推进 core。恰恰相反，它明确了双方契约：core 返回足够多的 raw hits，manager 负责业务过滤。

### corrected 结果怎么读

[`Stage 4B corrected report`](../vector_core_reset/stage4b_corrected_vector_core_benchmark_report.md) 记录的 recall@10 为：

| 规模 | core_flat | core_hnsw | hnswlib_hnsw | faiss_flat | faiss_hnsw |
| --- | ---: | ---: | ---: | ---: | ---: |
| 10K / 64d | 1.0000 | 0.9870 | 0.9980 | 0.9990 | 0.9960 |
| 100K / 64d | 1.0000 | 0.9770 | 0.9960 | 0.9950 | 0.9890 |

这些数字能说明 corrected harness 下，外部 backend 的早期严重 recall 缺口主要是 overfetch 口径错误；core_hnsw 的 recall、内存和 rebuild 问题仍然存在。

但证据边界也必须写清：报告列出了 `results/vector_core_benchmark/stage4b_*` JSON/Markdown 路径，这些 Stage 4B 原始文件当前不在工作区，也没有进入 `a473bea` 提交树。因此上表的可点击来源是阶段报告，而不是仍可复核的 raw JSON。

另外，hnswlib 100K RDB reload 仍然 timeout；报告没有把 timeout 归因到具体根因。不能因为它超时就断言 hnswlib graph rebuild 本身有某个确定 bug。

## 16.12 测试、结果与证据等级

### 测试资产与执行证据

| 能力 | 测试资产 | 执行证据 | 能下的结论 |
| --- | --- | --- | --- |
| core_flat correctness / dispatch | [`current script`](../../tests/vector_core_flat_correctness_test.sh) | [`Stage 2E report`](../vector_core_reset/stage2e_core_flat_regression_report.md) 记录 PASS | 配置/CLI、scope、update、delete、response 不暴露 id/slot 基本证明 |
| core_flat rebuild | [`current script`](../../tests/vector_core_flat_rebuild_test.sh) | [`Stage 2E report`](../vector_core_reset/stage2e_core_flat_regression_report.md) 记录 PASS | VINDEX rebuild 后产品行为基本证明 |
| core_flat RDB reload | [`current script`](../../tests/vector_core_flat_rdb_reload_test.sh) | [`Stage 2E report`](../vector_core_reset/stage2e_core_flat_regression_report.md) 记录 PASS | record load 后重建 core_flat 基本证明 |
| core_hnsw correctness | [`1810908 script`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/tests/vector_core_hnsw_correctness_test.sh#L1-L258) | [`Stage 3 closeout`](../vector_core_reset/stage3_core_hnsw_closeout_correctness_report.md) 记录 PASS | 产品配置、scope、threshold、update、delete 基本证明 |
| core_hnsw rebuild/RDB/AOF | [`rebuild`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/tests/vector_core_hnsw_rebuild_test.sh)、[`RDB`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/tests/vector_core_hnsw_rdb_reload_test.sh)、[`AOF`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/1810908/tests/vector_core_hnsw_aof_replay_test.sh) | [`Stage 3 closeout`](../vector_core_reset/stage3_core_hnsw_closeout_correctness_report.md) 记录 PASS | derived graph 能从 record/replay 路径恢复，基本证明 |
| cross-backend recall | [`8922145 runner`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/8922145/tests/vector_core_benchmark_compare.sh#L1-L50) | [`Stage 4B report`](../vector_core_reset/stage4b_corrected_vector_core_benchmark_report.md) | 10K/100K 64d corrected 趋势暂时可信；raw files 未保留 |
| high-dimensional direct vector | `vcmd_profile_bench` 的 Stage 7C 模式 | [`384d`](../../results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_384d.json)、[`512d`](../../results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_512d.json)、[`768d`](../../results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_768d.json)、[`1024d`](../../results/vector_core_benchmark/stage7c_direct_vector_highdim_1k_1024d.json) | 1K synthetic direct-vector 的真实 backend_dim、AVX2 和 exact recall 有原始结果 |
| ONNX 512d product smoke | 临时 local_c 配置，3 次 VSET + 1 次 VSEARCH | [`Stage 7C report`](../vector_core_reset/stage7c_direct_vector_highdim_harness.md) | 512d ONNX 能进入 core_hnsw；不证明语义 recall 或规模性能 |

仅有测试文件不等于执行通过。上表把 script 和 report/raw result 分开，是为了避免把“仓库存在测试”写成“当前环境重新跑过”。本章生成过程中没有重跑这些历史 benchmark。

结果文件本身还有两级证据。Stage 6/7 的若干 JSON/Markdown 当前仍存在于本地 `results/vector_core_benchmark/`，并与阶段报告互相对应，但该目录被 `.gitignore` 忽略，这些文件没有进入 `a473bea` 提交树；它们是当前工作区可点击的原始产物，不是 Git 持久化证据。Stage 4B 和历史 external baseline 报告列出的结果目录当前已不存在，因此相关数字只能降级引用明确记录它们的阶段报告。

### 四级证据结论

**已经证明：**

- core contract 可以只使用 record_id/vector/score；阶段静态检查未在 `src/vector/core` 发现 `g_vector_state`、scope、question、answer。
- core_flat 是 exact vector scan，core_hnsw 是 ANN；二者都能从 manager active records 重建。
- RDB 保存 records/embeddings 而非第一版 core_hnsw graph。
- product response 不需要暴露 core slot 或 record_id。

**基本证明：**

- core_flat/core_hnsw 可通过配置进入 VSET/VDEL/VSEARCH/VINDEX/VINFO。
- correctness、rebuild、RDB reload 和 AOF replay 在阶段报告记录的环境中通过。
- manager/core ownership 足以承载多个 backend，而不把 scope/response 下沉到 core。

**暂时观察到：**

- corrected 10K/100K 64d benchmark 中，外部 HNSW recall 高于第一版 core_hnsw。
- 第一版 core_hnsw 的主要工程问题从“link 细节”逐渐收敛到 rebuild layer search、neighbor selection、固定 upper storage 和高维距离重算。
- Stage 7C 的 1K synthetic 384/512/768/1024d 都得到 1.0 exact recall，但这是小规模确定性 workload。

**尚未证明：**

- 真实文本语义相关性；本阶段没有 qrels。
- 512d ONNX 在 10K/100K 的 recall、latency、memory 和长稳。
- 外部 FAISS/hnswlib 与 core 在所有参数、数据和计时层上完全公平。
- 第一版 core_hnsw 已达到生产级内存、rebuild、删除和更新语义。
- compatibility ops 已经是最终稳定 ABI。

## 16.13 失败、低收益优化与真正的转折

这条路线按 Git 顺序看，更接近下面的过程：

```text
旧 manager 内嵌 flat/HNSW
→ standalone FAISS 暴露比较口径混乱
→ hnswlib/FAISS 依赖一度不可用
→ hnswlib 配置曾错误落到 flat ops，run 作废
→ 拆出 core contract
→ core_flat RDB reload 首轮失败
→ core_flat/core_hnsw 接入产品
→ Stage 4 exact backend recall 反常
→ 追到 scope overfetch，而不是误判 FAISS 算法
→ manager resolve 被 overfetch 放大，Stage 5A 再做 record_id fast map
→ rebuild/profile 定位 core_hnsw 热点
→ 局部 micro-optimization 收益递减
→ 高维 harness 修正后确认结构问题仍在
```

几个失败尤其值得在面试中保留。

### 失败一：早期 external baseline 不能比较

第一个 benchmark audit 给出 `NOT_APPROVED`，而不是硬做一张 native vs FAISS 排名表。后来又经历 dependency unavailable、hnswlib 实际绑定 flat、efSearch 未按配置生效。这说明 baseline 工作首先是验证实验设计，不是生成数字。

### 失败二：exact FAISS “recall 低”其实是 adapter 过滤 bug

如果只看最终 recall，会以为修正很顺利；真正的工程价值在于：exact-vs-exact 不一致触发了逐 query 诊断，最后定位到 topK 后置 scope filter。这个 bug 同时存在于 FAISS 和 hnswlib adapter，证明问题在公共接入语义，不在某个外部算法。

### 失败三：reserve 消除 resize，却没有关闭 hnswlib add gap

capacity pre-reserve 是合理低风险尝试，但报告显示主要耗时仍在 `addPoint()` 的 base-layer search 与 neighbor selection。不能把“resize_count 归零”写成“写性能已优化”。

### 失败四：第一版 core_hnsw 的局部优化进入低收益区

[`Stage 6A`](../vector_core_reset/stage6a_core_hnsw_rebuild_profile_audit.md) 先把 rebuild 拆成 manager scan、reset、reserve、add，以及 add 内的 random-level、layer-search、neighbor-select、link 和 vector-copy；后续再逐类优化。到 [`Stage 6E`](../vector_core_reset/stage6e_core_hnsw_layer_search_remaining_hotspot.md)，同口径 10K add/layer-search 改善约 1%，报告明确判为未达到 acceptable pass，并停止扩大 P1 micro-optimization。对应 [`10K raw JSON`](../../results/vector_core_benchmark/stage6e_hnsw_layer_search_hotspot_benchmark_10k_64d.json) 保留了 profile 口径。

### 失败五：第一次 AVX2 更快，但 recall 变了

[`Stage 7B`](../vector_core_reset/stage7b_core_hnsw_avx2_distance_backend.md) 记录，早期水平归约改变浮点累加顺序，构图决策随之变化，recall 从既有基线回退；该实现被拒绝。最终 AVX2 路径保持标量累加顺序，接受较保守的加速换取 recall 不变。

### 失败六：所谓 384d benchmark 实际仍是 64d

Stage 7A 发现文本 benchmark 虽传 `DIM=384`，hash provider 仍固定生成 64d，事件计数和 memory 完全相同。Stage 7B direct-vector 尝试又因 index 先按 64d 初始化而正确拒绝输入。直到 Stage 7C 修复 harness，才真实跑到 384/512/768/1024d。

这些失败共同推动了一个转折：第一版 core_hnsw 已经足够暴露问题，但继续在它上面做零散补丁的边际收益越来越低；需要先冻结边界、oracle 和 baseline，再决定是否重写内核。

## 16.14 工程取舍、当前缺陷与下一阶段触发条件

### 为什么 record store 必须是真值

如果 graph 是真值，question/answer/scope/TTL、AOF/RDB 和 replication 都要理解 ANN 内部 slot、层级和删除语义。graph 算法一变，持久化兼容性也会跟着变。

record 作为真值则更稳定：

```text
持久化和复制只关心业务 record；
core 可以替换；
graph 损坏可以 rebuild；
删除/TTL 最终由 manager 判断；
benchmark backend 也不需要成为数据 owner。
```

代价是 cold start/rebuild 成本。这一代 core_hnsw 恰好证明，派生状态设计正确不代表 rebuild 成本已经可接受。

### 为什么不直接采用 FAISS 作为产品索引

直接采用 FAISS 可以减少自研 ANN 工作，但会引入 C++/第三方构建、部署、内存统计、删除/更新、配置语义和故障隔离问题。更关键的是，它不能替代 KVLite manager：record、scope、TTL、response、RDB/AOF 和 replication 仍需自己实现。

本阶段选择“自研 core + 外部 baseline 并行”，是为了同时得到可控架构与独立参照，不是为了证明自研一定比库快。

### 为什么不能只保留 HNSW

没有 flat exact oracle，ANN recall 只能依赖另一个外部库或自相比较；scope、tie、delete 和 adapter bug 更难定位。core_flat 的性能不是竞争目标，它的价值是确定性和可解释性。

### 为什么 index-only 与 e2e 都要保留

只测 RESP 路径，backend 差异会被 embedding、manager 和网络淹没；只测 index-only，无法发现配置分派、record 回表、scope、response、RDB/AOF 等产品问题。

两层都保留后，才能用差分归因：

```text
L1 差、L3 也差
→ 先看 backend；

L1 接近、L3 差
→ 先看 manager / embedding / response / network；

L1 正确、L3 recall 差
→ 先看 adapter overfetch / filtering / identity resolve。
```

### 当前缺陷

1. 第一版 core_hnsw 固定 upper storage 浪费内存，图布局不是最终形态。
2. rebuild add 热点仍在 layer search 和 neighbor selection；局部优化已进入低收益区。
3. logical delete 和 vector update 的 graph 语义有限。
4. 产品 dispatch 仍有 legacy compatibility ops 和硬编码分支。
5. 外部 adapter 仍保留 manager/global-state 债务，不能当 core 模板。
6. Stage 4B raw files 未保留，corrected 大规模数字只能追溯到报告。
7. 高维原始结果只有 1K synthetic direct vectors；真实 ONNX 只有极小 smoke。
8. 没有真实文本 qrels，不能宣称语义质量。
9. 没有统一 serialized index size，runtime memory source 也需要逐 backend 判断。

下一阶段的触发条件不是“再加一个 profile 字段”，而是：现有边界、exact oracle、外部 baseline 和产品接入已经足以证明第一版 core_hnsw 的结构性问题。下一步才有资格进入 `core_hnsw_v2` 重写，并重新通过同一套 index-only、产品命令和 recall gate；本章到这里停止，不展开 v2 数据结构或后续结果。

## 16.15 Agent 辅助开发边界

本范围九个提交的 commit body 中没有 `Co-Authored-By`。Git 能确认提交 author、实际 diff 和阶段文档，不能确认某一行是人工还是 Agent 生成，也不能仅凭文档密度推断作者分工。

因此面试中更诚实的边界是：

```text
我需要能独立负责的问题定义：
为什么 manager 语义不能进入 core；
为什么 record 是 source of truth；
为什么 exact-vs-exact 不一致必须先停测；
为什么 L1 与 L3 不能混比。

我需要能独立负责的验收决策：
拒绝无效 hnswlib run；
拒绝 recall 回退的 AVX2；
不把 reserve 的低收益包装成突破；
不把 FAISS memory=0 当真实结果；
不把 synthetic recall 当语义 qrels。

Agent 可以辅助的工作：
生成 adapter/core 脚手架；
补重复的测试客户端与报告表格；
枚举静态边界 grep；
整理 benchmark JSON/Markdown。

不能外包的理解：
record_id 与 slot reuse 安全；
core hit 到 manager response 的调用链；
overfetch 为什么属于 contract 问题；
metric/normalize/M/ef 的可比条件；
RDB record 与 derived graph 的恢复关系。
```

只有这些内容确实符合个人经历时，才用第一人称陈述。没有 Git 证据时，不应声称某段代码由 Agent 或某个具体人逐行完成。

## 16.16 面试表达与本章小结

### 1 分钟版本

“Route C 已经解决了向量命令由谁执行，但旧 vector 模块内部还是把 record store、scope、flat、HNSW 和 response 混在一起，外部 FAISS benchmark 又走另一条路径，所以性能或 recall 出问题时很难归因。

这一阶段我把边界重置成 manager 和 vector core。manager 拥有 record、scope、TTL、threshold、持久化和 response；core 只接收 record_id+vector，返回 record_id+score。record store 是真值，flat/HNSW graph 都是可从 active records 重建的派生状态。

我先实现 core_flat 作为 exact nearest-neighbor oracle，再实现第一版 core_hnsw，并通过 compatibility adapter 接入 VSET、VDEL、VSEARCH、VINDEX 和 VINFO。同时接入 FAISS Flat/HNSW 与 hnswlib，分别做 exact cross-check 和外部 ANN 参照。

最关键的返工是 Stage 4：FAISS Flat recall 异常并不是 FAISS 不准，而是 adapter 只取 topK 后又做 scope 过滤。修正 FAISS/hnswlib overfetch 后才形成 corrected baseline。这个阶段证明了边界和验证方法，但第一版 core_hnsw 仍有内存、rebuild 和高维热点，所以后续才需要 v2。”

### 3～5 分钟版本

“阶段起点 `33a886f` 的 vector state 把 store、question+scope id_map、old flat、old HNSW、SQ8、current index 和 embedding provider 放在一起。虽然已有 ops table，但 add 输入 record_index，search 参数带 scope，输出已经是 question/answer/scope result。因此它是 manager backend 抽象，不是独立 ANN core。

先接 FAISS 和 hnswlib 后，benchmark audit 发现 standalone C++、进程内 API 和 TCP/RESP text path 混在一起。最早审查直接给 NOT_APPROVED；依赖一度不可用，一次 hnswlib run 还实际落到了 flat ops。后来才固定 L1 index-only 和 L3 full e2e：L1 用同一 normalized synthetic float 数据、IP、topK、M、efConstruction、efSearch，测 direct add/search 与 exact recall；L3 走真实 TCP/RESP，但当时使用 64d hash embedding，不是 ONNX。

架构上，`kvs_vector_core_ops_t` 把接口压缩为 record_id、vector、score。manager 继续拥有业务字段、active/deleted、scope、threshold、response、AOF/RDB/replication 和 rebuild orchestration。RDB 保存 record 与 embedding，load 后扫描 active records 重建 index，所以 graph 是 derived state。

core_flat 连续存 vector 和 record_id，线性扫描所有未删除 slot，用固定容量 min-heap 取 topK。它是 exact vector oracle，不是语义 qrels，也不是和 FAISS 争性能。FAISS Flat 作为第二个 exact 实现反向校验它。

第一版 core_hnsw 则拥有自己的 slot map、neighbor arrays、visited tags 和 reusable search context。它真实进入可配置产品路径，但没有成为默认 hnsw，而且产品 dispatch 仍通过 legacy compatibility ops，因此不能说 ABI 已最终稳定。

验证里最有价值的不是一张排名，而是 Stage 4A/4B 的返工。FAISS Flat 和 core_flat 理论上都 exact，结果却不一致。逐 query 检查发现外部 adapter 只取 topK raw hits，manager scope filter 后返回不足。FAISS 和 hnswlib 都改成 topK*4+32 overfetch 后，corrected 10K/100K recall 才可信。这个修复还放大了 manager resolve 的全表扫描，Stage 5A 再用 manager-owned record_id_map 把平均复杂度降到 O(raw_topk)，同时保留 record->id 校验防 slot reuse。

后续 profile 证明第一版 core_hnsw 的 rebuild 热点在 layer search 和 neighbor selection；局部 micro-optimization 最后只剩约 1% 收益。高维 harness 也经历了假 384d 实际 64d 的返工，修正后只有 1K synthetic 384到1024d 和 512d ONNX 小 smoke，不能外推真实语义 100K。这就是为什么本阶段结束于可验证边界，而不是结束 HNSW 演进。”

### 常见追问与回答要点

**1. 为什么自己实现 HNSW，不直接用 FAISS？**

不是为了宣称自研更快。自研 core 用于掌握 record_id、删除、rebuild、内存和 C 接口边界；FAISS/hnswlib 保留为独立 baseline。即使采用 FAISS，manager、scope、TTL、RDB/AOF 和 response 仍要自己实现。

**2. core_flat 为什么能作为 ground truth？**

因为它对所有未删除向量做同 metric 全扫描，不做近似剪枝。它证明的是向量 nearest-neighbor exact topK，不是文本语义正确；还要用 FAISS Flat 做 exact-vs-exact cross-check。

**3. Flat TopK 为什么用 min-heap？**

只保留 K 个候选，堆顶是当前最差项。遍历 N 个向量时，新分数优于堆顶才替换，避免保存并排序全部 N 个结果；时间约 `O(N*dim + N log K)`，额外空间 `O(K)`。

**4. FAISS 和 hnswlib 参数如何对齐？**

至少固定 dataset/query/seed、dim、metric、normalize、topK、M、efConstruction、efSearch 和插入顺序。还要确认参数真的进入 backend；本阶段 hnswlib 就出现过配置 efSearch 被 topK 覆盖的问题。

**5. index-only 和 e2e 为什么不能直接比较？**

index-only 只含 direct add/search；e2e 还含 TCP、RESP、text embedding、executor、record 回表、scope/threshold 和 response。二者用于分层归因，不用于直接做库间倍数。

**6. record store 和 graph 谁是真值？**

record store。RDB/AOF/replication 保存或重放业务 record；graph 是运行时派生状态，可从 active records rebuild。这样 core 可替换，图损坏也不会丢 question/answer。

**7. 删除、重建和持久化为什么依赖这一边界？**

VDEL 的业务存在性由 manager 决定，core 只 mark_delete(record_id)；VINDEX/RDB load 由 manager 扫 active records 重建。若 core 持有 scope/record slot 真值，slot reuse 和格式演进都会变危险。

**8. 这一阶段最大的失败或返工是什么？**

最有代表性的是 exact FAISS recall 异常。没有立即归因外部算法，而是停下 benchmark，最后定位到 scope overfetch。其次是高维 harness 标称 384d 实际仍跑 64d，修正后才接受结果。

### 容易夸大的内容

不要说：

```text
“core_flat 证明了真实语义质量”
“FAISS/hnswlib 已经是生产 backend”
“所有 backend 做到了完全公平比较”
“FAISS memory_bytes=0，所以它几乎不占内存”
“1K 1024d recall=1，就证明 100K 高维可用”
“第一版 core_hnsw 已经是后来的 v2”
“manager/core interface 已冻结成最终 ABI”
“hnswlib 100K RDB timeout 已经找到根因”
```

更准确的说法是：建立了 manager/core 职责边界、exact vector oracle、外部 baseline 和产品接入证据；corrected 64d benchmark 能用于阶段内比较，高维与真实语义证据仍有限。

### 本章小结

这一阶段最重要的产物不是某个 HNSW 性能数字，而是一套以后可以反复使用的判断框架：

```text
业务真值在 record store；
ANN graph 是 derived state；
manager 处理语义，core 处理向量；
core_flat 给 exact vector ground truth；
FAISS/hnswlib 给外部实现参照；
index-only 隔离算法，e2e 验证产品；
exact-vs-exact 不一致时先审计 benchmark；
任何加速都不能用 recall 回退换取。
```

它已经证明 core_flat/core_hnsw 能独立存在并进入产品 V 族命令，也证明外部 baseline 可以帮助定位 adapter 和参数问题。它没有证明第一版 core_hnsw 的结构已经足够，也没有证明真实语义 10K/100K 已完成。

正因为边界、oracle 和 baseline 已经建立，下一阶段才能把问题收敛为一个更具体的目标：重写并重新验证 `core_hnsw_v2`。第十六章到这里结束。
