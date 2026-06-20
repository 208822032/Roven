# 阶段 4C：LeakSanitizer 内存泄漏复盘

## 1. 背景

阶段 4C 的主目标不是新增 executor 能力，而是稳定阶段 4B 的 reactor-only single command executor 边界。验证时增加了 ASAN/UBSAN 和 LeakSanitizer 覆盖，结果功能路径全部通过：

- ONNX semantic test 通过。
- pipeline 顺序正确。
- high accept 200/500 短连接通过。
- disconnect / fd reuse 压力通过。
- 没有 UAF、double-free、stack-use-after-return 或 UBSAN runtime error。

但在服务端优雅退出时，LeakSanitizer 报告了少量泄漏。这个问题本身不影响阶段 4C 的在线正确性，但适合作为面试中说明“如何区分功能正确、内存安全、退出清理完整性”的案例。

## 2. 现象

第一次 ASAN/LSAN 运行时，使用了普通 benchmark 后留下的 `data/kvstore-embedding.rdb`。服务启动会加载约 1200 条 vector 记录，退出时 LeakSanitizer 报告较多泄漏，栈集中在：

- RDB load vector section。
- HNSW index rebuild / insert。
- vector string duplication。
- array/rbtree 初始化和数据恢复。

为了排除历史数据污染，验证时删除了测试生成的 embedding RDB/AOF，重新用干净库启动。干净库下功能仍全部通过，但退出时仍有约数百字节泄漏，代表问题不是“benchmark 数据太多”本身，而是 shutdown 清理路径仍不完整。

典型日志摘要：

```text
command executor stats: submitted=533 completed=533 cmd_depth=0 cmd_max=24 resp_depth=0 resp_max=49 dropped=0 eventfd_fail=0
ERROR: LeakSanitizer: detected memory leaks
SUMMARY: AddressSanitizer: 512 byte(s) leaked in 32 allocation(s).
```

关键点：

- executor stats 显示 submitted 与 completed 相等。
- command queue / response queue 最终 depth 都是 0。
- eventfd write fail 是 0。
- LeakSanitizer 报在进程退出阶段，不是在线请求处理阶段 crash。

## 3. 定位方法

定位时没有直接把所有 LSAN 输出当成一个问题处理，而是按栈来源分类：

1. 看是否有危险内存错误：
   - 是否出现 `heap-use-after-free`
   - 是否出现 `double-free`
   - 是否出现 `stack-use-after-return`
   - 是否出现 UBSAN runtime error

2. 看泄漏发生阶段：
   - 请求执行中泄漏。
   - response drop 路径泄漏。
   - executor stop/drain 泄漏。
   - 服务优雅退出时存储引擎未完整 destroy。

3. 看所有权链条是否闭合：
   - async command task 是否释放 owned argv。
   - response task 是否释放 owned buffer。
   - response queue 是否 drain。
   - session ref 是否 release。
   - fd slot owner / conn extension 是否释放。

4. 用 executor stats 交叉验证：
   - `submitted_tasks == completed_tasks` 说明 executor 没有丢未完成 task。
   - `command_queue_depth == 0` 说明 command queue 没有残留。
   - `response_queue_depth == 0` 说明 response queue 没有残留。
   - `eventfd_write_fail_count == 0` 说明回投唤醒没有失败积压。

## 4. 发现的问题类型

### 4.1 阶段 4C 修复的问题：event_register 失败清理

在 high accept 和 ASAN 压力下，检查到一个真实的阶段 4C 边界问题：

```text
init_conn_extension
  -> net_session_init
  -> malloc iovecs
  -> conn_extensions[fd] = ext
  -> event_register
  -> set_event / epoll_ctl ADD 失败
```

原路径里，如果 `event_register()` 已经初始化了 `conn_extension`，但最后 `set_event()` 失败，只关闭 client fd，不释放 `conn_extension`。这会导致：

- `net_session.recv_buf/send_buf` 泄漏。
- `iovecs` 泄漏。
- fd slot owner 指针残留风险。

阶段 4C 已修复：

- `init_conn_extension()` 中 `iovecs` 分配失败时释放 `ext`。
- `event_register()` 中 `set_event()` 失败时 `mark_closing + free_conn_extension + conn_list[fd].fd = -1`。

这个修复和本阶段的 high accept 目标一致，因为 EPOLLET + 并发 accept 会更容易覆盖连接注册失败边界。

### 4.2 仍存在但不在本轮修复范围的问题：存储层退出清理

干净库下仍有 direct leak 栈来自 `handle_set -> kvs_array_set_len`，以及 indirect leak 来自 `master_sync_command`、HNSW/vector insert 等路径。

这类泄漏的共同特征是：

