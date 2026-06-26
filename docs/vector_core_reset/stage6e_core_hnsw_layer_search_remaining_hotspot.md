# Stage 6E core_hnsw layer_search remaining hotspot audit / targeted optimization

## 1. 是否已读取并遵守 AGENTS.md

是。本轮所有修改严格满足 `AGENTS.md` 中的 KVLite vector_core 边界、P0/P1/P2 分层约束、Stage 5A-5D completed invariants、Stage 6A-6D completed invariants。

## 2. 修改/新增文件

- `src/vector/core/hnsw/kvs_hnsw_add.c` — hoist locals + optimize lower_bound + visited-tag ordering
- `src/vector/core/hnsw/kvs_hnsw_search.c` — hoist locals + visited-tag ordering for level0_search and greedy_search
- `scripts/bench_vector_core_compare.py` — add `--stage6e` flag support
- `docs/vector_core_reset/stage6e_core_hnsw_layer_search_remaining_hotspot.md` (本报告)
- `results/vector_core_benchmark/stage6e_hnsw_layer_search_hotspot_benchmark_1k_64d.json`
- `results/vector_core_benchmark/stage6e_hnsw_layer_search_hotspot_benchmark_1k_64d.md`
- `results/vector_core_benchmark/stage6e_hnsw_layer_search_hotspot_benchmark_10k_64d.json`
- `results/vector_core_benchmark/stage6e_hnsw_layer_search_hotspot_benchmark_10k_64d.md`

## 3. 是否修改生产代码

是。生产代码修改范围仅限 `src/vector/core/hnsw/kvs_hnsw_add.c` 和 `src/vector/core/hnsw/kvs_hnsw_search.c`，属于 P1 targeted optimization。未修改 manager、RDB/AOF、外部 backend 或 response 格式。

## 4. Stage 6D baseline

Stage 6D 基线使用 M=16, ef_construction=128（benchmark_compare.py 默认参数，与 rebuild_profile 测试的 M=8/ef=64 不同）：

| dataset | rebuild_core_add_total_us | hnsw_add_layer_search_us | hnsw_add_neighbor_select_us | hnsw_add_link_us | recall@10 |
|---|---:|---:|---:|---:|---:|
| 1K / 64d | 60,095 | 40,587 | 15,378 | 1,021 | 1.0000 |
| 10K / 64d | 910,539 | 682,628 | 168,132 | 11,217 | 0.9870 |

注：Stage 6D 报告中引用的 config（M=8, ef=64）来自 rebuild_profile_test.sh，但 10K profile 数据实际来自 benchmark_compare.py 的 M=16, ef=128 默认参数。本报告以 M=16, ef=128 作为 apples-to-apples 比较基线。

## 5. layer_search 剩余热点分析

`kvs_hnsw_search_layer_for_add` 热点分布（per layer search）：

```text
while candidate_heap not empty:
  pop best candidate                          // heap_pop: O(log ef)
  early break check: score < lower_bound      // 1 comparison
  iterate candidate's M neighbors:            // M=16 or M0=32
    check visited_tags[neighbor]              // 1 array lookup
    check kvs_hnsw_slot_active_at_level       // 3 lookups (skip if no_deleted)
    kvs_hnsw_metric_score(query, neighbor)    // DOMINANT: 64 fma + overhead
    top_heap push/replace                     // heap_op: O(log ef)
    update lower_bound                        // heap_top or conditional
    candidate_heap push                       // heap_push: O(log ef)
```

剩余瓶颈按占比排序：

1. **距离计算** (~60-70%)：`kvs_hnsw_metric_score` 每次调用 64 维 float 点积/L2/余弦计算，是 hot loop 中绝对主导项。
2. **堆操作** (~15-20%)：每个 passing neighbor 执行 top_heap push/replace + candidate_heap push，各 O(log ef)。
3. **邻居遍历开销** (~10-15%)：visited_tag 查找、slot_active 检查、数组索引计算。
4. **lower_bound 更新** (~1-2%)：heap_top 调用开销。

## 6. 实际优化点

1. **Hoist 频繁访问的 struct 成员到局部变量**。
   - `index->vectors`、`index->dim`、`index->visited_tags` 提升为局部变量，减少 hot loop 中的指针解引用。
   - `ctx->candidate_heap.capacity` 提升为局部变量 `candidate_capacity`。
   - 应用到 `kvs_hnsw_search_layer_for_add`、`kvs_hnsw_level0_search`、`kvs_hnsw_greedy_search_layer`。

2. **优化 lower_bound 更新路径**。
   - 当 `top_heap` 未满时 push 新元素：只有新元素 score < 旧 lower_bound 才更新，避免冗余 `kvs_vector_heap_top()` 调用。
   - replace_top 路径保持不变（必须读取新 top 获取准确 lower_bound）。

