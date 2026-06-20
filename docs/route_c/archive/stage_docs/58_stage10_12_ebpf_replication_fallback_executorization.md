# Stage 10.12 eBPF Replication Fallback Executorization

## 1. Route C Full Executor Coverage

Route C 的最终目标是 Full Executor Coverage：所有 shared state read/write/snapshot/mutation 路径都要归属到 single command executor。`g_kv_data_rwlock` 本阶段不删除、不弱化；它继续作为保守保护边界，但网络线程不能再通过 eBPF replication fallback 同步进入 store/TTL/vector/HNSW/AOF/RDB/replication shared state。

本阶段不拆分 10.12A/10.12B/10.12C，一次性收敛 reactor 和 proactor 的 eBPF REPLSYNC/is_replication fallback。ntyco sync mode 仍是下一阶段之外的 legacy sync mode，本阶段未修改。

## 2. Stage 10.12 前调用链

reactor eBPF fallback 旧链路：

```text
reactor_submit_async_command
  -> ext->is_replication || reactor_command_is_replsync(session)
  -> net_session_process_resp(fd, session, reactor_session_handler, ext)
  -> reactor_session_handler
  -> kvs_handler
  -> kvs_filter_protocol
  -> kvs_command_task_execute_sync
```

proactor eBPF fallback 旧链路：

```text
proactor_submit_async_command
  -> ext->is_replication || proactor_command_is_replsync(session)
  -> p_process_received_data(ext)
  -> net_session_process_resp(ext->fd, &ext->session, proactor_session_handler, ext)
  -> proactor_session_handler
  -> kvs_handler
  -> kvs_filter_protocol
  -> kvs_command_task_execute_sync
```

这两条链路的问题是网络线程可以在 eBPF replication fallback 下同步进入 handler，并通过 `kvs_filter_protocol()` 访问 shared state。

## 3. Stage 10.12 后调用链

reactor 目标链路：

```text
reactor_submit_async_command
  -> net_session_parse_one_resp
  -> reactor_handle_replication_fallback
       -> kvs_replication_fallback_classify
       -> REPLSYNC: inline small protocol response, mark ext->is_replication
       -> replicated apply: kvs_command_executor_submit_replicated
       -> normal command: reactor_submit_command_task -> kvs_command_executor_submit
```

proactor 目标链路：

```text
proactor_submit_async_command
  -> net_session_parse_one_resp
  -> proactor_handle_replication_fallback
       -> kvs_replication_fallback_classify
       -> REPLSYNC: inline small protocol response, mark ext->is_replication
       -> replicated apply: kvs_command_executor_submit_replicated
       -> normal command: proactor_submit_command_task -> kvs_command_executor_submit
```

`reactor_submit_async_command()` 不再调用 `net_session_process_resp()`；`proactor_submit_async_command()` 不再调用 `p_process_received_data()`。`p_process_received_data()` 和旧 synchronous session handler 仍存在给非本阶段代码边界使用，但不再是 eBPF fallback 的执行路径。

## 4. REPLSYNC 策略

当前 eBPF `REPLSYNC` 在网络层只做协议控制：

- 校验 `g_cfg.sync_mode == KVS_SYNC_EBPF`。
- 校验当前节点 role 是 `ROLE_SLAVE`。
- 将连接标记为 `ext->is_replication = 1`。
- 写入很小的 inline RESP：`+OK\r\n` 或 `-ERR REPLSYNC not allowed in current role\r\n`。

该路径不遍历 live store，不读写 TTL/vector/HNSW/AOF/RDB，也不触发 full sync snapshot。因此当前 REPLSYNC 不进入 executor。若后续 REPLSYNC 语义扩展为触发 full sync 或读取 shared state，必须新增最小 control task 或复用已有 full sync snapshot task。

## 5. is_replication Apply 策略

新增 `kvs_replication_fallback_classify()` 对 eBPF fallback 中已解析的 RESP 命令显式分类：

- `SET/RSET/HSET/SSET/PAPERSET`
- `DEL/RDEL/HDEL/SDEL/PAPERDEL`
- `MOD/RMOD/HMOD/SMOD/PAPERMOD`
- `PEXPIREAT`
- `PERSIST`

这些复制 apply 命令转换为 `ms_op_t` 和 key/value/expire metadata 后，调用 Stage 10.3 已有的 `kvs_command_executor_submit_replicated()`。网络线程只复制/投递任务和排队小 `+OK` 响应，不直接调用底层 set/del/mod/ttl apply。

误入 eBPF fallback 的普通命令走 normal `command_task`：

```text
reactor_submit_command_task / proactor_submit_command_task
  -> kvs_command_task_build
  -> kvs_command_task_copy_argv
  -> kvs_command_executor_submit
  -> response_task 回投原 owner
```

