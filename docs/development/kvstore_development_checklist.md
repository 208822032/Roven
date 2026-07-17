# KVLite 后续开发能力清单与路线跟踪

本文档是 KVLite 当前能力、剩余任务、阶段依赖和验收状态的长期唯一跟踪入口。它只维护当前有效路线，不替代各阶段设计报告，也不把历史报告中的结论自动视为当前完成状态。

当前系统主线：

```text
RESP 客户端
  -> 统一命令执行框架
  -> 普通 KV / 向量命令
  -> 持久化记录
  -> 向量管理层
  -> KVLite HNSW 向量索引
  -> RDB/AOF、快照、重建和主从同步
```

数据真相边界：

```text
产品记录是持久化数据依据。
HNSW 图和索引快照属于可重建的派生状态。
```

向量管理层拥有产品记录、key/scope、TTL、metadata、question/answer、过滤、响应组装、持久化、复制和重建编排。向量索引核心只拥有 `record_id`、vector、score 和索引局部状态。后续阶段不得改变该边界，不得把 HNSW 图写入 RDB/AOF，也不得修改已冻结的公开 `VSEARCH` 响应和 `kvs_vector_index_ops_t` 签名。

## 状态标记

- `[x]` 已实现并通过当前可执行测试。源码、配置字段或历史文档单独存在，不足以标记为完成。
- `[~]` 已有基础实现，但存在缺陷、缺少当前测试证据或尚未达到阶段门禁。
- `[ ]` 尚未实现，或目标阶段尚未开始。

本次状态基线为 2026-07-17。完成项引用当前仓库内仍可执行的测试；仅有历史阶段报告而专项测试已删除的能力，必须再由现行产品测试支撑，否则只能标记为 `[~]`。

## 文档维护规则

1. 每完成一个阶段，必须同步更新状态和验证证据。
2. 没有测试证据不得标记 `[x]`。
3. 历史报告只能作为设计证据，不能替代当前可执行测试。
4. 新任务必须挂到本文已有主线，不能在文档外单独扩张。
5. 发现新问题时，先归类到现有章节，再决定是否需要新阶段。
6. 改变阶段顺序时，必须记录原因、前置依赖和受影响门禁。
7. 本文只维护当前有效路线，不堆叠过期临时方案。
8. 每个阶段结束必须更新修改文件、测试、已知风险和下一阶段门禁。
9. `[x]` 项发生回归或证据失效时，必须立即降级为 `[~]`。
10. Benchmark、配置、复制和向量主线必须分别归档证据，不得用一个主线的结果代替另一个主线的正确性门禁。

## 基线证据索引

- 项目与产品架构：`README.md`、`docs/route_c/route_c_final_report.md`。
- 统一配置现状与冻结 schema：`docs/configuration/unified_configuration_audit.md`、`docs/configuration/kvstore.conf.example`。
- V 族功能和持久化：`docs/vector_command_functionality_audit.md`、`tests/vector_command_persistence_recovery_test.sh`。
- HNSW snapshot：`docs/hnsw_index_snapshot/hnsw_s_final_consolidation_report.md`。
- HNSW rebuild：`docs/hnsw_rebuild_productization/hnsw_r_final_product_rebuild_consolidation_report.md`。
- 真实向量与产品 Benchmark：`docs/vector_benchmark_readiness/`，重点使用 B-scale-6 与 B-scale-7 报告。
- 主从同步现状与冻结设计：`docs/replication/vector_replication_stage0_audit.md`。
- manager/core 所有权与稳定约束：根目录 `AGENTS.md`。

上述文档用于解释设计和已知边界；状态升级仍必须引用当前仓库内可执行测试或新增阶段测试结果。

## 当前能力总览

### 普通 KV 与统一命令体系

- [x] `protocol.resp`：RESP 请求解析、长度传递和响应编码。
  - 实现位置：`src/protocol/`、`src/network/`、`src/core/kvs_operation.c`。
  - 已知边界：公开命令仍是 KVLite 当前 RESP 子集，不声明完整 Redis 协议兼容。
  - 验证：`tests/test_binary_key.c`、`tests/testcase.c`；Pipeline 路径由 `tests/testcase.c` 的批量响应解析覆盖。

- [x] `storage.four_engines`：array、rbtree、hash 和 skiptable 四种 KV 数据结构共用命令语义。
  - 实现位置：`src/core/`、四类存储实现及 `src/core/kvs_operation.c`。
  - 验证：`tests/test_binary_key.c`、`tests/ttl/test_ttl_all_backends.sh`。
  - 门禁：四类 backend 的 SET/GET/MOD/EXIST/DEL 和二进制 key/value 回归持续通过。

- [x] `ttl.lifecycle`：TTL 设置、查询、取消、惰性过期、后台扫描及持久化恢复。
  - 实现位置：`src/core/kvs_ttl*.c`、持久化和命令执行路径。
  - 验证：`tests/ttl/test_ttl_mvp.sh`、`tests/ttl/test_ttl_all_backends.sh`、`tests/ttl/test_ttl_rdb.sh`、`tests/ttl/test_ttl_aof.sh`。

- [x] `protocol.pipeline`：单连接 Pipeline 批量解析、执行和多响应返回。
  - 实现位置：RESP parser、三个网络模型和统一命令任务执行路径。
  - 验证：`tests/testcase.c` Pipeline mode；`docs/binary_safe_key_value_review.md` 记录 100 万命令验证结果。

- [x] `persistence.commands`：`AOFREWRITE` 和 `FULLDUMP` 进入统一命令表和 executor 所有权模型。
  - 实现位置：`src/core/kvs_operation.c`、`src/core/kvs_command_executor.c`、`src/persistence/`。
  - 验证：TTL AOF/RDB suites、`tests/testcase.c` 及 Route C 产品回归证据。

