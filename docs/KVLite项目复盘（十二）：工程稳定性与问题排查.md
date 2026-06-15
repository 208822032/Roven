# 第十二章 工程稳定性与问题排查：内存、日志、资源生命周期和崩溃定位

## 12.1 为什么 C 项目的难点不只是功能实现

前面几章里，KVLite 已经把一个内存 KV 的主要链路基本串起来了。

从最短 `SET` 请求链路，到网络层缓冲区、RESP 协议解析、`command_table` 命令分发、多数据结构存储层、TTL、AOF/RDB、向量语义缓存、主从复制，再到第十一章的性能压测，这些内容证明了一件事：系统在正常输入和正常流程下可以跑起来。

但写 C 项目最容易误判的地方也在这里。功能能跑，不代表系统稳定。

因为 C 没有 GC，也没有自动资源托管。一个对象什么时候分配、谁拥有、什么时候释放、释放后还有没有线程会继续访问，都需要开发者自己说清楚。普通请求路径可能很顺，但后台线程、异常返回、连接断开、文件读写失败、shutdown 顺序一旦交叉，就可能出现内存泄漏、double free、use-after-free 或者 fd 泄漏。

模块越多，这类问题越明显。网络线程持有 `recv_buf` / `send_buf`，RESP parser 持有 tokens，命令层把 token 映射到 `command_context_t`，存储层保存 key/value，AOF/RDB 会临时读取和重放命令，复制线程会异步发送消息，向量模块还有 record、embedding 和 HNSW neighbor。任何一层对所有权理解错了，都可能在另一层暴露。

所以我后来对这个项目的理解变成了：

```text
功能实现解决的是能不能跑。
工程稳定性解决的是能不能长期、反复、在异常路径下稳定地跑。
```

这也是本章要复盘的重点。它不是继续介绍某个功能模块怎么设计，而是从内存生命周期、资源释放顺序、日志可观测性和崩溃定位这些角度，把前面的模块重新串起来。

## 12.2 内存管理不是 malloc/free 这么简单

最开始写 C 项目时，很容易把内存管理理解成：

```text
malloc 之后记得 free
```

这个原则当然没错，但真实项目里远远不够。

KVLite 里很多对象不是在一个函数里申请、一个函数里释放。比如 RESP parser 解析出的 token 会传给 `kvs_filter_protocol`，再通过 `parse_command_argument` 映射到 `command_context_t`。普通 KV 的 key/value 会被存储结构拷贝保存。向量模块的 question、answer、scope、embedding 会进入 vector store。复制消息会先由主线程生成，再交给队列和后台 worker 发送。AOF/RDB load 还会把文件里的数据临时读进 buffer，再重放到存储层。

这时“谁申请谁释放”必须进一步拆成几个问题：

```text
这个对象是临时借用，还是所有权转移？
这个指针指向 parser token，还是存储层自己的拷贝？
失败路径走到一半时，已经申请的资源谁清理？
队列 push 成功后，生产者还能不能 free？
后台线程退出前，主线程能不能销毁全局数据结构？
```

KVLite 里常见的内存风险包括：

- leak：异常路径提前返回，临时 buffer 没释放。
- double free：浅拷贝迁移后，新旧两个结构都释放同一批指针。
- use-after-free：主线程释放了数据结构，后台线程还在访问。
- buffer overflow：RESP bulk 长度、AOF/RDB 读取长度、网络 buffer 扩容边界没处理好。
- invalid free：释放了非当前内存后端分配的指针，或者释放已经被内存池销毁后的块。

这些问题通常不会在小规模功能测试里马上出现。它们更容易在 Pipeline、大 value、复制队列堆积、AOF/RDB 恢复失败、向量 rebuild、shutdown 这些路径里暴露。原因很简单：正常路径只走一条直线，稳定性问题往往藏在分叉和退出顺序里。

## 12.3 malloc、内存池和 jemalloc：三种内存后端的边界

KVLite 没有把内存分配散落在业务代码里，而是做了一层统一入口。

对外主要是：

```c
#define kvs_malloc(size) mem_alloc(size)
#define kvs_free(ptr)    mem_free(ptr)
#define kvs_realloc(ptr, new_size) mem_realloc(ptr, new_size)
```

实际后端由配置里的 `mem_backend` 决定。当前配置和源码里支持三类：

```text
pool
system
jemalloc
```

这三个后端的边界不一样。

`system malloc` 最简单，也最适合调试。它的行为更接近 ASan、Valgrind 这类工具的默认预期。怀疑自定义内存池有问题时，切到 system backend 是一个很有效的排除法：如果 system malloc 下问题消失，说明要重点看 pool；如果仍然复现，问题更可能是上层生命周期错误。

自定义 `mempool` 适合大量小对象。`src/storage/kvs_mempool.c` 里按 16、24、32、48、64、96、128、192、256、384、512、768、1024 这些 size class 建池，小块从 free list 里取，超过阈值的大块走 large block 记录链表。它的优势是减少频繁小对象分配的系统调用和碎片，但代价是实现复杂度更高：free list、chunk 记录、大块链表、锁、destroy 顺序都要正确。

