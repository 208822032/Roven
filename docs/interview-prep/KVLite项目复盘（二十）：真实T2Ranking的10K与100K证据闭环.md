# 第二十章 真实 T2Ranking 的 10K/100K 证据闭环

## 20.1 为什么 synthetic benchmark 已经不够

第十七章用 synthetic vector 和 FAISS parity 证明了 `core_hnsw_v2` 的算法正确性下限；第十八、十九章解决了 snapshot 与在线 rebuild 生命周期。但这些结论仍回答不了一个产品问题：换成真实中文长文档、真实 ONNX embedding 和 100K records 后，输入是否可复现、exact truth 是否可信、ANN recall 是否仍成立、产品链路是否还能保持同样结果。

synthetic/hash embedding 的价值是快速、可控、容易定位算法错误，缺点是缺少真实 tokenizer、token cap、embedding 分布和生成成本。小规模 smoke 可以证明工具能跑，不能替代 10K/100K 的 build、recall、资源和产品容量门禁。

本章解决的核心问题是：

```text
冻结真实 T2Ranking subset 与 bge-small-zh-v1.5 模型口径
-> 一次生成、反复校验 embedding cache
-> 用 core_flat + independent brute-force 建立 exact embedding truth
-> 同输入、同顺序、同参数比较 core_hnsw_v2 与 FAISS
-> 再进入 prepared 和 public-text 产品路径
-> 用 100K 结果暴露真实 capacity correctness 缺陷
```

本章的 `recall` 都是 ANN 返回 ID 与 exact embedding-space topK 的交集，不是 T2Ranking qrels 的语义 MRR/NDCG。真实 embedding 让向量分布更接近产品，但仍不等于业务语义质量或线上 SLA。

## 20.2 Git 范围、演进主线与源码导航

范围核验结果：

```text
起点：fd439e9
终点：3b247a3
inclusive range：fd439e9^..3b247a3
下一提交：7cd837f（统一配置与启动恢复门禁，不属于本章）
```

四个提交形成同一证据链，而不是四篇独立故事：

| Commit | 真正推进的阶段 | 关键转折 |
| --- | --- | --- |
| `fd439e9` | 1K cache/truth/core/FAISS 工具闭环 | 先证明每层能独立生成和校验 |
| `4600dd5` | T2Ranking deterministic subset 与 10K 完整 sweep | 从自建 fixture 进入冻结真实 corpus |
| `ff06c94` | 100K 分片 cache 与 build profiling | 资源门禁、低收益优化止损 |
| `3b247a3` | 100K truth/index-only/product E2E | tie-break 修正、checkpoint、capacity defect 修复 |

核心入口：

