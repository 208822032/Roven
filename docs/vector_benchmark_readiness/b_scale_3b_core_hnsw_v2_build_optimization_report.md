# B-scale-3B: core_hnsw_v2 build path optimization

## 1. 范围与边界

已读取并遵守根目录 `AGENTS.md`。本阶段只审计和验证 `core_hnsw_v2` build path；未修改 HNSW termination、neighbor-selection heuristic、reverse-prune 语义、层级生成、M/M0/efConstruction、图布局、V 命令、默认 backend、RDB/AOF、rebuild 状态机或 FAISS。

现有 B-scale-0/1/2/3 与 B-scale-3A 工作树修改均保留。本报告按用户要求位于 `docs/vector_benchmark_readiness/`；这覆盖了 `AGENTS.md` 的通用 `docs/vector_core_reset/` 报告位置要求，但不改变其余架构约束。

输入固定为：

```text
cache       = /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding
groundtruth = /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth
insertion   = /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_3_10k_benchmark/insertion_order.ids
insertion sha256 = 7888c4f1109bdff8d98954c607c87d25295c98ca72fece15d3917854d9d63a5b
records=10000, queries=200, dim=512, topK=10
M=48, M0=96, efConstruction=512, normalized IP, single thread
```

实验输出位于仓库外：

```text
/tmp/kvlite_vector_benchmark_artifacts/b_scale_3b_build_optimization
```

## 2. 原始构建调用链

```text
bench_core_hnsw_v2_build()
  -> kvs_hnsw_v2_index_init/reserve()
  -> deterministic insertion loop
     -> kvs_hnsw_v2_index_add()
        -> vector/id-slot write + random level
        -> kvs_hnsw_v2_connect_new_node()
           -> upper greedy + bounded construction layer search
           -> kvs_hnsw_v2_neighbor_select()
           -> forward edge append
           -> reverse edge append or reverse prune
              -> existing-neighbor direct scores
              -> known new-edge score reuse
              -> cached diversity-score lookup
              -> neighbor_select + adjacency rewrite
  -> graph/memory stats refresh
```

Build 使用 shared AVX2 distance backend。已有 known-score reuse 和 add-local pair cache 来自前序 Stage 9E3c，本阶段未重复扩展。

## 3. 实际瓶颈排序

三次 baseline 的阶段计时 median：

| rank | field | median us | build median 占比 | 说明 |
|---:|---|---:|---:|---|
| 1 | `reverse_prune_us` | 31,708,110 | 77.94% | 满邻接行上的重评分与 diversity selection |
| 2 | `neighbor_select_us` | 28,177,192 | 69.27% | 包含 reverse-prune 内部调用，与上一项重叠 |
| 3 | `construction_layer_search_us` | 6,728,440 | 16.54% | insertion bounded layer search |
| 4 | `graph_stats_refresh_us` | 228,881 | 0.56% | 每次 add 后统计刷新 |
| 5 | `forward_link_us` | 48,533 | 0.12% | 固定容量邻接 append |

`reverse_link_us=31,865,412` 与 `reverse_prune_us` 重叠，不能与其它行直接相加。主要结论来自 wall-clock 与 exact work counters 的共同证据，而不是重叠字段求和。

## 4. 工作量与资源

Baseline、profiling-off 候选和 metric prototype 的 exact counters 全部一致：

| counter | value |
|---|---:|
| `distance_compute_count` | 511,526,516 |
| `construction_distance_compute_count` | 35,362,867 |
| `neighbor_select_distance_compute_count` | 384,602,689 |
| `reverse_prune_distance_compute_count` | 91,560,960 |
| `reverse_prune_neighbor_select_distance_compute_count` | 369,796,064 |
| `known_score_reuse_count` | 956,832 |
| `pair_cache_lookup_count` | 400,746,656 |
| `pair_cache_hit_count` / `miss_count` | 30,950,592 / 369,796,064 |
| `pair_score_saved_count` | 30,950,592 |
| `temporary_allocation_count` / `free_count` | 39,996 / 39,996 |
| cumulative `temporary_allocation_bytes` | 1,921,961,152 |
| runner `reserve_count` / `resize_count` | 1 / 0 |

Pair-cache hit ratio约为 7.72%。前序 Stage 9E3c 已证明扩大 local cache 会增加成本，本阶段不重开 global cache、edge-score sidecar 或图布局改造。

Peak RSS median：baseline `117,164 KiB`，profiling-off `117,208 KiB`，metric prototype `117,076 KiB`；差异处于运行波动范围。累计 allocation bytes 是跨 9,999 次 add 的周转量，不是常驻内存。

