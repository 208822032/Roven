# B-scale-3: T2Ranking 10K core_hnsw_v2 / FAISS index-only benchmark report

## 1. AGENTS.md 遵守情况

已读取并遵守仓库根目录 `AGENTS.md`。本阶段只新增/修改 benchmark 工具、smoke test、Makefile opt-in target 和 `docs/vector_benchmark_readiness/` 文档；未修改生产 V 命令代码，未修改默认 backend，未修改默认 HNSW 参数，未修改 RDB/AOF，未修改 `VSEARCH` response，未修改 `kvs_vector_index_ops_t`。

冲突说明：`AGENTS.md` 的历史阶段模板要求阶段报告写入 `docs/vector_core_reset/`；当前 benchmark readiness 路线要求所有 benchmark 文档集中在 `docs/vector_benchmark_readiness/`。本报告按任务指定目录输出，同时保持 AGENTS 的生产边界。

## 2. 阶段目标和边界

目标：读取 B-scale-1 冻结的 T2Ranking 10K real embedding cache 和 B-scale-2 冻结的 core_flat exact ground truth，在同一确定性插入顺序、同一 topK、同一 HNSW 参数、同一单线程环境和同一计时规则下执行 index-only 对比：

```text
core_hnsw_v2
FAISS IndexFlatIP exact parity
FAISS IndexHNSWFlat
```

本阶段未执行：

- ONNX embedding 推理。
- B-scale-0 子集重新生成。
- B-scale-2 ground truth 重新生成。
- qrels 作为 ANN recall ground truth。
- product VSET / VSEARCH e2e benchmark。
- 参数 sweep 之外的调参或优化。
- HNSW-R 或 rebuild 相关变更。

## 3. 现有 B3/B4 runner 审计结论

审计文件：

```text
scripts/benchmark/generate_real_embedding_hnsw_v2_benchmark.c
scripts/benchmark/generate_real_embedding_faiss_benchmark.cpp
tests/vector_real_embedding_hnsw_v2_benchmark_smoke_test.sh
tests/vector_real_embedding_faiss_benchmark_smoke_test.sh
Makefile
```

结论：

- 旧 B3/B4 runner 已适合 1K 固定 dataset layout，但默认以 `--dataset-id/--out-root` 读取同目录 cache / ground truth。
- 旧 B3 runner 只覆盖 `core_hnsw_v2` 单 backend，旧 B4 runner 只覆盖 FAISS，无法天然保证两个 HNSW backend 使用同一插入排列。
- 旧 runner 只覆盖单个 `efSearch=1024`，不能输出 recall-latency 曲线。
- 旧 B3/B4 smoke 已保留并通过，确保 1K harness 无回归。

本阶段采用新的通用 combined index-only runner，而不是复制 T2Ranking 专用工具。C++ runner 负责 FAISS 和共享流程；C helper 包装 core_hnsw_v2，避免 KVLite C headers 与 FAISS C++ headers 直接混用。

## 4. 新增/修改文件

新增：

```text
scripts/benchmark/generate_real_embedding_hnsw_faiss_sweep_benchmark.cpp
scripts/benchmark/real_embedding_core_hnsw_v2_sweep_helper.c
tests/vector_real_embedding_hnsw_faiss_sweep_smoke_test.sh
docs/vector_benchmark_readiness/b_scale_3_t2ranking_10k_hnsw_faiss_benchmark_report.md
```

修改：

```text
Makefile
docs/vector_benchmark_readiness/README.md
docs/vector_benchmark_readiness/index.md
```

`Makefile` 仅新增 opt-in targets：

```text
gen-embedding-hnsw-faiss-sweep-benchmark
smoke-vector-real-embedding-hnsw-faiss-sweep-benchmark
```

## 5. 输入 provenance

Embedding cache：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding
```

Exact ground truth：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth
```

输入参数：

```text
record_count = 10000
query_count = 200
dim = 512
topK = 10
dtype = float32
normalized = true
metric = ip_normalized
```

输入 checksum：

