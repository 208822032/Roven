# Stage 5C VINFO Backend Stats Report

## 1. 阶段目标

Stage 5C 是 P0 observability-only 阶段。目标是在不优化算法、不修改持久化格式、不改变 VSEARCH response 的前提下，补齐 VINFO backend capacity / resize / memory stats，并让 benchmark 能可靠展示 backend 内部容量、有效数量、内存口径和 Stage 5B VSEARCH profiling breakdown。

## 2. 修改/新增文件

- `include/kvs_vector.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_hnswlib_ops.c`
- `src/core/kvs_operation.c`
- `scripts/bench_vector_core_compare.py`
- `tests/vector_vinfo_backend_stats_test.sh`
- `docs/vector_core_reset/stage5c_vinfo_backend_stats_report.md`

`src/vector/kvs_vector_hnswlib_ops.c` 是允许列表外的最小相邻 helper 修改。原因是 hnswlib 已有 `kvs_hnswlib_index_get_max_elements()`，但 VINFO runtime path 会把 `hnswlib_capacity` 覆盖为 0；新增弱/强函数指针只把已有 capacity getter 接入 VINFO，不改 `kvs_vector_index_ops_t` 签名，不改 backend 算法。

## 3. 是否修改生产代码

是。修改仅限运行时 stats/VINFO/benchmark reporting：

- 新增 `kvs_vector_info_t` 统一 backend stats 字段。
- 在 `kvs_vector_info()` 尾部统一归一化当前 backend stats。
- VINFO 追加字段。
- benchmark 解析并输出 backend stats。

未修改 `src/vector/core/*`，未修改 RDB/AOF，未修改 VSEARCH response。

## 4. 当前 VINFO stats 现状

Stage 5B 后 VINFO 已有：

- `index_type`
- `index_size`
- `capacity`
- `dimension`
- `metric`
- `index_memory_bytes`
- `vector_memory_bytes`
- `total_vector_memory_bytes`
- `core_hnsw_*`
- `faiss_ntotal`
- `hnswlib_ntotal`
- `hnswlib_capacity`
- `vsearch_total_*`
- `vsearch_backend_search_*`
- `vsearch_manager_resolve_*`
- `vsearch_response_assembly_*`
- `vsearch_raw_hit_count`
- `vsearch_resolved_hit_count`
- `vsearch_filtered_hit_count`

缺口是没有统一 `backend_*` 口径，FAISS/hnswlib 的 memory 字段不能直接用于跨 backend 内存比较，hnswlib capacity runtime path 仍可能输出 0。

## 5. 新增/修正的 backend stats 字段

VINFO 固定新增：

- `backend_name`
- `backend_size`
- `backend_capacity`
- `backend_dim`
- `backend_metric`
- `backend_memory_bytes`
- `backend_memory_bytes_source`
- `backend_deleted_count`
- `backend_needs_rebuild`
- `backend_resize_count`
- `backend_reserve_count`
- `backend_resize_count_source`
- `backend_reserve_count_source`

并补齐 core 别名字段：

- `core_flat_size`
- `core_flat_capacity`
- `core_flat_memory_bytes`
- `core_hnsw_memory_bytes`

## 6. 每个 backend 的 stats 覆盖情况

| backend | size | capacity | dim/metric | memory_bytes | deleted_count | hnsw params |
|---|---:|---:|---|---|---:|---|
| `core_flat` | measured | measured | measured | measured | measured | n/a |
| `core_hnsw` | measured | measured | measured | measured | measured | measured |
| `hnswlib_hnsw` | measured via `ntotal` | measured via `max_elements` | config/runtime | estimated | 0 | measured/config |
| `faiss_flat` | measured via `ntotal` | `ntotal` | config/runtime | estimated | 0 | n/a |
| `faiss_hnsw` | measured via `ntotal` | `ntotal` | config/runtime | estimated | 0 | config |

## 7. memory_bytes 口径

- `measured`: 来自 KVLite core info 的内部 `memory_bytes`，用于 `core_flat` / `core_hnsw`。
- `estimated`: 基于公开 runtime stats 的保守估算，用于 hnswlib/FAISS/native legacy paths。
- `unsupported`: 当前 backend 没有足够信息时使用，不把 0 当成可信内存值。

FAISS Stage 5C 不再让 benchmark 把 `index_memory_bytes=0` 当作 FAISS 可比较内存。benchmark 使用 `backend_memory_bytes` 和 `backend_memory_bytes_source`，并在 source 为 `estimated` 或 `unsupported` 时保留口径标记。

## 8. resize_count / reserve_count 口径

未实现真实计数，VINFO 输出：

- `backend_resize_count:n/a`
- `backend_reserve_count:n/a`
- `backend_resize_count_source:unsupported`
- `backend_reserve_count_source:unsupported`

原因：现有 reserve/resize 路径没有统一计数器；低风险接入需要触碰多个 core/native/external reserve 点，容易混入 P1/P2 范围。本阶段先明确 unsupported，避免输出误导性 0。