- [x] `commands.vector_family`：`VSET/VGET/VSEARCH/VDEL/VINDEX/VINFO` 已接入公开命令体系。
  - 验证：`tests/vector_core_hnsw_v2_product_integration_test.sh`、`tests/vector_command_persistence_recovery_test.sh`、`tests/vector_vinfo_backend_stats_test.sh`。

- [x] `executor.unified_path`：普通 KV、TTL、持久化和向量命令共用 RESP 解析、命令任务、单 executor 提交与响应框架。
  - 实现位置：`src/core/kvs_command_task.c`、`src/core/kvs_command_executor.c`、`src/core/kvs_operation.c`。
  - 已知边界：当前弱锁模型依赖单 executor；未来多 executor 必须重新设计同步。
  - 验证：现行 TTL、持久化和向量产品测试；Route C 专项脚本已删除，历史报告只保留为架构证据。

### 向量检索能力

- [x] `vector.embedding.text`：文本生成确定性 hash 向量，并可选使用本地 tokenizer/ONNX Runtime 生成真实向量。
  - 实现位置：`src/vector/kvs_vector_embedding.c`、`src/embedding/embedding_runtime_onnx.c`。
  - 验证：`tests/vector/semantic_onnx_test.sh`、`tests/vector_real_embedding_cache_smoke_test.sh`、B-scale-7 public text 路径的 `runtime_encode_count` 证据。

- [x] `vector.index.exact`：精确向量索引作为 exact recall oracle、正确性基线和降级后端。
  - 产品名称：精确向量索引；内部配置值和路径可保留 `core_flat`。
  - 验证：`tests/vector_core_flat_correctness_test.sh`、`tests/vector_core_flat_rebuild_test.sh`、`tests/vector_core_flat_rdb_reload_test.sh`。

- [x] `vector.index.kvlite_hnsw`：KVLite HNSW 向量索引支持 add/search/mark-delete/stats/rebuild/snapshot。
  - 产品名称：KVLite HNSW 向量索引；内部配置值、源码路径和测试名可保留 `core_hnsw_v2`。
  - 验证：`tests/vector_core_hnsw_v2_graph_construction_test.sh`、`tests/vector_core_hnsw_v2_search_recall_test.sh`、`tests/vector_core_hnsw_v2_product_integration_test.sh`。

- [x] `vector.manager.filtering`：scope、threshold、active/deleted 和 stale `record_id` 过滤留在 manager。
  - 实现位置：`src/vector/kvs_vector.c`、`src/vector/kvs_vector_index.c`。
  - 已知边界：slot 复用后必须继续验证 `record->id == hit.record_id`。
  - 验证：`tests/vector_manager_resolve_fastpath_test.sh`、product integration 和持久化恢复测试。

- [x] `vector.rebuild.sync`：同步扫描 active records 并重建派生索引。
  - 验证：`tests/vector_core_flat_rebuild_test.sh`、`tests/vector_rebuild_state_machine_test.sh`、product integration。

- [x] `vector.rebuild.async`：单次后台影子重建，支持 status/cancel/retry。
  - 实现位置：`src/vector/kvs_vector_rebuild*.c`、manager rebuild state。
  - 已知边界：one-shot worker；VSET/VDEL 与 worker guard 仍保守串行，高并发公平性未验证。
  - 验证：`tests/vector_rebuild_async_worker_test.sh`、`tests/vector_command_persistence_recovery_test.sh`。

- [x] `vector.rebuild.publish_model`：当前索引、影子索引和待回收索引的 reader-safe 发布模型。
  - 验证：`tests/vector_rebuild_shadow_swap_test.sh`、`tests/vector_index_snapshot_shadow_publish_test.sh`。

- [x] `vector.rebuild.delta_publish`：重建期间捕获 VSET/VDEL 增量，重放、最终校验并原子发布；失败时保留旧 active index。
  - 验证：`tests/vector_rebuild_delta_capture_test.sh`、`tests/vector_rebuild_shadow_replay_test.sh`、`tests/vector_rebuild_shadow_swap_test.sh`。

- [x] `vector.snapshot.lifecycle`：索引快照加载、manifest/checksum/records 校验，以及缺失、损坏、不兼容时从 records 回退重建。
  - 已知边界：snapshot 是 derived cache；尚无多版本 GC、mmap serving 或完整 fsync crash-consistency。
  - 验证：`tests/vector_index_snapshot_manifest_test.sh`、`tests/vector_core_hnsw_v2_snapshot_roundtrip_test.sh`、`tests/vector_index_snapshot_load_after_records_test.sh`。

- [x] `vector.snapshot.after_publish`：同步重建和异步发布后 best-effort 写快照，写失败不回滚已发布索引。
  - 验证：`tests/vector_index_snapshot_write_after_rebuild_test.sh`、`tests/vector_index_snapshot_shadow_publish_test.sh`。

- [x] `vector.vinfo.observability`：VINFO 暴露 records/index、backend capacity/memory source、搜索阶段、重建和快照状态。
  - 验证：`tests/vector_vinfo_backend_stats_test.sh`、`tests/vector_vsearch_profile_breakdown_test.sh`、`tests/vector_index_snapshot_vinfo_status_test.sh`。

- [x] `vector.benchmark.product_100k`：10 万条真实文本向量完成产品 VSET/VSEARCH/VINFO 端到端验证。
  - 验证：`docs/vector_benchmark_readiness/b_scale_7_t2ranking_100k_product_e2e_report.md` 及其 runner validator；`tests/vector_t2ranking_product_e2e_smoke_test.sh` 保留可执行 smoke gate。
  - 已知边界：单实例、单客户端、单次正式 load，不是并发 SLA。

- [x] `vector.benchmark.faiss_index_only`：KVLite HNSW 与 FAISS HNSW 完成 10 万条纯索引 recall/latency/build 对比。
  - 验证：`docs/vector_benchmark_readiness/b_scale_6_t2ranking_100k_hnsw_faiss_index_only_report.md`、`tests/vector_real_embedding_hnsw_faiss_sweep_smoke_test.sh`。
  - 已知边界：FAISS 没有产品 backend；其 runtime memory 为 unsupported，不得与 KVLite measured/estimated memory 直接比较。

