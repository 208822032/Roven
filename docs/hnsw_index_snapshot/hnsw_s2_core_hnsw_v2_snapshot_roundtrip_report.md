# HNSW-S2: core_hnsw_v2 snapshot 目录组织 + graph cache roundtrip 报告

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

本阶段只做 `core_hnsw_v2` 的 anncore graph cache serialize / deserialize，以及 snapshot 源码目录组织。未修改 VSEARCH response、RDB/AOF 主格式、`kvs_vector_index_ops_t`、默认 backend、默认 HNSW 参数、startup load、rebuild success hook、background rebuild 或 KVManager snapshot lifecycle。`core_hnsw_v2` 仍不访问 `g_vector_state`、manager `record_id_map`、scope、TTL、metadata、question、answer。

冲突说明：`AGENTS.md` 的历史 stage 模板要求报告位于 `docs/vector_core_reset/`；本任务明确属于 `docs/hnsw_index_snapshot/` 的 HNSW-S 路线，因此报告按任务指定目录输出，同时继续遵守 manager/vector_core、RDB/AOF、response 和 index ops 边界。当前仓库中 legacy `core_hnsw` / FAISS / hnswlib 相关删除和兼容 fallback 是前序 CODE-HYGIENE 改动，本阶段没有恢复或处理这些路径。

## 是否修改生产代码

是，但只新增 v2-only snapshot graph cache 能力，并接入构建。当前产品启动、RDB/AOF、VSEARCH、VINDEX REBUILD、active backend 绑定路径均未调用该能力。

## 新增/修改文件

新增：

- `include/vector/kvs_vector_hnsw_v2_snapshot.h`
- `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_format.h`
- `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_io.c`
- `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_serialize.c`
- `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_deserialize.c`
- `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_validate.c`
- `tests/vector_core_hnsw_v2_snapshot_roundtrip_test.c`
- `tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh`
- `docs/hnsw_index_snapshot/hnsw_s2_core_hnsw_v2_snapshot_roundtrip_report.md`

修改：

- `Makefile`
- `docs/hnsw_index_snapshot/README.md`
- `docs/hnsw_index_snapshot/index.md`
- `docs/hnsw_index_snapshot/snapshot_design.md`
- `docs/hnsw_index_snapshot/implementation_plan.md`

## 新增目录结构

```text
src/vector/core/hnsw_v2_snapshot/
  kvs_hnsw_v2_snapshot_format.h
  kvs_hnsw_v2_snapshot_io.c
  kvs_hnsw_v2_snapshot_serialize.c
  kvs_hnsw_v2_snapshot_deserialize.c
  kvs_hnsw_v2_snapshot_validate.c
```

新增该目录是为了让 graph cache 读写能力独立于 add/search/neighbor_select/reverse_prune 主路径，避免把冷启动 cache 逻辑塞进构图或搜索热路径文件。

## Header 边界

Public header：

```text
include/vector/kvs_vector_hnsw_v2_snapshot.h
```

只暴露：

```c
int kvs_hnsw_v2_index_serialize_file(const kvs_hnsw_v2_index_t *index, const char *path);
int kvs_hnsw_v2_index_deserialize_file(kvs_hnsw_v2_index_t *index, const char *path);
int kvs_hnsw_v2_index_validate_basic(const kvs_hnsw_v2_index_t *index, char *reason, size_t reason_size);
```

Private format header：

```text
src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_format.h
```

只在 v2 snapshot 实现内部使用，不暴露给 KVManager。

## 序列化字段清单

持久化的是 anncore graph cache：

- params：`dim`、`metric`、`M`、`M0`、`efConstruction`、`efSearch`
- index state：`size`、`capacity`、`active_count`、`deleted_count`、`entry_point`、`entry_level`、`max_level`
- runtime continuation：`rng_state`
- record mapping：`slot_to_record_id`
- vectors：每个 slot 写入 `dim * sizeof(float)`，不写 vector stride padding
- node state：`levels`、`deleted_flags`
- graph：`level0_counts`、`level0_neighbors`、`upper_counts`、`upper_neighbors`

不序列化：

- manager record store
- manager `record_id_map`
- question / answer / scope / TTL / metadata
- response assembly
- RDB/AOF 主格式
- `visited_tags`
- search context buffers
- graph stats / memory stats

`record_id -> slot` hash map 不直接写入 snapshot，而是在 deserialize 时按 slot 顺序从 `slot_to_record_id` 重建。这样不把 hash bucket layout 固化为文件格式，并保留 tombstone old slot + re-add same record_id 后最新 slot 覆盖旧 slot 的语义。

## Binary format header

当前 private header：

