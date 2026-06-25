# Stage 3I core_hnsw integration report

## 1. 修改 / 新增文件列表

本轮修改：

- `include/kvs_vector.h`
- `include/vector/kvs_vector_manager.h`
- `src/core/kvs_config.c`
- `src/core/kvs_operation.c`
- `src/core/kvs_command_executor.c`
- `src/vector/kvs_vector.c`
- `Makefile`

本轮新增临时集成测试：

- `/tmp/kvlite_stage3i_core_hnsw_integration.py`

本轮新增报告：

- `docs/vector_core_reset/stage3i_core_hnsw_integration_report.md`

## 2. 是否修改生产代码

是。本轮把 core_hnsw 接入真实 V 族命令路径：

- 新增 `KVS_VECTOR_INDEX_CORE_HNSW`。
- manager state 新增 `kvs_hnsw_index_t core_hnsw_index`。
- 配置和命令行支持 `core_hnsw`。
- 新增 core_hnsw old-ops adapter。
- VDEL 对 core_hnsw 调用 `kvs_hnsw_index_mark_delete()`。
- VINDEX REBUILD / RDB load 后 rebuild 支持 core_hnsw。
- VINFO 增加 core_hnsw 字段。

未修改 RDB/AOF 格式，未替换 hnswlib / faiss / old hnsw。

## 3. core_hnsw adapter 调用链

VSET：

```text
VSET
  -> kvs_vector_set_commit_prepared
  -> flat baseline add
  -> kvs_vector_index_add_current
  -> g_kvs_vector_core_hnsw_ops.add
  -> kvs_vector_manager_get_active_record_id_by_index
  -> kvs_hnsw_index_add(record_id, embedding)
```

VSEARCH：

```text
VSEARCH
  -> kvs_vector_index_search_current
  -> g_kvs_vector_core_hnsw_ops.search
  -> kvs_hnsw_index_search(query, raw_topk)
  -> kvs_vector_manager_resolve_hits
  -> 原有 kvs_vector_search_result_t 响应格式
```

VDEL：

```text
VDEL
  -> kvs_vector_del
  -> 保存 record_id
  -> kvs_vector_record_mark_deleted
  -> kvs_hnsw_index_mark_delete(record_id)
  -> id_map remove / free-list push
```

VINDEX REBUILD：

```text
VINDEX REBUILD
  -> kvs_vector_index_rebuild
  -> compact store / rebuild id_map
  -> rebuild flat baseline
  -> g_kvs_vector_core_hnsw_ops.rebuild
  -> kvs_hnsw_index_reset
  -> kvs_vector_manager_for_each_active_embedding
  -> kvs_hnsw_index_add(record_id, embedding)
```

## 4. VSET / VSEARCH / VDEL / VINDEX REBUILD / VINFO 行为

VSET：

- `core_hnsw` 下仍写入旧 flat baseline。
- 当前 backend 非 flat，因此继续通过 old ops 写入 core_hnsw。
- core_hnsw 只接收 `record_id + embedding`。

VSEARCH：

- core_hnsw 返回 `record_id + score`。
- manager 负责回表、scope 过滤、threshold 过滤、deleted/active 过滤和响应组装。
- 响应格式不暴露 record_id / slot。

VDEL：

- 第一次删除会增加 `core_hnsw_deleted_count`。
- repeated delete 返回 NO EXIST 或等价错误，不重复增加 `core_hnsw_deleted_count`。
- mark_delete 不删除 id map，不改图，不 compact。

VINDEX REBUILD：

- 从 active records 重建 core_hnsw。
- rebuild 后 `core_hnsw_deleted_count` 回到 0。
- deleted records 不重新 add，因此不可见。

VINFO：

- `index_type:core_hnsw`
- `vector_backend:core_hnsw`
- 保留既有字段。
- 新增 core_hnsw 字段见下一节。

## 5. RDB reload 后 rebuild 行为

RDB 仍只保存 vector records 和已有诊断字段，不保存 HNSW graph。

加载流程：

```text
kvs_vector_reset_for_load
  -> 初始化 core_hnsw_index
RDB restore records
kvs_vector_rebuild_after_load
  -> rebuild id_map
  -> rebuild flat baseline
  -> bind current index
  -> core_hnsw adapter rebuild from active records
```

验证结果：

- reload 后 active records 可查。
- reload 后 deleted records 不可见。
- reload 后 `VINDEX REBUILD` 成功。
- reload + rebuild 后 `core_hnsw_deleted_count` 为 0。

## 6. VINFO 字段支持情况

新增字段：

