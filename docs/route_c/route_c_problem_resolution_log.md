# Route C Problem Resolution Log

本文档按问题组织（非阶段流水账），记录 Route C 迭代过程中发现和解决的所有问题。

## 1. master full sync live store traversal

- **问题现象**：`slave_sync_thread()` 直接遍历 `global_array/global_rbtree/global_hash/global_skiptable` 发送全量同步数据，与网络 handler、TTL active expire、RDB/AOF 路径并发访问同一节点。
- **根因**：`src/distributed/distributed.c` 中 master full sync 线程不持全局锁遍历 live store。
- **影响范围**：可能与并发写操作冲突，导致数据不一致或 crash。
- **修复方案**：短期（Stage 0.5）持 `g_kv_data_rwlock` 读锁遍历；长期（Stage 10.5）改为 executor `snapshot_task`，在 executor 线程内持 `kvs_owner_rwlock_rdlock` 遍历。
- **验证方式**：`tests/route_c/master_full_sync_stage10_5.sh` — master full sync + shutdown during sync 通过。
- **当前状态**：✅ 已修复。

## 2. slave apply 绕 executor

- **问题现象**：`slave_connect_master()` 解码主节点消息后直接调用各后端 set/del/mod，绕过 `kvs_filter_protocol()` 和全局锁。
- **根因**：slave apply 路径 `src/distributed/distributed.c` 不走统一命令入口。
- **影响范围**：可能与本地读、TTL 扫描、持久化并发冲突。
- **修复方案**：长期（Stage 10.3）封装为 `replicated_command_task`，通过 executor 线程执行，持 `kvs_owner_rwlock_wrlock`。
- **验证方式**：`tests/route_c/replicated_command_task_stage10_3.sh` — 复制命令执行 + stats 验证通过。
- **当前状态**：✅ 已修复。

## 3. RDB save live traversal

- **问题现象**：`kvs_rdb_save()` 直接遍历普通 KV 和 vector section，函数内部不持 `g_kv_data_rwlock`。
- **根因**：`src/persistence/kvs_rdb.c` 中 RDB 快照遍历可能访问被并发释放或修改的节点。
- **影响范围**：RDB 快照不一致或 crash。
- **修复方案**：长期（Stage 10.6）改为 executor `snapshot_task`，在 executor 线程内持 `kvs_owner_rwlock_rdlock` 遍历。
- **验证方式**：`tests/route_c/rdb_snapshot_stage10_6.sh` — RDB save/load 回归通过。
- **当前状态**：✅ 已修复。

## 4. AOF rewrite live traversal / COW snapshot failure

- **问题现象 1**：AOF rewrite 使用 `aof_state.data_rwlock` 和原子读取，但写命令主路径不以该锁保护全局 store，vector rewrite 也直接遍历 `g_vector_state.store`。
- **问题现象 2**：`acquire_cow_snapshot()` 被调用时 `rewrite_in_progress` 还未设为 1，导致首次 rewrite 立即失败（`failed to acquire COW snapshot`）。proactor 路径使用了编译期 `NETWORK_SELECT` 而非运行时 `network_mode`。
- **根因 1**：rewrite 不能作为一致性快照边界。
- **根因 2**：`src/persistence/kvs_aof_rewrite.c` 中状态设置顺序错误 + 条件编译错误。
- **影响范围**：AOF rewrite 不一致；COW snapshot 失败影响 mixed stress。
- **修复方案**：
  - 长期（Stage 10.7）：改为 executor `snapshot_task`，持 `kvs_owner_rwlock_rdlock`；
  - COW 修复：`rewrite_in_progress = 1` 移到 `acquire_cow_snapshot()` 之前；proactor 分支改为运行时 `g_cfg.network_mode` 判断。
- **验证方式**：`tests/route_c/aof_rewrite_snapshot_stage10_7.sh` + `tests/route_c/aofrewrite_cow_snapshot_stage12.sh` 通过。
- **当前状态**：✅ 已修复。

## 5. TTL scanner owner 问题

- **问题现象**：`ttl_scan_once()` 在后台线程中直接读取 live store/TTL metadata。
- **根因**：TTL 扫描线程与 executor 线程并发访问 store。
- **影响范围**：数据竞争。
- **修复方案**：Stage 10.1/10.10 将 TTL scanner 收敛到 executor 线程，`kvs_command_executor_run_ttl_scan()` 持 `kvs_owner_rwlock_wrlock`。
- **验证方式**：`tests/route_c/ttl_executor_stage10_1.sh` + `tests/route_c/ttl_scanner_owner_stage10_10.sh` 通过。
- **当前状态**：✅ 已修复。

## 6. lazy expire owner 问题