| file | sha256 |
|---|---|
| `manifest.json` | `ff349b42dc6ed07fc492b0163df536c89783c0998cbc61be0ec974702953ac9b` |
| `embeddings.f32` | `aeead23e186bc73da2d92ce52885bf5e1763fbed5cb430059ba65abf21c77fc1` |
| `query_embeddings.f32` | `2b1d3e5952051dcf4eee938b6edf3454fef1660f16aa0e319d9e3d53e8dca318` |
| `groundtruth.jsonl` | `cee138ab689c73475e4775d2311026eacff41fad57309ef063b41b0c5b9f444b` |
| `groundtruth.manifest.json` | `0d338c6a7e19e0b5faaf726f525e208abf1c7905d58fa44c7846c39f3402d783` |

运行前 validate：

```text
[validate] PASS dataset_id=b_scale_1_10k_embedding record_count=10000 query_count=200 dim=512
[validate] PASS dataset_id=b_scale_1_10k_embedding query_count=200 topk=10 record_count=10000 dim=512
```

## 6. 确定性插入顺序

算法：

```text
SHA256(source_revision + "\thnsw-insertion\t" + external_pid)
tie-break = numeric record_id ascending
source_revision = 2a369a430a70979223f1b9a41b1919774d46b432
```

输出：

```text
insertion_order.ids
insertion_order.manifest.json
```

Checksum：

```text
insertion_order.ids sha256 = 7888c4f1109bdff8d98954c607c87d25295c98ca72fece15d3917854d9d63a5b
```

为什么不直接使用 B-scale-0 collection 顺序：

```text
B-scale-0 的 collection.10k.tsv 为了保证 selected-query positive 全部保留，
先放置全部 positive pid，再补齐 non-positive。
如果直接使用该顺序构建 HNSW，会把 qrels/positive 分布注入插入顺序。
B-scale-3 的插入顺序必须与 qrels 无关，因此用 source_revision + external_pid 稳定哈希重排。
```

core_hnsw_v2 和 FAISS HNSW 使用同一 `insertion_order.ids`。FAISS internal label 会映射回 canonical numeric `record_id` 后再计算 recall；exact ground truth 仍使用 numeric `record_id`。

## 7. backend、参数和环境

HNSW 参数：

```text
M = 48
M0 = 96
efConstruction = 512
efSearch sweep = 32,64,128,256,512,1024
topK = 10
metric = ip_normalized
```

FAISS：

```text
IndexFlatIP exact parity
IndexHNSWFlat(dim=512, M=48, METRIC_INNER_PRODUCT)
actual level-0 neighbor capacity = 96
```

计时：

```text
warmup_rounds = 1
measured_rounds = 5
latency sample = single query search
cache_load_us = 16425
```

环境：

```text
CPU = AMD EPYC 9654 96-Core Processor
logical_cpus_visible = 4
compiler = 11.4.0
optimization_flags = -O2
FAISS version = 1.14.3
core_hnsw_v2 implementation = kvs_hnsw_v2_index
OMP_NUM_THREADS = 1
OPENBLAS_NUM_THREADS = 1
MKL_NUM_THREADS = 1
FAISS thread_count = 1
core_hnsw_v2 search threads = 1
CPU affinity = not_set_by_runner
peak_rss_before_kb = 48772
peak_rss_after_kb = 181896
```

## 8. 输出 artifact

