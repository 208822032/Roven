# Unified Configuration Stage 0: 项目配置现状审计与统一方案冻结

## 1. 范围与结论

本阶段只审计并冻结设计，不修改生产代码、测试、Makefile 或现有 `config/*.conf`。名义上的默认入口已经是 `config/kvstore.conf`，但当前并非统一配置体系：主进程只把一部分运行参数汇入 `g_cfg`，eBPF daemon 独立解析另一份文件，多个模块仍直接读取环境变量或硬编码常量，测试和 benchmark 还生成自己的配置片段。

项目级配置来源共七类；其中前六类会影响生产构建或运行，第七类只影响测试/benchmark：

1. Makefile 变量和编译宏。
2. C 内置默认值及模块硬编码常量。
3. 主进程 `key=value` 配置文件。
4. 主进程命令行覆盖。
5. 生产进程直接读取的环境变量。
6. `ebpf_syncd` 独立配置文件和位置参数。
7. 测试/benchmark 生成的临时配置及脚本环境变量。

冻结目标是：

```text
build capabilities
  + built-in runtime defaults
  + one main config file
  + explicit CLI overrides
  -> validate once
  -> immutable effective kvs_config_t + per-field source metadata
  -> modules only consume that object
```

编译能力不参与运行时优先级；它只限定配置可启用的能力集合。

## 2. Stage 0 审计时的入口与调用链

以下调用链是 Stage 0 发现的问题基线；Stage 1 后的现状见第 13 节和 Stage 1 实现报告。

### 2.1 主服务

```text
main
  -> kvs_config_init_default(&g_cfg)
  -> kvs_config_apply_cmdline()             # 第一次，既取 config path 又修改值
       -> 还会提前派生 sync_port
  -> kvs_config_load_file(explicit path)
       or kvs_config_load_file("config/kvstore.conf")
  -> kvs_config_apply_cmdline()             # 第二次，保证 CLI 覆盖
       -> 再次尝试派生 sync_port
  -> 模块直接读取 g_cfg
  -> 少数模块再读 getenv() 或使用本地常量
```

当前优先级表面上是“默认 < 文件 < CLI”，但第一次 CLI 解析已经把默认 master 的 `sync_port` 固化为 `8080 + 1000`。后续配置若只改变 role/port/master_port 而未显式写 `sync_port`，第二次解析不会重新派生，因此实际结果可能不是最终配置对应的端口。

显式配置文件打开失败时只记录日志并继续使用默认/CLI；默认文件打开失败甚至不记录专门错误。未知 key、无 `=` 行、非法枚举、缺失 CLI 参数和多数非法数值会被静默忽略、保留旧值或夹到边界。

### 2.2 eBPF daemon

```text
ebpf_syncd main
  -> kvs_config_init_default(&g_cfg)         # 仅给 mem_init 间接使用
  -> positional argv[1]
       or "../../config/ebpf_sync.conf"
  -> load_daemon_conf()                     # 独立 parser / daemon_conf_t
  -> hard-coded queue/buffer/object settings
```

主配置中的 `ebpf_conf` 会写入 `g_cfg`，但没有生产读取点，也不会启动或配置 `ebpf_syncd`。daemon 默认路径、`ebpf_kern.o` 查找和当前工作目录绑定。

### 2.3 测试与 benchmark

至少 27 个 shell/Python 文件生成临时 `.conf` 或 benchmark 专用配置。它们重复维护 port、sync_port、persistence、network、embedding 和 vector 字段；部分脚本又用环境变量覆盖 host/port/backend/dim。临时文件本身是合理的测试隔离方式，但字段定义和校验必须复用统一 schema，不能继续复制 parser 语义。

## 3. 当前配置来源矩阵

### 3.1 `g_cfg` 字段

