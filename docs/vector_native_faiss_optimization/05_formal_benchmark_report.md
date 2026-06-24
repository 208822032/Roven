# Formal Benchmark Report

Run id: `formal_20260624T042000Z`

Raw results:

- Index-only: `results/vector_native_faiss/index_only/formal_20260624T042000Z/raw.csv`
- Server-path: `results/vector_native_faiss/server_path/formal_20260624T042000Z/raw.csv`
- Backend availability: `results/vector_native_faiss/backend_availability_formal_20260624T042000Z.json`

## 1. 测试目标

本轮目标是在同一数据集参数下分层比较 KVLite 当前 native/hnswlib 路径与 Faiss 路径：

1. index-only：只比较底层 index add/search，不经过 RESP、server、executor、metadata lookup 或 response serialization。
2. server-path：通过真实 `kvstore` server、真实 RESP `VSET`/`VSEARCH`，只通过临时配置文件切换 backend。

Faiss 后端在本环境不可用，因此本报告不伪造 Faiss 数值；Faiss 行记录为 `unavailable`。

## 2. 测试环境

| Item | Value |
|---|---|
| Date | 2026-06-24 |
| Build | `make ENABLE_HNSWLIB=1 ENABLE_FAISS=0 -j 4` passed |
| Strict probe | `make ENABLE_FAISS=1 ENABLE_HNSWLIB=1 -j$(nproc)` failed |
| Strict build log | `results/vector_native_faiss/index_only/formal_20260624T042000Z/strict_build.log` |
| hnswlib source | `third_party/hnswlib/hnswlib/hnswlib.h` present |
| Faiss source | headers present |
| Faiss static lib | `third_party/faiss/build/faiss/libfaiss.a` missing |
| Server network mode | `reactor` |
| Embedding provider | `hash`, `embedding_dim=64` |
| Distance backend | `avx2` selected by server |

本轮为保证 server-path 的 `hnswlib_hnsw` 真正绑定到 hnswlib ops，补了最小 backend 切换点：`KVS_VECTOR_INDEX_HNSWLIB_HNSW` 绑定 hnswlib ops、handle 生命周期和日志/VINFO 识别。`formal_20260624T033500Z` 在修复前发现 hnswlib 配置仍落到 flat ops，已判定无效，不进入正式结论。

## 3. Backend 可用性

| Backend | Status | Notes |
|---|---|---|
| `native_flat` | available | built-in exact flat backend |
| `hnswlib_hnsw` / current HNSW | available | vendored `third_party/hnswlib` 可 include/link，server log 确认为 `index_type=hnswlib_hnsw` |
| `faiss_flat` | unavailable | Faiss headers present but `libfaiss.a` missing |
| `faiss_hnsw` | unavailable | Faiss headers present but `libfaiss.a` missing |
| `native_hnsw` | unavailable | legacy native HNSW 已移除，当前代码要求使用 `hnswlib_hnsw` |

## 4. 数据集与参数

数据集由 `scripts/benchmark/gen_vector_benchmark_dataset.py` 生成或复用：

| Dataset | N | nq | dim | seed | metric | normalize | topK | scope |
|---|---:|---:|---:|---:|---|---|---:|---|
| 10K/64d | 10000 | 1000 | 64 | 42 | ip | true | 10 | empty |
| 100K/64d | 100000 | 1000 | 64 | 42 | ip | true | 10 | empty |

HNSW 参数：

| Param | Value |
|---|---:|
| M | 32 |
| efConstruction | 256 |
| efSearch | 128 in config; current hnswlib ops uses `topK` as runtime ef minimum |

Server-path 使用真实 `VSET`/`VSEARCH`，但当前 RESP API 接收文本而非 binary float vectors，因此 server-path 使用同一 manifest 参数下的确定性文本/hash embedding；server-path 不报告 recall，不和 index-only recall 混用。

## 5. Index-only 结果

| Dataset | Backend | Operation | Avg us | P95 us | QPS | Recall@10 | Status |
|---|---|---|---:|---:|---:|---:|---|
| 10K/64d | native_flat | search | 323 | 331 | 3095.899 | 1.000000 | pass |
| 10K/64d | hnswlib_hnsw | add | 189 | 247 | 5273.544 |  | pass |
| 10K/64d | hnswlib_hnsw | search | 114 | 121 | 8724.633 | 0.994100 | pass |
| 100K/64d | native_flat | search | 3179 | 3196 | 314.495 | 1.000000 | pass |
| 100K/64d | hnswlib_hnsw | add | 457 | 731 | 2183.890 |  | pass |
| 100K/64d | hnswlib_hnsw | search | 337 | 367 | 2959.902 | 0.899700 | pass |

Faiss rows are `unavailable`; no Faiss latency or recall number is reported.

## 6. Server-path 结果

| Dataset | Backend | Operation | Avg us | P95 us | QPS | Success | Error | Timeout | Index avg us | Status |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| 10K/64d | native_flat | VSET | 60 | 70 | 16568.06 | 10000 | 0 | 0 | 0 | pass |
| 10K/64d | native_flat | VSEARCH | 170 | 200 | 5741.81 | 1000 | 0 | 0 | 0 | pass |
| 10K/64d | hnswlib_hnsw | VSET | 1150 | 1320 | 871.62 | 10000 | 0 | 0 | 1076 | pass |
| 10K/64d | hnswlib_hnsw | VSEARCH | 140 | 170 | 6890.28 | 1000 | 0 | 0 | 0 | pass |
| 100K/64d | native_flat | VSET | 60 | 70 | 16547.82 | 100000 | 0 | 0 | 0 | pass |
| 100K/64d | native_flat | VSEARCH | 1230 | 1300 | 815.40 | 1000 | 0 | 0 | 0 | pass |
| 100K/64d | hnswlib_hnsw | VSET | 1170 | 1360 | 854.90 | 100000 | 0 | 0 | 1095 | pass |
| 100K/64d | hnswlib_hnsw | VSEARCH | 170 | 190 | 6029.33 | 1000 | 0 | 0 | 0 | pass |

