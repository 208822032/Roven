# Stage 5B VSEARCH profile breakdown report

## 1. 阶段目标

Stage 5B 的目标是补齐 VSEARCH profiling，把端到端 VSEARCH latency 拆成可观测的内部口径：

- backend search
- manager resolve
- response assembly
- raw / resolved / filtered hit count

本阶段是 P0 instrumentation-only，不优化算法，不修改 search 结果、score、topK、scope / threshold / deleted 过滤语义，不修改 VSEARCH response 格式。

## 2. 修改 / 新增文件

Stage 5B 文件：

- `include/kvs_vector.h`
- `src/vector/kvs_vector.c`
- `src/core/kvs_operation.c`
- `src/core/kvs_command_task.c`
- `src/core/kvs_command_executor.c`
- `scripts/bench_vector_core_compare.py`
- `tests/vector_vsearch_profile_breakdown_test.sh`
- `docs/vector_core_reset/stage5b_vsearch_profile_breakdown_report.md`

说明：

- 用户允许在 profiling 头文件或实现文件实际路径不同的情况下最小修改现有 profiling 相关文件。
- 当前运行时 VINFO/profile stats 的主通道实际在 `include/kvs_vector.h`、`src/vector/kvs_vector.c`、`src/core/kvs_operation.c`、`src/core/kvs_command_task.c`、`src/core/kvs_command_executor.c`，不是只在 `include/kvs_vcmd_profile.h` / `src/core/kvs_vcmd_profile.c`。
- 因此本阶段复用了现有 runtime stats + VINFO 机制，没有新增另一套独立 profile 输出。

未修改：

- `src/vector/core/*`
- `src/persistence/*`
- `include/vector/kvs_vector_index_internal.h` 中的 `kvs_vector_index_ops_t`
- RDB/AOF 格式
- VSEARCH response 格式

## 3. 是否修改生产代码

是。

修改性质：

- 只新增 runtime profiling counters、VINFO 输出字段、benchmark 报告列。
- 不改变 core/backend 搜索算法。
- 不改变 manager 过滤语义。
- 不改变 response 字符串内容。
- 不改变 RDB/AOF 持久化格式。

## 4. 当前 VSEARCH 调用链

同步路径：

```text
VSEARCH
  -> handle_vsearch_command()
  -> kvs_vector_search_prepare()
  -> kvs_vector_search_with_prepared()
  -> kvs_vector_index_search_current()
  -> current backend ops->search()
  -> kvs_vector_manager_resolve_hits()
  -> response assembly in handle_vsearch_command()
```

prepared worker 路径：

```text
VSEARCH
  -> kvs_command_task_prepare()
  -> kvs_vector_search_prepare()
  -> kvs_command_task_execute_sync()
  -> kvs_vector_search_prepared_to_resp()
  -> kvs_vector_search_with_prepared()
  -> kvs_vector_index_search_current()
  -> current backend ops->search()
  -> kvs_vector_manager_resolve_hits()
  -> response assembly in kvs_vector_search_prepared_to_resp()
```

## 5. profiling 字段设计

VINFO 新增 / 暴露字段：

- `vsearch_total_count`
- `vsearch_total_avg_us`
- `vsearch_total_us`
- `vsearch_total_max_us`
- `vsearch_backend_search_count`
- `vsearch_backend_search_avg_us`
- `vsearch_backend_search_total_us`
- `vsearch_backend_search_max_us`
- `vsearch_manager_resolve_count`
- `vsearch_manager_resolve_avg_us`
- `vsearch_manager_resolve_total_us`
- `vsearch_manager_resolve_max_us`
- `vsearch_response_assembly_count`
- `vsearch_response_assembly_avg_us`
- `vsearch_response_assembly_total_us`
- `vsearch_response_assembly_max_us`
- `vsearch_raw_hit_count`
- `vsearch_resolved_hit_count`
- `vsearch_filtered_hit_count`

兼容别名：

- `vsearch_legacy_search_count`
- `vsearch_legacy_search_avg_us`
- `vsearch_legacy_response_build_count`
- `vsearch_legacy_response_build_avg_us`

## 6. endpoint latency 与 internal timing 的口径差异

benchmark 中的 `endpoint VSEARCH avg ms` 是客户端侧 RESP 命令往返时间，包含：

- socket send / recv
- server 排队与调度
- embedding prepare
- backend search
- manager resolve
- response assembly
- response push

VINFO 中的 internal timing 是 server 内部统计：