| 配置项 | 当前来源/内置默认 | 使用模块 | 重复或漂移 | 是否真正生效 |
| --- | --- | --- | --- | --- |
| `port` | default `8080`; file; CLI `--master` | network/main | 四份主配置端口不同 | 是 |
| `role` | default `master`; file; `--slave`/`--master` | main/replication/write guard | `master.conf`/`slave.conf` 独立 | 是 |
| `master_ip`, `master_port` | empty/`0`; file; `--master` | TCP slave validation/派生 | eBPF 另有同名字段 | 部分；连接实际使用 `sync_port` |
| `sync_port` | default `0`, CLI parser 派生；file | TCP distribution | 两次 CLI 解析可过早固化；master/slave 注释含义不一致 | 是，但缺省派生不可靠 |
| `enable_aof` | compile macro `ENABLE_AOF=1`; file | main/AOF | build capability 与 runtime switch 混在同名概念 | 是 |
| `aof_fsync_mode`, `aof_filename` | `everysec`, `data/kvstore.aof`; file/CLI | AOF | 各 profile 使用不同路径/策略 | 是 |
| `rdb_filename` | `data/kvstore.rdb`; file | startup/shutdown RDB | 没有 runtime `enable_rdb`，始终受编译宏控制 | 是 |
| `dump_filename` | `data/kvstore.dat`; file | 无 | 所有主配置都保留旧字段 | 否，无消费点 |
| `network_mode` | compile constant `NETWORK_SELECT=reactor`; file | main/network/AOF path | header 注释仍称占位；各网络实现又有不同硬编码限制 | 是 |
| `mem_backend` | `pool`; file | memory init | Makefile 总是链接 jemalloc，能力未显式校验 | 是 |
| `max_value_size` | `512 KiB`; files多为 `1/10 MiB` | 原计划 command validation | 两处检查已注释；RESP 另有 `10 MiB` 限制 | 否 |
| `sync_mode` | `tcp`; file | main/network/command propagation | Makefile 仍传无实际消费者的 `SYNC_MODE`; eBPF daemon另配 | 是 |
| `log_level`, `log_async`, `log_file` | `info`, off, stderr; file/CLI | log init | async queue/line/batch仍硬编码 | 是；非法 level 回退 info |
| `ebpf_conf` | `config/ebpf_sync.conf`; file | 无 | daemon 独立收位置参数 | 否 |
| `embedding_provider` | `hash`; file/CLI | vector embedding | 任意非 `local_c/c_api` 值都落到 hash | 是，但非法值静默降级 |
| `embedding_model_path` | empty; file/CLI | local C runtime | vocab 路径由模型目录隐式拼出；record中的model version只hash provider/path/dim，不校验模型或词表内容 | local_c 时生效 |
| `embedding_dim` | `64`; file/CLI | local C runtime/model ID | hash provider固定 64，忽略该值；没有独立 index dimension field | local_c 时生效 |
| `embedding_num_threads`, `embedding_inter_threads` | `1`, `0`; file/CLI | ONNX runtime | build 必须选择 ONNX implementation | local_c/ONNX 时生效 |
| `embedding_max_text_len` | `4096`; file/CLI | runtime | ONNX 同时拿它作 text limit 和 token count，再硬夹到 512 tokens | 是，但语义混合 |
| `embedding_ort_graph_opt_level`, `embedding_ort_execution_mode` | `basic`, `sequential`; file/CLI | ONNX runtime | 非法字符串由 runtime 再解释/报错 | ONNX 时生效 |
| `embedding_worker_pool_enabled`, `embedding_worker_threads` | off, `1`; file/CLI | executor | 与 model intra-op threads 可形成资源乘积 | 是 |
| `embedding_worker_queue_capacity`, `embedding_prepare_queue_capacity` | 都为 `1024`; file/CLI | executor只读前者 | 两个别名按出现顺序互相覆盖 | 是，但重复定义 |
| `embedding_per_thread_runtime_enabled`, `embedding_per_thread_runtime_fallback_mutex`, `embedding_shared_ort_env_enabled` | off/on/on; file/CLI | embedding runtime | 调试/兼容策略混在模型配置 | 是 |
| 所有 `embedding_batch_*` | 多组内置默认；file/CLI | 仅 parser/结构/统计字段 | worker 没有读取这些 `g_cfg` 字段 | 否，当前是死配置 |
| `embedding_cache_enabled/capacity/ttl_ms/policy` | off/10000/3600000/lru; file/CLI | embedding cache | 仅支持 lru，非法值静默保留 | 是 |
| `vector_index_type` | built-in `core_hnsw_v2`; file/CLI | backend resolver | 三份现有配置仍写 removed `hnsw`/`hnswlib_hnsw` | 是；旧名称会降级 `core_flat` |
| `vector_hnsw_v2_experimental_enabled` | off; file/CLI | VINFO telemetry | 不再控制 v2 选择 | 仅观测兼容字段 |
| `vector_metric` | `ip`; file/CLI | index/manager | 接受 `l2/cosine`，没有统一模型兼容 gate | 是 |
| `vector_hnsw_m` | config default `32` | v2 configure | v2 最小默认 48，配置 32 被静默抬到 48 | 是，但文本值不等于有效值 |
| `vector_hnsw_ef_construction` | config default `128` | v2 configure | v2 最小默认 512，现有 128/256 被静默抬到 512 | 同上 |
| `vector_hnsw_ef_search` | config default `256` | v2 configure | v2 最小默认 1024，现有 128/256 被静默抬到 1024 | 同上 |
| HNSW `M0` | 没有配置项 | v2 configure | 由 `max(2*M, 96)` 硬编码派生 | 生效但不可配置 |
| `vector_hnsw_initial_capacity` | `0`; file/CLI | record store/v2 reserve | `0` 表示最小 16 后动态增长 | 是 |
| `vector_score_threshold` | `0.85`; file/CLI | VSEARCH | 与 command 显式 threshold 共存 | 是 |
| `vector_scope_overfetch_factor` | `4`; file/CLI | scoped search | 现有 master 配置为 1 | 是 |
| `vector_distance_backend` | `auto`; file/CLI | distance dispatch | AVX2 还受架构/编译产物约束 | 是，unsupported 时回退 |

