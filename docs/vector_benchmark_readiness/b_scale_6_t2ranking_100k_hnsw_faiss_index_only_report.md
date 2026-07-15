# B-scale-6: T2Ranking 100K core_hnsw_v2 / FAISS index-only benchmark

## 1. 范围与 runner 审计

本阶段读取 B-scale-4 100K embedding cache 和 B-scale-5 exact ground truth，只运行 `core_hnsw_v2`、FAISS `IndexFlatIP` parity 和 `IndexHNSWFlat` index-only benchmark。未调用 ONNX，未重新生成 embedding/ground truth，未经过 VSET/VSEARCH、网络、响应序列化或产品 e2e，未修改 HNSW 图算法、参数、产品向量代码、RDB/AOF 或公开接口。

对 B-scale-3 combined runner 的审计结论：

- 已原生支持外部 cache、ground-truth、output 目录；record/query/dim/topK 来自 manifest 和文件大小验证，无 10K count 硬编码。
- 两个 HNSW backend 共用 `SHA256(source_revision + "\thnsw-insertion\t" + external_pid)` 插入顺序、相同输入、参数、warmup/round 和 recall 口径。
- 冻结参数和完整 `efSearch=32,64,128,256,512,1024` sweep 已支持；单线程设置明确。
- core 正式 search hot-loop profiling 默认并实际关闭；轻量 search work counters 保留。
- 原 runner 的缺口是 core/FAISS 全部成功后才一起发布。一个 backend 失败会丢失另一个已完成结果，不满足 100K 中断恢复要求。

## 2. 最小 runner 修改

修改：

```text
scripts/benchmark/generate_real_embedding_hnsw_faiss_sweep_benchmark.cpp
tests/vector_real_embedding_hnsw_faiss_sweep_smoke_test.sh
```

runner 新增 `--backend all|core|faiss|finalize`。core 和 FAISS 分别写入正式目录旁的 `<output>.work`，输出文件完成 checksum 后再以 backend checkpoint marker 收口。marker 签名覆盖输入 manifest、ground truth manifest、插入顺序、数据 shape、HNSW 参数、ef list；core 还覆盖 profiling/buffer mode。重试只复用签名和文件 SHA-256 均匹配的 backend。`finalize` 要求两侧 checkpoint 有效，生成总 manifest，在 work 目录运行 validator，删除内部 marker/temp 后整目录原子 rename；正式目录此前不存在。

结果 schema 保持版本 1，兼容增加 `build_cpu_us`、process peak RSS、index size 和 core graph stats。validator 现在校验 summary 与全部 per-query 文件的 manifest SHA-256。smoke 增加 partial finalize 拒绝、core checkpoint 复用、分 backend finalize 和 per-query corruption 拒绝。

FAISS 第一次正式运行在 HNSW build 前被 Flat parity 拒绝。唯一差异是 query 67 的第 8/9 名：两个 float32 score 完全相同，FAISS 原始 tie 顺序为 `69789,27739`，B-scale-5 冻结 numeric ID tie-break 为 `27739,69789`。runner 只在 Flat parity 的计时区间外对相等 score 应用 numeric record ID 升序；最大 score absolute difference 为 `8.34465027e-07`，低于 `1e-4` tolerance。失败日志保留，core checkpoint checksum 未变化，仅重跑 FAISS。

## 3. 输入与固定配置

```text
cache       = /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_4_100k_embedding
groundtruth = /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_5_100k_groundtruth
output      = /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_6_100k_benchmark
records=100000, queries=200, dim=512, topK=10
metric=normalized inner product
M=48, M0=96, efConstruction=512
efSearch=32,64,128,256,512,1024
warmup=1, measured rounds=5, thread_count=1
source revision=2a369a430a70979223f1b9a41b1919774d46b432
```

输入 checksum：

