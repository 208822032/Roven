# HNSW-S0: index snapshot / manifest 项目结构分析报告

## AGENTS.md 遵守情况

已读取并遵守根目录 `AGENTS.md`。

本阶段只新增 `docs/hnsw_index_snapshot/` 下的方案文档，未修改生产代码、测试、Makefile、VSEARCH response、RDB/AOF 主格式、`kvs_vector_index_ops_t` 或默认 HNSW 参数；未实现 graph persistence、snapshot serialize / deserialize、background thread、graph refactor、SIMD/layout/reverse_prune cache 优化；未让 `core_hnsw_v2` 访问 manager-owned semantic state。

未发现硬边界冲突。`AGENTS.md` 的历史 stage 模板要求 stage report 放在 `docs/vector_core_reset/`；本任务明确新建 `docs/hnsw_index_snapshot/` 作为 HNSW-S 路线目录，因此本报告按任务指定目录输出，同时遵守 manager/vector_core、RDB/AOF、response 和 index ops 边界。

## 是否修改生产代码

否。

本阶段没有修改：

- `src/`
- `include/`
- `tests/`
- Makefile
- VSEARCH response
- RDB/AOF 主格式
- `kvs_vector_index_ops_t`
- 默认 HNSW 参数

## 新增文件

- `docs/hnsw_index_snapshot/README.md`
- `docs/hnsw_index_snapshot/index.md`
- `docs/hnsw_index_snapshot/snapshot_design.md`
- `docs/hnsw_index_snapshot/implementation_plan.md`
- `docs/hnsw_index_snapshot/hnsw_s0_project_structure_analysis_report.md`

## 当前 RDB/AOF 与 vector records 恢复路径分析

当前 RDB vector section 保存的是 records，而不是 HNSW graph：

```text
record_index
state / deleted
record_id
question / answer / scope / normalized_question
embedding_dim + embedding bytes
created_at_ms / updated_at_ms / expire_at_ms
record generation
embedding_model_version
```

RDB load 路径是：

```text
rdb_load_vector_section()
  -> kvs_vector_reset_for_load()
  -> kvs_vector_restore_record_with_embedding() for each record
  -> kvs_vector_rebuild_after_load()
```

`kvs_vector_restore_record_with_embedding()` 会恢复 record 字段、record_id、embedding、TTL、generation 和 embedding model version。ACTIVE 且未 deleted 的 record 会重新写入 manager-owned question/scope id_map 和 record_id_map。

`kvs_vector_rebuild_after_load()` 会：

```text
1. kvs_vector_id_map_rebuild_from_store()
2. kvs_vector_rebuild_flat_index_from_store()
3. kvs_vector_index_bind_current()
4. 如果当前 backend 是 core_hnsw_v2:
     current_index.ops->rebuild()
```

AOF load 路径通过 RESP parser 重放命令：

```text
aof_load()
  -> resp_parser
  -> kvs_filter_protocol()
  -> VSET / VDEL 等命令路径
```

AOF rewrite 的 vector snapshot 当前写出的是 `VSET question answer [scope]` 命令，不写 HNSW graph。这里的 `snapshot` 是 AOF rewrite 的数据快照概念，不是 anncore graph snapshot。

结论：当前持久化 source of truth 是 records。HNSW graph 没有进入 RDB/AOF。

## 当前 core_hnsw_v2 rebuild 路径分析

`core_hnsw_v2` rebuild adapter 位于 `src/vector/kvs_vector.c`：

```text
kvs_vector_core_hnsw_v2_ops_rebuild()
  -> kvs_hnsw_v2_index_reset()
  -> kvs_vector_configure_core_hnsw_v2()
  -> kvs_hnsw_v2_index_reserve(active_records)
  -> kvs_vector_manager_for_each_active_embedding()
       -> kvs_hnsw_v2_index_add(record_id, embedding, dim)
  -> kvs_hnsw_v2_graph_stats_refresh()
  -> kvs_hnsw_v2_memory_refresh()
```

`kvs_vector_manager_for_each_active_embedding()` 扫描 manager record store，只把 `record_id + embedding + dim` 交给 anncore。anncore 不读取 question、answer、scope、TTL、metadata 或 manager `record_id_map`。

同步 `VINDEX REBUILD` 当前路径是：

```text
kvs_vector_index_rebuild()
  -> kvs_vector_store_compact()
  -> kvs_vector_id_map_rebuild_from_store()
  -> rebuild exact flat / selected backend
  -> core_hnsw_v2: kvs_vector_core_hnsw_v2_ops_rebuild()
  -> kvs_vector_index_bind_current()
  -> update last_rebuild status
```

结论：当前 rebuild 是 records -> derived index 的同步重构。snapshot load 应插在 records 恢复、manager map 重建和 current index bind/rebuild 决策之间，不能替代 records load。

## snapshot 应放在 KVManager 还是 anncore

生命周期应放在 KVManager，graph 序列化能力应放在 anncore。

KVManager 应负责：

- records load / replay 完成后的 manifest 校验。
- backend、dim、metric、HNSW 参数、active_count、generation/checksum 对比。
- snapshot 文件路径、temp file、fsync、atomic rename。
- invalid snapshot discard 和 fallback rebuild。
- snapshot status / error 观测。
- 与 background shadow rebuild 的联动。

