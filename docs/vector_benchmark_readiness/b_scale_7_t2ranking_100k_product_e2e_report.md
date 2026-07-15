# B-scale-7: T2Ranking 100K Product E2E Benchmark

## 1. 范围与 roadmap 对齐

已读取并遵守根目录 `AGENTS.md`。原 roadmap 的 B7 是 `core_hnsw_v2` product VSET/VSEARCH/VINFO E2E；本阶段按最新执行约束拆成：

- B-scale-7A：prepared-with-embedding，只绕过 tokenizer/ONNX；RESP parse、command dispatch、record commit、ID mapping、index ops、manager resolve、result build 和 response encode 均走正常产品链路。
- B-scale-7B：在同一已加载产品索引上使用公开文本 `VSEARCH`，执行真实 tokenizer/ONNX 和完整后续链路。

未修改公开协议、VSEARCH response、`kvs_vector_index_ops_t`、RDB/AOF 格式、默认 backend、图算法或冻结参数。未重新生成 embedding/ground truth，未执行产品并发或参数调优。

## 2. 产品 backend 审计

`core_hnsw_v2` 已通过 current index ops 接入 VSET/VSEARCH/VINFO。产品代码没有 FAISS ops/source；`faiss`、`faiss_hnsw`、`faiss_flat` 只保留 compatibility name 并 fallback 到 `core_flat`，VINFO FAISS 字段固定 disabled/0。恢复 FAISS 需要 add/search、external-ID mapping、update/delete、rebuild、lifecycle 和 persistence 边界的完整产品实现，属于情况 C，不是薄 experimental adapter。

因此本阶段完成 core B-scale-7A/7B；不以 B-scale-6 standalone FAISS API 冒充产品链路，也不恢复 FAISS product backend。

## 3. Prepared hook 与调用链

新增 opt-in server `build/tools/bin/kvstore_vector_product_e2e_benchmark`。它在独立 object tree 使用 ONNX runtime，并用 GNU linker `--wrap` 包装：

```text
kvs_vector_set_prepare
kvs_vector_search_prepare
```

fixture-tagged VSET/VSEARCH 从 B-scale-4 mmap cache 调用已有 `*_prepare_with_embedding()`；普通文本 VSEARCH fall through 到真实 `kvs_vector_search_prepare()`。之后均为：

```text
RESP parser -> command task/dispatch -> prepared commit/search
-> manager record store/current index ops -> record_id resolve/filter
-> result build -> unchanged RESP response
```

私有环境变量和仓库外 control file 只用于 cache path、冻结 HNSW seed、efSearch sweep、prepare-path audit；没有新增 RESP command。server 以 disposable cwd/data directory 运行，避免读取开发目录的 RDB/snapshot。

## 4. 输入与正式配置

```text
cache=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_4_100k_embedding
groundtruth=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_5_100k_groundtruth
index_baseline=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_6_100k_benchmark
output=/home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_7_100k_product_e2e

records=100000, queries=200, dim=512, topK=10
M=48, M0=96, efConstruction=512
efSearch=32,64,128,256,512,1024
warmup=1, measured rounds=5
single server, single RESP client, normalized IP
insertion order SHA-256=a4f237e3e908fa3407c57be92fb4ab84d0eabbc849a1a92837dce3d2423fafde
```

AOF disabled，fsync mode `no`，replication 未连接。RDB 编译能力保持开启；graceful shutdown 生成的 disposable RDB 为 `540018950 bytes` / SHA-256 `8d78593bccda109aa3f5e80587edbb31b8543222c4b3377ca1602687ded7dae8`，记录后从正式 artifact 清理。运行未修改 RDB 格式。

正式命令：

```bash
python3 scripts/benchmark/run_t2ranking_product_e2e.py generate \
  --cache-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_4_100k_embedding \
  --groundtruth-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_5_100k_groundtruth \
  --index-baseline-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_6_100k_benchmark \
  --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_7_100k_product_e2e

python3 scripts/benchmark/run_t2ranking_product_e2e.py validate \
  --output-dir /home/zhengpan/share/benchmark_artifacts/T2Ranking/b_scale_7_100k_product_e2e
```

## 5. 100K load 与 correctness defect

首次运行暴露：record store `active_records=100000`，但 v2 `size=34480`，recall@10 约 0.33。代码审计确认 `kvs_hnsw_v2_index_reserve()` 是 destructive reserve，add-time capacity growth 会清空已有图；而已有 `vector_hnsw_initial_capacity` 只被 parse/log，未用于初始化。

