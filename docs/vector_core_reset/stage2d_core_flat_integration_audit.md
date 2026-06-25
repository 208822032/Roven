# Stage 2D-0 core_flat integration audit

## 1. Scope

本轮只审计 `core_flat` 接入前的调用链和最小修改点，不接入 `VINDEX`，不修改 manager 搜索路径，不修改旧 flat / ANN / SQ8 / Faiss / hnswlib 行为。

审计目标是回答一个问题：

```text
怎样用最小改动让 VINDEX / vector_index_type 可以选择 core_flat，
同时保持现有 VSET / VSEARCH / VDEL / VINFO 语义不变。
```

结论：

- `core_flat` 不应直接替换旧 `flat_index` baseline。
- 推荐新增一个旧 `kvs_vector_index_ops_t` adapter，把 `kvs_vector_core_ops_t` 的 `record_id + vector -> record_id + score` 结果转换回当前 manager 需要的 `kvs_vector_search_result_t`。
- `core_flat` 初次接入应作为新的 backend 类型，命名为 `core_flat`，不要复用旧 `flat`。

## 2. Current Call Chains

### 2.1 V command entry

命令注册在 `src/core/kvs_operation.c`：

```text
VSET    -> handle_vset_command
VGET    -> handle_vget_command
VSEARCH -> handle_vsearch_command
VDEL    -> handle_vdel_command
VINDEX  -> handle_vindex_command
VINFO   -> handle_vinfo_command
```

参数解析同样在 `src/core/kvs_operation.c`：

```text
VSET question answer [scope]
VGET question [scope]
VSEARCH query [scope] [topk] [threshold]
VDEL question [scope]
VINDEX REBUILD
VINFO
```

当前 `VINDEX` 只接受 `REBUILD`，没有运行时切换 index 类型的子命令。

### 2.2 VSET current path

同步路径：

```text
handle_vset_command
  -> kvs_vector_set_prepare
  -> kvs_vector_set_commit_prepared
```

核心提交逻辑在 `src/vector/kvs_vector.c`：

```text
kvs_vector_set_commit_prepared
  -> kvs_vector_find_record
  -> kvs_vector_record_take_prepared_fields
  -> kvs_vector_id_map_put
  -> kvs_vector_flat_index_add      // index_type != SQ8 时保持旧 flat baseline
  -> kvs_vector_index_add_current   // index_type != FLAT 时写当前 backend
```

现状要点：

- 旧 `flat_index` 既是 `flat` backend，又被 HNSW / hnswlib / Faiss 等路径作为 debug / recall baseline 写入。
- 当 `index_type == KVS_VECTOR_INDEX_TYPE_FLAT` 时，`VSET` 不走 `kvs_vector_index_add_current()`，只写旧 `flat_index`。
- 非 flat backend 通过 `current_index.ops->add` 写入。

因此 `core_flat` 接入时不应使用现有 `KVS_VECTOR_INDEX_TYPE_FLAT` 分支，否则会落回旧 flat 路径。

### 2.3 VSEARCH current path

命令路径：

```text
handle_vsearch_command
  -> kvs_vector_search_prepare
  -> kvs_vector_search_with_prepared
  -> kvs_vector_index_search_current
  -> g_vector_state.current_index.ops->search
```

旧 `kvs_vector_index_ops_t.search` 当前直接返回 `kvs_vector_search_result_t`，也就是已经完成回表后的结果。

旧 flat / ANN 路径：

```text
kvs_vector_flat_ops_search
  -> kvs_vector_flat_index_search

kvs_vector_hnsw_ops_search
  -> kvs_vector_ann_index_search
```

Faiss / hnswlib 路径：

```text
backend search -> kvs_vector_hit_t(record_id, score)
  -> kvs_vector_manager_resolve_hits
  -> kvs_vector_search_result_t
```

`kvs_vector_manager_resolve_hits()` 当前负责：