- 数据已经成功写入 store。
- 退出时 RDB 能正常保存。
- 进程生命周期内数据本来就应该常驻内存。
- LeakSanitizer 在进程退出时要求 `dest_kvengine()` 完整释放所有 store 内存。

这说明问题更接近“存储引擎 destroy 不完整”，而不是“executor 在线路径泄漏”。阶段 4C 严禁修复 vector AOF rewrite/load 既有问题，也不应顺手重构存储层释放逻辑，所以这里只记录，不扩大范围。

### 4.3 夹带 async 栈的 indirect leak

LSAN 输出里还能看到 `reactor_submit_async_command`、`kvs_command_task_copy_argv`、`net_session_parse_one_resp` 等栈。

这容易误判为 executor task 泄漏，但需要结合 stats 和所有权链条判断：

- executor completed 等于 submitted。
- command queue depth 最终是 0。
- response queue depth 最终是 0。
- stop drain 释放 queued task/response。
- response append/drop 都释放 owned response buffer 并 release session ref。

因此这些栈更像是存储层或 shutdown 清理没有完全释放时，LSAN 把 allocation chain 上的 async 调用栈显示出来；不能仅凭栈里出现 `reactor_submit_async_command` 就认定 executor queue 泄漏。

## 5. 为什么没有在 4C 中彻底修掉所有 LSAN leak

阶段 4C 的边界是“reactor executor 稳定化与观测增强”，不是存储引擎 destroy 重构。本轮严禁事项包括：

- 不修复 vector AOF rewrite/load 既有问题。
- 不改变 RDB/AOF 文件格式。
- 不改变主从协议。
- 不顺手重构无关代码。
- 不进入阶段 5。

如果为了让 LeakSanitizer 完全干净而改动 array/rbtree/vector/distributed 的 destroy 路径，会带来两个风险：

1. 改动面超过 4C，容易把 executor 稳定化和存储生命周期重构混在一个阶段里。
2. 存储层 destroy 涉及 RDB/AOF、TTL、replication、vector HNSW，错误释放比退出泄漏更危险，可能引入 UAF 或 double-free。

因此本轮只修复确认属于 4C 边界的连接注册失败清理，其余作为已知退出清理缺口记录。

## 6. 面试表述版本

可以这样讲：

> 在 reactor executor 稳定化阶段，我用 ASAN/UBSAN/LSAN 跑 semantic、pipeline、断连、fd reuse 和 high accept。功能路径全部通过，也没有 UAF、double-free 或 UBSAN。但是 LeakSanitizer 在服务退出时报了少量泄漏。
>
> 我没有直接把它当成一个大问题修，而是先按栈分类。第一类来自 vector/RDB/HNSW 和存储结构，是进程生命周期数据在 `dest_kvengine()` 里没有完整释放，属于既有退出清理问题。第二类栈里出现了 async executor 的 `reactor_submit_async_command` 和 `kvs_command_task_copy_argv`，但结合 executor stats 看，submitted 等于 completed，command queue 和 response queue 最终深度都是 0，说明 executor 队列本身没有残留。
>
> 真正在本阶段修掉的是 `event_register` 失败路径：高并发 accept 下，如果 conn_extension 已经初始化，但 epoll add 失败，原来只 close fd，没有释放 session/iovec。我补了 mark closing 和 free_conn_extension，保证 fd 注册失败路径也闭合。
>
> 最后我把剩余 LSAN 报告记录为存储/vector/distributed 的退出清理缺口，没有在 executor 阶段顺手重构。原因是阶段边界很重要：4C 的目标是 reactor executor 的安全边界和观测，不应该把存储引擎 destroy 重构混进来，否则风险会比退出泄漏更高。

## 7. 可以强调的工程判断

这次问题体现了几个工程判断：

- sanitizer 报告要分层看，不是看到 executor 栈就认定 executor 泄漏。
- stats 可以帮助验证异步所有权是否闭环。
- UAF/double-free 和退出 leak 的风险等级不同，处理策略也不同。
- 高并发 accept 不只验证性能，也会覆盖连接初始化失败清理路径。
- 严格控制阶段边界，比“顺手修完所有看见的问题”更重要。

## 8. 后续建议

后续可以单独开一个“shutdown destroy 完整性”阶段，目标不是 executor，而是系统退出时的资源闭环：

- array/rbtree/hash/skiptable 的 key/value 释放一致性。
- vector records、embedding、HNSW graph、prepared/index rebuild 临时对象释放。
- distributed sync queue / master sync command 生命周期。
- TTL background 和 distributed shutdown 的 TSAN race。
- signal handler 中 `g_shutdown_flag` 改为 `sig_atomic_t` 或 atomic。

这个阶段应独立于 reactor executor 提交，避免把执行模型变化和存储生命周期重构混在一起。