- **问题现象**：GET/EXIST 等读命令的 lazy expire 路径可能在网络线程直接删除过期 key。
- **根因**：lazy expire 需要写锁（删除节点），历史上在 `kvs_filter_protocol()` 内做 read-lock → write-lock retry。
- **影响范围**：非 executor 线程修改 store。
- **修复方案**：Stage 10.2 将 lazy expire 统一在 executor 线程内完成（`kvs_command_task_execute_handler` 内的 `KVS_CMD_RETRY_WITH_WRITE_LOCK` 重试路径）。
- **验证方式**：`tests/route_c/lazy_expire_stage10_2.sh` — reactor + proactor lazy expire + shutdown 通过。
- **当前状态**：✅ 已修复。

## 7. ntyco legacy sync path

- **问题现象**：ntyco 使用原始阻塞 `poll/recv/send`，空闲连接可阻塞整个 scheduler，阻止 QUIT 连接和 shutdown 路径完成。
- **根因**：`src/network/ntyco.c` 内连接协程使用 raw blocking I/O。
- **影响范围**：ntyco mixed stress 无法正常退出。
- **修复方案**：替换为 coroutine-friendly `nty_recv/nty_send`；移除 scheduler-blocking raw `poll` loop；添加 `ntyco_request_shutdown()` 唤醒 hook。
- **验证方式**：`tests/route_c/ntyco_mixed_shutdown_stage12.sh` + `tests/route_c/ntyco_executor_stage10_13.sh` 通过。
- **当前状态**：✅ 已修复。

## 8. eBPF replication fallback

- **问题现象**：eBPF 复制路径需要与 executor 所有权模型对齐。
- **根因**：eBPF syncd 后台线程直接操作复制数据。
- **影响范围**：eBPF 模式下数据一致性和 executor 所有权不一致。
- **修复方案**：Stage 10.12 将 eBPF 复制 fallback 路径 executor 化。
- **验证方式**：`tests/route_c/ebpf_replication_executor_stage10_12.sh` 通过。
- **当前状态**：✅ 已修复。

## 9. shutdown owner boundary

- **问题现象**：`dest_kvengine()` 在 shutdown 时访问 store，需要明确所有权边界。
- **根因**：shutdown 路径可能在 executor 线程之外执行。
- **影响范围**：shutdown 时的数据竞争或 hang。
- **修复方案**：Stage 10.9 明确 shutdown owner boundary：先 `kvs_command_executor_stop()` drain executor，再 `dest_kvengine()` 清理。`dest_kvengine()` 的锁操作改为 wrapper（Stage 13），非 executor 线程在 weak-lock 模式下 fallback 到真实锁。
- **验证方式**：`tests/route_c/shutdown_owner_stage10_9.sh` + `tests/route_c/shutdown_flag_atomic_stage12.sh` 通过。
- **当前状态**：✅ 已修复。

## 10. g_shutdown_flag data race

- **问题现象**：TSAN 报告 `g_shutdown_flag` 在 `handle_quit()`（写）和 `master_sync_server()`（读）之间存在 data race。
- **根因**：`g_shutdown_flag` 定义为 plain `int`，跨线程无同步读写。
- **影响范围**：shutdown 信号可能丢失或延迟。
- **修复方案**：引入 `kvs_shutdown.h` helpers，使用 `__atomic_load_n` / `__atomic_store_n`。所有调用点改为 helper。
- **验证方式**：`tests/route_c/shutdown_flag_atomic_stage12.sh` + TSAN 零报告。
- **当前状态**：✅ 已修复。

## 11. TTL scanner stop/running race

- **问题现象**：TSAN 报告 `g_ttl_scan_stop` 和 `g_ttl_scan_running` 存在 data race。
- **根因**：`src/core/kvs_ttl_background.c` 中使用 `volatile int`，不提供跨线程原子性保证。
- **影响范围**：TTL scanner 启停控制不可靠。
- **修复方案**：改为 plain `int` + `__atomic_load_n` / `__atomic_store_n`（ACQUIRE/RELEASE）。
- **验证方式**：TSAN 零报告。
- **当前状态**：✅ 已修复。

## 12. distributed master_listen_fd race

- **问题现象**：TSAN 报告 `global_dist_config.master_listen_fd` 在 master_sync_server 线程和 main 线程之间存在 data race。
- **根因**：`src/distributed/distributed.c` 中使用 plain assignment。
- **影响范围**：分布式配置不一致。
- **修复方案**：改用 `__atomic_load_n` / `__atomic_store_n`。
- **验证方式**：TSAN 零报告。
- **当前状态**：✅ 已修复。

## 13. sync_backend g_tcp_running race

- **问题现象**：TSAN 报告 `g_tcp_running` 在 worker 线程和 main 线程之间存在 data race。
- **根因**：`src/distributed/sync_backend_tcp.c` 中使用 plain `int`。
- **影响范围**：TCP 同步状态不一致。
- **修复方案**：改用 `__atomic_load_n` / `__atomic_store_n`。
- **验证方式**：TSAN 零报告。
- **当前状态**：✅ 已修复。

## 14. AOF stop race

