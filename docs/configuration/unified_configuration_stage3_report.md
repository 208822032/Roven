# Unified Configuration Stage 3 实现报告

## 1. 范围与结果

本阶段将 vector、embedding、vector snapshot、vector rebuild 及其直接性能统计开关迁移到 Stage 1/2 的最终 effective `g_cfg`。生产代码和测试有修改；公开 V 命令格式、正常响应结构、RDB/AOF 格式、HNSW 算法、manager/core ownership 和 `kvs_vector_index_ops_t` 未修改。

最终消费链：

```text
defaults -> one config parse -> one CLI merge -> derive paths/M0/model identity
-> validate once -> publish g_cfg
-> vector/embedding/snapshot/rebuild read g_cfg only
```

## 2. Vector 与 embedding

- `vector.enabled=false` 时不初始化 embedding provider、record store、flat/HNSW index、snapshot 或 rebuild worker；VSET/VGET/VSEARCH/VDEL/VINDEX/VINFO 使用已有错误响应明确拒绝。
- 正式配置只接受 `core_flat/core_hnsw_v2`。legacy 低 HNSW 参数可按兼容规则归一化并告警；canonical M/M0/efConstruction/efSearch/initial capacity 不再在 runtime 二次 clamp。
- hash provider 使用 `vector.dimension`，不再固定 64 维。`embedding.output_dimension` 必须等于 index dimension；当前 normalize=true 是固定语义契约。
- local_c 在启动期验证 ONNX capability、model/vocab 可读性、配置维度以及 ONNX 输出 tensor 最后一维。model/vocab 相对 config_root，max_text_bytes 与 max_tokens 分离。
- worker pool、ORT intra/inter threads、execution mode、graph optimization、cache enable/capacity/TTL/lru policy 均读取 effective config。旧 `KVLITE_EMBEDDING_PROFILE` 读取已移除。
- 当前没有面向产品命令的完整 batch consumer；所有 `embedding_batch_*` 保持 unsupported，显式配置失败，正式示例不再列出这些字段。

## 3. 模型身份和恢复契约

启动时一次性计算 64-bit model content identity，输入包括 provider、model file content hash、vocab file content hash、output dimension、normalize 契约和 model-version label。查询路径不重复计算文件 hash；相同路径内容变化会改变 identity。

新记录继续写入现有 32-bit `embedding_model_version` 字段，因此 RDB/AOF 二进制/命令格式未变。恢复时验证 active record 的 dimension、finite、单位范数和 model identity；Stage 3 之前的 provider/path/dim legacy identity 可识别并告警接受。RDB record 本身没有历史 metric 字段，在“不修改 RDB 格式”的边界下无法单独证明旧记录最初使用的 metric；snapshot 可完整校验 metric。

## 4. Snapshot

manifest/index 路径相对 `server.data_dir` 解析并在启动前检查冲突、类型和权限；不再依赖 cwd。未显式配置 vocab 的 local_c legacy profile 只允许从 model 同目录推导一次并告警。旧 cwd snapshot 不自动移动或删除。

`enabled/load_on_start/invalid_action/write_after_rebuild` 已接线：disabled 不加载/写入；`fail` 拒绝缺失、损坏或不兼容 snapshot；`rebuild` 从 records 重建；发布后的 snapshot 写失败仍为 best-effort，不回滚已发布 index。`cross_node_reuse=true` 当前明确失败。

snapshot manifest 从 v1 升级到 v2，新增 model content identity；backend、dimension、metric、HNSW 参数、records/vector checksum 和模型身份全部参与校验。旧 manifest 按 invalid_action 处理。HNSW graph 仍是 derived state，不进入 RDB/AOF。

## 5. Rebuild 与观测

