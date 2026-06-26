# Stage 5A manager resolve fastpath report

## 1. 阶段目标

Stage 5A 的目标是把 `kvs_vector_manager_resolve_hits()` 从按 backend hit 扫描 record store，改为 manager-owned `record_id -> record_index -> record` fast resolve。

本阶段只处理 manager resolve 共同瓶颈，不处理 core_hnsw memory / rebuild，不修改 RDB/AOF 格式，不修改 `kvs_vector_index_ops_t` 签名，不改变响应格式。

## 2. 修改 / 新增文件

生产代码：

- `include/vector/kvs_vector_manager.h`
- `include/vector/kvs_vector_record.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_index.c`

测试：

- `tests/vector_manager_resolve_fastpath_test.sh`

报告：

- `docs/vector_core_reset/stage5a_manager_resolve_fastpath_report.md`

未修改：

- `src/vector/core/*`
- `src/persistence/*`
- `include/vector/kvs_vector_index_internal.h` 中的 `kvs_vector_index_ops_t`

## 3. 是否修改生产代码

是。

修改范围限定在 manager / record store 层：

- 新增 manager-owned `record_id -> record_index` 内存 map。
- 在 VSET / VDEL / restore / rebuild / compact 路径维护该 map。
- `kvs_vector_manager_resolve_hits()` 使用 `kvs_vector_manager_get_record_by_id()` 直接回表。

## 4. 当前 resolve 调用链

核心路径：

```text
VSEARCH
  -> kvs_vector_search() / kvs_vector_search_with_prepared()
  -> kvs_vector_index_search_current()
  -> current backend ops->search()
  -> backend search returns kvs_vector_hit_t { record_id, score }
  -> kvs_vector_manager_resolve_hits()
  -> kvs_vector_manager_get_record_by_id(record_id, &record)
  -> kvs_vector_search_result_from_record()
```

已验证的 core backend：

- `core_flat`: backend 返回 `record_id + score` 后进入 manager resolve。
- `core_hnsw`: backend 返回 `record_id + score` 后进入 manager resolve。

External backend 仍通过 adapter 进入同一个 manager resolve：

- `hnswlib_hnsw`
- `faiss_flat`
- `faiss_hnsw`

## 5. 修改前复杂度

Stage 50 审计确认旧路径在 `kvs_vector_manager_resolve_hits()` 中对每个 hit 扫描 record store：

```text
O(raw_topk * record_store_capacity)
```

overfetch 修复后 raw hits 增多，因此该成本会同时影响所有走 manager resolve 的 backend。

## 6. 修改后 fast path 设计

新增 `kvs_vector_record_id_map_t`：

```text
record_id -> record_index
```

`kvs_vector_manager_resolve_hits()` 现在只做：

```text
for each backend hit:
  1. threshold 过滤
  2. kvs_vector_manager_get_record_by_id(hit.record_id, &record)
  3. scope 过滤
  4. 按 backend hit 顺序填充 question / answer / scope / score
```

复杂度变为：

```text
O(raw_topk * average_hash_probe)
```

通常等价于：

```text
O(raw_topk)
```

仍保留 manager 层语义：

- active / deleted 校验
- scope 过滤
- threshold 过滤
- topK 输出容量控制
- response formatting

## 7. record_id -> slot/record 映射来源

`record_id_map` 是 `kvs_vector_store_t` 的 manager-owned 内存态字段，不进入 core。

维护路径：

- `kvs_vector_store_init()` 初始化 map。
- `kvs_vector_store_destroy()` 销毁 map。
- `kvs_vector_store_reserve()` 随 store 扩容预留 map 容量。
- `kvs_vector_set()` 新增 active record 后写入 map。
- `kvs_vector_set_commit_prepared()` prepared VSET commit 后写入 map。
- `kvs_vector_restore_record_with_embedding()` RDB/AOF restore active record 后写入 map。
- `kvs_vector_del()` 删除 active record 时移除 map entry。
- `kvs_vector_store_compact()` compact 后重建 map。
- `kvs_vector_id_map_rebuild_from_store()` 从 active records 同步重建 key/scope map 和 record_id map。

不保存额外 map 到 RDB/AOF。

## 8. slot 复用安全

`kvs_vector_manager_get_record_by_id()` 做完整防护：