### 3.2 未进入 `g_cfg` 的生产参数

| 配置类 | 当前来源和默认 | 使用模块 | 问题 |
| --- | --- | --- | --- |
| listen address | `INADDR_ANY` | reactor/proactor/ntyco | 不可限制绑定地址 |
| connection/listen limits | reactor/proactor 1024；listen backlog 1024/10 | network | 三种模型不一致 |
| request/response limits | RESP bulk 10 MiB；response 2 MiB；netbuf 64 MiB；batch commands 10,000,000 | protocol/network | 多处重复且口径不同 |
| storage engines/capacity | 四种 engine 编译时全开；array/hash 等各自宏 | storage/main | 无统一 engine/capacity 配置，重复宏值冲突 |
| TTL scanner | always start; interval 1000 ms; budget/candidates 32 | ttl/executor | slave 直接跳过；不可配置 |
| RDB enable/recovery order | `ENABLE_RDB=1`; RDB -> AOF -> vector rebuild | main/persistence | runtime 无开关，load 错误处理不统一 |
| AOF rewrite | buffer 64 MiB, min 28 MiB, growth 64 MiB/50% | AOF headers/source | `include/kvstore.h` 还有另一套 16/64/10 MiB 宏 |
| vector snapshot | cwd 下 `vector_index.manifest/.snapshot`; startup try-load; rebuild后best-effort写 | vector | 不在 data dir，不可禁用/改路径 |
| rebuild delta/retry | initial delta 16, geometric growth; manual command retry/cancel | vector | 无资源上限、自动策略或配置 gate |
| TCP queue/slaves | queue 1,048,576; max slaves 20 | distributed | 不是 backlog；无 offset/readiness 参数 |
| eBPF daemon | 独立 master/slaves/max_payload file | ebpf_syncd | 与主配置重复且可漂移 |
| eBPF internals | queue 65,535; max slaves 10; chunk 4096; tx 128 KiB; flush 10 ms | ebpf_syncd/kernel | 配置文件里的 `max_payload` 只控制 FULLDUMP recv buffer，不控制 ring size |
| HNSW profiling | build/search wall-clock profiling init 为 on | vector core | 生产无配置；benchmark shim才可关闭 |
| V command profiling | compile macro `KVLITE_VCMD_PROFILE` | command profiling | 不是 runtime 配置 |
| owner lock mode | env `KVLITE_ROUTE_C_WEAK_RWLOCK`, default weak | owner lock | 模块绕过统一配置 |
| ONNX profiling | env `KVLITE_EMBEDDING_PROFILE` | embedding runtime | 模块绕过统一配置 |
| log writer limits | queue 256, line 2048, flush batch 64 | logging | `log_async` 只有总开关 |

### 3.3 编译能力

| 能力 | 当前来源 | 当前语义 |
| --- | --- | --- |
| ONNX runtime | `ENABLE_ONNX_EMBEDDING`，默认 0 | 选择 `embedding_runtime.c` 或 `_onnx.c`; runtime 配 `local_c` 不能证明二进制具备 ONNX |
| eBPF binaries | `ENABLE_EBPF`，默认 1 | 只决定是否构建 daemon/object，不由主配置启动 |
| storage/RDB/AOF | header `ENABLE_*` 宏 | 与 runtime fields 混杂，且多个 header 重复定义 |
| AVX2 | architecture-derived `-mavx2` | runtime `auto/avx2` 仍可能回退 |
| V command profiling | `KVLITE_VCMD_PROFILE` | compile-out/on；当前 Makefile 无统一 runtime switch |
| `SYNC_MODE` | Makefile `-DSYNC_MODE=$(SYNC_MODE)` | 当前生产同步选择使用 `g_cfg.sync_mode`; 该宏没有有效消费者 |
| FAISS/hnswlib flags | 多个脚本传 `ENABLE_FAISS/HNSWLIB` | 当前 Makefile/product backend不消费，FAISS只在独立 benchmark target使用 |