### V 族持久化验证

- [x] `vector.persistence.rdb_only`：仅保留 RDB 后恢复向量记录、embedding、ID 映射和查询。
  - 验证：`tests/vector_command_persistence_recovery_test.sh`、`tests/vector_core_flat_rdb_reload_test.sh`。

- [x] `vector.persistence.aof_only`：删除 RDB/快照后，仅由 AOF 文本 VSET/VDEL 重放恢复。
  - 已知边界：`local_c` 会重新运行 tokenizer/ONNX，不复用冻结 embedding。
  - 验证：`tests/vector_command_persistence_recovery_test.sh`。

- [x] `vector.persistence.rdb_aof`：RDB 后重放 AOF，更新和删除最终态正确且不产生重复 active records。
  - 验证：`tests/vector_command_persistence_recovery_test.sh`。

- [x] `vector.persistence.query_parity`：恢复后 scope、VGET、VSEARCH 和 records/index 数量一致。
  - 验证：`tests/vector_command_persistence_recovery_test.sh`。

- [x] `vector.persistence.snapshot_fallback`：有效快照加载；缺失、损坏、checksum 或参数不兼容时回退 records rebuild。
  - 验证：`tests/vector_index_snapshot_vinfo_status_test.sh`、`tests/vector_index_snapshot_load_after_records_test.sh`。

- [~] `vector.write.failure_atomicity`：VSET 内部分配失败和 index add/commit 失败的回滚路径已有实现。
  - 缺口：只有参数错误和 rebuild delta append 失败测试，没有 malloc/index-add 故障注入。
  - 预期验证：分别注入 record/string/vector 分配失败和 backend add 失败，检查 logical key、record store、ID maps、active index、delta 和 AOF/复制入队均无半提交。
  - 门禁：所有注入点保持 records/index 一致，失败写不进入持久化或复制成功序列。

## HNSW 容量与写入安全

该主线是进入高并发和主从复制长期验证的重要前置安全项。Replication-1 可以在冻结的失败语义下接入，但 replica apply 遇到 capacity/resize/OOM 失败时必须立即停止，不推进 `applied_offset`、不发送 ACK；容量失败不得被当作可忽略的单条写失败。

- [~] `hnsw.capacity.initial_config`：`vector_hnsw_initial_capacity` 已进入 manager store 和当前 HNSW index 的首次 reserve。
  - 已有：B-scale-7 修复了配置只 parse/log、不实际 reserve 导致 100K 图被中途清空的问题。
  - 缺口：未知或超过预留规模时仍可能触发 destructive reserve；统一配置迁移尚未完成。
  - 验证：B-scale-7 的 `active_records == size == active_count == capacity == 100000` 门禁。

- [ ] `hnsw.capacity.no_destructive_reserve`：禁止对已有图执行 destructive reserve。
  - 目标：reserve 不得在成功返回后丢失旧节点、边、deleted flags 或 ID/slot 映射。
  - 实现位置：KVLite HNSW index reserve/add 边界，具体文件由阶段审计冻结。
  - 验证：小初始容量连续跨越多个扩容点，逐次检查全量 ID、search recall 和图统计。
  - 门禁：任何保留旧数据的 reserve 测试失败都不得进入复制并发验证。

- [ ] `hnsw.capacity.lossless_growth_or_fail`：实现无损扩容，或在写入前明确失败且保持旧状态。
  - 前置条件：禁止 destructive reserve。
  - 验证：成功扩容与明确拒绝两条路径；失败前后 graph checksum、records/index count 和 VSEARCH 结果一致。

- [ ] `hnsw.capacity.atomic_overflow_write`：超容量 VSET 不产生 records/index 分裂。
  - 验证：new insert、覆盖更新、slot 复用、rebuild delta capture 和 AOF/复制 post-commit 分别覆盖。
  - 门禁：返回失败时记录、映射、索引、delta、持久化和复制流均保持一致。

- [ ] `hnsw.capacity.alloc_fault_injection`：覆盖 record、字符串、vector、map、图和临时 workspace 分配失败。
  - 前置条件：建立可重复、可定位的 allocator fault hook，不用系统 OOM 代替确定性测试。
  - 门禁：每个注入点都能证明无泄漏、无半提交、旧查询仍可用。

- [ ] `hnsw.capacity.index_add_fault_injection`：注入索引 add/link/reserve 提交失败。
  - 验证：新增和覆盖 VSET、async rebuild delta、replica mutation apply。
  - 门禁：失败不推进产品 commit sequence 或 replica offset。

- [ ] `hnsw.capacity.lifecycle_consistency`：更新、删除、同步/异步重建、RDB/AOF 恢复和 snapshot fallback 后容量状态一致。
  - 验证：低初始容量下串联 update/delete/rebuild/restart，再核对 active/deleted/capacity/record IDs。

- [ ] `hnsw.capacity.vinfo`：VINFO 完整报告 capacity、无损扩容次数、reserve 次数和最近失败状态。
  - 已有基础：backend capacity 和 memory source 已可观测；resize/reserve counter 允许 unsupported，失败状态尚未冻结。
  - 门禁：不把 unsupported 伪装为 0；计数和实际触发点可由测试机器检查。

- [ ] `hnsw.capacity.low_initial_100k_gate`：10 万条、低初始容量的产品回归门禁。
  - 前置条件：无损扩容/明确失败、故障注入和 VINFO 语义完成。
  - 验证：多次跨容量边界，`records == index size == active count`，recall 不低于冻结基线，无中途图重置。

## 统一配置能力

`docs/configuration/kvstore.conf.example` 当前是 Stage 3 可加载 schema。Stage 1 的统一对象、descriptor 和 parse-once，Stage 2 的 server/network/storage/TTL/persistence，以及 Stage 3 的 vector/embedding/snapshot/rebuild 已迁移。replication/eBPF 和最终兼容清理仍按 Stage 4～6 执行。

