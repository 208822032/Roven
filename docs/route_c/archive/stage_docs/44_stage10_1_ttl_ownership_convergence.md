# Stage 10.1: TTL Ownership Convergence

## 背景

Stage 10 `43_stage10_rwlock_removal_readiness_audit.md` 的结论是 `NOT_READY`。
其中一个直接 blocker 是 TTL active expire 后台线程仍在 `src/core/kvs_ttl_background.c`
中持 `g_kv_data_rwlock` 写锁扫描并删除普通 KV。

Stage 10.1 只处理 TTL ownership 收敛，不删除或弱化 `g_kv_data_rwlock`，不处理
AOF/RDB/replication/full sync 的 snapshot/task 化。

## 目标

本阶段把 TTL active expire 从“后台线程直接删除 store”改为：

```text
TTL scanner thread
  -> 持读锁发现 expired key + observed expire_at
  -> 释放读锁
  -> submit ttl_expire_task 到 single command executor
  -> executor 持写锁 re-check
  -> executor 串行删除
```

核心原则：

- scanner 可以发现过期 key，但不能删除或修改 live store。
- executor 才能做删除/修改。
- TTL 删除和普通 SET/DEL/VSET/VDEL/VSEARCH commit 由同一个 executor 线程串行执行。
- 继续保留 `g_kv_data_rwlock`。

## 不做事项

- 不删除或弱化 `g_kv_data_rwlock`。
- 不做 rwlock removal。
- 不做 AOF/RDB/replication/full sync 重构。
- 不做多 command executor、commit ordering、response reorder。
- 不做 batch embedding、per-thread OrtSession、HNSW/index 优化。
- 不改变 RESP 协议或用户可见 TTL/KV/vector 语义。
- 不修改 lazy expire 主逻辑；只做审计并保留到 Stage 10.2。

## 当前 TTL Active Expire 路径

改造前：

```text
ttl_scan_thread_main
  -> ttl_scan_once
  -> pthread_rwlock_wrlock(g_kv_data_rwlock)
  -> scan array/hash/rbtree/skiptable
  -> directly delete expired key
  -> optionally master_sync_command("DEL")
```

问题：

- 后台线程是 live store 写 owner。
- 删除动作绕过 single command executor。
- 去锁前无法证明 store/TTL metadata 只有一个状态 owner。

## 新 TTL Executor Task 路径

新增内部 task：

```c
typedef struct kvs_ttl_expire_task_s {
    struct kvs_ttl_expire_task_s *queue_next;
    char *key;
    size_t key_len;
    int64_t observed_expire_at_ms;
    uint64_t enqueue_us;
} kvs_ttl_expire_task_t;
```

新增 API：

```c
int kvs_command_executor_submit_ttl_expire(
    const char *key,
    size_t key_len,
    int64_t observed_expire_at_ms
);
```

executor 新增 TTL queue，并在主循环中按以下顺序消费：

```text
prepared queue
ttl expire queue
normal command queue
```

这个顺序避免 Stage 9 prepared task 堆积，同时让 TTL 删除不绕过 single executor。

## TTL Task Ownership

- scanner 在读锁内只复制 key 和 observed expire_at。
- submit API 在 heap 上创建 `kvs_ttl_expire_task_t`，并复制 key。
- submit 成功后 executor 拥有 task/key。
- submit 失败时 submit API 释放 task/key，并增加 `ttl_expire_submit_failed`。
- executor 执行或 drain 时负责释放 task/key。
- TTL task 没有 `response_task`，不持 session ref，不进入 owner poller response 路径。

## 如何避免删除新值

TTL task 不会只带 key 直接删除。executor 执行时调用：

```c
ttl_expire_observed_key(key, key_len, observed_expire_at_ms)
```

该函数在 executor 持 `g_kv_data_rwlock` 写锁期间重查：

1. key 仍存在。
2. 当前 `expire_at_ms` 仍等于 scanner 观察到的 `observed_expire_at_ms`。
3. 当前时间仍大于等于该 expire_at。

如果 key 被客户端 `DEL`、`SET`、`EXPIRE`、`PERSIST` 更新，task 会跳过，不删除当前值。

## Lazy Expire 审计结果

本阶段未修改 lazy expire。

当前 lazy expire 路径：

- `GET` 和 `EXIST` 通过 `ttl_expire_if_needed_by_structure()` 检查过期。
- 读锁下发现过期时返回 `KVS_CMD_RETRY_WITH_WRITE_LOCK`。
- `kvs_command_task_execute_sync()` 释放读锁并重新获取写锁，再执行删除。
- `TTL`/`EXPIRE`/`PEXPIREAT`/`PERSIST` 本身是写锁命令。

保留原因：

- lazy expire 已经在普通 reactor/proactor async path 的 executor 内执行。
- 但 ntyco 和 replication/fallback 同步 handler 仍存在，不能声明 lazy expire 完全 executor-owned。
- 将 lazy expire 从读路径拆成独立 task 会触碰用户命令返回时机和 handler 深度耦合，留到 Stage 10.2 单独处理。

## Shutdown 顺序