```text
1. record_id 必须能在 record_id_map 中找到 record_index。
2. record_index 必须小于 store.capacity。
3. record 必须存在。
4. record->id 必须等于 hit.record_id。
5. record->state 必须是 ACTIVE。
6. record->deleted 必须为 false。
```

因此即使旧 backend hit 返回 stale `record_id`：

- 如果旧 id 已从 map 删除，resolve 返回 not found。
- 如果 slot 被新 record 复用，`record->id == hit.record_id` 校验会阻止误命中新 record。
- inactive / deleted record 会被跳过。

## 9. VDEL / update / rebuild / RDB / AOF 安全性

VDEL：

- `kvs_vector_del()` 在移除 key/scope id_map 后移除 `record_id_map`。
- repeated delete 不应重复增加 deleted count，也不会恢复旧 map entry。

Update same key/scope：

- 当前语义仍是覆盖 answer，不新建 record，不改变 record_id。
- `record_id_map` 不需要更新。
- 测试验证 update 后搜索无重复结果。

VINDEX REBUILD：

- `kvs_vector_store_compact()` 会按 active records 重建 `record_id_map`。
- `kvs_vector_id_map_rebuild_from_store()` 也会从 active records 重建两个 manager map。
- core/backend index 仍是 derived state。

RDB reload：

- RDB 只保存 records。
- load 后 `kvs_vector_restore_record_with_embedding()` 和 rebuild 路径恢复 `record_id_map`。

AOF replay：

- AOF replay 执行 VSET / VDEL 语义，不保存 map。
- replay 后 map 由 manager 写路径维护。

## 10. 优化项明细

目标：

- 将 manager resolve 从 `O(raw_topk * record_store_capacity)` 降为 `O(raw_topk)` 平均复杂度。

证据：

- `kvs_vector_manager_resolve_hits()` 不再包含 record store 全量扫描。
- per-hit resolve 改为 `kvs_vector_manager_get_record_by_id(hit->record_id, &record)`。
- helper 显式检查 `record->id == record_id`。

影响范围：

- 所有返回 `record_id + score` 并进入 manager resolve 的 backend。
- 覆盖 `core_flat / core_hnsw / hnswlib_hnsw / faiss_flat / faiss_hnsw`。

风险等级：

- P0，低风险共性优化。

允许修改文件：