### Configuration-1：统一配置结构和校验接口

- [x] `configuration.effective_struct`：建立唯一 effective 配置结构，覆盖 server/network/storage/TTL/persistence/vector/embedding/snapshot/rebuild/replication/eBPF/observability。
  - 实现位置：预计 `include/kvs_config.h`、`src/core/kvs_config.c` 及只读 capability view；阶段开始时再冻结文件清单。
  - 验证：默认配置 dump 与 golden schema 一致。

- [x] `configuration.field_descriptors`：以字段描述表统一默认值、类型、范围、枚举、alias、来源和 capability 校验。
  - 前置条件：统一配置结构字段冻结。
  - 门禁：文件和 CLI 不维护两套字段分支。

- [x] `configuration.ini_sections`：解析冻结的 INI section/schema。
  - 验证：所有 section 正常加载；未知 section/key、重复 key、缺失 value 和尾随垃圾明确失败。

- [x] `configuration.legacy_aliases`：Stage 1～5 兼容旧配置名称并输出 deprecated warning/source。
  - 已知边界：removed backend 不是可静默兼容的 alias。
  - 验证：现有 `config/*.conf` 逐项加载并核对转换结果。

- [x] `configuration.parse_once`：配置文件只解析一次。
  - 目标：先扫描 `-c/--config`，再生成一次 main config merge。
  - 前置条件：统一字段描述表完成。
  - 验证：配置文件打开/解析计数、重复解析回归。
  - 门禁：模块不得自行重新打开主配置。

- [x] `configuration.cli_once`：CLI 只通过同一 descriptor 覆盖一次。
  - 验证：`built-in < file < explicit CLI` 优先级矩阵。

- [x] `configuration.derived_after_merge`：sync port、M0、线程/队列资源组合和路径等派生值只在 merge 后计算一次。
  - 门禁：文件和 CLI 顺序不改变最终 effective value。

- [x] `configuration.source_tracking`：每个字段记录 `default`、`file:line` 或 `cli` 来源。
  - 验证：启动日志/config dump 可机器检查，敏感值脱敏。

- [x] `configuration.strict_validation`：严格拒绝非法整数、溢出、非法枚举、冲突路径和未编译能力。
  - 验证：负数、边界、溢出、unsupported capability 和字段冲突矩阵。

- [x] `configuration.path_resolution`：统一 `config_root`、`data_dir`、运行数据和静态资产路径解析接口。
  - 门禁：不依赖进程 cwd，不尝试多个相对路径；规范化结果和来源可观测。

- [x] `configuration.compatibility_gate`：保持现有配置可兼容加载，并对未知/无法转换项明确失败。
  - 验证：`config/kvstore.conf`、master/slave、embedding 和测试临时配置兼容套件。

Configuration-1 最小门禁：结构、descriptor、parse-once、CLI merge、source tracking、strict parse、path API 和 legacy warning 全部有自动测试；未修改公开命令、RDB/AOF 格式和 manager/core 边界。通过后才进入 Configuration-2。

当前证据：`tests/unified_config_stage1_test.sh` PASS；实现和来源矩阵见 `docs/configuration/unified_configuration_stage1_report.md`。

### Configuration-2：服务、网络、存储和持久化迁移

- [x] `configuration.network.listen_limits`：已有 port/network mode 和各网络实现限制，但尚未迁入统一 effective config。
  - 目标：统一 listen address、port、backlog、connection/request/response limits。
  - 验证：合法/冲突地址端口、请求上限和响应上限测试。

- [x] `configuration.network.three_models`：reactor、proactor、ntyco 已实现，配置与资源限制尚未统一。
  - 验证：三个网络模型用同一配置结构启动 smoke，unsupported build 明确失败。

- [x] `configuration.storage.capacity`：四种 storage 和部分编译期容量已存在，尚无统一 runtime 配置/校验。
  - 验证：engine capability、initial capacity、memory backend 和 max value gate。

- [x] `configuration.ttl.scan`：TTL interval/budget 有当前默认和解析字段，尚未统一迁移。
  - 验证：disabled/enabled、interval/budget 边界和四 backend 回归。

- [x] `configuration.persistence.rdb_aof`：RDB/AOF/fsync/rewrite 已实现，runtime switch、路径和错误处理尚未统一。
  - 验证：RDB-only、AOF-only、RDB+AOF、disabled 和权限/路径冲突。

- [x] `configuration.persistence.rewrite`：AOF rewrite 已实现，但阈值存在多处宏/硬编码。
  - 门禁：只从 effective config 读取，inactive 字段不伪装生效。

- [x] `configuration.storage.max_value`：`storage.max_value_bytes` 真正进入协议/命令写入门禁。
  - 验证：边界值和超限值在四 backend、一条请求和 Pipeline 中一致拒绝。

- [~] `configuration.runtime_paths`：RDB、AOF、snapshot、manifest、log 统一相对 `data_dir`。
  - 门禁：路径冲突/权限/文件类型在启动前失败。

- [x] `configuration.recovery_order`：当前实际顺序为 RDB -> AOF -> index，但未由统一配置校验且存在 RDB 向量恢复重复调用。
  - 门禁：只接受 `rdb_then_aof_then_index`，并保持现有恢复兼容。

Configuration-2 最小门禁：三个网络启动、普通 KV/TTL、max value、RDB/AOF/rewrite、路径失败和恢复顺序自动测试全绿；所有迁移模块只读 effective config。通过后才进入 Configuration-3。

当前证据：Stage 2 专项、ASan/UBSan TTL 四 backend、RDB、AOF、RDB+AOF、vector persistence、core_flat 和 binary-key 回归均 PASS。Stage 3 已迁移 snapshot/manifest，普通日志路径仍未迁移，因此 `configuration.runtime_paths` 保持 `[~]`。

### Configuration-3：向量和模型配置迁移

