# 第八章 AOF 与 RDB 持久化：内存 KV 的重启恢复与数据安全

## 8.1 从"内存能存"到"重启后还能恢复"：为什么需要持久化

前面六章花了很大力气，把 KVLite 从一条 SET 请求的简单链路，逐步演进到了支持 RESP 解析、command_table 分发、四种数据结构存储、TTL 全链路覆盖的 KV 系统。

但不管前面的能力做得多完整，有一个问题始终悬在头上：**所有数据都在进程堆内存里。** 进程退出、崩溃、机器重启——数据全部消失。

最开始数据能写进内存已经让我觉得系统跑通了。testcase 跑完 PASS，benchmark QPS 数据也好看。但有一次我手动停了服务再重启，发现之前压测写入的几十万条数据全没了——那一刻才真正意识到，内存 KV 还缺一个关键能力：**数据怎么回来**。

如果 KVLite 只做纯缓存（cache），丢失也许还能接受——缓存本来就是"丢了可以重建"的数据。但如果要走向一个更完整的 KV 存储系统，就必须有重启恢复能力。持久化的目标不是"永不丢数据"，而是在性能、可靠性和实现复杂度之间做取舍。

KVLite 做了两种持久化：AOF 和 RDB。它们解决的问题不同、设计思路不同、恢复代价不同。这一章就来讲它们各自是怎么做、怎么配合、以及做的时候踩了哪些坑。

## 8.2 AOF 和 RDB 解决的是两个不同问题

两种持久化对应两种完全不同的"恢复"思路。

**AOF（Append Only File）** 是写命令日志。每一条会改变数据状态的命令，在执行成功后，被追加到 AOF 文件末尾。恢复时，把 AOF 文件里的命令从头到尾重放一遍，内存状态就回到了最后一次 fsync 的时刻。

- 优点：数据更完整，每条写命令都被记录了，两次 fsync 之间丢失的数据量很小。
- 缺点：文件会不断膨胀（同一个 key 被 SET 了一百次，AOF 里就有一百条 SET），恢复时需要逐条执行命令，速度较慢。

**RDB（Redis Database snapshot）** 是某个时刻的内存快照。直接把当前所有内存数据（key、value、TTL）序列化到一个紧凑的二进制文件中。恢复时，加载这个文件，重建所有 key-value 和过期时间。

- 优点：文件紧凑、恢复快——直接加载序列化数据，不需要逐条执行命令。
- 缺点：两次快照之间的数据可能丢失。如果每 5 分钟做一次快照，上一次快照之后 5 分钟内写入的数据在崩溃时会丢失。

两者不是谁替代谁，而是两种不同的取舍。KVLite 同时做了 AOF 和 RDB，不是为了"功能齐全"，而是为了亲自理解日志型恢复和快照型恢复在设计、实现、维护上的差异。

## 8.3 AOF append：为什么写命令成功后才追加日志

AOF 记录写命令，这个"记录"的时机非常关键。

正确的做法是：**handler 执行成功之后才追加 AOF。** 如果 handler 返回错误（比如参数不对、key 不存在），这条命令不应该进入 AOF——因为恢复重放时它也不会生效，写进去只会浪费空间和造成困惑。

在 `kvs_filter_protocol` 中（`kvstore.c:141-155`），AOF 追加的逻辑紧跟在 `cmd->handler(&ctx, response)` 返回之后：

```text
ret = cmd->handler(&ctx, response)  → 命令执行

if (ret >= 0 && cmd->write_command) → 执行成功且是写命令
    if (!aof_is_loading())           → 正在加载 AOF 时不追加
        aof_append(count, tokens, token_lens)
```

这里有三个判断条件，每个都有明确的意义：

**`ret >= 0`**：handler 返回非负数才表示成功。失败的命令不记录。

**`cmd->write_command`**：来自第五章 `command_table` 的元信息字段。只有写命令才需要持久化——GET 读操作不需要进 AOF。这个判断正是 command_table 作为"横切能力挂载点"的典型应用：AOF 不需要知道具体是 SET 还是 EXPIRE，只需要知道 `write_command == 1`。

**`!aof_is_loading()`**：正在从 AOF 文件重放命令恢复数据时，不能再把重放的命令追加回 AOF 文件——否则 AOF 会无限自增。`aof_state.loading` 标志在 `aof_load` 期间设为 1，`aof_append` 检测到后直接 return。