```c
typedef struct kvs_hnsw_v2_snapshot_header_s {
    char magic[32];
    uint32_t format_version;
    uint32_t header_size;

    uint32_t dim;
    uint32_t metric;
    uint32_t m;
    uint32_t m0;
    uint32_t ef_construction;
    uint32_t ef_search;

    uint64_t size;
    uint64_t capacity;
    uint64_t active_count;
    uint64_t deleted_count;

    int64_t entry_point;
    int32_t entry_level;
    int32_t max_level;

    uint64_t rng_state;
    uint64_t payload_size;
    uint64_t payload_checksum;
} kvs_hnsw_v2_snapshot_header_t;
```

Payload 顺序：

```text
slot_to_record_id[0..size)
vectors[slot][0..dim)
levels[0..size)
deleted_flags[0..size)
level0_counts[0..size)
level0_neighbors[slot][0..M0)
upper_counts[slot][level=1..MAX_LEVEL)
upper_neighbors[slot][level=1..MAX_LEVEL][0..M)
```

## Checksum / bounds checking 策略

- checksum 使用 FNV-1a 64-bit，覆盖 payload bytes。
- deserialize 校验 `magic`、`format_version`、`header_size`、`dim`、`metric`、HNSW 参数、`size <= capacity`、`deleted_count <= size`、`active_count + deleted_count == size`。
- 校验 `payload_size` 与 header 参数推导值一致。
- 校验实际文件大小等于 `sizeof(header) + payload_size`。
- payload 读取失败、checksum mismatch、header malformed 都返回失败。
- `kvs_hnsw_v2_index_validate_basic()` 校验 level bounds、entry bounds、deleted flag、degree bounds、neighbor slot range、upper layer neighbor level、self edge、duplicate edge、duplicate active record id。
- deserialize 后刷新 `graph_stats` 和 `memory`，不直接信任旧 stats。

## Deserialize failure safety

deserialize 使用临时 `kvs_hnsw_v2_index_t`：

```text
read header
validate header
init temp index
reserve temp capacity
read payload into temp
verify checksum
rebuild temp record_id_to_slot
refresh temp graph/memory stats
validate temp graph
destroy target
move temp into target
```

任一失败：

- 释放临时 index。
- 不 destroy 目标 index。
- 不发布半初始化 graph。
- 返回失败。

roundtrip 测试覆盖 bad magic、truncated file、malformed header 后目标 index 仍可搜索。

## Roundtrip 测试覆盖

新增：

```text
tests/vector_core_hnsw_v2_snapshot_roundtrip_test.c
tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh
```

覆盖：

- empty index serialize / deserialize / search。
- 100 vectors / 32d 小规模 graph roundtrip。
- serialize 前后 topK record_id 和 score 顺序一致。
- mark_delete 后 roundtrip，deleted record 不返回。
- same-record update：tombstone old slot + re-add same record_id 后 roundtrip。
- roundtrip 后 `record_id_to_slot` 指向最新 active slot。
- size / capacity / deleted_count / entry / max_level / level0 degree stats 保持一致。
- duplicate edge / degree overflow 为 0。
- corrupted magic fails。
- truncated file fails。
- malformed header fails。
- deserialize failure 不污染目标 index。

## 为什么本阶段不接 KVManager startup

S2 只验证 anncore graph cache bytes 能否 roundtrip。startup load 需要 records load 之后的 manifest 对照、fallback 策略、active index 发布和 rebuild 调度，属于 HNSW-S3。把 startup 接线混入 S2 会把文件格式验证、manager lifecycle 和 fallback 策略混在一个阶段，风险过大。

## 为什么本阶段不修改 RDB/AOF

records 仍是 source of truth。S2 的 `vector_index.snapshot` payload 是可丢弃 cache，不属于 RDB/AOF 主格式。snapshot 缺失、损坏或不匹配时，后续仍应回到 records rebuild。

## Snapshot 仍不是 source of truth

S2 不保存 manager record store，也不保存业务字段。snapshot 只能恢复 `record_id + vector + graph` 的 index-local derived state。真实数据仍来自 records / RDB / AOF。

## 与 S1 manifest 的关系

S1 manifest 是外层 `vector_index.manifest`，使用文本 `key=value`，负责 backend、dim、metric、HNSW 参数、count、generation/checksum、snapshot size/checksum 等元信息。

S2 binary header 是 anncore 内部 `vector_index.snapshot` payload header，负责单个 snapshot 文件的本地 format、payload size 和 payload checksum。S2 没有修改 S1 manifest schema。

## 当前 call chain

当前仅测试调用：

```text
tests/vector_core_hnsw_v2_snapshot_roundtrip_test
  -> kvs_hnsw_v2_index_add / mark_delete / search
  -> kvs_hnsw_v2_index_serialize_file
  -> kvs_hnsw_v2_index_deserialize_file
  -> kvs_hnsw_v2_index_search / info / validate
```