`jemalloc` 更适合长时间运行和高并发分配场景。当前项目通过 `jemalloc_wrapper.c` 把 `jemalloc_alloc_impl` / `jemalloc_free_impl` / `jemalloc_realloc_impl` 接到统一内存层里。它不是为了“炫技”，而是给压测和长期运行提供一个成熟分配器选项。

配置化内存后端的价值很实际：

- 调试内存错误时，可以切到 `system`，让 ASan/Valgrind 的报告更直接。
- 做性能压测时，可以比较 `pool`、`system`、`jemalloc` 在小对象、混合对象、大块分配下的差异。
- 怀疑自定义池有 bug 时，可以用后端切换缩小范围。
- 业务层统一走 `kvs_malloc` / `kvs_free`，不需要到处改调用点。

这里的核心不是“我实现了内存池”，而是系统里有一条可切换、可验证、可回退的内存分配边界。

## 12.4 哪些对象适合池化，哪些对象不适合

内存池不是万能优化。

适合池化的对象通常有几个特点：

- 小对象。
- 生命周期短。
- 分配释放频繁。
- 大小固定或接近固定。
- 数量多，但单个对象不大。

比如队列节点、协议解析中的中间对象、某些 key/value metadata、小型结构体、连接扩展对象，都比较适合走池化思路。它们的分配模式重复，大小也比较稳定，用 size class 管理可以降低分配成本。

不适合池化的对象也很明确：

- 超大对象。
- 大小变化很大的对象。
- 生命周期很长且数量不稳定的对象。
- 向量 embedding 大数组。
- HNSW 多层 neighbor 数组。
- RDB/AOF 大 buffer。
- 网络层可能扩到很大的 `recv_buf` / `send_buf`。

这些对象如果硬塞进小块池，反而会增加复杂度。比如 vector embedding 是 `dimension * sizeof(float)` 的连续数组，HNSW 每个节点还有多层 `neighbors[level]`，大小和层数相关。RDB/AOF load 的 buffer 会根据文件和半包情况扩容。它们更适合走普通大块分配，或者交给 jemalloc 这种成熟分配器。

KVLite 的 mempool 当前也有这个边界：小于等于 size class 的走池，大块通过 large block 记录链表管理。这个设计提醒我，池化的目标不是“所有 malloc 都消失”，而是让高频小对象更可控。

面试里我会强调这一点：

> 内存池优化的是分配模式稳定的小对象。如果对象大小不稳定、生命周期很长，或者本身就是大数组，池化可能带来碎片、统计困难和释放顺序复杂化，不一定更好。

## 12.5 网络连接、buffer 和 RESP token 的生命周期

网络层是最容易暴露内存边界问题的地方，因为它直接处理字节流。

KVLite 现在把单连接状态抽成了 `net_session_t`，里面包括：

```text
recv_buf
send_buf
resp_parser
```

`recv_buf` 保存已经从 socket 读到、但还没有完全消费的字节。Pipeline、半包、粘包都依赖它。`net_session_process_resp` 会用 `resp_peek_command` 从 `recv_buf` 里探测一条完整 RESP 命令的长度，确认完整后再交给有状态 parser 解析。

`send_buf` 保存已经生成、但可能还没全部发出去的响应。尤其在 reactor/proactor 模式里，写事件可能一次发不完，所以需要 `send_offset` 记录发送进度。

RESP parser 的 token 生命周期要特别注意。当前 parser 在解析 bulk body 时会为每个 token 单独 `kvs_malloc(bulk_len + 1)`，并且用 `token_lens` 记录真实长度。这比直接让 token 指向 `recv_buf` 稳妥，因为 `recv_buf` 后续会 `netbuf_consume_front` 裁掉已消费字节。如果 token 长期引用 `recv_buf` 内部地址，裁剪或扩容后就会变成悬垂指针。

这里还有一个二进制安全问题：token 后面补了 `\0`，是为了兼容文本命令处理，但真正的长度必须看 `token_lens`。如果 key/value 里包含 `\0`，乱用 `strlen` 就会截断。前面的 binary-safe 修复已经覆盖了不少主路径，但这一章更关心的是：`command_context_t` 里的 `key`、`value`、`query_text`、`scope` 到底是借用 parser token，还是被下层拷贝保存。

连接关闭时，释放顺序也要完整：

```text
close fd
free recv_buf
free send_buf
free resp_parser tokens / token_lens
free connection extension
```

`net_session_free` 里做了 `netbuf_free(&recv_buf)`、`netbuf_free(&send_buf)` 和 `resp_parser_free(&resp_parser)`。reactor/proactor/ntyco 的连接扩展再负责释放 fd、iovecs 或上下文对象。

这个问题重要，是因为 shutdown 时网络线程和主线程最容易交叉。如果主线程已经释放了连接 buffer 或全局存储结构，而网络线程还在处理读写事件，就会出现 use-after-free。稳定的网络层不是只要能解析 RESP，还要保证连接生命周期闭环。

## 12.6 向量模块里的内存风险：record、embedding 和 HNSW neighbor

向量模块是 KVLite 里内存风险最高的模块之一。