- threshold 过滤。
- 通过 `record_id` 扫描 record store 回表。
- active / deleted 过滤。
- scope 过滤。
- 填充 question / answer / scope / score。

这正好可以被 `core_flat` adapter 复用。

### 2.4 VDEL current path

命令路径：

```text
handle_vdel_command
  -> kvs_vector_del
```

当前删除逻辑：

```text
kvs_vector_del
  -> kvs_vector_find_record
  -> kvs_vector_record_mark_deleted
  -> kvs_vector_id_map_remove
  -> kvs_vector_store_free_list_push
  -> store active/deleted counters update
  -> kvs_vector_flat_index_mark_stale
  -> g_vector_state.sq8_flat_index.needs_rebuild = 1
```

现状要点：

- 当前 `VDEL` 没有调用 `current_index.ops->mark_delete`，因为旧 `kvs_vector_index_ops_t` 没有 `mark_delete` 回调。
- 旧 flat 通过 `mark_stale` 等待 rebuild，不做即时逻辑删除。
- hnswlib / Faiss 也没有通过统一 ops 做删除，依赖 manager 回表过滤或 rebuild。

`core_flat` 已有 `mark_delete(record_id)`，但为了最小接入，Stage 2D-1 不建议立刻修改 `kvs_vector_index_ops_t` 签名。可以先继续依赖 manager 的 active/deleted 回表过滤，后续单独阶段再做 VDEL -> core mark_delete 接入。

### 2.5 VINDEX current path

命令路径：

```text
handle_vindex_command
  -> kvs_vector_index_rebuild
```

当前 `VINDEX` 只支持：

```text
VINDEX REBUILD
```

`kvs_vector_index_rebuild()` 当前会：

1. compact record store。
2. rebuild `id_map`。
3. 构建临时旧 flat index。
4. 按 `index_type` 构建 HNSW 或 SQ8。
5. swap 旧索引。
6. `kvs_vector_index_bind_current()` 重新绑定当前 backend。

对 hnswlib / Faiss：

- `kvs_vector_rebuild_after_load()` 已经会在 bind 后调用 `current_index.ops->rebuild`。
- `kvs_vector_index_rebuild()` 当前主要显式处理旧 flat / native HNSW / SQ8，未统一调用所有 backend ops rebuild。

`core_flat` 接入时，最小方案是在 `kvs_vector_index_rebuild()` 对 `KVS_VECTOR_INDEX_CORE_FLAT` 增加独立临时 core_flat 构建和 swap，或让 adapter 的 `rebuild` 使用 `kvs_vector_manager_for_each_active_embedding()` 重建当前 `core_flat`。

### 2.6 VINFO current path

命令路径：

```text
handle_vinfo_command
  -> kvs_vector_info
  -> handle_vector_info_response
```

`kvs_vector_info()`：

```text
kvs_vector_info_from_state
  -> current_index.ops->info
```

`handle_vector_info_response()` 输出固定数组，当前 backend 名称映射分散在命令层日志和 VINFO 字段中。

现状要点：

- `kvs_vector_info_t.index_type` 是 int，取值来自 `kvs_vector_index_type_t`。
- 当前输出可以表达 `flat / hnsw / sq8_flat / faiss_hnsw / faiss_flat / hnswlib_hnsw`。
- 接入 `core_flat` 需要新增 enum 值，并在显示名称映射中增加 `core_flat`。

## 3. Current Backend Dispatch

### 3.1 Config parsing

配置默认值在 `src/core/kvs_config.c`：

```text
vector_index_type = hnsw
vector_metric = ip
```

配置文件和命令行均解析 `vector_index_type`，当前允许值：

```text
flat
hnsw
hnswlib_hnsw
ivf_flat
sq8_flat
faiss_hnsw
faiss_flat
```

注意：

- 代码字段名是 `vector_index_type`，不是 `vector.index`。
- `ivf_flat` 被配置解析接受，但 `kvs_vector_index_type_from_config()` 没有真正返回对应 backend。

`kvs_vector_index_type_from_config()` 当前映射：