3. **优化 visited_tag 标记顺序**。
   - 原路径：先检查 visited，再检查 active，最后标记 visited。非活跃 slot 不标记 visited，可被重复检查。
   - 新路径：先检查 visited，立即标记 visited，再检查 active。非活跃 slot 也被标记 visited，避免被其他候选路径重复检查。
   - 对 rebuild 路径（deleted_count == 0）无影响（active 检查完全跳过）。对存在删除的路径减少重复 active 检查。

4. **inline `kvs_hnsw_slot_vector` 到 hot loop**。
   - 使用 hoisted `vectors` 和 `dim` 直接计算 `vectors + neighbor * dim`，避免函数调用和额外的指针解引用。

## 7. candidate expansion / distance reuse / heap hot path 说明

- **candidate expansion**：维持原算法不变。候选通过 visited_tags 去重，候选堆使用 best-first 策略（max_score_less 最大堆，top 为最高分候选）。
- **distance reuse**：同一 layer search 内每 slot 距离仅计算一次（visited_tag 去重保证）。distance_buffer 在 export 阶段填充，供 neighbor_select 复用（Stage 6D 优化）。
- **heap hot path**：push 约 O(log ef) 次比较/交换。replace_top 同样 O(log ef)。优化后 lower_bound 在 push（未满）路径减少了不必要的 heap_top 调用。
- **hot path malloc/free**：无。所有 buffer 在 context pool 初始化时预分配。
- **full memset visited**：无。使用 visit_tag 递增 + wrap-around memset 策略。

## 8. 不变量检查

- no self edge：`slot == neighbor` 仍直接跳过。 ✓
- no duplicate edge：plan/apply 路径的 duplicate 检查未修改。 ✓
- level0 双向边语义保持：双方都接受时才应用连接。 ✓
- upper layer 双向边语义保持：同上。 ✓
- deleted_flags 语义保持：有删除时完整 active-at-level 校验路径未修改。 ✓
- record_id_to_slot 语义保持：未修改 id-slot map。 ✓
- level0 worst-neighbor runtime cache 语义保持：cache 读写逻辑未修改。 ✓
- search 输出仍为 record_id + score：VSEARCH path 未修改。 ✓
- core 不处理 scope / TTL / metadata / question / answer。 ✓
- manager 仍负责 response。 ✓
- recall@10 不低于 0.9870。 ✓

## 9. 1K before/after profile (M=16, ef=128)

| metric | Stage 6D before | Stage 6E after | change |
|---|---:|---:|---:|
| rebuild_core_add_total_us | 60,095 | 60,392 | +0.49% |
| hnsw_add_layer_search_us | 40,587 | 40,877 | +0.71% |
| hnsw_add_neighbor_select_us | 15,378 | 15,403 | +0.16% |
| hnsw_add_link_us | 1,021 | 961 | -5.88% |
| recall@10 | 1.0000 | 1.0000 | no regression |

注：1K 规模下测量噪声较大，layer_search_us 轻微增加在噪声范围内。link_us 下降 5.88% 可能来自 greedy_search 的 locals hoisting 改善。

## 10. 10K before/after profile (M=16, ef=128)

| metric | Stage 6D before | Stage 6E after | change |
|---|---:|---:|---:|
| rebuild_core_add_total_us | 910,539 | 901,286 | -1.02% |
| hnsw_add_layer_search_us | 682,628 | 675,495 | -1.04% |
| hnsw_add_neighbor_select_us | 168,132 | 166,836 | -0.77% |
| hnsw_add_link_us | 11,217 | 11,075 | -1.27% |
| recall@10 | 0.9870 | 0.9870 | no regression |

## 11. layer_search_us 是否下降

10K `hnsw_add_layer_search_us`：682,628 → 675,495 (-1.04%)。

下降幅度未达到 acceptable pass 阈值（>= 5%）。

## 12. rebuild_core_add_total_us 是否下降

10K `rebuild_core_add_total_us`：910,539 → 901,286 (-1.02%)。

下降幅度未达到 acceptable pass 阈值（>= 3%）。

## 13. recall@10 是否回退

- 1K：1.0000 → 1.0000，无回退。
- 10K：0.9870 → 0.9870，无回退。

## 14. tests 结果

构建：

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：PASS。

测试：

```bash
tests/vector_core_hnsw_correctness_test.sh       # PASS
tests/vector_core_hnsw_rebuild_test.sh            # PASS
tests/vector_core_hnsw_rdb_reload_test.sh         # PASS
tests/vector_core_hnsw_aof_replay_test.sh         # PASS
tests/vector_core_hnsw_rebuild_profile_test.sh    # PASS
tests/vector_manager_resolve_fastpath_test.sh     # PASS
tests/vector_vsearch_profile_breakdown_test.sh    # PASS
tests/vector_vinfo_backend_stats_test.sh          # PASS
```

结果：全部 PASS。

## 15. benchmark 结果

