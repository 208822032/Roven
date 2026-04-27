# KVLite 分层架构深度复盘

先说结论：KVLite 的真实热路径不是旧的 resp_process_command 或旧式 vtable，而是 启动编排 -> 网络前端 -> 会话/缓冲 -> RESP 解析 -> kvs_filter_protocol -> command_table -> handle_* -> 具体数据结构。TTL、持久化、主从同步都不是独立平铺的功能块，而是嵌在命令执行前后和节点字段里的跨层机制。

一个很关键的事实是：当前仓库里，resp_process_command 只在头文件里声明，src/ 中没有调用；kvs_operations.h / kvstore.h 里保留的旧操作表也没有进入热路径。当前热路径以 command_table 和各个 handle_* 为中心。

## 第一部分：KVLite 的分层总览

### 分层图

启动编排（src/core/kvstore.c）
-> 配置、日志、内存、RDB、AOF、复制、TTL 后台初始化
-> 网络前端（reactor / proactor / ntyco）
-> 事件驱动与连接管理
-> net_session / netbuf
-> RESP 解析与编码
-> kvs_filter_protocol + command_table
-> handle_* 命令层
-> 四种具体数据结构
-> TTL 跨层机制
-> 持久化层（AOF / RDB / FULLDUMP）
-> 主从复制层（TCP / eBPF）
-> 公共基础设施层（config / log / queue / batcher / globals / memory）

