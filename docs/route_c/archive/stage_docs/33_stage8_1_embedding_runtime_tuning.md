# Stage 8.1: embedding runtime 调优与稳定化

## 1. 背景

Stage 8 已完成 per-thread embedding runtime / per-worker `OrtSession` 最小闭环。worker pool 可以并发 prepare，worker 内通过独立 `EmbeddingHandle` 执行 encode，prepare 完成后 task 回到 single command executor 串行 commit/search/finalize。

Stage 8 审计结论是 CONDITIONALLY_APPROVED：架构和生命周期正确，`encode_max_inflight` 已达到 2/4，说明 global mutex 已解除；但性能变差，主要原因不是 correctness bug，而是 `embedding_worker_threads * embedding_num_threads` 资源叠加，以及每个 worker 独立 `OrtEnv` 带来的 ONNX Runtime 内部资源竞争。

Stage 8.1 的目标是调优已经释放的 encode 并发能力，而不是扩大 executor 架构：

```text
worker pool 并发 prepare
  -> per-worker embedding runtime 并发 encode
  -> 控制 ORT 内部线程资源
  -> 共享 OrtEnv
  -> 保持 per-worker OrtSession
  -> prepared_queue 回到 single executor
  -> commit/search/finalize 串行
```

核心原则仍然是：

```text
embedding encode 并发，状态 commit 串行。
```

## 2. 不做多 executor / commit ordering

本阶段没有做多 command executor、commit ordering、pipeline 并行或 response reorder。

原因：

- Stage 8.1 要解决的是 embedding runtime 资源竞争，不是提交顺序模型。
- 当前 commit/search/finalize 仍由 single command executor 串行执行，AOF/replication/vector store/HNSW 可见性语义保持不变。
- 直接进入多 executor 会引入跨连接 commit ordering、AOF ordering、replication ordering、HNSW/id_map 并发可见性和回滚问题。
- Stage 8.1 的性能结果显示资源调优已经显著改善吞吐，尚不需要为了 Stage 8 的退化直接扩大架构。

## 3. 性能退化归因

Stage 8 baseline 使用：

```text
embedding_worker_threads = 4
embedding_num_threads = 4
resource_product = 16
per-worker OrtEnv + per-worker OrtSession
```

在本机 `detected_cpu_cores=4` 的环境中，4 个 worker 各自配置 4 个 ONNX intra-op threads，形成明显过量的 ORT 内部执行线程竞争。同时每个 worker 独立 `OrtEnv`，进一步放大启动和 runtime 内部资源开销。

Stage 8.1 将资源乘积作为一等 stats 和日志字段：

```text
resource_product = embedding_worker_threads * embedding_num_threads
```

建议：

- `worker_threads * embedding_num_threads <= CPU physical cores`。
- bge-small-zh-v1.5 小模型优先用 `embedding_worker_threads=2, embedding_num_threads=1` 起步。
- 追求吞吐时优先逐步增大 `embedding_worker_threads`，不要同时增大 worker threads 和 ONNX intra-op threads。

启动时会打印：

```text
embedding runtime resources: worker_threads=... embedding_num_threads=... resource_product=... detected_cpu_cores=...
```

当 `resource_product > detected_cpu_cores` 或 `resource_product > 8` 时打印 `KVS_LOG_WARN`。

## 4. shared OrtEnv + per-worker OrtSession

Stage 8.1 在 ONNX provider 中实现了共享 `OrtEnv`，同时保留每个 `EmbeddingHandle` 独立的 mutable/runtime state：

```text
global/shared:
  OrtEnv

per EmbeddingHandle / per worker:
  OrtSessionOptions
  OrtSession
  OrtMemoryInfo
  vocab/tokenizer state
  last_error
  profiling fields
```

实现要点：