## 4. 现有文件漂移与冲突

| 文件 | 主要用途 | 已确认漂移/风险 |
| --- | --- | --- |
| `config/kvstore.conf` | 名义默认入口 | `vector_index_type=hnsw` 已移除，实际降级 core_flat；路径依赖 cwd；`max_value_size` 无效 |
| `config/kvstore-embedding.conf` | ONNX 示例 | `hnswlib_hnsw` 已移除，实际降级 core_flat；模型、vocab和编译能力无统一 preflight；含全部死 `embedding_batch_*` |
| `config/master.conf` | master 示例 | `hnsw` 降级 core_flat；`sync_mode=ebpf` 但 `ebpf_conf` 不生效；port 9300 与 eBPF 文件 master_port 8080 不一致 |
| `config/slave.conf` | slave 示例 | 未配置 vector/embedding，继承 built-in v2/hash，与 master 实际 core_flat/hash 不同；eBPF endpoint来自另一文件 |
| `config/ebpf_sync.conf` | daemon 独立入口 | IP/port 是环境特定值；不受主配置优先级和校验约束；`max_payload` 名称容易被误认为 ring buffer size |

TCP 与 eBPF 在主服务中由 `sync_mode` 二选一，但这不是完整互斥门禁：外部仍可同时启动 `ebpf_syncd` 和 TCP replication，两个进程不共享 effective config，也无法证明不会向同一 replica 重复写入。

## 5. 冻结的统一 schema

目标入口为 `config/kvstore.conf`。语法沿用 `key = value`、`# comment`，只增加 INI section；canonical key 表示为 `section.key`。Stage 1 已实现 section parser、descriptor schema、legacy alias 和严格失败；尚未接线字段显式配置时按 `unsupported` 或 `future` 拒绝。

### 5.1 Section 与关键字段

| Section | 冻结字段 | 说明 |
| --- | --- | --- |
| `[server]` | `role`, `config_root`, `data_dir`, `test_mode` | role保留 `master/slave`; standalone由 replication disabled 表示；test_mode生产默认 false |
| `[network]` | `listen_address`, `port`, `mode`, `worker_threads`, `max_connections`, `listen_backlog`, `max_request_bytes`, `max_response_bytes`, `max_commands_per_read` | 三种网络模型使用同一限制；unsupported线程模型需报错 |
| `[storage]` | `default_engine`, `enabled_engines`, `memory_backend`, `array_initial_capacity`, `hash_initial_capacity`, `max_value_bytes` | 编译能力不足时拒绝；`max_value_bytes` 必须真正进入协议/command gate |
| `[ttl]` | `enabled`, `scan_enabled`, `scan_interval_ms`, `scan_budget` | slave是否执行由复制语义决定，不能模块内静默跳过而无日志 |
| `[persistence]` | `rdb_enabled`, `rdb_path`, `aof_enabled`, `aof_path`, `aof_fsync`, `aof_rewrite_enabled`, rewrite thresholds, `startup_recovery_order`, `legacy_dump_path` | recovery order只允许受支持枚举；legacy dump只为迁移保留 |
| `[vector]` | `enabled`, `index_type`, `dimension`, `metric`, `normalize`, `hnsw_m`, `hnsw_m0`, `hnsw_ef_construction`, `hnsw_ef_search`, `initial_capacity`, `default_topk`, `default_threshold`, `scope_overfetch_factor`, `distance_backend` | `core_hnsw_v2` 是内部值；用户文档称 KVLite HNSW |
| `[embedding]` | `provider`, `model_path`, `vocab_path`, `output_dimension`, `model_version`, `intra_op_threads`, `inter_op_threads`, `worker_pool_enabled`, `worker_threads`, `worker_queue_capacity`, `batch_enabled`, `batch_size`, `batch_max_wait_us`, `max_text_bytes`, `max_tokens`, `cache_*`, ORT options | vocab显式化；文本字节限制与token cap分开；normalize以vector为唯一语义 |
| `[vector_snapshot]` | `enabled`, `manifest_path`, `index_path`, `load_on_start`, `invalid_action`, `write_after_rebuild`, `cross_node_reuse` | 默认 invalid_action=rebuild；graph仍是派生状态 |
| `[vector_rebuild]` | `async_enabled`, `delta_initial_capacity`, `delta_max_entries`, `retry_policy`, `cancel_enabled`, `write_snapshot_after_publish` | 不改变现有VINDEX协议；仅控制能力和资源边界 |
| `[replication]` | `transport`, `master_host`, `master_service_port`, `master_sync_port`, `sync_listen_port`, `full_sync_policy`, `not_ready_query_policy`; future `backlog_bytes`, `ack_interval_ms`, `reconnect_backoff_ms` | transport仅 `disabled/tcp`; future fields在实现前不得伪装生效 |
| `[ebpf]` | `mode`, `object_path`, `observe_host`, `observe_port`, `mirror_targets`, `ring_buffer_bytes`, `max_event_bytes`, `user_queue_capacity`, `flush_interval_us` | mode仅 `disabled/observe/experimental_mirror` |
| `[observability]` | `log_level`, `log_path`, `log_async`, `log_queue_capacity`, `vinfo_stats_enabled`, `vsearch_stage_stats_enabled`, `replication_stats_enabled`, `embedding_profile_enabled`, `hnsw_build_profile_enabled`, `hnsw_search_profile_enabled`, `owner_lock_mode` | 测量开关必须显式记录且不进入持久化 |

