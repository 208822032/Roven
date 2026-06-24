# KVLite Native vs Faiss Benchmark Plan Review

## 1. 审查目标

本次审查目标不是优化 native HNSW，也不是执行完整压测，而是判断当前仓库中的向量压测方案是否能公平比较 KVLite native 自研端到端向量方案和 Faiss backend 方案，并设计下一阶段更公平、更可复现、更适合瓶颈归因的 benchmark plan。

简短结论：当前方案不能直接用于 native vs Faiss 的正式对比。现有资产有价值，但混合了 server-path、direct vector API、standalone Faiss API、ONNX 专项实验和历史结果，尚未形成同一数据集、同一参数、同一路径、同一指标口径的 native/Faiss 对比。

## 2. 当前已有压测资产

### Benchmark 脚本

| 文件 | 类型 | 当前价值 | 审查结论 |
|---|---|---|---|
| `scripts/benchmark/bench_vector_rw.sh` | server-path wrapper | 启动 `kvstore`，调用 `tests/vector_benchmark` 走真实 RESP VSET/VGET/VSEARCH/VDEL | 可复用为 end-to-end 基础，但当前不能直接切到 Faiss server backend |
| `scripts/benchmark/bench_vector_ef_compare.sh` | server-path ef_search 对比 | 通过真实 VSEARCH 比较不同 ef_search 的 topK overlap | 可复用 recall/overlap 思路，但它比较的是 ef_search 一致性，不是 native vs Faiss |
| `scripts/bench_vcmd_profile.sh` | in-process profile wrapper | 调 `vcmd_profile_bench` 和 `faiss_baseline` | 只能作为 index/API/profile 实验入口，不能与 server-path 结果混比 |
| `scripts/benchmark/vector_rw_client.py` | RESP client | 简单 VSET/VGET/VSEARCH/VDEL client | 可复用为小规模验证工具，当前指标弱于 `tests/vector_benchmark` |

### 测试 / benchmark 二进制

| 文件 | 路径 | 当前价值 | 审查结论 |
|---|---|---|---|
| `tests/vector_benchmark.c` | 真实 TCP + RESP client | 统计 QPS、avg、p50、p95、p99、min、max、success/failed/errors | end-to-end server benchmark 可复用 |
| `tests/vector_search_bench.c` | 进程内 vector API | 支持 flat/HNSW search 和 recall@1/recall@k | 可作为 index-only 参考，但不覆盖 Faiss backend |
| `tests/vector_store_bench.c` | 进程内 store/write bench | 批量 mock embedding 写入 vector store | 可辅助分析写入路径，不适合作 native/Faiss 对比主线 |
| `tests/vcmd_profile_bench.c` | 进程内 profile bench | 分 ann_core、vset_vec、vset_text、vsearch_vec、vsearch_text | 可复用思路，但当前硬编码 `vector_index_type=hnsw`，不是完整 backend 矩阵 |
| `tests/faiss_baseline.cpp` | standalone Faiss API | 直接测 Faiss IndexFlatIP/IndexHNSWFlat，含 add/search p50/p95/p99、recall@k、RSS | 可作为 Faiss standalone baseline，不能直接和 KVLite server-path 结果比较 |
| `tests/vcmd_onnx_faiss_bench.cpp` | ONNX + KVLite/Faiss in-process | 可用同一 ONNX embedding 生成数据，比较 KVLite ANN 和 Faiss ANN，含 recall1/5/10 | 对 index-only/embedding专项很有价值，但不是真实 RESP server path |

### 结果文件

| 文件/目录 | 内容 | 审查结论 |
|---|---|---|
| `results/vector_rw_summary.md` | 2026-05-19 的 VSET/VGET/VSEARCH/VDEL server-path hnsw 结果 | 只有单 backend，不能作为 native vs Faiss 对比 |
| `results/vector_rw_raw.csv` | 上述 raw CSV | 可作为格式参考 |
| `results/stage8_*`, `results/stage9_*` | embedding/worker/batch 历史实验结果 | 适合参考 embedding path，不适合作 native/Faiss index 对比 |

