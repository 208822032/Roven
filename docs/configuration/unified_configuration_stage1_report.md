# Unified Configuration Stage 1 实现报告

## 1. 范围与结果

本阶段实现统一配置结构、解析、来源和校验接口，并将主进程切换到单次加载流程。生产配置基础设施有修改；公开协议、RDB/AOF 格式、向量算法、主从同步逻辑、Makefile 和业务模块配置消费点未修改。

最终流程：

```text
prescan -c/--config/--no-config
  -> descriptor defaults
  -> parse one config file
  -> apply CLI once through the same descriptors
  -> derive config_root/data_dir/sync_port once
  -> validate once
  -> publish g_cfg
```

旧流程会先应用 CLI、加载文件、再应用 CLI，并可能在最终 port/role 覆盖前固化 `sync_port`。新流程中 `sync_port` 只在 merge 完成后计算；显式值保持不变，未显式值按 master 的 `network.port + 1000` 或 slave 的 `replication.master_service_port + 1000` 派生，并记录 derived source。

## 2. 配置结构

`kvs_config_field_descriptor_t` 为每个字段记录 section、canonical name、legacy aliases、类型、默认值、约束/枚举、CLI override 能力和 `implemented/inactive/unsupported/future` 状态。Stage 0 冻结的 11 个 section 均进入描述表：

```text
server, network, storage, ttl, persistence, vector, embedding,
vector_snapshot, vector_rebuild, replication, ebpf, observability
```

每个已存储字段在 `kvs_config_t.sources[]` 中记录 `default`、`file:<absolute-path>:<line>`、`cli` 或 `derived`；legacy alias 另有标志，格式化输出带 `legacy-alias:`。`kvs_config_print_effective()` 提供只读值/来源/status 输出，本阶段没有把该输出加入公开协议或默认日志。

现有模块继续读取 `g_cfg` 的兼容字段。统一对象新增 `config_path`、`config_root`、`data_dir`、`no_config` 和 source metadata；没有新增第二个业务配置对象。

## 3. 解析和兼容

新格式 `[network] port=8080` 和旧格式 `port=8080` 最终指向同一 descriptor。旧字段输出废弃提示；同一文件内 canonical/legacy 重复、两个 queue aliases 重复或 CLI 内重复覆盖均失败。文件值可由 CLI 覆盖，这是既定优先级，不视为重复。

兼容规则：

- `flat` 明确归一化为 `core_flat`；removed `core_hnsw`、hnswlib、FAISS 等产品 backend 名称直接失败。
- legacy `c_api` 归一化为 `local_c`。
- legacy HNSW 低值仅在旧字段上归一化为当前实际下限 `48/512/1024` 并告警；canonical 字段低于下限直接失败。
- `embedding_prepare_queue_capacity` 与 `embedding_worker_queue_capacity` 是同一字段的 aliases，同时出现失败；最终只生成一个 queue 值。
- 所有 `embedding_batch_*` 当前标记 unsupported，显式配置失败。
- legacy `sync_mode=ebpf` 暂时保留当前 runtime 值并给出高可见告警；TCP/eBPF schema 迁移仍属于 Stage 4。

为使正式 legacy profiles 在严格模式下继续加载，同时不保留 silent fallback，`config/kvstore.conf`、`config/master.conf` 和 `config/kvstore-embedding.conf` 的 removed backend 改为其此前实际生效的 `core_flat`；embedding profile 删除 dead batch 字段和重复 queue alias。其余 legacy key 保留用于兼容验证。

## 4. 严格失败与校验

以下输入已由测试确认失败：未知 section/key、重复字段、缺少 `=`、空值、非法 bool/int/float/enum、整数溢出、范围越界、section/数值尾随内容、显式文件缺失、CLI 缺值/未知项、`--config` 与 `--no-config` 冲突、removed backend、unsupported/future 字段及新旧名称重复。

加载完成后校验绝对 root、sync port、TCP slave master endpoint、vector backend/HNSW 当前有效下限、embedding queue alias 一致性和 `local_c` 非空 model path。模型/词表存在性、编译能力、维度一致性和业务路径冲突需要 Stage 2/3 的模块信息，本阶段不伪装已验证。