普通 KV 的 value 通常是一段字符串或字节数组，而向量模块一条 record 里有更多动态字段：

```text
question
answer
scope
normalized_question
embedding
```

`kvs_vector_record_destroy` 必须逐个释放这些字段，然后把 record reset 成 empty。`VSET` 也不是简单覆盖一个 value，而是要复制 question/answer/scope，生成 normalized question，再生成 embedding。任何一步失败，都要释放前面已经分配的半成品。

这也是为什么 `kvs_vector_record_set_fields` 里大量出现：

```text
分配 question_copy
分配 answer_copy
分配 normalized_copy
分配 scope_copy
生成 embedding
失败时逐层释放
```

HNSW 的风险更明显。一个 HNSW node 不只有一个数组，而是：

```text
neighbors[level]
neighbor_counts
neighbor_capacities
```

每个节点的 level 不同，每一层 neighbor 数组都要单独分配和释放。`kvs_vector_ann_node_destroy` 必须先遍历每层 `neighbors[level]`，再释放 `neighbors`、`neighbor_counts` 和 `neighbor_capacities`。如果中途某层分配失败，初始化函数也要能清理已经创建的层。

删除路径也不能简单 `free(record)`。`VDEL` 当前采用逻辑删除：record 标记 deleted，从 id_map 移除，把 record index 放入 free list，并把索引标记 stale。这样做的原因是 HNSW 图结构直接删除节点很复杂，旧索引里可能还保存着 `record_index`。

为了防止旧索引误中新 record，KVLite 在索引项里保存 `record_id`。搜索时不仅看 `record_index`，还要比较：

```text
index_record_id == current_record.id
```

如果 record slot 被复用了，但 id 对不上，就说明这是 stale entry，必须跳过。

这里体现的是一个很重要的工程边界：

```text
record / embedding 是主数据。
flat / HNSW / SQ8 index 是派生索引。
```

主数据的生命周期由 store 管理，派生索引可以 rebuild。`kvs_vector_index_rebuild` 的做法是先 compact store、重建 id_map，再构建临时 flat/HNSW/SQ8 索引，成功后 swap 到全局状态；失败时销毁临时索引，保留旧状态。这个设计重点不是“重建更快”，而是避免 rebuild 失败把旧索引破坏掉。

需要谨慎的是：rebuild 里 compact store 会移动 active record，源码注释也明确写了 active record 是浅拷贝迁移，不能对旧 active record 再 destroy，否则会 double free。这类注释非常关键，因为它直接说明了所有权转移规则。

## 12.7 AOF/RDB 中的临时资源和恢复路径风险

持久化路径的问题，往往不在正常 save/load，而在失败路径。

AOF append 里有几个资源：

```text
AOF 文件句柄
AOF 内存 buffer
后台 fsync 线程
rewrite 临时文件和 rewrite buffer
loading / pause_rewrite 状态
```

`aof_init` 如果中途失败，需要按已经初始化的阶段反向释放：filename、buf、temp_filename、fp、mutex、cond、rewrite_mutex、后台线程。`aof_free` 则要先停止 rewrite，再停止后台 fsync 线程，最后 flush、关闭文件、释放 buffer 和 mutex。

AOF load 的风险更隐蔽。它会打开文件，分配一个可扩容 buffer，用 RESP parser 逐条解析命令，然后调用 `kvs_filter_protocol` 重放。重放期间必须设置 loading/sync context，避免“加载 AOF 时再次 append AOF”或者再次触发复制广播。源码里有 `aof_state.loading = 1`、`pause_rewrite = 1` 和 `set_sync_context(2)`，就是为了解决这个防重入问题。

RDB 的风险集中在临时 buffer、短读和半恢复状态。

`kvs_rdb_save` 先写临时文件，flush + fsync 后再 rename 到正式文件。这样至少避免写到一半直接覆盖旧 RDB。写失败时要关闭 fp 并 unlink 临时文件。

`kvs_rdb_load` 会先清空当前数据结构，再按 section 读取 array/rbtree/hash/skiptable/vector。每个 key/value 都是临时分配，写入存储结构后再释放。vector section 更复杂：RDB 读出 question、answer、scope、normalized_question、embedding 后，调用 `kvs_vector_restore_record_with_embedding` 恢复 record，最后 `kvs_vector_rebuild_after_load` 重建 id_map 和索引。

这里为什么重要？

因为恢复路径本质上是在“用外部文件重建内存状态”。文件可能短读，格式可能错，vector rebuild 可能失败，内存可能分配失败。如果恢复到一半失败，系统要么能明确返回错误，要么能保证不会泄漏临时资源。更进一步，后续可以考虑把 RDB load 做成临时状态构建成功后再整体 swap，避免半恢复状态污染现有内存。

当前正文里我不会说 KVLite 已经完整实现事务化恢复。更准确的说法是：项目已经在多处处理了临时 buffer 释放、RDB 临时文件、AOF loading 防重入和 vector rebuild，但恢复失败后的整体回滚语义还有继续强化空间。

## 12.8 复制链路里的引用计数、队列和批量发送释放问题

复制链路的稳定性重点不是“能不能把命令发给 slave”，而是消息在异步路径里的所有权。

