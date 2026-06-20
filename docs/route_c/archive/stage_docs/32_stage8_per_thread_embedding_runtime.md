# Stage 8: per-thread embedding runtime / OrtSession

## 1. 背景

Stage 7 已经把 VSET / VSEARCH 的 embedding prepare 从 single command executor 拆到 embedding worker pool。worker 只执行 `kvs_command_task_prepare()`，prepare 完成后 task 回到 command executor，再串行执行 execute_sync / commit / search / post_commit / finalize。

Stage 7 的闭环是正确的，但 ONNX/local_c provider encode 外围仍有全局 provider mutex。这个互斥保证了共享 `EmbeddingHandle` 的安全，但也把多个 worker 的 `embedding_encode()` 重新串行化。因此 Stage 8 的目标是：

```text
embedding encode 并发，状态 commit 串行。
```

本阶段不做多 command executor、不做 commit ordering、不做 pipeline 并行，也不改变 RESP/RDB/AOF/replication/vector 语义。

## 2. 为什么不做多 executor

当前瓶颈在 VSET/VSEARCH prepare 阶段的 embedding encode。多 command executor 会立即引入跨连接 commit ordering、AOF ordering、replication ordering、HNSW/id_map 可见性和错误回滚问题，超出本阶段边界。

Stage 8 只释放 Stage 7 worker pool 的 encode 并发能力：

```text
network owner loop
  -> command_task
  -> single command executor
  -> embedding worker pool
  -> per-worker embedding runtime encode
  -> prepared_queue
  -> single command executor serial commit/search/finalize
  -> owner-aware response queue
```

## 3. Provider 线程安全审计

### ONNX / local_c provider

`embedding_runtime_onnx.c` 和非 ONNX `embedding_runtime.c` 都以 `EmbeddingHandle` 为 provider handle。共享 handle 不能直接无锁并发使用，原因包括：

- `last_error` 是 handle-local 可变 buffer。
- ONNX wrapper 中 profile counters、`profile_inflight`、`profile_max_inflight` 是 handle-local 可变状态。
- ONNX `EmbeddingHandle` 持有 `OrtEnv`、`OrtSessionOptions`、`OrtSession`、`OrtMemoryInfo` 和 vocab/tokenizer 状态。
- encode 的 input ids / attention mask / token type ids / Ort tensor / output tensor 是每次调用局部分配，适合并发；但 wrapper state 仍属于 handle。
- destroy 生命周期必须保证不与 running encode 并发。

结论：不在共享 global handle 上移除 mutex。Stage 8 为每个 embedding worker 创建独立 `EmbeddingHandle`。ONNX build 下该 handle 独立创建 `OrtEnv`、`OrtSessionOptions`、`OrtSession`、`OrtMemoryInfo`、vocab 和 handle-local `last_error`。

### hash provider

hash provider 是本地 deterministic hash 计算，不依赖 `EmbeddingHandle`，也没有共享可变状态。本阶段保持无锁路径。

## 4. 实现设计

新增/扩展的 embedding runtime API 位于 `include/kvs_vector.h` 和 `src/vector/kvs_vector_embedding.c`：

```c
int kvs_vector_embedding_thread_context_init(void);
void kvs_vector_embedding_thread_context_destroy(void);
int kvs_vector_embedding_thread_context_available(void);
void kvs_vector_embedding_runtime_get_stats(kvs_vector_embedding_runtime_stats_t *out);
```

`src/vector/kvs_vector_embedding.c` 内部新增 TLS：

```c
static __thread kvs_vector_embedding_thread_context_t *g_embedding_thread_ctx;
```

worker thread 启动时调用 `kvs_vector_embedding_thread_context_init()`。如果配置开启且当前 provider 是 local_c/c_api，则通过 `embedding_create()` 创建该 worker 独有的 `EmbeddingHandle`。worker 退出前调用 `kvs_vector_embedding_thread_context_destroy()`，先清 TLS，再 `embedding_destroy()`。

encode 选择逻辑：