- `src/embedding/embedding_runtime_onnx.c` 新增 `g_onnx_env_mu`、`g_shared_ort_env`、`g_shared_ort_env_refcount`。
- `embedding_create()` 获取 shared env ref，然后创建 per-handle `OrtSessionOptions` / `OrtSession` / `OrtMemoryInfo`。
- `embedding_destroy()` 先释放 per-handle session/options/memory_info，再释放 shared env ref。
- 最后一个 handle 释放 ref 时才调用 `ReleaseEnv`。
- create failure 统一通过 `embedding_release_ort_resources()` 回滚，避免 double free 和 UAF。
- 不共享 `OrtSession`，不共享 `last_error`，不共享 mutable tokenizer buffer。

为便于 Stage 8 baseline 对比，新增配置：

```conf
embedding_shared_ort_env_enabled = 1
```

默认开启。性能脚本中 Stage 8 baseline 显式设置为 `0`，用于复现 “独立 OrtEnv per worker” 行为；Stage 8.1 tuned A/B 设置为 `1`。

## 5. 配置

`config/kvstore-embedding.conf` 已改为 Stage 8.1 推荐示例：

```conf
embedding_worker_pool_enabled = 1
embedding_worker_threads = 2

embedding_per_thread_runtime_enabled = 1
embedding_per_thread_runtime_fallback_mutex = 1
embedding_shared_ort_env_enabled = 1

embedding_num_threads = 1
```

全局默认仍保守：worker pool 和 per-thread runtime 默认关闭，fallback mutex 默认开启，shared OrtEnv 默认开启。

## 6. Stats

保留 Stage 8 stats：

- `per_thread_context_enabled`
- `per_thread_context_created`
- `per_thread_context_destroyed`
- `per_thread_context_create_failed`
- `per_thread_context_fallback_count`
- `encode_global_mutex_count`
- `encode_thread_context_count`
- `encode_current_inflight`
- `encode_max_inflight`
- `encode_total_us`
- `encode_max_us`

新增 Stage 8.1 stats：

- `shared_ort_env_enabled`
- `shared_ort_env_created`
- `shared_ort_env_destroyed`
- `shared_ort_env_refcount_current`
- `shared_ort_env_refcount_max`
- `ort_session_created`
- `ort_session_destroyed`
- `embedding_runtime_resource_product`
- `embedding_worker_threads_effective`
- `embedding_num_threads_effective`

worker pool shutdown 日志用于观察 worker context 和 encode 并发。vector provider destroy 后新增 final runtime stats，用于确认 global provider handle 也释放后：

- `shared_ort_env_created == shared_ort_env_destroyed`
- `ort_session_created == ort_session_destroyed`
- `shared_ort_env_refcount_current == 0`

## 7. Correctness

已运行：

```bash
git diff --check
make clean && make embedding ENABLE_EBPF=0
tests/embedding_runtime_tuning_stage8_1.sh
```

覆盖：

- reactor / proactor。
- worker disabled。
- worker enabled + per-thread runtime disabled，作为 global mutex baseline。
- worker enabled + Stage 8.1 tuned config。
- semantic ONNX。
- VSET / VGET / VSEARCH / VDEL。
- repeat VSET。
- pipeline 10 / 50。
- high accept 200。
- fd reuse。
- 多连接并发 VSET/VSEARCH。

关键结果：

| mode | config | contexts | sessions | shared env | resource_product | global_mutex | thread_context | encode_max_inflight |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| reactor | Stage7 mutex tuned | 0/0 | 1/1 | 1/1 | 2 | 486 | 0 | 1 |
| reactor | Stage8.1 tuned 2x1 | 2/2 | 3/3 | 1/1 | 2 | 0 | 492 | 2 |
| proactor | Stage7 mutex tuned | 0/0 | 1/1 | 1/1 | 2 | 532 | 0 | 1 |
| proactor | Stage8.1 tuned 2x1 | 2/2 | 3/3 | 1/1 | 2 | 0 | 532 | 2 |

结论：shared `OrtEnv` exactly-once 创建/销毁，per-worker `OrtSession` 创建/销毁匹配，per-thread runtime 没有走 global mutex fallback，encode 并发仍然成立。