- [x] `configuration.vector.enabled`：关闭后不初始化 embedding、record store、index、snapshot、rebuild worker；六个 V handler 返回现有明确错误。
- [x] `configuration.vector.index_type`：正式配置只接受 `core_flat/core_hnsw_v2`，removed backend 在统一 parser 中 fail-fast。
- [x] `configuration.vector.dimension_metric`：hash/local_c、index、snapshot 和恢复记录统一校验 dimension/metric/normalize 契约。
- [x] `configuration.vector.hnsw_parameters`：M/M0/efConstruction/efSearch/initial capacity 读取 effective config，canonical 值不再 runtime clamp。
  - 边界：destructive reserve 和未知规模扩容仍属于下一道 HNSW capacity safety gate。
- [x] `configuration.embedding.assets`：model/vocab 相对 config_root 解析；legacy vocab 推导一次并告警；内容 hash 进入模型 identity。
- [x] `configuration.embedding.ort_threads`：intra/inter-op、graph optimization、execution mode、text bytes/token cap 严格接线，ONNX capability/output shape 在启动期校验。
- [x] `configuration.embedding.cache`：cache enable/capacity/TTL/lru policy 由 effective config 控制，disabled 附属字段标记 inactive。
- [x] `configuration.vector_snapshot`：manifest/index 相对 data_dir，load/invalid action/write policy 可控；manifest v2 校验 model identity。
- [x] `configuration.vector_rebuild`：async/cancel/manual retry、delta initial/max 和 publish 后 snapshot 开关已接线；delta 达上限进入 FAILED。
- [x] `configuration.embedding.dead_batch`：当前执行链没有正式 batching consumer，字段保持 unsupported 并从正式示例删除，显式配置失败。
- [x] `configuration.vector.removed_backend_error`：统一配置文件和 CLI 不再把 removed backend 静默 fallback。

Configuration-3 最小门禁：hash/local_c、模型/vocab/维度错误、HNSW 参数、removed backend、snapshot valid/missing/corrupt、同步/异步 rebuild 和 V 族持久化回归全绿；配置迁移不得改变公开响应或持久化格式。该门禁已通过。

当前证据：`tests/unified_config_stage3_test.sh`（含 ASan/UBSan）、runtime/vector-disabled/ONNX 专项、core_flat/HNSW、snapshot、rebuild 和 RDB/AOF/RDB+AOF 回归均 PASS。实现报告见 `docs/configuration/unified_configuration_stage3_report.md`。

### Configuration-4：主从同步与 eBPF 配置迁移

- [x] `configuration.replication.tcp`：TCP master/slave 只读 effective config；master/slave sync port 已拆分并按角色校验。
- [x] `configuration.replication.readiness_policy`：配置契约冻结为 `reject`，其他值启动失败；当前协议没有 READY 状态机，实际门禁由 Replication 后续阶段实现。
- [x] `configuration.ebpf.parse_once`：eBPF daemon 直接调用 `kvs_config_load()`，不再解析独立 eBPF 业务配置或位置参数。
- [x] `configuration.ebpf.modes`：只接受 `disabled/observe/experimental_mirror`；observe 不做 FULLDUMP 或写入转发。
- [x] `configuration.replication.target_exclusion`：TCP 与 experimental mirror 保守互斥；observe 可与 TCP 共存。
- [x] `configuration.replication.future_fields`：offset/ACK/backlog/reconnect/partial-resync/promotion 显式配置均报 future。

Configuration-4 证据：Stage 1～4 配置矩阵和 sanitizer、eBPF capability/object/path、TCP/eBPF互斥、daemon统一入口、四 backend 普通 KV/TTL TCP replication、完整 eBPF/FAISS/HNSWLIB 构建均 PASS。真实 eBPF kprobe 测试因当前进程无 `CAP_BPF/CAP_SYS_ADMIN` 且 unprivileged BPF 被禁用而跳过。实现报告见 `docs/configuration/unified_configuration_stage4_report.md`。

### Configuration-5：测试、README 和兼容迁移

状态：主从同步完成后再执行，本阶段不启动。

- [ ] `configuration.tests.existing_profiles`：迁移全部 `config/*.conf` 并逐一验证 warning/effective values。
- [ ] `configuration.tests.temp_generators`：测试临时配置统一通过 schema helper 生成。
- [ ] `configuration.benchmark_profiles`：Benchmark 配置使用统一 schema，记录全部 effective 参数和来源。
- [ ] `configuration.readme.commands`：更新 README 启动命令和不再受支持的旧写法。
- [ ] `configuration.deprecation_messages`：旧 alias、环境变量和 removed backend 提示稳定可测试。
- [ ] `configuration.example_profiles`：提供 ordinary standalone、vector standalone、TCP master/slave 和 eBPF observe profile。

Configuration-5 最小门禁：现有配置逐一 load、README 命令 smoke、测试 schema helper、Benchmark manifest 和 warning golden 全部通过；不存在静默忽略字段。通过后进入 Configuration-6。

### Configuration-6：删除旧解析和废弃配置

状态：主从同步完成且 Configuration-5 完成后再执行，本阶段不启动。

- [ ] `configuration.cleanup.parsers`：删除主服务、模块和 eBPF daemon 的重复 parser。
- [ ] `configuration.cleanup.env`：删除已过兼容期的生产环境变量读取。
- [ ] `configuration.cleanup.dead_fields`：删除无消费者、伪生效和重复定义的配置。
- [ ] `configuration.cleanup.aliases`：删除 Stage 1～5 的 deprecated alias，旧 key 明确失败。
- [ ] `configuration.cleanup.module_reads`：禁止模块直接读取配置文件或环境变量，只消费 effective config/capability view。
- [ ] `configuration.cleanup.full_regression`：全量配置、普通 KV、TTL、持久化、向量、snapshot/rebuild 和复制回归。