### 相关文档

| 文件 | 价值 |
|---|---|
| `docs/vector/stage3_native_hnsw_legacy_cleanup.md` | 记录 native_hnsw 被降级、hnswlib_hnsw 成为默认推荐后端的历史判断 |
| `docs/vector/ARCHITECTURE_FIT_REVIEW.md` | 描述 VSET/VSEARCH 当前链路、index abstraction、native HNSW 与 Faiss 差距 |
| `docs/archive/superpowers/plans/2026-05-19-vector-benchmark-redesign.md` | 记录 `tests/vector_benchmark.c` / `bench_vector_rw.sh` 的设计来源 |
| `docs/KVLite项目复盘（九）：向量语义缓存与HNSW近似检索.md` | 总结 ef_search、topK overlap、recall@K 应使用 flat ground truth 的原则 |

## 3. 当前方案合理性判断

结论：`NOT_APPROVED`

原因：

1. 现有结果没有形成 native/Faiss 同数据、同参数、同路径对比。
2. server-path benchmark 可测 VSET/VSEARCH 端到端，但当前脚本的命令行 backend 切换不能可靠切到 `faiss_hnsw` / `faiss_flat`。
3. Faiss 结果主要来自 standalone / in-process API benchmark，和 KVLite server-path benchmark 不是同一层级。
4. 当前默认 `vector_index_type=hnsw` 在 `ENABLE_HNSWLIB=1` 时绑定 hnswlib backend；`native_hnsw` 在当前代码中已不是可通过配置正常选择的 backend。因此“native 自研 HNSW”需要先定义清楚：是 legacy native HNSW、当前默认 hnswlib_hnsw，还是 KVLite server path 加任意非-Faiss backend。
5. 当前缺少统一固定的数据文件、query 文件、seed、normalize 策略、HNSW 参数表、recall@k ground truth 和 server-path 内部耗时拆分。

## 4. 当前方案主要问题

### 路径公平性

`tests/vector_benchmark.c` 通过 TCP/RESP 发送真实 VSET/VSEARCH，能覆盖 parser、handler、executor、response 和网络路径。这对端到端用户路径是正确的。

但 `tests/faiss_baseline.cpp` 直接调用 Faiss C++ API，不经过 KVLite RESP、handler、executor、metadata lookup、response serialization 或网络。它只能和 KVLite index-only benchmark 比较，不能和 server-path VSEARCH latency/QPS 直接比较。

当前 KVLite server 代码已经有 `kvs_vector_index_t + kvs_vector_index_ops_t` 抽象，VSET/VSEARCH 最终通过 `kvs_vector_index_add_current()` 和 `kvs_vector_index_search_current()` 调到当前 backend。因此理论上可以实现“同一 VSET/VSEARCH path，只替换底层 index backend”。但现有 benchmark 脚本还没有把这件事稳定固化。

### 数据公平性

当前 server-path benchmark 使用 synthetic/semantic 文本，通过 embedding provider 生成向量；Faiss standalone benchmark 使用 `srand(42)` 随机 float 向量。二者数据分布、query set、normalize 时机和写入顺序都不同，不能直接比较。

`tests/vcmd_onnx_faiss_bench.cpp` 已经具备“同一 ONNX embedding 数据给 KVLite ANN 和 Faiss ANN”的雏形，可复用为 index-only 数据生成/对比参考。

### 参数公平性

当前参数不统一：

| 项 | 当前状态 |
|---|---|
| `Makefile` | 默认 `ENABLE_FAISS=0`，`ENABLE_HNSWLIB=1` |
| 默认 HNSW | `vector_index_type=hnsw`，编译 hnswlib 时实际绑定 hnswlib_hnsw |
| `native_hnsw` | 当前代码中报错并要求使用 hnswlib_hnsw，配置/CLI 也没有稳定入口 |
| Faiss standalone | 默认 `M=16 efConstruction=200 efSearch=128` |
| `config/master.conf` | `M=32 efConstruction=256 efSearch=128` |
| `config/kvstore.conf` | `M=32 efConstruction=256 efSearch=256` |