```text
hnsw / hnswlib_hnsw -> HNSWLIB_HNSW when ENABLE_HNSWLIB
sq8_flat            -> SQ8_FLAT
faiss_hnsw          -> FAISS_HNSW when ENABLE_FAISS_BASELINE
faiss_flat          -> FAISS_FLAT when ENABLE_FAISS_BASELINE
default             -> FLAT
```

### 3.2 current_index binding

`kvs_vector_index_bind_current()` 根据 `g_vector_state.index_type` 填充：

```text
g_vector_state.current_index.type
g_vector_state.current_index.metric
g_vector_state.current_index.dimension
g_vector_state.current_index.impl
g_vector_state.current_index.ops
```

当前分支：

```text
HNSW            -> old ann_index + g_kvs_vector_hnsw_ops
HNSWLIB_HNSW    -> hnswlib handle + hnswlib ops
SQ8_FLAT        -> sq8_flat_index + sq8 ops
FAISS_*         -> faiss handle + faiss ops
default         -> old flat_index + g_kvs_vector_flat_ops
```

`core_flat` 的最小接入点就在这里：新增一个明确分支，绑定到 `core_flat` 实例和 adapter ops。

## 4. Old Ops vs New Core Ops

### 4.1 Old `kvs_vector_index_ops_t`

旧 ops 面向 manager 结果：

```text
add(index, record_index, embedding, embedding_dim)
search(index, query, dim, params(scope/topk/threshold), results, results_capacity, results_count)
rebuild(index)
info(index, info)
destroy(index)
```

问题：

- `add` 输入是 `record_index`，不是稳定 `record_id`。
- `search` 输出是 `kvs_vector_search_result_t`，backend 可以直接参与回表和业务过滤。
- 没有 `mark_delete`。
- `scope` 和 `threshold` 在旧 search params 中传入 backend。

### 4.2 New `kvs_vector_core_ops_t`

新 core ops 面向纯计算结果：

```text
reserve(index, capacity)
add(index, record_id, vector, dim)
search(index, query, dim, topk, kvs_vector_hit_t out, out_count)
mark_delete(index, record_id)
reset(index)
info(index, kvs_vector_core_info_t)
destroy(index)
```

差异：

- 新 core 使用稳定 `record_id`。
- 新 core search 只返回 `record_id + score`。
- 新 core 不知道 scope / TTL / metadata / question / answer。
- 新 core 有 `mark_delete / reset / info`，旧 ops 没有完整对应字段。

### 4.3 Adapter necessity

需要 adapter 层。

原因：

- 直接把 VSEARCH 改成 `kvs_vector_core_ops_t` 会牵动 manager 搜索路径，超出 Stage 2D 最小接入范围。
- 当前 Faiss / hnswlib 已经证明 adapter 模式可行：backend 返回 id/score，再用 `kvs_vector_manager_resolve_hits()` 回表。
- adapter 可以把 `record_index` 转成 `record_id`，避免 `core_flat` 看到 manager record slot。

推荐 adapter 职责：

```text
old add(record_index, embedding)
  -> kvs_vector_manager_get_active_record_id_by_index(record_index)
  -> core_flat.add(record_id, embedding)

old search(query, params, results)
  -> core_flat.search(query, topk or overfetch, hits)
  -> kvs_vector_manager_resolve_hits(hits, scope, threshold, results)

old rebuild(index)
  -> core_flat.reset()
  -> core_flat.reserve(active_records)
  -> kvs_vector_manager_for_each_active_embedding(add record_id/vector)

old info(info)
  -> core_flat.info(core_info)
  -> copy useful fields into kvs_vector_info_t
```

## 5. Recommended Minimal Integration Plan

### 5.1 Naming boundary

Keep these names distinct:

```text
flat       = existing legacy flat index, kvs_vector_flat_index_t
core_flat  = new vector_core exact flat index, kvs_flat_index_t
```

Do not rename old flat types or functions.