Configuration-6 最小门禁：静态检查确认模块无直接 file/getenv 读取，旧 key failure golden、config dump golden、full build 和全量回归通过；统一配置路线完成。

## 主从同步能力

可靠向量复制的冻结来源是“应用层 commit 后的确定性 manager mutation + TCP”。公开文本 VSET、HNSW graph 和 eBPF 抓取流量都不是可靠向量复制载荷。

### 当前状态

- [x] `replication.tcp.incremental_kv`：普通 KV 有限 TCP 增量同步。
  - 验证：`tests/ttl/test_ttl_tcp_replication.sh` 覆盖四 backend 的 set/update/delete/TTL。
  - 已知边界：无 offset、ACK、backlog 和自动重连。

- [x] `replication.tcp.snapshot_kv`：普通 KV 有限 TCP 快照式同步。
  - 验证：`tests/ttl/test_ttl_tcp_replication.sh` 的 new slave/rejoin 场景。
  - 已知边界：不是 RDB，只含普通 KV；通过全局 broadcast queue，缺少 snapshot offset 和多从节点隔离。

- [~] `replication.ebpf.experimental_kv_mirror`：eBPF 普通 KV FULLDUMP/增量实验性镜像。
  - 已有：`tests/ttl/test_ttl_ebpf_replication.sh` 在环境满足时覆盖普通 KV/TTL。
  - 缺口：内核/权限依赖，队列可丢、无 offset/checksum/backlog，不能证明断线一致性。

- [ ] `replication.vector.incremental`：向量 VSET/VDEL 增量复制。
- [ ] `replication.full_sync.rdb_network`：RDB/等价 framed records+embedding 网络全量同步。
- [ ] `replication.offset`：主节点全局 offset 和从节点 applied offset。
- [ ] `replication.ack`：成功 apply 后 ACK。
- [ ] `replication.backlog`：有界、按 offset 定位的可靠 backlog。
- [ ] `replication.partial_resync`：断线后的 backlog 增量追赶。
- [ ] `replication.auto_reconnect`：TCP 从节点自动重连和状态恢复。
- [ ] `replication.replica_readiness`：FULL_SYNC/CATCHING_UP/READY/BLOCKED 状态与查询门禁。
- [ ] `replication.promotion_recovery`：从节点晋升、旧主回归和故障恢复。

### Replication-1：向量增量复制

- [ ] `replication.vector.mutation_codec`：定义 versioned、length-delimited、带 checksum 的确定性向量变更格式。
  - 字段门禁：UPSERT/DELETE、sequence、record_id、prev_record_id、question/scope、answer、normalized question、embedding、model version 和时间字段。
  - 已知边界：不复制 HNSW graph，不复制 manager generation，不修改公开 RESP。

- [ ] `replication.vector.upsert_delete`：主节点 manager commit 成功后按 commit 顺序生成 UPSERT/DELETE。
  - 前置条件：容量失败和 VSET 失败原子性语义已冻结；失败写不得进入复制流。
  - 门禁：mutation 入队顺序点位于 manager commit 后、客户端成功响应前。

- [ ] `replication.vector.replica_no_onnx`：从节点直接使用主节点 embedding，不执行 tokenizer/ONNX。
  - 验证：replica runtime encode counter 保持 0，模型/维度/metric 不兼容 fail-fast。

- [ ] `replication.vector.atomic_apply`：从节点原子更新 record store、question/scope map、record_id map、current index 和 rebuild delta。
  - 安全规则：任一步失败都不推进 `applied_offset`、不 ACK；capacity/resize/OOM 同样停止 apply。

- [ ] `replication.vector.sequence_rules`：duplicate/stale 幂等跳过，gap/checksum/prev_record_id 分叉明确拒绝。
  - 验证：duplicate、gap、checksum、update/delete、slot reuse 和 stale hit。

- [ ] `replication.vector.rebuild_concurrency`：复制 apply 与异步 rebuild delta capture 一致。
  - 验证：full scan 前后注入 UPSERT/DELETE，最终 shadow/active 与主节点记录一致。

- [ ] `replication.vector.parity_gate`：主从 record_id、next_id、active records 和 index count 一致。
  - 门禁：纯增量场景全绿后才进入 Replication-2。

Replication-1 最小门禁：codec roundtrip/checksum、ID/顺序 parity、duplicate/gap、update/delete、ONNX-zero-rerun、rebuild 并发、capacity apply failure 全绿；不改 AOF 格式和 eBPF 路径。

### Replication-2：全量同步和索引就绪

- [ ] `replication.full_sync.records_embedding`：传输主节点产品记录、embedding、稳定 ID 和 `next_id`，不把 HNSW 图作为真相。
- [ ] `replication.full_sync.framing`：完整帧、长度、schema version、checksum 和完成帧。
- [ ] `replication.full_sync.snapshot_offset`：全量快照绑定 offset O，后续只应用 `(O, current]`。
- [ ] `replication.full_sync.next_id`：从节点恢复 `next_id` 且不与已存在 record_id 冲突。
- [ ] `replication.full_sync.local_index`：从节点按兼容条件加载本地 snapshot，失败则从 records 重建。
- [ ] `replication.full_sync.states`：实现 FULL_SYNC/CATCHING_UP/READY/BLOCKED/FAILED 状态。
- [ ] `replication.full_sync.query_gate`：READY 前拒绝 VSEARCH，不返回部分索引结果。
- [ ] `replication.full_sync.atomic_install`：全量安装失败不暴露部分 records/index，不覆盖最后一个可用状态。

Replication-2 最小门禁：新从节点 cold full sync、frame/checksum failure、snapshot offset、next_id、模型兼容、READY 前查询拒绝、完成后 VGET/VSEARCH/records/index parity 全绿。在线 concurrent catch-up 的完整能力留给 Replication-3，但不得伪装 READY。

### Replication-3：backlog 和断线追赶