- `core_hnsw_size`
- `core_hnsw_capacity`
- `core_hnsw_m`
- `core_hnsw_m0`
- `core_hnsw_ef_construction`
- `core_hnsw_ef_search`
- `core_hnsw_max_level`
- `core_hnsw_deleted_count`

字段映射：

- `capacity / size / dim / metric / memory_bytes / ready / needs_rebuild / deleted_count` 来自 `kvs_hnsw_index_info()`。
- `M / M0 / ef_construction / ef_search / max_level` 来自 `kvs_hnsw_index_t` manager state。
- `index_memory_bytes` 使用 core_hnsw 的 `memory_bytes`，总内存统计仍包含 flat baseline。

## 7. 集成测试结果

执行：

```sh
python3 /tmp/kvlite_stage3i_core_hnsw_integration.py \
  /home/zhengpan/share/KVLite/9.1-kvstore ./kvstore
```

结果：

```text
kvlite_stage3i_core_hnsw_integration PASS
```

覆盖：

- `--vector_index_type core_hnsw` 命令行覆盖配置。
- 配置文件 `vector_index_type = core_hnsw`。
- 默认 index type 未改变。
- VINFO 显示 `index_type:core_hnsw`。
- VSET / VSEARCH。
- scopeA / scopeB 隔离。
- update 后 answer 更新且无重复可见结果。
- VDEL 后不返回 deleted record。
- repeated delete 不重复增加 `core_hnsw_deleted_count`。
- VINDEX REBUILD 后 active records 可查、deleted records 不可见。
- RDB save / reload。
- reload 后 active records 可查、deleted records 不可见。
- reload 后 VINDEX REBUILD 成功。

## 8. 静态边界检查结果

执行：

```sh
grep -RIn "g_vector_state" \
  include/vector/kvs_vector_hnsw.h \
  src/vector/core/hnsw 2>/dev/null
```

结果：无输出。

执行：

```sh
grep -RIn "question\\|answer\\|scope\\|TTL\\|metadata" \
  include/vector/kvs_vector_hnsw.h \
  src/vector/core/hnsw 2>/dev/null
```

结果：仅 `include/vector/kvs_vector_hnsw.h` 的边界注释出现。

执行：

```sh
rg -n "kvs_vector_manager|kvs_vector_search_result_t|record store|g_vector_state" \
  include/vector/kvs_vector_hnsw.h src/vector/core/hnsw
```

结果：无输出。

执行：

```sh
rg -n "kvs_malloc|kvs_free|malloc|free" src/vector/core/hnsw/kvs_hnsw_search.c
```

结果：无输出。

## 9. 全量构建结果

执行：

```sh
make clean
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
```

结果：通过，输出 `✓ 构建完成`。

## 10. 当前 git status --short

```text
 M Makefile
 D docs/vector_core_reset/00_vector_core_slimming_report.md
 D docs/vector_core_reset/01_vector_manager_kernel_design.md
 D docs/vector_core_reset/02_third_party_vector_engine_audit.md
 D docs/vector_core_reset/03_kvlite_vector_manager_kernel_design.md
 D docs/vector_core_reset/04_vector_compute_kernel_design.md
 D docs/vector_core_reset/05_vector_backend_reselection_plan.md
 D docs/vector_core_reset/06_performance_first_self_hnsw_design.md
 D docs/vector_core_reset/07_usearch_inspired_flat_hnsw_core_execution_plan.md
 M include/kvs_vector.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_command_executor.c
 M src/core/kvs_config.c
 M src/core/kvs_operation.c
 M src/vector/kvs_vector.c
?? docs/vector_core_reset/stage3abc_core_hnsw_lifecycle_context_report.md
?? docs/vector_core_reset/stage3de_core_hnsw_heap_random_report.md
?? docs/vector_core_reset/stage3f2_core_hnsw_multilayer_add_report.md
?? docs/vector_core_reset/stage3f_core_hnsw_add_report.md
?? docs/vector_core_reset/stage3gh_core_hnsw_search_delete_report.md
?? include/vector/kvs_vector_hnsw.h
?? src/vector/core/hnsw/
```

旧设计文档删除为进入本轮前已有状态，本轮未处理。

## 11. 禁止项确认

本轮未修改 RDB/AOF 格式。

本轮未修改 `kvs_vector_index_ops_t` 签名。

本轮未替换 hnswlib / faiss / old hnsw。

本轮未删除旧 flat baseline 写入。

本轮未做 benchmark。

core_hnsw 本体仍不访问 `g_vector_state`，不处理 scope / TTL / metadata / question / answer。