Do not expose `core_flat` as `"flat"` during first integration, because old `flat` still has special baseline behavior in VSET and VINFO.

### 5.2 Add a new index type

Add one enum value:

```text
KVS_VECTOR_INDEX_CORE_FLAT
```

Recommended numeric value: append after current values to avoid changing existing persisted enum values.

Important:

- RDB currently stores `index_type`, but load path ignores persisted `index_type_from_rdb` and preserves init config. Still, do not renumber existing enum values.
- `include/kvs_vector.h` is public API, so use a clear name and avoid changing existing values.

### 5.3 Add manager state for core_flat

Add one field to `kvs_vector_state_t`:

```text
kvs_flat_index_t core_flat_index;
```

This requires including `vector/kvs_vector_flat.h` in the manager internal header or in `kvs_vector.c`.

The field belongs beside other derived indexes:

```text
flat_index
ann_index
sq8_flat_index
core_flat_index
```

### 5.4 Add old-ops adapter

Add static adapter ops in `src/vector/kvs_vector.c` or a small new C file under `src/vector/core/flat/` only if the Makefile already picks it up.

For minimal build impact, first implementation can live in `src/vector/kvs_vector.c` near existing ops wrappers:

```text
g_kvs_vector_core_flat_ops
  .add
  .search
  .rebuild
  .info
  .destroy
```

Adapter internals should call:

```text
kvs_flat_index_add
kvs_flat_index_search
kvs_flat_index_reset
kvs_flat_index_reserve
kvs_flat_index_info
kvs_flat_index_destroy
kvs_vector_manager_get_active_record_id_by_index
kvs_vector_manager_for_each_active_embedding
kvs_vector_manager_resolve_hits
```

Do not modify `kvs_vector_index_ops_t` in Stage 2D-1.

### 5.5 Bind current index

Update `kvs_vector_index_bind_current()`:

```text
if index_type == CORE_FLAT:
  dimension = core_flat_index.dim
  impl = &core_flat_index
  ops = &g_kvs_vector_core_flat_ops
```

This makes existing `kvs_vector_index_add_current()` and `kvs_vector_index_search_current()` work without changing command handlers.

### 5.6 Config

Allow:

```text
vector_index_type = core_flat
--vector_index_type core_flat
```

Update `kvs_vector_index_type_from_config()`:

```text
core_flat -> KVS_VECTOR_INDEX_CORE_FLAT
```

Do not change default from `hnsw`.

### 5.7 VSET behavior

Recommended Stage 2D-1 behavior:

```text
index_type == flat:
  old behavior unchanged
  VSET writes old flat only

index_type == core_flat:
  VSET writes old flat baseline
  VSET also writes current_index adapter, which writes core_flat by record_id

index_type == hnsw/hnswlib/faiss/sq8:
  unchanged
```

This requires making sure the existing branch:

```text
if (g_vector_state.index_type != KVS_VECTOR_INDEX_TYPE_FLAT) {
  kvs_vector_index_add_current(...)
}
```

will include `core_flat`, because `core_flat` is a new enum distinct from old `FLAT`.

### 5.8 VSEARCH behavior

For `vector_index_type=core_flat`:

```text
kvs_vector_search_with_prepared
  -> kvs_vector_index_search_current
  -> core_flat adapter search
  -> kvs_flat_index_search
  -> kvs_vector_manager_resolve_hits
```

Expected behavior:

- Response format unchanged.
- Scope filtering unchanged.
- Threshold filtering unchanged.
- Deleted record filtering unchanged through manager resolution.
- Search should return only active records visible to manager.

Potential overfetch issue:

- `core_flat.search(topk)` returns at most `topk` raw hits before manager scope/deleted filtering.
- If many top raw hits are filtered by scope, result count can be less than old flat search, because old flat applies scope while scanning.
- Minimal first step may accept current `topk` behavior for `core_flat`.
- Better Stage 2D-1 option: adapter asks `core_flat` for `min(index_size, max(topk * 4, topk + 32))` hits, then manager filters down to `topk`.