## 5. 路径接口

配置文件先通过 `realpath()` 变为绝对路径。`config_root` 默认以配置文件目录为基准，`data_dir` 再相对 `config_root` 解析；无配置文件时以启动 cwd 为 config root。`kvs_config_resolve_path()` 规则：

- `KVS_CONFIG_PATH_DATA`：相对 `data_dir`，供 RDB/AOF/snapshot/log 使用。
- `KVS_CONFIG_PATH_CONFIG` / `KVS_CONFIG_PATH_ASSET`：相对 `config_root`，供模型、词表和 eBPF object 使用。
- 绝对路径只做 lexical normalization，不改变根。

测试在切换 cwd 到 `/` 后加载绝对配置路径，结果仍绑定配置文件目录。Stage 1 只提供并验证接口；现有 RDB/AOF、embedding、snapshot 和 daemon 调用点尚未批量迁移。

## 6. 测试与验证

执行结果：

```text
gcc -I./include -I./src -Wall -Wextra -Werror -std=c11 \
  -c src/core/kvs_config.c -o /tmp/kvs_config.o
PASS

tests/unified_config_stage1_test.sh
PASS

make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j"$(nproc)"
PASS

tests/vector_core_flat_correctness_test.sh
PASS

tests/vector_core_hnsw_v2_default_backend_test.sh
PASS

tests/vector_command_persistence_recovery_test.sh
PASS (RDB, AOF, RDB+AOF)
```

Stage 1 测试覆盖 defaults、descriptor、INI、legacy warning、file/CLI precedence、`sync_port` 派生、严格失败、`--no-config`、cwd 无关路径、source/print、queue/backend 特例和四份现有主配置。default-backend 回归同步更新为 removed backend 必须失败。

静态边界检查：`src/vector/core` 无 `g_vector_state` 命中，也无 scope/TTL/metadata/question/answer 命中；`include/vector/kvs_vector_manager.h` 的既有 manager extern 是唯一 `g_vector_state` 命中。`record_index` 命中均位于 manager/store 层，`kvs_vector_index_ops_t` 定义和签名未修改。`git diff --check` PASS。

## 7. 文件和后续边界

Stage 1 修改/新增：

```text
include/kvs_config.h
src/core/kvs_config.c
src/core/kvstore.c
config/kvstore.conf
config/master.conf
config/kvstore-embedding.conf
tests/unified_config_stage1_test.c
tests/unified_config_stage1_test.sh
tests/vector_core_hnsw_v2_default_backend_test.sh
docs/configuration/unified_configuration_audit.md
docs/configuration/kvstore.conf.example
docs/configuration/unified_configuration_stage1_report.md
```

未接线业务字段保持 `unsupported/future`，显式配置会失败；`inactive` 仅用于当前结构中已有但没有消费者或只保留兼容状态的字段。旧环境变量、eBPF daemon parser 和模块硬编码未在本阶段迁移。

下一阶段严格按既定路线执行 `Unified Configuration Stage 2 - server、network、storage 和 persistence 配置迁移`。

阶段结束时 `git status --short` 中，Stage 1 文件为：

```text
 M config/kvstore-embedding.conf
 M config/kvstore.conf
 M config/master.conf
 M include/kvs_config.h
 M src/core/kvs_config.c
 M src/core/kvstore.c
 M tests/vector_core_hnsw_v2_default_backend_test.sh
?? docs/configuration/
?? tests/unified_config_stage1_test.c
?? tests/unified_config_stage1_test.sh
```

开始前已有且未处理的改动仍包括 `README.md`、`docs/replication/`、`docs/vector_command_functionality_audit.md`、`tests/vector_command_persistence_recovery_test.sh`；另有不属于本阶段的 `docs/development/`。本阶段未提交 commit。

```text
unified_config_object_implemented=yes
descriptor_schema_implemented=yes
config_parsed_once=yes
cli_merged_once=yes
derived_values_computed_after_merge=yes
strict_validation_passed=yes
legacy_config_compatible=yes
path_resolution_interface_ready=yes
ready_for_configuration_stage2=yes
```