EXPIRE 命令在 AOF 中有特殊处理：`EXPIRE key 10` 会被转换为 `PEXPIREAT key <绝对毫秒>` 再写入 AOF。这保证了无论 AOF 在什么时刻被重放，过期时间语义都是正确的（第七章详细讨论过）。

另外，二进制安全在 AOF 路径上也得到了支持。`aof_append` 接收 `arg_lens` 参数——当 `arg_lens` 非 NULL 时，使用传入的长度而非 `strlen` 计算参数大小。这样即使 value 中包含 `\0` 字节，AOF 中写入的 RESP 格式长度也是正确的。

## 8.4 fsync 策略：write 成功不等于真正落盘

AOF 追加调用 `fwrite` 之后，数据只是进入了 C 库的缓冲区（用户态）。即使调了 `fflush`，数据也只是进入了内核的页缓存（page cache），还没有真正写到磁盘上。

如果这时候断电，内核页缓存中的数据会丢失——AOF 文件里可能缺少最后几条命令。

`fsync`（或 `fdatasync`）的作用就是强制将内核页缓存中的数据刷到物理磁盘上。但 `fsync` 本身是一个昂贵的系统调用——每次调用都涉及磁盘 IO 等待。所以"每条命令都 fsync"虽然最安全，但对 QPS 的影响也最大。

KVLite 支持三种 fsync 策略（定义在 `include/kvs_aof.h:7-9`）：

| 策略 | 行为 | 可靠性 | 性能 |
|------|------|--------|------|
| `AOF_FSYNC_ALWAYS` | 每条写命令后立即 fsync | 最高，几乎不丢数据 | 最慢 |
| `AOF_FSYNC_EVERYSEC` | 后台线程每秒 fsync 一次 | 最多丢 1 秒数据 | 均衡 |
| `AOF_FSYNC_NO` | 不主动 fsync，交给操作系统 | 丢失风险最大 | 最快 |

在当前实现中（`kvs_aof.c:293-327`），写命令追加到 AOF 缓冲区后：

- `ALWAYS` 模式：标记 `should_fsync = 1`，在锁外调用 `aof_fsync(1)` 强制刷盘。
- `EVERYSEC` 模式：如果缓冲区过半（≥ `buf_size/2`），通过条件变量唤醒后台 fsync 线程；后台线程每秒检查一次，发现有待刷数据就刷。
- `NO` 模式：依赖于操作系统的脏页回写机制，不主动触发。

对于超大命令（预估长度超过缓冲区一半），当前实现做了**直写优化**——跳过缓冲区，直接 `fprintf` + `fwrite` 到文件，然后 `fflush`，`ALWAYS` 模式下再 `fsync`。这避免了超大命令撑爆缓冲区。

需要诚实说明的是，这个 fsync 策略已经具备了基本的三档可配能力，但如果要严格对标生产级，还有一些可以增强的细节：比如 `EVERYSEC` 模式下的 fsync 是由后台线程的 1 秒定时器驱动的，而非独立的周期性定时器，需要等到有缓冲区写入时才会被信号唤醒——这意味着如果 1 秒内没有任何新写命令，待刷数据可能会延迟。

## 8.5 AOF 文件为什么会膨胀，为什么需要 rewrite

AOF 记录的是写命令历史。但"历史"和"当前状态"之间差距很大。

假设对一个 key 做了如下操作：

```text
SET name alice
SET name bob
SET name charlie
DEL name
SET name dave
```

AOF 中记录了 5 条命令，但当前内存状态只有一条：`name = "dave"`。前 4 条命令对恢复没有任何贡献——重放它们只是浪费时间。

当 KVLite 运行数小时或数天后，AOF 文件可能变得非常大。恢复时需要逐条重放全部历史命令，耗时很长。而实际上大部分命令对"当前内存应该是什么样"已经没有意义了。

AOF rewrite 解决的就是这个问题。它的本质不是"压缩文本"或"去重命令"，而是：**根据当前内存状态，重新生成一份最小等价的命令序列。**

rewrite 的过程是：

1. 遍历四种数据结构中所有活跃的 key-value。
2. 对每个 key，生成一条 `SET key value`（或等价命令，根据数据结构类型决定前缀）。
3. 如果 key 有 TTL（`expire_at_ms >= 0`），追加一条 `PEXPIREAT key <绝对毫秒>`。
4. 已删除或已过期的 key 不写入新文件。