## 5. Baseline 与候选结果

同一 profiling 模式下的 baseline 与 build-only metric prototype：

| mode | build_us min | median | max | median us/record | median improvement |
|---|---:|---:|---:|---:|---:|
| baseline, detailed profiling on | 40,495,254 | 40,680,354 | 40,750,638 | 4,068.035 | - |
| build-only metric dispatch prototype | 39,291,493 | 39,680,750 | 39,925,914 | 3,968.075 | 2.46% |

单独关闭 build 细粒度 wall-clock：

| mode | build_us min | median | max | median us/record | median improvement |
|---|---:|---:|---:|---:|---:|
| profiling off | 40,075,616 | 40,877,411 | 40,925,782 | 4,087.741 | -0.48% |

两个候选都低于约 5% 的有效改善线。metric prototype 虽保持 graph/search 完全一致，但已从最终代码回退；build profiling 开关保留为 guarded measurement primitive，默认仍开启，不作为性能优化结论。

原 B-scale-3 FAISS HNSW build 为 `6,458,684us`。本轮可重复 baseline median 与 FAISS 的差距约为 `6.30x`。已回退 prototype 只能把该实验差距缩到约 `6.14x`，不足以改变路线判断，也不作为当前实现结果。

## 6. 保留与放弃的修改

保留：

- build profiling 可关闭开关；关闭时 build path 不调用 `gettimeofday()`，默认行为保持开启。
- 正式 benchmark search profiling 默认关闭，落实 B-scale-3A 已确认决策。
- exact allocation/work counters 和 benchmark result 输出。
- benchmark-only stable graph SHA-256。
- sweep helper 对 HNSW v2 internal header 的 Makefile 依赖，避免 struct header 更新后复用陈旧 object。
- smoke 中 profiling on/off、buffer mode、graph checksum、ordered result 和 counter 一致性门禁。

放弃：

- 关闭 build profiling：median `-0.48%`，无有效收益。
- build-only metric dispatch specialization：median `+2.46%`，低于接受线，最终代码已回退。
- build workspace 重构：39,996 次临时分配存在，但相对 511.5M distance calls 证据不足；不为未证明收益扩大生命周期代码。
- 扩 pair cache、edge-score storage、global cache：前序报告已否定当前边界内的收益/风险比。
- 邻接布局、heuristic、termination、参数调整：超出本阶段且会破坏完全一致性要求。

## 7. 正确性与确定性

九次真实运行均满足：

```text
graph_sha256 = 1ab22d52d325d784753e2a76b2a2dc3c421b9a6d673893293ac8175b0b5cc296
size = active_count = 10000
deleted_count = 0
```

每次输出 6 x 200 = 1200 条 per-query 结果。baseline、profiling-off 和已回退 prototype 的 returned IDs、顺序、scores、recall 和 exact counters 逐字段完全一致。

| efSearch | recall@1 | recall@10 |
|---:|---:|---:|
| 32 | 1.0000 | 0.9955 |
| 64 | 1.0000 | 0.9990 |
| 128 | 1.0000 | 1.0000 |
| 256 | 1.0000 | 1.0000 |
| 512 | 1.0000 | 1.0000 |
| 1024 | 1.0000 | 1.0000 |

## 8. 测试与检查

通过：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_v2_graph_construction_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_hnsw_v2_performance_profile_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh
tests/vector_real_embedding_hnsw_faiss_sweep_smoke_test.sh
9 x B-scale-3 runner validate
1200-row/run cross-mode ordered-result comparison
```

Static manager/core boundary grep 无新增违规；`kvs_vector_index_ops_t`、VSEARCH response、RDB/AOF 和 product rebuild 未修改。

## 9. 成本归类与结论

算法设计成本：固定 `M0=96`、`efConstruction=512` 下，reverse-prune 需要对满邻接行执行 direct scoring 和 diversity neighbor selection；这是当前图语义和质量约束的一部分。若不改变 heuristic、保存 edge scores 或调整图表示，不能局部消除大部分 369.8M reverse-prune neighbor-select distance misses。

实现成本：build wall-clock profiling、通用 metric dispatch、per-add 临时分配和统计刷新都存在，但本轮测得前两项最多只有 2.46% median 改善，后两项没有足够证据支持改动。它们不是当前约 6.30x FAISS build gap 的主要解释。

本阶段没有接受任何大于约 5% 的 build 性能优化。这个负结果是有效 gate：不降低 recall、不改图、不改参数，也不把噪声包装成优化。

下一阶段保持既定路线：

```text
B-scale-4 - T2Ranking 100K Real Embedding Cache
```
