# Stage 9G0: experimental core_hnsw_v2 product integration

## AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。本阶段与 `AGENTS.md` 无硬边界冲突。

本阶段按用户提供的 Stage 9G0 路线执行：`core_hnsw_v2` 只作为 experimental backend 接入，默认 disabled；未改变 RDB/AOF 格式，未修改 `kvs_vector_index_ops_t` 签名，未修改 VSEARCH response，未让 `src/vector/core/hnsw_v2` 访问 `g_vector_state`、`record_id_map`、scope、TTL、metadata、question、answer。

## Modified / added files

修改：

- `Makefile`
- `include/kvs_config.h`
- `include/kvs_vector.h`
- `include/vector/kvs_vector_manager.h`
- `src/core/kvs_config.c`
- `src/core/kvs_operation.c`
- `src/vector/kvs_vector.c`

新增：

- `tests/vector_core_hnsw_v2_product_integration_test.c`
- `tests/vector_core_hnsw_v2_product_integration_test.sh`
- `docs/vector_core_reset/stage9g0_experimental_core_hnsw_v2_product_integration.md`

生产代码有变更，范围限于 experimental v2 backend flag、manager/product wrapper、VINFO/VINDEX 接线和 lifecycle 初始化/销毁。

## Experimental flag / default behavior

新增配置：

```text
vector_hnsw_v2_experimental_enabled = no
```

默认值为 `0`。`vector_index_type=core_hnsw_v2` 只有在该 flag 显式启用时才选择 v2；未启用时记录日志并 fallback 到 current `core_hnsw`。默认 `vector_index_type` 仍未切到 v2，默认 VSET/VDEL/VSEARCH/VINDEX 行为保持现有 backend。

## Current call chain

默认路径：

```text
config vector_hnsw_v2_experimental_enabled=0
  -> current product backend
  -> current core_hnsw / existing default behavior
```

experimental v2 路径：

```text
config vector_index_type=core_hnsw_v2
config vector_hnsw_v2_experimental_enabled=yes
  -> kvs_vector_index_bind_current()
  -> g_kvs_vector_core_hnsw_v2_ops
  -> core_hnsw_v2 index-only API
  -> manager resolve/filter/response assembly
```

## Product wrapper design

新增 v2 product wrapper：

- `init/reserve/reset/destroy`：随 manager vector state 初始化和 load reset 初始化 `core_hnsw_v2_index`。
- `add/upsert`：manager 从 record slot 获取 active `record_id`，只把 `record_id + vector + dim` 传给 v2。
- `mark_delete`：VDEL 调用 `kvs_hnsw_v2_index_mark_delete(record_id)`。
- `search`：v2 返回 `kvs_vector_hit_t {record_id, score}`，随后仍由 manager 做 resolve、scope filter、threshold filter、active/deleted filter 和 response assembly。
- `rebuild`：同步从 record store active records 重建 v2 graph。
- `info`：把 v2 size/capacity/deleted/active/memory/ready 等状态填入 `kvs_vector_info_t`。

没有把 manager 语义字段传入 v2 core。

## VSET / VDEL / VSEARCH

experimental v2 enabled 时：

- VSET 新增记录后可被 VSEARCH 命中。
- same-record update 使用 tombstone re-add 语义：旧 `record_id` 在 v2 中 mark_delete，新 `record_id` 和新 embedding re-add。
- VDEL 使用 v2 tombstone，deleted record 不进入 VSEARCH result。
- repeated VDEL 不破坏 index。
- VSEARCH response 格式保持不变，测试通过 RESP 结果内容验证。
- scope / active / deleted / threshold filtering 仍在 manager 中完成。

默认 disabled 时，配置 `core_hnsw_v2` 不会选中 v2，测试确认 `index_type != core_hnsw_v2`。

## VINDEX rebuild

experimental v2 enabled 时，`VINDEX REBUILD` 调用 v2 wrapper rebuild：

```text
reset v2 graph
reserve active_records capacity
for each active record:
  add(record_id, embedding)
refresh graph/memory stats
```

deleted records 不进入 rebuilt index。测试验证 rebuild 后 active record 可搜索，deleted record 不返回，且 tombstone count 被 rebuild 清零。

当前是同步 rebuild，不是最终生产方案。根据 Stage 9E/9F 数据，10K/512d v2 build 仍约 56-69s，作为最终同步生产 rebuild path 风险较高。

## RDB / AOF consistency

RDB/AOF 持久化边界保持不变：

- RDB/AOF 只保存 records，不保存 v2 HNSW graph。
- load/replay 后通过 active records 重建 v2 graph。
- product integration test 覆盖 VSET/VDEL 后 RDB reload 和 AOF replay，重启后 v2 backend 可搜索 active records，deleted records 不返回。

## VINFO / status

VINFO 做兼容扩展，不改变 VSEARCH response。新增/透出：

