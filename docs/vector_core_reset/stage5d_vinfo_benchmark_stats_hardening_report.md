# Stage 5D VINFO / benchmark stats hardening report

## 1. 阶段目标

Stage 5D 是 P0 hardening-only 阶段，目标是固化 Stage 5B / Stage 5C 之后的统计口径：

- VINFO backend stats 字段在所有 backend 下稳定可解析。
- VSEARCH profile 字段在所有 backend 下稳定可解析并随 VSEARCH 增长。
- benchmark JSON / Markdown 同时输出 endpoint latency、internal profile timing、memory bytes 和 memory source。
- `backend_search_us` 明确标注为估算口径：`ops search wall us - manager resolve us`。
- `estimated` / `unsupported` 不被报告成真实 measured 内存。
- `resize_count` / `reserve_count` 继续保持 `n/a` + `unsupported`，不伪装成 0。

本阶段不优化算法，不修改 `src/vector/core/*`，不修改 RDB/AOF 格式，不修改 `kvs_vector_index_ops_t` 签名。

## 2. 修改 / 新增文件

Stage 5D 修改：

| 文件 | 类型 | 说明 |
|---|---|---|
| `scripts/bench_vector_core_compare.py` | 修改 | 增加字段稳定性校验、Stage 5D JSON/Markdown 输出、memory source 和 timing source 标注。 |
| `tests/vector_vinfo_backend_stats_test.sh` | 修改 | 完整枚举 backend stats + VSEARCH profile 字段，检查 memory source 与 resize/reserve source 语义。 |
| `tests/vector_vsearch_profile_breakdown_test.sh` | 修改 | 完整枚举 VSEARCH profile 字段，检查计数随 VSEARCH 增长并确认 response 不暴露 `record_id` / `slot`。 |
| `docs/vector_core_reset/stage5d_vinfo_benchmark_stats_hardening_report.md` | 新增 | 本报告。 |

本阶段未修改生产代码。

当前工作区仍包含 Stage 5A / 5B / 5C 已完成但未提交的生产代码和报告文件；Stage 5D 没有清理或改动 unrelated diff。

## 3. VINFO 字段稳定性检查

所有 backend 覆盖：

- `core_flat`
- `core_hnsw`
- `hnswlib_hnsw`
- `faiss_flat`
- `faiss_hnsw`

backend stats 字段均可解析：

```text
backend_name
backend_size
backend_capacity
backend_dim
backend_metric
backend_memory_bytes
backend_memory_bytes_source
backend_deleted_count
backend_needs_rebuild
backend_resize_count
backend_reserve_count
backend_resize_count_source
backend_reserve_count_source
```

Stage 5B VSEARCH profile 字段均可解析：

```text
vsearch_total_count
vsearch_total_avg_us
vsearch_total_us
vsearch_total_max_us
vsearch_backend_search_count
vsearch_backend_search_avg_us
vsearch_backend_search_total_us
vsearch_backend_search_max_us
vsearch_manager_resolve_count
vsearch_manager_resolve_avg_us
vsearch_manager_resolve_total_us
vsearch_manager_resolve_max_us
vsearch_response_assembly_count
vsearch_response_assembly_avg_us
vsearch_response_assembly_total_us
vsearch_response_assembly_max_us
vsearch_raw_hit_count
vsearch_resolved_hit_count
vsearch_filtered_hit_count
```

focused tests 已检查：

- profile count 随 VSEARCH 增长。
- raw hit count >= resolved hit count。
- VSEARCH response 不暴露 `record_id` / `slot`。

## 4. memory source 语义检查

本阶段确认：

| backend | source 规则 | 检查结果 |
|---|---|---|
| `core_flat` | `measured` 且 `backend_memory_bytes > 0` | 通过 |
| `core_hnsw` | `measured` 且 `backend_memory_bytes > 0` | 通过 |
| `hnswlib_hnsw` | `estimated` 或 `unsupported`，不得当作 measured | 通过，本次为 `estimated` |
| `faiss_flat` | `estimated` 或 `unsupported`，不得把旧 `index_memory_bytes=0` 当真实内存 | 通过，本次为 `estimated` |
| `faiss_hnsw` | `estimated` 或 `unsupported`，不得把旧 `index_memory_bytes=0` 当真实内存 | 通过，本次为 `estimated` |

benchmark JSON 每个 result 现在同时保留：

- `backend_stats.backend_memory_bytes`
- `backend_stats.backend_memory_bytes_source`
- 顶层兼容字段 `backend_memory_bytes`
- 顶层兼容字段 `backend_memory_source`