这样生成的新 AOF 文件，只包含"让内存到达当前状态所需的最少命令"——每个现存 key 一条 SET + 可选的 PEXPIREAT。这就是 rewrite 的价值：**把历史日志压缩为当前状态的等价表达。**

rewrite 的触发条件是（`kvs_aof.c:304-311`）：

```text
当前文件大小 >= AOF_REWRITE_MIN_SIZE（28MB）
且增长量 >= AOF_REWRITE_MIN_GROWTH（64MB）
且增长比例 >= AOF_REWRITE_PERCENTAGE（50%）
且没有正在进行的 rewrite
```

## 8.6 AOF rewrite 的文件安全：临时文件、rename 与 fork/COW

rewrite 涉及三个需要小心处理的工程问题：文件写入安全性、rewrite 期间的并发写入、以及内存快照的一致性。

### 临时文件 + atomic rename

rewrite 不能直接覆盖原 AOF 文件。如果写到一半服务崩溃或磁盘满了，原 AOF 文件损坏，数据就彻底丢了。

正确的做法是（`kvs_aof_rewrite.c:90-92`）：

```text
写入到临时文件（<filename>.tmp）
写完、fsync 确保落盘
rename 临时文件 → 原文件（原子替换）
```

在同一文件系统内，`rename` 是一个原子操作——要么完全成功（旧文件被替换为新文件），要么完全失败（临时文件还在）。不会出现"换了一半"的中间态。当前实现中，`atomic_replace_file` 还会先把原文件 rename 为一个备份文件（`<filename>.bak`），再 rename 临时文件——这样即使第二步失败，原文件也不丢。

### rewrite 期间的并发写入：rewrite buffer

rewrite 遍历当前内存数据生成新文件的过程中，主线程可能还在处理新的写命令。这些新命令不能丢失——否则新 AOF 文件不包含 rewrite 期间发生的写入。

KVLite 的解决方案是 rewrite buffer（`aof_state.rewrite_buf`，定义在 `kvs_aof_internal.h:35-38`）：

```text
子进程（或线程）遍历内存生成临时文件
主线程继续处理写命令 → aof_append 检测到 rewrite_in_progress
  → 如果 COW 保护就绪：写入 AOF buffer + 写入 rewrite buffer
  → 如果没有 COW：只写入 rewrite buffer
子进程完成后，把 rewrite buffer 中的追加命令拼接到临时文件末尾
fsync → rename
```

rewrite buffer 保证了 rewrite 期间没有数据丢失。

### fork + COW：一次真实的 coredump 复盘

AOF rewrite 需要遍历当前内存状态。如果直接在主线程中做，整个服务会在 rewrite 期间被阻塞。Redis 的做法是 fork 一个子进程，利用操作系统的 COW（写时复制）机制——子进程拥有父进程地址空间的只读副本，rewrite 遍历这份"冻结"的快照，主线程继续服务写请求。

KVLite 也尝试了这条路径。但这中间踩了一个很深的坑。

commit `643a04f`（2025-09-25）的 message 写得很直白：**"参考 redis 使用 fork 和 COW 处理 aof 重写时 父线程和子线程访问同一个全局数据结构的冲突问题 仍然会出现 core dump 需解决"**。随后 commit `feb2935`（2025-09-26）标记"已解决 coredump"。

这个坑的本质是：**多线程程序的 fork 是一个危险操作。**

`fork` 只复制调用 `fork` 的那个线程。如果父进程有多个线程（比如网络 IO 线程、后台 fsync 线程、TTL 后台扫描线程），fork 之后子进程只有 fork 调用者这一个线程。其他线程持有的锁状态会被"冻结"在子进程的地址空间中——锁可能处于"已被某个不存在的线程持有"的状态，子进程里任何尝试获取这把锁的操作都会死锁。

KVLite 遇到的 coredump，推测原因链条是：fork 子进程时，某个后台线程正持有 `g_kv_data_rwlock` 或者 mempool 的内部锁 → 子进程中该锁永远无法释放 → 子进程的 rewrite 遍历尝试获取同一把锁 → 死锁或非法内存访问 → coredump。

解决策略包括（`kvs_aof_rewrite.c:41-80`）：