### 5.9 VDEL behavior

Stage 2D-1 minimal behavior:

```text
kvs_vector_del
  -> record marked deleted
  -> id_map remove
  -> old flat mark stale
  -> sq8 needs_rebuild
  -> no core_flat mark_delete yet
```

This is acceptable only because `core_flat` search adapter calls `kvs_vector_manager_resolve_hits()`, and manager filters deleted records.

Recommended follow-up Stage 2D-2:

- Add a manager-local helper that resolves the deleted record's stable `record_id`.
- If current index is `core_flat`, call `kvs_flat_index_mark_delete()` before or after record state change.
- Keep old `kvs_vector_index_ops_t` unchanged unless there is a separate stage to add delete to all backends.

### 5.10 VINDEX REBUILD behavior

For `core_flat`, rebuild should:

```text
core_flat.reset()
core_flat.reserve(active_records)
kvs_vector_manager_for_each_active_embedding(add record_id/vector)
core_flat.info()
kvs_vector_index_bind_current()
```

There are two implementation choices:

1. Add `core_flat` handling inside `kvs_vector_index_rebuild()`.
2. Let `g_kvs_vector_core_flat_ops.rebuild()` do the reset/reserve/iteration, then ensure `kvs_vector_index_rebuild()` calls current ops rebuild for `CORE_FLAT`.

Recommended minimal option:

- Add explicit `CORE_FLAT` branch in `kvs_vector_index_rebuild()`.
- Keep old flat / ANN / SQ8 rebuild code unchanged.
- Rebind `current_index` after swap/reset as today.

### 5.11 VINFO behavior

For `core_flat`, `kvs_vector_info_from_state()` should:

```text
index_type = CORE_FLAT
index_ready = core_flat_info.ready
needs_rebuild = core_flat_info.needs_rebuild
dimension = core_flat_info.dim
index_memory_bytes includes core_flat memory
flat_physical_size may remain old flat physical size or add a new core field later
index_size remains active_records for public semantic stability
```

`current_index.ops->info` adapter can fill:

```text
metric_type
index_memory_bytes
```

Do not change VINFO response format in Stage 2D-1 unless strictly needed. If a backend name string exists in logs, add `"core_flat"` there.

## 6. Files To Modify In Stage 2D-1

Recommended minimal file list:

```text
include/kvs_vector.h
include/vector/kvs_vector_manager.h
src/core/kvs_config.c
src/vector/kvs_vector.c
Makefile
```

Notes:

- `Makefile` only needs a change if current build does not compile `src/vector/core/*.c` and `src/vector/core/flat/*.c` into the main binary.
- If core files are already in build by the time Stage 2D-1 starts, do not modify Makefile.

Potentially needed only for VINFO display text:

```text
src/core/kvs_command_executor.c
src/core/kvs_operation.c
```

Avoid touching command handlers unless `core_flat` needs to appear in a user-visible backend name string.

## 7. Files Not To Modify In Stage 2D-1

Do not modify:

```text
src/persistence/kvs_rdb.c
src/persistence/kvs_aof.c
src/persistence/kvs_aof_rewrite.c
src/vector/kvs_vector_index.c        // old flat / ANN / SQ8 implementation
src/vector/kvs_vector_faiss_ops.c
src/vector/kvs_vector_hnswlib_ops.c
src/vector/kvs_vector_faiss_backend.cpp
src/vector/kvs_vector_hnswlib_backend.cpp
src/vector/core/flat/kvs_flat_search.c
src/vector/core/flat/kvs_flat_storage.c
src/vector/core/flat/kvs_flat_index.c
src/vector/core/flat/kvs_flat_info.c
include/vector/kvs_vector_core.h
src/core/kvs_command_task.c
src/core/kvs_command_executor.c      // except optional backend-name display
```

Do not move `src/vector` directories in Stage 2D-1.

## 8. Coupling Points And Risks

### 8.1 record_index vs record_id