TCP 复制路径里，写命令执行成功后，`master_sync_command` 会编码出一条 `sync_msg_t`，再调用 `sync_backend_broadcast_owned`。这里的语义很关键：push 成功后，消息所有权转移给 TCP backend 队列；push 失败时，生产者负责释放。

后台 worker 从 `msg_queue` 取出消息，拷贝 slave 列表快照，在锁外对每个 slave 发送，最后释放 `slaves_snapshot` 和 `msg`。这个路径里当前没有显式 refcount，因为一条消息由一个 worker 拿到后同步 fanout 给所有 slave，发送完成统一释放。

但 eBPF syncd 里就出现了显式引用计数。`ebpf_sync_msg_t` 里有 `atomic_int refcnt`，`broadcast_to_all_slaves` 会按运行中的 slave 数量设置引用数，然后把同一条消息指针 push 到多个 slave 队列。每个 slave sender 用完后调用 `ebpf_sync_msg_put`。

这类代码为什么危险？

因为 refcount 少减一次，就泄漏。多减一次，就可能 double free。某个队列 push 失败时忘记 put，也会泄漏。某个 sender 线程还没发完，主线程提前清理队列或关闭 socket，就可能变成 use-after-free 或发送失败后的残留消息。

复制链路还有几个高发场景：

- 队列满：push 失败时必须释放或回退。
- slave 慢：队列堆积，消息生命周期变长。
- send 失败：不能既释放又让别的线程继续持有。
- slave 断开：fd 生命周期和 sender 线程退出要同步。
- shutdown：先停止生产新消息，再 drain 或丢弃队列，再 join worker。

`tx_batcher` 解决的是另一个相关问题：不要每条消息都直接 send，而是先 append 到批量 buffer，满了或定时再 flush。它提升吞吐，但也增加了退出时的责任：线程退出前要 flush 尾部数据，send 失败时要重置 fd 和 batcher 状态。

所以复制稳定性不是只看“写命令广播”，而是看队列、消息所有权、refcount、socket 和线程退出这几件事能不能闭环。

## 12.9 eBPF syncd 的资源生命周期：ringbuf、link、socket 和线程

eBPF syncd 不是加载一个 BPF 程序就结束。

它同时涉及内核态资源和用户态资源：

```text
BPF object
BPF link
BPF config map
BPF ringbuf
master FULLDUMP socket
slave socket
每个 slave 的 msg_queue
每个 slave 的 sender thread
tx_batcher
用户态 rx stream buffer
```

这些资源的生命周期不能混在一起看。

BPF object/load/attach 失败时，要关闭已经打开的 object，销毁已经 attach 的 link。ringbuf 创建成功后，主循环通过 `ring_buffer__poll` 消费事件；退出时必须先停止循环，再 `ring_buffer__free`。否则 poll 线程继续访问已经释放的 ringbuf，就是典型 UAF。

slave sender 线程也一样。每个 slave 有自己的 fd、queue、worker 和 tx_batcher。退出时应该先把 `running` 置 0，让 worker 跳出循环，然后 join worker，最后 close fd、destroy queue。如果先 close socket，worker 还在 send，轻则报错，重则访问已失效状态。

eBPF syncd 还做了 FULLDUMP：启动时先连 master 普通服务端口，发送 `FULLDUMP`，收到数据后广播给 slave。这里的临时 socket、接收 buffer、非阻塞读取错误都需要清理。

这类资源和普通 `malloc/free` 不一样。它是：

```text
内核对象 + 用户态线程 + socket + 队列 + 批量发送缓冲
```

只要其中一个退出顺序错了，问题就可能表现成 ringbuf poll 异常、send 失败、消息泄漏、refcount 错误或者 shutdown 卡住。

## 12.10 日志系统：为什么日志也会影响稳定性

日志不是核心业务功能，但它会影响稳定性。

一方面，日志是排障线索。shutdown segfault 这类问题，如果没有日志、dmesg、core、gdb 这些证据，定位只能靠猜。另一方面，日志本身也是代码路径。高频日志会放大 IO 开销，同步日志可能阻塞主链路，异步日志如果退出顺序错了，也会访问已释放资源。

KVLite 当前日志入口统一到了 `KVS_LOG_ERROR`、`KVS_LOG_WARN`、`KVS_LOG_INFO`、`KVS_LOG_DEBUG` 这些宏，再进入 `kvs_log_write` 统一格式化。日志里包含时间、级别、模块、文件行号和消息内容。这个统一出口很重要，因为它让后续的异步写盘、日志级别、错误日志带 errno 都能集中处理。

同步日志的优点是简单可靠。写完函数返回，基本能确定日志已经输出。但缺点也明显：如果热路径高频写文件，业务线程就会被 IO 拖住。尤其是协议错误、复制 send 失败、AOF fsync 错误这类路径，如果压力下疯狂打日志，可能把问题进一步放大。

异步日志的优点是削峰。业务线程格式化后入队，后台线程统一写文件。但它也带来新的生命周期问题：队列满了怎么办？ERROR 能不能丢？退出时剩余日志是否 flush？日志文件什么时候 close？后台线程什么时候 join？