正式对比前必须统一 M、efConstruction、efSearch、metric、topK、dataset size、dim，并记录 backend 实际生效值。

### 指标完整性

`tests/vector_benchmark.c` 已覆盖端到端 QPS、avg、p50、p95、p99、min、max、success、failed、errors、elapsed_ms。

不足：

1. 没有 recall@k。
2. 没有内存/RSS。
3. 没有 CPU usage。
4. 没有 timeout 语义和 timeout 计数。
5. 没有 server 内部 index add/search 耗时、metadata lookup、response serialization、queue wait 的统一输出。

### recall 可比性

当前有三类 recall/overlap 资产：

1. `tests/faiss_baseline.cpp` 使用 Faiss FlatIP 做 ground truth，计算 recall@k。
2. `tests/vector_search_bench.c` 可比较 flat 与 HNSW recall@1/recall@k。
3. `scripts/benchmark/bench_vector_ef_compare.sh` 比较不同 ef_search 的 topK overlap，并明确说明真正 recall@K 需要 flat ground truth。

这些思路可复用，但当前 server-path native vs Faiss 对比还没有统一 ground truth 流程。

### profiling 完整性

当前已有两套观测基础：

1. `include/kvs_vcmd_profile.h` 定义了 VSET/VSEARCH total、parse、queue、embedding、index、candidate_lookup、response、network 等阶段。
2. `kvs_vector_vset_commit_stats_t` / `kvs_command_task_t` 已有部分 VSET commit、index add、VSEARCH search、AOF/replication/dirty mark timing 字段。

不足是正式 benchmark 脚本没有稳定采集这些字段，也没有确认所有阶段已经在真实 server path 接入。当前最稳妥的最小拆分目标应先做到：

```text
end-to-end latency
index add/search latency
non-index overhead = end-to-end - index latency
```

### 结果可复现性

当前可复现性不足：

1. server-path 文本生成是固定 prefix 规则，但没有固化成可共享 dataset/query 文件。
2. Faiss standalone 使用随机向量，seed 固定为 42，但与 server-path 不共享数据。
3. HNSW 参数散落在脚本、配置、代码默认值中。
4. 结果目录没有 native/Faiss 对比 run manifest。
5. 未记录 build flags，例如 `ENABLE_FAISS=1`、`ENABLE_HNSWLIB=1`、compiler flags、CPU SIMD path。

### 是否能指导优化

当前结果不足以回答“native 如何接近 Faiss”。如果只看到 Faiss standalone 比 KVLite server 快，无法判断差距来自 index 实现、embedding、metadata lookup、executor queue、RESP serialization 还是网络。

必须拆成 index-only 和 end-to-end 两层，且两层使用相同数据与参数。

## 5. 推荐压测设计

### 5.1 Index-only benchmark

目的：只比较底层 index 实现差距，回答“native/hnswlib/Faiss index 本身差多少”。

对比 backend：

| Backend | 说明 |
|---|---|
| `native_flat` | KVLite flat 精确 baseline |
| `native_hnsw` | 仅当 legacy native HNSW 恢复为可选择 backend 时纳入 |
| `hnswlib_hnsw` | 当前默认非-Faiss HNSW，应作为实际 KVLite HNSW backend |
| `faiss_flat` | Faiss exact baseline |
| `faiss_hnsw` | Faiss ANN baseline |

输入要求：

1. 固化 `dataset.bin` / `queries.bin` 或 CSV/TSV manifest。
2. 固定 `seed=42`。
3. 固定 `N`、`dim`、`nq`、`topK`。
4. IP metric 下统一 L2 normalize；L2 metric 下不 normalize 或明确记录。
5. 写入顺序完全一致。
6. scope 统一为空，先不引入 scope filter；scope 性能单独测。

指标：

