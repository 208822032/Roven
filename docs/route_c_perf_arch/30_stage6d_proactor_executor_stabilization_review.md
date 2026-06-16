# 阶段 6D：proactor executor 稳定化、观测与性能对比审查

## 1. 背景

阶段 6C 已经完成 proactor 普通连接接入 single command executor 的最小异步闭环：

```text
proactor read CQE
  -> parse one RESP command
  -> submit command_task to single executor
  -> executor pushes response_task
  -> eventfd wakes proactor owner loop
  -> owner-aware poll PROACTOR/0 response
  -> append send_buf
  -> post write SQE
```

阶段 6D 的目标是压稳 6C 闭环、补齐异常路径观测，并对 reactor executor 与 proactor executor 做同条件性能对比。本阶段没有扩大并发模型：不做多 executor、不做 embedding worker pool、不做 pipeline 并行、不做 response reorder。

## 2. 6C 遗留的非阻塞问题

6C 审查遗留两类非阻塞问题：

- proactor eventfd CQE 后重新 arm executor eventfd poll SQE 时，`proactor_arm_executor_eventfd()` 返回失败缺少日志和统计。
- `kvs_async_net_dispatch_response()` 调用 `ops->enable_write(target)` 后不检查返回值；proactor 的 `enable_write` 失败缺少本地观测。

这两处都属于异常观测不足，而不是已知主路径错误。

## 3. 本阶段补强内容

代码变更：

- `src/network/proactor.c`
  - 增加 proactor executor 本地 stats。
  - eventfd drain / CQE / rearm failure 增加统计。
  - rearm failure 增加 `KVS_LOG_ERROR`。
  - `proactor_async_enable_write()` 内部检查 `set_event_send()` 返回值，失败时记录 stats 和详细错误日志。
  - response dispatch append-failed、async submit failed、fallback、pipeline restore submit 增加统计。
  - proactor shutdown 前 dump executor 通用 stats 和 proactor 本地 stats。

测试/压测脚本：

- `tests/proactor_executor_stability_6d.sh`
  - proactor high accept 500。
  - fd reuse 300。
  - KV pipeline 10/50 组 SET/GET。
  - vector pipeline 10/50 组 VSET/VSEARCH。
- `tests/proactor_executor_perf_compare_6d.sh`
  - reactor/proactor 同配置启动。
  - VSET latency、VSEARCH latency。
  - VSET throughput c=4/8/16。
  - VSEARCH throughput c=4/8/16。
  - KV SET/GET throughput smoke c=4。

## 4. eventfd rearm 失败处理

处理策略：

- `EVENT_EXECUTOR_EVENTFD` CQE 到达后，清 pending marker。
- 正常 CQE drain eventfd 并处理 owner response。
- 非 shutdown 下重新 arm eventfd poll。
- 如果 `proactor_arm_executor_eventfd()` 返回 `<0`：
  - `executor_eventfd_rearm_failed++`
  - 记录 `KVS_LOG_ERROR("failed to rearm executor eventfd poll in proactor; executor responses may stop waking owner loop")`

资源行为：

- `proactor_arm_executor_eventfd()` 失败路径已经释放本地 `io_uring_op_t`，不会泄漏 eventfd op 元数据。
- rearm 失败不关闭连接、不销毁 response、不引入复杂恢复机制。
- 失败后风险是后续 executor response 可能无法继续通过 eventfd 唤醒 proactor owner loop；因此记录为严重错误。owner loop 的后续周期性扫描和其他 CQE 可能提供有限兜底，但不能作为可靠恢复机制。

## 5. enable_write 失败处理

采用低风险方案 B：

- 不修改 `kvs_async_net_dispatch_response()` 的通用行为。
- 不影响 reactor async_net_ops。
- 不改变 response_task ownership。
- 不把已经 append 到 `send_buf` 的 response 误判为 drop。

具体处理：

- `proactor_async_enable_write()` 调用 `set_event_send(ext->ring, ext)`。
- 返回 `<0` 时：
  - `enable_write_failed++`
  - 记录 fd、session_id、generation、request_seq、send_buf size、send_offset、write_in_flight。
- response 已经 append 到 `send_buf`，dispatch 后继续 `restore_read/release_session`。
- `proactor_process_executor_responses()` 末尾已有 `post_send_if_needed()` 扫描 current connections，会在条件满足时兜底重试提交 write SQE。

## 6. stats 观测项

executor 通用 stats 已确认存在：

- `submitted_tasks`
- `completed_tasks`
- `submit_failed`
- `command_queue_depth`
- `command_queue_max_depth`
- `response_queue_depth`
- `response_queue_max_depth`
- `pushed_responses`
- `polled_responses`
- `eventfd_wakeup_count`
- `eventfd_write_fail_count`
- `dropped_responses`
- `session_mismatch_drop`
- `session_closing_drop`
- `response_append_failed`