稳定性上，日志的目标不是越多越好，而是：

```text
正常路径不要拖垮系统。
异常路径保留关键证据。
退出路径尽量 flush 完整。
```

## 12.11 异步日志：队列削峰、丢弃策略和退出 flush

`docs/log/phase3_async_logging.md` 里记录了 KVLite 异步日志的设计：用一个有界环形队列把业务线程和文件 IO 解耦。

当前 `kvs_log_writer.c` 里队列容量是 256，每条记录使用固定长度 buffer，后台线程一次最多批量取 64 条 flush。实现没有上来就做无锁队列，而是用 `mutex + condvar + ring buffer`，这是更容易验证退出语义的选择。

队列满时策略也比较清楚：

- `INFO` / `DEBUG` 低优先级日志可以丢弃，并累加 dropped 统计。
- `WARN` / `ERROR` 不直接丢，改为同步 fallback 写出。
- 退出时输出一条 dropped summary，避免静默丢失。

这个策略体现的是取舍：高压下不能让日志无限阻塞主流程，但关键错误又不能完全丢掉。

退出路径同样重要。`kvs_log_shutdown` 会先把全局 writer 摘掉并设置 shutting_down，再调用 `kvs_log_writer_shutdown`。writer shutdown 里设置 stopping，signal cond，join 后台线程，输出 dropped summary，最后 flush/close stream、销毁 mutex/cond、释放 queue。

异步日志和复制队列、`tx_batcher` 本质上是同一类工程思想：

```text
主流程产生事件。
后台线程批量处理。
队列满时有明确策略。
退出时按顺序 flush / join / free。
```

这种思想一旦引入，稳定性关注点也从“函数返回值对不对”扩展成“事件有没有残留，后台线程有没有退出，资源有没有提前释放”。

## 12.12 shutdown segfault：一次 use-after-free 问题排查复盘

`docs/shutdown_segfault_fix_report.md` 记录了一次很典型的稳定性问题。

现象是：KVLite 在 shutdown 时偶发 segmentation fault。它不是每次都复现，但在压测、Pipeline、大量请求之后退出时更容易触发。

第一步不是改代码，而是收集证据。

报告里记录，`dmesg` 里有 7 次历史崩溃，其中 3 次像 libc heap corruption，4 次集中在同一位置：`handle_mod`，进一步链路是：

```text
handle_mod
→ kvs_rbtree_mod_len
→ kvs_rbtree_lookup_node
→ 访问已释放 rbtree node 的 left/right/key/value 字段
```

`fault address 0x2c` 这种信息很关键。它不像普通参数错误，更像是结构体指针已经无效，然后访问结构体内部某个偏移字段。结合 `addr2line` / gdb 定位到 rbtree lookup，基本可以推断：正常命令逻辑未必有问题，更像是 shutdown 阶段还有线程在访问已经释放的全局 rbtree。

然后再看 shutdown 顺序。

修复前最可疑的点有两个：

1. `master_sync_server` 线程是 `while(1)`，没有退出条件，`distributed_shutdown` 也没有完整 join 它。
2. `dest_kvengine()` 的释放顺序不对，先销毁 array/rbtree/hash/skiptable/vector 和 mempool，再停 distributed / AOF。

这就形成了 use-after-free：

```text
后台分布式线程仍可能访问 global_rbtree
主线程已经 destroy rbtree node
甚至 mem_destroy 已经释放内存池
后台线程再 lookup / mod
崩溃
```

修复思路不是“在崩溃点加 if”，而是调整生命周期。

新的顺序是：

```text
1. 先设置 shutdown 标记，停止接收新请求
2. 停止 distributed 相关后台线程
3. 停止 AOF/rewrite/fsync 等后台资源，并做必要 flush
4. 停 TTL 后台线程
5. 加写锁，销毁全局数据结构和 vector
6. 最后 mem_destroy
7. 最后关闭日志系统
```

源码里的 `dest_kvengine` 已经体现了关键顺序：TCP 模式下先 `distributed_shutdown()`，再 `aof_free()`，然后拿 `g_kv_data_rwlock` 写锁销毁全局存储结构和 vector，最后 `mem_destroy()`。

分布式模块也做了生命周期修复：保存 `master_thread` 和 `master_listen_fd`，shutdown 时先 `shutdown/close` listen fd 唤醒阻塞的 `accept`，再 join master thread，之后 join slave 线程，最后关闭 sync backend 和释放 slave 数组。

报告里还顺手修了一个接口风险：`rbtree_delete()` 曾经在 `kvs_free(y)` 后仍然 `return y`，这是返回 freed pointer 的危险接口。虽然调用方没有使用返回值，但这种接口会误导后续维护者，所以改成返回 NULL 更稳。

回归验证也不是只跑一次。

报告里记录的 ASan 编译命令是：

```bash
OPT_CFLAGS="-O1 -g -fno-omit-frame-pointer" \
EXTRA_CFLAGS="-fsanitize=address" \
EXTRA_LIBS="-fsanitize=address" \
make
```

验证包括：