| input | SHA-256 |
|---|---|
| B-scale-4 `manifest.json` | `f817924c115eef731d7f1e7d0ca94661a4e753715a4b025639fa3746393e1ce1` |
| `embeddings.f32` | `970ce35b4fd60e82f9ed06a81ef97da5fa02ba97cd4ae848f76ad10293ea0994` |
| `query_embeddings.f32` | `2b1d3e5952051dcf4eee938b6edf3454fef1660f16aa0e319d9e3d53e8dca318` |
| B-scale-5 `groundtruth.jsonl` | `7383a3803ab3e2caea8f85e7cef28bfb8982ff7b3f3e8251e072ffb5b86e9cbb` |
| B-scale-5 manifest | `8bc82b9bb4e258eda0d8d70811059f0662c84c5b56f06197d965d8391ba1340c` |
| insertion order | `a4f237e3e908fa3407c57be92fb4ab84d0eabbc849a1a92837dce3d2423fafde` |

B-scale-4 cache validate 和 B-scale-5 ground-truth validate 均通过。FAISS/core 使用同一份 `insertion_order.ids`；qrels 未用于 ANN recall。

## 4. 实际命令

正式 backend 分开运行并各自使用 `/usr/bin/time -v`：

```bash
build/tools/bin/generate_real_embedding_hnsw_faiss_sweep_benchmark generate \
  --cache-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_4_100k_embedding \
  --groundtruth-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_5_100k_groundtruth \
  --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_6_100k_benchmark \
  --backend core \
  --source-revision 2a369a430a70979223f1b9a41b1919774d46b432 \
  --top-k 10 --ef-search-list 32,64,128,256,512,1024 \
  --core-build-profiling on --core-search-profiling off \
  --core-hit-buffer-reuse off

# 同输入、output、revision、topK 和 ef list
build/tools/bin/generate_real_embedding_hnsw_faiss_sweep_benchmark generate --backend faiss ...
build/tools/bin/generate_real_embedding_hnsw_faiss_sweep_benchmark generate \
  --backend finalize ... --core-build-profiling on --core-search-profiling off
build/tools/bin/generate_real_embedding_hnsw_faiss_sweep_benchmark validate ...
```

## 5. Build 与 index stats

| backend | build wall us | build CPU us | us/record | process peak RSS | build ratio |
|---|---:|---:|---:|---:|---:|
| core_hnsw_v2 | 678,326,503 | 677,430,191 | 6,783.265 | 1,050,732 KiB | 3.142x FAISS |
| FAISS IndexHNSWFlat | 215,897,392 | 214,753,355 | 2,158.974 | 1,136,584 KiB | 1.000x |

Peak RSS 使用 backend 独立进程的 `/usr/bin/time -v`；FAISS result 中 build 点 RSS 为 `857,780 KiB`，整个进程 peak 包含 sweep 和 index serialization。两阶段均无 swap，退出码为 0。

core graph/index：

```text
size=active_count=capacity=100000
deleted_count=0
duplicate_edge_count=0
degree_overflow_count=0
memory_bytes=851194400 (estimated)
graph_sha256=9234d541f7c355ec6ade4fbf44c2d912bfb4d28ae318e4c44193c7fb3608d7ad
distance_compute_count=7110803523
construction_layer_search_us=214469634
neighbor_select_us=353243837
reverse_prune_us=414842459
temporary_allocation_count=399996
resize_count=0
```

FAISS：

```text
ntotal=100000
requested M=48
actual level-0 neighbor capacity=96
efConstruction=512
serialized_index_bytes=244818402 (storage estimate, not runtime memory)
runtime memory_bytes=unsupported
```

## 6. 完整 search sweep