Markdown 表格将 `estimated` 显示为 `estimated (not measured)`。

## 5. resize / reserve unsupported 口径检查

当前 `resize_count` / `reserve_count` 不在 Stage 5D 实现真实计数，输出保持：

```text
backend_resize_count:n/a
backend_reserve_count:n/a
backend_resize_count_source:unsupported
backend_reserve_count_source:unsupported
```

focused tests 和 benchmark `stats_field_check` 均检查该口径。未输出 misleading `0`。

## 6. benchmark JSON / Markdown 字段

JSON 顶层新增 / 固化：

```text
stage: 5D
backend_stats_keys
vsearch_profile_keys
backend_search_timing_source: estimated_from_ops_search_wall_us_minus_manager_resolve_us
memory_source_policy: core_flat/core_hnsw measured; hnswlib/faiss estimated-or-unsupported
```

每个 backend result 包含：

```text
backend_stats
vsearch_profile
stats_field_check
backend_memory_bytes
backend_memory_source
vsearch_latency
recall_at_10
rebuild
rdb_load_rebuild
```

Markdown 表头稳定为：

```text
backend
status
endpoint VSEARCH avg ms
backend search avg us (estimated)
manager resolve avg us
response assembly avg us
raw hits/query
resolved hits/query
recall@10
backend size
backend capacity
backend memory bytes
memory source
resize/reserve
stats fields
rebuild wall ms
RDB load+rebuild ms
notes
```

## 7. 1K benchmark 结果

命令：

```bash
N=1000 DIM=64 TOPK=10 QUERIES=50 tests/vector_core_benchmark_compare.sh
```

输出：

- `results/vector_core_benchmark/stage5d_stats_hardening_benchmark_1k_64d.json`
- `results/vector_core_benchmark/stage5d_stats_hardening_benchmark_1k_64d.md`

| backend | status | endpoint avg ms | backend search avg us | manager resolve avg us | response avg us | memory bytes | memory source | recall@10 |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| core_flat | pass | 0.105 | 48 | 1 | 7 | 304128 | measured | 1.0000 |
| core_hnsw | pass | 0.151 | 78 | 2 | 9 | 2627792 | measured | 1.0000 |
| hnswlib_hnsw | pass | 0.434 | 353 | 2 | 9 | 447816 | estimated | 1.0000 |
| faiss_flat | pass | 0.148 | 93 | 1 | 7 | 264000 | estimated | 1.0000 |
| faiss_hnsw | pass | 0.467 | 400 | 2 | 8 | 392000 | estimated | 0.9900 |

所有 backend 的 `stats_field_check` 均为 ok。

## 8. 10K benchmark 结果

命令：

```bash
N=10000 DIM=64 TOPK=10 QUERIES=100 tests/vector_core_benchmark_compare.sh
```

输出：

- `results/vector_core_benchmark/stage5d_stats_hardening_benchmark_10k_64d.json`
- `results/vector_core_benchmark/stage5d_stats_hardening_benchmark_10k_64d.md`

| backend | status | endpoint avg ms | backend search avg us | manager resolve avg us | response avg us | memory bytes | memory source | recall@10 |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| core_flat | pass | 0.453 | 371 | 2 | 8 | 4866048 | measured | 1.0000 |
| core_hnsw | pass | 0.158 | 78 | 3 | 9 | 41826512 | measured | 0.9870 |
| hnswlib_hnsw | pass | 0.495 | 411 | 3 | 9 | 4020816 | estimated | 0.9980 |
| faiss_flat | pass | 29.412 | 29191 | 5 | 12 | 2640000 | estimated | 0.9990 |
| faiss_hnsw | pass | 0.115 | 47 | 2 | 8 | 3920000 | estimated | 0.9960 |

所有 backend 的 `stats_field_check` 均为 ok。

说明：本次 10K `faiss_flat` endpoint / backend timing 明显偏高，属于本次 run 的性能观测值。Stage 5D 不修改 search 算法、manager resolve、FAISS backend 或 response 格式，因此不把该数值归因为本阶段行为变化。

## 9. 未运行更大规模 benchmark 的原因

未运行 100K。

原因：

- Stage 5D 的目标是统计字段和报告口径 hardening，不是大规模性能结论。
- 用户要求至少执行 1K，环境允许时执行 10K；本阶段已完成 1K 和 10K。
- 100K 在历史阶段中受 core_hnsw rebuild / hnswlib RDB reload 时间影响较大，属于 P1/P2 性能归因范围，不应混入本 P0 hardening 阶段。