### 5.2 当前有效行为对应的兼容默认

Stage 1 的 built-in defaults必须优先保持当前有效行为，而不是照抄失真的文本值：

- `network.mode=reactor`, `network.port=8080`, `server.role=master`。
- `storage.memory_backend=pool`, `storage.max_value_bytes`在真正接线前标记 unsupported，不能继续假装生效。
- `vector.enabled=true`, `vector.index_type=core_hnsw_v2`, `vector.dimension=64`, `vector.metric=ip`。
- v2 effective defaults为 `M=48`, `M0=96`, `efConstruction=512`, `efSearch=1024`；不再用 32/128/256 再静默抬高。
- `embedding.provider=hash`; hash维度必须与 `vector.dimension`一致或在校验期报错。
- TTL interval 1000 ms/budget 32、rebuild delta initial 16作为兼容默认。
- snapshot默认启用，但路径迁到 `server.data_dir`；迁移阶段必须处理旧 cwd 文件。
- replication transport在兼容期保持 tcp；新示例单机显式写 disabled，避免意外开放复制端口。

## 6. 优先级、解析和路径规则

### 6.1 唯一优先级

```text
built-in defaults
  < main config file
  < explicit CLI overrides
```

实现约束：

1. 启动只先扫描 `-c/--config`，不得在加载文件前修改任何业务字段或派生值。
2. 文件只解析一次；CLI通过与文件相同的 field descriptor/validator 覆盖，不维护第二条 if/else 字段列表。
3. 所有派生值在 merge 后统一计算一次，例如 sync port、M0、effective worker/thread resource product。
4. 每个字段记录 `default/file:line/cli` 来源；启动日志输出关键 effective value和来源，敏感值脱敏。
5. 未知 section/key、重复 key、缺失 value、溢出、尾随垃圾和非法枚举必须失败。
6. 显式配置文件不可读必须失败；默认入口不可读也必须给出明确错误，是否允许纯默认启动由显式 `--no-config` 决定。
7. 环境变量不新增为第四层。现有两个生产 env开关迁到 `[observability]`，兼容期只作为deprecated alias且低于CLI、给出warning。
8. 编译宏只提供 capability manifest；配置请求未编译能力时启动失败。

### 6.2 路径规则

- 首先规范化显式配置文件的绝对路径；`server.config_root` 默认是该文件所在目录。
- `server.data_dir` 相对 `config_root` 解析并规范化；RDB/AOF/snapshot/log等运行数据路径相对 `data_dir`。
- model/vocab/eBPF object等静态资产路径相对 `config_root`；绝对路径保持绝对。
- CLI传入的相对路径使用相同基准，不得改为进程 cwd。
- 启动日志打印规范化后的路径；创建目录前校验类型、权限和路径冲突。
- 禁止依赖“从仓库根启动”或 daemon 自行尝试多个 cwd 相对位置。

