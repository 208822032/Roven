# Stage 7G: layer_search distance / SIMD / alignment optimization

## 1. AGENTS.md compliance

已读取并遵守项目根目录 `AGENTS.md`。

本阶段依据 `docs/vector_core_reset/stage7f_highdim_layer_search_audit.md`，只优化 high-dim `kvs_hnsw_search_layer_for_add()` 的 distance compute / SIMD / call overhead。未修改 RDB/AOF，未修改 `kvs_vector_index_ops_t`，未修改 VSEARCH response，未做 metadata compact，未做 visited_tags / levels / deleted_flags 合并，未做 upper_neighbors compact，未做 graph persistence / shard / scope-aware search，未降低 `ef_construction` / `ef_search`。

本 prompt 与 `AGENTS.md` 未发现冲突。

## 2. Modified / added files

Stage 7G 修改文件：

- `src/vector/core/hnsw/kvs_hnsw_add.c`
- `src/vector/kvs_vector_distance_avx2.c`

Stage 7G 新增文件：

- `docs/vector_core_reset/stage7g_layer_search_distance_alignment_optimization.md`

工作树中仍保留 Stage 7E / Stage 7F 未提交修改和无关 `README.md` 修改；本阶段未回退或处理这些既有改动。

## 3. Whether production code changed

是。

生产代码改动限定在：

- HNSW add-time layer_search 的距离函数选择。
- shared AVX2 distance kernel 的内部 SIMD 累加实现。

未改变 HNSW 候选接受规则、heap 比较规则、邻接表结构、搜索参数、删除过滤规则或 VSEARCH response。

## 4. Current call chain

```text
vcmd_profile_bench ann_core
  -> kvs_vector_restore_record_with_embedding()
  -> kvs_vector_index_rebuild_current()
  -> kvs_vector_core_hnsw_ops_rebuild()
  -> kvs_vector_manager_for_each_active_embedding()
  -> kvs_hnsw_index_add()
  -> kvs_hnsw_search_layer_for_add()
  -> distance score function
```

## 5. Before behavior

Stage 7F 数据显示：

- `hnsw_add_layer_search_us` 是 high-dim rebuild 剩余主耗时。
- `layer_search_level0_us` 占 `hnsw_add_layer_search_us` 的 99.8% 以上。
- 每轮约 500K 次 distance compute。
- 耗时随维度从 384d 到 1024d 明显增长。

代码检查结果：

- `layer_search` 已使用 shared distance backend，当前 benchmark 选择 `avx2`。
- 热循环内每次距离计算仍通过 `kvs_hnsw_metric_score()`，重复执行 `index->distance` 检查、metric switch、shared wrapper lazy-init 检查。
- AVX2 dot / L2 / cosine kernel 每 8 个 float 都 `_mm256_storeu_ps()` 到栈上，再逐元素标量累加。
- HNSW vector storage 未保证 32-byte 对齐；AVX2 kernel 使用 `_mm256_loadu_ps()`，因此本阶段不做 allocation / layout 修改。

## 6. After design

局部优化：

- 在 `kvs_hnsw_search_layer_for_add()` 入口用 `kvs_hnsw_layer_search_score_fn()` 选择一次 score 函数。
- 对 AVX2 backend 直接选择 `kvs_vector_distance_dot_avx2()` / `kvs_vector_distance_cosine_avx2()` / L2 wrapper，避免每次 distance compute 重复走 metric switch 和 shared wrapper。
- 对 fallback backend 保留原有 fallback score wrapper。
- 将 AVX2 dot / squared_l2 / cosine kernel 改为寄存器累加，循环结束后一次 horizontal sum，移除每 8-float 块的栈 store 和逐元素标量累加。

未做：

- 未做 vector allocation / storage alignment 修改。
- 未做 memory layout 重构。
- 未减少搜索量。
- 未改 Stage 7F profiling 字段或 benchmark schema；本轮 after 结果仍由既有 `stage7f_highdim_layer_search_*` 文件输出承载。