1. add total / avg / p50 / p95 / p99 / max / QPS。
2. search total / avg / p50 / p95 / p99 / max / QPS。
3. recall@1 / recall@10 / recall@k。
4. RSS / index memory bytes。
5. timeout / error count。

实现原则：

1. 不走网络。
2. 不走 RESP。
3. 不做 response serialization。
4. 直接调用统一 index abstraction 或同等 adapter。
5. 不把 Faiss standalone API 与 KVLite server-path 结果混合比较。

输出建议：

```text
results/vector_native_faiss/index_only/<run_id>/manifest.json
results/vector_native_faiss/index_only/<run_id>/raw.csv
results/vector_native_faiss/index_only/<run_id>/summary.md
```

### 5.2 End-to-end server benchmark

目的：比较真实用户路径性能，回答“真实 VSET/VSEARCH path 切换 backend 后差多少”。

对比 backend：

| Backend | 前提 |
|---|---|
| `hnswlib_hnsw` 或当前非-Faiss HNSW | 当前可作为 KVLite native/server backend 主体 |
| `faiss_hnsw` | 必须 `ENABLE_FAISS=1`，且通过配置文件或修正 CLI 确认实际生效 |
| `native_flat` | 可用于 server-path exact baseline |
| `faiss_flat` | 可用于 server-path Faiss exact baseline |

输入要求：

1. 通过真实 VSET / VSEARCH 命令。
2. native 和 Faiss 使用同一 server path。
3. 只通过配置切换 backend。
4. dataset、query set、k、dim、metric、normalize、scope、写入顺序完全一致。
5. 每次运行保存 `VINFO`、配置文件、build flags、git commit、CPU 信息。

指标：

1. VSET end-to-end avg/p50/p95/p99/max/QPS。
2. VSEARCH end-to-end avg/p50/p95/p99/max/QPS。
3. timeout / error count。
4. RSS / memory bytes。
5. server-side VSET index add avg/max。
6. server-side VSEARCH index search avg/max。
7. non-index overhead。

重要修正：

`bench_vector_rw.sh` 当前通过 `--vector_index_type "$INDEX_TYPE"` 切 backend，但命令行 parser 只接受 `flat/hnsw/ivf_flat/sq8_flat`，不接受 `faiss_hnsw/faiss_flat`。下一阶段如果继续使用这个脚本，必须先改成“生成临时配置文件并通过 config 设置 backend”，或修正 CLI parser 后再运行。

输出建议：

```text
results/vector_native_faiss/server_path/<run_id>/manifest.json
results/vector_native_faiss/server_path/<run_id>/raw.csv
results/vector_native_faiss/server_path/<run_id>/summary.md
results/vector_native_faiss/server_path/<run_id>/server_*.log
```

## 6. 推荐测试矩阵

最小矩阵：

| Case | Dataset | Dim | Backend | Operation |
|---|---:|---:|---|---|
| C1 | 10K | 64 | hnswlib_hnsw / current native server HNSW | VSET / VSEARCH |
| C2 | 10K | 64 | faiss_hnsw | VSET / VSEARCH |
| C3 | 10K | 64 | native_flat | VSET / VSEARCH |
| C4 | 10K | 64 | faiss_flat | VSET / VSEARCH |
| C5 | 100K | 64 | hnswlib_hnsw / current native server HNSW | VSET / VSEARCH |
| C6 | 100K | 64 | faiss_hnsw | VSET / VSEARCH |

如果 legacy native_hnsw 重新暴露为可选择 backend，再增加：

| Case | Dataset | Dim | Backend | Operation |
|---|---:|---:|---|---|
| C1n | 10K | 64 | native_hnsw | VSET / VSEARCH |
| C5n | 100K | 64 | native_hnsw | VSET / VSEARCH，允许 timeout 并记录 estimated |

成本可控后再补：

| Case | Dataset | Dim | Backend | Operation |
|---|---:|---:|---|---|
| C7 | 10K | 512 | hnswlib_hnsw / current native server HNSW | VSET / VSEARCH |
| C8 | 10K | 512 | faiss_hnsw | VSET / VSEARCH |