proactor 本地新增 stats：

- `executor_eventfd_rearm_failed`
- `executor_eventfd_cqe_count`
- `executor_eventfd_drain_count`
- `executor_responses_polled`
- `executor_dispatch_failed`
- `enable_write_failed`
- `async_submit_failed`
- `async_fallback_count`
- `pipeline_restore_submit_count`

阶段 6D 性能脚本 proactor shutdown stats 样例：

```text
submitted=3001 completed=3001 submit_failed=0 cmd_depth=0 cmd_max=15
resp_depth=0 resp_max=10 pushed=3001 polled=3001
eventfd_wakeup=3001 eventfd_write_fail=0
dropped=0 mismatch_drop=0 closing_drop=0 append_failed=0

eventfd_cqe=2454 eventfd_drain=2492 eventfd_rearm_failed=0
responses_polled=3001 dispatch_failed=0 enable_write_failed=0
async_submit_failed=0 async_fallback=0 pipeline_restore_submit=0
```

该样例显示 owner-aware response polling 下 proactor 处理的 response 数与 executor polled 数一致，且没有 drop、dispatch failed、eventfd write failed、rearm failed 或 enable_write failed。

## 7. 稳定性测试结果

构建：

```bash
make clean && make embedding
```

结果：通过。仍有既有 warning：

```text
src/core/kvstore.c:266: warning: implicit declaration of function 'kvs_vector_rebuild_after_load'
```

proactor 稳定性：

```bash
tests/proactor_executor_stability_6d.sh
```

结果：

```text
high_accept=500: ok
fd_reuse=300: ok
pipeline_kv=20 replies: ok
pipeline_kv=100 replies: ok
pipeline_vector=20 replies: ok
pipeline_vector=100 replies: ok
stage6d proactor stability: ok
```

覆盖结论：

- high accept 500 未出现 hang / crash。
- fd reuse 300 未出现旧 response 错写新连接。
- KV pipeline 10/50 组 SET/GET 保序。
- VSET/VSEARCH pipeline 10/50 组响应类型和顺序正确。
- ASAN/UBSAN 下同一稳定性脚本通过。

reactor/proactor ONNX semantic 与 KV/TTL smoke：

```bash
bash tests/semantic_onnx_test.sh 127.0.0.1 <port>
```

结果：

- reactor：KV SET/GET、EXPIRE/TTL/PERSIST、semantic ONNX 通过。
- proactor：KV SET/GET、EXPIRE/TTL/PERSIST、semantic ONNX 通过。

## 8. reactor/proactor 性能对比

测试脚本：

```bash
RESULT_DIR=results/stage6d_rerun tests/proactor_executor_perf_compare_6d.sh
```

说明：

- 使用 hash embedding，避免 ONNX 推理波动掩盖网络/executor 对比。
- 每项 200 requests。
- VSEARCH 每个 case 使用同 scope 预填充 200 条记录。
- KV SET/GET throughput 采用 c=4 smoke；更高并发的临时 Python smoke 在两种网络下都出现对称失败，未纳入性能结论。

### VSET latency

| mode | qps | avg ms | p50 ms | p95 ms | p99 ms |
|---|---:|---:|---:|---:|---:|
| reactor | 6751.05 | 0.15 | 0.15 | 0.17 | 0.19 |
| proactor | 6535.09 | 0.15 | 0.15 | 0.18 | 0.21 |

### VSEARCH latency

| mode | qps | avg ms | p50 ms | p95 ms | p99 ms |
|---|---:|---:|---:|---:|---:|
| reactor | 1381.59 | 0.72 | 0.66 | 1.54 | 1.68 |
| proactor | 1513.60 | 0.66 | 0.66 | 0.71 | 0.91 |

### VSET throughput

| mode | clients | qps | avg ms | p50 ms | p95 ms | p99 ms |
|---|---:|---:|---:|---:|---:|---:|
| reactor | 4 | 71890.73 | 0.05 | 0.05 | 0.07 | 0.09 |
| proactor | 4 | 51988.56 | 0.07 | 0.07 | 0.10 | 0.11 |
| reactor | 8 | 81833.06 | 0.09 | 0.09 | 0.11 | 0.12 |
| proactor | 8 | 66115.70 | 0.11 | 0.12 | 0.13 | 0.15 |
| reactor | 16 | 70274.07 | 0.21 | 0.22 | 0.27 | 0.29 |
| proactor | 16 | 71098.47 | 0.21 | 0.21 | 0.24 | 0.25 |

### VSEARCH throughput