### 1. 网络接入层
- 主要职责：把 socket 接入、accept、读写、非阻塞设置、前端循环稳定跑起来；当前仓库同时提供 reactor、proactor、ntyco 三种前端，编译入口见 [Makefile](../Makefile#L26) 和 [Makefile](../Makefile#L55)，运行时由 [src/core/kvstore.c](../src/core/kvstore.c#L250) 按配置选择。
- 不负责什么：不解析 RESP 语义，不决定 SET / GET / EXPIRE 之类的业务规则，不直接碰存储结构。
- 上层调用来源：main 中的 [reactor_start](../src/core/kvstore.c#L381)、[proactor_start](../src/core/kvstore.c#L378)、[ntyco_start](../src/core/kvstore.c#L375)。
- 下层依赖对象：net_session_t、netbuf_t、resp_parser_t、kvs_filter_protocol 回调。
- 对应源码位置： [src/network/reactor.c](../src/network/reactor.c#L148), [src/network/proactor.c](../src/network/proactor.c#L195), [src/network/ntyco.c](../src/network/ntyco.c#L361), [src/network/net_session.c](../src/network/net_session.c#L53), [src/utils/netbuf.c](../src/utils/netbuf.c#L10)
- 关键结构体 / 函数：reactor_start、proactor_start、ntyco_start、accept_cb、recv_cb、send_cb、set_event_recv、set_event_send、ntyco_conn_routine。
- 面试里应该怎么概括：这一层的本质是“把网络事件变成稳定字节流入口”，只负责接入和回包，不理解业务命令。

### 2. 事件驱动 / 连接管理层
- 主要职责：维护连接对象、收发缓冲、发送偏移、在路由层和业务层之间做背压控制；reactor 里是 conn_list + conn_extensions，proactor 里是 conn_extension_t + in_flight 标志，ntyco 里是每连接一个协程上下文。
- 不负责什么：不决定命令的语义，不做 TTL 判断，不做 AOF / 复制决策。
- 上层调用来源：网络接入层的 accept / epoll / io_uring / coroutine 调度。
- 下层依赖对象：net_session_t、netbuf_t、resp_parser_t、epoll / io_uring / poll。
- 对应源码位置： [src/network/reactor.c](../src/network/reactor.c#L148), [src/network/reactor.c](../src/network/reactor.c#L305), [src/network/reactor.c](../src/network/reactor.c#L393), [src/network/proactor.c](../src/network/proactor.c#L195), [src/network/proactor.c](../src/network/proactor.c#L231), [src/network/ntyco.c](../src/network/ntyco.c#L160)
- 关键结构体 / 函数：conn_extension_t、ntyco_conn_ctx_t、event_register、event_unregister、recv_cb、send_cb、set_event_recv、set_event_send、post_recv_if_needed、post_send_if_needed。
- 面试里应该怎么概括：这一层负责“连接状态机”和“发送背压”，让网络层和协议层解耦。

### 3. 协议解析与命令分发层
- 主要职责：把字节流变成 tokens，再把 tokens 变成命令上下文，最后把命令路由到对应 handler；这是整个系统里最标准的“协议层 + 分发层”。
- 不负责什么：不关心存储结构内部实现，不关心 socket 读写细节，不直接做持久化文件格式。
- 上层调用来源：net_session_process_resp 交上来的完整 RESP 命令；AOF replay 和 eBPF 复制也会复用这层。
- 下层依赖对象：command_table、command_context_t、handle_*、kvs_filter_protocol、resp_generate_*。
- 对应源码位置： [src/network/resp_protocol.c](../src/network/resp_protocol.c#L89), [src/network/resp_protocol.c](../src/network/resp_protocol.c#L216), [src/network/resp_protocol.c](../src/network/resp_protocol.c#L538), [src/core/kvs_operation.c](../src/core/kvs_operation.c#L500), [src/core/kvstore.c](../src/core/kvstore.c#L47)
- 关键结构体 / 函数：resp_peek_command、resp_parser、resp_parser_init、resp_parser_reset、resp_parser_free、resp_generate_simple_string_n、resp_generate_bulk_string_n、lookup_command、check_arity、parse_command_argument、kvs_filter_protocol。
- 面试里应该怎么概括：这一层的核心是“把命令解释对，并且把参数和路由规则统一起来”。

### 4. 存储引擎抽象层
- 主要职责：为上层提供统一命令入口和参数归一化，决定同一个 SET / GET / DEL 命令应该落到哪种数据结构；当前实现更像“软抽象”，不是一个真正独立的 runtime vtable 层。
- 不负责什么：不负责 TCP 读写，不负责 RESP 流切包，不负责具体节点的内存分配策略，不负责 AOF / RDB 的文件格式。
- 上层调用来源：kvs_filter_protocol、AOF replay、slave 回放、FULLDUMP。
- 下层依赖对象：四种具体数据结构实现，以及 TTL / 持久化 / 复制的后置钩子。
- 对应源码位置： [src/core/kvs_operation.c](../src/core/kvs_operation.c#L500), [include/kvs_operations.h](../include/kvs_operations.h), [include/kvstore.h](../include/kvstore.h#L285)
- 关键结构体 / 函数：command_info、command_context_t、command_table、handle_set、handle_get、handle_del、handle_exist、handle_range、handle_sort、handle_expire、handle_ttl、handle_persist、handle_pexpireat。
- 面试里应该怎么概括：这一层的价值是“把命令语义和底层结构解耦”，但要诚实地说，当前仓库的抽象还不够纯，很多策略都混在 kvs_operation.c 里。

### 5. 具体数据结构实现层
- 主要职责：真正存 key / value / expire_at_ms，并完成 point lookup、range / sort、删除、遍历等具体操作。
- 不负责什么：不负责命令名解析，不负责 socket，不负责 AOF / RDB / 复制消息编码。
- 上层调用来源：handle_*、TTL 逻辑、AOF / RDB / 复制恢复、FULLDUMP。
- 下层依赖对象：kvs_malloc / kvs_free、内部比较函数、遍历回调。
- 对应源码位置： [include/kvs_array.h](../include/kvs_array.h#L11), [include/kvs_hash.h](../include/kvs_hash.h#L12), [include/kvs_rbtree.h](../include/kvs_rbtree.h#L16), [include/kvs_skiptable.h](../include/kvs_skiptable.h#L7), [src/storage/kvs_array.c](../src/storage/kvs_array.c#L483), [src/storage/kvs_hash.c](../src/storage/kvs_hash.c#L324), [src/storage/kvs_rbtree.c](../src/storage/kvs_rbtree.c#L596), [src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c#L343)
- 关键结构体 / 函数：kvs_array_item_t、hashnode_t、rbtree_node、Node、kvs_array_set / get / del / mod / exist / range / sort、kvs_hash_set / get / del / mod / exist / range / sort、kvs_rbtree_set / get / del / mod / exist / range / sort、kvs_skiptable_set / get / del / mod / exist / range / sort。
- 面试里应该怎么概括：这一层就是“数据结构本体”，所有上层语义最后都要落到这里。

### 6. TTL / 过期控制层
- 主要职责：把过期语义做成跨层机制。TTL 不是单独的 KV 元对象，而是直接下沉到四种结构的节点字段 expire_at_ms 里，再通过命令层、惰性删除、后台扫描、持久化、复制一起联动。
- 不负责什么：不单独存一张 TTL 索引表，不单独做时间轮，不负责 socket 交互。
- 上层调用来源：EXPIRE / TTL / PERSIST / PEXPIREAT 命令，GET / EXIST 的读路径，RDB / AOF / 复制回放，后台扫描线程。
- 下层依赖对象：ttl_target_ref_t、ttl_lookup_*、ttl_target_*、ttl_scan_once、四种数据结构节点。
- 对应源码位置： [src/core/kvs_operation.c](../src/core/kvs_operation.c#L360), [src/core/kvs_operation.c](../src/core/kvs_operation.c#L375), [src/core/kvs_operation.c](../src/core/kvs_operation.c#L421), [src/core/kvs_ttl_background.c](../src/core/kvs_ttl_background.c#L191), [include/kvs_types.h](../include/kvs_types.h#L1), [include/kvs_array.h](../include/kvs_array.h#L11), [include/kvs_hash.h](../include/kvs_hash.h#L12), [include/kvs_rbtree.h](../include/kvs_rbtree.h#L16), [include/kvs_skiptable.h](../include/kvs_skiptable.h#L7)
- 关键结构体 / 函数：ttl_target_ref_t、ttl_expire_if_needed_by_structure、ttl_apply_any_key_op、ttl_target_apply_expired_side_effects、ttl_scan_once、kvs_ttl_background_start、kvs_ttl_background_stop。
- 面试里应该怎么概括：TTL 在 KVLite 里是“跨层机制”，不是单独模块；它的关键不是定时器本身，而是和读写、持久化、复制的联动。

### 7. 内存管理层
- 主要职责：提供统一分配接口，屏蔽 pool / system / jemalloc 三种后端；当前默认走自定义内存池，网络缓冲、AOF 缓冲、复制消息、解析 token、数据结构节点基本都通过 kvs_malloc / kvs_free 走统一入口。
- 不负责什么：不负责 key / value 语义，不负责 RESP，不负责文件格式。
- 上层调用来源：网络缓冲、协议解析、存储节点、AOF / RDB / 复制、日志、队列。
- 下层依赖对象：mem_pool_t、mem_block_header_t、large_block_record_t、g_mem_manager。
- 对应源码位置： [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L43), [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L165), [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L313), [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L403), [include/kvs_memory.h](../include/kvs_memory.h#L1), [include/kvs_memory_internal.h](../include/kvs_memory_internal.h#L1)
- 关键结构体 / 函数：mem_init、mem_alloc、mem_free、mem_realloc、mem_destroy、kvs_strdup、memleak_alloc、memleak_free。
- 面试里应该怎么概括：这一层的核心是“统一分配入口 + 可切换后端 + 降碎片 / 降分配成本”。

### 8. 持久化层
- 主要职责：把内存中的状态落盘和恢复；KVLite 里是 AOF + RDB 双轨并行，AOF 负责增量语义，RDB 负责快照与冷启动。
- 不负责什么：不负责网络接入，不负责命令表路由，不负责具体存储结构选择。
- 上层调用来源：启动编排、命令执行后置钩子、FULLDUMP、AOF rewrite、恢复流程。
- 下层依赖对象：RESP 解析/编码、四种数据结构遍历、TTL 元数据、内存管理。
- 对应源码位置： [include/kvs_aof.h](../include/kvs_aof.h#L20), [src/persistence/kvs_aof.c](../src/persistence/kvs_aof.c#L38), [src/persistence/kvs_aof.c](../src/persistence/kvs_aof.c#L180), [src/persistence/kvs_aof.c](../src/persistence/kvs_aof.c#L372), [src/persistence/kvs_aof_rewrite.c](../src/persistence/kvs_aof_rewrite.c#L41), [src/persistence/kvs_aof_rewrite.c](../src/persistence/kvs_aof_rewrite.c#L657), [include/kvs_rdb.h](../include/kvs_rdb.h#L11), [src/persistence/kvs_rdb.c](../src/persistence/kvs_rdb.c#L106), [src/persistence/kvs_rdb.c](../src/persistence/kvs_rdb.c#L193)
- 关键结构体 / 函数：aof_state_t、aof_init、aof_append、aof_load、aof_fsync、aof_rewrite_background、kvs_rdb_save、kvs_rdb_load。
- 面试里应该怎么概括：持久化层不是只会“写文件”，它还要处理恢复、重写、TTL 保真和与复制的一致性。

### 9. 主从复制 / 同步层
- 主要职责：把 master 上的变化同步到 slave，包括 TCP 语义复制和 eBPF 请求流复制两条路径；TCP 更偏“语义消息”，eBPF 更偏“流量中继”。
- 不负责什么：不负责命令解析本身，不负责存储结构选择，不负责 RESP 对外响应。
- 上层调用来源：kvs_filter_protocol 的写命令后置钩子、AOF / FULLDUMP、REPLSYNC 握手、后台同步线程。
- 下层依赖对象：master_slave_proto、sync_backend、msg_queue、tx_batcher、slave / master socket。
- 对应源码位置： [include/distributed.h](../include/distributed.h#L40), [src/distributed/distributed.c](../src/distributed/distributed.c#L328), [src/distributed/distributed.c](../src/distributed/distributed.c#L611), [src/distributed/distributed.c](../src/distributed/distributed.c#L122), [src/distributed/sync_backend_tcp.c](../src/distributed/sync_backend_tcp.c#L34), [src/utils/master_slave_proto.c](../src/utils/master_slave_proto.c#L19), [src/eBPF/ebpf_syncd.c](../src/eBPF/ebpf_syncd.c#L270), [src/eBPF/ebpf_syncd.c](../src/eBPF/ebpf_syncd.c#L435), [src/eBPF/ebpf_syncd.c](../src/eBPF/ebpf_syncd.c#L482)
- 关键结构体 / 函数：distributed_config_t、slave_info_t、master_sync_command、slave_connect_master、sync_backend_init、sync_backend_broadcast_owned、ms_encode_message、ms_decode_message、normalize_and_broadcast_command。
- 面试里应该怎么概括：这一层的重点不是“把数据发过去”，而是“把一致性语义、全量 / 增量、TTL 语义一起传过去”。

### 10. 公共基础设施层
- 主要职责：提供所有层共享的通用支撑，包括配置、日志、全局访问器、RESP 编解码辅助、缓冲区、消息队列、批量发送、复制消息协议等。
- 不负责什么：不负责具体业务命令，不直接承担存储语义。
- 上层调用来源：几乎所有层，尤其是启动编排、网络前端、持久化、复制、TTL。
- 下层依赖对象：配置文件、日志写入器、缓冲区、队列、批处理器、全局状态。
- 对应源码位置： [src/core/kvs_config.c](../src/core/kvs_config.c#L20), [src/utils/kvs_log.c](../src/utils/kvs_log.c#L176), [src/utils/kvs_log_writer.c](../src/utils/kvs_log_writer.c#L164), [src/utils/msg_queue.c](../src/utils/msg_queue.c#L26), [src/utils/tx_batcher.c](../src/utils/tx_batcher.c#L35), [src/core/kvs_globals.c](../src/core/kvs_globals.c#L1), [src/utils/netbuf.c](../src/utils/netbuf.c#L10), [src/utils/master_slave_proto.c](../src/utils/master_slave_proto.c#L19)
- 关键结构体 / 函数：kvs_config_t、kvs_log_writer_t、msg_queue_t、tx_batcher、g_cfg、kvs_get_global_array / rbtree / hash / skiptable。
- 面试里应该怎么概括：这一层的价值是“把各层公共能力收口”，让业务层只关心命令语义。

## 第二部分：按层分析完整请求链路

### 1. SET 一个普通 key
1. 网络前端收到 socket 可读事件，把字节写进 recv_buf；reactor / proactor / ntyco 的差异只在 I/O 外壳，不改变后面的链路。
2. 事件驱动 / 连接管理层把当前连接的 recv_buf 交给 net_session_process_resp。
3. 协议层先用 resp_peek_command 找完整 RESP 帧，再用 resp_parser 把数组转成 tokens。
4. 命令分发层经过 lookup_command、check_arity、parse_command_argument，最终把 SET 归一到 command_context_t。
5. 存储引擎抽象层在 kvs_filter_protocol 里拿写锁，然后调用 handle_set。
6. 具体数据结构层根据 data_structure 分支到 kvs_array_set / kvs_rbtree_set / kvs_hash_set / kvs_skiptable_set。
7. 如果写成功且是 master + TCP，同步层会调用 aof_append 追加 AOF，并调用 master_sync_command 广播语义消息。
8. RESP 响应由 handle_set 生成，写回 send_buf，再由前端 send 路径刷回 socket。

这个链路里，命令非法会在协议层或命令层返回，结构未启用会在 handle_set 返回错误，网络异常会在前端关闭连接，AOF / 复制错误通常记录日志但不一定中断主请求。

### 2. GET 一个带 TTL 且可能过期的 key
1. 网络 / 连接 / 协议三层和 SET 一样，先完成帧切分、tokens 解析和命令路由。
2. 命令分发层识别 GET，先拿读锁，并把 lock_mode 传给命令处理器。
3. handle_get 不直接调底层 get，而是先走 ttl_expire_if_needed_by_structure。
4. TTL 层查到节点后，先读 expire_at_ms；如果未过期，直接返回 value；如果已过期，读锁下先返回 KVS_CMD_RETRY_WITH_WRITE_LOCK。
5. kvs_filter_protocol 收到重试信号后释放读锁，换写锁，再调用一次 handle_get。
6. 第二次进入时，TTL 层会执行 ttl_target_apply_expired_side_effects：删除节点，并在 master + TCP 且非复制回放时同步 DEL。
7. 过期后返回 NO EXIST；如果未过期，则返回 bulk string。
8. 这条链路不会走 AOF 追加，因为 GET 不是写命令；真正的过期删除由 TTL 层和持久化 / 复制共同兜住。

这条链路最关键的边界是：GET 并不等于纯读，因为 TTL 可能把它升级成“先读后写”的两阶段流程。

## 第三部分：网络接入层分析

1. 网络层在项目中的职责是接入 socket、承接事件、搬运字节、做发送回压控制。它只知道 fd、缓冲区、事件，不知道业务 key / value 的语义。
2. Reactor / io_uring / 协程分别处于三种前端实现：reactor 用 epoll，proactor 用 io_uring，ntyco 用 poll + 协程调度。它们都以 net_session_process_resp 作为统一业务入口。
3. socket、连接对象、读写事件、发送缓冲、接收缓冲分别由 conn_list / conn_extensions / conn_extension_t / ntyco_conn_ctx_t / net_session_t / netbuf_t 管理。
4. 网络层和协议层的边界在 net_session_process_resp：网络层只把字节交给它，协议层负责把字节变成 tokens，并把完整命令交给 handler。
5. 网络层不能直接操作底层数据结构，因为那会把 I/O 事件、粘包处理、命令语义、TTL / 复制 / 持久化全部耦合在一起，后续很难切换前端实现。
6. 异常处理方式是分层处理：断连 / 半包 / 写阻塞在网络层解决，RESP 错误在协议层解决，非法命令 / 参数错误在命令层解决，存储失败在 handler 返回错误。
7. 关键源码位置： [src/network/reactor.c](../src/network/reactor.c#L305), [src/network/reactor.c](../src/network/reactor.c#L393), [src/network/proactor.c](../src/network/proactor.c#L177), [src/network/proactor.c](../src/network/proactor.c#L195), [src/network/ntyco.c](../src/network/ntyco.c#L153), [src/network/ntyco.c](../src/network/ntyco.c#L160), [src/network/net_session.c](../src/network/net_session.c#L53), [src/utils/netbuf.c](../src/utils/netbuf.c#L36)

面试里这层应该怎么说：先讲“前端负责 I/O，不负责业务”，再讲 reactor / proactor / 协程三种实现都复用同一会话层。

最常见的 8 个追问：
- 为什么同时保留 reactor、proactor、ntyco 三种前端？
- 为什么会话层要单独抽出来，而不是直接写在 recv 回调里？
- 为什么 send_buf 和 recv_buf 要分别管理？
- 半包 / 粘包是在哪一层处理的？
- EAGAIN / EWOULDBLOCK 是怎么处理的？
- 为什么 REPLSYNC 要在网络层单独识别？
- 网络层为什么不能直接调用 storage 函数？
- 这一层切换前端时，哪些代码最需要保持不变？

## 第四部分：协议解析与命令分发层分析

1. 协议层负责两件事：一是把 RESP 字节流解析成 tokens，二是把 tokens 解析成命令上下文并做命令级校验。
2. 从字节流到命令对象的转换是两段式：先用 resp_peek_command 只判断一条完整命令有多少字节，再用 resp_parser 做状态机解析，避免半包直接污染语义层。
3. 半包 / 粘包 / 非法命令 / 参数错误都在这层处理：peek 返回 0 表示还不够，返回负数表示协议错误，check_arity 和 parse_command_argument 负责语义级参数合法性。
4. 命令分发器通过 command_table 把 SET / GET / DEL / EXPIRE / TTL / PERSIST / FULLDUMP 路由到不同后端；TTL 命令的 data_structure 设为 -1，表示它们不归某个具体存储结构。
5. 协议层和网络层的边界是“帧”和“字节流”，协议层和存储层的边界是“已验证命令上下文”和“具体节点操作”。
6. 这层还负责响应编码：resp_generate_simple_string_n、resp_generate_bulk_string_n、resp_generate_integer、resp_generate_array_start / item 都在这里。
7. 关键源码位置： [src/network/resp_protocol.c](../src/network/resp_protocol.c#L89), [src/network/resp_protocol.c](../src/network/resp_protocol.c#L216), [src/network/resp_protocol.c](../src/network/resp_protocol.c#L538), [src/core/kvs_operation.c](../src/core/kvs_operation.c#L500), [src/core/kvs_operation.c](../src/core/kvs_operation.c#L740), [include/resp_protocol.h](../include/resp_protocol.h#L85)

面试里这层应该怎么说：先说“先切包，再解码，再做命令级校验”，最后强调它是 AOF replay 和 eBPF 复制也复用的统一入口。

最常见的 8 个追问：
- 为什么要先 peek 再 parse？
- resp_parser 为什么要做成状态机？
- 为什么顶层只接受 RESP 数组？
- `KVS_CMD_RETRY_WITH_WRITE_LOCK` 的语义是什么？
- 为什么命令表要带 data_structure？
- 为什么 TTL 命令不绑定具体结构？
- AOF replay 为什么也走协议层？
- resp_process_command 为什么没有进入当前热路径？

## 第五部分：存储引擎抽象层分析

1. KVLite 目前没有一个独立的 runtime 引擎接口，热路径上的抽象是 command_table + command_context_t + handle_* 这一套组合。
2. 上层命令通过 command_table 屏蔽具体底层差异，同一个 SET / GET / DEL 可以映射到 array、rbtree、hash、skiptable 四种实现。
3. 为什么需要这一层：如果命令直接打到四种数据结构，协议层就会认识过多后端细节，TTL、FULLDUMP、RDB / AOF / 复制也会失去统一入口。
4. 这一层并不真正承担 TTL 检查、持久化挂钩、复制挂钩这些工作；这些逻辑要么在 kvs_filter_protocol 里挂接，要么在具体 handler / TTL helper 里完成。
5. 当前仓库里，include/kvs_operations.h 和 include/kvstore.h 里的旧操作表还在，但 src/ 里没有实际调用，这说明旧抽象是兼容层，不是当前主链路。
6. 关键源码位置： [src/core/kvs_operation.c](../src/core/kvs_operation.c#L500), [include/kvs_operations.h](../include/kvs_operations.h), [include/kvstore.h](../include/kvstore.h#L289)

这一层的价值：把“命令语义”和“底层结构实现”隔开，让同一套协议可以横跨四种结构和 TTL / 持久化 / 复制。

为什么这层是项目分层是否清晰的关键：如果它不清晰，整个系统就会退化成在网络回调里直接写存储逻辑，后面任何扩展都会非常痛苦。

最常见的 8 个追问：
- 为什么不把命令直接打到具体结构？
- 为什么 command_table 要同时存 handler 和 data_structure？
- 为什么 TTL 命令要单独处理？
- 为什么 handle_get / handle_exist 需要重试写锁？
- 旧的 kvs_operations.h 为什么还保留？
- PAPER* 为什么还走 RBTree？
- 新增一种结构时需要改哪些地方？
- 这层和 TTL / 复制 / 持久化到底谁先谁后？

## 第六部分：具体数据结构实现层分析

### 四种结构对比表

| 结构 | 定位 | 核心文件 | 适合什么 | 不太适合什么 | TTL / 持久化 / 复制协作方式 |
|---|---|---|---|---|---|
| array | table + side_index 的顺序容器 | [include/kvs_array.h](../include/kvs_array.h#L11), [src/storage/kvs_array.c](../src/storage/kvs_array.c#L483) | 小中规模点查、简单扫描、range / sort | 高并发重写、超大 keyspace | 节点内直接存 expire_at_ms；RDB / AOF / 复制遍历时直接读字段 |
| hash | 线性探测式槽表 + tombstone | [include/kvs_hash.h](../include/kvs_hash.h#L12), [src/storage/kvs_hash.c](../src/storage/kvs_hash.c#L324) | 点查、更新、删除 | 有序遍历、范围查询 | 节点内存 expire_at_ms；save / load / replay 时按槽位处理 |
| rbtree | 有序平衡树 | [include/kvs_rbtree.h](../include/kvs_rbtree.h#L16), [src/storage/kvs_rbtree.c](../src/storage/kvs_rbtree.c#L596) | range、sort、顺序遍历、较复杂的 TTL 遍历 | 纯点查极致性能 | 节点自带 expire_at_ms；RDB / AOF rewrite / FULLDUMP 特别适合遍历 |
| skiptable | 有序跳表 | [include/kvs_skiptable.h](../include/kvs_skiptable.h#L7), [src/storage/kvs_skiptable.c](../src/storage/kvs_skiptable.c#L343) | range、sort、顺序扫描、按层级查找 | 高度追求紧凑内存占用 | 节点自带 expire_at_ms；复制 / 持久化通过有序遍历协作 |

### 这一层对上暴露什么统一能力
- 对上层统一暴露 set / get / del / mod / exist / range / sort 七类能力，外加 TTL 字段和遍历语义。
- 对上层看起来是同一套命令，对下层才是四套不同的实现。

### 每种结构在系统里的定位
- array：更像“可索引的顺序容器”，带 side_index，适合简单场景和扫描场景。
- hash：更偏点查，适合大多数 key-value 随机访问。
- rbtree：适合有序遍历、范围查询和需要稳定顺序输出的场景。
- skiptable：也是有序结构，但实现形态更像多层指针跳跃，适合 range / sort 和较自然的顺序访问。

### 插入 / 查询 / 更新 / 删除路径对应代码
- array：kvs_array_set、kvs_array_get、kvs_array_del、kvs_array_mod、kvs_array_exist、kvs_array_range、kvs_array_sort。
- hash：kvs_hash_set、kvs_hash_get、kvs_hash_del、kvs_hash_mod、kvs_hash_exist、kvs_hash_range、kvs_hash_sort。
- rbtree：kvs_rbtree_set、kvs_rbtree_get、kvs_rbtree_del、kvs_rbtree_mod、kvs_rbtree_exist、kvs_rbtree_range、kvs_rbtree_sort。
- skiptable：kvs_skiptable_set、kvs_skiptable_get、kvs_skiptable_del、kvs_skiptable_mod、kvs_skiptable_exist、kvs_skiptable_range、kvs_skiptable_sort。

### 哪些结构适合点查，哪些适合范围查，哪些内存占用更低
- 点查优先：hash。
- 范围查优先：rbtree、skiptable。
- 简单扫描 / 轻量容器：array。
- 内存占用一般是空间换时间的折中，不要机械地只看算法名；array / hash 更偏槽位化，rbtree / skiptable 更偏指针和结构开销。

### 它们和 TTL / 持久化 / 复制之间如何协作
- 四种结构都把 expire_at_ms 放在节点内部，所以 TTL 不需要一张额外索引表。
- RDB 保存 / 加载时按结构遍历，保存时跳过过期项，加载时跳过已经过期的记录。
- AOF rewrite / FULLDUMP 会遍历结构并把 TTL 语义重新编码成 RESP 命令。
- 复制层在 full sync 时按结构遍历，把当前有效数据和绝对过期时间一起发出去。

### 当前做过哪些优化
- array：side_index 是最明显的优化点，避免纯线性扫描。
- TTL：GET / EXIST 读路径先读锁，过期时再升级写锁，避免所有读都直接写锁。
- TTL 后台：采用有预算的周期扫描，不是无脑全表遍历。
- 复制：TCP 用消息队列把写路径和发送路径解耦。
- eBPF：通过 normalize_and_broadcast_command 把 TTL 语义统一掉。

### 这一层的性能瓶颈在哪里
- 比较成本：key 比较、hash 计算、范围遍历都可能成为热点。
- 分配成本：value / key / 节点的频繁分配和释放会放大内存管理压力。
- 遍历成本：range / sort / TTL scan / full dump 都依赖遍历。
- 复制和持久化的二次编码：RESP 编码、ms 编码、AOF rewrite 都会额外消耗 CPU。

面试里这层应该怎么说：四种结构并存不是为了炫技，而是为了把“点查、范围查、顺序输出、TTL 语义”分别落到更合适的实现上。

这一层最常见的 12 个追问：
- 为什么 array 还要有 side_index？
- hash 的 tombstone 怎么影响性能？
- rbtree 为什么适合 range / sort？
- skiptable 为什么也是有序结构？
- 为什么节点内部都要放 expire_at_ms？
- 为什么 TTL 不做统一外部索引？
- 四种结构在 FULLDUMP / RDB / AOF 里的区别是什么？
- 哪种结构更适合大 value？
- 复制和持久化为什么要按结构遍历？
- range / sort 为什么一定要考虑过期过滤？
- 现在最明显的优化点是什么？
- 这一层最可能的瓶颈在哪里？

## 第七部分：TTL / 过期控制层分析

1. TTL 在项目里准确地说是“跨层机制”，不是单独一层。它由节点字段、命令处理、读路径惰性删除、后台扫描、持久化、复制共同组成。
2. EXPIRE / TTL / PERSIST / PEXPIREAT 的入口在 command_table 和 ttl_apply_any_key_op；它们不是某种底层结构专属命令。
3. 惰性删除发生在 GET / EXIST 路径，核心是 ttl_expire_if_needed_by_structure；轻量后台扫描发生在 kvs_ttl_background.c 的 ttl_scan_once。
4. GET / SET / DEL / 覆盖写 / 持久化恢复 / 主从复制都要联动 TTL：覆盖写会清 TTL，RDB / AOF 要保存绝对 expire_at_ms，复制要把绝对时间传下去。
5. TTL 元数据不在单独对象里，而是直接存在每种结构节点的 expire_at_ms 字段中。
6. TTL 与四种底层数据结构的协同方式是 ttl_lookup_* + ttl_target_*：先找到节点，再读 / 改 / 删 expire_at_ms。
7. TTL 与协议层 / 引擎层 / 持久化层 / 复制层的边界：协议层只接受命令；引擎层只路由到 TTL helper；持久化层负责保存绝对时间；复制层负责把绝对时间同步给 slave。
8. 如果 key 特别多且 TTL 特别密集，主要问题会是全局 rwlock 争用、扫描成本、复制回放成本，以及过期删除和同步之间的排队。

关键源码位置： [src/core/kvs_operation.c](../src/core/kvs_operation.c#L375), [src/core/kvs_operation.c](../src/core/kvs_operation.c#L421), [src/core/kvs_operation.c](../src/core/kvs_operation.c#L544), [src/core/kvs_ttl_background.c](../src/core/kvs_ttl_background.c#L191), [src/core/kvs_ttl_background.c](../src/core/kvs_ttl_background.c#L238), [include/kvs_array.h](../include/kvs_array.h#L17), [include/kvs_hash.h](../include/kvs_hash.h#L31), [include/kvs_rbtree.h](../include/kvs_rbtree.h#L25), [include/kvs_skiptable.h](../include/kvs_skiptable.h#L14)

TTL 在项目分层中的准确位置：节点字段属于数据结构层，判断与执行属于命令 / TTL 层，背景扫描属于公共运行机制，保存与同步属于持久化和复制层，所以它必须被当作跨层机制来理解。

TTL 的设计权衡：不用单独索引换来实现简单和一致性，但要接受读路径重试、背景扫描、遍历过滤和复制 / 持久化同步这些额外成本。

最常见的 12 个追问：
- 为什么 TTL 要存绝对时间而不是剩余秒数？
- 为什么 master 才主动过期，slave 不主动过期？
- 为什么 GET / EXIST 要先读锁、过期后再升级写锁？
- 为什么 TTL 不能只靠后台扫描？
- 为什么 EXPIRE 会被规范化成 PEXPIREAT 或 DEL？
- 为什么 PERSIST 需要单独同步？
- 为什么覆盖写要清 TTL？
- 为什么 RDB 要跳过已过期记录？
- 为什么 AOF rewrite 要写 PEXPIREAT？
- 为什么 TTL 不做统一外部索引？
- TTL 密集时最大的瓶颈是什么？
- TTL 语义在复制和持久化里如何保持一致？

## 第八部分：内存管理层分析

1. 当前项目有统一内存管理层，外部统一通过 kvs_malloc、kvs_free、kvs_realloc 访问；真正的后端可以是 pool、system 或 jemalloc。
2. 内存池用于所有走统一入口的对象：网络 buffer、RESP token、AOF 缓冲、复制消息、存储节点、TTL 临时对象等。换句话说，宏层面的统一入口覆盖了大部分高频分配点。
3. 但并不是所有对象都完全走池：内存管理器内部、日志写入器、部分库级实现仍然会用到 malloc / calloc / free，这些属于内部实现细节。
4. 网络层 buffer 有自己的管理逻辑，但底层仍调用统一分配器：netbuf_init / netbuf_reserve / netbuf_free 都通过 kvs_malloc / kvs_realloc / kvs_free 做容量管理。
5. 需要内存池的原因是减少频繁小对象分配的成本、降低碎片、给大并发请求提供更稳定的分配延迟。
6. 内存池和性能 / 碎片 / 分配次数的关系非常直接：池化越充分，小对象越容易复用；但实现复杂度、调试难度和大块管理成本也会上升。
7. 内存管理层和数据结构层的边界是：数据结构只关心“要多少内存”，内存管理层决定“从哪里拿、什么时候还”。
8. 内存管理和持久化 / 复制之间有间接关系：AOF / RDB / 复制消息 / 解析 token 的分配策略会直接影响整个 I/O 链路的尾延迟。

关键源码位置： [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L43), [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L165), [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L313), [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L403), [src/utils/netbuf.c](../src/utils/netbuf.c#L10), [src/utils/netbuf.c](../src/utils/netbuf.c#L36), [include/kvs_memory_internal.h](../include/kvs_memory_internal.h#L1)

内存管理层在整个系统里的角色：它不是业务层，但它决定业务层和 I/O 层的实际延迟下限。

当前实现的收益和局限：收益是统一入口、可切换后端、可复用；局限是内部仍有少量直连 malloc / free，且大块 / 小块共存让实现复杂度上升。

最常见的 8 个追问：
- 为什么要自研内存池，而不是只用 malloc？
- pool / system / jemalloc 三种后端怎么切？
- 为什么网络 buffer 也要走统一分配？
- 大对象和小对象如何区分？
- 现在哪些模块还没完全走统一入口？
- 内存池最容易出什么 bug？
- 为什么 mem_realloc 在 pool 后端不是真正原地扩容？
- 这层和复制 / 持久化有什么关系？

## 第九部分：持久化层分析

1. 持久化层负责把运行时状态落盘和恢复；KVLite 里是 AOF + RDB 双轨并行，AOF 负责增量语义，RDB 负责快照与冷启动。
2. 和引擎层的交互方式是：AOF replay 直接回到 kvs_filter_protocol，RDB save / load 则遍历四种数据结构并恢复节点字段。
3. 和 TTL 的交互非常紧：AOF rewrite 会把过期时间重新编码成 PEXPIREAT；RDB version 2 会保存绝对 expire_at_ms；load 时会跳过已经过期的记录。
4. 和复制层的交互是：AOF replay 期间要设置 sync context 避免再次回写；slave 回放后也会把回放结果写 AOF；复制 full sync 也必须保留 TTL。
5. AOF 写入、RDB 生成、恢复加载的路径分别是 aof_append / aof_fsync / aof_load、kvs_rdb_save / kvs_rdb_load、aof_rewrite_background。
6. 刷盘策略上，AOF 支持 no / everysec / always；RDB 采用临时文件 + fsync + rename 的原子替换方式。
7. 为什么要 AOF + RDB：AOF 负责更细的增量语义和恢复点，RDB 负责快照、启动速度和更直接的冷启动恢复。
8. 当前持久化层的性能瓶颈主要在 fsync、重写、RESP 解析、遍历以及 COW / 锁竞争上。

关键源码位置： [include/kvs_aof.h](../include/kvs_aof.h#L20), [src/persistence/kvs_aof.c](../src/persistence/kvs_aof.c#L38), [src/persistence/kvs_aof.c](../src/persistence/kvs_aof.c#L180), [src/persistence/kvs_aof.c](../src/persistence/kvs_aof.c#L372), [src/persistence/kvs_aof_rewrite.c](../src/persistence/kvs_aof_rewrite.c#L41), [src/persistence/kvs_aof_rewrite.c](../src/persistence/kvs_aof_rewrite.c#L657), [include/kvs_rdb.h](../include/kvs_rdb.h#L11), [src/persistence/kvs_rdb.c](../src/persistence/kvs_rdb.c#L106), [src/persistence/kvs_rdb.c](../src/persistence/kvs_rdb.c#L193)

持久化层在分层架构中的位置：它不是业务判定点，而是业务状态的外化层；它必须看懂 TTL 和结构遍历，但不能把网络和命令逻辑吞进去。

面试里如何讲 AOF / RDB 的职责边界：AOF 解决“最近发生了什么”，RDB 解决“当前长什么样”；前者重语义，后者重快照和恢复速度。

最常见的 10 个追问：
- 为什么 AOF 需要后台 fsync？
- 为什么 AOF rewrite 需要 COW 保护？
- 为什么 proactor 模式下 COW 会被关闭？
- AOF load 为什么要走 RESP parser？
- RDB 为什么要用临时文件 + rename？
- 为什么 RDB version 2 要保存绝对 expire_at_ms？
- 为什么 load 时要跳过已过期记录？
- 为什么持久化层要和复制层互相配合？
- FULLDUMP 和 RDB 的区别是什么？
- AOF 和 RDB 各自的性能瓶颈是什么？

## 第十部分：主从复制 / 同步层分析

1. 复制层负责把 master 上的变化同步到 slave，并且同时支持 TCP 语义复制和 eBPF 请求流复制两条路径。
2. 复制层从命令执行后置事件接收变更：kvs_filter_protocol 在 master + TCP 模式下直接调用 master_sync_command；AOF / FULLDUMP / slave 回放也会和复制层产生交互。
3. TCP 路径是语义消息同步：master_sync_command 先把写操作编码成 master_slave_proto 消息，再扔进 sync_backend_broadcast_owned，由后台线程异步发送。
4. 全量同步和增量同步在 TCP 路径里分开处理：master_sync_server 接连接后，slave_sync_thread 先遍历本地结构发全量，再依赖后续的增量消息保持同步。
5. TCP / eBPF 双链路的位置不同：TCP 是明确的主从语义复制，eBPF 是抓 master 进入流量并转发给 slave 的用户态中继。
6. TTL、删除、覆盖、恢复等语义在复制里必须对齐：master 是 TTL 的唯一裁判，slave 不主动过期，只回放 master 给出的 DEL / PERSIST / PEXPIREAT / 覆盖写动作。
7. 为什么批处理能降低系统调用：TCP 复制用消息队列把写路径和发送路径解耦，eBPF 侧也可以通过批量发送降低小包开销。
8. 复制层和持久化层的关系非常近：slave 回放后会写 AOF，AOF rewrite 也要保留 TTL 和语义。
9. 复制层和网络层的关系是两套 socket 角色：业务连接和复制连接都复用 RESP / 前端框架，但 REPLSYNC 会把连接切入复制上下文。
10. 复制层的边界问题主要是语义一致性：特别是 eBPF 路径，它抓的是请求流，不是提交后的状态日志，所以 TTL 归一化尤其重要。

关键源码位置： [include/distributed.h](../include/distributed.h#L40), [src/distributed/distributed.c](../src/distributed/distributed.c#L328), [src/distributed/distributed.c](../src/distributed/distributed.c#L611), [src/distributed/distributed.c](../src/distributed/distributed.c#L122), [src/distributed/sync_backend_tcp.c](../src/distributed/sync_backend_tcp.c#L34), [src/utils/master_slave_proto.c](../src/utils/master_slave_proto.c#L19), [src/eBPF/ebpf_syncd.c](../src/eBPF/ebpf_syncd.c#L270), [src/eBPF/ebpf_syncd.c](../src/eBPF/ebpf_syncd.c#L435), [src/eBPF/ebpf_syncd.c](../src/eBPF/ebpf_syncd.c#L482)

复制层在系统分层中的准确位置：它挂在命令执行后和恢复回放前后，是“语义输出层”，不是业务判定层。

复制层最能体现工程化能力的点：全量 / 增量分离、TCP 消息队列解耦、TTL 语义归一化、以及 REPLSYNC 连接切换。

最常见的 12 个追问：
- 为什么 master 是 TTL 的唯一裁判？
- 为什么 slave 不主动过期？
- TCP 复制为什么不用直接同步原始命令文本？
- master_sync_command 做了什么格式化工作？
- 为什么要单独有 sync_backend 队列？
- 全量同步和增量同步怎么衔接？
- REPLSYNC 的作用是什么？
- eBPF 路径为什么要做 EXPIRE 归一化？
- 复制层如何处理 DEL / PERSIST / 覆盖写？
- slave 回放时为什么要设置 sync context？
- 复制和 AOF 的关系是什么？
- 复制层最大的边界风险是什么？

## 第十一部分：公共基础设施层分析

1. 配置层：kvs_config.c / kvs_config.h 负责把端口、角色、AOF、RDB、网络模式、同步模式、内存后端和日志配置统一起来；main 里先 load config，再 apply 命令行覆盖，最后按最终结果决定启动路径。
2. 日志层：kvs_log.c / kvs_log_writer.c 负责统一日志输出，支持同步和异步两种写入方式；异步 writer 有队列、有降级、有 dropped 统计，这在面试里很加分。
3. 缓冲区层：netbuf.c / netbuf.h 负责收发 buffer 的初始化、扩容、前移裁剪和释放，是网络层和协议层之间最常见的支撑模块。
4. 队列层：msg_queue.c / msg_queue.h 负责复制线程之间的消息缓冲，是 TCP replication 解耦的关键。
5. 批处理层：tx_batcher.c / tx_batcher.h 负责把多次 append 合并成一次 flush，适合 eBPF / 复制发送场景降低 syscall 次数。
6. 全局访问层：kvs_globals.c / kvs_globals.h 把全局配置和四种结构的获取函数收口，避免各处直接散落 extern。
7. 复制消息协议层：master_slave_proto.c / master_slave_proto.h 是 TCP 复制语义消息的二进制封装，这更像基础设施而不是业务逻辑。
8. 哪些属于真正的公共基础设施：config、log、netbuf、msg_queue、tx_batcher、globals、memory。
9. 哪些只是某层私有工具：例如 REPLSYNC 识别、AOF rewrite 的 COW 保护、TTL 的 ttl_lookup_* 更偏某层内部工具，不应被当成全局基础设施。
10. 最容易被忽略但面试很加分的点：配置的最终覆盖顺序、日志异步降级、队列容量设计、batcher 的 flush 时机、以及 globals 只是访问器而不是业务接口。

最值得复习的文件： [src/core/kvs_config.c](../src/core/kvs_config.c#L20), [src/utils/kvs_log.c](../src/utils/kvs_log.c#L176), [src/utils/kvs_log_writer.c](../src/utils/kvs_log_writer.c#L164), [src/utils/netbuf.c](../src/utils/netbuf.c#L10), [src/utils/msg_queue.c](../src/utils/msg_queue.c#L26), [src/utils/tx_batcher.c](../src/utils/tx_batcher.c#L35), [src/core/kvs_globals.c](../src/core/kvs_globals.c#L1), [src/utils/master_slave_proto.c](../src/utils/master_slave_proto.c#L19)

## 第十二部分：从“分层架构”角度总结 KVLite

1. KVLite 的分层总体上是清晰的：网络前端、会话缓冲、协议解析、命令分发、数据结构、TTL、持久化、复制和基础设施都能找到对应落点。
2. 做得好的层是：网络前端和 net_session 的分离、RESP 解析的统一、四种数据结构的节点字段统一、TTL 的绝对时间统一、AOF / RDB / 复制对 TTL 的统一保留。
3. 边界还可以更清楚的地方是：kvs_operation.c 里同时混着命令表、TTL、FULLDUMP、持久化和复制后置逻辑，属于当前最明显的“策略混合点”。
4. 耦合较重的地方还有：REPLSYNC 在三个网络前端里重复处理、kvstore.c 既是启动入口又承接大量编排逻辑、旧的 kvs_operations.h / kvstore.h 兼容层还在。
5. 如果为了面试表达，建议把项目讲成“一个有清晰前后端分离、命令统一分发、TTL 跨层联动、持久化和复制彼此对齐的分层存储系统”。
6. 如果为了线上演进，下一步最值得做的是：把命令策略从 kvs_operation.c 再抽一层、把复制语义归一化再抽一层、把 TTL 背景扫描和读路径重试收得更干净。

### 适合一面口述的 3 分钟项目分层讲解稿
KVLite 不是一个功能堆在一起的 KV 项目，我在复盘时会把它理解成一条很清楚的链路：启动时先做配置、日志、内存、RDB、AOF 和复制初始化，然后按配置选择 reactor、proactor 或 ntyco 作为网络前端。网络前端只负责把 socket 里的字节搬到会话缓冲里，真正的协议解析统一走 net_session，再通过 RESP 解析成 tokens。

解析完以后，命令先经过 kvs_filter_protocol 做命令校验、权限检查和参数归一化，再由 command_table 路由到 handle_set、handle_get 这些处理器。处理器再往下才会落到 array、hash、rbtree、skiptable 四种具体数据结构。TTL 不是一个独立的孤岛，而是直接下沉到节点里的 expire_at_ms 字段，GET / EXIST 会先做惰性删除，后台还有一个轻量扫描线程补充清理。

持久化上，KVLite 同时保留 AOF 和 RDB：AOF 负责增量语义和回放，RDB 负责快照和快速恢复，而且两者都保留绝对 TTL。复制上，TCP 路径走语义消息广播，eBPF 路径走请求流中继，但两条路最后都要把 TTL 语义对齐。整体来说，这个项目的分层是清楚的，最需要继续抽象的是 kvs_operation.c 里混在一起的策略逻辑。

## 第十三部分：代码回看索引（按层次复习）

### 第一轮：只看层次骨架

| 文件 / 符号 | 所属层 | 作用 | 为什么重要 | 适合回答哪些面试题 |
|---|---|---|---|---|
| [src/core/kvstore.c](../src/core/kvstore.c#L250) 的 main | 启动编排 | 串起配置、内存、持久化、复制、TTL、网络 | 这是整套系统的真实入口 | 为什么先 load RDB 再 load AOF；网络前端如何选 |
| [src/network/net_session.c](../src/network/net_session.c#L53) 的 net_session_process_resp | 网络 / 协议交界 | 把缓冲区命令变成 handler 输入 | 看懂它就看懂半包 / 粘包处理 | 为什么要先 peek 再 parse |
| [src/network/resp_protocol.c](../src/network/resp_protocol.c#L89) 的 resp_peek_command | 协议层 | 判断完整 RESP 帧 | 切包边界的关键 | 半包怎么处理 |
| [src/core/kvs_operation.c](../src/core/kvs_operation.c#L500) 的 command_table | 命令分发 | 命令名到处理器和结构映射 | 决定整个命令路由 | 为什么同一命令能映射到四种结构 |
| [src/core/kvs_operation.c](../src/core/kvs_operation.c#L785) 的 handle_set | 存储抽象 / 数据结构层 | 统一写入口 | 连接命令和具体结构 | 新增结构要改哪里 |
| [src/core/kvs_operation.c](../src/core/kvs_operation.c#L375) 的 ttl_expire_if_needed_by_structure | TTL 层 | 读路径过期判定和删除 | GET / EXIST 的关键逻辑 | 为什么 GET 可能升级成写 |
| [src/persistence/kvs_aof.c](../src/persistence/kvs_aof.c#L38) 的 aof_init | 持久化层 | 初始化 AOF 状态 | 了解 AOF 全链路入口 | AOF 为什么有后台线程 |
| [src/persistence/kvs_rdb.c](../src/persistence/kvs_rdb.c#L106) 的 kvs_rdb_save | 持久化层 | RDB 快照写出 | TTL 保真和快照格式都在这里 | RDB 为什么跳过过期项 |
| [src/distributed/distributed.c](../src/distributed/distributed.c#L328) 的 distributed_init | 复制层 | 初始化 master / slave 角色 | 决定复制链路怎么起 | 主从模式如何拆分 |
| [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L43) 的 mem_init | 内存层 | 选择内存后端并初始化池 | 所有高频分配的根 | 为什么需要统一内存层 |

### 第二轮：看关键调用链

| 文件 / 符号 | 所属层 | 作用 | 为什么重要 | 适合回答哪些面试题 |
|---|---|---|---|---|
| [src/network/reactor.c](../src/network/reactor.c#L305) 的 recv_cb / send_cb | 网络接入 / 事件驱动 | epoll 读写事件入口 | 真实的 socket 驱动路径 | epoll 如何驱动请求 |
| [src/network/proactor.c](../src/network/proactor.c#L195) 的 set_event_recv / set_event_send | 网络接入 / 事件驱动 | io_uring 投递收发 | proactor 的核心 | 为什么 io_uring 需要 in_flight |
| [src/network/ntyco.c](../src/network/ntyco.c#L160) 的 ntyco_conn_routine | 网络接入 / 事件驱动 | 协程内收发循环 | 线程模型不同但后端一致 | 协程前端怎么复用会话层 |
| [src/core/kvs_operation.c](../src/core/kvs_operation.c#L831) 的 handle_get | TTL / 数据结构 | 读路径的过期处理入口 | 这是 TTL 最容易被问的地方 | GET 为什么要重试写锁 |
| [src/core/kvs_operation.c](../src/core/kvs_operation.c#L972) 的 handle_expire | TTL | TTL 命令统一入口 | EXPIRE / PEXPIREAT 的行为收敛 | 为什么要规范化 TTL |
| [src/persistence/kvs_aof.c](../src/persistence/kvs_aof.c#L372) 的 aof_load | 持久化 | AOF 回放 | replay 要复用协议层和命令层 | 为什么回放要走 RESP |
| [src/distributed/distributed.c](../src/distributed/distributed.c#L611) 的 master_sync_command | 复制层 | 语义消息编码与广播 | master 到 slave 的关键中枢 | 为什么不用原始文本复制 |
| [src/distributed/distributed.c](../src/distributed/distributed.c#L328) 的 slave_connect_master | 复制层 | slave 接收和回放 | slave 语义一致性核心 | slave 怎么应用 TTL |
| [src/core/kvs_ttl_background.c](../src/core/kvs_ttl_background.c#L191) 的 ttl_scan_once | TTL / 后台任务 | 周期扫描过期键 | 补足惰性删除 | 为什么还需要后台扫描 |
| [src/storage/kvs_array.c](../src/storage/kvs_array.c#L756) 的 kvs_array_sort | 数据结构层 | 排序输出 | range / sort 类题目常问 | 为什么有序结构更适合 sort |

### 第三轮：看优化点与边界

| 文件 / 符号 | 所属层 | 作用 | 为什么重要 | 适合回答哪些面试题 |
|---|---|---|---|---|
| [src/core/kvs_operation.c](../src/core/kvs_operation.c#L375) 的 ttl_expire_if_needed_by_structure | TTL | 读锁 / 写锁两阶段过期删除 | 这是当前最典型的边界设计 | GET 为什么不是纯读 |
| [src/network/resp_protocol.c](../src/network/resp_protocol.c#L89) 的 resp_peek_command | 协议层 | 无状态帧探测 | 半包 / 粘包的第一道闸门 | 为什么要先 peek |
| [src/utils/netbuf.c](../src/utils/netbuf.c#L36) 的 netbuf_reserve | 基础设施 | buffer 扩容 | 网络层尾延迟和内存安全的基础 | 为什么 buffer 不能固定大小 |
| [src/utils/msg_queue.c](../src/utils/msg_queue.c#L72) 的 msg_queue_push / pop | 复制基础设施 | 生产者 / 消费者解耦 | TCP 复制提速关键 | 为什么复制要异步 |
| [src/persistence/kvs_aof_rewrite.c](../src/persistence/kvs_aof_rewrite.c#L657) 的 aof_rewrite_background | 持久化 | AOF 重写 | COW、rewrite、TTL 写回都在这里 | AOF 重写怎么保 TTL |
| [src/persistence/kvs_rdb.c](../src/persistence/kvs_rdb.c#L320) 的 rdb_save_array / rdb_load_array | 持久化 | TTL 绝对时间保存与恢复 | 说明 RDB version 2 的语义 | 为什么 RDB 要存绝对时间 |
| [src/distributed/sync_backend_tcp.c](../src/distributed/sync_backend_tcp.c#L34) 的 tcp_backend_worker | 复制层 | 后台广播线程 | 复制和写路径解耦的关键 | 为什么复制要异步 |
| [src/eBPF/ebpf_syncd.c](../src/eBPF/ebpf_syncd.c#L435) 的 normalize_and_broadcast_command | 复制层 | eBPF TTL 语义归一化 | 请求流复制最容易失真 | EXPIRE 为什么要转 PEXPIREAT |
| [src/storage/kvs_mempool.c](../src/storage/kvs_mempool.c#L165) 的 mem_alloc | 内存层 | 统一分配入口 | 池化 / 系统 / jemalloc 切换点 | 为什么需要统一分配接口 |
| [src/core/kvs_operation.c](../src/core/kvs_operation.c#L1177) 的 handle_fulldump | 命令层 / 持久化辅助 | 生成全量 RESP 导出 | 命令层和持久化层的边界题 | FULLDUMP 为什么只允许 master |

### 第一轮怎么看
- 先看 kvstore.c 的 main，搞清楚启动顺序。
- 再看 network / resp_protocol / net_session，把字节流如何进到 handler 弄明白。
- 然后看 kvs_operation.c 的 command_table 和 handle_get / handle_set / TTL 函数。
- 最后再看四种数据结构和持久化 / 复制 / 内存支撑层。

### 第二轮怎么看
- 把 main -> network -> net_session -> RESP -> kvs_filter_protocol -> handle_* -> storage 这条链走通。
- 把 handle_get -> TTL retry -> delete side effect 这条链抠清楚。
- 把 aof_load -> parse -> kvs_filter_protocol、master_sync_command -> ms_encode_message -> sync_backend_broadcast_owned、slave_connect_master -> ms_decode_message 这几条链都走一遍。

### 第三轮怎么抠
- 重点抠 ttl_expire_if_needed_by_structure、resp_peek_command、netbuf_reserve、msg_queue_push / pop、aof_rewrite_background、kvs_rdb_save / load、master_sync_command、normalize_and_broadcast_command。
- 这些地方最适合被面试官追问，也最适合你手写画图解释边界。
