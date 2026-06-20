# Stage 10.10 TTL Scanner Runtime Owner Convergence

## 目标

Stage 10.10 将 TTL scanner 从“后台线程持读锁扫描 live store/TTL metadata”收敛为“后台线程只定时触发，扫描和删除由 single command executor 执行”。

本阶段不删除、不弱化 `g_kv_data_rwlock`，不引入多 executor，不改变 EXPIRE/PEXPIRE/TTL/PTTL/PERSIST/GET lazy expire、RDB/AOF/replication 语义。

## Stage 10.10 前调用链

1. `main()` 启动 `kvs_command_executor_start()`。
2. `main()` 启动 `kvs_ttl_background_start()`。
3. `ttl_scan_thread_main()` 每秒唤醒。
4. master 节点调用 `ttl_scan_once()`。
5. `ttl_scan_once()` 持 `g_kv_data_rwlock` 读锁。
6. scanner 直接遍历 `kvs_get_global_array/hash/rbtree/skiptable()`。
7. scanner 复制 expired key 和 observed expire time。
8. scanner 释放读锁。
9. scanner 调用 `kvs_command_executor_submit_ttl_expire()`。
10. executor 消费 `kvs_ttl_expire_task_t`，持写锁调用 `ttl_expire_observed_key()` 二次校验并删除。

问题：scanner 运行期仍直接读取 live store/TTL metadata，并依赖 `g_kv_data_rwlock` 读锁。

## Stage 10.10 后调用链

1. `main()` 启动 `kvs_command_executor_start()`。
2. `main()` 启动 `kvs_ttl_background_start()`。
3. `ttl_scan_thread_main()` 每秒唤醒。
4. master 节点调用 `kvs_command_executor_submit_ttl_scan(32)`。
5. executor 主循环弹出 `kvs_ttl_scan_task_t`。
6. executor 持 `g_kv_data_rwlock` 写锁执行 `kvs_command_executor_run_ttl_scan()`。
7. executor 在同一线程内扫描 array/hash/rbtree/skiptable 的 TTL metadata。
8. executor 复制本批 expired candidate key 和 observed expire time。
9. executor 在仍持写锁的情况下调用 `ttl_expire_observed_key()` re-check + delete。
10. executor 更新 TTL scan 和 expire candidate 统计，释放 scan task。

## Owner 边界

`src/core/kvs_ttl_background.c` 现在只负责：

- 线程启动/停止。
- 周期性 `nanosleep()`。
- slave 跳过 active expire trigger。
- 向 executor 提交 `ttl_scan_task`。

它不再直接访问：

- `g_kv_data_rwlock`
- `kvs_get_global_array/hash/rbtree/skiptable`
- `kv_is_expired`
- `ttl_expire_observed_key`
- live store node 的 `expire_at_ms`

`src/core/kvs_command_executor.c` 现在拥有 TTL active scan/read/check/delete 的运行期逻辑。

## ABA 防护

Stage 10.1 的 observed expire timestamp 防 ABA 逻辑保留。

虽然 Stage 10.10 的扫描和删除都在 executor 线程内完成，但扫描阶段仍复制 `observed_expire_at_ms`，flush 阶段仍调用 `ttl_expire_observed_key(key, key_len, observed_expire_at_ms)`。这样即使后续再次拆分 scan/expire candidate，也不会丢失当前保护语义。

## Shutdown / Stop / Destroy 边界

安全顺序保持：

1. shutdown 开始后先 `kvs_ttl_background_stop()`，停止继续 submit scan trigger。
2. final AOF/RDB save 仍在 executor 存活期间完成。
3. `kvs_command_executor_stop()` 设置 `stopping=1`，join executor thread。
4. executor stopped/stopping 后 `kvs_command_executor_submit_ttl_scan()` 返回失败并释放 task。
5. stop 清理残留 `ttl_scan_head` 队列，递减 `ttl_scan_queue_depth`，记录 `ttl_scan_task_dropped`。
6. store/vector/destroy 在 executor stop 之后执行，避免 scan task 在 destroy 后访问 live store。

当前未发现 task 泄漏、queue 泄漏、UAF、double-free、shutdown hang 的已知路径。

## 统计

保留 Stage 10.1 的 TTL expire candidate 统计：

- `ttl_expire_task_submitted`
- `ttl_expire_task_executed`
- `ttl_expire_task_failed`
- `ttl_expire_task_dropped`
- `ttl_expire_queue_depth`
- `ttl_expire_deleted_keys`
- `ttl_expire_skipped_keys`

新增 TTL scanner executor 统计：

- `ttl_scan_task_submitted`
- `ttl_scan_task_executed`
- `ttl_scan_task_failed`
- `ttl_scan_task_dropped`
- `ttl_scan_submit_failed`
- `ttl_scan_queue_depth`
- `ttl_scan_queue_max_depth`
- `ttl_scan_wait_count`
- `ttl_scan_wait_total_us`
- `ttl_scan_wait_max_us`

## 测试

新增：

- `tests/route_c/ttl_scanner_owner_stage10_10.sh`

覆盖：

- static owner audit
- active expire
- TTL overwrite ABA
- PERSIST race
- EXPIRE update race
- reactor/proactor shutdown during TTL scan
- TTL scan queue depth drain

建议验证：

```bash
git diff --check
make clean
make embedding ENABLE_EBPF=0
tests/route_c/ttl_scanner_owner_stage10_10.sh
tests/route_c/shutdown_owner_stage10_9.sh
tests/route_c/slave_connect_owner_stage10_9a.sh
tests/route_c/replication_fallback_stage10_8.sh
bash tests/route_c/replicated_command_task_stage10_3.sh
tests/route_c/ttl_executor_stage10_1.sh
bash tests/route_c/lazy_expire_stage10_2.sh
bash tests/route_c/master_full_sync_stage10_5.sh
bash tests/route_c/rdb_snapshot_stage10_6.sh
bash tests/route_c/aof_rewrite_snapshot_stage10_7.sh
```

## 剩余 Blocker

- `g_kv_data_rwlock` 仍保留，executor TTL scan 当前仍持写锁扫描 live store。
- RDB/AOF/full sync snapshot 仍通过 executor 读取 live store，这属于已有 Stage 10.5-10.7 边界，不在本阶段扩大处理。
- ntyco/eBPF replication 相关同步路径未在本阶段改造。