## 9. benchmark 输出变化

`scripts/bench_vector_core_compare.py` 更新为 Stage 5C 输出：

- endpoint VSEARCH latency
- Stage 5B internal profile timing
- `backend_size`
- `backend_capacity`
- `backend_memory_bytes`
- `backend_memory_bytes_source`
- `backend_resize_count/backend_reserve_count`

输出前缀从 `stage5b_vsearch_profile_benchmark` 改为 `stage5c_backend_stats_benchmark`。

## 10. build 结果

已执行：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：通过。

## 11. tests 结果

已执行并通过：

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

新增测试：

```bash
tests/vector_vinfo_backend_stats_test.sh
```

覆盖：

- `core_flat`
- `core_hnsw`
- `hnswlib_hnsw`
- `faiss_flat`
- `faiss_hnsw`

断言：

- VINFO 包含统一 `backend_*` 字段。
- core backend memory_bytes > 0 且 source 为 `measured`。
- external backend memory source 不再是误导性的可信 0。
- Stage 5B profiling 字段仍存在并增长。
- VSEARCH response 不暴露 `record_id` / `slot`。

## 12. benchmark 结果

已执行：

```bash
N=1000 DIM=64 TOPK=10 QUERIES=50 tests/vector_core_benchmark_compare.sh
```

输出：

- `results/vector_core_benchmark/stage5c_backend_stats_benchmark_1k_64d.json`
- `results/vector_core_benchmark/stage5c_backend_stats_benchmark_1k_64d.md`

摘要：

| backend | endpoint avg ms | backend avg us | resolve avg us | response avg us | recall@10 | backend size | backend capacity | backend memory bytes | memory source |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `core_flat` | 0.106 | 48 | 1 | 7 | 1.0000 | 1000 | 1024 | 304128 | measured |
| `core_hnsw` | 0.151 | 77 | 2 | 9 | 1.0000 | 1000 | 1024 | 2627792 | measured |
| `hnswlib_hnsw` | 0.449 | 353 | 2 | 10 | 1.0000 | 1000 | 1128 | 447816 | estimated |
| `faiss_flat` | 1.085 | 1006 | 2 | 7 | 1.0000 | 1000 | 1000 | 264000 | estimated |
| `faiss_hnsw` | 0.124 | 57 | 2 | 8 | 0.9900 | 1000 | 1000 | 392000 | estimated |

## 13. recall@10 是否回退

未发现 Stage 5C 引入的 recall 回退。1K benchmark 中：

- `core_flat`: 1.0000
- `core_hnsw`: 1.0000
- `hnswlib_hnsw`: 1.0000
- `faiss_flat`: 1.0000
- `faiss_hnsw`: 0.9900

`faiss_hnsw` 的 0.9900 与 ANN 近似搜索特性一致，本阶段未修改搜索算法。

## 14. 静态边界检查结果

已执行：

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
grep -RIn "record_id_map" src/vector/core include/vector 2>/dev/null
git diff -- src/persistence
```

结果：

- `src/vector/core` 未新增 `g_vector_state` 访问；命中仅为既有 manager/header 注释或声明。
- `src/vector/core` 未出现 scope/ttl/metadata/question/answer 逻辑。
- `kvs_vector_index_ops_t` 仅有既有声明/使用；签名未修改。
- `record_id_map` 仅在 manager record header 中；未进入 `src/vector/core`。
- `src/persistence` 无 diff。

## 15. git status --short

当前包含 Stage 5A/5B 未提交改动以及 Stage 5C 改动：

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
?? docs/vector_core_reset/优化步骤.md
?? tests/vector_manager_resolve_fastpath_test.sh
?? tests/vector_vinfo_backend_stats_test.sh
?? tests/vector_vsearch_profile_breakdown_test.sh
```

## 16. 下一步建议

Next Stage:
  Stage 5D: VINFO / benchmark stats correctness hardening

Reason:
  1. Stage 5C 已补齐 P0 backend observability，但 resize/reserve 仍是 unsupported。
  2. 在进入 P1 core_hnsw memory/rebuild 前，最好先用 benchmark 多规模样本确认 stats 口径稳定。
  3. 继续保持 observability-only，风险低于直接改 core_hnsw graph/memory/rebuild。

Allowed files:
  `include/kvs_vector.h`, `src/vector/kvs_vector.c`, `src/core/kvs_operation.c`, `scripts/bench_vector_core_compare.py`, `tests/vector_vinfo_backend_stats_test.sh`, `docs/vector_core_reset/stage5d_*.md`

Forbidden:
  `src/vector/core/*` algorithm changes, RDB/AOF format, `kvs_vector_index_ops_t` signature, VSEARCH response format, backend replacement.

Acceptance:
  VINFO 数组计数稳定；benchmark JSON/Markdown 对 memory source、capacity、size、profile timing 均可解析；核心回归、Stage 5A/5B/5C 测试通过。