产品路径未调用：

```text
kvs_vector_rebuild_after_load()
kvs_vector_index_bind_current()
RDB/AOF load
VINDEX REBUILD
VINFO
```

## 测试结果

Build：

```text
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
```

Required tests：

```text
tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh PASS
tests/vector_index_snapshot_manifest_test.sh PASS
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_tombstone_test.sh PASS
tests/vector_core_hnsw_v2_default_backend_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
```

Search recall 关键输出：

| count | dim | ef | recall@10 | build_us | search_avg_us |
|---:|---:|---:|---:|---:|---:|
| 1000 | 512 | 256 | 1.0000 | 4,016,503 | 219 |
| 5000 | 512 | 256 | 1.0000 | 44,177,953 | 1,020 |
| 10000 | 512 | 64 | 0.7780 | 127,553,358 | 1,102 |
| 10000 | 512 | 128 | 0.9580 | 127,553,358 | 1,597 |
| 10000 | 512 | 256 | 0.9980 | 127,553,358 | 2,208 |
| 10000 | 512 | 512 | 1.0000 | 127,553,358 | 2,608 |
| 10000 | 512 | 1024 | 1.0000 | 127,553,358 | 3,198 |

## Boundary grep

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h include/vector/kvs_vector_hnsw_v2_snapshot.h || true
grep -RIn "record_id_map" src/vector/core/hnsw_v2 src/vector/core/hnsw_v2_snapshot include/vector/kvs_vector_hnsw_v2.h include/vector/kvs_vector_hnsw_v2_snapshot.h || true
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 src/vector/core/hnsw_v2_snapshot include/vector/kvs_vector_hnsw_v2.h include/vector/kvs_vector_hnsw_v2_snapshot.h || true
```

结果：

```text
no output
```

## git diff --check

```text
PASS
```

## git status --short

当前 worktree 包含大量前序 CODE-HYGIENE / HNSW 产品化 / S1 未提交改动。本阶段新增/修改项包括：

```text
 M Makefile
 M docs/hnsw_index_snapshot/README.md
 M docs/hnsw_index_snapshot/implementation_plan.md
 M docs/hnsw_index_snapshot/index.md
 M docs/hnsw_index_snapshot/snapshot_design.md
?? docs/hnsw_index_snapshot/hnsw_s2_core_hnsw_v2_snapshot_roundtrip_report.md
?? include/vector/kvs_vector_hnsw_v2_snapshot.h
?? src/vector/core/hnsw_v2_snapshot/
?? tests/vector_core_hnsw_v2_snapshot_roundtrip_test.c
?? tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh
```

完整 `git status --short`：

```text
 M Makefile
 M docs/hnsw_productization/README.md
 M docs/hnsw_productization/hnsw_p0_recall_attribution_report.md
 M docs/hnsw_productization/hnsw_p2_100k_balanced_default_report.md
 M docs/hnsw_productization/hnsw_p5_rebuild_cold_start_report.md
 M docs/hnsw_productization/hnsw_p5_rebuild_cold_start_strategy.md
 M docs/hnsw_productization/index.md
 M docs/hnsw_productization/interview_narrative.md
 M docs/vector_core_reset/core_hnsw_v2_default_backend_summary.md
 M docs/vector_core_reset/core_hnsw_v2_final_architecture.md
 M docs/vector_core_reset/core_hnsw_v2_interview_review.md
 M docs/vector_core_reset/stage9e1a_core_hnsw_v2_candidate_quality_attribution.md
 M docs/vector_core_reset/stage9e2_core_hnsw_v2_bounded_heap_optimization.md
 M docs/vector_core_reset/stage9e3c_core_hnsw_v2_reverse_prune_score_reuse.md
 M docs/vector_core_reset/stage9g2_default_backend_switch_hardening.md
 M docs/vector_core_reset/stage9g3_scale_stability_final_consolidation.md
 M include/kvs_command_task.h
 M include/kvs_config.h
 M include/kvs_vector.h
 D include/kvs_vector_faiss_backend.h
 D include/kvs_vector_hnswlib_backend.h
 D include/vector/kvs_vector_hnsw.h