- fork 之前通过 `pthread_rwlock_wrlock` 获取全局写锁，阻塞所有新写操作，确保所有后台线程处于安全状态。
- 子进程 fork 后**立刻解锁**（子进程有自己独立的锁副本，解锁不影响父进程）。
- 父进程 fork 后也解锁，恢复正常的读写服务。
- 内存池引入互斥锁保护（commit `d71eaf6`），防止子进程访问父进程正在操作的内存池。

此外，Proactor（io_uring）模式下不使用 fork，而是使用 pthread 在后台线程中做 rewrite——因为 io_uring 和 fork 的交互更加复杂。这体现了"不同网络模型对系统能力的约束不同"。

这段经历让我深刻理解了 Redis 文档里提到的"fork 的代价"到底是什么——不是 fork 本身的系统调用开销，而是 COW 页复制对内存的压力，以及多线程环境下 fork 的安全性问题。

## 8.7 RDB snapshot：为什么快照适合快速恢复

相比 AOF 需要逐条重放命令，RDB 的恢复思路完全不同：直接把序列化的 key-value 数据加载到内存中。

当前 RDB 文件的格式（`kvs_rdb.c:432-466`）：

```text
[RDB_MAGIC: 6 bytes]     // "KVLITE" 魔数
[version: 1 byte]         // RDB_VERSION
[timestamp: 8 bytes]      // 快照时间戳
[type=0x01: 1 byte]       // ARRAY 段
  [count: 4 bytes]
  [key_len:4][key:N][value_len:4][value:N][expire_at_ms:8] x count
  ...
[type=0x02: 1 byte]       // RBTREE 段
  ...
[type=0xFF: 1 byte]       // EOF
```

每个数据结构的记录格式是一致的：key 长度 + key 内容 + value 长度 + value 内容 + 绝对过期时间。长度使用 `uint32_t` 保存，过期时间使用 `int64_t`。

保存时：
- 跳过已过期的 key（`kv_is_expired(item->expire_at_ms, now_ms)`）
- 使用 `key_len`/`value_len` 而非 `strlen`，binary-safe
- 写入临时文件 + fsync + rename 原子替换

加载时：
- 读魔数和版本号校验文件有效性
- 按类型标记分发到对应的加载函数
- 对每条记录检查 `expire_at_ms`：如果已经过期，跳过不恢复；如果未过期，恢复到对应数据结构
- 恢复时使用 `_len` 版本的存储接口（`kvs_array_set_len` 等）

RDB 相比 AOF，最大的优势是**恢复速度快**——不需要逐条执行命令，而是批量反序列化。但它的劣势是**粒度粗**——只能恢复到上一次快照的时刻。好在 KVLite 的启动恢复是先加载 RDB，再重放 AOF（见 8.9 节），这弥补了 RDB 的粒度问题。

需要诚实指出的是，当前 RDB 实现没有 checksum。文件损坏时只能靠魔数校验和 EOF 标记做基本判断，缺少 CRC 或 SHA 级别的完整性校验。对于训练型项目来说这已经足够证明"快照持久化"的原理，但生产级系统应当加上 checksum。

## 8.8 启动恢复：AOF 和 RDB 应该怎么配合

KVLite 的启动恢复流程（`kvstore.c:347-369`）是：

```text
init_kvengine()          → 初始化四种空数据结构
kvs_rdb_load(filename)   → 先加载 RDB 快照（如果文件存在）
aof_init(filename, mode) → 初始化 AOF（打开文件、启动后台线程）
aof_load(filename)       → 再重放 AOF 日志（覆盖/补全 RDB 之后的数据）
```

这个顺序的设计逻辑是：

**RDB 先加载**：如果存在 RDB 文件，先把快照时刻的数据恢复进去。这是"基础数据"。

**AOF 后重放**：然后重放 RDB 快照之后产生的 AOF 命令。AOF 中可能有"这条 key 在 RDB 快照后被修改了"的 SET 命令，也可能有 EXPIRE/PEXPIREAT 等 TTL 变更。重放 AOF 之后，内存状态就追到了最新的 fsync 时刻。

AOF 加载期间做了几个重要保护：

