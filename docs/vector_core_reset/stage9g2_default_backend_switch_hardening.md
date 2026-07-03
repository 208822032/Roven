# Stage 9G2: default backend switch hardening

## AGENTS.md compliance

已读取并遵守根目录 `AGENTS.md`。本阶段与 `AGENTS.md` 无冲突。

本阶段只做 default backend switch hardening：未做 SIMD / reverse_prune / layout / cache 优化，未引入 background thread，未删除 `core_flat` 或 legacy current `core_hnsw`，未修改 RDB/AOF 格式，未修改 `kvs_vector_index_ops_t` 签名，未修改 VSEARCH response，未让 `src/vector/core/hnsw_v2` 访问 manager-owned state。

## Modified / added files

Stage 9G2 本阶段修改 / 新增：

- `Makefile`
- `include/kvs_config.h`
- `include/kvs_vector.h`
- `include/vector/kvs_vector_manager.h`
- `src/core/kvs_config.c`
- `src/vector/kvs_vector.c`
- `tests/README.md`
- `tests/vector_core_hnsw_v2_product_integration_test.c`
- `tests/vector_core_hnsw_v2_product_integration_test.sh`
- `tests/vector_core_hnsw_v2_default_backend_test.c`
- `tests/vector_core_hnsw_v2_default_backend_test.sh`
- `docs/vector_core_reset/stage9g2_default_backend_switch_hardening.md`

工作树中仍保留 Stage 9G0 / 9G1 未提交改动与报告；本阶段未回退这些内容。

## Production code changed

生产代码有变更，范围限于 backend selection / config default / comments:

- 默认 `vector_index_type` 从 `hnsw` 改为 `core_hnsw_v2`。
- `vector_index_type=core_hnsw_v2` 不再被 `vector_hnsw_v2_experimental_enabled` gate 阻断。
- `vector_index_type=flat` 和 `core_flat` 都选择 `core_flat` exact baseline。
- `vector_index_type=core_hnsw` 继续显式选择 legacy current `core_hnsw`。
- `vector_index_type=hnsw` / `hnswlib_hnsw` 保留为 hnswlib compatibility adapter 名称。

未改 current `core_hnsw` 算法代码。

## Current call chain

默认未配置：

```text
kvs_config_init_default()
  -> vector_index_type = core_hnsw_v2
  -> kvs_vector_index_type_from_config()
  -> KVS_VECTOR_INDEX_CORE_HNSW_V2
  -> g_kvs_vector_core_hnsw_v2_ops
  -> core_hnsw_v2 record_id/vector-only API
  -> manager resolve/filter/response assembly
```

显式 legacy:

```text
vector_index_type=core_hnsw
  -> KVS_VECTOR_INDEX_CORE_HNSW
  -> legacy current core_hnsw product wrapper
```

显式 exact baseline:

```text
vector_index_type=flat
or vector_index_type=core_flat
  -> KVS_VECTOR_INDEX_CORE_FLAT
  -> core_flat exact baseline
```

## Before behavior

Stage 9G0 / 9G1 后，`core_hnsw_v2` 已可作为 product backend 使用，但需要显式配置：

```text
vector_index_type=core_hnsw_v2
vector_hnsw_v2_experimental_enabled=yes
```

未配置默认仍不是 v2。

## After design

默认 ANN backend 已切到 `core_hnsw_v2`。`core_flat` 保留为 exact correctness baseline / fallback；legacy current `core_hnsw` 保留为显式 regression/reference/emergency fallback。

配置名映射：

| config | backend |
|---|---|
| unset | `core_hnsw_v2` |
| `core_hnsw_v2` | `core_hnsw_v2` |
| `flat` | `core_flat` |
| `core_flat` | `core_flat` |
| `core_hnsw` | legacy current `core_hnsw` |
| `hnsw` / `hnswlib_hnsw` | legacy hnswlib compatibility adapter when compiled |

## `vector_hnsw_v2_experimental_enabled`

该字段保留为历史兼容 / telemetry 字段：

```text
vector_hnsw_v2_experimental_enabled=0
```

不再阻止默认或显式 `core_hnsw_v2` 路径。VINFO 仍输出 `core_hnsw_v2_experimental_enabled`，用于兼容旧观察脚本；不要再把它解释为 v2 enable gate。

## VINFO compatibility

默认 v2 path 下 VINFO 显示：

- `index_type:core_hnsw_v2`
- `backend_name:core_hnsw_v2`
- `core_hnsw_v2_size`
- `core_hnsw_v2_deleted_count`
- `core_hnsw_v2_active_count`
- `core_hnsw_v2_memory_bytes`
- `rebuild_state`
- `rebuild_delta_count`
- `rebuild_error_code`

legacy `core_hnsw` path 下原有 `core_hnsw_*` fields 仍可用。`flat` path 映射到 `core_flat`，不展示误导性 v2 active backend。

## Failure / fallback strategy

默认 v2 init failure：fail fast，不静默 fallback 到 legacy `core_hnsw` 或 `core_flat`。

`VINDEX REBUILD` 继续使用现有 safe rebuild flow；rebuild 失败不应破坏 record store，也不应把半初始化 shadow graph 切为 active。可直接注入的 product failure harness 本阶段只覆盖 repeated VDEL / update / tombstone re-add / rebuild / RDB reload / AOF replay；更细粒度 init/rebuild failure injection 留给 Stage 9G3 hardening。

## Safety invariants