submit/build/copy 失败均 fail-fast，关闭连接或让调用方关闭连接，不回退同步 handler。

## 6. Full Sync Snapshot 复用

本阶段不改 full sync bytes sender。现有 full sync 已经走：

```text
slave_sync_thread
  -> kvs_command_executor_build_full_sync_snapshot(&snapshot)
  -> master_send_full_sync_snapshot(snapshot)
  -> kvs_full_sync_snapshot_destroy(snapshot)
```

executor 只构造 snapshot；snapshot bytes 的 socket send 在 sync sender 线程执行，不访问 live store。RDB save 和 AOF rewrite 仍分别通过 `kvs_command_executor_build_rdb_snapshot()` 与 `kvs_command_executor_build_aof_rewrite_snapshot()` 构造 snapshot。

## 7. Response / Socket Send Ownership

- normal command response：沿用 `kvs_response_task_t` 和 `kvs_async_net_adapter`，回投 original reactor/proactor owner，并通过 session id/generation 校验 fd reuse。
- REPLSYNC 和 replicated apply 的 `+OK`：网络层只 append 小响应到 session send buffer，后续由 reactor/proactor socket send 机制发送。
- executor 不直接执行 socket send，也不持 socket 做阻塞 IO。
- full sync 大量 snapshot bytes：由已有 full sync sender 发送 snapshot 对象，不访问 live store。

## 8. 生命周期与失败路径

- disconnect during pending eBPF normal task：normal task 持有 session ref；response 回投时通过 owner/session/generation 校验，失配则 drop response 并 release ref。
- disconnect during pending replicated apply：`kvs_command_executor_submit_replicated()` 内部复制 key/value，不持有 session/fd；连接断开只影响 inline response，不影响 apply task 生命周期。
- fd reuse：normal response target 携带 fd/session_id/generation；reactor/proactor validate_owner 防止旧 fd response 写入新连接。
- submit failure：replicated submit 失败时 reset parser 并返回 -1；normal command submit 失败时释放 task/response/session ref 并 fail-fast，不同步执行 handler。
- executor stopping/stopped：`kvs_command_executor_submit()` 和 `kvs_command_executor_submit_replicated()` 在 stopped/stopping 时返回失败，调用方关闭连接或 fail-fast。
- shutdown drain/drop：normal command task 由已有 executor stop drain/drop 逻辑处理；replicated task 由 `repl_head/repl_tail` 队列 drain/drop 逻辑处理。
- response target release：normal response 仍由 async adapter `release_session` 释放；inline response 没有 executor-owned response target。
- task ownership：normal task/response 由网络线程分配、提交后交给 executor；replicated task 由 submit API 内部分配并由 executor 销毁。
- UAF/double-free/task leak/queue leak/shutdown hang：normal task 使用已有 refcount/generation 边界；replicated task 不持 session；submit failure 清理本地对象；executor stop 负责队列清理。

## 9. 实际完成范围

已完成：

- reactor eBPF fallback 不再同步调用 `net_session_process_resp -> reactor_session_handler -> kvs_handler`。
- proactor eBPF fallback 不再同步调用 `p_process_received_data -> net_session_process_resp -> proactor_session_handler -> kvs_handler`。
- 复制 apply 复用 `kvs_command_executor_submit_replicated()`。
- 普通命令误入 fallback 转为 normal `command_task`。
- `kvs_command_task` 捕获并执行 task-level `sync_context`，确保复制上下文不会触发二次 replication。
- 新增 Stage 10.12 静态审计测试。

未完成：

- ntyco sync mode executor 化。
- eBPF 动态内核路径 smoke；当前环境默认 `ENABLE_EBPF=0` 构建，无法验证内核 eBPF 程序和 sockmap 实际流量。

## 10. 剩余风险

- eBPF 真实部署下 replicated apply 的 ACK 时机仍是 submit 成功后立即 `+OK`，不是 apply 完成后 ACK；这保持最小改造，但不是强确认语义。
- REPLSYNC 未来如果变成 full sync trigger，必须新增 executor control task。
- classifier 覆盖当前普通 KV/TTL/PAPER apply；vector replication 若后续加入，需要扩展 `ms_op_t` 和 replicated task。
- ntyco 仍同步绕过 executor，Route C Full Executor Coverage 尚未完成。

## 11. 下一阶段建议

1. 单独收敛 ntyco sync mode 到 executor，或明确移除/禁用 ntyco 于 Route C。
2. 在支持 eBPF 的环境增加动态 REPLSYNC + replicated apply smoke，覆盖 reactor/proactor。
3. 评估 replicated apply ACK 是否需要从 submit ACK 升级为 executor completion ACK；若需要，新增轻量 response target，而不是让 executor send socket。
4. 若 REPLSYNC 扩展为 full sync/control 操作，复用 full sync snapshot task 或新增 replsync_control_task。