anncore 可以提供：

- `serialize_graph()`
- `deserialize_graph()`
- `estimate_snapshot_size()`
- `validate_graph_basic()`

anncore 不应负责：

- RDB/AOF。
- manifest。
- record_generation。
- 文件路径。
- atomic rename。
- fallback policy。
- VINFO response assembly。
- scope / TTL / metadata / question / answer。

## minimal manifest 是否必要

必要。

原因：

- snapshot 是 cache，必须证明它与当前 records/config 匹配。
- `core_hnsw_v2` 默认参数已经变为 balanced：`M=48 / M0=96 / efConstruction=512 / efSearch=1024`，旧参数 snapshot 不能直接加载。
- dim、metric、backend、format version、active_count、generation/checksum 任一不匹配都可能导致 stale graph。
- 裸 snapshot 只能做字节读取，无法决定是否安全 bind 为 active index。

第一版不要求复杂 manifest，但至少需要：

```text
magic
manifest_version
snapshot_format_version
backend
dim
metric
M / M0 / efConstruction / efSearch
active_count / deleted_count
record_generation 或 checksum
snapshot_size
snapshot_checksum
created_at
```

## 是否可以只做 snapshot cache

可以，而且必须先按 cache 语义做。

cache-only 语义：

- snapshot 可删除。
- snapshot 可损坏。
- snapshot 可因参数、records、版本变化失效。
- snapshot load 失败不得影响 records load。
- snapshot write 失败不得影响 active index。
- snapshot invalid 时走 rebuild。

这符合当前 AGENTS.md 和 v2 final architecture 中的 source-of-truth 边界。

## 为什么 snapshot 不能是 source of truth

因为 source of truth 仍是 record store：

- records 保存 question、answer、scope、TTL、metadata、embedding、record_id、record generation。
- manager 负责 active/deleted、scope、threshold 和 response。
- HNSW graph 只保存 index-local graph/search state。
- graph 可以由 active records 重建。

如果把 snapshot 当 source of truth，会引入错误语义：

- snapshot 需要承载 manager semantic state。
- RDB/AOF 与 snapshot 会出现双 source-of-truth。
- stale snapshot 可能返回已删除或过期 records。
- `core_hnsw_v2` 可能被迫理解 manager record lifecycle。

这些都违反 manager/vector_core 边界。

## HNSW-S 路线阶段拆分

```text
HNSW-S0:
  docs-only 项目结构分析和方案设计

HNSW-S1:
  minimal manifest 数据结构和读写工具
  不接 anncore serialize

HNSW-S2:
  core_hnsw_v2 serialize / deserialize graph cache
  test-only roundtrip

HNSW-S3:
  KVManager snapshot validate / load / fallback rebuild
  startup 接线

HNSW-S4:
  rebuild 成功后写 snapshot
  atomic write / checksum / temp rename

HNSW-S5:
  VINFO 暴露 snapshot status
  loaded_from_snapshot / snapshot_valid / last_snapshot_error

HNSW-S6:
  与 background shadow rebuild 联动
  shadow rebuild 成功后生成新 snapshot
```

详细每阶段目标、允许修改、禁止修改、输入依赖、主要文件、必跑测试、成功标准和回退策略见 [implementation_plan.md](implementation_plan.md)。

## 与 HNSW-R / buildperf 的关系

```text
HNSW-R:
  background shadow rebuild / online maintenance

HNSW-S:
  index snapshot / cold-start acceleration

HNSW-buildperf:
  anncore build CPU optimization
```

S 线解决 cold-start 快速可搜索。R 线解决 online rebuild 不阻塞。buildperf 线解决 50 分钟 build CPU 本身太慢。

不能把三条线混在一个阶段：

- S 线不应实现 background worker。
- R 线不应实现 graph persistence。
- buildperf 不应修改 startup / manifest 语义。

## 下一阶段 HNSW-S1 建议

建议进入：

```text
HNSW-S1:
  minimal manifest 数据结构和读写工具
```

HNSW-S1 不应接 anncore serialize / deserialize。先把 manifest schema、checksum、mismatch 判定、temp write / read helper 和 machine-checkable tests 做稳定。

HNSW-S1 需要先确认：

- `g_vector_state.generation` 是否足以代表当前 records 版本。
- RDB load 后 generation 如何恢复或推进。
- 是否需要 `record_id_checksum` / `vector_checksum` 作为 fallback。
- snapshot 文件路径应如何从 config 派生，避免硬编码。

## git diff --check

```text
PASS
```

## git status --short

```text
?? docs/hnsw_index_snapshot/
?? docs/hnsw_productization/hnsw_p5_build_rebuild_bottleneck_analysis.md
```

其中 `docs/hnsw_productization/hnsw_p5_build_rebuild_bottleneck_analysis.md` 是本阶段开始前已有的未跟踪文档，本阶段未修改或处理。

## HNSW-S0 decision

```text
HNSW-S0 decision:
  new_docs_route_created = yes
  production_code_changed = no
  snapshot_cache_only = yes
  minimal_manifest_required = yes
  snapshot_is_source_of_truth = no
  kvmanager_anncore_separation_preserved = yes
  next_stage = HNSW-S1
```