- `async_enabled=false` 拒绝 ASYNC/RETRY；`cancel_enabled=false` 拒绝 CANCEL；retry policy 只接受当前实现的 manual。
- delta buffer 从配置的 initial capacity 增长，但不超过 max entries；达到上限时 append 失败并将 rebuild 置为 FAILED，不静默丢 delta。
- `write_snapshot_after_publish` 和 snapshot write policy 控制发布后写入；失败不回滚 active index。
- vsearch stage、embedding、HNSW build/search profiling 均由 effective config 控制。正式默认关闭 HNSW wall-clock profiling；VINFO 保持原 schema，disabled snapshot 通过 `snapshot_supported=0` 表示，未新增响应字段。

## 6. 正确性与兼容边界

保持不变：公开 RESP、RDB/AOF 格式、record store source-of-truth、manager filtering/response ownership、core input/output、HNSW graph/search/termination、复制语义。`initial_capacity` 已真实进入 reserve，但 destructive reserve 和跨容量边界原子性不在本阶段修复，下一阶段单独处理。

非 snapshot 产品测试的临时配置显式关闭 snapshot；snapshot 专项改用独立临时 manifest/index 路径，不再备份或改写项目根目录文件。

## 7. 测试结果

已执行并通过：

```text
strict C11 config compile
tests/unified_config_stage1_test.sh
tests/unified_config_stage2_test.sh
tests/unified_config_stage3_test.sh                 # includes ASan/UBSan
tests/unified_config_stage3_runtime_test.sh
tests/unified_config_stage3_vector_disabled_test.sh
tests/unified_config_stage3_onnx_test.sh             # semantic + output-dim/model mismatch

tests/vector_core_flat_correctness_test.sh
tests/vector_core_flat_rebuild_test.sh
tests/vector_core_flat_rdb_reload_test.sh
tests/vector_core_hnsw_v2_skeleton_test.sh
tests/vector_core_hnsw_v2_graph_construction_test.sh
tests/vector_core_hnsw_v2_search_recall_test.sh
tests/vector_core_hnsw_v2_product_integration_test.sh

tests/vector_index_snapshot_manifest_test.sh
tests/vector_index_snapshot_load_after_records_test.sh
tests/vector_index_snapshot_write_after_rebuild_test.sh
tests/vector_index_snapshot_vinfo_status_test.sh
tests/vector_index_snapshot_shadow_publish_test.sh
tests/vector_rebuild_state_machine_test.sh
tests/vector_rebuild_delta_capture_test.sh
tests/vector_rebuild_shadow_replay_test.sh
tests/vector_rebuild_shadow_swap_test.sh
tests/vector_rebuild_async_worker_test.sh
tests/vector_command_persistence_recovery_test.sh     # RDB/AOF/RDB+AOF
tests/vector_vsearch_profile_breakdown_test.sh
```

完整 `make clean` + FAISS/HNSWLIB 构建和 `make embedding` 均通过。HNSW v2 recall 脚本最终退出码为 0：1K/5K recall@10 均为 1.0000，10K sweep 在 efSearch=512/1024 为 1.0000。最终静态边界检查确认 `src/vector/core` 不引用 `g_vector_state`、scope、TTL、metadata、question、answer 或 manager-owned `record_index`；`git diff --check` 通过，无残留 kvstore/test 进程。

## 8. 文件与下一阶段

主要生产文件：`include/kvs_config.h`、`src/core/kvs_config.c`、vector manager/embedding/snapshot/rebuild、embedding C API/ONNX runtime，以及 server/command executor 的 enabled gate。新增四组 Stage 3 专项测试；更新 snapshot fixture、统一配置审计、示例和开发清单。

下一阶段严格保持既定路线：

```text
HNSW capacity safety gate
```

Configuration-4、Replication-1、eBPF 和普通日志配置迁移均未提前开始。

```text
vector_config_migrated=yes
embedding_config_migrated=yes
embedding_dimension_gate_verified=yes
model_content_identity_implemented=yes
hnsw_parameter_validation_strict=yes
dead_batch_configuration_resolved=yes
snapshot_config_migrated=yes
rebuild_config_migrated=yes
vector_observability_config_migrated=yes
development_checklist_updated=yes
ready_for_hnsw_capacity_safety_gate=yes
```
