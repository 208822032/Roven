# Unified Configuration Stage 4 实现报告

## 1. 范围与结论

本阶段只迁移现有 TCP replication 和 eBPF daemon 的运行配置。生产代码、配置基础设施、相关测试和文档有修改；普通 KV replication wire/FULLDUMP/apply、公开 RESP、RDB/AOF、向量命令和 HNSW 算法均未修改，VSET/VDEL 仍未进入复制链路。

最终入口：

```text
defaults -> one main config -> CLI overrides -> derive role-specific ports/paths/targets
-> validate capability/policy/exclusion -> effective g_cfg
-> kvstore TCP runtime and ebpf_syncd consume the same object
```

## 2. TCP replication

- `replication.transport` 只接受 `disabled/tcp`，默认 disabled。
- master 消费 `sync_listen_port`；slave 消费 `master_host/master_sync_port`。`master_service_port` 仅保留服务端点和 legacy 派生信息。
- legacy `master_ip/master_port/sync_port/sync_mode` 仍由 descriptor alias 进入同一字段；`sync_port` 按最终 role 转换，和 canonical 端口重复时失败。
- master sync listener 与 service port 冲突时启动失败；TCP slave 缺少 IPv4 master host 或 sync port 时失败。
- `full_sync_policy` 当前只允许 required，`not_ready_query_policy` 当前只允许 reject。现实现尚无 replication READY 状态机，因此这里只冻结并验证策略，不伪装已实现在线 readiness gate。

## 3. eBPF daemon 与边界

`ebpf_syncd` 删除 `load_daemon_conf()` 和位置参数配置入口，直接调用 `kvs_config_load()`；object/source/targets/ring/queue/event/flush 全部来自 effective config。object path 相对 config root，启动前校验编译 capability、文件可读性、IPv4 source、ring power-of-two 和 targets。

模式语义：

```text
disabled             daemon clean exit
observe              attach/observe only; no sender, FULLDUMP or write forwarding
experimental_mirror  current experimental FULLDUMP + command mirror
```

TCP 可与 observe 共存；TCP 与 experimental mirror 保守互斥，避免当前没有静态 TCP slave target 集合时发生 double apply。mirror 不声明 offset、ACK、backlog、READY 或可靠复制。legacy `sync_mode=ebpf` 转为 replication disabled + experimental mirror，并输出 HIGH warning；历史 `ebpf_conf` 仅保留 inactive 提示，不再导入第二份配置。

启用多 mirror target 后复核到旧共享消息 `put` 的引用计数判断有多余分号，会在首个 target 消费时提前释放；本阶段做了单行修复，仅恢复原有 refcount 语义，不改变队列、命令或 wire format。

## 4. Future 字段和兼容边界

`backlog_bytes/ack_interval_ms/reconnect_backoff_ms/offset/partial_resync/promotion_policy` 保持 future，显式配置立即失败。未新增 backlog、重连、partial resync、晋升、向量复制或复制可观测协议字段。

普通非复制测试中过去用 `sync_mode=ebpf` 仅关闭 TCP 的配置改为明确 disabled；真实 eBPF TTL 脚本改用统一主配置启动 daemon，不再生成独立 `ebpf_sync.conf`。

## 5. 测试结果

已执行并通过：

```text
tests/unified_config_stage1_test.sh
tests/unified_config_stage2_test.sh                 # config + 三网络集成
tests/unified_config_stage3_test.sh                 # ASan/UBSan
tests/unified_config_stage3_runtime_test.sh
tests/unified_config_stage3_vector_disabled_test.sh
tests/unified_config_stage3_onnx_test.sh
tests/unified_config_stage4_test.sh                 # capability 0/1 + ASan/UBSan
tests/ttl/test_ttl_tcp_replication.sh               # array/rbtree/hash/skiptable
src/eBPF/ebpf_syncd --no-config                     # disabled clean exit
make clean && make ENABLE_EBPF=1 ENABLE_HNSWLIB=1 ENABLE_FAISS=1 -j$(nproc)
```

TCP 回归覆盖增量 set/delete/overwrite、TTL/PEXPIREAT/PERSIST 和新 slave FULLDUMP，四 backend 全部通过。eBPF daemon 和 kernel object 均成功构建。当前进程没有 `CAP_BPF/CAP_SYS_ADMIN`，且 `kernel.unprivileged_bpf_disabled=2`，因此真实 kprobe/eBPF replication 测试按门禁跳过；未将编译成功伪报为内核运行通过。

静态检查确认 daemon 不再包含独立业务配置 parser 或 `ebpf_sync.conf` 读取；`git diff --check` 通过。完整累计工作树状态见最终汇报。

## 6. 文件和下一阶段

Stage 4 生产文件：`include/kvs_config.h`、`src/core/kvs_config.c`、`src/core/kvstore.c`、command propagation guards、三网络 REPLSYNC gate、`src/eBPF/ebpf_syncd.c` 和 `Makefile` capability 标记。新增 `tests/unified_config_stage4_test.{c,sh}`，并更新 TCP/eBPF 及受影响的配置回归脚本。

统一配置主线在 Stage 4 后暂停；Configuration Stage 5/6 等主从同步完成后再执行。下一阶段：

```text
Replication Stage 1A — HNSW 写入与容量安全前置门禁
```

Stage 1A 只处理向量复制所需的失败原子性和 destructive reserve 风险，完成后立即进入向量增量复制，不新建独立大型 capacity 设计阶段。

```text
replication_config_migrated=yes
ebpf_config_migrated=yes
ebpf_independent_parser_removed=yes
tcp_ebpf_exclusion_enforced=yes
future_replication_fields_rejected=yes
existing_tcp_replication_preserved=yes
development_checklist_updated=yes
configuration_work_paused=yes
ready_for_replication_stage1a=yes
```
