# 第二十一章 统一配置与启动恢复门禁

## 21.1 功能越多，越不能让配置只是“被读取”

第二十章把向量路径推到了真实 T2Ranking 10K/100K，但它也暴露了另一类问题：embedding provider、HNSW 参数、snapshot、rebuild、RDB/AOF 和 replication 都有了自己的开关、路径和默认值，一个值即使被 parser 读到，也可能没有进入真正的初始化路径。

`7cd837f` 之前的审计找到了典型漂移：配置文件仍写已移除的 backend 名称，resolver 会静默落到 `core_flat`；文本中的 `M/ef` 与内核实际使用值不同；snapshot 路径受启动 cwd 影响；配置文件、CLI 和模块内硬编码重复定义默认值。这些不是“文档不够清楚”，而是启动时缺少一个唯一、可验证的 effective configuration。

本章的唯一主线是：

```text
built-in defaults < config file < explicit CLI
-> derive paths and dependent values once
-> validate supported combinations once
-> publish one effective g_cfg
-> initialize consumers and restore records
-> load a compatible snapshot or rebuild the derived index
-> reject startup when the configured recovery contract cannot be met
```

这个阶段解决的是“配置真正生效且恢复顺序可验证”，不是重新讲 T2Ranking，也不是已经实现向量主从复制。

## 21.2 Git 范围、聚合提交与源码导航

范围核验结果：

```text
起点：7cd837f
终点：7cd837f
inclusive range：7cd837f^..7cd837f
父提交：3b247a3（第二十章终点）
```

`7cd837f` 是 `80 files changed, 8385 insertions, 1485 deletions` 的聚合提交。不能把它写成一次性交付，应按实际工程流程还原：

| 实际阶段 | 落地内容 | 证据边界 |
| --- | --- | --- |
| Unified Configuration Stage 1 | descriptor schema、默认值、file/CLI merge、source metadata、derive/validate | 建立配置基础设施，不等于所有 consumer 已迁移 |
| Stage 2 | server/network/storage/TTL/RDB/AOF 消费 effective config | 固定恢复顺序和路径语义，不改持久化格式 |
| Stage 3 | vector/embedding/snapshot/rebuild 迁移 | 建立模型、索引、snapshot 兼容门禁 |
| Stage 4 | TCP replication/eBPF 配置归一 | 保留已有 KV TCP 行为；未实现向量复制协议 |
| Vector Replication Stage 0/0.5 | 审计现状、冻结未来设计 | 只是 audit/design，不是 codec/apply/READY 实现 |

核心历史源码入口：