统一参数建议：

| 参数 | 值 |
|---|---|
| `seed` | 42 |
| `metric` | ip |
| `normalize` | true |
| `topK` | 10 |
| `M` | 32 |
| `efConstruction` | 256 |
| `efSearch` | 128 |
| `scope` | empty for first pass |
| `timeout` | 10K: 5 min/case; 100K: 20 min/case |

## 7. 推荐 profiling 点

最小必需：

1. VSET total。
2. VSET index add。
3. VSEARCH total。
4. VSEARCH index search。
5. non-index overhead。
6. timeout / error count。

正式分析建议：

1. metadata lookup：id_map lookup、record lookup、scope filter。
2. response serialization：VSEARCH result to RESP array。
3. queue wait：command executor queue wait、embedding worker queue wait。
4. embedding prepare：仅在 text/ONNX 模式打开。
5. malloc/free count：如可行，用 allocator stats 或采样方式记录。
6. memory usage：RSS、VINFO memory bytes、backend ntotal/index memory。
7. CPU usage：perf stat 或 `/usr/bin/time -v`。

## 8. 后续优化分析框架

1. 如果 index-only 差距很大，主要问题在 native/hnswlib index 实现或 adapter 参数。
2. 如果 index-only 接近但 end-to-end 差距大，主要问题在 server path：RESP、executor、metadata lookup、response serialization 或网络。
3. 如果 VSEARCH 差距大于 VSET，重点看 search buffer、topK heap、distance cache、candidate export、scope filter、response serialization。
4. 如果 VSET 差距大于 VSEARCH，重点看 index add、record ownership、id_map、AOF/replication post-commit。
5. 如果 512d 差距扩大，重点看 SIMD、内存布局、向量连续性、cache miss。
6. 如果 100K 差距扩大，重点看 HNSW graph layout、neighbor expansion、visited bitmap、candidate set 管理。
7. 如果 recall 差异大，不能只比较 latency，必须先统一 HNSW 参数和 recall 水平；必要时按 recall target 调 efSearch 后再比较性能。
8. 如果 Faiss server-path 比 Faiss standalone 慢很多，问题在 KVLite wrapper/server path，而不是 Faiss index。

## 9. 下一阶段执行建议

推荐下一阶段 Codex 任务顺序：

1. 固化 benchmark 数据生成器：输出 dataset/query 文件和 manifest，固定 seed、dim、normalize、metric、scope。
2. 固化 index-only benchmark：复用或改造 `tests/vcmd_onnx_faiss_bench.cpp` / `tests/vector_search_bench.c`，统一输出 raw.csv 和 recall。
3. 固化 server-path benchmark：基于 `tests/vector_benchmark.c`，用临时配置文件切 backend，不依赖当前不完整的 CLI backend parser。
4. 补齐 profiling 点：至少输出 VSET index add、VSEARCH index search、non-index overhead。
5. 跑 10K/64d 最小矩阵。
6. 跑 100K/64d 扩展矩阵，native_hnsw 若不可用或超时必须记录为 timeout/estimated，而不是静默跳过。
7. 生成正式 benchmark report：区分 index-only 和 server-path，不混合结论。
8. 基于结果拆分 P0 / P1 / P2 优化任务。

建议下一阶段 prompt：

```text
请基于 docs/vector_native_faiss_optimization/00_benchmark_plan_review.md 执行下一阶段：
1. 不跑完整压测，先实现可复现 benchmark 数据生成器和 manifest。
2. 固化 index-only native/Faiss benchmark 输出格式，确保同一 dataset/query/seed/metric/topK/HNSW 参数。
3. 修正 server-path benchmark 的 backend 切换方式，优先生成临时配置文件，不依赖当前 CLI parser。
4. 只跑 1K/64d smoke 验证输出，不做正式 10K/100K 压测。
5. 输出 smoke report 和后续正式压测命令。
```