## 7. 启动前校验门禁

### 7.1 通用

- port、线程、容量、队列、字节数使用无溢出的严格整数解析和明确范围。
- `network.mode` 必须由当前平台/build支持；listen address合法；端口不冲突。
- 启用的 storage engine必须已编译；memory backend必须已链接并可初始化。
- RDB、AOF、snapshot、manifest、log不得解析为同一文件；父目录必须可读写。
- disabled feature的已知字段允许保留为profile值，只做语法校验并在启动日志标为inactive；不得隐式启用，也不得假装生效。

### 7.2 Vector/embedding

- `vector.enabled=true` 时 provider、dimension、metric和index type必须完整合法。
- `local_c` 要求 ONNX capability、model和vocab均存在可读；session输出维度必须等于 `embedding.output_dimension` 和 `vector.dimension`。
- `vector.normalize=true` 为当前semantic path固定契约；恢复记录的model/version/dimension/metric不兼容时拒绝READY。
- `M > 0`, `M0 >= M`, `efConstruction >= M0`, `efSearch > 0`, initial capacity > 0；不再静默clamp。
- removed product backend名称不再静默映射flat。兼容阶段给deprecated错误/明确迁移提示；最终只接受 `core_flat/core_hnsw_v2`。
- snapshot参数必须与 records checksum、backend、dimension、metric和HNSW参数一致；invalid action只能fail或rebuild。

### 7.3 Persistence/rebuild

- recovery order只接受冻结的 `rdb_then_aof_then_index`；改变顺序不是普通调参。
- AOF disabled时fsync/rewrite字段标inactive；RDB disabled时不得在shutdown仍保存。
- rebuild delta max必须不小于initial；retry/cancel策略要与当前state machine能力一致。

### 7.4 Replication/eBPF

- slave + TCP要求master host和master sync port；master要求sync listen port。
- replica在模型/version/dimension/metric与master不兼容时不得进入READY。
- `not_ready_query_policy` 当前只允许未来实现的`reject`; 不允许部分index静默服务。
- offset/ACK/backlog尚未实现前，配置这些future字段必须报unsupported，而不是记录后忽略。
- eBPF capability未编译或无权限时，`observe/experimental_mirror`启动失败。

## 8. TCP/eBPF互斥规则

冻结模式：

```text
replication.transport = disabled | tcp
ebpf.mode             = disabled | observe | experimental_mirror
```

规则：

1. `ebpf.mode=observe`只观测，不发送写入，可与TCP共存。
2. `experimental_mirror`不是可靠复制，不得与TCP向同一target/dataset写入。
3. 启动校验把TCP target和eBPF mirror target规范化后求交；非空即失败。
4. eBPF不能声明offset/ACK/backlog或READY语义；实验结果必须单独标记。
5. 旧 `sync_mode=ebpf` 迁移为 `replication.transport=disabled` + `ebpf.mode=experimental_mirror`，并强制显式targets；不能静默启动外部daemon。

## 9. 旧配置迁移表