| backend | efSearch | recall@1 | recall@10 | min query recall | avg us | p50 | p95 | p99 | QPS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| core | 32 | 1.0000 | 0.9810 | 0.7 | 390.248 | 372 | 587 | 765 | 2,562.473 |
| core | 64 | 1.0000 | 0.9910 | 0.7 | 650.426 | 619 | 967 | 1,156 | 1,537.454 |
| core | 128 | 1.0000 | 0.9985 | 0.9 | 1,119.607 | 1,050 | 1,676 | 1,864 | 893.171 |
| core | 256 | 1.0000 | 0.9995 | 0.9 | 1,875.470 | 1,795 | 2,744 | 3,083 | 533.200 |
| core | 512 | 1.0000 | 1.0000 | 1.0 | 3,217.314 | 3,126 | 4,636 | 5,347 | 310.818 |
| core | 1024 | 1.0000 | 1.0000 | 1.0 | 5,484.760 | 5,332 | 7,532 | 8,773 | 182.323 |
| FAISS | 32 | 0.9850 | 0.9715 | 0.4 | 266.106 | 260 | 356 | 468 | 3,757.901 |
| FAISS | 64 | 0.9950 | 0.9880 | 0.7 | 470.766 | 463 | 615 | 690 | 2,124.198 |
| FAISS | 128 | 0.9950 | 0.9955 | 0.8 | 834.914 | 819 | 1,069 | 1,170 | 1,197.728 |
| FAISS | 256 | 1.0000 | 0.9985 | 0.9 | 1,506.924 | 1,483 | 1,921 | 2,080 | 663.603 |
| FAISS | 512 | 1.0000 | 1.0000 | 1.0 | 2,748.486 | 2,707 | 3,483 | 4,002 | 363.837 |
| FAISS | 1024 | 1.0000 | 1.0000 | 1.0 | 5,147.052 | 5,091 | 6,335 | 6,833 | 194.286 |

core 平均 search work：

| efSearch | distance computes | visited nodes | neighbor scans | final top |
|---:|---:|---:|---:|---:|
| 32 | 1,639.350 | 1,447.450 | 3,197.280 | 32 |
| 64 | 2,660.085 | 2,468.185 | 6,222.240 | 64 |
| 128 | 4,431.455 | 4,239.555 | 12,331.200 | 128 |
| 256 | 7,342.510 | 7,150.610 | 24,598.080 | 256 |
| 512 | 12,052.000 | 11,860.100 | 49,160.640 | 512 |
| 1024 | 19,318.820 | 19,126.920 | 98,306.880 | 1,024 |

hot-loop profiling 关闭，因此 `avg_final_sort_us=0` 是“未计时”，不是 sort cost 为 0。

## 7. 同 efSearch、同 recall 与 10K 对比

同 efSearch 下 core recall@10 始终不低于 FAISS；core avg latency / FAISS avg latency 从 ef32 的 `1.467x` 随 ef 增大降至 ef1024 的 `1.066x`。差值：

| efSearch | recall delta core-FAISS | avg latency core/FAISS |
|---:|---:|---:|
| 32 | +0.0095 | 1.467x |
| 64 | +0.0030 | 1.382x |
| 128 | +0.0030 | 1.341x |
| 256 | +0.0010 | 1.245x |
| 512 | 0 | 1.171x |
| 1024 | 0 | 1.066x |

以 `recall@10 >= 0.99` 为门槛，core 首次在 ef64 达到 `0.9910 / 650.426 us`；FAISS 首次在 ef128 达到 `0.9955 / 834.914 us`。首次完全召回均为 ef512：core `3,217.314 us`，FAISS `2,748.486 us`，core 慢 `1.171x`。没有非单调 recall。

相对 B-scale-3 10K 正式结果：

- core build 从 `37,560,662` 增至 `678,326,503 us`，为 `18.06x`；FAISS 从 `6,458,684` 增至 `215,897,392 us`，为 `33.43x`。build gap 从 10K 的 `5.82x` 收窄为 100K 的 `3.14x`，不能据此外推更大规模。
- 同 efSearch 的 avg search latency，core 为 10K 的约 `2.87x-3.08x`，FAISS 为约 `2.56x-3.04x`。
- 10K 首次完全召回是 core ef128、FAISS ef256；100K 两者均移至 ef512。低 ef recall 随规模下降，证明 1K/10K 结果不能替代 100K gate。