- [ ] `replication.backlog.master_offset`：主节点每条已提交 mutation 分配单调全局 offset。
- [ ] `replication.backlog.bounded_store`：有界 backlog，记录明确的 start/end offset。
- [ ] `replication.backlog.applied_offset`：从节点只在完整原子 apply 成功后推进。
- [ ] `replication.backlog.ack`：周期 ACK applied offset，用于 lag 和裁剪，不作为客户端写成功门禁。
- [ ] `replication.backlog.reconnect`：自动重连并请求 `(applied_offset, current]`。
- [ ] `replication.backlog.range_gate`：请求早于 backlog start 时转全量同步。
- [ ] `replication.backlog.corruption`：gap/checksum/apply 失败停止追赶并转全量同步。
- [ ] `replication.backlog.multi_replica`：每个从节点独立连接、offset、ACK、full-sync 和失败状态。
- [ ] `replication.backlog.not_transport_queue`：发送队列和 backlog 明确分离，队列不得承担历史回放语义。

Replication-3 最小门禁：断线追赶、backlog overrun -> full sync、gap/checksum -> full sync、多从节点隔离、snapshot 后 mutation 恰好 apply 一次，最终主从一致。

### Replication-4：重启、晋升和故障恢复

- [ ] `replication.recovery.persisted_offset`：本地持久化 applied offset，带版本和 checksum。
- [ ] `replication.recovery.dedup_state`：重启恢复 duplicate/stale rejection 状态。
- [ ] `replication.recovery.next_id`：重启和晋升后 `next_id` 不与历史 ID 冲突。
- [ ] `replication.recovery.promotion`：READY 从节点晋升为新主节点，offset/ID/记录状态连续。
- [ ] `replication.recovery.old_master_return`：旧主节点回归时通过兼容/offset 判断追赶或全量重同步。
- [ ] `replication.recovery.interrupted_full_sync`：全量同步中断不发布部分状态，重启可清理或重试。
- [ ] `replication.recovery.model_mismatch`：模型/version/dimension/metric 不一致时拒绝 READY、晋升或 mutation apply。
- [ ] `replication.recovery.old_persistence`：保持旧 RDB/AOF 兼容，不把复制状态偷偷写入旧格式。
- [ ] `replication.recovery.fault_matrix`：crash、I/O、checksum、OOM/capacity、断网、backlog overrun 和进程重启故障注入。

Replication-4 最小门禁：crash/restart 不重复 apply，晋升后可继续安全分配 ID 和复制，旧主回归无双写，全量中断不提供部分查询，旧持久化文件仍可恢复。

## eBPF 定位

```text
可靠复制：
  应用层提交后的确定性变更 + TCP

eBPF：
  流量观测、性能诊断、旁路审计和实验性镜像
```

- [~] `ebpf.capture.ordinary_writes`：已能捕获并解析部分普通写命令。
  - 缺口：捕获的是网络字节而不是 commit 结果；内核事件和用户队列可能丢失。
- [~] `ebpf.mirror.ordinary_kv`：普通 KV 实验性 FULLDUMP/增量镜像已有实现和环境依赖测试。
- [ ] `ebpf.configuration.unified`：接入统一配置，不再由 daemon 独立解析。
- [ ] `ebpf.observability.traffic`：请求字节、命令、连接和 drop 统计。
- [ ] `ebpf.observability.commit_reconcile`：核对请求数、成功提交数和复制日志数。
- [ ] `ebpf.observability.queue_loss`：ring buffer/user queue drop 和重连缺口可观测。
- [ ] `ebpf.audit.side_channel`：旁路一致性审计，不直接修改从节点 source of truth。
- [ ] `ebpf.mirror.final_decision`：基于观测价值、维护成本和误用风险决定是否保留 experimental mirror。

明确禁止：

- eBPF 直接复制公开 VSET 文本或在从节点重新生成 embedding。
- eBPF 生成 replication sequence、维护 backlog 或声明 ACK/READY 语义。
- TCP 与 eBPF 同时写入相同从节点数据集。
- 把 eBPF ring buffer、用户队列或 transport queue 视为可靠复制日志。

## 持久化与启动恢复待办

- [x] `recovery.vector.rdb`：RDB 恢复产品记录、embedding、ID 和 `next_id`。
  - 验证：`tests/vector_command_persistence_recovery_test.sh`。
- [x] `recovery.vector.aof_text`：AOF 文本 VSET/VDEL 重放恢复最终记录状态。
  - 验证：`tests/vector_command_persistence_recovery_test.sh`。
- [x] `recovery.vector.snapshot_fallback`：索引快照加载、校验和从 records 回退重建。
  - 验证：snapshot load/VINFO tests。
- [~] `recovery.vector.duplicate_rebuild_call`：RDB 向量段末尾和主启动流程都会调用索引恢复。
  - 当前影响：最终数据正确，但 RDB-only 缺快照时先 rebuild/write，再次调用又 load，增加启动成本并覆盖首次 fallback 状态。
- [ ] `recovery.vector.single_index_restore`：消除重复恢复调用，冻结唯一的 RDB -> AOF -> index 恢复点。
  - 验证：调用计数、RDB-only/AOF-only/RDB+AOF、valid/missing/corrupt snapshot。
- [ ] `recovery.vector.first_fallback_vinfo`：保留正确的首次 snapshot fallback reason/status。
- [ ] `recovery.aof.deterministic_vector_format_design`：独立评估 AOF 是否迁移为带 embedding 的内部确定性变更格式。
  - 已知边界：不得混入 Replication-1～3；除非重新冻结设计，否则 AOF 文本兼容不能阻塞向量增量复制。
- [ ] `recovery.replica.applied_offset`：本地持久化从节点 offset/去重状态。
- [ ] `recovery.replica.restart`：从节点重启后恢复 records/index/next_id/offset 并安全追赶。

## 并发和规模验证

本章节必须排在配置、容量安全、单机正确性和复制正确性之后。扩大 Benchmark 不能抢占这些阶段。