| mode | clients | qps | avg ms | p50 ms | p95 ms | p99 ms |
|---|---:|---:|---:|---:|---:|---:|
| reactor | 4 | 1660.21 | 2.39 | 2.40 | 2.47 | 2.61 |
| proactor | 4 | 1628.70 | 2.44 | 2.45 | 2.51 | 2.53 |
| reactor | 8 | 1642.95 | 4.78 | 4.86 | 4.90 | 4.93 |
| proactor | 8 | 1628.80 | 4.82 | 4.90 | 4.95 | 4.99 |
| reactor | 16 | 1637.21 | 9.40 | 9.72 | 10.14 | 10.19 |
| proactor | 16 | 1624.13 | 9.47 | 9.82 | 9.96 | 10.03 |

### KV SET/GET throughput smoke

| mode | clients | qps | avg ms | p50 ms | p95 ms | p99 ms |
|---|---:|---:|---:|---:|---:|---:|
| reactor | 4 | 18757.14 | 0.17 | 0.15 | 0.33 | 0.66 |
| proactor | 4 | 19090.47 | 0.17 | 0.15 | 0.27 | 0.37 |

性能结论：

- VSEARCH 主要受单 executor 和 HNSW/search 路径限制，reactor/proactor 吞吐基本持平。
- VSET c=4/c=8 reactor 更高，c=16 proactor 持平略高；这符合 proactor 当前 single_inflight + send_buf gating 的保守策略。
- 普通 KV c=4 smoke 未发现 proactor 明显退化。

## 9. ASAN/UBSAN/LSAN/TSAN 情况

ASAN/UBSAN 构建：

```bash
make clean && \
EXTRA_CFLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
EXTRA_LIBS="-fsanitize=address,undefined" \
make embedding ENABLE_EBPF=0
```

结果：通过。

ASAN/UBSAN proactor 稳定性：

```bash
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
tests/proactor_executor_stability_6d.sh
```

结果：通过，未发现阶段 6D 新增 UAF、double free 或 UBSAN abort。

LSAN：本次 ASAN 命令启用 `detect_leaks=1`，功能回归完成后日志出现既有 vector/HNSW 退出清理泄漏摘要：

```text
SUMMARY: AddressSanitizer: 3088 byte(s) leaked in 193 allocation(s).
```

堆栈集中在 `kvs_vector_ann_index_add()` / `kvs_vector_ann_node_init_layers()` / `kvs_vector_ann_collect_candidates_layer()` 等 HNSW/vector index allocation 路径，归类为既有 vector/HNSW 退出清理噪音，不归入阶段 6D proactor executor 新问题。

TSAN：本阶段未执行。原因是当前项目中 ONNX runtime、signal shutdown、TTL/distributed 后台线程可能产生非 6D 噪音；建议 TSAN 作为单独审查项运行，避免混入本阶段结论。

## 10. 已知既有问题归类

不归入阶段 6D 的既有问题：

- `kvs_vector_rebuild_after_load` 隐式声明 warning。
- MOD 不清 TTL 语义问题。
- LSAN 下可能出现 vector/HNSW/ONNX 退出清理噪音。
- replication / REPLSYNC 仍按阶段边界保持同步 fallback。

## 11. 是否建议进入下一阶段

建议进入下一阶段，但只建议进入“设计和小步验证”。

理由：

- proactor executor 主闭环在普通构建和 ASAN/UBSAN 下通过 high accept、fd reuse、pipeline、KV/TTL/vector/semantic 回归。
- eventfd rearm 失败、enable_write 失败、dispatch failed、drop、queue depth 已具备可观测性。
- proactor shutdown stats 显示任务 submitted/completed/polled 对齐，没有 response 互抢或 owner mismatch drop。
- reactor/proactor 性能基本符合单 executor + single_inflight 预期，没有发现 proactor 明显异常退化。

进入下一阶段前仍应保持当前边界：不要直接叠加多 executor、embedding worker pool 和 commit 串行化三类大改。

## 12. 下一阶段建议

优先级建议：

1. embedding worker pool 设计优先。
2. commit 串行化其次。
3. 多 executor 最后。

原因：

- 当前性能数据表明 VSEARCH 受单 executor / search / embedding 路径限制更明显，单纯扩大网络侧并发收益有限。
- embedding worker pool 能先把耗时 prepare 从 commit/executor 串行路径拆出去，收益更直接。
- commit 串行化是多 executor 前置约束；在没有 commit ordering 方案前直接做多 executor，风险会落到写命令顺序、AOF/replication 顺序和 response ownership 上。
- 多 executor 涉及 response routing、owner wake、write command ordering 和 shared state 锁边界，应放在 worker pool 和 commit 串行化边界清楚之后。

## 13. 建议提交

建议提交本阶段改动。

建议 git add：

```bash
git add src/network/proactor.c \
        tests/proactor_executor_stability_6d.sh \
        tests/proactor_executor_perf_compare_6d.sh \
        docs/route_c_perf_arch/30_stage6d_proactor_executor_stabilization_review.md
```

建议 commit message：

```bash
git commit -m "稳定proactor异步执行闭环观测"
```