输出目录：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_3_10k_benchmark
```

| file | bytes | sha256 |
|---|---:|---|
| `benchmark_manifest.json` | 2858 | `a41a80b3557a4442be56ae2e0caae98a00ede9fcfc1c052da83e5bb66ddb589d` |
| `insertion_order.ids` | 48890 | `7888c4f1109bdff8d98954c607c87d25295c98ca72fece15d3917854d9d63a5b` |
| `insertion_order.manifest.json` | 330 | `a73edf7481ab32e0aee7331250c3f018c9c315e091f7a1eed030a6cbf141e4cd` |
| `core_hnsw_v2_results.json` | 3193 | `38460b81a185db34fe3eff86404e2c1b162139c37428641119e4a99272330c22` |
| `core_hnsw_v2_per_query.jsonl` | 817462 | `884b68cd80350feba9eddc3eb40d6060a093bfee5fc80ee1c20f898a67f5d0c5` |
| `faiss_flat_results.json` | 1332 | `51b9021113d768bda673e3d617b1f094e74de1d606a078f746044521da6226b2` |
| `faiss_flat_per_query.jsonl` | 129473 | `04dae95f6f9db2c2c245bc8b0aa7927df1b0f53c59728c8ebb4b7f1bd143892d` |
| `faiss_hnsw_results.json` | 3195 | `bafb86165ee4738847a83a21a0ee32728b8a18b67bddd758460cd1ae372bae30` |
| `faiss_hnsw_per_query.jsonl` | 815039 | `fb91886914629e6831601be48ff4d4e86d9e6b29a5dd53c9c9de679f77089dd3` |

## 9. FAISS Flat parity

FAISS `IndexFlatIP` 只作为 exact parity sanity check：

```text
strict_parity = yes
recall@1 = 1.000000
recall@10 = 1.000000
topk_exact_match_rate = 1.000000
max_score_abs_diff = 1.01327896e-06
build_us = 12389
search_avg_us = 856.175
p50_us = 850
p95_us = 866
p99_us = 924
qps = 1167.986
serialized_index_bytes = 20480045
serialized_index_bytes_source = storage_estimate
memory_bytes_source = unsupported
```

## 10. core_hnsw_v2 sweep

Build / stats：

```text
build_us = 37560662
build_avg_us_per_record = 3756.066
memory_bytes = 85224384
memory_bytes_source = estimated
size = 10000
active_count = 10000
deleted_count = 0
actual M0 = 96
```

| efSearch | recall@1 | recall@10 | exact topK | min recall@10 | avg_us | p50_us | p95_us | p99_us | QPS |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 | 1.0000 | 0.9955 | 0.9600 | 0.8000 | 134.316 | 133 | 187 | 208 | 7445.129 |
| 64 | 1.0000 | 0.9990 | 0.9900 | 0.9000 | 222.952 | 220 | 304 | 343 | 4485.270 |
| 128 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 368.761 | 363 | 483 | 552 | 2711.784 |
| 256 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 610.773 | 604 | 793 | 871 | 1637.269 |
| 512 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1043.133 | 1043 | 1270 | 1350 | 958.651 |
| 1024 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 1910.635 | 1908 | 2134 | 2215 | 523.386 |

## 11. FAISS HNSW sweep

Build / stats：

```text
build_us = 6458684
build_avg_us_per_record = 645.868
requested M = 48
actual level-0 neighbor capacity = 96
serialized_index_bytes = 24476898
serialized_index_bytes_source = storage_estimate
memory_bytes_source = unsupported
```

| efSearch | recall@1 | recall@10 | exact topK | min recall@10 | avg_us | p50_us | p95_us | p99_us | QPS |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 | 1.0000 | 0.9860 | 0.9000 | 0.7000 | 87.454 | 86 | 108 | 123 | 11434.583 |
| 64 | 1.0000 | 0.9960 | 0.9700 | 0.8000 | 157.255 | 157 | 193 | 215 | 6359.098 |
| 128 | 1.0000 | 0.9990 | 0.9900 | 0.9000 | 277.375 | 275 | 339 | 369 | 3605.228 |
| 256 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 503.122 | 502 | 596 | 629 | 1987.589 |
| 512 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 946.343 | 945 | 1080 | 1146 | 1056.699 |
| 1024 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 2007.276 | 2010 | 2174 | 2274 | 498.188 |

## 12. 内存和 storage 口径

`core_hnsw_v2` 的 `memory_bytes=85224384` 来自 KVLite core info，是 estimated 口径。FAISS runtime memory 不可靠获取，本阶段不伪造 `memory_bytes`，只记录 `serialized_index_bytes` 作为 storage estimate：

```text
FAISS Flat serialized_index_bytes = 20480045
FAISS HNSW serialized_index_bytes = 24476898
```

这些指标口径不同，不能直接当作 runtime memory parity 比较。

## 13. qrels 使用说明

T2Ranking qrels 文件：

```text
/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_0/qrels.retrieval.dev.200.tsv
sha256 = 97221f7ca305c817e41da2e1cee3e136b38e3fb563586b2ed5e06cba7563af1f
positive_pairs = 1131
```

qrels 未参与插入顺序、ANN recall、FAISS Flat parity、HNSW quality 计算或 ground truth 校验。本阶段 recall 只表示 ANN topK 与 B-scale-2 exact embedding topK 的交集比例，不是 semantic MRR/NDCG。

## 14. 重复运行确定性

重复运行同一 generate 命令：

```text
[publish] output_dir=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_3_10k_benchmark status=unchanged core_outputs_identical=true
```

验证项：

- `insertion_order.ids` byte-identical。
- FAISS Flat per-query byte-identical。
- core_hnsw_v2 每个 efSearch 的 returned IDs / recall byte-identical。
- FAISS HNSW 每个 efSearch 的 returned IDs / recall byte-identical。
- build/search timing 不参与 deterministic checksum。

## 15. 测试结果

```text
$ make gen-embedding-hnsw-faiss-sweep-benchmark
PASS

