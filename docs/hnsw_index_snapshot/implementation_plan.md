# HNSW-S implementation plan

## HNSW-S0: docs-only 项目结构分析和方案设计

目标：

- 创建 `docs/hnsw_index_snapshot/`。
- 明确 snapshot 是 cache，不是 source of truth。
- 明确 minimal manifest、启动校验、写入、失败处理和 KVManager / anncore 边界。

允许修改：

- `docs/hnsw_index_snapshot/*`

禁止修改：

- `src/`
- `include/`
- `tests/`
- Makefile
- VSEARCH response
- RDB/AOF 主格式
- `kvs_vector_index_ops_t`
- 默认 HNSW 参数

输入依赖：

- HNSW-P2 100K balanced gate。
- HNSW-P5 rebuild / cold-start 分析。
- current `core_hnsw_v2` final architecture。

主要文件：

- `docs/hnsw_index_snapshot/README.md`
- `docs/hnsw_index_snapshot/snapshot_design.md`
- `docs/hnsw_index_snapshot/implementation_plan.md`
- `docs/hnsw_index_snapshot/hnsw_s0_project_structure_analysis_report.md`

必跑测试：

```bash
git diff --check
git status --short
```

成功标准：

- 新 docs route 建立。
- 不修改生产代码。
- 明确下一阶段 HNSW-S1。

回退策略：

- 删除本目录新增文档即可，不影响代码和持久化数据。

## HNSW-S1: minimal manifest 数据结构和读写工具

目标：

- 增加独立 manifest schema 和读写工具。
- 只处理 manifest，不接 anncore serialize。
- 验证 magic、version、backend、dim、metric、HNSW 参数、active_count、generation/checksum、snapshot_size、snapshot_checksum。

允许修改：

- manager / persistence 辅助模块中新增 snapshot manifest helper。
- 对应 unit test。
- docs 更新。

禁止修改：

- RDB/AOF 主格式。
- `core_hnsw_v2` graph serialize / deserialize。
- VSEARCH response。
- `kvs_vector_index_ops_t`。
- 默认 HNSW 参数。

输入依赖：

- 当前 manager config。
- `g_vector_state.generation` 与 record generation 语义审计。
- 可复用的 checksum/hash helper，如果没有则新增 snapshot-local helper。

主要文件：

- 新增 manager-owned manifest helper。
- 新增 manifest unit test。
- 更新 `docs/hnsw_index_snapshot/*`。