- `aof_state.loading = 1`：防止重放产生的写命令再次追加回 AOF。
- `set_sync_context(2)`：防止 slave 端的 `ROLE_SLAVE + write_command` 检查拦截重放命令。
- 使用和网络层同一套 `resp_peek_command` + `resp_parser` 解析：AOF 文件格式就是 RESP 命令序列，不需要另外的解析器。
- 半包处理：AOF 文件可能因为写入中断而末尾有不完整的命令——`resp_peek_command` 返回 0 时等到 EOF 后丢弃残余。

如果 RDB 和 AOF 都不存在（首次启动），数据结构保持为空。这是正常的冷启动状态。

另外，shutdown 时（`kvstore.c:427-432`），KVLite 会做最后一次 AOF fsync 和 RDB 快照：

```text
aof_fsync(1);           // 确保所有缓冲区数据刷盘
kvs_rdb_save(filename); // 保存最新快照
```

这保证了正常关闭后的下一次启动，数据是最新的。

## 8.9 持久化如何和命令层、存储层、TTL 协同

持久化不是独立的文件 IO 模块，它是命令层、存储层、TTL 和文件系统的交界面。

**命令层提供**：`command_info.write_command` 判断哪些命令需要进 AOF；tokens + token_lens 提供 binary-safe 的命令参数；`kvs_filter_protocol` 作为 AOF 重放的统一入口。

**存储层提供**：四种数据结构的遍历能力（用于 RDB 保存和 AOF rewrite 的数据导出）；`_len` 版本的存储接口（binary-safe 的 key/value 长度）。

**TTL 提供**：`expire_at_ms` 字段随数据一起保存和恢复；EXPIRE → PEXPIREAT 转换保证 AOF 和复制的语义稳定；已过期 key 在 RDB 保存和 AOF rewrite 时被跳过。

这个协同关系说明：**持久化的可靠性，取决于它上面每一层的信息是否被完整传递。** 如果存储层不给 key_len，RDB 就无法 binary-safe 保存；如果 command_table 没有 write_command，AOF 就不知道哪些命令该记录；如果 TTL 不给 expire_at_ms，持久化恢复后的过期语义就全错。

## 8.10 持久化测试：怎么证明重启恢复是对的

持久化的测试不能只测"文件是不是生成了"。要测的是"恢复之后的数据是不是和关之前一致"。KVLite 的持久化测试覆盖了以下维度：

- AOF 追加后，重启通过 `aof_load` 重放恢复，验证 key/value 一致
- RDB 快照保存后，重启通过 `kvs_rdb_load` 恢复，验证所有数据结构的数据完整
- TTL + AOF：EXPIRE 转 PEXPIREAT 的 log 记录正确，重放后过期时间恢复正确
- TTL + RDB：快照中 `expire_at_ms` 正确保存和恢复，加载时已过期 key 被跳过
- AOF rewrite：rewrite 后新 AOF 文件可正常加载，TTL 保留
- 四种数据结构：array/hash/rbtree/skiptable 各自的数据都能通过 RDB/AOF 恢复
- 大文件：AOF 文件膨胀后 rewrite 可正常生成压缩文件
- 性能对比：开启/关闭 AOF 的 QPS 差异
- shutdown 保存：正常退出流程保存数据，重启可恢复

每个测试的验证逻辑都是一致的：**写数据 → 设置状态（TTL） → 持久化 → 重启 → 检查数据是否一致、TTL 是否正确。**

## 8.11 当前持久化设计解决了什么，又留下了什么问题

### 解决了什么

1. **内存数据具备重启恢复能力。** AOF 和 RDB 双机制覆盖了日志恢复和快照恢复两种模式。

2. **AOF 完整记录了写命令历史。** 通过 command_table 的 `write_command` 字段统一接入，不需要在每条命令里单独判断。

3. **AOF rewrite 控制文件膨胀。** fork + COW（reactor/ntyco）或后台线程（proactor）做非阻塞重写，基于当前内存状态生成最小等价日志。

4. **rewrite buffer 保证了 rewrite 期间的数据完整性。** rewrite 期间的新写命令被缓存，完成后追加到临时文件。

5. **临时文件 + fsync + rename 保证了文件替换的原子性。** 写入失败不破坏原文件。

6. **三种 fsync 策略（always/everysec/no）可配置。** 使用者可以根据可靠性需求权衡性能。

7. **TTL 语义完整进入持久化。** EXPIRE 转 PEXPIREAT 写入 AOF，`expire_at_ms` 随 RDB 记录一起保存和恢复。