- [ ] `scale.concurrent.vset_vsearch`：多客户端并发 VSET/VSEARCH，记录吞吐和 p50/p95/p99。
- [ ] `scale.concurrent.write_rebuild`：写入与异步重建并发，验证 delta、发布、读延迟和内存峰值。
- [ ] `scale.concurrent.write_snapshot`：写入与 snapshot write/load 并发，验证 source checksum 和 best-effort 失败语义。
- [ ] `scale.concurrent.write_replication`：写入与主从复制并发，验证 commit/sequence/ACK/offset 一致性。
- [ ] `scale.replication.multi_replica`：多从节点独立 full sync、catch-up、lag 和 failure isolation。
- [ ] `scale.stability.long_running`：长时间混合 KV/TTL/vector/persistence/rebuild/replication 稳定性。
- [ ] `scale.capacity.one_million`：100 万条记录容量、build/rebuild、snapshot、恢复和复制验证。
- [ ] `scale.performance.concurrent`：并发吞吐率、endpoint latency、backend/resolve/response 分解和资源曲线。
- [ ] `scale.fault_resource_pressure`：故障注入、磁盘/内存/队列/连接不足和恢复验证。
- [ ] `scale.memory_thread_safety`：ASAN/LSAN/TSAN、泄漏、use-after-free、锁/epoch/refcount 和 shutdown。

规模阶段最小门禁：先冻结 workload、fixture、配置、并发度、测量开关和 correctness assertions；每项性能结果都必须同时报告错误数、数据一致性、内存来源和测试环境，不能只报告 QPS。

## README 和项目冻结

- [~] `project.readme.architecture`：README 已更新为重构后的普通 KV/向量、manager/core、持久化、snapshot 和 rebuild 架构。
  - 缺口：统一配置和可靠主从同步尚未实现，相关启动/同步描述仍需后续迁移。
- [x] `project.benchmark.100k`：10 万条真实文本向量基准结果已同步。
  - 验证：`docs/vector_benchmark_readiness/b_scale_6_t2ranking_100k_hnsw_faiss_index_only_report.md`、B-scale-7 产品报告。
- [x] `project.benchmark.hnsw_faiss`：KVLite HNSW/FAISS 纯索引对比已完成并明确口径。
- [x] `project.benchmark.product_e2e`：产品 VSET/VSEARCH/VINFO 端到端结果已完成。
- [x] `project.limitations.current`：README 和审计文档已记录 FAISS 非产品 backend、snapshot derived cache、AOF 重跑 ONNX、单客户端基准和容量风险。
- [ ] `project.readme.configuration_commands`：配置 Stage 5 后更新启动命令、profile 和 deprecated 提示。
- [ ] `project.readme.replication_architecture`：Replication-4 后更新 full sync/offset/backlog/readiness/晋升架构。
- [ ] `project.milestone.final_document`：生成最终里程碑、验证矩阵、限制和运维说明。
- [ ] `project.git.cleanup`：按阶段整理提交，确认 dirty worktree 中每项改动归属。
- [ ] `project.git.annotated_tag`：clean tree、全量 gate 和最终文档通过后创建 annotated tag。

## 冻结的全局开发顺序

```text
1. Configuration Stage 1
2. Configuration Stage 2
3. Configuration Stage 3
4. Configuration Stage 4
5. Replication Stage 1A：HNSW 写入与容量安全前置门禁
6. Replication Stage 1：向量增量复制
7. Replication Stage 2
8. Replication Stage 3
9. Replication Stage 4
10. Configuration Stage 5
11. Configuration Stage 6
12. 并发与更大规模验证
13. README、提交与里程碑冻结
```

依赖复核结论：

- 配置基础层必须先统一结构、解析、来源和路径，后续复制配置才能只有一个真相来源。
- 向量、embedding、snapshot 和 rebuild 配置迁移必须先于主从模型/维度/metric/snapshot 兼容校验。
- Replication Stage 1A 是向量增量复制的最小前置门禁，只处理失败原子性、destructive reserve 和 capacity apply failure；不扩展为独立大型 capacity 设计阶段。
- Replication-1 开始前必须冻结 capacity failure 的停止语义：apply 失败不推进 offset、不 ACK；Stage 1A 完成后立即进入向量增量复制。
- Configuration-4 只统一复制/eBPF 配置和互斥校验，不实现向量复制；统一配置工作在此暂停。
- eBPF 配置统一不会改变其“非可靠复制主路径”的定位。
- AOF 内部格式迁移是独立设计，不阻塞 Replication-1～3，也不得借复制阶段修改旧 AOF/RDB 格式。
- Configuration-5 需要真实的 Replication-1～4 schema 和 profile 才能一次性迁移测试/README；Configuration-6 再删除兼容 parser/alias，因此二者放在复制完成后合理。
- 更大规模 Benchmark 和并发压测依赖配置可复现、容量安全、复制正确性和故障语义，不能提前抢占。

审阅结果：Configuration Stage 1～4 已完成；Stage 5/6 延后到主从同步完成后。下一步按冻结路线进入 Replication Stage 1A。

## 当前阶段与更新模板

当前下一阶段：

```text
Replication Stage 1A - HNSW 写入与容量安全前置门禁
```

进入门禁：只解决向量复制所必需的失败原子性和 destructive reserve 风险；不实现复制协议，不修改 HNSW 算法参数，完成后立即进入向量增量复制。

每个阶段结束后在本节追加或更新一条记录：

```text
阶段：
状态：[x] / [~] / [ ]
修改文件：
生产代码是否变化：
测试与结果：
静态检查：
已知风险：
下一阶段门禁：
```

## 本次建立文档的验证

- 修改范围：只新增 `docs/development/kvstore_development_checklist.md`。
- 生产代码、测试、配置和 Makefile：未修改。
- 功能测试/build：本任务不改变可执行行为；完成状态引用当前仓库现行测试与最新审计证据。
- `git diff --check` 和最终 `git status --short`：在本次任务收尾时执行并汇报。