- `test_binary_key`：36 passed，0 failed。
- `mempool_test`：PASS，无 sanitizer 错误。
- `testcase mode 5 pipeline`：PASS，ASan 下也通过。
- 正常 shutdown：干净退出，日志出现 master sync server stopped。
- `tests/repro_shutdown_segfault.sh`：循环启动、并发读写、随机 SIGTERM，专门覆盖退出路径。

这次排查最大的收获是：shutdown 路径和正常请求路径一样重要。稳定性 bug 不能靠猜，要靠证据链定位：

```text
dmesg/core 发现崩溃特征
addr2line/gdb 定位源码路径
结合线程生命周期推理 UAF
调整 shutdown 顺序
ASan + 回归测试验证
```

## 12.13 ASan、Valgrind、gdb、dmesg 和 addr2line 分别解决什么问题

工具不能代替分析，但工具能帮我建立证据链。

`dmesg` 适合第一时间看进程是怎么死的。比如是不是 SIGSEGV，fault address 是多少，ip 大概在哪个地址。它给的是系统层面的崩溃摘要。

core dump + `gdb` 适合看崩溃现场。可以看调用栈、线程、局部变量、寄存器、结构体内容。`tests/repro_shutdown_segfault.sh` 里也预留了 `gdb -batch -ex "bt full"`，崩溃后自动导出部分 backtrace。

`addr2line` 适合把地址映射回源码文件和行号。dmesg 里常常只有地址，没有源码行，这时用带 `-g` 的可执行文件配合 addr2line 才能定位到具体代码。

ASan 适合发现运行时内存错误：

- heap-use-after-free
- double free
- heap-buffer-overflow
- invalid free
- stack-use-after-scope 等

它的优势是报错非常直接，通常能给出分配栈、释放栈和非法访问栈。缺点是需要重新编译，而且性能开销明显，不适合长期压测。

Valgrind 更适合小规模慢速验证，尤其是泄漏检查和非法读写。它不要求像 ASan 那样依赖编译器插桩，但运行更慢。适合在关键 testcase 或小数据集上跑。

gdb 除了看 core，也适合复现时 attach、打断点、看线程状态。比如 shutdown 卡住时，可以看哪个线程还在 `accept`、`send`、`pthread_cond_wait` 或者拿着锁不放。

perf 更偏性能热点，不是本章重点。但当稳定性问题表现成“退出很慢”或者“日志/复制线程 CPU 异常高”时，perf 也能辅助判断。

这里的重点是：工具不是目的，证据链才是目的。面试里只说“我会 ASan/gdb”价值不高，要能讲清楚“我先看到什么证据，再推断什么，再用什么工具验证”。

## 12.14 如何做回归验证：不是修完能跑一次就结束

稳定性问题最怕“修完刚好不复现”。

因为 UAF、double free、队列竞态、shutdown 顺序问题，本来就是概率性问题。修完后手动启动一次、关闭一次，没有崩溃，并不能证明修复有效。

KVLite 里这类修复应该至少覆盖四类路径：

```text
正常路径
压力路径
异常路径
退出路径
```

具体到本项目，我会这样设计回归：

- Pipeline 大请求：验证 `recv_buf`、RESP parser、`send_buf` 和批量响应顺序。
- binary key/value：验证 `token_lens`、key_len/value_len 没被 `strlen` 截断。
- `mempool_test`：验证小块、大块、混合保留、碎片后大块分配等分配模式。
- vector set/search/delete/rebuild：验证 record、embedding、free list、stale index 和 rebuild。
- AOF load/append：验证 loading 状态不重复 append，不重复复制。
- RDB save/load：验证临时文件、短读错误、vector section 和 rebuild。
- master/slave sync：验证队列 push、worker send、slave 断开。
- eBPF syncd：验证 ringbuf、sender thread、queue 和 socket 的退出。
- shutdown 多轮压测：循环启动、并发读写、SIGTERM、检查 core/log。
- ASan build：重点看 UAF、double free、overflow、invalid free。

回归测试还要防止“修一个路径，破另一个路径”。比如调整 shutdown 顺序时，不能只验证 distributed，AOF flush、RDB save、TTL 后台线程、日志 shutdown 也要一起看。因为真实问题往往就发生在多个模块同时退出的时候。

## 12.15 这个模块可以关联哪些八股知识点

#### C 语言八股

- `malloc/free`：动态内存申请和释放。
- 内存泄漏：异常路径没有释放资源。
- use-after-free：对象释放后仍被访问。
- double free：同一块内存释放两次。
- buffer overflow：写越界，常见于长度计算错误。
- 野指针：指针指向无效地址。
- 指针生命周期：借用、拥有、转移、失效。
- ownership：谁负责释放对象。
- shallow copy / deep copy：向量 store compact 里 active record 浅拷贝迁移就是典型例子。
- struct 资源管理：结构体内部字段需要成对 destroy。
- 函数返回错误码：C 项目用返回值传播失败。
- `goto cleanup` 模式：失败路径统一回收已分配资源。

#### Linux / 系统八股

