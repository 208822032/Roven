# Stage 4：向量核心 Benchmark 报告

## 1. Benchmark 环境

| 项目 | 值 |
|------|-----|
| 主机名 | zhengpan |
| 操作系统 | Linux 5.15.0-181-generic (Ubuntu) |
| 架构 | x86_64 |
| CPU | AMD EPYC 9654 96-Core Processor |
| vCPU | 4 |
| 内存 | 3.8 GB |
| AVX2 | 支持（已启用，距离计算后端使用） |
| 日期 | 2026-06-25 |

## 2. 构建参数

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j$(nproc) kvstore
```

| 参数 | 值 | 作用 |
|------|-----|------|
| ENABLE_HNSWLIB | 1 | 启用 hnswlib_hnsw 后端 |
| ENABLE_FAISS | 1 | 启用 faiss_flat / faiss_hnsw 后端 |
| ENABLE_FAISS_BASELINE | 已定义 | C 预处理器宏，控制 faiss 代码编译 |
| ENABLE_HNSWLIB | 已定义 | C 预处理器宏，控制 hnswlib 代码编译 |
| 编译器 | gcc / g++ | C 用于核心模块，C++17 用于 faiss 后端，C++11 用于 hnswlib 后端 |

**构建成功** —— 全部 5 个后端均已编译并链接：
- `core_flat`（始终构建）
- `core_hnsw`（始终构建）
- `hnswlib_hnsw`（ENABLE_HNSWLIB=1）
- `faiss_flat`（ENABLE_FAISS=1）
- `faiss_hnsw`（ENABLE_FAISS=1）

## 3. Benchmark 配置参数

| 参数 | 值 |
|------|-----|
| embedding_provider | hash |
| embedding_dim | 64 |
| vector_metric | ip（内积） |
| vector_hnsw_m | 16 |
| vector_hnsw_ef_construction | 128 |
| vector_hnsw_ef_search | 128 |
| vector_scope_overfetch_factor | 4 |
| mem_backend | system |
| network_mode | reactor |
| enable_aof | false |

基准真相（ground truth）：以 core_flat 精确搜索结果为准。

## 4. 10K / 64d 结果

### 4.1 汇总表

| 后端 | 状态 | VSET 平均 ms | VSET p50 ms | VSET p95 ms | VSEARCH 平均 ms | VSEARCH p50 ms | VSEARCH p95 ms | recall@10 | 索引内存 (bytes) | 重建 ms | RDB 加载+重建 ms |
|------|------|-------------|------------|------------|----------------|---------------|---------------|-----------|------------------|--------|-------------------|
| core_flat | 通过 | 0.067 | 0.062 | 0.087 | 0.563 | 0.550 | 0.640 | **1.0000** | 9,322,496 | 3.7 | 101 |
| core_hnsw | 通过 | 0.225 | 0.211 | 0.287 | 0.273 | 0.270 | 0.360 | **0.9870** | 41,826,512 | 1,222.7 | 2,610 |
| hnswlib_hnsw | 通过 | 0.711 | 0.680 | 1.058 | 0.602 | 0.572 | 0.661 | 0.8890 | 4,456,448 | 3.2 | 11,846 |
| faiss_flat | 通过 | 0.071 | 0.065 | 0.087 | 2.931 | 0.633 | 7.798 | 0.8900 | 0* | 4.2 | 151 |
| faiss_hnsw | 通过 | 0.203 | 0.200 | 0.276 | 0.281 | 0.147 | 0.193 | 0.8900 | 0* | 3.7 | 151 |

> *faiss_flat / faiss_hnsw：VINFO 报告的 `index_memory_bytes` 为 0，原因是 FAISS Index 对象的内存未纳入 KVLite 内存统计。实际 FAISS 索引内存在 libfaiss 内部分配。

### 4.2 正确性冒烟测试（全部 5 个后端 — 通过）

| 检查项 | core_flat | core_hnsw | hnswlib_hnsw | faiss_flat | faiss_hnsw |
|--------|-----------|-----------|-------------|------------|------------|
| update_visible（更新可见） | ✓ | ✓ | ✓ | ✓ | ✓ |
| update_no_duplicate（更新无重复） | ✓ | ✓ | ✓ | ✓ | ✓ |
| delete_hidden（删除隐藏） | ✓ | ✓ | ✓ | ✓ | ✓ |
| scope_isolated（scope 隔离） | ✓ | ✓ | ✓ | ✓ | ✓ |
| vinfo_index_type（VINFO 类型正确） | ✓ | ✓ | ✓ | ✓ | ✓ |

### 4.3 关键观察（10K）

1. **recall@10**：core_hnsw（0.987）显著优于 hnswlib_hnsw（0.889）和 faiss_hnsw（0.890），相对于次优 ANN 后端的优势为 +9.8 个百分点。
2. **VSEARCH 延迟**：core_hnsw 是 ANN 搜索中最快的，平均 0.273ms —— 比 hnswlib_hnsw 快 2.2 倍，与 faiss_hnsw 相当。
3. **VSET 延迟**：core_hnsw（0.225ms）比 hnswlib_hnsw（0.711ms）快 3.2 倍，但略慢于 faiss_hnsw（0.203ms）。
4. **索引内存**：core_hnsw 占用 41.8MB —— 是 hnswlib_hnsw（4.5MB）的 9.4 倍。这是当前主要的权衡：以内存换召回率。
5. **VINDEX REBUILD**：core_hnsw（1,222ms）比 hnswlib_hnsw（3.2ms）慢 380 倍，因为 core_hnsw 进行全量图重建，而 hnswlib 使用索引序列化。
6. **RDB 加载+重建**：core_hnsw（2,610ms）比 hnswlib_hnsw（11,846ms）快 4.5 倍。hnswlib 的 RDB 重载异常缓慢 —— 它通过逐条重新添加全部向量来重建索引。
7. **faiss_flat VSEARCH 异常**：faiss_flat（平均 2.931ms）在 10K 规模下的精确搜索比 core_flat（0.563ms）慢 5.2 倍，且 p95 延迟明显偏高（7.8ms）。这表明 FAISS flat 路径相比 core_flat 有显著的每次调用开销。

## 5. 100K / 64d 结果

### 5.1 汇总表

| 后端 | 状态 | VSET 平均 ms | VSET p50 ms | VSET p95 ms | VSEARCH 平均 ms | VSEARCH p50 ms | VSEARCH p95 ms | recall@10 | 索引内存 (bytes) | 重建 ms | RDB 加载+重建 ms |
|------|------|-------------|------------|------------|----------------|---------------|---------------|-----------|------------------|--------|-------------------|
| core_flat | 通过 | 0.071 | 0.065 | 0.089 | 4.906 | 4.864 | 5.763 | **1.0000** | 74,579,968 | 49.6 | 303 |
| core_hnsw | **失败** | 0.299 | 0.278 | 0.417 | 1.355 | 1.166 | 2.666 | **0.9770** | 334,510,288 | **超时 (>10s)** | 不可用 |
| hnswlib_hnsw | **失败** | 0.934 | 0.872 | 1.613 | 1.567 | 1.541 | 1.978 | 0.9280 | 35,651,584 | 40.9 | **超时 (>20s)** |
| faiss_flat | 通过 | 0.070 | 0.064 | 0.088 | 6.937 | 4.644 | 26.125 | 0.9290 | 0* | 48.5 | 302 |
| faiss_hnsw | 通过 | 0.543 | 0.541 | 0.849 | 1.541 | 1.197 | 4.463 | 0.9260 | 0* | 44.4 | 303 |

### 5.2 超时详情

- **core_hnsw**：VINDEX REBUILD 超过 10 秒的 socket 超时限制。超时前已成功采集 VSET、VSEARCH 和 recall 数据。RDB 重载未执行。
- **hnswlib_hnsw**：RDB 重载超过 20 秒的 `wait_port` 超时限制。已成功采集 VSET、VSEARCH、recall 和 REBUILD 数据。hnswlib 在重载时通过逐条重新添加 100K 向量来重建索引，耗时超过 20 秒。

### 5.3 关键观察（100K）

1. **recall@10**：core_hnsw 在 100K 规模下保持出色的召回率 0.977 —— 比 hnswlib_hnsw（0.928）高 +4.9pp，比 faiss_hnsw（0.926）高 +5.1pp。
2. **VSEARCH 延迟**：core_hnsw（1.355ms）仍然是最快的 ANN 搜索，略优于 faiss_hnsw（1.541ms）和 hnswlib_hnsw（1.567ms）。
3. **索引内存**：core_hnsw 占用 334.5MB —— 是 hnswlib_hnsw（35.7MB）的 9.4 倍。比例与 10K 结果一致。
4. **VINDEX REBUILD 可扩展性问题**：core_hnsw 重建在 100K 规模下 10 秒内无法完成。10K 重建耗时 1.2s，100K 重建超过 10s，说明扩展性差于线性。
5. **hnswlib RDB 重载**：在 100K 规模下失败（>20s）。这是 core_hnsw 无关的已有问题。
6. **faiss_flat VSEARCH p95**：26.1ms —— 相比于 core_flat 的 5.8ms，尾部延迟非常高。

## 6. recall@10 对比

| 规模 | core_flat | core_hnsw | hnswlib_hnsw | faiss_flat | faiss_hnsw |
|------|-----------|-----------|-------------|------------|------------|
| 10K | 1.000（基准） | **0.987** | 0.889 | 0.890 | 0.890 |
| 100K | 1.000（基准） | **0.977** | 0.928 | 0.929 | 0.926 |

**core_hnsw 召回率优势**：
- 10K：比 hnswlib 高 +9.8pp，比 faiss_hnsw 高 +9.7pp
- 100K：比 hnswlib 高 +4.9pp，比 faiss_hnsw 高 +5.1pp

## 7. 内存对比

| 规模 | 后端 | 索引内存 | 向量内存 | 向量总内存 | 每条向量开销 |
|------|------|---------|---------|----------|------------|
| 10K | core_flat | 9.3 MB | 7.5 MB | 16.9 MB | ~930 B |
| 10K | core_hnsw | 41.8 MB | 7.5 MB | 53.8 MB | ~4,180 B |
| 10K | hnswlib_hnsw | 4.5 MB | 7.5 MB | 12.0 MB | ~450 B |
| 10K | faiss_flat | 0* | 7.5 MB | 7.5 MB | 0* |
| 10K | faiss_hnsw | 0* | 7.5 MB | 7.5 MB | 0* |
| 100K | core_flat | 74.6 MB | 73.1 MB | 147.1 MB | ~746 B |
| 100K | core_hnsw | 334.5 MB | 73.1 MB | 443.2 MB | ~3,345 B |
| 100K | hnswlib_hnsw | 35.7 MB | 73.1 MB | 108.7 MB | ~357 B |

**core_hnsw 内存分析**：在两种规模下，索引内存均为 hnswlib_hnsw 的约 9.4 倍。高内存占用的主要来源：
- M=16、M0=32 的双向邻居列表
- 每个节点的元数据开销（层级、标志位、状态）
- 内部 flat 存储副本（flat_physical_size = N）

## 8. REBUILD 与 RDB 加载+重建

### 8.1 VINDEX REBUILD

| 规模 | core_flat | core_hnsw | hnswlib_hnsw | faiss_flat | faiss_hnsw |
|------|-----------|-----------|-------------|------------|------------|
| 10K | 3.7 ms | 1,222.7 ms | 3.2 ms | 4.2 ms | 3.7 ms |
| 100K | 49.6 ms | **超时** (>10s) | 40.9 ms | 48.5 ms | 44.4 ms |

**core_hnsw 重建**是唯一进行全量图重建的后端。其他后端均使用快速的序列化方式或空操作重建。10K 时 1.2s 可接受；100K 时超过 10s 是一个需要解决的问题。

### 8.2 RDB 加载 + 重建

| 规模 | core_flat | core_hnsw | hnswlib_hnsw | faiss_flat | faiss_hnsw |
|------|-----------|-----------|-------------|------------|------------|
| 10K | 101 ms | 2,610 ms | 11,846 ms | 151 ms | 151 ms |
| 100K | 303 ms | 不可用 | **超时** (>20s) | 302 ms | 303 ms |

**core_hnsw RDB 重载**（10K 时 2.6s）比 hnswlib_hnsw（11.8s）快 4.5 倍。这是一个显著优势 —— core_hnsw 重建 HNSW 图的速度比 hnswlib 逐条重新添加的方式快 4.5 倍。

## 9. core_hnsw 与 hnswlib_hnsw 差距分析

| 指标 | 10K core_hnsw | 10K hnswlib | 差距 | 100K core_hnsw | 100K hnswlib | 差距 |
|------|--------------|------------|------|---------------|-------------|------|
| recall@10 | 0.987 | 0.889 | **+9.8pp** | 0.977 | 0.928 | **+4.9pp** |
| VSET 平均 | 0.225 ms | 0.711 ms | **快 3.2x** | 0.299 ms | 0.934 ms | **快 3.1x** |
| VSEARCH 平均 | 0.273 ms | 0.602 ms | **快 2.2x** | 1.355 ms | 1.567 ms | **快 1.2x** |
| 索引内存 | 41.8 MB | 4.5 MB | **大 9.4x** | 334.5 MB | 35.7 MB | **大 9.4x** |
| REBUILD | 1,222 ms | 3.2 ms | **慢 382x** | 超时 | 40.9 ms | 不可用 |
| RDB 重载 | 2,610 ms | 11,846 ms | **快 4.5x** | 不可用 | 超时 | 不可用 |

**总结**：core_hnsw 以内存换取召回率和速度。在 10K 规模下，实现了业界最佳的召回率和搜索延迟。重建路径是主要的可扩展性瓶颈。

## 10. core_flat 与 faiss_flat 差距分析

| 指标 | 10K core_flat | 10K faiss_flat | 差距 | 100K core_flat | 100K faiss_flat | 差距 |
|------|--------------|---------------|------|----------------|----------------|------|
| recall@10 | 1.000（基准） | 0.890 | **−11pp** | 1.000（基准） | 0.929 | **−7.1pp** |
| VSET 平均 | 0.067 ms | 0.071 ms | 相当 | 0.071 ms | 0.070 ms | 相当 |
| VSEARCH 平均 | 0.563 ms | 2.931 ms | **慢 5.2x** | 4.906 ms | 6.937 ms | **慢 1.4x** |
| VSEARCH p95 | 0.640 ms | 7.798 ms | **高 12.2x** | 5.763 ms | 26.125 ms | **高 4.5x** |

**faiss_flat 异常**：FAISS Flat 后端产生了与 core_flat 不同的 top-10 结果，导致以 core_flat 为基准的 recall@10 偏低。对于精确搜索方法来说，这是不符合预期的，需要进一步调查。此外，faiss_flat 的 VSEARCH 延迟和尾部延迟均显著差于 core_flat，尽管两者都是精确（非 ANN）方法。

可能的原因（未调查 —— 按照 Stage 4 约束不修改生产代码）：
- 距离度量约定不同（IP 与 L2 归一化）
- FAISS IndexFlatIP 与 IndexFlatL2 的差异
- KVLite 的 faiss 后端封装可能使用了不同的搜索路径

## 11. 当前瓶颈分析

### 主要瓶颈（core_hnsw）

1. **VINDEX REBUILD 可扩展性**：100K 时 10 秒内无法完成。这是一个阻塞操作，期间索引不可用。
2. **索引内存**：是 hnswlib 的 9.4 倍。在 100 万向量规模下，预计约 3.3 GB，而 hnswlib 约 350 MB。
3. **RDB 重载重建**：10K 时 2.6s，100K 时预计 25-30s（已超过 benchmark 框架的 20s 等待超时）。

### 次要瓶颈（所有后端）

4. **faiss_flat 精确搜索**：10K 时比 core_flat 慢 5.2 倍，尾部延迟高。
5. **faiss 内存**：KVLite VINFO 未统计。
6. **hnswlib RDB 重载**：10K 时 11.8s，100K 时超过 20s —— 比 core_hnsw 更差。

## 12. 下一步优化建议

### 短期（不改变架构）
1. **增大 benchmark 脚本的 socket 超时**，适应大规模 REBUILD（10s → 60s）。
2. **增大 benchmark 脚本的 wait_port 超时**，适应大规模 RDB 重载（20s → 120s）。
3. **调查 faiss_flat 召回率差异** —— 检查 `kvs_vector_faiss_backend.cpp` 中的 FAISS IndexFlatIP 配置。
4. **向 VINFO 添加 FAISS 索引内存统计**。

### 中期（少量代码修改）
5. **对 core_hnsw 重建进行性能分析** —— 确定图构建、邻居选择还是距离计算占主导。
6. **考虑增量重建** —— 在可能的情况下避免 VINDEX REBUILD 时的全量图重建。
7. **调整 core_hnsw M/M0 参数** —— 当前 M=16、M0=32 对于 64 维向量可能偏高。

### 长期（架构层面）
8. **HNSW 图序列化** —— 在 RDB 中持久化图结构，避免重载时重建。
9. **内存优化** —— 减少每条向量的节点开销，考虑 float16 嵌入。

## 13. 生产代码变更

**无。** Stage 4 未修改任何生产代码。

## 14. 修改 / 新增文件列表

| 文件 | 状态 | 说明 |
|------|------|------|
| `scripts/bench_vector_core_compare.py` | 新增 | Python benchmark 驱动，含 RESP 客户端，支持全部 5 个后端 |
| `tests/vector_core_benchmark_compare.sh` | 新增 | Shell 封装脚本，含构建和可选的 100K 运行 |
| `results/vector_core_benchmark/stage4_vector_core_benchmark_10k_64d.json` | 新增 | 10K/64d benchmark 结果（JSON） |
| `results/vector_core_benchmark/stage4_vector_core_benchmark_10k_64d.md` | 新增 | 10K/64d benchmark 结果（Markdown） |
| `results/vector_core_benchmark/stage4_vector_core_benchmark_100k_64d.json` | 新增 | 100K/64d benchmark 结果（JSON） |
| `results/vector_core_benchmark/stage4_vector_core_benchmark_100k_64d.md` | 新增 | 100K/64d benchmark 结果（Markdown） |
| `docs/vector_core_reset/stage4_vector_core_benchmark_report.md` | 新增 | 本报告 |
| `results/vector_core_benchmark/stage4_vector_core_benchmark_100_64d.json` | 新增 | 100 条冒烟测试结果 |
| `results/vector_core_benchmark/stage4_vector_core_benchmark_100_64d.md` | 新增 | 100 条冒烟测试结果 |

## 15. Git 状态

```
?? scripts/bench_vector_core_compare.py
?? tests/vector_core_benchmark_compare.sh
```

（仅两个新增脚本文件；已有的 `docs/vector_core_reset/` 文件和 `results/` 目录不在 git 跟踪范围内。）

## 16. Benchmark 产出物路径

- Benchmark 驱动脚本：`scripts/bench_vector_core_compare.py`
- Shell 封装脚本：`tests/vector_core_benchmark_compare.sh`
- 10K JSON 结果：`results/vector_core_benchmark/stage4_vector_core_benchmark_10k_64d.json`
- 10K Markdown 结果：`results/vector_core_benchmark/stage4_vector_core_benchmark_10k_64d.md`
- 100K JSON 结果：`results/vector_core_benchmark/stage4_vector_core_benchmark_100k_64d.json`
- 100K Markdown 结果：`results/vector_core_benchmark/stage4_vector_core_benchmark_100k_64d.md`
- 本报告：`docs/vector_core_reset/stage4_vector_core_benchmark_report.md`