$ tests/vector_real_embedding_hnsw_faiss_sweep_smoke_test.sh
=== B-scale-3 HNSW/FAISS sweep smoke test: ALL CHECKS PASSED ===

$ tests/vector_real_embedding_hnsw_v2_benchmark_smoke_test.sh
=== B3 core_hnsw_v2 index-only benchmark smoke test: ALL CHECKS PASSED ===

$ tests/vector_real_embedding_faiss_benchmark_smoke_test.sh
=== B4 FAISS index-only benchmark smoke test: ALL CHECKS PASSED ===

$ build/tools/bin/generate_real_embedding_cache validate \
    --dataset-id b_scale_1_10k_embedding \
    --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding
[validate] PASS dataset_id=b_scale_1_10k_embedding record_count=10000 query_count=200 dim=512

$ build/tools/bin/generate_real_embedding_groundtruth validate \
    --cache-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding \
    --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth
[validate] PASS dataset_id=b_scale_1_10k_embedding query_count=200 topk=10 record_count=10000 dim=512

$ build/tools/bin/generate_real_embedding_hnsw_faiss_sweep_benchmark validate \
    --cache-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_1_10k_embedding \
    --groundtruth-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_2_10k_groundtruth \
    --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_3_10k_benchmark \
    --dataset-id b_scale_1_10k_embedding \
    --top-k 10
[validate] PASS output_dir=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_3_10k_benchmark record_count=10000 query_count=200 topk=10
```

Smoke 覆盖：

- 外部 cache / groundtruth / output 路径。
- shared insertion order permutation。
- efSearch sweep 输出完整。
- FAISS Flat strict parity。
- repeated generation deterministic。
- duplicate insertion order validate 失败。
- bad cache checksum 失败。
- bad ground truth schema/count 失败。
- 旧 B3/B4 1K smoke 无回归。

## 16. 当前限制和风险

- 本阶段只覆盖 T2Ranking 10K index-only benchmark，不证明 100K real embedding recall 或 build/search SLA。
- `recall@1=1.0` 在本数据上同时出现在较低 efSearch，不代表所有数据集 top1 都稳定。
- `core_hnsw_v2` 和 FAISS HNSW 的 runtime memory 口径不同，只能并列记录，不能直接比较。
- FAISS / core build 时间是当前机器单次运行结果；后续 scale 阶段应保留同一 runner 和环境字段，再做多规模对比。
- 本阶段没有运行 product VSET / VSEARCH e2e，因此不能推导产品端响应延迟。

## 17. 下一阶段

推荐进入：

```text
B-scale-4 — T2Ranking 100K Real Embedding Cache
```

B-scale-4 应继续复用 B-scale-0 的 `collection.100k.tsv` 和 `queries.dev.200.tsv`，生成 100K real embedding cache；不要从 B-scale-3 结果推导 100K benchmark SLA。

## 18. Decision block

```text
B-scale-3 decision:
  production_code_changed = no
  benchmark_tools_changed = yes
  product_behavior_changed = no
  tests_changed = yes
  makefile_changed = yes
  docs_changed = yes
  b_scale_1_cache_validation_passed = yes
  b_scale_2_groundtruth_validation_passed = yes
  onnx_inference_rerun = no
  groundtruth_regenerated = no
  qrels_used_for_ann_recall = no
  shared_insertion_order_added = yes
  core_hnsw_v2_used = yes
  faiss_flat_ip_used = yes
  faiss_hnsw_flat_used = yes
  record_count = 10000
  query_count = 200
  dim = 512
  topk = 10
  ef_search_sweep_complete = yes
  deterministic_repeat_passed = yes
  product_e2e_benchmark_run = no
  artifacts_outside_repo = yes
  ready_for_B_scale_4 = yes
  recommended_next = B-scale-4-T2Ranking-100K-Real-Embedding-Cache
```