- file descriptor 生命周期：socket、RDB/AOF 文件、BPF map/link 都有 fd 或内核资源。
- socket close：close 不能和其他线程 send/recv 乱序。
- pthread 退出：设置退出标记、唤醒阻塞调用、join 线程。
- 条件变量：异步日志和 AOF fsync 线程都依赖 condvar。
- atomic 引用计数：eBPF syncd 的多 slave 共享消息需要 refcnt。
- signal / SIGSEGV：崩溃信号和 fault address。
- core dump：崩溃现场快照。
- dmesg：查看内核记录的进程崩溃信息。
- addr2line：地址映射到源码行。
- ASan：编译插桩发现内存错误。
- Valgrind：慢速检查泄漏和非法访问。
- gdb：调用栈、变量、线程状态。
- page cache / fsync：AOF/RDB 的刷盘语义。
- ringbuf / epoll：eBPF ring buffer 和 reactor 网络事件。

#### 后端工程八股

- 优雅退出：先停入口，再停后台线程，最后释放全局资源。
- 异步队列：日志队列、复制队列、eBPF slave queue。
- 日志丢弃策略：低优先级丢弃，高优先级同步 fallback。
- 资源释放顺序：destroy 不是随便调用。
- 后台线程 shutdown：退出条件、唤醒、join、清理。
- 回归测试：修复后覆盖正常、压力、异常、退出路径。
- 故障复盘：现象、证据、推理、根因、修复、验证。
- 可观测性：日志、dmesg、core、统计计数。
- 防御式编程：长度检查、NULL 检查、错误码、边界条件。

#### 存储系统八股

- WAL / AOF loading：重放时防止再次 append。
- snapshot loading：RDB 恢复内存状态。
- recovery context：loading/sync_context 防重入。
- replay 防重入：重放命令不能再次复制。
- 主数据和派生索引：vector record 是主数据，HNSW 是派生索引。
- rebuild：恢复后重建 id_map 和 index。
- crash consistency：临时文件 + fsync + rename。
- partial failure：读到一半失败、重建失败、send 失败时如何处理。

## 12.16 面试中怎么讲工程稳定性和问题排查

#### 1 分钟版本

> KVLite 这个项目里，我后来意识到 C 项目不是功能能跑就结束。前面我实现了网络、RESP、命令分发、多数据结构、TTL、AOF/RDB、向量检索和复制，但真正难的是内存生命周期和跨模块资源释放。
>
> 我做了统一内存分配入口，底层可以切 system malloc、自定义 pool 和 jemalloc。调试时用 system malloc/ASan 更直观，压测时可以比较 pool 和 jemalloc。网络层里每个连接管理自己的 recv_buf、send_buf 和 RESP parser tokens；向量模块里 record、embedding、HNSW neighbor 都有明确 destroy；AOF/RDB load 设置 loading/sync context，避免重放时再次 append 或复制。
>
> 我还排查过一次 shutdown 偶发 segfault。dmesg 和 addr2line 指向 `handle_mod` / rbtree lookup，推断是后台分布式线程在 shutdown 期间访问了已经释放的 rbtree node。最后调整 shutdown 顺序：先停接收和分布式线程，再停 AOF，拿写锁销毁全局数据结构，最后 mem_destroy。修复后用 ASan、binary key test、mempool test、Pipeline 测试和 shutdown stress test 回归。
>
> 所以我讲工程稳定性，不是只说会用工具，而是讲清楚对象所有权、线程退出顺序、证据链定位和回归验证。

#### 3 分钟版本

> KVLite 里工程稳定性主要分几块。
>
> 第一是内存后端。我把业务分配统一到 `kvs_malloc/kvs_free`，底层支持 pool、system malloc 和 jemalloc。pool 适合小对象和高频短生命周期分配，system malloc 适合调试，jemalloc 适合长时间运行和碎片控制。这样出问题时可以切后端判断是 allocator 问题还是上层生命周期问题。
>
> 第二是请求链路生命周期。每个连接有 `recv_buf` 和 `send_buf`，RESP parser 解析 bulk string 时会分配 token，并用 `token_lens` 保留二进制长度。`command_context_t` 只是把 token 映射成命令参数，下层如果要长期保存 key/value 必须自己拷贝。连接关闭时释放 fd、buffer、parser token 和连接扩展对象。
>
> 第三是复杂模块的资源管理。向量 record 里有 question、answer、scope、normalized_question 和 embedding，HNSW node 还有多层 neighbor 数组。VSET 失败路径要清理半成品，VDEL 采用逻辑删除和 free list，索引里用 record_id 跳过 stale entry，rebuild 时先构建临时索引，成功后再 swap。AOF/RDB load 里有临时 buffer 和 loading/sync_context，避免恢复时再次 append 或复制。
>
> 第四是异步资源。TCP 复制里消息 push 成功后所有权转移给队列，worker 发送后释放；eBPF syncd 里多 slave 共享消息，需要 atomic refcount。异步日志也是类似思路：业务线程入队，后台线程批量 flush，队列满时 INFO/DEBUG 可丢，WARN/ERROR 同步 fallback，退出时 flush 剩余日志。
>
> 最典型的问题是 shutdown segfault。现象是退出时偶发崩溃，dmesg 里多次指向 `handle_mod`，addr2line/gdb 进一步定位到 rbtree lookup 访问释放后的 node。根因是 shutdown 顺序错了，后台分布式线程还没退出，主线程已经销毁全局数据结构和内存池。修复后调整顺序：先停后台线程，再释放 AOF，再拿写锁销毁数据结构，最后 mem_destroy。然后用 ASan、Pipeline、binary key、mempool 和 shutdown stress 做回归。
>
> 这体现的是我对 C 后端项目的理解：功能正确只是第一步，真正上线需要能处理异常路径、退出路径和长期运行下的资源生命周期。