- VSEARCH response format unchanged.
- RDB/AOF format unchanged; v2 graph remains derived runtime state rebuilt from records.
- `kvs_vector_index_ops_t` unchanged.
- `core_flat` remains exact baseline / fallback.
- legacy current `core_hnsw` remains explicitly selectable.
- v2 core boundary preserved: no `g_vector_state`, no `record_id_map`, no scope/TTL/metadata/question/answer in `src/vector/core/hnsw_v2`.
- No silent fallback from default v2 to another backend.

## Correctness tests

Build:

```text
make clean && make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)" PASS
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 optional-profile-tests -j"$(nproc)" PASS
```

core_flat baseline:

```text
tests/vector_core_flat_correctness_test.sh PASS
tests/vector_core_flat_rebuild_test.sh PASS
tests/vector_core_flat_rdb_reload_test.sh PASS
```

legacy current core_hnsw:

```text
tests/vector_core_hnsw_correctness_test.sh PASS
tests/vector_core_hnsw_rebuild_test.sh PASS
tests/vector_core_hnsw_rdb_reload_test.sh PASS
tests/vector_core_hnsw_aof_replay_test.sh PASS
```

core_hnsw_v2:

```text
tests/vector_core_hnsw_v2_tombstone_test.sh PASS
tests/vector_core_hnsw_v2_skeleton_test.sh PASS
tests/vector_core_hnsw_v2_graph_construction_test.sh PASS
tests/vector_core_hnsw_v2_search_recall_test.sh PASS
tests/vector_core_hnsw_v2_product_integration_test.sh PASS
tests/vector_core_hnsw_v2_default_backend_test.sh PASS
```

manager-owned rebuild harness:

```text
tests/vector_rebuild_state_machine_test.sh PASS
tests/vector_rebuild_delta_capture_test.sh PASS
tests/vector_rebuild_shadow_replay_test.sh PASS
tests/vector_rebuild_shadow_swap_test.sh PASS
```

Default path command-level coverage:

- default config without `vector_index_type` selects `core_hnsw_v2`
- VSET then VSEARCH returns active records
- VDEL excludes deleted record
- repeated VDEL stays safe
- VINDEX REBUILD clears tombstones and preserves active search
- VINFO shows `backend_name:core_hnsw_v2`
- RDB reload rebuilds v2 from records
- AOF replay rebuilds v2 from records
- `vector_index_type=flat` selects `core_flat`
- `vector_index_type=core_hnsw` selects legacy current `core_hnsw`
- VSEARCH response does not expose `record_id` or `slot`

## Recall / benchmark result

No performance optimization was attempted in this stage. Existing v2 recall gate remains healthy:

| Dataset | ef_search | recall@10 |
|---|---:|---:|
| 1K / 512d | 256 | 1.0000 |
| 5K / 512d | 256 | 1.0000 |
| 10K / 512d | 64 | 0.6800 |
| 10K / 512d | 128 | 0.8840 |
| 10K / 512d | 256 | 0.9860 |
| 10K / 512d | 512 | 1.0000 |
| 10K / 512d | 1024 | 1.0000 |

ef sensitivity is monotonic non-decreasing.

## Static boundary check result

All required static checks passed:

```bash
grep -RIn "g_vector_state" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output
grep -RIn "record_id_map" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output
grep -RIn "scope\|ttl\|metadata\|question\|answer" src/vector/core/hnsw_v2 include/vector/kvs_vector_hnsw_v2.h || true
# no output
git diff --check
# PASS
```

## Optional profile target

`make optional-profile-tests` still builds:

- `vector_core_hnsw_v2_performance_profile_test`
- `vector_core_hnsw_v2_distance_profile_test`
- `vector_core_hnsw_v2_reverse_prune_profile_test`

Default `make` / `make tests` do not rebuild old current-core HNSW profile artifacts and do not include expensive optional v2 profile binaries.

## VSEARCH / RDB / ops compatibility

- VSEARCH response format changed: no.
- RDB/AOF format changed: no.
- `kvs_vector_index_ops_t` changed: no.
- current `core_hnsw` algorithm changed: no.

## git status --short

```text
 M Makefile
 M "docs/vector_core_reset/\344\274\230\345\214\226\346\255\245\351\252\244.md"
 M "docs/vector_core_reset/\346\211\247\350\241\214\346\255\245\351\252\244\344\271\246.md"
 M include/kvs_config.h
 M include/kvs_vector.h
 M include/vector/kvs_vector_manager.h
 M src/core/kvs_config.c
 M src/core/kvs_operation.c
 M src/vector/kvs_vector.c
 M tests/README.md
 D tests/vcmd_profile_bench.c
 D tests/vector_core_hnsw_rebuild_profile_test.sh
?? docs/vector_core_reset/stage9g0_experimental_core_hnsw_v2_product_integration.md
?? docs/vector_core_reset/stage9g1_cleanup_codebase_consolidation.md
?? docs/vector_core_reset/stage9g2_default_backend_switch_hardening.md
?? tests/vector_core_hnsw_v2_default_backend_test.c
?? tests/vector_core_hnsw_v2_default_backend_test.sh
?? tests/vector_core_hnsw_v2_product_integration_test.c
?? tests/vector_core_hnsw_v2_product_integration_test.sh
```

## Next suggested step

如果 Stage 9G2 final static checks 通过，进入：

```text
Stage 9G3: scale/stability gate and final consolidation
```

Stage 9G3 should stay limited to scale/stability gate, tombstone ratio rebuild policy, cold start / bulk import risk, final architecture docs, and retrospective. Do not directly enter SIMD/layout/reverse_prune micro-optimization and do not delete legacy current `core_hnsw`.