1. [`7cd837f` 的类型、字段状态与 source metadata](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/include/kvs_config.h#L67-L95)
2. [`kvs_config_field_descriptor_t` 的 schema contract](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/include/kvs_config.h#L217-L236)
3. [CLI 覆盖与重复 override 拒绝](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/src/core/kvs_config.c#L1210-L1305)
4. [路径和依赖值 derive](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/src/core/kvs_config.c#L1646-L1729)
5. [编译能力、恢复顺序与路径 validate](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/src/core/kvs_config.c#L1806-L1888)
6. [vector/embedding/HNSW 组合门禁](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/src/core/kvs_config.c#L1900-L1977)
7. [`kvs_config_load()` 的唯一 merge/derive/validate 链](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/src/core/kvs_config.c#L2010-L2057)
8. [effective value/source/status 输出](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/src/core/kvs_config.c#L2190-L2305)
9. [服务主启动和恢复顺序](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/src/core/kvstore.c#L201-L365)
10. [snapshot 从 manifest 校验到 bind 的加载路径](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/src/vector/kvs_vector.c#L3128-L3210)
11. [records 恢复后 snapshot/fallback rebuild 决策](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/src/vector/kvs_vector.c#L3293-L3337)
12. [VINFO 中的实际 backend/core 状态](https://gitlab.0voice.com/zhengpan/9.1-kvstore/-/blob/7cd837f/src/vector/kvs_vector.c#L1580-L1710)

## 21.3 不是一个更大的结构体，而是一份可执行 schema

新配置层的核心不是把旧 `g_cfg` 继续加字段，而是让每个字段都有 descriptor：canonical section/key、legacy alias、bool/int/size/float/string/enum 类型、默认值、约束、CLI 可否覆盖，以及 `implemented/inactive/unsupported/future` 状态。字段值之外，`sources[]` 还保留 `default/file/cli/derived`、来源文件和行号。

状态不是文档标签，而是启动 contract：

- `implemented`：可以显式配置，但还要通过跨字段 validate；
- `inactive`：只保留兼容警告，不应被当成运行时能力；
- `unsupported/future`：显式配置即失败，避免“读到但忽略”。

这个设计也把布尔、整数、枚举、溢出和范围错误收敛到一个赋值路径。未知 section/key、缺少 `=`、空值、无效枚举都不再静默回退。

## 21.4 default、file 和 CLI 只合并一次

冻结的优先级是：

```text
built-in descriptor default
< selected config file
< explicit CLI override
```

`kvs_config_load()` 先预扫描 `--config/--no-config`，然后初始化 descriptor defaults，对选中的 config 做 `realpath`，文件只 parse 一次，CLI 只 apply 一次，最后统一 derive 和 validate。这替代了旧流程的“CLI -> file -> 再次 CLI”，也避免 `sync_port` 在 role/port 最终覆盖前就被提前派生。

重复规则是故意严格的：同一文件内 canonical key 与 legacy alias 重复算冲突；同一 CLI 字段重复覆盖也失败；file 中的值被一次显式 CLI 覆盖则是正常优先级。显式指定但缺失的 config 文件会失败，`--config` 与 `--no-config` 并用也失败。

[`Stage 1 测试`](../../tests/unified_config_stage1_test.c) 实际检查默认值、CLI 覆盖 file、source metadata、derived port、legacy warning 和各类错误。[`Stage 1 报告`](../configuration/unified_configuration_stage1_report.md) 记录了该阶段的历史 PASS；本次写作未重跑测试。

## 21.5 路径语义：先固定根，再打开文件

当使用 config 文件时，路径链是：

```text
realpath(config file)
-> config file directory
-> server.config_root
-> server.data_dir
-> RDB / AOF / snapshot runtime paths
```

model、vocab 和 eBPF object 等 asset 相对 `config_root`，RDB/AOF/snapshot 等运行数据相对 `data_dir`。如果明确不用 config 文件，启动 cwd 才作为 config root。因此，有 config 文件时，后续运行时 cwd 变化不再改变文件语义。

这是路径解析门禁，不是文件内容门禁：路径存在、可读或父目录可写，不等于 ONNX 模型输出维度正确，也不等于 snapshot manifest 与当前 records/backend 兼容。旧 cwd 下的 snapshot 也不会自动迁移。[`Stage 2 报告`](../configuration/unified_configuration_stage2_report.md) 记录了 persistence 路径和 cwd recovery 测试的历史结果。

## 21.6 backend、embedding 与 HNSW 必须作为一个组合校验

单字段合法不足以启动 vector path。`kvs_config_validate()` 对当时已支持组合建立了交叉约束：

```text
backend in {core_flat, core_hnsw_v2}
embedding.output_dimension == vector.dimension
vector.normalize == true
M >= 48
M0 >= M
efConstruction >= M0
efSearch > 0
initial_capacity > 0
rebuild.delta_max_entries >= delta_initial_capacity
```

`local_c` 还需要 model/vocab 可读，provider 初始化会进一步检查当前 binary 是否包含 ONNX runtime capability。这一点要精确表述：部分 compile/runtime 组合在 config validate 拒绝，`local_c` 的最终 ONNX 能力门禁在 embedding provider init，不是 parser 单独完成。`vector.enabled=false` 则跳过 provider、record store、index、snapshot 和 rebuild worker 初始化。

旧 backend resolver 仍保留对任意 caller 的兼容 fallback，但经过验证的主启动路径会先拒绝已移除名称，不再让错配置静默切到 `core_flat`。这不等于 resolver 的兼容债务已消失。

## 21.7 编译能力和运行配置之间不能靠 fallback 猜

配置层会按 compile-time mask 检查请求的 storage engine；eBPF 请求需要对应编译能力、object path 和 mode 组合。future replication 字段如 backlog、ACK interval、partial resync 和 promotion policy 在实现前显式配置即失败，而不是允许服务带着假能力启动。

[`Stage 4 配置测试`](../../tests/unified_config_stage4_test.c) 覆盖 TCP/eBPF 组合、future 字段和能力拒绝；[`Stage 4 报告`](../configuration/unified_configuration_stage4_report.md) 记录 build 与 TCP 回归 PASS，但当时环境缺少 `CAP_BPF/CAP_SYS_ADMIN`，真实 kprobe/eBPF 测试按门禁跳过。因此证据是“配置迁移与已有 TCP 行为保持”，不是“eBPF 复制运行通过”。

## 21.8 effective config 在消费者启动前逻辑冻结

主进程的实际时序是：

```text
kvs_config_load
-> log init and effective summaries
-> memory/storage engine init
-> vector/embedding/index init when enabled
-> RDB load when enabled
-> AOF init and replay when enabled
-> kvs_vector_rebuild_after_load
-> replication and TTL init
-> network listener
```

defaults/file/CLI 合并完成后才 derive dependent fields，validate 通过后才发布给全局 `g_cfg`。各 consumer 不再重新打开业务配置文件。日志会记录 server/network、persistence path、vector/backend/HNSW、model identity、snapshot 和 replication 摘要；`kvs_config_print_effective()` 可输出 `field=value source=... status=...`。

但“冻结”在这里是启动顺序上的逻辑不变，不是类型系统保证的 immutable object。`g_cfg` 仍是可写全局 C struct，没有 frozen bit 或只读页面。个别与业务 unified config 不同层的实验开关仍可能读环境变量，不应声称全仓已消灭所有 env/hardcode。

## 21.9 records 恢复后，还要让派生索引进入可用状态

RDB/AOF records 仍是业务真值，HNSW graph 仍是可丢弃派生状态。恢复路径先重建 manager `record_id` map，然后验证 active record 的 embedding 存在、dimension、finite、归一化和 model identity，再绑定配置选中的 backend。

snapshot 决策是：

```text
snapshot enabled && load_on_start
-> validate manifest against backend/config/model/records
-> deserialize graph
-> validate graph and loaded records
-> bind loaded index
-> success

missing / incompatible / corrupt
-> invalid_action=fail: return error
-> invalid_action=rebuild: rebuild from active records
-> best-effort write a new snapshot
```

`snapshot_valid` 在 manifest 兼容性验证后设置，`snapshot_loaded` 只有在 deserialize、graph/record validation 和 bind 全部完成后才设置，所以前者不等于后者。同样，records 恢复成功不等于 index ready。

[`Stage 3 runtime 测试`](../../tests/unified_config_stage3_runtime_test.c) 覆盖 effective HNSW 参数、ONNX capability、rebuild delta 上限和 snapshot fail policy；[`snapshot-after-records 测试`](../../tests/vector_index_snapshot_load_after_records_test.c) 覆盖 valid load 与多种 invalid fallback。它们的历史 PASS 见 [`Stage 3 报告`](../configuration/unified_configuration_stage3_report.md) 和 [`V 族命令/持久化审计`](../vector_command_functionality_audit.md)。

## 21.10 fail-fast 与 fallback 的边界

当前明确终止启动的情况包括：config parse/derive/validate 失败，storage/vector init 失败，AOF init 失败，以及 `kvs_vector_rebuild_after_load()` 返回失败。snapshot `invalid_action=fail` 会把缺失、损坏或不兼容变成启动错误；`rebuild` 则是显式配置的 records-only 降级，不是静默掩盖错误。snapshot writer 失败仍是 best effort，不否定已恢复的 active index。

但不能把当前启动链概括为“所有恢复错误都 fail-fast”：

- `main` 调用 `kvs_rdb_load()` 和 `aof_load()` 时没有检查返回值；
- RDB 向量段末尾会调用一次 `kvs_vector_rebuild_after_load()`，main 在 AOF 后又调用一次；
- 因此 RDB-only + missing snapshot 可能先 rebuild/write，然后第二次加载刚写的 snapshot，结果正确但重复工作，且可覆盖第一次 fallback 观测状态。

这些是恢复控制面仍需收敛的真实缺口。

## 21.11 `index_ready` 可观测，但还不是完整的生产门禁

VINFO 可以看到 `backend_size`、`core_hnsw_v2_size`、`core_hnsw_v2_active_count`、`index_ready`，以及 snapshot attempted/valid/loaded/fallback/error 状态。[`persistence recovery 产品测试`](../../tests/vector_command_persistence_recovery_test.sh) 对小规模 RDB-only、AOF-only 和 RDB+AOF 场景显式断言 `active_records=backend_size=core_hnsw_v2_active_count=3`，并检查 VGET/VSEARCH、update/delete 与缺失 snapshot fallback。审计报告记录这些场景 PASS。

然而生产主启动路径并没有一条显式断言：

```text
active_records == backend_size == core_hnsw_v2_active_count
```

同步 records-only rebuild 会遍历 active embeddings，任一 add 失败会返回错误，main 会检查 rebuild 返回值；这提供了基本门禁，但不等于独立物理计数一致性断言。而且 VINFO `index_size` 是逻辑 `active_records`，不能用它单独证明底层 graph 完整。

因此本阶段是“恢复链已接入、小规模一致性基本证明”，不是“任意损坏、任意规模下已形成完整 READY 状态机”。

## 21.12 测试和证据等级

| 证据 | 历史结果 | 能证明 | 不能证明 |
| --- | --- | --- | --- |
| [`Stage 1 C test`](../../tests/unified_config_stage1_test.c) + [`Stage 1 report`](../configuration/unified_configuration_stage1_report.md) | PASS | schema、strict parse、source、file/CLI precedence | 所有业务 consumer 都已迁移 |
| [`Stage 2 C test`](../../tests/unified_config_stage2_test.c) 与 [`integration test`](../../tests/unified_config_stage2_integration_test.py) + [`Stage 2 report`](../configuration/unified_configuration_stage2_report.md) | PASS | server/storage/TTL/persistence 配置和 cwd recovery | 任意损坏持久化文件都 fail-fast |
| [`Stage 3 config test`](../../tests/unified_config_stage3_test.c)、[`runtime test`](../../tests/unified_config_stage3_runtime_test.c)、[`vector-disabled test`](../../tests/unified_config_stage3_vector_disabled_test.sh) + [`Stage 3 report`](../configuration/unified_configuration_stage3_report.md) | PASS | vector/embedding/snapshot/rebuild 参数被实际消费 | 跨版本 snapshot 长期兼容 |
| [`Stage 3 ONNX test`](../../tests/unified_config_stage3_onnx_test.sh) + Stage 3 report | PASS | 支持 ONNX 的 build 中 local_c 配置可进入 runtime | 所有模型都与当前 schema 兼容 |
| [`Stage 4 test`](../../tests/unified_config_stage4_test.c) + [`Stage 4 report`](../configuration/unified_configuration_stage4_report.md) | PASS；eBPF 真实 kprobe SKIP | TCP/eBPF config gate 和已有 TCP 回归 | eBPF kernel path 已运行通过 |
| [`persistence recovery test`](../../tests/vector_command_persistence_recovery_test.sh) + [`functionality audit`](../vector_command_functionality_audit.md) | PASS | 小规模 RDB/AOF/snapshot fallback 与 V 命令恢复 | 生产规模、长时间、向量复制 |

以上 PASS 是提交内阶段报告记录的历史执行结果，不是本次文档审计重跑的结果。本章没有性能、时延、吞吐或内存新数字；第二十章的 T2Ranking 只是配置漂移会在真实规模暴露的背景，本章不重复消费其 benchmark 结果。

## 21.13 复制前置审计：配置入口存在，协议能力不存在

[`Vector Replication Stage 0 审计`](../replication/vector_replication_stage0_audit.md) 确认了一个必须保留的边界：当时 `VSET/VDEL` 没有进入向量复制链，不存在 vector mutation codec/apply、repl offset、ACK、backlog、partial resync、promotion recovery 或 replica READY 状态机。Stage 4 实现的是已有 TCP replication/eBPF 的配置收口，以及对未来字段的 fail-fast；`full_sync_policy` 和 `not_ready_query_policy` 只是冻结的政策入口，不是已运行的 READY gate。

这个审计对本章的意义是：统一配置必须拒绝“写下就算支持”，但它不能代替数据面实现。复制一致性和向量主从同步尚未实现，本章只冻结配置入口、能力边界与前置审计。

## 21.14 失败、返工与当前缺陷

这个阶段的价值不在于“字段都搬完了”，而在于把过去会被默默掩盖的错误变成明确契约：

- 旧 file/CLI/default merge 顺序不一致，改为一次 merge 后才 derive；
- 配置字段存在但 consumer 仍用硬编码，以 Stage 2/3/4 逐组迁移修正；
- 已移除 backend 的静默 fallback 曾使实际索引与文本不同，现在主启动 validate 拒绝；
- snapshot manifest v2 加入 model content identity 后，v1 成为 incompatible，按 `invalid_action` 失败或 rebuild；
- RDB 格式没有修改，因此 RDB record 无法单独证明历史 metric；legacy model identity 仍可能警告接受；
- main 忽略 RDB/AOF load return、vector rebuild 可重复执行、物理 index-count gate 不完整；
- `g_cfg` 只是逻辑冻结，不是结构上不可变；仍存在局部 env/hardcode 债务；
- dynamic destructive HNSW growth 的失败原子性未在本提交解决；
- vector replication 仍只有审计和设计边界。

可选的宽松方案是对无效值警告后继续、由各模块自行 fallback。它对老配置更容忍，但无法证明文本与运行时相同，对 persistence 和 derived index 尤其危险。当前选择严格失败，代价是升级时需要显式迁移旧配置和 snapshot。

## 21.15 Agent 辅助边界与面试表达

### Agent 辅助边界

可以让 Agent 并行检索重复默认值、环境变量、硬编码路径和未消费字段，生成 parser 边界测试与配置迁移表。但以下决策必须由工程负责人定义并验收：真值与派生状态的边界，哪些旧行为可兼容、哪些必须 fail-fast，恢复顺序，snapshot invalid policy，以及“配置入口”和“已实现能力”的证据线。不能根据代码风格推断逐行作者。

### 1 分钟版本

> 随着 embedding、HNSW、snapshot 和 rebuild 接入，项目出现了“配置被读到但没真正生效”的风险。我们在 `7cd837f` 里用 descriptor schema 统一类型、默认值、状态和来源，固定 default < file < CLI，最终 merge 后只 derive/validate 一次。启动时先拒绝编译能力、backend、dimension、metric、HNSW 和路径冲突，再恢复 RDB/AOF records，验证加载 snapshot 或从 records rebuild。关键边界是 snapshot valid 不等于 loaded，records 恢复不等于 index ready。当前仍有 load 返回值被忽略、重复 rebuild 和缺少完整物理计数门禁的债务；向量复制只完成审计，没有实现。

### 3～5 分钟版本

1. **背景**：先讲旧配置的三类漂移：重复默认值、parser 与 consumer 脱节、cwd 路径不稳定。
2. **合并模型**：说明 descriptor + source metadata，default < file < CLI，重复值拒绝，merge 完才 derive，从而避免派生值提前冻结。
3. **门禁**：区分 parse 类型检查、cross-field validate、compile capability 检查和 provider runtime init；说明为什么 removed/future 字段必须失败。
4. **恢复链**：RDB/AOF 恢复 records，重建 manager map，验证 embedding/model，然后 snapshot load 或 records rebuild。强调 graph 仍是 derived cache。
5. **证据与不足**：Stage 1～4 报告与小规模 persistence recovery 已 PASS，但主链忽略 RDB/AOF load return、rebuild 可重复、缺少显式物理 parity gate，eBPF 真实测试跳过，vector replication 尚未实现。

### 常见追问

**为什么不允许无效配置自动 fallback？**

因为 backend、dimension、metric 和 model 会改变持久 records 的解释和派生索引的兼容性。静默 fallback 会让运行时与操作者认知分叉，fail-fast 更易定位且不会在错误 backend 上接受流量。

**配置什么时候算生效？**

不是 parser 返回成功时，而是 file/CLI merge、derive、cross-field validate 通过，并被实际 consumer 初始化路径使用后。source metadata、启动日志和测试共同证明这一点。

**为什么 snapshot valid 还不够？**

manifest 兼容只说明文件头和当前配置/records 匹配，之后还有 deserialize、graph validation、record validation 和 bind。只有全链成功才是 loaded。

**恢复成功是否已证明索引完全一致？**

还没有。小规模测试会检查三组计数和查询行为，但生产主启动没有独立的 `active_records == physical index count` 硬断言，也没有覆盖任意损坏与长期运行。

**复制配置字段已经有了，为什么还说向量复制没实现？**

因为 schema 只能表达和拒绝组合，不能生成 mutation codec、apply、offset/ACK/backlog、partial resync 或 READY 状态机。Stage 0 文档是审计和设计边界，不是实现证据。

### 本章小结与下一阶段边界

`7cd837f` 将配置从分散文本和模块默认值提升为有 schema、source、derive 和 validate 的启动 contract，并让 vector/snapshot/rebuild 实际消费这份 effective config。它形成了当前的恢复闭环：records 是真值，snapshot 是派生 cache，不兼容时按明确策略失败或 rebuild。

证据边界同样清楚：已实现统一配置与单机恢复接入；小规模回归已有历史 PASS；但配置对象不是结构性不可变，恢复错误仍有未统一处理的路径，完整 index-ready parity gate 未形成，向量复制尚未落地。

下一阶段应等待真实实现闭环后再成章，候选方向是“复制 Stage 1A：向量 mutation 的可持久编码与从库 apply 契约”。当前 Git 历史只有审计/设计，不填写 commit 范围，也不在本章提前展开 repl offset、ACK、backlog、partial resync 或 TTL 复制。
