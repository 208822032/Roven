# Lock And Shared State Audit

## 1. 本阶段目标

列出当前 `g_kv_data_rwlock` 的使用点、共享状态访问点、绕锁路径，以及路线 C 中这些路径的未来归属。

## 2. 当前代码现状

`g_kv_data_rwlock` 定义在 `src/core/kvstore.c`：

- `pthread_rwlock_t g_kv_data_rwlock = PTHREAD_RWLOCK_INITIALIZER;`

主加锁点在 `kvs_filter_protocol()`：

- 查 `command_table`
- 根据 `lock_mode` 选择读锁或写锁
- 调用 handler
- `GET/EXIST` 如果 lazy expire 需要删除，会返回 `KVS_CMD_RETRY_WITH_WRITE_LOCK`，外层升级为写锁重试

`command_table` 位于 `src/core/kvs_operation.c`。典型读锁命令：

- `GET/RGET/HGET/SGET/PAPERGET`
- `EXIST/REXIST/HEXIST/SEXIST`
- `VGET/VSEARCH/VINFO`

典型写锁命令：

- `SET/DEL/MOD` 及 R/H/S/PAPER 变体
- `EXPIRE/TTL/PERSIST/PEXPIREAT`
- `RANGE/SORT` 等遍历类命令
- `VSET/VDEL/VINDEX`

## 3. 设计方案

共享状态审计表：

| 路径 | 文件/函数 | 访问资源 | 当前是否持 g_kv_data_rwlock | 未来路线 C 归属 |
|---|---|---|---|---|
| 网络命令入口 | `src/core/kvstore.c::kvs_filter_protocol` | 普通 KV / TTL / vector / HNSW | 是 | commit executor |
| 普通 KV handler | `src/core/kvs_operation.c::handle_set/get/del/mod` | array/rbtree/hash/skiptable | 由外层持锁 | commit executor |
| TTL lazy expire | `src/core/kvs_operation.c::ttl_expire_if_needed_by_structure` | 普通 KV entry 删除 | 由外层读锁升级写锁 | commit executor |
| TTL 命令 | `handle_expire/ttl/persist/pexpireat` | `expire_at_ms` 和删除 | 写锁 | commit executor |
| TTL active expire | `src/core/kvs_ttl_background.c::ttl_scan_once` | 普通 KV 扫描和删除 | 是，写锁 | expire_scan_task |
| VSET | `src/core/kvs_operation.c::handle_vset_command` -> `kvs_vector_set` | vector store / id_map / HNSW / embedding | 写锁 | prepare worker + commit executor |
| VSEARCH | `handle_vsearch_command` -> `kvs_vector_search` | embedding / HNSW / vector records | 读锁 | prepare worker + commit executor search |
| VDEL | `handle_vdel_command` -> `kvs_vector_del` | vector record lazy delete / id_map | 写锁 | commit executor |
| VINFO | `handle_vinfo_command` -> `kvs_vector_info` | vector state | 读锁 | commit executor |
| RDB save | `src/persistence/kvs_rdb.c::kvs_rdb_save` | 普通 KV / vector 遍历 | 函数内不持全局锁 | snapshot_task |
| RDB load | `kvs_rdb_load` | 普通 KV / vector restore | 启动期单线程 | 启动期或 replay_task |
| AOF append | `src/persistence/kvs_aof.c::aof_append` | AOF state，不读 store | 不持全局锁 | executor 生成日志记录，IO thread 写盘 |
| AOF load | `aof_load` -> `kvs_filter_protocol` | 重放命令修改 store | 通过 `kvs_filter_protocol` 持锁 | 启动期或 replay_task |
| AOF rewrite | `src/persistence/kvs_aof_rewrite.c` | 遍历 live store | 使用 `aof_state.data_rwlock`，不是全局 store 锁 | snapshot_task |
| master full sync | `src/distributed/distributed.c::slave_sync_thread` | 直接遍历普通 KV | 否 | snapshot_task 后发送 |
| slave apply | `src/distributed/distributed.c::slave_connect_master` | 直接 set/del/mod 普通 KV | 否 | replicated_command_task |
| vector store | `src/vector/kvs_vector.c` | `g_vector_state.store/id_map/index` | 依赖外层锁 | commit executor |
| HNSW index | `src/vector/kvs_vector_index.c` | ANN graph / record refs | 依赖外层锁 | commit executor 或 snapshot |
| shutdown destroy | `src/core/kvstore.c::dest_kvengine` | 销毁全局 store | 写锁 | executor drain 后销毁 |

绕锁重点：

- master full sync 遍历 store。
- slave apply 直接修改 store。
- RDB save 函数内部不持全局锁。
- AOF rewrite 的锁不是 `g_kv_data_rwlock`。
- vector 内部默认不自带线程安全，依赖外层。

## 4. 不修改范围

阶段 0 不修改代码，不修复任何路径，只输出审计结果和后续任务边界。

## 5. 涉及模块和文件

- `src/core/kvstore.c`
- `src/core/kvs_operation.c`
- `src/core/kvs_ttl_background.c`
- `src/vector/kvs_vector.c`
- `src/vector/kvs_vector_index.c`
- `src/persistence/kvs_rdb.c`
- `src/persistence/kvs_aof.c`
- `src/persistence/kvs_aof_rewrite.c`
- `src/distributed/distributed.c`

## 6. 正确性边界

本阶段的正确性产物是“路径完整性”。如果某条 store/vector/HNSW 访问路径未列入审计，后续删除锁会有隐性数据竞争。

## 7. 性能影响预期

无运行时性能影响。本阶段只做文档和代码阅读。

## 8. 风险点

- 误判某些启动期单线程路径为运行期并发路径。
- 漏掉 AOF rewrite、full sync、slave apply 这类不经过 `kvs_filter_protocol()` 的路径。

## 9. 验证方式

- 使用 `rg "g_kv_data_rwlock|pthread_rwlock|kvs_filter_protocol|kvs_vector_|expire_at|aof_|rdb_|master_sync|slave"` 交叉确认。
- 对每个后台线程入口列出是否访问 store。

## 10. 回滚方式

无代码修改，无需回滚。

## 11. 下一阶段前置条件

必须确认审计表覆盖所有 live store 访问点，才能进入阶段 0.5 的绕锁修复设计。

结论：

```text
TTL 是锁存在的重要原因，但不是唯一原因。
真正原因是多个执行上下文可能同时访问 store/vector/HNSW/TTL。
```