- `vsearch_total_*`: prepare 完成后到 response assembly 完成的内部 VSEARCH 总耗时，不包含网络传输。
- `vsearch_backend_search_*`: backend ops search 的 wall time 扣除 manager resolve 后的估算 backend search 时间。
- `vsearch_manager_resolve_*`: manager resolve 边界内耗时。
- `vsearch_response_assembly_*`: RESP array / bulk string 组装耗时。

因此 endpoint latency 不应与三段 internal timing 直接求和比较；它们是不同观测口径。

## 7. backend_search_us 采集位置

采集位置：

```text
src/vector/kvs_vector.c
  -> kvs_vector_index_search_current()
```

实现方式：

- 在调用 `index->ops->search()` 前后记录 wall time。
- manager resolve 在 backend adapter 内部发生，因此用 thread-local `g_vsearch_current_resolve_us` 累加当前 search 中的 resolve 时间。
- backend search 计为 `ops->search_wall_us - manager_resolve_us`。

这样不需要修改 `kvs_vector_index_ops_t` 签名，也覆盖所有当前 backend。

## 8. manager_resolve_us 采集位置

采集位置：

```text
src/vector/kvs_vector.c
  -> kvs_vector_manager_resolve_hits()
```

统计内容：

- resolve wall time
- 输入 `hit_count` 作为 raw hits
- 输出 `out_count` 作为 resolved hits
- `raw - resolved` 作为 filtered hits

说明：

- `filtered_hit_count` 是低成本统计，表示没有进入最终 result 的 raw hits 数量。
- 它覆盖 threshold、not found、inactive、deleted、scope mismatch、结果容量截断等所有原因，不拆分 skipped-by-reason。

## 9. response_assembly_us 是否实现

已实现。

采集位置：

- 同步路径：`src/core/kvs_operation.c -> handle_vsearch_command()`
- prepared worker 路径：`src/core/kvs_command_task.c -> kvs_vector_search_prepared_to_resp()`

字段映射：

- 现有 `vsearch_response_build_*` runtime stats 作为 VINFO 中的 `vsearch_response_assembly_*` 输出。

## 10. raw_hit_count / resolved_hit_count 统计方式

统计位置：

```text
kvs_vector_manager_resolve_hits(hits, hit_count, ..., results, ..., &results_count)
```

口径：

- `vsearch_raw_hit_count += hit_count`
- `vsearch_resolved_hit_count += out_count`
- `vsearch_filtered_hit_count += hit_count - out_count`，仅在 raw > resolved 时增加

输出顺序仍由 backend hit score 顺序决定；Stage 5B 不改变排序和 score。

## 11. backend 覆盖情况

focused test 覆盖：

- `core_flat`
- `core_hnsw`
- `hnswlib_hnsw`
- `faiss_flat`
- `faiss_hnsw`

覆盖方式：

- 每个 backend 启动独立 server。
- 写入多条 vector records。
- 执行 VSEARCH。
- 读取 VINFO。
- 断言 total / backend / resolve / response counters 随 VSEARCH 增加。
- 断言 raw/resolved hits 非零且 raw >= resolved。
- 断言 response 不暴露 `record_id` / `slot`。

## 12. build 结果

通过：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：

```text
构建完成
二进制: kvstore src/eBPF/ebpf_syncd
```

## 13. tests 结果

通过：

```text
tests/vector_core_flat_correctness_test.sh       PASS
tests/vector_core_flat_rebuild_test.sh           PASS
tests/vector_core_flat_rdb_reload_test.sh        PASS
tests/vector_core_hnsw_correctness_test.sh       PASS
tests/vector_core_hnsw_rebuild_test.sh           PASS
tests/vector_core_hnsw_rdb_reload_test.sh        PASS
tests/vector_core_hnsw_aof_replay_test.sh        PASS
tests/vector_manager_resolve_fastpath_test.sh    PASS
tests/vector_vsearch_profile_breakdown_test.sh   PASS
```

## 14. benchmark 结果

命令：

```bash
N=1000 DIM=64 TOPK=10 QUERIES=50 tests/vector_core_benchmark_compare.sh
```

输出：

- `results/vector_core_benchmark/stage5b_vsearch_profile_benchmark_1k_64d.json`
- `results/vector_core_benchmark/stage5b_vsearch_profile_benchmark_1k_64d.md`

摘要：