- 有可用 TLS context，且 provider type/dimension 匹配：使用 per-thread handle，不获取 global provider mutex。
- 没有 TLS context：使用 global provider handle，并继续走 Stage 7 的 global provider mutex。
- hash provider：保持原无锁 hash encode。

command executor inline prepare、worker disabled、context 创建失败 fallback 都继续使用 global mutex，因此普通路径保持安全。

## 5. Worker Pool 接入

`src/core/kvs_embedding_worker_pool.c` 的 worker 生命周期变为：

```text
worker_thread_main:
  kvs_vector_embedding_thread_context_init()
  while running:
    pop prepare_queue
    kvs_command_task_prepare(task)
    kvs_command_executor_submit_prepared(task)
  kvs_vector_embedding_thread_context_destroy()
```

worker 仍只做 prepare：

- 不访问全局 store。
- 不持有 `g_kv_data_rwlock`。
- 不写 AOF。
- 不做 replication。
- 不生成最终 response。
- 不直接访问 reactor/proactor。

如果 context 初始化失败且 `embedding_per_thread_runtime_fallback_mutex=1`，worker 继续运行，但 encode 走 global mutex fallback；如果 fallback 关闭，则 init 返回错误。当前 worker pool 选择保守 fallback 语义，保证 correctness 优先。

## 6. 生命周期和顺序

task / response / session ownership 没有变化：

- 网络线程创建 heap task、owned argv、response_task，并持有 session ref。
- command executor 提交 VSET/VSEARCH 到 worker pool 后不销毁 task、不释放 session ref。
- worker prepare 完成后回投 prepared queue。
- command executor 从 prepared queue 取回 task，转移 session ref 到 response_task，串行 execute_sync / post_commit / finalize。
- response 仍通过 owner-aware response queue 回投 reactor/proactor。
- 网络 owner loop dispatch 后释放 response_task session ref 并销毁 response_task。

commit/search/finalize 仍只在 single command executor 线程执行。VSET 仍在串行路径写 record / HNSW / id_map，VSEARCH 仍在串行路径读取索引并生成 response，AOF/replication 顺序仍以后续串行 commit 顺序为准。

同 session pipeline 保序仍由 `single_inflight + send_buf gating` 保证：

- 同一连接一次只有一个 in-flight task。
- send_buf append/drop 完成后才 restore_read。
- worker pool 不会让同一 session 的多个 VSET/VSEARCH 并行 prepare。
- 不做 response reorder。

不同 session 的 prepare 完成顺序可能影响进入 serial commit 的顺序。KVLite 当前不承诺跨连接到达顺序的全局严格线性化；如需更强语义，需要后续 commit ordering 设计。

## 7. 配置项

默认值保持保守：

```conf
embedding_per_thread_runtime_enabled = 0
embedding_per_thread_runtime_fallback_mutex = 1
```

`config/kvstore-embedding.conf` 显式开启 Stage 8 测试路径：

```conf
embedding_worker_pool_enabled = 1
embedding_worker_threads = 4
embedding_worker_queue_capacity = 1024
embedding_per_thread_runtime_enabled = 1
embedding_per_thread_runtime_fallback_mutex = 1
```

命令行也支持：

```text
--embedding_per_thread_runtime_enabled yes|no
--embedding_per_thread_runtime_fallback_mutex yes|no
```

## 8. Stats

worker pool shutdown log 继续输出 Stage 7 prepare stats，并新增 Stage 8 runtime stats：

- `per_thread_enabled`
- `contexts_created`
- `contexts_destroyed`
- `context_create_failed`
- `context_fallback`
- `encode_global_mutex`
- `encode_thread_context`
- `encode_current_inflight`
- `encode_max_inflight`
- `encode_avg_us`
- `encode_max_us`

这些字段用于回答：

- worker 是否创建了 per-thread context。
- context created/destroyed 是否匹配。
- encode 是否走 thread context，还是 fallback 到 global mutex。
- `encode_max_inflight` 是否大于 1。
- per-thread runtime 是否引入新的 shutdown 泄漏。

## 9. Correctness 测试

已运行：