## 7. Safety invariants

- `layer_search_distance_compute_count` 未下降，优化不是靠减少搜索量获得。
- `ef_construction` / `ef_search` 未降低。
- HNSW graph 连接规则未改。
- core 仍只处理 record_id / vector / score / index-local graph/search state。
- 新增改动不进入 RDB/AOF。
- `kvs_vector_index_ops_t` 未改。
- VSEARCH response 未改。

## 8. Before / after benchmark

Before 使用 Stage 7F 报告数据；after 使用本阶段同一命令重新运行：

```bash
./build/tests/bin/vcmd_profile_bench ann_core 1000 384 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 512 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 768 10 50
./build/tests/bin/vcmd_profile_bench ann_core 1000 1024 10 50
```

| dim | hnsw_add_layer_search_us before | hnsw_add_layer_search_us after | change | layer_search_level0_us before | layer_search_level0_us after | change | distance_compute before | distance_compute after | recall@10 before | recall@10 after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 384 | 196,743 | 58,625 | -70.2% | 196,400 | 58,462 | -70.2% | 499,711 | 499,711 | 1.0000 | 1.0000 |
| 512 | 247,848 | 65,856 | -73.4% | 247,322 | 65,645 | -73.5% | 499,900 | 499,900 | 1.0000 | 1.0000 |
| 768 | 377,044 | 78,838 | -79.1% | 376,654 | 78,702 | -79.1% | 499,513 | 499,513 | 1.0000 | 1.0000 |
| 1024 | 465,741 | 93,164 | -80.0% | 465,008 | 92,955 | -80.0% | 499,762 | 499,762 | 1.0000 | 1.0000 |

Supporting after metrics:

| dim | rebuild_core_add_total_us | layer_search_upper_us | neighbor_select_us | hnsw_add_link_us | distance_backend |
| --- | ---: | ---: | ---: | ---: | --- |
| 384 | 70,759 | 101 | 4,152 | 2,853 | avx2 |
| 512 | 78,848 | 157 | 4,289 | 2,929 | avx2 |
| 768 | 92,038 | 86 | 4,138 | 2,881 | avx2 |
| 1024 | 108,234 | 163 | 4,429 | 2,885 | avx2 |

## 9. recall@10

未回退。

| dim | recall@10 |
| --- | ---: |
| 384 | 1.0000 |
| 512 | 1.0000 |
| 768 | 1.0000 |
| 1024 | 1.0000 |

## 10. Correctness tests

Passed:

```bash
tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh
tests/vector_core_hnsw_rebuild_profile_test.sh
```

## 11. Static boundary check result

Commands:

```bash
grep -RIn "g_vector_state" src/vector/core || true
grep -RIn "record_id_map" src/vector/core || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core || true
```

Result: no output.

`git diff --check`: passed.

## 12. Build result

Passed:

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

## 13. git status --short

Status after implementation and validation:

```text
 M README.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_index.c
 M src/vector/core/hnsw/kvs_hnsw_info.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_distance_avx2.c
 M tests/vcmd_profile_bench.c
 M tests/vector_core_hnsw_rebuild_profile_test.sh
?? docs/vector_core_reset/stage7e_neighbor_select_score_reuse_optimization.md
?? docs/vector_core_reset/stage7f_highdim_layer_search_audit.md
?? docs/vector_core_reset/stage7g_layer_search_distance_alignment_optimization.md
```

## 14. Next suggested step

Based only on Stage 7G data, distance/SIMD/call-overhead optimization was effective and did not reduce recall. Remaining `layer_search_level0_us` is now 58.5ms to 93.0ms for 384d to 1024d at this workload, while Stage 7F counters still show millions of level0 neighbor scans and visited skips.

The next step, if more rebuild optimization is needed, should be a profiling / layout audit of level0 neighbor access and visited_tags behavior. Do not directly start a layout refactor without that audit.