## 10. tests 结果

构建：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：PASS。

回归和 stage tests：

```bash
tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
tests/vector_manager_resolve_fastpath_test.sh
tests/vector_vsearch_profile_breakdown_test.sh
tests/vector_vinfo_backend_stats_test.sh
```

结果：全部 PASS。

## 11. recall@10 是否回退

未发现由 Stage 5D 引入的 recall 回退。

Stage 5D 只修改 benchmark 脚本、focused tests 和报告，不修改 search 算法、score、topK、manager resolve 或 response 格式。

本轮最终 benchmark：

- 1K：`core_flat/core_hnsw/hnswlib_hnsw/faiss_flat` 为 1.0000，`faiss_hnsw` 为 0.9900。
- 10K：`core_flat` 为 1.0000，`core_hnsw` 为 0.9870，`hnswlib_hnsw` 为 0.9980，`faiss_flat` 为 0.9990，`faiss_hnsw` 为 0.9960。

ANN backend 的 recall 差异是既有近似检索/排序差异，不是 Stage 5D 产生的新语义变化。

## 12. 静态边界检查结果

命令：

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
grep -RIn "record_id_map" src/vector/core include/vector 2>/dev/null
git diff -- src/persistence
```

结果：

- `src/vector/core` 未新增 `g_vector_state`；命中仅为既有 include 注释 / manager extern。
- `src/vector/core` 未命中 `scope` / `ttl` / `metadata` / `question` / `answer`。
- `kvs_vector_index_ops_t` 签名未修改。
- `record_id_map` 未进入 `src/vector/core`，仅在 manager record 头部声明。
- `src/persistence` 无 diff，RDB/AOF 格式未修改。

## 13. git status --short

Stage 5D 完成后工作区仍含 Stage 5A / 5B / 5C 的未提交改动：

```text
 M include/kvs_vector.h
 M include/vector/kvs_vector_manager.h
 M include/vector/kvs_vector_record.h
 M scripts/bench_vector_core_compare.py
 M src/core/kvs_command_executor.c
 M src/core/kvs_command_task.c
 M src/core/kvs_operation.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_hnswlib_ops.c
 M src/vector/kvs_vector_index.c
?? AGENTS.md
?? docs/vector_core_reset/stage5a_manager_resolve_fastpath_report.md
?? docs/vector_core_reset/stage5b_vsearch_profile_breakdown_report.md
?? docs/vector_core_reset/stage5c_vinfo_backend_stats_report.md
?? docs/vector_core_reset/stage5d_vinfo_benchmark_stats_hardening_report.md
?? docs/vector_core_reset/优化步骤.md
?? tests/vector_manager_resolve_fastpath_test.sh
?? tests/vector_vinfo_backend_stats_test.sh
?? tests/vector_vsearch_profile_breakdown_test.sh
```

`AGENTS.md` 和 `docs/vector_core_reset/优化步骤.md` 为既有未跟踪文件，本阶段未处理。

## 14. 下一步建议

Next Stage:

```text
Stage 5E: P0 stats/reporting final audit and baseline freeze
```

Reason:

1. Stage 5A/5B/5C/5D 已完成 manager resolve fast path、VSEARCH profile、VINFO backend stats 和 benchmark stats hardening。
2. 进入 P1 core_hnsw memory/rebuild 前，应先把当前 JSON/Markdown benchmark baseline、VINFO 字段和 focused tests 固定为可复跑基线。
3. Stage 5D 仍发现 run-to-run 性能波动，例如 10K `faiss_flat` 单次偏高；P1 前需要明确是否采用多轮 median 或只保留 smoke benchmark。

Allowed files:

```text
scripts/bench_vector_core_compare.py
tests/vector_core_benchmark_compare.sh
docs/vector_core_reset/stage5e_*.md
```

Forbidden:

```text
src/vector/core/*
RDB/AOF 格式
kvs_vector_index_ops_t 签名
VSEARCH response 格式
core_hnsw graph / memory / rebuild 优化
external backend 替换
```

Acceptance:

```text
1. 明确 benchmark baseline 文件命名和 Stage 5A-5D 最终验收矩阵。
2. 明确是否需要多轮 benchmark median。
3. 不修改生产代码。
4. 全量 vector regression + Stage 5A/5B/5C/5D focused tests 继续通过。
```