```bash
make embedding ENABLE_EBPF=0
make clean && make embedding
tests/embedding_per_thread_runtime_stage8.sh
```

`tests/embedding_per_thread_runtime_stage8.sh` 覆盖：

- reactor / proactor。
- worker disabled。
- worker enabled + per-thread runtime disabled，作为 Stage 7 mutex baseline。
- worker enabled + per-thread runtime enabled。
- `semantic_onnx_test.sh`。
- VSET / VGET / VSEARCH / VDEL。
- TTL EXPIRE / PERSIST。
- repeat VSET。
- invalid VSET / invalid VSEARCH / unknown vector command。
- pipeline 10 / 50。
- fd reuse。
- high accept 200。
- 多连接并发 VSET。
- stats 校验 submitted == completed。
- per-thread 模式校验 contexts_created == contexts_destroyed、thread_context encode > 0、`encode_max_inflight > 1`。

关键 stats：

| mode | config | contexts | global_mutex | thread_context | encode_max_inflight |
|---|---:|---:|---:|---:|---:|
| reactor | Stage 7 mutex | 0/0 | 515 | 0 | 1 |
| reactor | Stage 8 per-thread | 4/4 | 0 | 524 | 4 |
| proactor | Stage 7 mutex | 0/0 | 532 | 0 | 1 |
| proactor | Stage 8 per-thread | 4/4 | 0 | 532 | 4 |

结论：per-thread runtime 在 reactor/proactor 下均真实启用，context 创建/销毁匹配，encode 并发度达到 worker 数。

## 10. 性能对比

已运行：

```bash
RESULT_DIR=results/stage8_per_thread_runtime_rerun tests/embedding_per_thread_runtime_perf_stage8.sh
```

结果目录：`results/stage8_per_thread_runtime_rerun/`

### latency

| mode | config | VSET avg/p50/p95/p99 ms | VSET qps | VSEARCH avg/p50/p95/p99 ms | VSEARCH qps |
|---|---|---:|---:|---:|---:|
| reactor | worker off | 3.14 / 2.98 / 4.45 / 4.58 | 318.28 | 1.59 / 1.51 / 1.99 / 3.05 | 629.02 |
| reactor | Stage7 threads=2 mutex | 3.16 / 2.99 / 4.73 / 4.84 | 316.29 | 1.60 / 1.52 / 2.03 / 3.54 | 625.30 |
| reactor | Stage8 threads=2 | 9.54 / 8.05 / 21.00 / 27.91 | 104.80 | 4.18 / 3.30 / 14.76 / 22.75 | 239.43 |
| reactor | Stage8 threads=4 | 16.94 / 12.28 / 41.25 / 49.93 | 59.01 | 6.27 / 3.99 / 23.05 / 41.40 | 159.44 |
| proactor | worker off | 3.22 / 3.09 / 3.63 / 4.56 | 310.30 | 1.62 / 1.58 / 1.94 / 2.17 | 618.45 |
| proactor | Stage7 threads=2 mutex | 3.13 / 3.08 / 3.54 / 3.65 | 319.59 | 2.06 / 1.53 / 3.53 / 14.32 | 485.86 |
| proactor | Stage8 threads=2 | 9.07 / 7.54 / 18.07 / 21.70 | 110.24 | 4.13 / 3.16 / 14.60 / 14.92 | 242.06 |
| proactor | Stage8 threads=4 | 11.70 / 8.76 / 24.07 / 40.30 | 85.46 | 5.65 / 4.00 / 16.59 / 25.50 | 176.94 |

### throughput c=16

| mode | config | VSET qps | VSEARCH qps | KV SET/GET c=4 qps |
|---|---|---:|---:|---:|
| reactor | worker off | 326.03 | 611.10 | 10362.73 |
| reactor | Stage7 threads=2 mutex | 247.65 | 616.32 | 9211.60 |
| reactor | Stage8 threads=2 | 130.84 | 227.94 | 10052.95 |
| reactor | Stage8 threads=4 | 101.24 | 214.65 | 11450.98 |
| proactor | worker off | 343.65 | 644.05 | 11391.72 |
| proactor | Stage7 threads=2 mutex | 335.90 | 680.21 | 7778.51 |
| proactor | Stage8 threads=2 | 150.81 | 296.25 | 12977.45 |
| proactor | Stage8 threads=4 | 128.74 | 237.70 | 6251.55 |