最小修复让该已有配置在 manager store 和 current index 首次 reserve 前生效。未修改 reserve、add、search、graph、termination 或 neighbor-selection 语义。runner 新增 `active_records == core_hnsw_v2_size == core_hnsw_v2_active_count == 100000` 硬门禁；smoke 新增 initial-capacity/recall 断言。失败结果保留在仓库外 `_failed_capacity_reset` sibling，未作为正式结果。

修复后正式 load：

```text
wall_us=740825676 (740.826s)
records_per_second=134.985
failure_count=0
active_records=size=active_count=capacity=100000
index_memory_bytes=851194400 (VINFO source=measured)
record/vector memory_bytes=568010344
total_vector_memory_bytes=1419204744
server peak RSS=2343708 KiB
```

B-scale-6 index-only build 为 `678326503us`；产品 load/build 为 `1.092x`，增加 `62499173us`。该差值包含 RESP 输入、文本/record commit、ID maps 和产品 add dispatch，不是纯图构建成本。

## 6. B-scale-7A prepared VSEARCH

client E2E 与 VINFO internal timing：

| ef | recall@10 | avg us | p50 | p95 | p99 | QPS | server avg us | backend avg us | resolve us | response us |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 | 0.9870 | 699.918 | 677.081 | 940.220 | 1071.043 | 1428.738 | 567.890 | 505.639 | 5.074 | 56.936 |
| 64 | 0.9905 | 913.090 | 881.653 | 1255.045 | 1379.158 | 1095.183 | 769.200 | 704.016 | 5.312 | 59.470 |
| 128 | 0.9985 | 1372.726 | 1320.452 | 1933.107 | 2147.554 | 728.478 | 1228.487 | 1164.887 | 5.233 | 57.997 |
| 256 | 0.9995 | 2235.441 | 2150.148 | 3192.199 | 3557.348 | 447.339 | 2082.436 | 2017.060 | 5.247 | 59.725 |
| 512 | 1.0000 | 3609.656 | 3520.274 | 5087.220 | 5830.157 | 277.035 | 3443.524 | 3376.513 | 5.396 | 61.095 |
| 1024 | 1.0000 | 6043.814 | 5884.419 | 8169.497 | 9290.573 | 165.458 | 5855.077 | 5786.277 | 5.589 | 62.559 |

prepare-path audit 在 7A 完成时为 `prepared_vset=100000`、`prepared_vsearch=7200`、`public_vsearch=0`，证明 load/7A 未执行 ONNX。产品 scope overfetch factor 为 1，但 adapter raw topK 为 42，因此 ef32 的实际 candidate floor 至少为 42；ef32 不与 B-scale-6 raw topK=10 / ef32 解释为纯同 ef kernel parity。

## 7. B-scale-7B public text + ONNX

同一索引、同一 query text 使用公开 VSEARCH：

| ef | recall@10 | avg us | p50 | p95 | p99 | QPS | server avg us | backend avg us |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 | 0.9870 | 5143.185 | 5018.569 | 6538.406 | 7105.985 | 194.432 | 724.618 | 655.135 |
| 64 | 0.9905 | 5342.179 | 5244.684 | 6766.583 | 7118.855 | 187.190 | 941.442 | 871.885 |
| 128 | 0.9985 | 6018.167 | 5901.716 | 7473.030 | 8495.741 | 166.164 | 1525.282 | 1453.849 |
| 256 | 0.9995 | 6820.877 | 6694.850 | 8352.711 | 9179.464 | 146.609 | 2364.288 | 2294.324 |
| 512 | 1.0000 | 8237.127 | 8100.168 | 10175.027 | 11016.521 | 121.402 | 3771.769 | 3701.241 |
| 1024 | 1.0000 | 10656.979 | 10468.204 | 13191.750 | 14223.138 | 93.835 | 6165.870 | 6094.192 |

`runtime_encode_count=7200`，等于 `6 ef * (1 warmup + 5 measured) * 200 queries`。每个 ef 的 prepared/public returned-ID checksum 完全相同，说明公开文本 ONNX 输出与 B-scale-4 frozen query cache 对齐，且 7B 只增加 embedding 路径成本。

## 8. B-scale-6 overhead 与参数判断

prepared product / index-only：

| ef | index-only avg us | server avg us | client avg us | server overhead us | total overhead us | client/index ratio | recall delta |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 | 390.248 | 567.890 | 699.918 | 177.642 | 309.670 | 1.794x | +0.0060 |
| 64 | 650.426 | 769.200 | 913.090 | 118.774 | 262.664 | 1.404x | -0.0005 |
| 128 | 1119.607 | 1228.487 | 1372.726 | 108.880 | 253.119 | 1.226x | 0 |
| 256 | 1875.470 | 2082.436 | 2235.441 | 206.966 | 359.971 | 1.192x | 0 |
| 512 | 3217.314 | 3443.524 | 3609.656 | 226.210 | 392.342 | 1.122x | 0 |
| 1024 | 5484.760 | 5855.077 | 6043.814 | 370.317 | 559.054 | 1.102x | 0 |