`index_avg_us=0` for VSEARCH 是当前 profiling 缺口，不表示 index search 没有耗时；本轮只可靠拿到了 hnswlib VSET 的 `vset_hnsw_add_avg_us`。

## 7. Native/hnswlib vs Faiss 差距倍数

Faiss 后端不可用，不能给出 native/hnswlib vs Faiss 的正式倍数。

当前可报告的替代对照是 `native_flat` vs `hnswlib_hnsw`：

| Layer | Dataset | Comparison | Ratio |
|---|---|---|---:|
| index-only search | 10K/64d | native_flat avg / hnswlib avg | 2.83x |
| index-only search | 100K/64d | native_flat avg / hnswlib avg | 9.43x |
| server VSEARCH | 10K/64d | native_flat avg / hnswlib avg | 1.21x |
| server VSEARCH | 100K/64d | native_flat avg / hnswlib avg | 7.24x |
| server VSET | 10K/64d | hnswlib avg / native_flat avg | 19.17x slower |
| server VSET | 100K/64d | hnswlib avg / native_flat avg | 19.50x slower |

## 8. Index-only 与 Server-path 差距归因

Search:

1. 100K 下 index-only hnswlib search 比 flat 快 9.43x，server-path VSEARCH 比 flat 快 7.24x，说明 100K search 的主要收益仍来自 index backend。
2. 10K 下 index-only hnswlib search 比 flat 快 2.83x，server-path 只快 1.21x，说明小数据集时 RESP、embedding、metadata、response 等固定开销明显稀释 index 差距。

Add/VSET:

1. index-only hnswlib add avg 189us/457us。
2. server VSET hnswlib avg 1150us/1170us，其中 server log 的 `vset_hnsw_add_avg_us` 为 1076us/1095us。
3. VSET 主要瓶颈在 hnswlib add path 和 backend wrapper 参数/容量管理，非 index 开销约 74-75us。

## 9. Recall 与 Latency Tradeoff

Index-only recall@10:

| Dataset | hnswlib_hnsw Recall@10 | Avg search us |
|---|---:|---:|
| 10K/64d | 0.994100 | 114 |
| 100K/64d | 0.899700 | 337 |

100K recall 从 99.41% 降到 89.97%，但 search 仍显著快于 flat。正式优化前必须先决定目标 recall 档位；如果要接近 0.99 recall，应该优先验证 hnswlib runtime ef 是否正确使用 `efSearch=128`，而不是当前 `topK` 最小值。

## 10. 主要瓶颈判断

1. Faiss baseline 缺失是当前最大公平性阻塞：无法完成 native/hnswlib vs Faiss 数值对比。
2. hnswlib server VSET 是当前已测路径的最大性能瓶颈：100K avg 1170us，约 19.5x 慢于 native_flat VSET，其中 index add avg 1095us。
3. hnswlib 100K search latency 表现好，但 recall@10 只有 0.899700，说明当前低延迟可能部分来自 efSearch 未按配置生效。
4. server-path profiling 对 VSEARCH index/non-index 拆分不完整，`index_avg_us` 不能可靠归因。
5. 100K 下 hnswlib server VSEARCH 比 flat 快 7.24x，说明大规模 search 已经主要受 index 选择驱动。

## 11. P0/P1/P2 优化路径

P0:

1. 补齐 Faiss 构建依赖或预构建 `libfaiss.a`，恢复 `faiss_flat/faiss_hnsw` baseline。
2. 修正 hnswlib efSearch 使用，确保 server 和 index-only 都使用 `efSearch=128`，再重跑 recall/latency。
3. 补 VSEARCH profiling：index search、metadata lookup、topK/export、response build、network send。
4. 优化 hnswlib VSET add path：初始容量、resize 策略、buffer pooling、减少 malloc/free。

P1:

1. SIMD distance kernel 与连续内存布局。
2. batch add/search。
3. scope-aware search 与 metadata lookup 优化。
4. topK heap/partial sort、distance cache。
5. response buffer 预分配。

P2:

1. HNSW graph 持久化，避免每次 load 后 rebuild。
2. index shard 与 shard executor。
3. 多 executor / shard executor 搜索。
4. SQ8/PQ 量化。
5. Faiss 长期作为 baseline/fallback backend。

## 12. 当前结论与下一步建议

当前结论：

1. vendored hnswlib 可用，正式 server-path 已通过真实 `VSET`/`VSEARCH`。
2. Faiss 不可用，正式 Faiss 对比不能完成。
3. hnswlib search 在 100K 已有明显 latency 优势，但 recall 需要先对齐。
4. hnswlib VSET add 是当前最大实测瓶颈。
5. 当前 profiling 不足以完整拆分 VSEARCH index/non-index 开销。

下一步建议先做 P0：恢复 Faiss baseline、修正 efSearch、补 profiling，再重跑同一 `10K/64d` 与 `100K/64d` 矩阵。