- `include/vector/kvs_vector_manager.h`
- `include/vector/kvs_vector_record.h`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_index.c`
- `tests/vector_manager_resolve_fastpath_test.sh`
- `docs/vector_core_reset/stage5a_manager_resolve_fastpath_report.md`

禁止修改内容：

- `src/vector/core/*`
- RDB/AOF 格式
- `kvs_vector_index_ops_t` 签名
- response 格式
- core_hnsw graph / memory / rebuild
- backend 替换

验收测试：

- clean build
- core flat correctness / rebuild / RDB reload
- core hnsw correctness / rebuild / RDB reload / AOF replay
- manager resolve fastpath test

benchmark 指标：

- 轻量 corrected benchmark `N=1000 DIM=64 TOPK=10 QUERIES=50` 通过。
- recall@10 未观察到 Stage 5A 引入的回退。

是否推荐立即执行：

- 已执行完成。

## 11. Build 结果

```bash
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：

```text
PASS
```

## 12. Tests 结果

全部 PASS：

```bash
tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh

tests/vector_core_hnsw_correctness_test.sh
tests/vector_core_hnsw_rebuild_test.sh
tests/vector_core_hnsw_rdb_reload_test.sh
tests/vector_core_hnsw_aof_replay_test.sh

tests/vector_manager_resolve_fastpath_test.sh
```

`tests/vector_manager_resolve_fastpath_test.sh` 覆盖：

- VSET 多条记录
- VSEARCH topK
- scopeA / scopeB 过滤
- threshold 过滤
- VDEL 后旧 record 不可见
- repeated delete
- update same key/scope
- update 后无重复
- VINDEX REBUILD
- RDB reload
- AOF replay
- slot reuse / stale record_id public API 近似覆盖
- response 不暴露 `record_id` / `slot`
- backend: `core_flat / core_hnsw / hnswlib_hnsw / faiss_flat / faiss_hnsw`

## 13. Benchmark / recall

本阶段没有保留 fresh before 二进制，因此不声称有严格 before/after wall-time 对照。

Stage 4B/Stage 50 的结论作为 before 背景：

- corrected benchmark 的 recall 口径可信。
- manager resolve 是所有 backend 的共同瓶颈候选。

Stage 5A 后轻量 corrected benchmark：

```bash
N=1000 DIM=64 TOPK=10 QUERIES=50 tests/vector_core_benchmark_compare.sh
```

结果文件：

```text
results/vector_core_benchmark/stage4b_corrected_benchmark_1k_64d.md
```

摘要：

| backend | status | VSEARCH avg ms | recall@10 |
|---|---:|---:|---:|
| core_flat | pass | 0.139 | 1.0000 |
| core_hnsw | pass | 0.150 | 1.0000 |
| hnswlib_hnsw | pass | 0.435 | 1.0000 |
| faiss_flat | pass | 0.858 | 1.0000 |
| faiss_hnsw | pass | 0.142 | 0.9900 |

结论：

- recall@10 未回退。
- 小规模 1K benchmark 主要用于 correctness/recall smoke，不足以量化 manager resolve 优化收益。
- 如需量化收益，下一阶段应补 VSEARCH profiling：backend search / manager resolve / response assembly 拆分。

## 14. 静态边界检查

命令：

```bash
grep -RIn "g_vector_state" src/vector/core include/vector 2>/dev/null
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core 2>/dev/null
grep -RIn "kvs_vector_index_ops_t" include src/vector 2>/dev/null
grep -RIn "for .*store.capacity\|record_store_capacity\|capacity" src/vector/kvs_vector.c 2>/dev/null
```

结果摘要：

- `src/vector/core` 未出现 `g_vector_state` 使用。
- `src/vector/core` 未出现 `scope / ttl / metadata / question / answer` 命中。
- `kvs_vector_index_ops_t` 只显示既有定义和 ops 实例；签名未修改。
- `kvs_vector_manager_resolve_hits()` 不再扫描 `store.capacity`，只遍历 backend hits。
- `kvs_vector.c` 仍有 `store.capacity` 命中，属于 init/reserve/rebuild/compact/memory estimate/active embedding iterator 等非 resolve 热路径。

## 15. 边界结论

manager / core 边界保持稳定：

- core 仍只处理 `record_id / vector / score / index-local state`。
- manager 仍负责 scope / TTL / metadata / question / answer / active-deleted filtering / response。
- `record_id_map` 属于 manager-owned record store 内存态。
- response 未暴露 record_id / slot。
- RDB/AOF 格式未修改。

## 16. git status 摘要

Stage 5A 相关修改：

```text
M include/vector/kvs_vector_manager.h
M include/vector/kvs_vector_record.h
M src/vector/kvs_vector.c
M src/vector/kvs_vector_index.c
?? tests/vector_manager_resolve_fastpath_test.sh
?? docs/vector_core_reset/stage5a_manager_resolve_fastpath_report.md
```

工作区存在未处理的 unrelated untracked 文件，本阶段未修改：

```text
?? AGENTS.md
?? docs/vector_core_reset/优化步骤.md
```

## 17. 下一步建议

Next Stage:

```text
Stage 5B: VSEARCH profiling 拆分 backend search / manager resolve / response assembly
```

Reason:

```text
1. Stage 5A 已消除最明显的 manager resolve O(raw_topk * capacity) 扫描。
2. 当前 benchmark 仍主要是端到端 VSEARCH，不能精确量化 resolve 优化收益。
3. profiling 拆分是低风险 P0 工作，不改变外部语义，也能为 P1 core_hnsw memory/rebuild 优化提供证据。
```

Allowed files:

```text
include/kvs_vcmd_profile.h
src/core/kvs_vcmd_profile.c
src/vector/kvs_vector.c
src/vector/kvs_vector_index.c
tests/vector_manager_resolve_fastpath_test.sh 或新增 profiling test
docs/vector_core_reset/stage5b_vsearch_profile_breakdown_report.md
```

Forbidden:

```text
src/vector/core/*
RDB/AOF 格式
kvs_vector_index_ops_t 签名
response 格式
core_hnsw graph / memory / rebuild
backend 替换
unrelated docs deletion / unrelated git diff
```

Acceptance:

```text
1. VSEARCH profile 能拆出 backend search、manager resolve、response assembly。
2. 统计字段不改变 VSEARCH 响应。
3. core_flat/core_hnsw/hnswlib/faiss 路径均能记录拆分。
4. 原有 Stage 5A 测试和 core regression 全部 PASS。
5. benchmark/report 明确 endpoint metrics 与 internal profile metrics 的口径差异。
```