- **问题现象**：TSAN 报告 `aof_state.stop` 在 AOF 后台 fsync 线程和 main 线程之间存在 data race。
- **根因**：`src/persistence/kvs_aof.c` + `include/kvs_aof_internal.h` + `include/kvstore.h` 中使用 `volatile int`。
- **影响范围**：AOF 停止信号不可靠。
- **修复方案**：改为 `int` + `__atomic_load_n` / `__atomic_store_n`。
- **验证方式**：TSAN 零报告。
- **当前状态**：✅ 已修复。

## 15. NtyCo init_hook race

- **问题现象**：TSAN 报告 NtyCo 的 `init_hook` 函数中全局函数指针被 main 线程写、后台线程读，无同步。
- **根因**：`NtyCo/core/nty_socket.c` 中使用 `if (!socket_f) init_hook()` 模式，非线程安全。
- **影响范围**：函数指针可能被部分写入时读取，导致 crash。
- **修复方案**：使用 `pthread_once` 保护。所有 `if (!_f) init_hook()` 改为无条件 `init_hook()` 调用（`pthread_once` 保证单次执行）。修复已 vendored 到 `third_party/NtyCo/core/nty_socket.c`。
- **验证方式**：`tests/route_c/ntyco_third_party_vendor_stage12_infra.sh` + TSAN 零报告。
- **当前状态**：✅ 已修复。

## 16. ntyco mixed shutdown hang

- **问题现象**：ntyco mixed stress 运行无法干净退出。
- **根因**：同问题 7（legacy sync path）—— 原始阻塞 I/O 阻止 scheduler 处理 QUIT。
- **影响范围**：mixed stress gate 失败。
- **修复方案**：同问题 7。
- **验证方式**：`tests/route_c/ntyco_mixed_shutdown_stage12.sh` 通过。
- **当前状态**：✅ 已修复。

## 17. real-lock fallback flaky (shutdown-exit)

- **问题现象**：聚合 correctness suite 中 real-lock 回滚路径出现 `server did not exit`。
- **根因**：编排时序问题（多个 server 快速启动/停止），非确定性逻辑错误。
- **影响范围**：仅影响全量编排脚本，standalone 通过。
- **修复方案**：未做代码修改。判定为低频编排时序 flaky。standalone `shutdown_owner_stage10_9.sh` 复跑通过。
- **验证方式**：clean tree 复跑 correctness suite 通过。
- **当前状态**：🟡 非阻塞 watch item。

## 18. replicated_command_task 偶发 Segmentation fault (watch item)

- **问题现象**：Stage 12 早期有一次 `replicated_command_task` segfault 报告。
- **根因**：未复现。可能为早期代码状态问题。
- **影响范围**：未确认。
- **修复方案**：Stage 12 Final 验证中未复现（所有模式：real-lock、weak-lock、TSAN、default build）。
- **验证方式**：`tests/route_c/replicated_command_task_stage10_3.sh` 在全部模式下通过。
- **当前状态**：🟡 历史 watch item，未复现。

## 19. weak-lock 性能并非绝对优势

- **问题现象**：Closeout benchmark 中 GET（-15.9%）、VSET（-9.9%）、VSEARCH（-15.3%）在 weak-lock 默认模式下较 real-lock 退化。
- **根因**：weak-lock 是架构简化手段（no-op 替代真实 pthread_rwlock），不是性能优化手段。GET/VSET/VSEARCH 路径的锁开销并非主要瓶颈，no-op 不能显著提升性能；而真实锁的缓存行效应在某些 workload 下反而对延迟有利。
- **影响范围**：Route C 定位应从"性能提升"调整为"所有权模型收口"。
- **修复方案**：不在此路线处理。后续独立性能优化路线可分析。
- **当前状态**：🟡 已知结论，非 bug。

## 汇总

| # | 问题 | 严重性 | 状态 |
|---|------|--------|------|
| 1 | master full sync live traversal | High | ✅ Fixed |
| 2 | slave apply 绕 executor | High | ✅ Fixed |
| 3 | RDB save live traversal | High | ✅ Fixed |
| 4 | AOF rewrite live traversal + COW failure | High | ✅ Fixed |
| 5 | TTL scanner owner | High | ✅ Fixed |
| 6 | lazy expire owner | High | ✅ Fixed |
| 7 | ntyco legacy sync path | High | ✅ Fixed |
| 8 | eBPF replication fallback | Medium | ✅ Fixed |
| 9 | shutdown owner boundary | High | ✅ Fixed |
| 10 | g_shutdown_flag data race | High | ✅ Fixed |
| 11 | TTL scanner stop/running race | High | ✅ Fixed |
| 12 | master_listen_fd race | Medium | ✅ Fixed |
| 13 | g_tcp_running race | Medium | ✅ Fixed |
| 14 | AOF stop race | Medium | ✅ Fixed |
| 15 | NtyCo init_hook race | High | ✅ Fixed |
| 16 | ntyco mixed shutdown hang | High | ✅ Fixed |
| 17 | real-lock fallback flaky | Low | 🟡 Watch |
| 18 | replicated_command_task segfault | Low | 🟡 Watch (未复现) |
| 19 | weak-lock 性能非绝对优势 | Info | 🟡 Known |