8. **binary-safe 支持。** AOF 追加和 RDB 保存都使用 `arg_lens`/`key_len`/`value_len` 而非 `strlen`。

### 留下了什么问题

1. **fork + COW 的健壮性。** 虽然 coredump 已经修了，但多线程 fork 的安全性依赖 `pthread_rwlock_wrlock` 在 fork 前锁住全局状态。如果未来增加新的后台线程，需要在 fork 前确保它们也处于安全状态——这是一种"依赖开发者自觉"而非"被机制强制保证"的安全。

2. **RDB 没有 checksum 和更严格的文件格式校验。** 文件损坏只能靠魔数和 EOF 做基本判断，缺少 CRC 或 SHA 完整性校验。不适合生产环境直接使用。

3. **AOF 和 RDB 的恢复没有显式的"原子性"保护。** 如果 RDB 加载成功但 AOF 加载到一半崩溃，重启后可能数据部分恢复、部分丢失。当前的设计依赖 shutdown 时的最后一次持久化覆盖来保证一致性——缺少类似"恢复日志"（recovery log）或"检查点"（checkpoint）的机制。

4. **`EVERYSEC` 的 fsync 触发依赖写事件而非独立定时器。** 如果连续 1 秒以上没有新写命令，待刷数据可能延迟到下一次写入才被刷盘。

5. **rewrite 期间持全局写锁（`data_rwlock`）的时间窗口虽然短，但仍然存在。** 在 fork 之前获取写锁会短暂阻塞所有读写请求。对于低延迟敏感的场景，这个窗口需要有监控。

6. **AOF rewrite 对超大 key 的处理。** 当前遍历四种数据结构时，每个 key 都是被完整读取到内存中再写入临时文件的。对于单个 value 很大的场景（比如 AI Chat 的完整对话历史），rewrite 的内存峰值可能较高。

7. **没有增量 RDB 或混合持久化。** 当前 RDB 每次都是全量快照，只有 AOF 是增量日志。Redis 4.0+ 的混合持久化（RDB + AOF tail）可以在全量和增量之间取得更好的平衡。

## 8.12 面试时我怎么讲这个阶段

### 30 秒版

问："KVLite 为什么同时做 AOF 和 RDB？它们分别解决什么问题？"

答："AOF 是写命令日志，每一条成功的写命令都追加到文件，恢复时逐条重放。优点是数据完整，缺点是文件会膨胀、恢复慢。RDB 是某个时刻的内存快照，序列化所有 key/value/expire 到一个二进制文件里。恢复快、文件紧凑，但两次快照之间的数据可能丢失。两者不是谁替代谁，是不同取舍。KVLite 启动时先加载 RDB 拿基础数据，再重放 AOF 追增量，这样既快又完整。"

### 2 分钟版

问："KVLite 的持久化是怎么设计的？AOF、RDB、rewrite、TTL 是怎么配合的？"

答："KVLite 最早是纯内存 KV，重启数据就没了。后来补了两个持久化：AOF 和 RDB。

AOF 挂在命令层——每条写命令执行成功后，通过 command_table 的 write_command 字段判断要不要进 AOF。AOF 追加的是 RESP 格式的命令序列，带 token_lens 支持 binary-safe。EXPIRE 命令进 AOF 时转成 PEXPIREAT 绝对时间戳，保证不管什么时候重放 TTL 语义都一致。fsync 有三种策略：always 每条都刷、everysec 后台每秒刷、no 交给操作系统。

AOF 文件会膨胀——同一个 key 被 SET 一百次就有一百条。所以需要 rewrite：根据当前内存状态重新生成最小等价的命令序列，每个现存 key 写一条 SET，有 TTL 的写 PEXPIREAT，已过期和已删除的不写。rewrite 写临时文件 + fsync + rename 原子替换原文件，防止写到一半崩溃损坏。rewrite 期间的并发写入通过 rewrite buffer 暂存，完成后追加到临时文件尾。

rewrite 用了 fork+COW 做非阻塞重写，但这中间踩过坑——多线程程序 fork 只复制当前线程，其他线程持有的锁在子进程里永远无法释放，导致 coredump。修复方案是 fork 前持全局写锁阻塞所有写操作，fork 后父子进程各解各的。Proactor 模式不用 fork，用后台线程。