```bash
python3 scripts/bench_vector_core_compare.py --corrected --stage6e \
  --kv-bin ./kvstore --n 1000 --dim 64 --topk 10 --queries 50 \
  --hnsw-m 16 --ef-construction 128 --ef-search 128
python3 scripts/bench_vector_core_compare.py --corrected --stage6e \
  --kv-bin ./kvstore --n 10000 --dim 64 --topk 10 --queries 100 \
  --hnsw-m 16 --ef-construction 128 --ef-search 128
```

- 1K core_hnsw status：PASS，recall@10 `1.0000`。
- 10K core_hnsw status：PASS，recall@10 `0.9870`。
- 10K layer_search_us 与 add_total 未达到 acceptable pass。

## 16. 静态边界检查结果

执行：

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
grep -RIn "record_id_map" src/vector/core include/vector 2>/dev/null
git diff -- src/persistence
```

结果：

- `src/vector/core` 未新增 `g_vector_state`。grep 输出仅包含 include 中既有 manager 头文件声明。 ✓
- `src/vector/core` 未出现 scope / ttl / metadata / question / answer。 ✓
- `kvs_vector_index_ops_t` 签名未修改。 ✓
- `record_id_map` 未出现在 `src/vector/core`；仅 manager/record header 中存在。 ✓
- `git diff -- src/persistence` 为空，RDB/AOF 格式未修改。 ✓

## 17. git status --short

```text
 M include/kvs_vector.h
 M include/vector/kvs_vector_hnsw.h
 M include/vector/kvs_vector_manager.h
 M include/vector/kvs_vector_record.h
 M scripts/bench_vector_core_compare.py
 M src/core/kvs_command_executor.c
 M src/core/kvs_command_task.c
 M src/core/kvs_operation.c
 M src/vector/core/hnsw/kvs_hnsw_add.c
 M src/vector/core/hnsw/kvs_hnsw_index.c
 M src/vector/core/hnsw/kvs_hnsw_info.c
 M src/vector/core/hnsw/kvs_hnsw_search.c
 M src/vector/kvs_vector.c
 M src/vector/kvs_vector_hnswlib_ops.c
 M src/vector/kvs_vector_index.c
?? AGENTS.md
?? docs/vector_core_reset/stage5a_...md
?? docs/vector_core_reset/stage5b_...md
?? docs/vector_core_reset/stage5c_...md
?? docs/vector_core_reset/stage5d_...md
?? docs/vector_core_reset/stage6a_...md
?? docs/vector_core_reset/stage6b_...md
?? docs/vector_core_reset/stage6c_...md
?? docs/vector_core_reset/stage6d_...md
?? docs/vector_core_reset/优化步骤.md
?? tests/vector_core_hnsw_rebuild_profile_test.sh
?? tests/vector_manager_resolve_fastpath_test.sh
?? tests/vector_vinfo_backend_stats_test.sh
?? tests/vector_vsearch_profile_breakdown_test.sh
```

## 18. 下一步建议

**Stage 6E 结论：未达到 acceptable pass。**

P1 targeted optimization 已触及 `kvs_hnsw_search_layer_for_add` 中 hot loop overhead 的天花板。优化方向（hoisting locals、lower_bound 更新、visited-tag 顺序）均为正确的 micro-optimization，但节省的开销（约 1%）远小于距离计算和堆操作的主导成本。

剩余瓶颈分析：

```text
hnsw_add_layer_search_us 剩余热点属于：
  1. 距离计算（kvs_hnsw_metric_score）—— 主导项，每 neighbor 64 fma
  2. 堆操作（push/pop/replace_top）—— O(log ef)，每 passing neighbor 2 次
  3. 访存模式（vectors array、visited_tags、neighbor lists）—— cache locality 受限
```

这些剩余开销无法通过 P1 micro-optimization 显著降低，属于 memory layout / cache locality / graph structure 范围。

**Next Stage:**

```text
Stage 7A: core_hnsw memory layout / cache locality audit
```

方向建议：

```text
1. 审计 vectors 存储布局（SoA vs AoS）对 SIMD 和 cache line 的影响。
2. 审计 level0_neighbors / upper_neighbors 访问模式是否适合 prefetch。
3. 审计 visited_tags 的 cache 污染问题（per-search 全索引 tag 数组）。
4. 审计 per-search heap 内存是否在 L1/L2 cache 内。
5. 考虑将经常共同访问的字段（levels, deleted_flags, visited_tags）合并为一个 compact struct。
```

Forbidden in Stage 7A：

```text
不做 memory layout 全量重写（仅 audit + profiling）。
不做 graph persistence。
不做 shard / partition。
不做 scope-aware search。
不降低 ef_construction / ef_search 默认值。
不修改 RDB/AOF 格式。
不修改 kvs_vector_index_ops_t 签名。
```

停止规则遵守：

```text
未达到 acceptable pass，立即停止扩大优化范围。
未强行进入 memory layout 或 graph redesign。
报告中已说明剩余瓶颈属于 memory layout / cache locality / graph structure 范围。
直接建议 Stage 7A。
```