Old ops add receives `record_index`; new core add needs stable `record_id`.

Use:

```text
kvs_vector_manager_get_active_record_id_by_index(record_index, &record_id)
```

Do not pass `record_index` as `record_id`.

### 8.2 Scope filtering recall

`core_flat` currently does not know scope. If adapter asks only `topk` raw hits, manager filtering can reduce final result count.

Risk mitigation:

- Stage 2D-1 can overfetch.
- Keep overfetch local to adapter; do not put scope into core.

### 8.3 VDEL immediate deletion

Without adapter delete, `core_flat` may still return deleted record ids internally. Manager will filter them, so external semantics remain correct.

Risk:

- More filtered candidates under heavy delete workloads.
- `core_flat.deleted_count` will not reflect VDEL until mark_delete is wired.

Recommendation:

- Do not wire VDEL mark_delete in Stage 2D-1 unless explicitly scoped.
- Add it in Stage 2D-2.

### 8.4 Rebuild ownership

`kvs_vector_index_rebuild()` currently owns old flat / ANN / SQ8 safe rebuild logic and does not uniformly call all backend ops rebuild.

Risk:

- Adding generic ops rebuild could accidentally change hnswlib / Faiss behavior.

Recommendation:

- Add a narrow `CORE_FLAT` branch only.

### 8.5 Build integration

The standalone tests compile core files explicitly. The main Makefile may not yet include:

```text
src/vector/core/kvs_vector_id_slot_map.c
src/vector/core/kvs_vector_heap.c
src/vector/core/flat/kvs_flat_index.c
src/vector/core/flat/kvs_flat_storage.c
src/vector/core/flat/kvs_flat_search.c
src/vector/core/flat/kvs_flat_info.c
```

If main binary references `core_flat`, these files must be linked.

### 8.6 VINFO memory accounting

`kvs_vector_estimate_current_index_memory_bytes()` currently switches on old index types. `core_flat` needs a case that reads `kvs_flat_index_info()`.

Do not reuse old flat memory estimator for `core_flat`.

## 9. Stage 2D-1 Execution Recommendation

Recommended next stage:

```text
Stage 2D-1: add core_flat adapter and config selection
```

Concrete steps:

1. Add `KVS_VECTOR_INDEX_CORE_FLAT` to `kvs_vector_index_type_t` by appending a new value.
2. Add `core_flat_index` to `kvs_vector_state_t`.
3. Initialize and destroy `core_flat_index` in vector module lifecycle.
4. Accept `core_flat` in config file and CLI `--vector_index_type`.
5. Map `core_flat` in `kvs_vector_index_type_from_config()`.
6. Add `g_kvs_vector_core_flat_ops` adapter without changing `kvs_vector_index_ops_t`.
7. Bind `CORE_FLAT` in `kvs_vector_index_bind_current()`.
8. Make `VINDEX REBUILD` rebuild `core_flat` from active embeddings.
9. Update VINFO internal info and backend-name display for `core_flat`.
10. Add a focused integration test:

```text
start kvstore with --vector_index_type core_flat
VSET q1 a1 scopeA
VSET q2 a2 scopeA
VSET q3 a3 scopeB
VSEARCH q1 scopeA 2
VDEL q1 scopeA
VSEARCH q1 scopeA 2
VINDEX REBUILD
VINFO
```

Expected:

- `VSEARCH` format unchanged.
- `VDEL` removes the deleted record from visible search results.
- `VINDEX REBUILD` succeeds.
- `VINFO` indicates `core_flat`.

## 10. Non-Goals

Do not do these in Stage 2D-1:

- Do not replace old `flat` with `core_flat`.
- Do not remove old flat baseline writes.
- Do not modify RDB/AOF formats.
- Do not change `kvs_vector_core_ops_t`.
- Do not add `mark_delete` to `kvs_vector_index_ops_t`.
- Do not implement `core_hnsw`.
- Do not move `src/vector` directories.
- Do not put scope / TTL / metadata / question / answer into `core_flat`.