### runtime stats from perf run

| mode | config | contexts | global_mutex | thread_context | encode_max_inflight |
|---|---|---:|---:|---:|---:|
| reactor | Stage7 threads=2 mutex | 0/0 | 1560 | 0 | 1 |
| reactor | Stage8 threads=2 | 2/2 | 0 | 1560 | 2 |
| reactor | Stage8 threads=4 | 4/4 | 0 | 1560 | 4 |
| proactor | Stage7 threads=2 mutex | 0/0 | 1560 | 0 | 1 |
| proactor | Stage8 threads=2 | 2/2 | 0 | 1560 | 2 |
| proactor | Stage8 threads=4 | 4/4 | 0 | 1560 | 4 |

性能结论：

- Stage 8 已释放真实 encode 并发，`encode_max_inflight` 达到 2/4，且不再走 global mutex。
- 本环境中 per-thread runtime 性能反而下降，threads=4 尾延迟明显变差。
- 这不是 correctness 失败，而是 runtime 资源竞争问题：每个 worker 独立 handle/OrtSession，加上当前 `embedding_num_threads=4`，会放大 CPU / memory / ORT 内部线程池竞争。
- 普通 KV SET/GET smoke 没有显示协议或网络闭环退化。

后续性能优化应先调小 per-session `embedding_num_threads`、评估共享 OrtEnv / 独立 OrtSession、控制 worker threads 与 ORT intra threads 的乘积，再考虑更复杂的 executor/commit ordering。

## 11. Sanitizer

已运行：

```bash
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=address,undefined" \
make embedding ENABLE_EBPF=0

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
tests/embedding_per_thread_runtime_stage8.sh
```

结果：

- ASAN/UBSAN functional correctness 通过。
- per-thread context stats 在 ASAN run 中仍满足 created == destroyed，`encode_max_inflight=4`。
- LSAN 在 shutdown 后仍报告既有清理噪音，堆栈落在 vector/HNSW、RESP parser、reactor/proactor ops、`kvs_command_task_copy_argv`、`master_sync_command` 等历史路径。
- 未发现指向 per-thread `EmbeddingHandle` / `OrtSession` / worker context / pthread lifecycle 的新增泄漏堆栈。
- TSAN 未运行。本阶段未把 TSAN 噪音专项纳入范围；如果后续运行后发现 worker context / runtime stats race，需要修。

## 12. 已知限制

- per-thread runtime 默认关闭，embedding 配置文件显式开启。
- 当前 per-thread handle 会独立加载 ONNX session/vocab，启动成本和内存占用会上升。
- 本次性能结果显示真实并发已开启，但吞吐没有提升；瓶颈可能转移到 ORT 内部线程竞争、CPU core、memory bandwidth 或 HNSW/search/commit 串行段。
- fallback path 语义已保留，但本轮未强制模拟 `embedding_create()` 失败。
- 不解决 vector/HNSW/RESP/proactor/reactor 既有 LSAN shutdown 清理噪音。
- 不做 TSAN shutdown 专项。

## 13. 审查结论和下一步

建议进入 Stage 8 代码审查。审查重点：

- per-thread context lifecycle 是否与 worker lifecycle 完全绑定。
- context init 失败 fallback 语义是否符合预期。
- global mutex fallback 是否仍覆盖 inline prepare / worker disabled。
- stats 是否足够支撑性能归因。

不建议下一阶段直接进入多 executor 或 commit ordering。当前更现实的 P0 是 embedding runtime 调优：

1. 评估每个 per-thread OrtSession 的 `embedding_num_threads`，避免 `worker_threads * embedding_num_threads` 过度抢占。
2. 评估共享 OrtEnv + per-worker OrtSession，减少 session 创建和 runtime 资源开销。
3. 如确认 encode 性能可控，再讨论 commit ordering 或多 executor。