#### 面试官追问方向

**你项目里哪里最容易内存泄漏？**

AOF/RDB load 的临时 buffer、向量 VSET 失败路径、HNSW neighbor 分配失败、复制队列 push 失败、eBPF 多 slave 共享消息这些地方最容易泄漏。因为它们都有“分配到一半失败”或者“所有权转移”的问题。

**use-after-free 是怎么发生的？**

典型场景是 shutdown：后台线程还在访问全局数据结构，主线程先 destroy 了存储结构或 mempool。KVLite 的 shutdown segfault 就是这种类型。

**shutdown segfault 怎么定位？**

先看 dmesg/core，确认 SIGSEGV 和 fault address；再用 addr2line/gdb 定位源码栈；结合线程模型判断是不是后台线程访问已释放资源；最后调整 shutdown 顺序并用 ASan 和压力回归验证。

**ASan 和 Valgrind 有什么区别？**

ASan 是编译插桩，发现 UAF、double free、越界更直接，速度比 Valgrind 快，但需要重新编译并有明显开销。Valgrind 不需要专门插桩，泄漏检查方便，但更慢，适合小规模验证。

**内存池有什么风险？**

实现复杂，free list、大块链表、锁和 destroy 顺序都可能出错。池化不适合所有对象，错误池化可能增加碎片和调试难度。

**为什么不是所有对象都适合池化？**

大对象、变长对象、长生命周期对象不适合。比如 vector embedding、HNSW neighbor、RDB/AOF 大 buffer，用普通大块分配或 jemalloc 更合适。

**异步日志队列满了怎么办？**

KVLite 的策略是低优先级 INFO/DEBUG 可以丢弃并统计，WARN/ERROR 走同步 fallback，退出时输出 dropped summary。

**后台线程退出顺序怎么保证？**

先设置退出标记，唤醒阻塞点，比如 close listen fd 唤醒 accept 或 cond_signal 唤醒等待线程，再 join，最后释放线程使用的资源。

**复制消息引用计数为什么容易出错？**

多 slave 共享同一条消息时，每个消费者都要 put 一次。少 put 泄漏，多 put double free，提前 free 就 UAF。队列 push 失败也要处理引用。

**如果线上偶发崩溃，你会怎么排查？**

先收集 dmesg、core、日志和版本信息，确认信号、地址和调用栈；如果能复现，开 ASan 或 debug build；如果不能稳定复现，做压力复现脚本；定位后最小修改生命周期或边界条件；最后补回归测试覆盖同类路径。

## 12.17 如果继续优化，可以怎么做

后续我会从规范、工具和专项测试三个方向继续补。

- 引入统一资源管理规范，明确 init/destroy、ownership transfer、borrowed pointer 的注释格式。
- 为每个模块补充 destroy 顺序文档，尤其是 network、AOF、distributed、eBPF、vector。
- 增加 ASan/UBSan/TSan 构建目标，而不是每次手写 `EXTRA_CFLAGS`。
- 增加 Valgrind nightly test，专门跑小规模泄漏检查。
- 增加 shutdown stress test 到自动化测试里。
- 增加 fault injection，比如 malloc 失败、短读、send 失败、队列满。
- 增加日志 flush 超时和 dropped 日志统计导出。
- 增加 memory leak metrics 或内存后端统计。
- 增加 refcount debug 模式，检查 eBPF syncd 消息引用计数。
- 增加 vector rebuild 失败回滚测试。
- 增加复制队列满、慢 slave、slave 断开测试。
- 增加 eBPF ringbuf 丢事件和 poll 错误测试。
- 增加代码里的 `goto cleanup` 规范，减少多出口遗漏释放。
- 增加 clang-tidy / cppcheck 静态分析。

这里面我最优先做的是 shutdown stress、fault injection 和 sanitizer 构建目标。因为它们最能覆盖 C 项目里真实出问题的路径。

## 12.18 本章小结

第十一章讲的是性能是否站得住，第十二章讲的是系统在异常路径和长期运行下是否稳。

KVLite 的工程稳定性集中体现在：

- 内存所有权。
- 资源释放顺序。
- 后台线程退出。
- 异步队列清理。
- 日志 flush。
- 恢复路径防重入。
- 工具化验证。

这章最重要的不是某个工具，而是方法：

```text
先看现象。
再收集证据。
再推理根因。
再最小修复。
最后回归验证。
```

面试时工程稳定性是加分项，因为它说明我不是只会写功能清单，也经历过真实的排查和修复。

如果说前面章节证明 KVLite 能跑、能存、能恢复、能同步、能检索、能压测，那么这一章证明的是：当它出问题时，我知道怎么定位、怎么修、怎么验证。