| 旧名称/来源 | 新名称 | 兼容方式 | 提示 | 删除阶段 |
| --- | --- | --- | --- | --- |
| `port` | `network.port` | Stage 1-5 alias | deprecated key + source | Stage 6 |
| `role` | `server.role` | alias，值仍为master/slave | standalone用replication disabled | Stage 6 |
| `network_mode` | `network.mode` | alias | 输出effective mode | Stage 6 |
| `mem_backend` | `storage.memory_backend` | alias | capability校验 | Stage 6 |
| `max_value_size` | `storage.max_value_bytes` | alias | Stage 2接线前标unsupported | Stage 6 |
| `master_ip` | `replication.master_host` | alias | slave only | Stage 6 |
| `master_port` | `replication.master_service_port` | alias | 不再隐式混同sync port | Stage 6 |
| `sync_port` | role相关的 `replication.sync_listen_port/master_sync_port` | Stage 1按role转换 | 要求日志显示转换结果 | Stage 6 |
| `sync_mode=tcp` | `replication.transport=tcp`, `ebpf.mode=disabled` | alias | 无 | Stage 6 |
| `sync_mode=ebpf` | replication disabled + eBPF experimental mirror | descriptor alias转换；必须同时提供当前schema所需object/source/targets | high-severity warning | Stage 6 |
| `ebpf_conf` + 独立文件 | `[ebpf]`字段 | 仅保留inactive废弃提示，不再导入第二份业务配置 | daemon只加载主配置 | Stage 6 |
| `enable_aof` | `persistence.aof_enabled` | alias | 无 | Stage 6 |
| `aof_fsync_mode` | `persistence.aof_fsync` | alias | 无 | Stage 6 |
| `aof_filename` | `persistence.aof_path` | alias + 新路径规则 | 打印旧/新绝对路径 | Stage 6 |
| `rdb_filename` | `persistence.rdb_path` | alias + 新路径规则 | 同上 | Stage 6 |
| `dump_filename` | `persistence.legacy_dump_path` | 只读兼容 | 当前未生效 | Stage 6 |
| `log_level/async/file` | `observability.log_*` | alias | 无 | Stage 6 |
| `embedding_provider` | `embedding.provider` | alias | 非法值不再回退hash | Stage 6 |
| `embedding_model_path` | `embedding.model_path` | alias | vocab显式或兼容推导并warning | Stage 6 |
| `embedding_dim` | `embedding.output_dimension` + `vector.dimension` | 旧值同时填两者 | 不一致fail | Stage 6 |
| `embedding_max_text_len` | `embedding.max_text_bytes`; token cap兼容为min(value,512) | alias | 建议显式max_tokens | Stage 6 |
| `embedding_num_threads/inter_threads` | embedding intra/inter op threads | alias | resource product日志 | Stage 6 |
| `embedding_ort_*` | 同section短名称 | alias | strict enum | Stage 6 |
| `embedding_worker_*` | embedding worker fields | alias | 无 | Stage 6 |
| `embedding_prepare_queue_capacity` | `embedding.worker_queue_capacity` | 与worker alias二选一 | 两者同时出现即fail | Stage 6 |
| `embedding_per_thread_runtime_*`, `embedding_shared_ort_env_enabled` | 对应embedding runtime字段 | alias | 记录effective runtime/session数量 | Stage 6 |
| `embedding_batch_*` | canonical batch fields | 先标unsupported | Stage 3决定接线或删除，不能继续死配置 | Stage 6 |
| `embedding_cache_*` | embedding cache fields | alias | strict policy | Stage 6 |
| `vector_index_type` | `vector.index_type` | current names alias | removed names明确失败 | Stage 6 |
| `vector_hnsw_v2_experimental_enabled` | 无 | 只读deprecated telemetry | v2已是正式backend | Stage 5 |
| `vector_metric` | `vector.metric` | alias | persistence/replica compatibility校验 | Stage 6 |
| `vector_hnsw_m/ef_*` | vector HNSW fields | alias | 不再clamp；旧低值迁移为effective 48/512/1024并warning | Stage 6 |
| 无旧M0 | `vector.hnsw_m0` | 默认按effective M派生一次并写日志 | 建议显式96 | 不适用 |
| `vector_hnsw_initial_capacity` | `vector.initial_capacity` | 0兼容为16 | 新schema要求正数 | Stage 6 |
| `vector_score_threshold` | `vector.default_threshold` | alias | 无 | Stage 6 |
| `vector_scope_overfetch_factor` | `vector.scope_overfetch_factor` | alias | 无 | Stage 6 |
| `vector_distance_backend` | `vector.distance_backend` | alias | capability日志 | Stage 6 |
| `KVLITE_ROUTE_C_WEAK_RWLOCK` | `observability.owner_lock_mode` | deprecated env alias | startup warning | Stage 6 |
| `KVLITE_EMBEDDING_PROFILE` | `observability.embedding_profile_enabled` | deprecated env alias | startup warning | Stage 6 |

旧 `config/master.conf`、`slave.conf`、`kvstore-embedding.conf` 在Stage 5前保留；它们应成为兼容profile或生成统一配置的示例，不再由模块赋予不同parser语义。

## 10. 分阶段实现与门禁

