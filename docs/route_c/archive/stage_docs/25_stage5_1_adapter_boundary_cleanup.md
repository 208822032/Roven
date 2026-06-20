# 阶段 5.1：adapter 边界小清理与注释补强

## 1. 背景

阶段 5 已经把 reactor executor response 回投流程抽象为 async network adapter：

- executor 只产出 `kvs_response_task_t`。
- adapter target 保存网络 owner、session identity 和 owned response buffer。
- reactor ops 负责 validate、append、drop、restore read 和 release session。

阶段 5.1 不新增功能，只处理阶段 5 审查中留下的非阻塞边界问题，让后续阶段 6 接 proactor/io_uring adapter 时接口契约更清晰。

## 2. 阶段 5 遗留的非阻塞问题

审查关注点包括：

- `kvs_async_net_dispatch_response()` 对非法 `ops` / `target` 的契约不完整。
- `APPEND_FAILED` 时 `event_unregister()` 的责任边界不够集中。
- `restore_read` 失败可能触发 fd unregister，随后 dispatch cleanup 继续 release target，需要说明 refcount 边界。
- `kvs_async_net_response_target_t.owner` 实际是 runtime-private opaque pointer，需要明确 executor 不得解引用。

## 3. 本阶段修复和注释补强

本阶段做了这些小改动：

- `dispatch_response()` 在 `target != NULL` 但 mandatory ops 缺失时尽力执行 target cleanup，释放 executor response target 持有的 session ref。
- 头文件补充 ops 契约：`validate_owner` 和 `append_response` 是 mandatory ops，缺失属于 adapter 注册编程错误。
- `owner` 字段补充 opaque 指针说明：executor 只能复制，只有对应 owner ops 能解释。
- fd reuse 防护说明明确为 fd + owner pointer + session_id + generation。
- reactor `append_response` 不再直接 `event_unregister()`，只返回 append failure。
- reactor `drop_response(APPEND_FAILED)` 统一负责关闭仍然 current 的 fd。
- `restore_read` 注释说明 unregister 释放的是 reactor 持有的 session ref，dispatch cleanup 释放的是 executor target 持有的 ref。

## 4. 不改变行为的说明

阶段 5.1 不改变：

- command_task 执行模型。
- response_task owned buffer 生命周期。
- reactor-only executor 现状。
- proactor/ntyco 同步 fallback。
- replication connection 同步 fallback。
- 每 session 单 in-flight。
- pipeline 顺序。
- response 格式。
- RESP/RDB/AOF/主从协议。
- `g_kv_data_rwlock`。
- MOD/TTL 既有语义。

`APPEND_FAILED` 的可见行为保持为关闭当前连接并记录 executor drop stats，只是把关闭责任集中到 `drop_response`。

## 5. 生命周期边界

response dispatch 的生命周期边界为：

1. executor response queue 交出 `kvs_response_task_t`。
2. adapter 从 response task 构造 `kvs_async_net_response_target_t`。
3. owner ops 先 validate fd slot、owner pointer、session_id、generation 和 closing。
4. append 成功后，dispatch 清理 `single_inflight`。
5. reactor 尝试 enable write 和 restore read。
6. dispatch 释放 executor response target 持有的 session ref。
7. reactor 销毁 `kvs_response_task_t` 和 owned response buffer。

drop 路径中，dispatch 先调用 owner `drop_response()` 记录 reason 和必要的 owner cleanup，再统一 clear in-flight 和 release target session ref。

`restore_read` 可能在 append 成功后提交同连接中已缓存的下一条请求。如果提交失败，它可能 unregister fd 并释放 reactor 持有的 session ref；随后 dispatch release 的是 executor target 独立持有的 ref，不是重复释放。

## 6. 为什么不接 proactor/io_uring

阶段 6 接 proactor/io_uring 前还需要先明确：

- proactor connection context 的 refcount。
- read/write SQE 与 session closing/generation 的关系。
- owner poller wakeup 和 response append 的线程边界。
- fd reuse 与 user data generation 的统一校验方式。

阶段 5.1 只补清当前 adapter 契约，不引入新的运行时路径，避免在边界不清时扩大行为面。

## 7. 验证结果

阶段 5.1 应运行：

- `make clean && make embedding`
- `tests/vector/semantic_onnx_test.sh`
- KV SET/GET
- VSET/VGET/VSEARCH/VDEL
- pipeline 顺序
- disconnect/fd reuse
- high accept 200 或 500 连接

已知不归入阶段 5.1 的既有问题：

- `tests/ttl/test_ttl_all_backends.sh` 中 MOD 不清 TTL 的语义失败。
- `kvs_vector_rebuild_after_load` 隐式声明 warning。
- store/vector/HNSW 退出清理相关 LSAN 泄漏。

## 8. 后续阶段 6 准备事项

阶段 6 可以在不改变 executor 的前提下新增 proactor/io_uring ops：

- `owner` 指向 proactor connection context。
- `validate_owner` 校验 context generation、session_id、closing 和 SQE 生命周期。
- `append_response` 写入 proactor send buffer。
- `enable_write` 投递或更新 write SQE。
- `wake_owner` 唤醒所属 poller。
- `release_session` 归还 proactor context/session ref。
