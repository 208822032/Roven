# Unified Configuration Stage 2 实现报告

## 1. 范围与文件

本阶段将 server、network、storage、TTL 和 persistence 迁移到 Stage 1 生成的最终 effective `g_cfg`。生产代码有修改；公开命令格式、正常 RESP 响应、RDB/AOF 文件格式、向量算法和复制语义未修改。vector、embedding、snapshot/rebuild、replication、eBPF、observability 未在本阶段迁移。

主要实现文件为 `include/kvs_config.h`、`src/core/kvs_config.c`、server/command/response/TTL、三种 network、四种 storage 和 AOF 实现。新增 `tests/unified_config_stage2_test.c`、`tests/unified_config_stage2_test.sh`、`tests/unified_config_stage2_integration_test.py`。

## 2. 有效配置消费链

```text
built-in defaults -> one config parse -> one CLI merge -> derive paths/sync_port
-> validate once -> publish g_cfg
-> server/network/storage/TTL/persistence read g_cfg only
```

启动日志输出 listen、网络限制、持久化开关、规范化绝对路径及来源。迁移模块的静态检查只发现 main 的一次 `kvs_config_load()`；未发现模块重新打开主配置或读取重复环境变量。

## 3. 网络、存储与 TTL

Reactor、Proactor、NtyCo 统一读取 listen address、port、backlog，并共用 `net_session` 的 connection/request/response/commands-per-read 门禁。旧 handler 仍使用 2 MiB 安全工作 buffer，`kvs_response_task_finalize()` 在发布前统一把超过 effective response limit 的结果转换为错误，避免低 limit 下越界。`worker_threads` 当前仅支持 1，其他值启动失败。

plain ARRAY 命令按 `storage.default_engine` 路由；显式 R/H/S/PAPER 命令若 engine 未启用则在加锁和提交前失败。`storage.max_value_bytes` 同样在 task build 阶段覆盖 SET/MOD、R/H/S/PAPER 和 pipeline，失败不会进入 storage、AOF 或 replication post-commit。array/hash initial capacity 由 effective config 初始化；四个 compiled engine 仍初始化，enabled list 是命令 capability gate。

`ttl.enabled=false` 明确拒绝 TTL 命令；`scan_enabled=false` 不启动线程但保留惰性过期。interval/budget 由后台线程读取。required TTL 回归发现并修复四个 `_len` MOD 未清 TTL、hash `_len` SET overwrite 未清 TTL，以及 skiptable `_len` delete 使用未完整初始化 update path 的缺陷。

## 4. 持久化与路径

`rdb_enabled`、`aof_enabled` 控制 startup load/init、运行写入和 shutdown save/fsync；唯一恢复顺序固定为 `rdb_then_aof_then_index`。AOF fsync 和 rewrite enable/threshold 全部读取 effective config，旧 rewrite 宏已移除。disabled 附属字段在 effective dump 中标记 inactive。

RDB、AOF、legacy dump 路径按 `config file directory -> config_root -> data_dir` 解析为绝对路径。旧 alias 的 `data/...` 去除重复 data 前缀并告警，不移动或删除文件。启用路径的父目录、权限、regular-file 类型和三路径冲突在启动前校验。snapshot/log 路径留待后续阶段。

## 5. 测试结果

- strict C11 config compile：PASS。
- Stage 1、Stage 2 unified config tests：PASS；Stage 2 包含 ASan/UBSan config test、三网络、四 engine、limits、TTL、RDB/AOF/fsync/rewrite/cwd recovery。
- ASan/UBSan 四 backend TTL：PASS；`test_ttl_all_backends.sh` 全绿。
- `test_ttl_rdb.sh`、`test_ttl_aof.sh`：PASS，四 backend save/load/replay/rewrite 全绿。
- `vector_command_persistence_recovery_test.sh`：PASS（RDB-only、AOF-only、RDB+AOF）。
- `vector_core_flat_correctness_test.sh`：PASS。
- `test_binary_key`：36 passed，0 failed。
- full build：`make clean && make ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j$(nproc)` PASS。
- static vector-core boundary checks：无本阶段新增边界违规。
- `git diff --check`：PASS。

## 6. 兼容边界与下一阶段

`storage.max_value_bytes` 默认 10 MiB，以保留此前有效的大 value 行为；legacy 128 MiB 测试上限仍可加载，网络输入另受 64 MiB request 上界限制。`FULLDUMP` 是复制全量只读命令，不受 RDB 开关控制。`ntyco_defult.c` 未进入构建，仅作为历史代码保留。

安全不变量：写入上限失败发生在 lock/commit/post-commit 之前；TTL scan 关闭不关闭 lazy expiration；RDB/AOF 开关不改变文件格式；路径规范化不移动或删除旧文件；response limit 不缩小旧 handler 的安全工作 buffer；manager/vector_core 边界和 `kvs_vector_index_ops_t` 未改变。

## 7. 工作树边界

`git diff --check` 最终 PASS。`git status --short` 仍包含进入阶段前已有的 README、Stage 1、replication、V 族审计、向量测试和持久化测试改动；本阶段未清理、回退或提交这些内容。本阶段新增的未跟踪文件仅为 Stage 2 报告及三份 Stage 2 测试，完整状态以最终汇报为准。

下一阶段保持既定路线：`Unified Configuration Stage 3 - 向量、模型、索引快照与异步重建配置迁移`。

```text
server_config_migrated=yes
network_config_migrated=yes
storage_config_migrated=yes
ttl_config_migrated=yes
persistence_config_migrated=yes
runtime_data_paths_migrated=yes
max_value_gate_implemented=yes
business_modules_read_effective_config=yes
development_checklist_updated=yes
ready_for_configuration_stage3=yes
```