| Stage | 最小实现 | 最小测试/验收门禁 |
| --- | --- | --- |
| 1: 统一结构和校验接口 | descriptor schema、section parser、source tracking、strict parse、parse-once、CLI merge、capability view | default/file/CLI优先级；未知/重复/非法/溢出/缺文件失败；`sync_port`最终派生；路径单测；旧key warning快照 |
| 2: server/network/storage/persistence | 模块只读effective config；接线listen/limits/TTL/RDB/AOF/rewrite | 三网络启动smoke；max value gate；TTL interval/budget；RDB/AOF/RDB+AOF恢复；路径冲突/权限失败；现有普通KV回归 |
| 3: vector/embedding/snapshot/rebuild | 移除模块硬编码路径；model/vocab/dim gate；HNSW effective参数；profiling开关；死batch字段处理 | hash/local_c；ONNX缺失/维度错；removed backend fail；snapshot有效/缺失/损坏；sync/async rebuild/delta/cancel/retry；V族持久化回归 |
| 4: TCP replication/eBPF | transport统一消费配置；eBPF不再独立解析；互斥和readiness gate | master/slave合法/非法矩阵；TCP/eBPF target冲突；未编译能力；TTL replication；vector replication按其阶段能力；无double apply |
| 5: tests/README/legacy compatibility | 迁移27类临时config生成器；更新启动文档；保留旧文件alias | 所有现有config逐一load+warning；README命令smoke；测试schema helper；无静默字段 |
| 6: 删除重复解析/废弃项 | 删除旧if/else parser、daemon parser、废弃env/keys和无效宏 | `rg`确认模块无文件/getenv读取；旧key明确失败；full build/regression；配置dump与golden一致 |

每个实现阶段都必须保留公开命令、RDB/AOF格式、manager/core边界和现有响应格式。Stage 0不执行这些实现测试。

## 11. 示例配置

目标格式见 `docs/configuration/kvstore.conf.example`。Stage 4 可解析其中未注释字段；future replication 和尚未迁移的 observability 字段继续保持注释且显式配置失败。它是 schema/部署样例，不替换正式 `config/kvstore.conf`。

## 12. Stage 1/2 实现状态

Stage 1 已完成统一配置基础层，详见 `docs/configuration/unified_configuration_stage1_report.md`。Stage 2 已将 server、network、storage、TTL 和 persistence 迁移到最终 effective `g_cfg`，详见 `docs/configuration/unified_configuration_stage2_report.md`。Stage 3 已迁移 vector、embedding、vector snapshot/rebuild 及其直接 profiling 开关，详见 `docs/configuration/unified_configuration_stage3_report.md`。Stage 4 已迁移现有 TCP replication 和 eBPF daemon 运行配置，详见 `docs/configuration/unified_configuration_stage4_report.md`。普通日志和未来复制 offset/ACK/backlog 仍未迁移。

Stage 2 后，RDB/AOF/legacy dump 路径统一按 `config file directory -> config_root -> data_dir` 解析；旧 flat 配置中的 `data/...` 路径会去除重复的 `data/` 前缀并告警。三种网络模型共享 session request/response/command/connection 限制，AOF rewrite 阈值和 TTL scan 参数只从 effective config 读取。`ntyco_defult.c` 是未参与 Makefile 的历史文件，其中硬编码不代表生产消费路径。

正式 legacy 配置继续加载并输出废弃提示。三份 stale profile 中 removed vector backend 已改为其此前实际 fallback 的 `core_flat`；embedding profile 同时移除了未接线的 batch 字段和重复 queue alias，并显式配置 model/vocab/token cap。所有 `embedding_batch_*` 继续以 unsupported 明确拒绝。`config/ebpf_sync.conf` 仅作为历史文件保留，`ebpf_syncd` 不再读取它或接受位置参数。

Stage 3 的模型身份由 provider、模型内容 hash、词表内容 hash、输出维度、normalize 契约和 model-version label 在启动时一次性生成；相同路径内容变化会改变 identity。snapshot manifest 升级为 v2 并校验该 identity，旧 manifest 依据 `invalid_action=fail/rebuild` 处理，RDB/AOF 格式未改变。snapshot/manifest 已迁到 `data_dir`；普通日志路径仍未迁移，因此全局 runtime-path 条目保持部分完成。

Stage 4 后，主服务与 daemon 都通过 `kvs_config_load()` 得到同一 effective schema。TCP 使用拆分后的 `master_sync_port/sync_listen_port`；legacy `sync_port` 按角色转换。`observe` 不执行 FULLDUMP、不创建 sender、不发送复制写入；`experimental_mirror` 仍是无 offset/ACK/backlog/READY 的实验路径，并与 TCP transport 保守互斥。当前 TCP wire/full-sync/apply 行为未改变，向量 VSET/VDEL 仍未复制。

统一配置实现主线在此暂停。Configuration Stage 5/6 等 Replication Stage 1～4 完成后再执行；当前下一阶段为 `Replication Stage 1A — HNSW 写入与容量安全前置门禁`，只处理向量复制所需的失败原子性和 destructive reserve 风险，不新建独立大型 capacity 设计阶段。