| backend | endpoint avg ms | backend avg us | resolve avg us | response avg us | raw/query | resolved/query | recall@10 |
|---|---:|---:|---:|---:|---:|---:|---:|
| core_flat | 0.111 | 53 | 1 | 7 | 42.00 | 10.00 | 1.0000 |
| core_hnsw | 0.149 | 70 | 2 | 9 | 42.00 | 10.00 | 1.0000 |
| hnswlib_hnsw | 0.443 | 347 | 4 | 9 | 72.00 | 10.00 | 1.0000 |
| faiss_flat | 0.705 | 467 | 1 | 7 | 72.00 | 10.00 | 1.0000 |
| faiss_hnsw | 12.488 | 12198 | 4 | 10 | 72.00 | 10.00 | 0.9900 |

说明：

- 该 benchmark 是 1K / 64d / 50 queries 小规模 smoke benchmark。
- Stage 5B 的核心结论是 profile 字段可读且能拆分 backend / resolve / response。
- faiss_hnsw 本次 run 的 backend_search_us 明显高于其他 backend，应在后续 P1 benchmark 中复测，不在 Stage 5B 内优化。

## 15. recall@10 是否回退

未发现由 Stage 5B instrumentation 导致的 recall 回退。

1K benchmark：

- `core_flat`: 1.0000
- `core_hnsw`: 1.0000
- `hnswlib_hnsw`: 1.0000
- `faiss_flat`: 1.0000
- `faiss_hnsw`: 0.9900

Stage 5B 不改变 search 算法、score、topK、scope / threshold / deleted 过滤语义，因此 recall 变化不应来自本阶段逻辑。

## 16. 静态边界检查结果

命令：

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
grep -RIn "record_id_map" src/vector/core include/vector 2>/dev/null
```

结果摘要：

- `src/vector/core/*` 未新增 `g_vector_state` 访问。
- `src/vector/core/*` 未新增 scope / ttl / metadata / question / answer 逻辑。
- `record_id_map` 未进入 `src/vector/core/*`，只在 manager/record header 中出现。
- `kvs_vector_index_ops_t` 签名未修改。
- `src/persistence/*` 无 diff，RDB/AOF 格式未修改。

已知 grep 命中说明：

- `include/vector/kvs_vector_manager.h:128` 的 `extern kvs_vector_state_t g_vector_state` 是既有 manager 声明。
- `include/vector/kvs_vector_index_internal.h` 中的 `g_vector_state` 命中是既有注释。

## 17. git status --short

当前工作区仍包含 Stage 5A 未提交产物和本 Stage 5B 产物：

```text
 M include/kvs_vector.h
 M include/vector/kvs_vector_manager.h
 M include/vector/kvs_vector_record.h
 M scripts/bench_vector_core_compare.py
 M src/core/kvs_command_executor.c
 M src/core/kvs_command_task.c
 M src/core/kvs_operation.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_index.c
?? AGENTS.md
?? docs/vector_core_reset/stage5a_manager_resolve_fastpath_report.md
?? docs/vector_core_reset/stage5b_vsearch_profile_breakdown_report.md
?? docs/vector_core_reset/优化步骤.md
?? tests/vector_manager_resolve_fastpath_test.sh
?? tests/vector_vsearch_profile_breakdown_test.sh
```

说明：

- `AGENTS.md` 和 `docs/vector_core_reset/优化步骤.md` 是本阶段未处理的既有 untracked 文件。
- Stage 5B 没有处理 unrelated docs 删除或 unrelated git diff。

## 18. 下一步建议

Next Stage:

```text
Stage 5C: VINFO backend capacity / resize / memory stats completion
```

Reason:

1. Stage 5A 已把 manager resolve 降到低微秒级。
2. Stage 5B 已证明 backend search 是当前主要内部耗时，且 profile 口径可读。
3. 在进入 core_hnsw memory/rebuild P1 前，应先补齐 backend capacity / resize / memory stats，避免 P1 benchmark 缺关键归因字段。

Allowed files:

```text
include/kvs_vector.h
src/vector/kvs_vector.c
src/core/kvs_operation.c
scripts/bench_vector_core_compare.py
tests/vector_vinfo_backend_stats_test.sh
docs/vector_core_reset/stage5c_vinfo_backend_stats_report.md
```

Forbidden:

```text
src/vector/core/*
RDB/AOF format
kvs_vector_index_ops_t signatures
core_hnsw graph / memory / rebuild optimization
backend replacement
VSEARCH response format
```

Acceptance:

```text
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_vinfo_backend_stats_test.sh
tests/vector_vsearch_profile_breakdown_test.sh
N=1000 DIM=64 TOPK=10 QUERIES=50 tests/vector_core_benchmark_compare.sh
static boundary checks unchanged
```