必跑测试：

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
manifest helper tests
git diff --check
```

成功标准：

- manifest roundtrip 通过。
- 不读取或写入 graph。
- manifest mismatch 能被 machine-checkable test 覆盖。

回退策略：

- 关闭 manifest load/write 调用点。
- 删除生成的 temp manifest 文件。

当前实现状态：

- 已新增 `include/vector/kvs_vector_index_snapshot.h` 和 `src/vector/kvs_vector_index_snapshot.c`。
- manifest 第一版使用文本 `key=value` 格式。
- 已提供 `init / write_file / read_file / validate / checksum_file` helper。
- `write_file` 使用 snapshot-local `.tmp` + `rename`，不重构全局文件 IO。
- checksum 使用 deterministic FNV-1a 64-bit，仅用于 cache corruption detection，不是 cryptographic checksum。
- 已新增快速单元测试 `tests/vector_index_snapshot_manifest_test.sh`，并接入默认测试二进制构建。
- 未接入 anncore serialize / deserialize、startup load、rebuild hook、RDB/AOF 或 VSEARCH response。

## HNSW-S2: core_hnsw_v2 serialize / deserialize graph cache

目标：

- 为 `core_hnsw_v2` 增加 index-local serialize / deserialize 能力。
- test-only roundtrip：build graph -> serialize -> destroy -> deserialize -> search / info / graph validate。

允许修改：

- `include/vector/kvs_vector_hnsw_v2.h` 中 v2-only API。
- `src/vector/core/hnsw_v2/*` 中 graph cache serialize / deserialize。
- v2 index-only tests。

禁止修改：

- manager semantic state。
- RDB/AOF 主格式。
- VSEARCH response。
- `kvs_vector_index_ops_t`。
- graph construction 算法。
- HNSW 默认参数。

输入依赖：

- S1 minimal manifest。
- 当前 v2 storage fields：vectors、slot_to_record_id、record_id_to_slot、levels、deleted_flags、level0 graph、upper graph、entry point、max level、size、capacity、params。

主要文件：

- `include/vector/kvs_vector_hnsw_v2_snapshot.h`
- `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_format.h`
- `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_io.c`
- `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_serialize.c`
- `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_deserialize.c`
- `src/vector/core/hnsw_v2_snapshot/kvs_hnsw_v2_snapshot_validate.c`
- `tests/vector_core_hnsw_v2_snapshot_roundtrip_test.c`
- `tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh`

必跑测试：

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_hnsw_v2_default_backend_test.sh
v2 snapshot roundtrip test
boundary grep
git diff --check
```

成功标准：

- roundtrip 后 recall / ordering / graph stats 不回退。
- duplicate/overflow 仍为 0。
- anncore 不访问 manager state。

回退策略：

- 不调用 deserialize 路径，继续 rebuild。

当前实现状态：

- 已新增独立目录 `src/vector/core/hnsw_v2_snapshot/`，snapshot 代码没有混入 add/search/neighbor_select/reverse_prune 主文件。
- 已新增 public header `include/vector/kvs_vector_hnsw_v2_snapshot.h`，只暴露 serialize / deserialize / validate API，不暴露 binary format。
- 已实现 test-only `core_hnsw_v2` graph cache serialize / deserialize roundtrip。
- 已覆盖 empty index、小规模 100 vectors / 32d、search result match、tombstone、same-record update、graph stats、corrupted magic、truncated file、malformed header、deserialize failure target safety。
- deserialize 使用临时 index，全部校验成功后才替换目标 index。
- 未接入 KVManager startup、rebuild success hook、background rebuild、RDB/AOF 或 VSEARCH response。
- S1 manifest schema 未修改，S2 binary header 只属于 anncore snapshot payload。

## HNSW-S3: KVManager snapshot validate / load / fallback rebuild

目标：

- 在 records load / replay 后尝试读取 manifest 和 snapshot。
- manifest valid 时调用 anncore deserialize 并 bind active index。
- invalid 时 fallback flat/degraded mode + trigger rebuild。

允许修改：

- KVManager startup/load 后接线。
- snapshot status manager state。
- fallback rebuild 调度。
- tests。

禁止修改：

- RDB/AOF 主格式。
- VSEARCH response。
- `kvs_vector_index_ops_t`。
- anncore 访问 manager 语义字段。
- background worker 正式实现，除非另一个 HNSW-R 阶段已完成。

输入依赖：

- S1 manifest。
- S2 anncore deserialize。
- 当前 `kvs_vector_rebuild_after_load()` 和 `kvs_vector_index_bind_current()` 路径。

主要文件：

- `src/vector/kvs_vector.c`
- manager snapshot helper。
- startup / reload tests。

必跑测试：

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
RDB/AOF reload tests
snapshot load/fallback tests
git diff --check
```

成功标准：

- valid snapshot 可跳过 full rebuild 并提供相同 search 结果。
- invalid snapshot 不影响 records load。
- fallback rebuild 行为可观测。

回退策略：

- 配置开关禁用 snapshot load。
- 删除 manifest/snapshot 后回到 records-only rebuild。

当前实现状态：

- 已新增默认路径 helper：`vector_index.manifest` / `vector_index.snapshot`。
- 已新增 manager-owned snapshot status，记录 attempt / loaded / valid / loaded_from_snapshot / fallback count / last path / last error。
- 已在 `kvs_vector_rebuild_after_load()` 中接入只读 snapshot load 尝试。
- 仅当 selected backend 是 `core_hnsw_v2` 时尝试 snapshot；`core_flat` 和 removed compatibility fallback 路径不尝试。
- expected manifest 由 manager 扫描当前 record store 构造，包含 backend、dim、metric、v2 参数、active/deleted count、records generation fallback、record_id checksum、vector checksum、snapshot file size/checksum。
- `record_generation` 使用当前 records 的最大 generation；报告中明确说明 `g_vector_state.generation` 在 load/replay 后不作为唯一可靠判据，record_id/vector checksum 是实际防线。
- manifest 和 snapshot 文件校验成功后调用 S2 `kvs_hnsw_v2_index_deserialize_file()`。
- deserialize 成功后，manager 再校验 loaded graph active slot 数量、record_id 回表、embedding dim 和 vector bytes 与当前 records 一致。
- snapshot 成功后直接 bind v2 active index，不重复 full rebuild。
- missing / mismatch / corrupt / deserialize failure 均 fallback 到原 records-only rebuild。
- 本阶段没有实现 snapshot writer、rebuild success hook、background rebuild、GC 或 RDB/AOF graph persistence；snapshot writer 已在 HNSW-S4 单独实现。
- 已新增 `tests/vector_index_snapshot_load_after_records_test.*` 覆盖 valid load、missing manifest、param mismatch、checksum mismatch、corrupt snapshot、deserialize failure、non-v2 backend 和 no double rebuild。

## HNSW-S4: rebuild 成功后写 snapshot

目标：

- full rebuild 或 background shadow rebuild 成功后生成新 snapshot。
- 使用 temp file + checksum + atomic rename。

允许修改：

- manager snapshot lifecycle。
- anncore serialize 调用点。
- file write / fsync / rename helper。
- tests。

禁止修改：

- RDB/AOF 主格式。
- VSEARCH response。
- graph construction 算法。
- 默认 HNSW 参数。

输入依赖：

- S1 manifest writer。
- S2 serialize。
- S3 load/fallback。

主要文件：

- manager snapshot helper。
- rebuild success hook。
- snapshot write tests。

必跑测试：

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
snapshot write atomicity tests
RDB/AOF reload tests
v2 regression tests
git diff --check
```

成功标准：

- 写失败不影响 active index。
- 正常错误路径会尽量通过 `.bak` rollback 保留旧 snapshot；崩溃窗口仍按 cache 语义由 startup validation 兜底。
- temp 文件残留可安全清理。

回退策略：

- 禁用 snapshot writer。
- 保留 snapshot reader fallback rebuild。

当前实现状态：

- 已新增 manager-owned writer：`kvs_vector_index_snapshot_write_current()`。
- writer 只在 active backend 是 `core_hnsw_v2` 时执行，`core_flat` 不写 snapshot。
- anncore 只负责 `kvs_hnsw_v2_index_serialize_file()` 和 `kvs_hnsw_v2_index_validate_basic()`；manifest 路径、records checksum、fallback policy 均在 manager 层。
- 写入流程：
  1. validate active v2 graph。
  2. serialize 到 `vector_index.snapshot.tmp`。
  3. 基于当前 manager records/config 和 tmp snapshot size/checksum 构造 manifest。
  4. 写 `vector_index.manifest.tmp`。
  5. 发布前把已有正式 snapshot/manifest 备份到 `.bak`。
  6. rename tmp snapshot 到 `vector_index.snapshot`。
  7. rename tmp manifest 到 `vector_index.manifest`。
  8. 成功后清理 `.bak`；失败时尽量 rollback `.bak` 并清理 tmp。
- 已接入两个 rebuild 成功路径：
  - `kvs_vector_rebuild_after_load()` snapshot load 失败后的 records-only fallback rebuild 成功后 best-effort 写 snapshot。
  - `kvs_vector_index_rebuild()` / VINDEX REBUILD 成功后 best-effort 写 snapshot。
- snapshot load 成功路径不重复写 snapshot。
- snapshot 写失败只更新 internal status / warning log，不改变 rebuild 返回值，不影响 active index。
- 已扩展 internal snapshot status：`snapshot_write_attempted`、`snapshot_written`、`last_snapshot_write_error`、`last_snapshot_write_started_ms`、`last_snapshot_write_finished_ms`、`last_snapshot_bytes`、`last_snapshot_checksum`。
- 已新增 `tests/vector_index_snapshot_write_after_rebuild_test.*`，覆盖 fallback 写入、下次 load、missing/corrupt 后重写、显式 VINDEX rebuild、non-v2 不写、写失败不阻塞 rebuild、active search 保持、旧 snapshot 保留和 leftover tmp 被忽略。
- 未实现 background shadow rebuild writer、snapshot GC、多版本 snapshot、mmap、graph mutation AOF、VINFO snapshot 字段或 snapshot admin command。

## HNSW-S5: VINFO 暴露 snapshot status

目标：

- 在 VINFO 中暴露 snapshot 状态，但不改变 VSEARCH response。
- 字段覆盖 snapshot load、valid、fallback rebuild、writer attempt、writer success、writer error、last bytes 和 checksum。

允许修改：

- manager info struct。
- VINFO response array count。
- VINFO/profile tests。

禁止修改：

- VSEARCH response。
- RDB/AOF 主格式。
- anncore manager 语义访问。

输入依赖：

- S3/S4 snapshot load/write status。

主要文件：

- `include/kvs_vector.h`
- `src/vector/kvs_vector.c`
- `src/core/kvs_operation.c`
- tests。

必跑测试：

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
VINFO machine-checkable tests
RDB/AOF reload tests
git diff --check
```

成功标准：

- snapshot status 可观测。
- VSEARCH response 不变。
- fields 不进入 RDB/AOF。

回退策略：

- 保留 internal status，移除 VINFO 输出字段。

当前实现状态：

- 已在 `kvs_vector_info_t` 中新增 manager-owned snapshot status 字段。
- `kvs_vector_info()` 仅当当前 backend 是 `core_hnsw_v2` 时从 `g_vector_state.snapshot_status` 拷贝状态。
- `core_flat` / non-v2 backend 明确返回 `snapshot_supported=0`，load/write attempt 为 0，error 字段为空。
- `VINFO` 新增 14 个 machine-checkable 字段：
  - `snapshot_supported`
  - `snapshot_load_attempted`
  - `snapshot_loaded`
  - `snapshot_valid`
  - `loaded_from_snapshot`
  - `snapshot_fallback_rebuild_count`
  - `snapshot_last_error`
  - `snapshot_write_attempted`
  - `snapshot_written`
  - `snapshot_last_write_error`
  - `snapshot_last_write_started_ms`
  - `snapshot_last_write_finished_ms`
  - `snapshot_last_bytes`
  - `snapshot_last_checksum`
- 第一版不暴露完整 manifest / snapshot 路径。
- 已新增 `tests/vector_index_snapshot_vinfo_status_test.*`，覆盖 missing snapshot fallback、valid snapshot load、corrupt fallback、writer success、writer failure、non-v2 semantics 和 product VINFO path。
- 已更新 `tests/vector_vinfo_backend_stats_test.sh`，要求所有 backend stats 检查包含 snapshot status keys。
- 未修改 `VSEARCH` response、RDB/AOF、`kvs_vector_index_ops_t`、默认 backend、默认 HNSW 参数、snapshot load/fallback/writer 语义。

## HNSW-S6: 与 background shadow rebuild 联动

目标：

- shadow rebuild 成功发布后生成新 snapshot。
- active / shadow / retired 生命周期与 snapshot 写入顺序协调。

允许修改：

- HNSW-R 与 HNSW-S 的 manager-owned orchestration glue。
- snapshot write policy。
- tests。

禁止修改：

- graph construction refactor。
- SIMD/layout/reverse_prune cache 优化。
- RDB/AOF 主格式。
- VSEARCH response。

输入依赖：

- HNSW-R reader safety / active-shadow-retired ownership。
- S4 snapshot writer。

主要文件：

- rebuild context integration。
- snapshot lifecycle helper。
- shadow rebuild tests。

必跑测试：

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_rebuild_state_machine_test.sh
tests/vector_rebuild_delta_capture_test.sh
tests/vector_rebuild_shadow_replay_test.sh
tests/vector_rebuild_shadow_swap_test.sh
snapshot shadow integration tests
git diff --check
```

成功标准：

- shadow publish 成功后新 snapshot 可用。
- publish 失败或 snapshot 写失败不破坏 old active index。
- restart 后可加载最新 valid snapshot。

回退策略：

- 禁用 shadow rebuild snapshot writer。
- 保留手动/full rebuild 后 snapshot 写入。

当前实现状态：

- 当前仓库仍只有 9F 系列 shadow rebuild skeleton / test harness，没有完整 product background rebuild worker。
- shadow publish 成功点是 `kvs_vector_rebuild_swap_ready_shadow_for_test()` 调用 `kvs_vector_rebuild_promote_shadow()` 后，`shadow_index` 被移动为 rebuild context 的 `active_index`，旧 active 进入 `retired_index`。
- S6 在该 test/internal publish 成功点之后接入 best-effort snapshot writer。
- writer 支持指定已 promote 的 `core_hnsw_v2` index 指针写出 graph cache；原 `kvs_vector_index_snapshot_write_current()` 仍写产品 `current_index` 对应的 `g_vector_state.core_hnsw_v2_index`。
- 当前 backend 不是 `core_hnsw_v2` 时，shadow publish 不写 snapshot。
- snapshot 写失败只更新 S5 write status / warning，不回滚 promote，不影响 rebuild active search。
- 写出的 snapshot 可由 S3 startup/load path 校验并加载。
- 未实现 background worker、reader safety、product active pointer publication、snapshot GC、多版本 snapshot、mmap、RDB/AOF graph persistence、admin command 或 buildperf。

## HNSW-S-final: 路线收口与风险审计

目标：

- 不新增 snapshot 功能。
- 汇总 HNSW-S1 到 S6 的能力边界。
- 审计 source-of-truth、RDB/AOF、VSEARCH response、`kvs_vector_index_ops_t` 和 KVManager / anncore 边界。
- 明确当前风险和后续路线分流。

允许修改：

- `docs/hnsw_index_snapshot/README.md`
- `docs/hnsw_index_snapshot/index.md`
- `docs/hnsw_index_snapshot/implementation_plan.md`
- `docs/hnsw_index_snapshot/snapshot_design.md`
- `docs/hnsw_index_snapshot/hnsw_s_final_consolidation_report.md`

禁止修改：

- `src/`
- `include/`
- `tests/`
- Makefile
- RDB/AOF 主格式
- `VSEARCH` response
- `kvs_vector_index_ops_t`
- 默认 backend
- 默认 HNSW 参数

必跑测试：

```bash
make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
tests/vector_index_snapshot_manifest_test.sh
tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh
tests/vector_index_snapshot_load_after_records_test.sh
tests/vector_index_snapshot_write_after_rebuild_test.sh
tests/vector_index_snapshot_vinfo_status_test.sh
tests/vector_index_snapshot_shadow_publish_test.sh
tests/vector_rebuild_state_machine_test.sh
tests/vector_rebuild_delta_capture_test.sh
tests/vector_rebuild_shadow_replay_test.sh
tests/vector_rebuild_shadow_swap_test.sh
tests/vector_core_hnsw_v2_default_backend_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh
tests/vector_vinfo_backend_stats_test.sh
tests/vector_core_flat_rdb_reload_test.sh
boundary grep
git diff --check
git status --short
```

成功标准：

- HNSW-S1 到 S6 能力矩阵清晰。
- snapshot minimal loop 明确完成。
- snapshot 仍明确是 cache，不是 source of truth。
- records / RDB / AOF 仍明确是 source of truth。
- 当前风险不被包装成生产 SLA。
- 后续不继续扩 S 线功能，而是分流到 HNSW-R、HNSW-buildperf、real/proxy embedding benchmark 或 future S 子路线。

当前实现状态：

- HNSW-S1 到 S6 已形成第一版 minimal cold-start snapshot loop。
- 当前 snapshot 可以在 records 恢复后加载 valid `core_hnsw_v2` graph cache；invalid / missing / corrupt snapshot 会 fallback records-only rebuild。
- rebuild、VINDEX REBUILD 和 shadow publish skeleton 成功后会 best-effort 写下一次可用 snapshot。
- VINFO 可观测 load / fallback / write status。
- 未实现 snapshot GC、多版本 snapshot、mmap/view、admin command、background worker、reader safety、graph mutation AOF 或 buildperf。