## 8. Performance

已运行：

```bash
RESULT_DIR=results/stage8_1_embedding_runtime_tuning \
tests/embedding_runtime_tuning_perf_stage8_1.sh
```

三组对比：

| label | worker_threads | embedding_num_threads | shared_ort_env | resource_product |
|---|---:|---:|---:|---:|
| Stage 8 baseline | 4 | 4 | 0 | 16 |
| Stage 8.1 tuned A | 2 | 1 | 1 | 2 |
| Stage 8.1 tuned B | 4 | 1 | 1 | 4 |

### Latency

| mode | config | VSET avg/p50/p95/p99 ms | VSET qps | VSEARCH avg/p50/p95/p99 ms | VSEARCH qps |
|---|---|---:|---:|---:|---:|
| reactor | Stage8 baseline 4x4 | 15.33 / 11.24 / 31.63 / 45.04 | 65.21 | 6.26 / 3.88 / 24.09 / 40.17 | 159.64 |
| reactor | tuned A 2x1 | 9.57 / 9.29 / 10.04 / 15.20 | 104.49 | 3.78 / 3.75 / 4.12 / 7.00 | 264.60 |
| reactor | tuned B 4x1 | 9.95 / 9.31 / 14.55 / 17.56 | 100.47 | 3.70 / 3.77 / 4.05 / 4.09 | 270.13 |
| proactor | Stage8 baseline 4x4 | 10.19 / 7.86 / 27.86 / 35.97 | 98.15 | 2.34 / 1.49 / 9.67 / 19.96 | 427.33 |
| proactor | tuned A 2x1 | 9.70 / 9.31 / 13.43 / 16.15 | 103.02 | 3.73 / 3.72 / 4.08 / 6.52 | 268.10 |
| proactor | tuned B 4x1 | 11.03 / 9.55 / 17.39 / 18.10 | 90.64 | 4.02 / 3.80 / 6.42 / 6.99 | 248.59 |

### Throughput c=16

| mode | config | VSET qps | VSEARCH qps | KV SET/GET c=4 qps |
|---|---|---:|---:|---:|
| reactor | Stage8 baseline 4x4 | 106.31 | 233.65 | 7260.30 |
| reactor | tuned A 2x1 | 223.64 | 551.28 | 8615.93 |
| reactor | tuned B 4x1 | 403.84 | 699.41 | 9416.09 |
| proactor | Stage8 baseline 4x4 | 102.44 | 235.12 | 9998.37 |
| proactor | tuned A 2x1 | 221.37 | 549.23 | 8390.49 |
| proactor | tuned B 4x1 | 404.56 | 720.88 | 14806.31 |

### Runtime stats from perf

| mode | config | contexts | sessions final | shared env final | resource_product | encode_max_inflight |
|---|---|---:|---:|---:|---:|---:|
| reactor | Stage8 baseline 4x4 | 4/4 | 5/5 | disabled | 16 | 4 |
| reactor | tuned A 2x1 | 2/2 | 3/3 | 1/1 | 2 | 2 |
| reactor | tuned B 4x1 | 4/4 | 5/5 | 1/1 | 4 | 4 |
| proactor | Stage8 baseline 4x4 | 4/4 | 5/5 | disabled | 16 | 4 |
| proactor | tuned A 2x1 | 2/2 | 3/3 | 1/1 | 2 | 2 |
| proactor | tuned B 4x1 | 4/4 | 5/5 | 1/1 | 4 | 4 |

性能结论：

- 资源乘积从 16 降到 2/4 后，VSET/VSEARCH c16 吞吐显著改善。
- tuned A 2x1 延迟更稳定，资源占用最低，是推荐示例配置。
- tuned B 4x1 在本轮吞吐最好，适合作为机器资源允许时的吞吐配置。
- proactor 单连接 latency 下 baseline VSEARCH qps 较高，但 p95/p99 明显差；tuned 配置尾延迟更稳。
- KV SET/GET smoke 无协议或网络闭环退化信号。