RM src/vector/core/hnsw_v2/kvs_hnsw_v2_heap.h -> include/vector/kvs_vector_hnsw_v2_heap.h
RM src/vector/core/hnsw_v2/kvs_hnsw_v2_score_cache.h -> include/vector/kvs_vector_hnsw_v2_score_cache.h
 M include/vector/kvs_vector_index_internal.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_command_executor.c
 M src/core/kvs_command_task.c
 M src/core/kvs_config.c
 M src/core/kvs_operation.c
 D src/vector/core/hnsw/kvs_hnsw_add.c
 D src/vector/core/hnsw/kvs_hnsw_context.c
 D src/vector/core/hnsw/kvs_hnsw_delete.c
 D src/vector/core/hnsw/kvs_hnsw_graph.c
 D src/vector/core/hnsw/kvs_hnsw_index.c
 D src/vector/core/hnsw/kvs_hnsw_info.c
 D src/vector/core/hnsw/kvs_hnsw_random.c
 D src/vector/core/hnsw/kvs_hnsw_search.c
 M src/vector/core/hnsw_v2/kvs_hnsw_v2_add.c
 M src/vector/core/hnsw_v2/kvs_hnsw_v2_neighbor_select.c
 M src/vector/core/hnsw_v2/kvs_hnsw_v2_search.c
 M src/vector/kvs_vector.c
 D src/vector/kvs_vector_faiss_backend.cpp
 D src/vector/kvs_vector_faiss_ops.c
 D src/vector/kvs_vector_hnswlib_backend.cpp
 D src/vector/kvs_vector_hnswlib_ops.c
 M src/vector/kvs_vector_index.c
 M tests/README.md
 D tests/faiss_baseline.cpp
 D tests/hnswlib_smoke/hnswlib_smoke.cpp
 D tests/vcmd_onnx_faiss_bench.cpp
 M tests/vector_core_benchmark_compare.sh
 M tests/vector_core_flat_correctness_test.sh
 M tests/vector_core_flat_rdb_reload_test.sh
 M tests/vector_core_flat_rebuild_test.sh
 D tests/vector_core_hnsw_aof_replay_test.sh
 D tests/vector_core_hnsw_correctness_test.sh
 D tests/vector_core_hnsw_faiss_parity_bridge.c
 D tests/vector_core_hnsw_faiss_parity_bridge.h
 D tests/vector_core_hnsw_faiss_parity_test.cpp
 D tests/vector_core_hnsw_faiss_parity_test.sh
 D tests/vector_core_hnsw_rdb_reload_test.sh
 D tests/vector_core_hnsw_rebuild_test.sh
 M tests/vector_core_hnsw_v2_100k_balanced_gate_test.sh
 M tests/vector_core_hnsw_v2_default_backend_test.c
 M tests/vector_core_hnsw_v2_default_backend_test.sh
 M tests/vector_core_hnsw_v2_distance_profile_test.sh
 M tests/vector_core_hnsw_v2_graph_construction_test.sh
 M tests/vector_core_hnsw_v2_parity_baseline_test.c
 M tests/vector_core_hnsw_v2_parity_baseline_test.sh
 M tests/vector_core_hnsw_v2_performance_profile_test.sh
 M tests/vector_core_hnsw_v2_product_integration_test.sh
 M tests/vector_core_hnsw_v2_reverse_prune_profile_test.sh
 M tests/vector_core_hnsw_v2_scale_stability_test.sh
 M tests/vector_core_hnsw_v2_search_recall_test.sh
 M tests/vector_core_hnsw_v2_skeleton_test.sh
 M tests/vector_core_hnsw_v2_tombstone_policy_test.sh
 M tests/vector_core_hnsw_v2_tombstone_test.sh
 D tests/vector_index_only_formal.cpp
 M tests/vector_manager_resolve_fastpath_test.sh
 M tests/vector_rebuild_delta_capture_test.sh
 M tests/vector_rebuild_shadow_replay_test.sh
 M tests/vector_rebuild_shadow_swap_test.sh
 M tests/vector_rebuild_state_machine_test.sh
 D tests/vector_search_bench.c
 M tests/vector_testcase.c
 M tests/vector_vinfo_backend_stats_test.sh
 M tests/vector_vsearch_profile_breakdown_test.sh
?? docs/hnsw_index_snapshot/
?? docs/hnsw_productization/hnsw_p5_build_rebuild_bottleneck_analysis.md
?? include/vector/kvs_vector_backend_resolve.h
?? include/vector/kvs_vector_hnsw_v2_snapshot.h
?? include/vector/kvs_vector_index_snapshot.h
?? src/vector/core/hnsw_v2_snapshot/
?? src/vector/kvs_vector_backend_resolve.c
?? src/vector/kvs_vector_index_snapshot.c
?? tests/vector_core_hnsw_v2_snapshot_roundtrip_test.c
?? tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh
?? tests/vector_index_snapshot_manifest_test.c
?? tests/vector_index_snapshot_manifest_test.sh
```

## HNSW-S2 decision

```text
production_code_changed = yes
new_snapshot_source_dir = yes
anncore_serialize_added = yes
anncore_deserialize_added = yes
startup_load_changed = no
rdb_aof_changed = no
manifest_schema_changed = no
snapshot_is_source_of_truth = no
ready_for_HNSW_S3 = yes
next_stage = HNSW-S3
```