- `core_hnsw_v2_experimental_enabled`
- `core_hnsw_v2_size`
- `core_hnsw_v2_capacity`
- `core_hnsw_v2_m`
- `core_hnsw_v2_m0`
- `core_hnsw_v2_ef_construction`
- `core_hnsw_v2_ef_search`
- `core_hnsw_v2_max_level`
- `core_hnsw_v2_deleted_count`
- `core_hnsw_v2_active_count`
- `core_hnsw_v2_memory_bytes`
- `core_hnsw_v2_ready`
- `rebuild_state`
- `rebuild_delta_count`
- `rebuild_error_code`
- `rebuild_error_message`
- `rebuild_shadow_present`
- `rebuild_retired_present`

`backend_name` 可显示 `core_hnsw_v2`，backend memory source 为 measured。

## Reader safety

本阶段未引入 background thread、epoch/refcount 或复杂 swap publication。当前 product command table 仍依赖现有命令执行/锁边界：

- VSEARCH/VINFO 属于读路径。
- VSET/VDEL/VINDEX 属于写路径。
- v2 active index 指针不通过 9F shadow/retired pointer 对产品读路径发布。
- retired cleanup 仍停留在 9F skeleton/test-only 语义。

如果后续要让 background rebuild 发布 active/shadow swap，必须先进入独立 reader-safety hardening，不应在本阶段隐式完成。

## Safety invariants

保持：

- current `core_hnsw` 仍是 fallback/default，不改其算法行为。
- `core_flat`、FAISS、hnswlib adapters 未被替换。
- `kvs_vector_index_ops_t` 签名未改。
- VSEARCH response 未改。
- RDB/AOF 格式未改，v2 graph 不持久化。
- vector_core/v2 未访问 manager/global state 或语义字段。
- record store 仍是 source of truth。

## Tests

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
```

Stage 9F prerequisites：

```text
tests/vector_rebuild_state_machine_test.sh PASS
tests/vector_rebuild_delta_capture_test.sh PASS
tests/vector_rebuild_shadow_replay_test.sh PASS
tests/vector_rebuild_shadow_swap_test.sh PASS
```

Stage 9G0 product integration：

```text
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
```

该脚本覆盖默认 disabled、explicit enabled、VSET/VSEARCH/VDEL、same-record update、VINDEX rebuild、VINFO、RDB reload、AOF replay。sandbox 内首次运行因本地 socket 被禁止出现 `PermissionError: [Errno 1] Operation not permitted`，提权后通过；该失败未进入业务逻辑。

v2 index-only / profiling：

```text
tests/vector_core_hnsw_v2_tombstone_test.sh PASS
tests/vector_core_hnsw_v2_skeleton_test.sh PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh PASS
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_performance_profile_test.sh PASS
tests/vector_core_hnsw_v2_distance_profile_test.sh PASS
tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh PASS
```

关键 v2 recall/profile 摘要：

| test | key result |
|---|---:|
| search recall 10K/512d ef=256 | recall@10 = 0.9860 |
| search recall 10K/512d ef=512 | recall@10 = 1.0000 |
| performance profile 10K/512d | build_us = 63,769,150 |
| performance profile 10K/512d ef=256 | recall@10 = 0.9860, search_avg_us = 1098.68 |
| distance profile 10K/512d AVX2 ef=256 | recall@10 = 0.9860, search_avg_us = 1328.18 |
| distance profile 10K/1024d AVX2 ef=256 | recall@10 = 0.9500, search_avg_us = 2915.94 |
| reverse prune profile 10K/512d ef=256 | recall@10 = 0.9860, search_avg_us = 1207.64 |
| reverse prune profile 10K/512d ef=512 | recall@10 = 1.0000, search_avg_us = 1738.66 |

current core_hnsw regressions：

```text
tests/vector_core_hnsw_correctness_test.sh PASS
tests/vector_core_hnsw_rebuild_test.sh PASS
tests/vector_core_hnsw_rdb_reload_test.sh PASS
tests/vector_core_hnsw_aof_replay_test.sh PASS
```

这些 socket tests 在 sandbox 内会被 socket permission 拦截；提权后均通过。

## Static boundary checks

```text
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
no output

grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
no output

grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
no output
```

`git diff --check`：

```text
PASS
```

## git status --short

```text
 M Makefile
 M include/kvs_config.h
 M include/kvs_vector.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_config.c
 M src/core/kvs_operation.c
 M src/vector/kvs_vector.c
?? docs/vector_core_reset/stage9g0_experimental_core_hnsw_v2_product_integration.md
?? tests/vector_core_hnsw_v2_product_integration_test.c
?? tests/vector_core_hnsw_v2_product_integration_test.sh
```

## Next suggested step

Stage 9G0 成功标准已满足：v2 可通过 explicit experimental flag 启用，默认 backend 行为不变，VSET/VDEL/VSEARCH/VINDEX/RDB/AOF/VINFO 路径已通过测试。

下一步建议进入 Stage 9G1 hardening，范围应只覆盖 product-read safety、config validation、VINFO compatibility/schema 和 failure/fallback 行为；不要直接做 SIMD/layout/reverse_prune 优化，也不要把 v2 切为默认 backend。