## 9. Sanitizer

### ASAN/UBSAN/LSAN

已运行：

```bash
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=address,undefined" \
make embedding ENABLE_EBPF=0

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
tests/embedding_runtime_tuning_stage8_1.sh
```

结果：

- correctness 功能路径通过。
- 每个服务进程 shutdown 后被 LSAN abort，脚本仍完成全部 case。
- Stage 8.1 final stats 正常：shared env 1/1，sessions 3/3，contexts 2/2。
- LSAN 栈落在既有路径：RESP parser / reactor / proactor argv、vector/HNSW、prepared field、rbtree create 等。
- 未发现指向 shared `OrtEnv`、per-worker `OrtSession`、TLS context、worker pool queue 或 refcount 的新增泄漏栈。

### TSAN

已运行：

```bash
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=thread -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=thread" \
make embedding ENABLE_EBPF=0
```

`TSAN_OPTIONS="halt_on_error=1"` 运行脚本时被既有 shutdown race 截断：

- `g_shutdown_flag`：`handle_quit` 写，与 distributed master sync thread 读竞争。

为了继续覆盖 Stage 8.1 路径，又运行：

```bash
TSAN_OPTIONS="halt_on_error=0 exitcode=0" \
tests/embedding_runtime_tuning_stage8_1.sh
```

结果：

- correctness 脚本完整通过。
- Stage 8.1 reactor/proactor tuned stats 正常：contexts 2/2，sessions 3/3，shared env 1/1，global_mutex=0，encode_max_inflight=2。
- TSAN reports 均落在既有 shutdown / background 路径：
  - `src/core/kvs_operation.c:1402 handle_quit` vs distributed master sync thread read `g_shutdown_flag`。
  - `src/core/kvs_ttl_background.c:254 kvs_ttl_background_stop`。
  - `src/distributed/distributed.c:311 distributed_shutdown`。
  - `src/distributed/sync_backend_tcp.c:254 sync_backend_shutdown`。
- 未发现 worker pool queue、TLS context、shared OrtEnv refcount、runtime stats 或 ONNX provider wrapper 相关 race。

## 10. 已知限制

- `embedding_shared_ort_env_enabled` 默认开启；baseline 对比脚本会显式关闭以复现 Stage 8 行为。
- shared `OrtEnv` 只减少 env 级别资源重复，不改变 per-worker `OrtSession` 内部资源占用。
- tuned B 4x1 吞吐更好，但资源占用和并发 pressure 高于 tuned A 2x1。
- 当前 TSAN 仍会报告既有 shutdown/background race；本阶段只归类，不修 TTL/distributed/shutdown 专项。
- 不修 MOD 不清 TTL，不做 vector/HNSW cleanup，不接 ntyco executor。

## 11. 推荐配置

默认示例推荐：

```conf
embedding_worker_pool_enabled = 1
embedding_worker_threads = 2
embedding_per_thread_runtime_enabled = 1
embedding_per_thread_runtime_fallback_mutex = 1
embedding_shared_ort_env_enabled = 1
embedding_num_threads = 1
```

吞吐优先且 CPU 资源足够时可尝试：

```conf
embedding_worker_threads = 4
embedding_num_threads = 1
```

不建议回到 `worker_threads=4, embedding_num_threads=4`。

## 12. 是否进入 commit ordering

不建议立即进入 commit ordering 实现。

建议先进入 Stage 8.1 代码审计，审计重点：

- shared `OrtEnv` refcount create/destroy/failure rollback。
- per-worker `OrtSession` 是否仍独立。
- global mutex fallback 是否保留。
- worker stats / runtime final stats 是否能可靠解释生命周期。
- TSAN 归类是否接受：Stage 8.1 新路径无报告，剩余为既有 shutdown/background race。

如果审计通过，再讨论 commit ordering 设计。下一阶段建议先做 commit ordering 的设计文档和风险拆解，不直接写多 executor 实现。