## 8. 正确性与 artifact

runner validator 和独立原始行审计均通过：

```text
query_count=200
hits_per_query=10
core per-query rows=1200
FAISS HNSW per-query rows=1200
duplicate_id_count=0
missing_query_count=0
nonfinite_score_count=0
score_order_violation_count=0
summary/per-query recall match=true
FAISS Flat strict rank match=true
FAISS Flat max score abs diff=8.34465027e-07
```

相同 score 的实际 tie 行为：core 的 6 个相邻 tie 均为 non-ascending numeric ID；FAISS HNSW 的 6 个相邻 tie 均为 ascending numeric ID。两者 score 均为 descending，recall 按 B-scale-5 topK ID set 计算，因此 tie 顺序不改变 recall。FAISS Flat parity 输出按冻结 exact tie-break canonicalize，strict rank 200/200 通过。

正式 artifact：

| file | bytes | SHA-256 |
|---|---:|---|
| `benchmark_manifest.json` | 2,863 | `88eef76427983d89a36d298fc825696892761eeabce9f7ccb4ce741e3e66581c` |
| `insertion_order.ids` | 588,890 | `a4f237e3e908fa3407c57be92fb4ab84d0eabbc849a1a92837dce3d2423fafde` |
| `insertion_order.manifest.json` | 331 | `0f7bf49f8a5151e04c2f874976f9eb9f478c2fc8324a2f09d24405517753ba56` |
| `core_hnsw_v2_results.json` | 5,695 | `d851d8babf82109d6a412103dd7e4d6080b04feafee386b2cd423819bde90b1d` |
| `core_hnsw_v2_per_query.jsonl` | 959,323 | `5585b0597e4e15b7569f730eecb67725cf3b8b8e60b4346122a709cdc4341aa7` |
| `faiss_flat_results.json` | 1,579 | `bac1444ae96d68ef8f82fd87432ad0853c2ed8e3909922cc09cd2a390acb23d0` |
| `faiss_flat_per_query.jsonl` | 131,903 | `59834aa419a6993d006c3e0c5acc3ea89ae4bacee74643178ce7fbc53dc65535` |
| `faiss_hnsw_results.json` | 4,219 | `578e6e97edaefcaa5ac66f4735cb90038bb0b7780eecee3ec9bb51f5f88ec491` |
| `faiss_hnsw_per_query.jsonl` | 829,679 | `1b743c760f4e8411c7c973845f3558f4f4cbafc3be6e2a83777878b7c9d8a114` |

Backend time/log、失败证据和独立 audit 位于 sibling `b_scale_6_100k_benchmark_logs/`，不在正式 manifest artifact 内。

## 9. 验证、限制与下一阶段

通过：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_real_embedding_hnsw_faiss_sweep_smoke_test.sh
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_v2_graph_construction_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
B-scale-4 cache validate
B-scale-5 ground-truth validate
B-scale-6 runner validate
independent per-query/checksum audit
git diff --check
```

100K 单次 build/search sweep 是当前机器上的 index-only 结果，不是多次 median、产品 e2e SLA 或参数最优性证明。core estimated memory 与 FAISS serialized storage bytes 口径不同，不直接比较。本阶段没有重新进入 B-scale-3B，也未根据结果调整算法或参数。

```text
B-scale-6 decision:
  production_code_changed = no
  product_behavior_changed = no
  benchmark_runner_changed = yes
  tests_changed = yes
  docs_changed = yes
  cache_validation_passed = yes
  groundtruth_validation_passed = yes
  core_search_profiling_disabled = yes
  shared_insertion_order_verified = yes
  full_sweep_complete = yes
  independent_audit_passed = yes
  artifacts_atomically_published = yes
  ready_for_B_scale_7 = yes
```

下一阶段名称和范围需按既定 roadmap 单独冻结；本阶段只给出 `ready_for_B_scale_7=yes`，不在此扩展实现。