public ONNX total product overhead 为 `4752.937-5172.219us`，主要是 tokenizer/ONNX；VINFO server timing 从 embedding prepare 之后开始，不能把 client-server 差值全部解释为网络。

- recall>=0.99：首次 ef64，prepared `913.090us`，public `5342.179us`。
- recall>=0.998：首次 ef128，prepared `1372.726us`，public `6018.167us`。
- recall=1.0：首次 ef512，prepared `3609.656us`，public `8237.127us`。
- ef128 是当前高 recall/latency 平衡点；ef256 将 recall 提高到 0.9995；ef512 是冻结 sweep 中的 exact-recall 点，代价明显更高。本阶段不据此修改产品默认参数。

FAISS 没有产品结果，不能回答 FAISS 产品端同 ef/同 recall/完全召回排序；B-scale-6 的 FAISS 数字仍仅是 index-only baseline。

## 9. Correctness、validator 与 artifact

runner validator 和独立全量 per-query 复算均通过：

```text
query_count=200, hits_per_query=10, measured per-query rows=12000
summary/per-query recall match=true
prepared/public IDs and order match=true
duplicate_id_count=0
missing_query_count=0
nonfinite_score_count=0
score_order_violation_count=0
```

正式文件：

| file | bytes | SHA-256 |
|---|---:|---|
| `product_e2e_manifest.json` | 2351 | `8a65538fe25d2e9c156c59455b1854fa877f542768338589c180ddd76b97fa72` |
| `product_e2e_result.json` | 32303 | `a64b2988541c658b9ffe6535979741a2cf85f82803ebe8d36fbe9bd60341f8e3` |
| `product_e2e_per_query.jsonl` | 5670125 | `e39059733971c41e9c6b3023de250ba811a5640838cb4ce380f30b1fc1f31793` |
| `benchmark.conf` | 1161 | `05e1247b164f690c53b47fd7f9ab305d1738d413933708ebdd6b2e127953bc3c` |
| `prepare_path_audit.txt` | 81 | `e076fcb8734cf1d16a6d2d66d39d02074f46ca876aa8b94efdb7a0b4060911e5` |
| `server.log` | 10579 | `61b8f56941d315437d895172a7f2ff59144907dca8e165c92b4d668c72220f3d` |
| `server.time.txt` | 944 | `62d1aa7c1c142b9149a5dc06e7b90c7c49f99d7f77ed47e71adc4462db1167d8` |

## 10. 测试与边界检查

通过：

```text
B-scale-4 cache validator
B-scale-5 ground-truth validator
B-scale-6 index-only validator
make product-e2e-benchmark-server
tests/vector_t2ranking_product_e2e_smoke_test.sh
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_core_hnsw_v2_product_integration_test.sh
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_vinfo_backend_stats_test.sh
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_v2_graph_construction_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh
tests/vector_index_snapshot_vinfo_status_test.sh
product runner validator + independent 12000-row audit
```

AGENTS 中列出的四个 legacy `tests/vector_core_hnsw_*` shell 脚本在当前仓库不存在；已执行当前 v2 对应回归。static boundary grep 无 core 对 `g_vector_state` 或 manager terms 的新增引用；index ops 签名未改。

## 11. 限制、决策与下一阶段

本结果是 100K、单实例、单客户端、单次正式 load 和五轮 query latency，不是并发 SLA。`vector_hnsw_initial_capacity=100000` 是本次 correctness 前提；v2 destructive dynamic reserve 对未知/超过预留规模仍是已知风险，本阶段不扩展为 storage/layout 重构。

```text
B-scale-7 decision:
  production_code_changed = yes
  public_protocol_changed = no
  vsearch_response_changed = no
  index_ops_changed = no
  rdb_aof_format_changed = no
  benchmark_tools_changed = yes
  tests_changed = yes
  core_product_e2e_complete = yes
  prepared_path_only_bypasses_tokenizer_onnx = yes
  public_text_onnx_path_complete = yes
  faiss_product_backend_available = no
  faiss_product_e2e_executed = no
  record_index_count_match = yes
  full_sweep_complete = yes
  validator_independent_audit_passed = yes
  artifacts_outside_repo = yes
  ready_for_B_scale_8 = yes
```

下一阶段按 roadmap 进入 B-scale-8；FAISS product parity 如需推进，必须单独立项，不能作为 core 主线的隐式 adapter 扩展。