1. [`fd439e9` cache generator 与 validate](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/fd439e9/scripts/benchmark/generate_real_embedding_cache.c#L1-L18)
2. [`fd439e9` finite、L2 normalize 和 little-endian f32](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/fd439e9/scripts/benchmark/generate_real_embedding_cache.c#L239-L301)
3. [`fd439e9` exact truth 与 independent oracle](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/fd439e9/scripts/benchmark/generate_real_embedding_groundtruth.c#L657-L694)
4. [`4600dd5` deterministic subset generator](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/4600dd5/scripts/benchmark/generate_t2ranking_subsets.py#L1-L42)
5. [`4600dd5` combined core/FAISS sweep runner](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/4600dd5/scripts/benchmark/generate_real_embedding_hnsw_faiss_sweep_benchmark.cpp#L1-L40)
6. [`ff06c94` cache shard/resume/merge runner](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/ff06c94/scripts/benchmark/run_real_embedding_cache_shards.py#L22-L83)
7. [`3b247a3` backend checkpoint/finalize runner](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/3b247a3/scripts/benchmark/generate_real_embedding_hnsw_faiss_sweep_benchmark.cpp#L1-L45)
8. [`3b247a3` product E2E input/correctness validation](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/3b247a3/scripts/benchmark/run_t2ranking_product_e2e.py#L252-L336)
9. [`3b247a3` prepared embedding shim](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/3b247a3/scripts/benchmark/vector_product_e2e_prepare_shim.c#L1-L48)
10. [`3b247a3` prepared/public dispatch split](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/3b247a3/scripts/benchmark/vector_product_e2e_prepare_shim.c#L291-L346)
11. [`3b247a3` initial capacity 修复](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/3b247a3/src/vector/kvs_vector.c#L798-L806)
12. [`3b247a3` 产品 load/index-ready 硬门禁](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/3b247a3/scripts/benchmark/run_t2ranking_product_e2e.py#L515-L616)

## 20.3 七层实验必须分开计时

这一阶段最重要的不是多跑一个脚本，而是让每层只回答一个问题：

| 层级 | 输入与输出 | 包含成本 | 明确排除 |
| --- | --- | --- | --- |
| subset freeze | 原始 T2Ranking -> 10K/100K TSV/IDs | 解析、选择、checksum | ONNX、index |
| embedding cache | text -> normalized f32 + JSONL | tokenizer、ONNX、normalize、artifact write | HNSW build/search |
| exact truth | frozen f32 -> exact topK | core_flat scan、double brute-force | ONNX、ANN、产品链路 |
| core index-only | f32 -> v2 build/search | graph build、kernel search | RESP、manager、ONNX |
| FAISS index-only | 同 f32 -> Flat/HNSW | FAISS C++ API | KVLite 产品 adapter |
| prepared E2E | cached f32 -> public V responses | RESP、dispatch、commit、index、resolve、response | tokenizer、ONNX |
| public text E2E | query text -> public V responses | tokenizer/ONNX + prepared 之后全链路 | 并发、多客户端 SLA |

这使结果可以归因：cache 慢不能归咎于 HNSW build，index-only 快不能冒充 endpoint latency，prepared/public 差值才能观察 embedding path 成本。

另外两组口径始终不能互换：serialized artifact size 不是 runtime memory；单次正式 build 加五轮 query latency也不是长期稳定 SLA。

## 20.4 数据集、revision 与 deterministic prefix

[`B-scale-0 报告`](../vector_benchmark_readiness/b_scale_0_t2ranking_subset_generation_report.md) 冻结：

```text
T2Ranking source revision = 2a369a430a70979223f1b9a41b1919774d46b432
algorithm = t2ranking-subset-v1
query_count = 200
collection sizes = 10,000 / 100,000
positive pairs = 1,131
```

query 先按 `SHA256(revision + "\tquery\t" + qid)` 排序；positive pid 先进入 collection，再用 `SHA256(revision + "\tdocument\t" + pid)` 补齐。相同 hash 以 ID tie-break，选择不依赖源 TSV 顺序、时间、Python hash 或随机数。

100K 不是另抽一份数据，而是在同一确定性 document 顺序上继续扩展，所以 10K 是 100K 的严格前缀。报告记录 `nested_prefix=true`、ID/TSV order 对齐和重复运行 `status=unchanged`。

正式 subset 位于仓库外：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/
manifest.json SHA-256:
9eb31fdbc37eed14984de5eb373b0047993646161efab4bcbe2c944c1305bddb
collection.10k.tsv SHA-256:
7d3155be4ad6c5a1c74c3d132e4318e16665533f378cbe04e4259579aea4b190
collection.100k.tsv SHA-256:
738f03d1a1b063a1957fb627669c9520b75b4745e1475d2740eabcc1b630736c
```

这些文件当前本机存在，但不在 Git 中，因此这里只记录绝对路径与 checksum，不构造仓库 URL。

## 20.5 真实 embedding cache：昂贵推理变成可复用输入

`fd439e9` 先以 1K/200 smoke 形成 cache contract；`4600dd5` 扩展为带 external ID 的 T2Ranking 10K；`ff06c94` 再用连续行分片生成 100K。cache 格式始终是：

```text
embeddings.f32        row-major little-endian float32 [records, 512]
query_embeddings.f32  row-major little-endian float32 [queries, 512]
records.jsonl          row/id/external pid/text 对齐
queries.jsonl          query row/qid/text 对齐
manifest.json          model/input/output/shape/checksum/params/timing
```

模型口径由 [`B-scale-1 报告`](../vector_benchmark_readiness/b_scale_1_t2ranking_10k_embedding_cache_report.md) 固定为 `bge-small-zh-v1.5`、ONNX Runtime、dim 512、float32、L2 normalization、normalized inner product。raw byte guard 使用 `max_text_len=1,048,576`；tokenizer 内部仍 capped at 512 tokens。此前 `max_text_len=512` 和 `65,536` 都因真实长文档超过 raw guard 失败，这个返工说明 byte length 和 token cap 不是同一参数。

validator 检查 schema、文件 SHA-256、shape、row/id order、NaN/Inf、逐行 L2 norm 和 JSONL 行数。`validate` 不调用 ONNX；生成先写临时目录，自验证后 rename 发布。这样昂贵推理可以只做一次，后续 truth、core、FAISS 和产品 prepared 共用完全相同 bytes。

10K 正式 cache 报告记录生成 10,000 个长文档约耗时 1,710.7 秒，peak RSS 约 1.91 GiB；这只是 embedding artifact 生成成本，不是 index build。正式 `embeddings.f32` SHA-256 为 `aeead23e...c77fc1`。

## 20.6 100K cache：分片、资源门禁与未完成重跑

100K 不能简单把单进程命令放大十倍。[`B-scale-4A 报告`](../vector_benchmark_readiness/b_scale_4a_100k_embedding_parallel_throughput_preflight_report.md) 先在 4 vCPU/3.8 GiB VM 上比较 process/thread/batch：最佳完整配置只有 `18.058 records/s`，未达到 `28 records/s` 门禁；`2p x 2t / batch64` 已出现 swap 压力而被停止。因此当时明确判定不能启动完整 90K。

资源条件变化后，[`B-scale-4 正式报告`](../vector_benchmark_readiness/b_scale_4_t2ranking_100k_embedding_cache_report.md) 复用前 10K 与全部 query cache，把新增 90K 分成四个连续 22,500-row shard；每个 worker 独立 ONNX session，完成 checksum/finite/norm/row-order 校验后按全局顺序 merge并原子发布。

正式结果：

```text
generated records = 90,000
generation wall = 2,544.260 s
aggregate throughput = 35.374 records/s
sampled aggregate peak RSS = 7,883,236 KiB
swap I/O / OOM / worker failure = none
```

一次完整 shard rerun 运行约 `1,769.2s` 后被止损，因为还需接近 42 分钟且已有多组 byte-identical、四 shard validate 和全量 merged validate 证据。这个部分 rerun 不计为确定性通过。

100K cache 位于：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_4_100k_embedding/
embeddings.f32 bytes = 204,800,000
SHA-256 = 970ce35b4fd60e82f9ed06a81ef97da5fa02ba97cd4ae848f76ad10293ea0994
```

前 10K embedding bytes 与 B-scale-1 完全一致，query cache 也 byte-identical，证明 10K/100K 不只是文本前缀，embedding artifact 也保持前缀关系。

## 20.7 exact ground truth：双 oracle 与明确 tie-break

[`ground truth generator`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/3b247a3/scripts/benchmark/generate_real_embedding_groundtruth.c#L657-L930) 对每个 query 建立两条独立计算路径：

```text
frozen query f32
-> core_flat exact IP search
-> topK hits

同一 query/base f32
-> independent double-precision dot product over every record
-> score descending, numeric record_id ascending
-> topK oracle
```

两边校验 set、score tolerance 和无 tie 时的 strict order。100K 共计算 200 x 100,000 = 20,000,000 个 scores，但不保存完整 score matrix，只复用一个 100K work array。

[`B-scale-5 报告`](../vector_benchmark_readiness/b_scale_5_t2ranking_100k_exact_groundtruth_report.md) 记录 200 个 query 全部 ID/set/order 门禁通过，nonfinite 为 0，并出现一组真实 topK score tie；冻结规则用 numeric record ID ascending 解歧。`groundtruth.jsonl`：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_5_100k_groundtruth/groundtruth.jsonl
rows = 200
SHA-256 = 7383a3803ab3e2caea8f85e7cef28bfb8982ff7b3f3e8251e072ffb5b86e9cbb
```

qrels 没有参与这一步。exact truth 表示 embedding-space 最近邻，不表示人工语义相关性。

## 20.8 core 与 FAISS 的同口径 index-only sweep

core 和 FAISS 共享同一 cache、truth、topK、单线程设置和确定性 insertion order：

```text
M=48, M0/level-0 capacity=96, efConstruction=512
efSearch=32,64,128,256,512,1024
warmup=1, measured rounds=5
metric=normalized inner product
```

FAISS `IndexFlatIP` 先做 exact parity，`IndexHNSWFlat` 才进入 ANN sweep。FAISS internal labels 映射回 canonical numeric record IDs；两个 HNSW backend使用同一 `insertion_order.ids`，而不是各自默认插入顺序。

index-only timer 包含 backend graph build 或单 query kernel search，不包含 ONNX、RESP、manager resolve、response assembly、RDB/AOF、snapshot 或 rebuild。

## 20.9 10K 证据闭环：规模上升改变相对表现

[`B-scale-3 10K 报告`](../vector_benchmark_readiness/b_scale_3_t2ranking_10k_hnsw_faiss_benchmark_report.md) 给出的单次结果：

| Backend | Build | ef32 recall@10 / avg | 首次 recall=1.0 |
| --- | ---: | ---: | ---: |
| core_hnsw_v2 | 37.561 s | 0.9955 / 134.316 us | ef128 / 368.761 us |
| FAISS HNSW | 6.459 s | 0.9860 / 87.454 us | ef256 / 503.122 us |

10K 上 core build 明显更慢，但在相同 ef 下 recall 更高；不能把这种 recall/latency trade-off 简化成谁“全面更快”。FAISS Flat strict parity 通过，说明差异来自 HNSW graph/search，而不是两边输入或 exact truth 不一致。

这一结果也推翻 1K smoke 的相对排序外推：1K 时 `efSearch=1024` 接近全量候选，不能代表 ANN 在更大规模的行为。

## 20.10 build 优化尝试：负结果也是门禁

`ff06c94` 没有只生成 100K cache，还对 10K build 做了针对性 profiling。[`B-scale-3B 报告`](../vector_benchmark_readiness/b_scale_3b_core_hnsw_v2_build_optimization_report.md) 将主要热点归因到 reverse prune 与其内部 neighbor selection；计数显示 pair cache hit ratio 约 `7.72%`。

两项候选均低于预设约 5% 接受线：

```text
关闭细粒度 profiling：median -0.48%
build-only metric dispatch prototype：median +2.46%
```

metric prototype 已回退；profiling 开关和工作量 counters 留作测量能力，不作为性能优化结论。没有为追求数字去降低 recall、调整 M/efConstruction、扩大低命中 cache 或重写图布局。

这次止损的工程价值是把“可以局部消除的实现成本”和“reverse-prune 图语义导致的算法成本”分开。低于噪声/收益门槛的优化不应包装成突破。

## 20.11 100K index-only：checkpoint、tie 返工与规模结论

100K 单 backend build 已是分钟级工作。原 combined runner 只有 core 与 FAISS 全部成功后才一起发布，一个后端失败会丢掉另一个已完成结果。`3b247a3` 增加 `--backend core|faiss|finalize`：每个 backend 完成 checksum 后写 checkpoint，签名绑定输入、truth、insertion order、参数和模式；finalize 要求两侧 checkpoint 有效，validate work dir 后原子 rename。

第一次 FAISS 正式运行被 Flat parity 拒绝：query 67 第 8/9 名 float32 score 相同，FAISS 原始顺序与 numeric-ID tie-break 相反。修复只在 Flat parity 计时区间外 canonicalize equal-score IDs，保留失败日志并复用未变化的 core checkpoint，没有重跑昂贵 core build。

[`B-scale-6 报告`](../vector_benchmark_readiness/b_scale_6_t2ranking_100k_hnsw_faiss_index_only_report.md) 记录：

| Backend | Build wall | ef64 recall@10 / avg | ef128 recall@10 / avg | 首次 recall=1.0 |
| --- | ---: | ---: | ---: | ---: |
| core_hnsw_v2 | 678.327 s | 0.9910 / 650.426 us | 0.9985 / 1,119.607 us | ef512 / 3,217.314 us |
| FAISS HNSW | 215.897 s | 0.9880 / 470.766 us | 0.9955 / 834.914 us | ef512 / 2,748.486 us |

100K 证明了两个不能从 10K 外推的变化：两边首次完全召回都移动到 ef512；core/FAISS build gap 从约 5.82x 收窄到约 3.14x。它仍是同机单次 build 和单线程 sweep，不是多次 median、最优参数证明或更大规模趋势。

正式 core result 当前存在于仓库外：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_6_100k_benchmark/core_hnsw_v2_results.json
SHA-256 = d851d8babf82109d6a412103dd7e4d6080b04feafee386b2cd423819bde90b1d
```

core 的 `memory_bytes` 是 estimate；FAISS 报告的是 serialized index storage 和 process RSS，不能做 runtime memory 排名。

## 20.12 prepared 产品 E2E：隔离 manager/endpoint 成本

产品 benchmark 没有恢复 FAISS backend。FAISS 在本章始终是 standalone index-only baseline；拿 compatibility name 跑产品会 fallback，不能冒充 FAISS E2E。

[`prepared shim`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/3b247a3/scripts/benchmark/vector_product_e2e_prepare_shim.c#L291-L346) 只对带 fixture tag 的请求从 mmap cache 取 embedding，调用既有 `*_prepare_with_embedding()`。之后仍经过：

```text
RESP parse -> command dispatch -> prepared commit/search
-> manager record store / current index ops
-> record_id resolve/filter -> result build -> RESP encode
```

因此 prepared 模式不是 index-only。它排除 tokenizer/ONNX，保留产品装载、manager 和 endpoint 成本。

[`B-scale-7 报告`](../vector_benchmark_readiness/b_scale_7_t2ranking_100k_product_e2e_report.md) 中 prepared 结果：

| ef | recall@10 | client avg | server avg | backend avg |
| ---: | ---: | ---: | ---: | ---: |
| 64 | 0.9905 | 913.090 us | 769.200 us | 704.016 us |
| 128 | 0.9985 | 1,372.726 us | 1,228.487 us | 1,164.887 us |
| 512 | 1.0000 | 3,609.656 us | 3,443.524 us | 3,376.513 us |

prepared 与 index-only 也不是纯同 ef parity：产品 raw topK 为 42，ef32 的 candidate floor 至少为 42，而 index-only topK 是 10。报告因此没有用低 ef 差异证明 kernel 回归。

## 20.13 public text + ONNX：完整查询路径，但不是并发 SLA

同一已加载 index 再使用普通中文 query 调公开 `VSEARCH`，shim fall through 到真实 tokenizer/ONNX。`runtime_encode_count=7,200`，恰好等于 `6 ef x (1 warmup + 5 measured) x 200 queries`；每个 ef 下 prepared/public returned-ID checksum 相同，证明 frozen query cache 与在线 ONNX 输出对齐。

[`B-scale-7 报告`](../vector_benchmark_readiness/b_scale_7_t2ranking_100k_product_e2e_report.md) 的 public 结果：

| ef | recall@10 | client avg | QPS |
| ---: | ---: | ---: | ---: |
| 64 | 0.9905 | 5,342.179 us | 187.190 |
| 128 | 0.9985 | 6,018.167 us | 166.164 |
| 512 | 1.0000 | 8,237.127 us | 121.402 |

相对 prepared，public 多出的约 4.75～5.17 ms 主要来自 tokenizer/ONNX；VINFO server timing 从 prepare 之后开始，client-server 差值也不能全部解释为网络。

这是单实例、单 RESP client、单次正式 load、每档五轮 query 的结果。它证明真实文本路径可运行且返回与 prepared 一致，不证明高并发吞吐、尾延迟稳定性或服务 SLA。

## 20.14 产品 E2E 实际覆盖了哪些生命周期能力

本次产品 runner 使用 disposable cwd/data dir，从 100K prepared VSET 逐条构建产品 index，然后直接做 query sweep：

```text
AOF：disabled，fsync=no
replication：未连接
RDB：编译/运行能力开启，graceful shutdown 生成一次 disposable RDB 后清理
snapshot load：未作为正式启动输入
snapshot write/cold start：未测
async rebuild：未调用
```

因此不能说本章验证了 RDB reload、AOF replay、snapshot cold start 或在线 rebuild。第十八、十九章能力只构成已有产品背景，本章没有重新证明它们。

prepared VSET 的 100K load 包含 RESP、record commit、ID maps 和 product add dispatch，报告记录约 740.826 秒；它与 678.327 秒 index-only build 的差值不是纯 manager overhead，因为两条执行环境和计时边界仍不同。

## 20.15 100K capacity correctness 缺陷与最小修复

第一次产品运行出现最有价值的失败：manager `active_records=100,000`，但 v2 `size=34,480`，recall@10 约 0.33。不是 ANN 参数不够，而是 product/index count 已经分叉。

代码审计发现 `kvs_hnsw_v2_index_reserve()` 对 populated index 是 destructive reserve；add-time growth 会重置已有图。同时 `vector_hnsw_initial_capacity` 当时只被 parse/log，没有在初始化 reserve 前生效。

`3b247a3` 的最小修复增加 [`kvs_vector_initial_record_capacity()`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/3b247a3/src/vector/kvs_vector.c#L798-L806)，并在 [`init/reset-for-load reserve`](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/3b247a3/src/vector/kvs_vector.c#L1907-L1918) 使用已配置初始容量。runner 同时加入：

```text
active_records == core_hnsw_v2_size
               == core_hnsw_v2_active_count
               == 100000
```

修复后正式 load 通过，失败结果保留在仓库外 `b_scale_7_100k_product_e2e_failed_capacity_reset/`，没有覆盖正式结果。

这个修复只保证**已知并预留 100K** 的测试不触发 destructive growth；它没有把动态 reserve 改成 lossless allocate-copy-publish。未知或超过预留规模仍可能触发同类风险，当前 `AGENTS.md` 也把它列为后续 failure-atomicity blocker。

## 20.16 测试、artifact 与证据等级

关键测试：

| 测试 | 主要覆盖 | 证据边界 |
| --- | --- | --- |
| [`subset smoke`](../../tests/vector_t2ranking_subset_generator_smoke_test.sh) | row-order independence、prefix、bad input | synthetic fixture，不是正式数据运行 |
| [`cache smoke`](../../tests/vector_real_embedding_cache_smoke_test.sh) | schema/checksum/finite/norm | 小规模，不证明 100K 资源 |
| [`shard smoke`](../../tests/vector_real_embedding_cache_parallel_shard_smoke_test.sh) | resume、gap/overlap、byte-identical merge | disposable fixture |
| [`truth smoke`](../../tests/vector_real_embedding_groundtruth_smoke_test.sh) | tie、wrong dim、nonfinite、atomic cleanup | 小规模 failure injection |
| [`sweep smoke`](../../tests/vector_real_embedding_hnsw_faiss_sweep_smoke_test.sh) | backend checkpoint、corruption、finalize | 不等于正式 build 已运行 |
| [`product E2E smoke`](../../tests/vector_t2ranking_product_e2e_smoke_test.sh) | prepared/public 和 capacity gate | 小规模产品路径 |

阶段报告记录这些测试当时 PASS；本次写作审计没有重新执行耗时 benchmark。正式 artifact 当前仍在 `/home/zhengpan/share/benchmark_artifacts/T2Ranking/`，本次抽查以下 checksum 与报告一致：

```text
b_scale_0/manifest.json
  9eb31fdbc37eed14984de5eb373b0047993646161efab4bcbe2c944c1305bddb
b_scale_4_100k_embedding/embeddings.f32
  970ce35b4fd60e82f9ed06a81ef97da5fa02ba97cd4ae848f76ad10293ea0994
b_scale_5_100k_groundtruth/groundtruth.jsonl
  7383a3803ab3e2caea8f85e7cef28bfb8982ff7b3f3e8251e072ffb5b86e9cbb
b_scale_6_100k_benchmark/core_hnsw_v2_results.json
  d851d8babf82109d6a412103dd7e4d6080b04feafee386b2cd423819bde90b1d
b_scale_7_100k_product_e2e/product_e2e_result.json
  a64b2988541c658b9ffe6535979741a2cf85f82803ebe8d36fbe9bd60341f8e3
```

证据等级：

```text
已经证明：subset/cache/truth checksum 链、100K index count、per-query recall 复算
基本证明：prepared/public 单客户端产品路径和 capacity 最小修复
暂时观察到：单机 build/search/latency/RSS 数字和参数平衡点
尚未证明：语义 qrels、并发 SLA、跨机器稳定性、动态无损扩容
```

## 20.17 工程取舍、Agent 辅助边界与面试表达

### 工程取舍

1. 用 cache 换时间和可复现性：承担约 512 MiB 的 100K artifact 空间，避免每轮重复昂贵 ONNX。
2. 用 checksum/provenance 换实验可信度：任何 subset、模型、顺序或参数变化都不能静默复用 truth。
3. 用分层 runner 换归因能力：代码更多，但能区分 embedding、kernel、manager 与 endpoint。
4. 用 checkpoint 换失败恢复：backend 分开发布，避免分钟级 core build 因后续 FAISS tie 问题白跑。
5. 对低收益优化止损：保留 measurement，不接受低于门槛且解释不了主差距的 prototype。
6. capacity 只做最小闭环：让冻结 100K 正确运行，但不把 benchmark 修复冒充通用无损扩容。

### Agent 辅助边界

Agent 适合辅助实现格式校验、生成 smoke/failure matrix、核对 manifest、比较 per-query JSONL、追踪计时边界和批量审计 checksum。人必须先决定数据 revision、实验分层、truth 定义、tie-break、接受线、资源停止线和结果能否用于改默认参数。

本章不能根据代码风格猜测逐行作者。可陈述的是：Git 记录了工具、测试、报告和修复的演进；本次复盘用历史源码、阶段报告与仍存在的外部 artifact 交叉核验，没有把 Agent 生成的脚本本身当成性能结论。

### 1 分钟版本

“之前的 HNSW 门禁主要是 synthetic。我们固定 T2Ranking revision，用稳定 hash 生成嵌套的 10K/100K subset，再用 bge-small-zh-v1.5 ONNX 生成带 checksum、row/ID 对齐和 finite/norm 校验的可复用 f32 cache。exact truth 同时由 core_flat 和独立 double brute-force生成，统一 score 降序、ID 升序 tie-break。之后 core 与 FAISS 共用输入、插入顺序和参数做 index-only sweep，再把 core 接到 prepared 和 public text E2E。100K 不仅显示 recall/latency 随 ef 的变化，还暴露了 destructive capacity growth：records 有 100K，但图只剩 34,480。我们让配置的初始容量真正生效并加 count gate，但没有假装动态扩容已经无损。整个结果是单机单客户端证据，不是语义 qrels 或并发 SLA。”

### 3～5 分钟版本

1. synthetic 适合算法定位，但真实长文本带来 tokenizer、512-token cap、ONNX 资源和不同 embedding 分布。
2. subset 用固定 revision/hash 选取，10K 是 100K 前缀；每个输入输出都有 SHA-256。
3. cache 把 ONNX 从后续实验移出；100K 用连续 shard、resume、validate、merge 和 atomic publish控制失败成本。
4. truth 用 core_flat 与 independent double brute-force 交叉验证，并冻结 score/ID tie-break；qrels 不参与 ANN recall。
5. core/FAISS index-only 共用 insertion order、M/efC/efS、单线程和计时边界；100K 首次完全召回均到 ef512。
6. build 优化中 metric prototype 只有 2.46% median 收益，低于 5% 门槛而回退。
7. backend checkpoint 让 FAISS tie-break 返工不必重跑 core；finalize 验 checksum 后原子发布。
8. prepared 隔离 manager/endpoint，public 再加入 ONNX；两者 ID checksum一致。
9. 产品 100K 暴露 capacity reset，最小修复让初始预留生效，但动态无损扩容仍未完成。

### 常见追问

**真实 embedding recall 为什么仍不是语义质量？**

因为 truth 是 embedding-space exact topK。qrels 是人工/数据集相关性标签，两者回答不同问题；本章没有计算 MRR/NDCG。

**为什么要保存 cache，而不是每轮重新推理？**

重新推理昂贵且会把 ONNX 成本混入 index build。cache 加 checksum 后既降低重复成本，也保证 core/FAISS/product prepared读到相同 bytes。

**core_flat 为什么还要 independent brute-force？**

只用一个实现会把实现 bug 固化成 truth。独立 double dot-product 和不同排序代码提供交叉证据。

**为什么 equal score 必须规定 tie-break？**

不规定时同一个 exact set 会因后端内部顺序导致 strict rank 假失败。冻结 numeric ID ascending 后，parity失败才有确定含义。

**prepared E2E 是否就是 index-only？**

不是。它只绕过 tokenizer/ONNX，仍包含 RESP、dispatch、manager commit/search、resolve 和 response encode。

**100K capacity 修复是否彻底解决扩容？**

没有。它让已知 `initial_capacity=100000` 在初始化时生效，避免本次运行触发 destructive reserve；超过预留仍需要 lossless growth 重构。

**为什么没有根据 100K 结果修改默认 efSearch？**

因为只有一个数据集、单机、单线程或单客户端结果。它能给出当前 trade-off，不能证明全场景最优默认值。

### 容易夸大的内容

不能说：

```text
ANN recall 就是 T2Ranking 语义效果；
index-only latency 就是 VSEARCH endpoint latency；
prepared 模式包含 ONNX；
serialized bytes 就是 runtime memory；
一次 100K sweep 就是稳定 SLA；
FAISS 已接入产品 E2E；
RDB/AOF/snapshot/rebuild 已被本轮完整验证；
2.46% prototype 已成为正式优化；
initial-capacity 修复已经实现通用无损扩容；
7cd837f 的统一配置和恢复门禁已经属于本章。
```

### 本章小结

本阶段把“真实 benchmark”从一句描述变成可追溯证据链：

```text
fixed revision subset
-> deterministic 10K/100K prefix
-> ONNX embedding cache + validation
-> core_flat/double exact truth
-> shared-order core/FAISS index-only sweep
-> checkpoint/retry/finalize
-> prepared product E2E
-> public text + ONNX E2E
-> 100K capacity count gate and minimal fix
```

已经证明的是 100K frozen input 下的 exact ANN recall、index count 和单客户端产品结果；尚未证明的是语义 qrels、并发服务 SLA、动态无损扩容和跨配置启动恢复一致性。

下一篇只进入“第二十一章：统一配置与启动恢复门禁”，候选范围 `7cd837f^..7cd837f`。