本阶段为了让 ntyco/offline TTL task 也有 executor owner，把 command executor 生命周期收敛到 core main：

```text
init store
load RDB/AOF
rebuild vector
init distributed
start command executor
start TTL scanner
start network mode
...
network exits
RDB save (existing behavior)
stop TTL scanner
stop command executor
destroy store
```

reactor/proactor 继续注册 executor response eventfd，但不再拥有 executor start/stop。
注意：RDB save、AOF/RDB/replication/full sync shutdown 边界没有在本阶段重构，仍是剩余 blocker。
TTL scanner 在 executor stopping/running 不可用时 submit 会失败并安全释放 task/key。

## 与 Stage 9 Worker Pool 的关系

- embedding prepare worker pool 仍只处理 VSET/VSEARCH prepare。
- TTL task 不进入 prepare queue，不持 response/session。
- executor 先消费 prepared queue，再消费 TTL queue，再消费 normal command queue。
- Stage 9 prepared queue 和 TTL queue 都由 `cmd_mu/cmd_cv` 唤醒，互不共享 task 类型。
- 测试覆盖 worker pool enabled threads=2 时 VSET/VSEARCH 与 TTL active expire 并发。

## Stats

新增 executor stats：

- `ttl_expire_task_submitted`
- `ttl_expire_task_executed`
- `ttl_expire_task_failed`
- `ttl_expire_task_dropped`
- `ttl_expire_queue_depth`
- `ttl_expire_queue_max_depth`
- `ttl_expire_deleted_keys`
- `ttl_expire_skipped_keys`
- `ttl_expire_submit_failed`
- `ttl_expire_wait_count`
- `ttl_expire_wait_total_us`
- `ttl_expire_wait_max_us`

executor stop 时打印：

```text
ttl executor stats: ttl_expire_task_submitted=... ttl_expire_queue_depth=...
```

## Tests

新增：

```bash
tests/route_c/ttl_executor_stage10_1.sh
```

覆盖：

- reactor/proactor。
- 基础 SET/EXPIRE/GET/TTL/PERSIST/DEL。
- 批量 active expire，验证 TTL task executed、deleted、queue depth drained。
- 旧 TTL task 不删除重新 SET/EXPIRE 的新值。
- 多客户端 SET/EXPIRE/GET/DEL 混合。
- worker pool enabled threads=2 下 VSET/VSEARCH 与 TTL active expire 共存。
- shutdown during TTL expire。

## 验证结果

已执行：

```text
bash -n tests/route_c/ttl_executor_stage10_1.sh: passed
git diff --check: passed
make clean && make embedding ENABLE_EBPF=0: passed
tests/route_c/ttl_executor_stage10_1.sh: passed
tests/embedding_worker_pool_stage9.sh: passed
tests/embedding_worker_pool_stage9_1.sh: passed
ASAN/UBSAN build: passed
ASAN_OPTIONS=detect_leaks=0 ... tests/route_c/ttl_executor_stage10_1.sh: passed
TSAN build: passed
TSAN_OPTIONS="halt_on_error=0 exitcode=0" tests/route_c/ttl_executor_stage10_1.sh: passed
```

TSAN 日志仍包含 Stage 9.1 已知的 shutdown/background 全局状态竞态：

- `src/core/kvs_operation.c:1443 in handle_quit`
- `src/core/kvs_ttl_background.c:218 in kvs_ttl_background_stop`
- `src/distributed/distributed.c:311 in distributed_shutdown`
- `src/distributed/sync_backend_tcp.c:254 in sync_backend_shutdown`

未发现 TTL task queue、TTL task ownership、executor TTL stats 或 Stage 9 worker pool 新竞态。

## Sanitizer 预期分类

必须修复：

- TTL task double free。
- key string leak。
- task use-after-free。
- expired task 删除新值。
- executor shutdown deadlock。
- TTL queue race。
- TTL task queue depth 无法归零。
- Stage 9 worker pool 回归失败。

可归类为既有问题：

- Stage 9.1 已记录的 `g_shutdown_flag`、TTL stop、distributed shutdown、sync backend shutdown 背景竞态。

## 剩余 Blocker

Stage 10.1 只收敛 TTL active expire 删除 owner。剩余去锁 blocker：

- lazy expire 仍有读锁升级写锁模式，且同步 fallback/ntyco 未完全收敛。
- AOF rewrite 仍直接遍历 live KV/vector。
- RDB save 仍直接遍历 live KV/vector。
- master full sync 仍直接遍历 live store。
- slave apply 仍直接修改 KV/TTL。
- ntyco/fallback 同步 handler 仍可绕过 async command task。
- shutdown save/stop/destroy 未统一 executor snapshot/drain 边界。

## 下一阶段建议

建议进入 Stage 10.2：lazy expire ownership convergence / synchronous fallback audit。

Stage 10.2 应重点处理：

- `GET/EXIST` lazy expire 与读锁升级写锁的 owner 边界。
- ntyco 与 reactor/proactor fallback 同步 handler 是否必须进入 executor。
- lazy expire 删除是否需要独立 internal task 或直接在 executor-only handler 内保留同步删除。