RDB 是快照：把四种数据结构的所有活跃 key/value/expire_at_ms 序列化到二进制文件。文件有魔数和版本号做基本校验。加载时如果 key 已经过期就跳过。同样用临时文件 + rename。

启动恢复顺序是：先加载 RDB（基础数据），再重放 AOF（追增量）。shutdown 时做最后一次持久化。

当前方案已经打通了主链路，但还有不少边界：RDB 没有 checksum，恢复没有原子性保护，rewrite 持全局锁有短暂窗口，everysec 的 fsync 依赖写事件触发而非独立定时器。这些都是如果继续生产化需要补的点。"

## 8.13 本章小结

这一章复盘了 KVLite 的 AOF 和 RDB 持久化。

内核是：内存 KV 重启数据会丢，所以需要持久化。

两种思路：AOF 记录写命令历史，逐条重放恢复；RDB 保存某个时刻的内存快照，批量加载恢复。

AOF 的关键设计：写命令成功后才追加；write_command 字段驱动；aof_is_loading 防自循环；EXPIRE 转 PEXPIREAT；三种 fsync 策略可配；rewrite 基于当前内存状态重建最小等价日志；临时文件 + rename 原子替换；rewrite buffer 保存并发写入。

RDB 的关键设计：魔数 + 版本号校验文件；按数据结构类型分段序列化；key_len/value_len/expire_at_ms 完整保存；加载时检查过期；临时文件 + fsync + rename。

踩过的大坑：多线程 fork + COW 导致 coredump，修复方案是 fork 前的全局锁保护 + 子进程立刻解锁。

启动恢复：先 RDB 后 AOF，把快照基础和增量日志结合起来。

持久化依赖命令层（write_command、tokens）、存储层（遍历、_len 接口）、TTL（expire_at_ms、PEXPIREAT 转换）的协同——它不是独立的文件 IO 模块，而是全链路的交汇点。

下一章将复盘主从复制，重点讲为什么单机 KV 需要数据冗余，以及全量同步、增量同步、TTL 同步和 eBPF 旁路同步如何工作。

## 8.14 本章对应的面试追问清单

1. 为什么内存 KV 需要持久化？只做缓存不行吗？
2. AOF 和 RDB 分别是什么思路？它们各自适合什么场景？
3. 为什么 AOF 要在写命令成功后才追加？失败要不要写？
4. command_table 的 write_command 和 AOF 有什么关系？
5. `fwrite` 成功一定落盘了吗？`fsync` 和 `fdatasync` 有什么区别？
6. always / everysec / no 三种 fsync 策略各自的取舍是什么？
7. AOF 文件为什么会膨胀？为什么不能一直追加？
8. AOF rewrite 的本质是什么？为什么不是文本压缩？
9. rewrite 为什么不能直接覆盖原 AOF？临时文件 + rename 解决了什么？
10. `rename` 一定原子吗？有什么前提条件？
11. rewrite 期间新写命令怎么办？rewrite buffer 是怎么工作的？
12. fork + COW 为什么适合做 rewrite？同时有什么风险？
13. 多线程程序 fork 有什么危险？为什么可能导致 coredump？
14. KVLite 是怎么修 fork coredump 的？fork 前持锁、fork 后解锁的原理是什么？
15. Proactor 模式为什么不 fork 而是用线程做 rewrite？
16. RDB 相比 AOF 有什么优势和缺点？
17. RDB 文件里应该保存哪些字段？为什么 key_len/value_len 比 strlen 好？
18. RDB 为什么要保存 expire_at_ms？加载时发现 key 已过期怎么处理？
19. AOF 中为什么 EXPIRE 要转 PEXPIREAT？不转会有什么问题？
20. AOF 重放时为什么要避免再次追加 AOF？`aof_is_loading` 是怎么起作用的？
21. 启动恢复顺序为什么是先 RDB 后 AOF？反过来行不行？
22. RDB 没有 checksum 有什么风险？生产环境该怎么做？
23. 如何测试 AOF/RDB 恢复正确性？不能只测"文件是否生成"。
24. binary-safe 对 AOF/RDB 有什么要求？用 strlen 会出什么问题？
25. 如果让你重新设计持久化，你会优先优化哪一项？

---

*第八章完。下一章主题：主从复制——单机 KV 的数据冗余与故障切换。*
