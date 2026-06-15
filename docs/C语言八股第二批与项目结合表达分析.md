# KVLite：C 语言八股第二批与项目结合表达分析

> 本文与《C 语言八股与项目结合表达分析》（第一批）互补，不重复以下已分析内容：
> 1. C 面向对象（struct + self + .h/.c + ops 表）
> 2. 函数指针与 Reactor 回调链
> 3. malloc/free 与内存泄漏定位
> 4. memcpy/memmove 与 RESP buffer 前移
> 5. 宏、static inline、static/extern/volatile
> 6. void* 泛型抽象
> 7. struct 内存布局、对齐、柔性数组
> 8. strcmp/memcmp 与 binary-safe key
> 9. VSEARCH 运行时多态（void* impl + ops 函数指针表）

---

## 1. C 语言中的错误处理：返回码、errno、goto cleanup

### 八股知识点

- C 没有异常机制，常用 `int` 返回码表示成功/失败（0 为成功，<0 为错误）
- `errno` 是线程局部变量，**只在系统调用 / libc 函数返回 -1/NULL 时才有效**，成功路径下 `errno` 不会自动清零
- `perror()` / `strerror(errno)` 用于将 errno 转成可读字符串
- 多资源申请的函数中，使用 `goto cleanup` 统一释放资源，避免每个错误分支重复写释放代码
- EINTR 表示系统调用被信号中断，通常应该重试而非报错

### KVLite 中的体现

#### 1.1 返回码体系

项目中有统一的返回码约定。例如 RDB 模块（`src/persistence/kvs_rdb.c:47`）：

```c
static int rdb_write_exact(FILE *fp, const void *buf, size_t size) {
    return fwrite(buf, size, 1, fp) == 1 ? RDB_OK : RDB_ERR;
}
```

`RDB_OK`（0）和 `RDB_ERR`（1）定义在 `include/kvstore.h:492-493`，所有 RDB 写入函数都遵循此约定。

向量模块也有自己的返回码体系（`include/kvs_vector.h:16-24`）：`KVS_VECTOR_OK`、`KVS_VECTOR_ERR_ALLOC`、`KVS_VECTOR_ERR_NOT_FOUND` 等。

#### 1.2 errno 的谨慎使用

项目中对 errno 的使用比较规范。网络层（`src/network/reactor.c:344`）仅在系统调用失败后检查 errno：

```c
int count = recv(fd, recv_buf->data + recv_buf->size, recv_size, 0);
if (count <= 0) {
    if (count == 0) {                    // 对端正常关闭，不是错误
        event_unregister(fd);
        return 0;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;                            // 非阻塞 IO 的正常状态
    } else {
        KVS_LOG_ERROR_ERRNO("net", ...);  // 真正的错误
        event_unregister(fd);
        return -1;
    }
}
```

注意 `KVS_LOG_ERROR_ERRNO` 宏（`include/kvs_log.h:50`）在实现时做了正确的 errno 快照（`src/utils/kvs_log.c:265`）：

```c
void kvs_log_write_errno(kvs_log_level_t level, ...) {
    int saved_errno = errno;   // 第一时间保存，避免后续调用覆盖
    ...
    kvs_log_emit(level, module, file, line, 1, saved_errno, fmt, ap);
}
```

**风险提示**：项目中有些地方在成功路径也调用了 `KVS_LOG_ERROR_ERRNO`，此时 errno 可能是一个未定义的值（来自之前某次成功的系统调用）。这不一定会出错（因为 errno 通常不会被成功调用清零），但日志中打印的 errno 无意义。

#### 1.3 goto cleanup 与统一出口

**RDB 保存**（`src/persistence/kvs_rdb.c:416-503`）是经典的多资源+goto cleanup 模式：

```c
int kvs_rdb_save(const char *filename){
    FILE *fp = fopen(tmpname, "wb");
    if(!fp) return RDB_ERR;

    // 写魔数、版本、时间戳...
    if (rdb_write_exact(fp, RDB_MAGIC, strlen(RDB_MAGIC)) != RDB_OK) goto write_error;
    if (rdb_write_exact(fp, &version, sizeof(version)) != RDB_OK)   goto write_error;
    // ... 更多写入 ...

    if (fclose(fp) == EOF) { fp = NULL; goto write_error_no_close; }
    fp = NULL;
    if (rename(tmpname, filename) != 0) { ... }
    return RDB_OK;

write_error:
    fclose(fp);
    fp = NULL;
write_error_no_close:
    unlink(tmpname);   // 删除半成品文件
    return RDB_ERR;
}
```

两个 label 的设计很精妙：`write_error` 负责"fp 还开着"的清理路径（需要 fclose），`write_error_no_close` 负责"fp 已经 close 了"的清理路径——避免了 double close。

**AOF 初始化**（`src/persistence/kvs_aof.c:37-176`）展示了更复杂的多资源回滚模式。初始化函数依次创建了 `filename strdup` → `buf malloc` → `temp_filename malloc` → `fopen` → `mutex_init` → `cond_init` → `rewrite_mutex_init` → `bgthread_create` → `rwlock_init`，每一步失败都按顺序回滚前面已分配的资源。这里没有用 goto，而是用 if-return 逐层清理，两种写法各有利弊——goto 更清晰但项目早期可能没统一风格。

**向量初始化**（`src/vector/kvs_vector.c:666-724`）使用了 `goto fail` 模式：

```c
int kvs_vector_init(void) {
    ret = kvs_vector_store_init(&g_vector_state.store);
    if (ret != KVS_VECTOR_OK) goto fail;
    ret = kvs_vector_store_reserve(...);
    if (ret != KVS_VECTOR_OK) goto fail;
    ret = kvs_vector_id_map_init(...);
    if (ret != KVS_VECTOR_OK) goto fail;
    // ... 继续初始化更多组件 ...
    return KVS_VECTOR_OK;

fail:
    kvs_vector_deinit();   // 统一清理
    return KVS_VECTOR_ERR_INTERNAL;
}
```

**NtyCo 连接处理**（`src/network/ntyco.c:242`）在退出协程时用 `goto out` 统一释放连接资源。

#### 1.4 网络 IO 错误处理

Reactor 网络层（`src/network/reactor.c`）对每条连接的 recv/send 都做了三种情况区分：
- `== 0`：对端正常关闭 → `event_unregister`
- `< 0 && (EAGAIN || EWOULDBLOCK)`：非阻塞正常 → break 或等待下次事件
- `< 0 && 其他`：真正的错误 → 日志+关闭连接

`epoll_wait` 也处理了 EINTR（`reactor.c:535`）：

```c
int nready = epoll_wait(epfd, events, 1024, -1);
if (nready < 0 && errno != EINTR) {
    KVS_LOG_ERROR_ERRNO("net", "epoll_wait failed");
    break;
}
```

#### 1.5 文件 IO 错误处理

AOF rewrite 中的 `safe_write` 函数（`src/persistence/kvs_aof_rewrite.c:117`）处理了 short write 和 EINTR：

```c
static int safe_write(int fd, const char *buf, size_t count){
    size_t written = 0;
    while(written < count){
        ssize_t n = write(fd, buf + written, count - written);
        if(n <= 0){
            if(n == -1 && errno == EINTR) continue;  // 信号中断，重试
            return -1;  // 真正的错误
        }
        written += n;
    }
    return 0;
}
```

### 面试中可以这样说

"KVLite 中我处理了多种错误场景。所有持久化相关的函数统一用 int 返回码，RDB_OK/RDB_ERR。多资源申请的函数用 goto cleanup 统一释放，比如 RDB 保存函数写了 8 个资源申请点，每个失败都跳转到 write_error 标签关闭文件、清理临时文件。网络层对 recv/send 做了三种情况区分：对端关闭返回 0、EAGAIN 表示非阻塞正常、其他错误记录日志并关闭连接。errno 方面，日志模块在记录 errno 时先做快照 `int saved_errno = errno`，防止后续函数调用覆盖。系统调用返回 EINTR 时统一重试，不会当成错误。"

### 可能被追问

**Q: errno 什么时候有效？**
A: 只在系统调用或 libc 返回错误指示（-1 或 NULL）时才有效。成功路径下的 errno 值无意义。项目中只在 `recv < 0`、`write <= 0` 等条件内才检查 errno。

**Q: 为什么 C 项目常用 goto cleanup？**
A: C 没有 RAII 和 defer，多资源函数如果不 goto 统一出口，每个错误分支都要复制粘贴 3-5 行释放代码，容易漏释放或释放顺序错误。goto cleanup 让正常路径和错误路径分离。

**Q: 系统调用被 EINTR 打断怎么办？**
A: 大部分情况应该重试。KVLite 的 `safe_write` 看到 `n == -1 && errno == EINTR` 就 `continue` 继续写；`epoll_wait` 看到 `nready < 0 && errno != EINTR` 才当错误处理（因为 EINTR 只是信号打断，不是 epoll 本身出错）。

---

## 2. C 文件 IO 与系统调用：open/read/write/close/fsync/rename

### 八股知识点

- **标准 IO（fopen/fwrite）vs 系统调用（open/write）**：标准 IO 有用户态缓冲区，fwrite 之后数据可能还在 libc buffer 里；write 直接进内核。持久化场景需要 fflush + fsync 才能保证落盘
- **short write**：write 可能只写入了部分字节，需要循环写直到全部写完
- **fsync vs fdatasync**：fsync 同步数据和元数据（mtime、size 等），fdatasync 只同步数据。AOF 追加写场景用 fdatasync 理论上更轻量
- **rename 原子性**：同一文件系统内 rename 是原子的，适合"写临时文件 → rename 覆盖"的持久化模式
- **AOF 追加写**：AOF 是 append-only，天然适合顺序写磁盘，性能比随机写好
- **AOF rewrite 目的**：AOF 文件随时间膨胀后，rewrite 用当前数据快照生成一份紧凑的新文件（用 SET 命令而非历史增量），再用 rename 原子替换

### KVLite 中的体现

#### 2.1 AOF 写入链路（`src/persistence/kvs_aof.c`）

AOF 使用标准 IO（`FILE *fp`）以追加模式打开（`fopen(filename, "a+")`，第 77 行）。写入链路：

1. `aof_append()` 将命令格式化为 RESP 写入 AOF 缓冲区（`aof_state.buf`）
2. 缓冲区过半或超过阈值时唤醒后台 fsync 线程
3. 后台线程 `aof_background_fsync()` 每 1 秒被条件变量唤醒，调用 `aof_fsync(0)`
4. `aof_fsync()` 执行 `fwrite` → `fflush` → 根据 sync 模式决定是否 `fsync`

fsync 策略（`include/kvs_config.h:46-49`）：
- `KVS_AOF_FSYNC_NO`：不主动 fsync，靠 OS 刷盘（性能最高，丢数据风险最大）
- `KVS_AOF_FSYNC_EVERYSEC`：每秒 fsync 一次（默认，性能与安全平衡）
- `KVS_AOF_FSYNC_ALWAYS`：每次写入后 fsync（最安全，性能最差）

`aof_fsync()` 中（第 350 行）：
```c
int fd = fileno(aof_state.fp);     // 从 FILE* 获取 fd
if(fsync(fd) == -1){               // 强制刷盘
    KVS_LOG_ERROR_ERRNO("persistence", "failed to fsync AOF file");
}
```

**大命令直写优化**（第 229-275 行）：如果单条命令过大（超过 AOF 缓冲区一半），走直写路径——先用缓冲区中的旧数据 flush，然后直接 `fprintf` + `fwrite` 到文件，避免大命令撑爆内存缓冲区。

#### 2.2 AOF rewrite 链路（`src/persistence/kvs_aof_rewrite.c`）

AOF rewrite 使用系统调用（`open/write/close`）而非标准 IO，原因是子进程中更偏好系统调用（避免 libc buffer 的 fork 安全隐患）。

**临时文件 + rename 原子替换**（第 575-597 行）：

```c
static int atomic_replace_file(const char *temp_file, const char *final_file){
    char backup_file[1024];
    snprintf(backup_file, sizeof(backup_file), "%s.bak", final_file);

    // 1. 备份原文件
    if (rename(final_file, backup_file) == -1 && errno != ENOENT) {
        return -1;
    }
    // 2. 临时文件 → 正式文件（同一文件系统内原子操作）
    if (rename(temp_file, final_file) == -1) {
        rename(backup_file, final_file);  // 失败时恢复备份
        return -1;
    }
    // 3. 删除备份
    unlink(backup_file);
    return 0;
}
```

完整 rewrite 流程：
1. 创建临时文件 `aof_filename.tmp` → 子进程遍历所有数据结构写入 RESP 命令
2. `fsync(fd)` 确保子进程写入的数据已落盘 → `close(fd)`
3. 父进程打开临时文件追加 rewrite buffer 中的增量命令 → `fsync` → `close`
4. `atomic_replace_file` 用 rename 原子替换
5. `aof_reopen_after_rewrite_locked()` 关闭旧 fp 重新打开新文件

**safe_write 循环写**（第 117-128 行）：处理 short write 和 EINTR。

**fsync 在子进程中的使用**（第 851 行）：子进程写完临时文件后显式 `fsync(fd)` 再 `_exit(0)`。

#### 2.3 RDB 保存/加载链路（`src/persistence/kvs_rdb.c`）

RDB 使用标准 IO（`fopen/fread/fwrite`）+ 二进制格式。写入链路：

1. `fopen(tmpname, "wb")` → 写入魔数 + 版本 + 时间戳
2. 依次写入各数据结构的二进制序列化数据
3. `fflush(fp)` → `fsync(fileno(fp))` → `fclose(fp)`
4. `rename(tmpname, filename)` 原子替换

RDB 写操作封装了一个 helpers 族（`rdb_read_exact`/`rdb_write_exact`/`rdb_write_u8`/`rdb_read_u64` 等），每个都返回 RDB_OK/RDB_ERR。

加载链路（第 506 行起）：`fopen(filename, "rb")` → 读魔数/版本校验 → 清空当前数据 → 按 type 枚举依次加载各数据结构。

#### 2.4 项目中是否存在短写风险

`kvs_rdb.c` 的 `rdb_write_exact`（第 47 行）用 `fwrite(buf, size, 1, fp) == 1` 来检测完整写入——标准 IO 的 `fwrite` 内部会处理短写，所以这里不直接面临 write 的短写问题。

但 `kvs_aof_rewrite.c` 的 `safe_write` 直接使用 `write()` 系统调用，所以需要循环写来防护 short write。

### 面试中可以这样说

"KVLite 的持久化包括 AOF 和 RDB 两套机制。AOF 用追加写模式，标准 IO fopen('a+')，命令先写内存缓冲区，后台线程每秒 fsync 一次，有三种同步策略可选。大命令走直写路径避免撑爆缓冲区。AOF rewrite 是经典方案：fork 子进程生成数据快照写入临时文件，rename 原子替换原文件，rewrite 期间的增量命令记录在 rewrite buffer 中最后追加。RDB 用二进制格式，先写临时文件再 fflush+fsync 确保落盘，最后 rename 原子替换。短写方面，系统调用 write 路径我写了 safe_write 循环写直到全部完成，同时处理 EINTR 重试。"

### 可能被追问

**Q: write 成功返回就一定落盘了吗？**
A: 不一定。write 只保证数据进了内核的 page cache，掉电可能丢失。必须 fsync/fdatasync 才能保证落盘。

**Q: fsync 和 fdatasync 区别？**
A: fsync 同步文件数据和元数据（inode 的 mtime、size 等），fdatasync 只同步数据。AOF 追加写场景下用 fdatasync 可以减少一次元数据写操作，但 KVLite 目前都用 fsync。

**Q: 为什么 rewrite 要写临时文件再 rename？**
A: 两个原因：一是 rename 在同一文件系统内是原子的，不会出现"文件写到一半被读到"的情况；二是 rewrite 过程长，不能直接覆盖正在使用的 AOF 文件。Redis 也是这么做的。

**Q: fopen/fwrite 和 open/write 有什么区别？**
A: fopen/fwrite 是标准 C 库，有用户态缓冲区（默认 4KB-8KB），适合小数据量或格式化输出。open/write 是系统调用，无用户态缓冲，适合大量数据的精确控制。KVLite 的 AOF 日常追加用 fopen，rewrite 子进程用 write 系统调用避免了 fork 后 libc buffer 状态的复杂性。

---

## 3. fork、COW 与 AOF rewrite / RDB snapshot

### 八股知识点

- **fork 后父子进程**：fork 创建子进程，子进程拥有父进程虚拟地址空间的完整副本。但由于 COW（Copy-On-Write），物理页不会立即复制——父子共享同一物理页，直到某一方写入时才触发页复制
- **COW 内存放大**：如果父进程在子进程运行期间大量写入，COW 触发的页复制会导致内存使用翻倍甚至更多
- **fork 多线程风险**：fork 只复制调用线程，其他线程在子进程中"消失"。如果其他线程持有锁，子进程中的对应锁永远无法释放，可能导致子进程死锁
- **子进程适合做快照**：子进程的地址空间是 fork 瞬间的快照，适合做 RDB 保存或 AOF rewrite 这类"读取当前数据、写入磁盘"的 IO 密集型任务
- **waitpid**：回收子进程，避免僵尸进程。WNOHANG 选项实现非阻塞检查

### KVLite 中的体现

KVLite 的 AOF rewrite 使用了 fork+COW 机制（`src/persistence/kvs_aof_rewrite.c`），且针对不同网络模型做了适配。

#### 3.1 COW 快照保护（第 42-81 行）

fork 前通过 `acquire_cow_snapshot()` 获取写锁保护：

```c
static int acquire_cow_snapshot(void){
    // 获取写锁，阻塞所有新的写操作
    if(pthread_rwlock_wrlock(&aof_state.data_rwlock) != 0){
        return -1;
    }
    aof_fsync(1);                    // 刷盘当前 AOF 缓冲区
    aof_state.cow_snapshot_ready = 1; // 设置快照就绪
    aof_state.snapshot_timestamp = time(NULL);
    return 0;
}
```

**设计意图**：fork 前拿写锁保证此时没有正在进行的写操作，fork 瞬间的数据是一致的。fork 后子进程释放锁（拥有自己的锁副本），父进程也释放锁恢复服务。

#### 3.2 fork + 子进程写临时文件（第 830-859 行）

```c
pid_t childpid = fork();
if(childpid == 0){
    // 子进程：拥有父进程数据的 COW 副本
    pthread_rwlock_unlock(&aof_state.data_rwlock);  // 自己的锁副本，unlock 即可

    int fd = create_rewrite_tempfile();
    if (fd == -1) _exit(1);

    if (write_data_to_tempfile(fd) == -1) { close(fd); _exit(1); }
    if (fsync(fd) == -1) { close(fd); _exit(1); }

    close(fd);
    _exit(0);    // 子进程直接 _exit，不经过 atexit 和 stdio flush
}else if(childpid > 0){
    // 父进程
    release_cow_snapshot();  // 释放写锁，恢复服务
    aof_state.rewrite_child_pid = childpid;

    int status = wait_for_child_with_timeout(childpid, 300);  // 5分钟超时
    // 追加重写缓冲区增量命令 → fsync → rename 原子替换
}
```

#### 3.3 子进程中的 COW 安全处理

子进程遍历数据结构时使用了 `__atomic_load_n` + `__ATOMIC_ACQUIRE`（第 283-286 行、第 350-353 行），避免编译器优化导致读取到不一致的指针。这是因为子进程的地址空间是 fork 时刻的 COW 快照，但数据结构的 key/value 指针指向的堆内存是共享的，需要使用 acquire 语义保证可见性。

**array 数据的两阶段快照**（第 328-398 行）：先原子读取所有有效记录的指针存入本地数组，再逐个写入文件。这降低了"写文件过程中父进程修改数据触发 COW 复制"的时间窗口。

#### 3.4 超时与强制终止（第 522-553 行）

`wait_for_child_with_timeout` 实现了三级超时机制：
1. `waitpid(pid, &status, WNOHANG)` 非阻塞轮询
2. 超时后 `kill(pid, SIGTERM)`，再等 1 秒
3. 如果还没退出，`kill(pid, SIGKILL)` 强制杀死

子进程本身也有超时保护（第 277-280 行、第 366-369 行），遍历超过 290 秒会主动停止。

#### 3.5 Proactor 模式的降级处理

fork 和 io_uring 有兼容性问题（io_uring 的共享内存映射在 fork 后行为不确定）。KVLite 在 Proactor 模式下用**线程替代 fork**（第 762-819 行）：

```c
if (is_proactor_mode()) {
    // 用线程而不是 fork，避免 io_uring 冲突
    pthread_create(&rewrite_tid, NULL, aof_rewrite_thread, NULL);
    pthread_join(rewrite_tid, &thread_result);
    // 追加重写缓冲区 → fsync → rename
}
```

这是一个务实的工程决策：fork+COW 在单线程 Reactor 下是最优解（零拷贝遍历），但在 Proactor 下用线程+读锁保证数据一致性。

#### 3.6 rewrite 期间的增量命令（第 628-677 行）

父进程在 rewrite 期间继续接收写命令，这些命令被记录在 `rewrite_buf` 中（通过 `aof_rewrite_append_command`）。子进程结束后，父进程将 rewrite buffer 的内容追加到临时文件末尾，保证不丢命令。

**风险评估**：
- COW 内存放大：如果 rewrite 期间父进程大量写入，COW 复制会导致内存压力。KVLite 没有做 COW 内存使用量监控
- fork 多线程风险：虽然 Reactor 模式下只有主线程在用，但后台线程（AOF fsync、TTL 扫描）在 fork 时会"消失"在子进程中。不过子进程不需要这些线程，且子进程用的是 `_exit()` 而非 `exit()`，不会触发 atexit handler

### 面试中可以这样说

"KVLite 的 AOF rewrite 参考了 Redis 的 fork+COW 方案。fork 前用写锁保证数据一致性，fork 后父进程立即释放锁恢复服务。子进程利用 COW 特性零拷贝遍历当前数据快照写入临时文件，父子进程共享物理页直到父进程写入新数据才触发 COW 复制。rewrite 期间的增量命令记录在 rewrite buffer 里，子进程结束后追到临时文件末尾，最后 rename 原子替换。

解决了一个实际问题：Proactor 模式下 io_uring 和 fork 不兼容，我做了降级处理——用线程+读锁替代 fork。超时方面实现了三级强制终止：先 SIGTERM、再等 1 秒、最后 SIGKILL。子进程内部也有 290 秒的单次超时保护，防止 rewrite 无限运行。"

### 可能被追问

**Q: fork 后父子进程内存是完全复制吗？**
A: 虚拟地址空间是复制的，但物理页通过 COW 共享。只有某一方写入时才触发页复制，分配新的物理页。

**Q: fork 多线程程序有什么风险？**
A: fork 只复制调用线程，其他线程在子进程中"消失"。如果其他线程持有锁，子进程中该锁永远无法释放。KVLite 在 Reactor 单线程主循环下 fork，问题较小，但后台线程仍存在——解决办法是 fork 前拿写锁阻塞其他操作，子进程立即 `_exit()` 退出。

**Q: rewrite 期间新写入命令怎么办？**
A: 记录在 rewrite buffer 中。子进程结束后，父进程打开临时文件追加这些增量命令，然后 fsync + rename 完成原子替换。这和 Redis 的做法一致。

**Q: 子进程能不能拿父进程的 mutex？**
A: 不能。子进程中的锁是 fork 时刻的副本，和父进程的锁是两套东西。KVLite 的子进程在 fork 后立即 `pthread_rwlock_unlock` 释放自己那份写锁副本，然后独立运行，不再与父进程有锁交互。

---

## 4. 非阻塞 IO、EAGAIN、EINTR 与 epoll 边缘细节

### 八股知识点

- **阻塞 vs 非阻塞**：阻塞 IO 在数据未就绪时挂起线程；非阻塞 IO 立即返回 -1 并设置 `errno=EAGAIN/EWOULDBLOCK`
- **EAGAIN** = 非阻塞 IO 的正常状态："现在没有数据可读/缓冲区满不可写，请稍后再试"
- **ET（边缘触发）vs LT（水平触发）**：ET 只在状态变化时通知一次，必须循环读到 EAGAIN 否则丢事件；LT 只要 fd 可读就反复通知
- **EINTR**：信号中断，通常应该重试
- **EPOLLIN/EPOLLOUT**：epoll 只告诉你"可能可读/可写"，send buffer 满时需要注册 EPOLLOUT 等待可写通知
- **连接断开**：recv 返回 0 表示对端正常关闭（FIN）；EPOLLERR/EPOLLHUP 表示异常断开

### KVLite 中的体现

#### 4.1 非阻塞设置（`src/network/reactor.c:66`）

```c
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
```

每个新连接注册时（第 163 行）都会调用 `set_nonblocking(fd)`，listen fd 也设为非阻塞（第 499 行）。

#### 4.2 recv 循环读到 EAGAIN（`src/network/reactor.c:306-386`）

```c
int recv_cb(int fd) {
    while (1) {
        // 扩容检查...
        int count = recv(fd, recv_buf->data + recv_buf->size, recv_size, 0);
        if (count <= 0) {
            if (count == 0) {
                event_unregister(fd);      // 对端关闭
                return 0;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;                      // 缓冲区已空，停止读取
            } else {
                KVS_LOG_ERROR_ERRNO("net", ...);
                event_unregister(fd);
                return -1;
            }
        }
        recv_buf->size += count;
        // 继续循环读，尽可能一次事件处理多帧数据
    }
    // 解析并生成响应
    net_session_process_resp(fd, &ext->session, reactor_session_handler, ext);
    // 有数据要发送 → 注册 EPOLLOUT
    if(send_buf->size > 0){
        set_event(fd, EPOLLIN | EPOLLOUT, 0);
    }
    return 0;
}
```

这是 ET 模式下的标准循环读取：一直 recv 直到返回 EAGAIN，确保不丢事件。

#### 4.3 send 处理 EAGAIN + EPOLLOUT 注册（`src/network/reactor.c:394-449`）

```c
int send_cb(int fd) {
    while (ext->send_offset < send_buf->size) {
        ssize_t sent = send(fd, send_buf->data + ext->send_offset,
                            remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // TCP 发送缓冲满，注册 EPOLLOUT 等待可写通知
                set_event(fd, EPOLLOUT | EPOLLIN, 0);
                return 0;
            }
            event_unregister(fd);
            return -1;
        }
        ext->send_offset += sent;
    }
    // 全部发送完毕，取消 EPOLLOUT 只监听 EPOLLIN
    set_event(fd, EPOLLIN, 0);
    return 0;
}
```

`MSG_NOSIGNAL` 标志避免了向已关闭的 socket 写入时触发 SIGPIPE 信号——这是网络编程的常见坑。

#### 4.4 epoll 使用模式

KVLite 使用 **EPOLLET（边缘触发）**（第 167 行）：

```c
return set_event(fd, event | EPOLLET, 1);
```

主循环（第 530-557 行）：
```c
struct epoll_event events[1024] = {0};
while (!g_shutdown_flag) {
    int nready = epoll_wait(epfd, events, 1024, -1);
    if (nready < 0 && errno != EINTR) break;   // EINTR 重试

    for (int i = 0; i < nready; i++) {
        int connfd = events[i].data.fd;
        if (events[i].events & EPOLLIN)  recv_cb(connfd);
        if (events[i].events & EPOLLOUT) send_cb(connfd);
        if (events[i].events & (EPOLLERR | EPOLLHUP)) event_unregister(connfd);
    }
}
```

#### 4.5 单连接 Pipeline 保护

虽然没有显式的单连接命令数限制，但有几个隐式保护：
- 接收无限循环但每次 `recv_size` 上限 256KB（第 334 行）
- 发送端 `MAX_SINGLE_SEND` 限制单次 send 为 1MB（第 34 行）
- 缓冲区 8MB 上限作为自然背压

**风险提示**：如果一个连接连续发大量 Pipeline 命令，当前实现会在一次 `recv_cb` 调用中全部处理完，可能导致其他连接饥饿。可以加一个处理上限（如一次最多处理 64 条命令）来改进公平性。

#### 4.6 NtyCo 协程网络层（`src/network/ntyco.c`）

NtyCo 使用 `poll` + 协程调度的方式，同样设置了非阻塞 socket（`fcntl(sockfd, F_SETFL, flags | O_NONBLOCK)`，第 355 行）。错误处理逻辑与 Reactor 一致：EAGAIN → 继续等，EINTR → 重试，其他错误 → 断开。

eBPF 同步守护进程（`src/eBPF/ebpf_syncd.c`）也将 socket 设为非阻塞（第 942 行），并使用 `send` + `MSG_NOSIGNAL` + `EAGAIN/EINTR` 处理。

### 面试中可以这样说

"KVLite 的网络层使用 epoll ET 模式 + 非阻塞 IO。每个连接通过 fcntl 设 O_NONBLOCK。recv 回调里循环读到 EAGAIN 才停，确保在 ET 模式下不丢事件。send 回调也是循环写，遇到 EAGAIN 就注册 EPOLLOUT 等内核通知可写，避免应用层忙等。send 用 MSG_NOSIGNAL 防止 SIGPIPE。epoll_wait 遇到 EINTR 不报错直接重试。连接断开通过 recv 返回 0 判断正常关闭，通过 EPOLLERR/EPOLLHUP 判断异常关闭。三个网络后端（Reactor/Proactor/NtyCo）都遵循这套 EAGAIN/EINTR 处理规范。"

### 可能被追问

**Q: ET 模式为什么必须循环读？**
A: ET 只在"不可读→可读"的状态变化时通知一次。如果只读一次，fd 上可能还有数据没读完，但不会再收到通知，数据就丢了。必须循环读到 EAGAIN 才安全。

**Q: send 返回部分字节怎么办？**
A: KVLite 用 `send_offset` 记录已发送位置，下次 EPOLLOUT 触发时从该位置继续发送，直到 `send_offset >= send_buf->size` 全部发完。

**Q: 如何防止单连接 Pipeline 太多命令饿死其他连接？**
A: 目前没有显式防护。实际上可以通过设置单次处理命令数上限（如 64 条）来保证公平性，这是项目的一个改进方向。

---

## 5. pthread 多线程：mutex/rwlock/cond、读写锁和竞态

### 八股知识点

- **pthread_mutex_t**：互斥锁，同一时刻只有一个线程持有，适合写操作
- **pthread_rwlock_t**：读写锁，多读单写。读锁可并发持有，写锁独占。适合读多写少场景
- **pthread_cond_t**：条件变量，配合 mutex 用于生产者-消费者模式。`pthread_cond_wait` 原子释放 mutex 并进入等待；被唤醒后重新获取 mutex
- **条件变量 while 判断**：`pthread_cond_wait` 可能被虚假唤醒，必须用 while 而非 if 检查条件
- **读写锁升级陷阱**：直接"读锁升级写锁"会死锁，必须先释放读锁再获取写锁
- **锁粒度**：锁太粗（全局锁）影响并发能力，锁太细（多个锁）增加死锁风险

### KVLite 中的体现

#### 5.1 全局读写锁保护数据（`src/core/kvstore.c:16`）

```c
pthread_rwlock_t g_kv_data_rwlock = PTHREAD_RWLOCK_INITIALIZER;
```

命令执行前根据命令类型决定加读锁还是写锁（第 94-106 行）：
- GET、EXIST 等只读命令 → `pthread_rwlock_rdlock`
- SET、DEL 等写命令 → `pthread_rwlock_wrlock`

**读写锁升级的重试机制**（第 108-118 行）：

```c
int ret = cmd->handler(&ctx, response);
if (use_read_lock && ret == KVS_CMD_RETRY_WITH_WRITE_LOCK) {
    pthread_rwlock_unlock(&g_kv_data_rwlock);
    lock_held = 0;
    if (pthread_rwlock_wrlock(&g_kv_data_rwlock) != 0) { ... }
    ctx.lock_mode = KVS_LOCK_MODE_WRITE;
    ret = cmd->handler(&ctx, response);
}
```

这是一个精巧的设计：命令处理函数如果发现需要写但当前只持了读锁，可以返回 `KVS_CMD_RETRY_WITH_WRITE_LOCK`（-3），上层的锁管理代码会释放读锁、重新获取写锁、再执行一次。这就是避免了"读锁升级死锁"的正确做法——先释放再获取。

#### 5.2 AOF 的多锁体系（`include/kvs_aof_internal.h`）

AOF 模块有三个锁：
- **`mutex`**（互斥锁）：保护 AOF 缓冲区的读写和 fsync 操作
- **`cond`**（条件变量）：后台 fsync 线程的唤醒信号
- **`data_rwlock`**（读写锁）：COW 快照保护，fork 前拿写锁阻塞写操作
- **`rewrite_mutex`**（互斥锁）：保护 rewrite buffer

#### 5.3 mutex + cond 的生产者-消费者（`src/persistence/kvs_aof.c:622-648`）

后台 fsync 线程：

```c
void aof_background_fsync(void *arg){
    while(!aof_state.stop){
        pthread_mutex_lock(&aof_state.mutex);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&aof_state.cond, &aof_state.mutex, &ts);  // 1秒超时

        if(aof_state.buf_len > 0 && ...){
            pthread_mutex_unlock(&aof_state.mutex);  // 释放锁后执行 fsync
            aof_fsync(0);
            pthread_mutex_lock(&aof_state.mutex);    // 重新获取
        }
        pthread_mutex_unlock(&aof_state.mutex);
    }
}
```

**风险提示**：注释中写道第 295 行 `FSYNC_ALWAYS` 下直接调 `aof_fsync(1)` 会死锁（因为 `aof_fsync` 内部也拿 `mutex`），所以改用了标志位 `should_fsync`，在锁外调用。这是典型的锁嵌套死锁问题及其修复。

#### 5.4 TTL 后台扫描线程（`src/core/kvs_ttl_background.c`）

TTL 扫描器是一个独立线程（`ttl_scan_thread_main`），每 1 秒遍历四种数据结构，拿写锁删除过期 key。因为要删除数据，所以需要 `pthread_rwlock_wrlock`（第 199 行）：

```c
static void ttl_scan_once(void) {
    if (pthread_rwlock_wrlock(&g_kv_data_rwlock) != 0) return;
    // 遍历四种数据结构，删除过期 key
    pthread_rwlock_unlock(&g_kv_data_rwlock);
}
```

**设计考量**：TTL 扫描用写锁会影响正常的读写请求，所以做了几个优化：
- 每次最多处理 32 个 key（`budget = 32`），限制持锁时间
- 从节点跳过扫描（`if (global_dist_config.role == ROLE_SLAVE) continue`），减少不必要的开销
- 遍历过程中收集过期 key，最后统一删除，减少中间状态

#### 5.5 分布式模块的 mutex（`src/distributed/distributed.c`）

`slave_mutex` 保护从节点列表的并发访问。`master_thread` 发送全量同步/增量同步时需要遍历 slave 列表，加锁保护。

#### 5.6 消息队列的无锁优化尝试

`msg_queue.h` 中使用了 `_Atomic size_t head/tail`，配合传统的 mutex+cond 实现了一个 MPMC 队列。但注意队列里还有 `count` 字段不是原子的——这暗示 push/pop 仍然依赖外部锁，原子变量主要用于无锁的快速边界检查。

### 面试中可以这样说

"KVLite 的多线程并发安全主要通过一个全局的 pthread_rwlock_t 实现。读命令（GET/EXIST）拿读锁，写命令（SET/DEL）拿写锁。为了解决"读锁无法直接升级写锁"的问题，我在锁管理层实现了重试机制：命令处理函数如果发现需要写但只有读锁，返回特定错误码 KVS_CMD_RETRY_WITH_WRITE_LOCK，外层释放读锁重新获取写锁再执行。

AOF 模块用 mutex+cond 实现了后台 fsync 线程的每秒定时刷盘，解决了生产者-消费者问题。注意为了防止死锁，fsync 不能在持有 mutex 的情况下调用——代码中专门在锁外执行 fsync。TTL 后台扫描线程也是读写锁配合，每次最多处理 32 个 key 控制持锁时间，避免长时间阻塞正常请求。"

### 可能被追问

**Q: pthread_rwlock 为什么适合 KV 读多写少？**
A: 读锁允许多个读者并发进入，写锁独占。KV 场景下 GET 请求远多于 SET，读写锁让大部分 GET 请求可以并行执行，不需要互斥等待。

**Q: 读锁升级写锁有什么风险？**
A: 直接升级会死锁——两个持有读锁的线程都尝试升级，互相等对方释放读锁，形成死锁。正确做法是释放读锁再获取写锁，KVLite 的 KVS_CMD_RETRY_WITH_WRITE_LOCK 正是这么做的。

**Q: 条件变量为什么要 while 而不是 if？**
A: `pthread_cond_wait` 可能被虚假唤醒（spurious wakeup）。如果用 if，被虚假唤醒后就直接往下执行但条件可能还不满足。用 while 会在被唤醒后重新检查条件。

**Q: 原子变量和 mutex 怎么选？**
A: 原子变量适合单个变量的简单操作（计数器、标志位），零锁开销。mutex 适合保护多个变量的复合操作或临界区。KVLite 的 msg_queue 用 `_Atomic size_t` 保护 head/tail 索引，但整体 push/pop 仍需要 mutex 保护 count 和 buffer 的一致性。

---

## 6. C 字符串安全：strncpy/snprintf/strdup/strlen 的坑

### 八股知识点

- **strcpy/sprintf** 不检查目标缓冲区大小，容易缓冲区溢出
- **strncpy** 不一定自动加 `'\0'`——如果 src 长度 >= n，dst 不会以 `'\0'` 结尾
- **snprintf** 更安全（自动截断并保证 `'\0'`），但要检查返回值判断是否被截断
- **strlen** 依赖 `'\0'` 结尾，不适合二进制数据（key/value 可能包含 `'\0'`）
- **strdup** 内部调用 malloc，需要调用者 free
- **strtok** 使用静态缓冲区，非线程安全；strtok_r 可重入

### KVLite 中的体现

#### 6.1 全面使用 snprintf

整个项目对字符串格式化几乎全部使用 `snprintf`，没有发现 `sprintf` 的使用。典型代码（`src/persistence/kvs_aof_rewrite.c:142`）：

```c
int n = snprintf(buf, sizeof(buf), "$%zu\r\n%s\r\n", cmd_len, cmd_type);
```

AOF 追加也使用两阶段 snprintf：先用 `snprintf(NULL, 0, ...)` 计算所需空间，再在缓冲区内写入。

**风险提示**：AOF rewrite buffer 写入处（第 660 行）使用了 `snprintf(buf_ptr, remaining_size, ...)` 但剩余空间检查在加锁区域之外计算，理论上有微小的 TOCTOU 风险。不过因为 rewrite buffer 是单写者（只有 rewrite 路径会追加），实际上问题不大。

#### 6.2 统一内存管理的 strdup（`src/storage/kvs_mempool.c:156`）

```c
char* kvs_strdup(const char *s){
    // 使用项目统一的内存池分配，而非 libc strdup 的 malloc
}
```

相比 libc 的 `strdup`（内部 malloc），`kvs_strdup` 走项目统一的内存管理，方便内存追踪和泄漏检测。

AOF 初始化中有一次直接用 libc `strdup`（第 51 行 `aof_state.filename = strdup(filename)`），但后续重写模块用 `kvs_strdup`，存在内存管理不统一的问题——`strdup` 分配的内存需要用 `free()` 释放，而 `kvs_strdup` 分配的需要用 `kvs_free()` 释放。从 AOF 释放函数看（`aof_free` 中用 `free(aof_state.filename)`），项目对此是有意识的。

**混合风险**：AOF rewrite 注释（第 100 行）写"不再混用 strdup/free，统一内存管理"，说明之前确实遇到过混用带来的问题。

#### 6.3 Binary-safe key/value 与字符串安全的边界

项目已经支持 binary-safe key/value（通过 `token_lens` 传递长度），但内部很多地方仍使用 `strlen` 计算长度。例如 `kvs_operation.c` 中命令参数解析：

```c
// 第 664 行：用 strlen 计算长度
errno = 0;
long long val = strtoll(s, &endptr, 10);
if (endptr == s || *endptr != '\0' || errno == ERANGE) { ... }
```

当 key/value 是二进制数据时，`strtoll` 这类依赖 `'\0'` 的函数不受影响（因为解析的是数字字符串），但 `strlen(key)` 或 `strcmp` 就不可用了。项目的边界处理是：
- RESP 解析层记录每个 token 的实际长度（`token_lens`）
- 存储层对 key 做 binary-safe 比较（`memcmp` + 长度优先）
- 日志和 RESP 响应仍假设可打印字符串（对于二进制 key 可能输出乱码，但这不影响功能正确性）

#### 6.4 配置解析的字符串安全（`src/core/kvs_config.c`）

配置解析对字符串拷贝做了长度保护：

```c
snprintf(cfg->aof_filename, sizeof(cfg->aof_filename), "%s", "data/kvstore.aof");
snprintf(cfg->vector_index_type, sizeof(cfg->vector_index_type), "%s", "hnsw");
```

使用 `snprintf` + `sizeof(cfg->field)` 作为长度上限，不会溢出。

配置 key-value 解析时（第 123 行）使用 `strchr(line, '#')` 查找注释，用 `strtok(line, " \t\r\n")` 切割 token。

**风险提示**：`strtok` 非线程安全。但配置加载只在启动时单线程执行，所以这里没有问题。

#### 6.5 日志模块的字符串安全（`src/utils/kvs_log.c`）

日志格式化大量使用 `vsnprintf` 和 `snprintf`，并且对特殊字符做了转义处理（`kvs_log_append_escaped`），避免日志注入——例如日志中的 `"` 字符会被转义为 `\"`。

### 面试中可以这样说

"KVLite 在字符串安全方面做了几个关键处理。所有格式化输出统一用 snprintf 而非 sprintf，并且传 sizeof(buf) 作为长度上限。项目自己封装了 kvs_strdup 走统一内存池而非 libc 的 strdup+malloc，避免混用 free 和 kvs_free 导致的崩溃——代码里确实遇到过 strdup/free 混用的 bug，AOF rewrite 模块专门加了注释'不再混用'。对于 binary-safe key，RESP 解析层记录 token_lens 实际长度，存储层用 memcmp + 长度优先比较，确保二进制 key 能正确存取。日志模块对所有输出做了转义，避免日志注入。"

### 可能被追问

**Q: strncpy 为什么不一定安全？**
A: 如果源字符串长度 >= n，strncpy 不会自动补 `'\0'`，导致目标不是合法的 C 字符串。后续用 strlen/printf 等依赖 `'\0'` 的函数可能越界读。所以项目用 snprintf 代替了 strncpy。

**Q: snprintf 返回值表示什么？**
A: 返回值是"如果缓冲区足够大，会写入的字符数（不含 `'\0'`）"，不是实际写入的字符数。如果返回值 >= 缓冲区大小，说明输出被截断了。

**Q: strlen 为什么不适合二进制数据？**
A: strlen 遇到第一个 `'\0'` 就停。对于二进制 key（可能内部包含 `'\0'`），strlen 返回的长度会偏小。KVLite 用 RESP 协议层的 token_lens 传递真实长度。

**Q: strtok 为什么线程不安全？**
A: strtok 使用函数内部的 static 变量保存解析状态，多线程同时调用会互相覆盖。应该用可重入版本 strtok_r。

---

## 7. enum、状态机与协议解析

### 八股知识点

- **enum** 表达有限状态的集合，比宏常量有更好的可读性和调试体验
- **协议解析**天然适合状态机：数据可能分多次到达（半包），每次根据当前状态和输入决定下一个状态
- **C 中 enum 本质是整数**：没有强类型隔离，可以隐式转换为 int，不同 enum 之间可以互相赋值
- **switch + enum 要处理 default**：避免新增枚举值后遗漏处理导致未定义行为
- **状态机优势**：比散乱的 if-else 更容易维护和验证，新增状态只需扩展状态转移表

### KVLite 中的体现

#### 7.1 RESP 解析器状态机（`include/resp_protocol.h:43-51`）

```c
typedef enum{
    RESP_PARSE_INIT,        // 初始状态
    RESP_PARSE_ARRAY_LEN,   // 正在解析数组长度
    RESP_PARSE_BULK_LEN,    // 正在解析 bulk string 的 $ 前缀
    RESP_PARSE_BULK_LEN_NUM,// 正在解析 bulk string 的长度数字
    RESP_PARSE_BULK_DATA,   // 正在解析 bulk string 的数据体
    RESP_PARSE_COMPLETE     // 一条完整命令解析完成
} resp_parse_state;
```

这是典型的 RESP 协议解析状态机。RESP 协议中一条命令如 `*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n`，解析需要经过以下状态转移：

1. `INIT` → 读到 `*` → 进入 `ARRAY_LEN` 状态
2. `ARRAY_LEN` → 读到 `3\r\n` → 进入 `BULK_LEN` 状态（开始解析第一个元素）
3. `BULK_LEN` → 读到 `$` → 进入 `BULK_LEN_NUM` 状态
4. `BULK_LEN_NUM` → 读到 `3\r\n` → 进入 `BULK_DATA` 状态
5. `BULK_DATA` → 读 3 字节 `SET` + `\r\n` → 如果是最后一个元素就 `COMPLETE`，否则回到 `BULK_LEN`

解析器结构体（第 54-67 行）在状态之间保存中间结果：
```c
typedef struct {
    resp_parse_state state;
    int array_len;
    int arg_count;       // 已解析参数数
    int arg_index;       // 当前参数索引
    char **tokens;       // 解析出的 token 数组
    size_t *token_lens;  // 每个 token 的真实长度
    int bulk_len;        // 当前 bulk string 长度
    int bulk_read;       // 已读的 body 字节数
} resp_parser_t;
```

**半包处理**（`src/persistence/kvs_aof.c:446-464`）：
```c
int cmd_len = resp_peek_command(buf, buf_len, offset);
if (cmd_len < 0) {
    parse_errors++;      // 协议错误
    offset += 1;
    continue;
} else if (cmd_len == 0) {
    // 半包：当前命令不完整，需要更多数据
    if (feof(fp)) {
        cmd_len = (int)(buf_len - offset);  // EOF 时按整块处理
    } else {
        break;  // 等待更多数据
    }
}
```

AOF 重放时的 `resp_peek_command` 是一个无状态的快速探测器，在不消耗数据的情况下计算第一条完整命令占用的字节数——这是流式解析的经典需求。

#### 7.2 AOF rewrite 的状态流转

AOF rewrite 有隐式的状态机：
- `rewrite_in_progress = 0`：空闲
- `rewrite_in_progress = 1 && cow_snapshot_ready = 0`：准备开始，获取 COW 快照
- `rewrite_in_progress = 1 && cow_snapshot_ready = 1`：子进程正在运行，命令需进入 rewrite buffer
- `rewrite_in_progress = 0 && cow_snapshot_ready = 0`：完成或失败，清理

这些状态不是用 enum 表达的（用的是多个 int 字段组合），导致状态判断散布在多个 if 条件中：

```c
if(aof_state.rewrite_in_progress && aof_state.cow_snapshot_ready){
    // COW 模式下处理...
}
```

**改进建议**：可以用一个 `rewrite_state` enum（如 `REWRITE_IDLE/REWRITE_PREPARING/REWRITE_RUNNING/REWRITE_FINISHING`）让状态更明确，减少"组合爆炸式"的 if 判断。

#### 7.3 其他重要的 enum

**向量索引类型**（`include/kvs_vector.h:33`）：
```c
typedef enum kvs_vector_index_type_e {
    KVS_VECTOR_INDEX_FLAT = 0,
    KVS_VECTOR_INDEX_HNSW = 1,
    KVS_VECTOR_INDEX_SQ8_FLAT = 2,
} kvs_vector_index_type_t;
```

**AOF fsync 模式**（`include/kvs_config.h:46-49`）：NO / EVERYSEC / ALWAYS

**命令锁模式**（`include/kvs_command.h:11-15`）：KVS_CMD_LOCK_NONE / READ / WRITE

**同步模式**（`include/kvs_config.h:60-63`）：TCP / EBPF

**主从角色**（`include/kvs_config.h:40-43`）：MASTER / SLAVE

**分布式消息操作类型**（`include/master_slave_proto.h:8`）：MS_OP_ARRAY_SET / MS_OP_TTL_PEXPIREAT 等几十种操作类型——这是用 enum 表达协议消息类型的典型案例。

#### 7.4 命令分发中的 switch+enum

`kvs_operation.c` 中的命令参数解析对不同命令有不同的解析逻辑。虽然命令分发用的是 command_table 查找（函数指针表），但参数解析和数据结构路由仍有用到 switch。

### 面试中可以这样说

"KVLite 中状态机最典型的是 RESP 协议解析器。我实现了 6 个解析状态：INIT → ARRAY_LEN → BULK_LEN → BULK_LEN_NUM → BULK_DATA → COMPLETE。解析器结构体中保存当前状态和中间结果（bulk_len/bulk_read），支持半包处理——数据不够就先保存状态，下次数据到了从断点继续解析。AOF 重放时还有个无状态的 resp_peek_command 函数，快速探测一条完整命令占多少字节，用于从流式数据中切命令。

其他 enum 的应用包括：向量索引类型（flat/hnsw/sq8_flat）、AOF 同步模式、命令锁模式、主从同步操作类型等。这些 enum 让代码的可读性比裸魔法数字好很多，也比宏常量更容易在调试器中查看。"

### 可能被追问

**Q: enum 和宏常量相比有什么优势？**
A: enum 有符号名和类型名，调试器能显示枚举名而非数字。编译器可以给 switch+enum 做完整性检查（warning: enumeration value not handled in switch）。宏常量只是文本替换，没有这些能力。

**Q: C enum 类型安全吗？**
A: 不类型安全。C 中 enum 本质是 int，不同 enum 类型之间可以随意赋值不会报错。C++ 的 `enum class` 解决了这个问题，但 C 做不到。

**Q: 协议解析为什么适合状态机？**
A: 网络数据是流式的，一次 recv 可能收到半条或多条命令。状态机将"解析到哪了"编码为当前状态，每次收到新数据从当前状态继续，天然处理了分帧和粘包问题。

---

## 8. 编译链接：头文件、声明定义、静态库/动态库

### 八股知识点

- **声明 vs 定义**：声明告诉编译器"这个东西存在"，不分配存储空间；定义分配存储空间。一个变量/函数可以有多个声明，但只能有一个定义
- **include guard**（`#pragma once` 或 `#ifndef`）防止同一个头文件被多次包含导致重复定义
- **extern** 声明跨文件的全局变量，告诉链接器"这个符号在其他 .o 文件中定义"
- **static** 限制函数/变量的作用域为当前编译单元（.c 文件），对外不可见
- **编译-汇编-链接**：预处理 → 编译生成 .o → 链接将多个 .o + .a/.so 合并为可执行文件
- **静态库(.a) vs 动态库(.so)**：静态库在链接时合并到可执行文件；动态库在运行时加载，可被多个进程共享
- **常见链接错误**：
  - `multiple definition`：多个 .o 中对同一符号都有定义（通常是头文件中写了函数定义）
  - `undefined reference`：引用了声明但没有定义的符号（通常是忘记链接某个 .o 或 .a）

### KVLite 中的体现

#### 8.1 include guard 保护（`include/`）

项目混合使用了两种 include guard：
- **`#pragma once`**：大多数头文件使用（如 `kvs_types.h`、`kvs_config.h`、`kvs_vector.h` 等）
- **`#ifndef/#define` 经典宏**：部分使用（如 `resp_protocol.h`、`distributed.h`、`msg_queue.h`）

两种方式功能等价，`#pragma once` 更简洁但不是 C 标准的一部分（所有主流编译器都支持）。

#### 8.2 extern 全局变量声明（`include/kvs_globals.h`）

```c
extern kvs_config_t g_cfg;
extern pthread_rwlock_t g_kv_data_rwlock;
```

定义放在 `src/core/kvs_globals.c` 中。其他 .c 文件通过 include 头文件获取声明，链接时解析到 kvs_globals.o 中的定义。

四个数据结构的全局实例在各存储模块中定义（如 `kvs_array.c` 中定义 `kvs_array_t global_array`），在需要访问它们的 .c 文件中用 `extern` 声明。

**风险提示**：项目中 `extern` 声明散布在多处（`kvstore.c:21`、`kvs_aof.c:19`、`kvs_aof_rewrite.c:25` 等），每个 .c 文件各自写 `extern kvs_array_t global_array;`。这违反了 DRY 原则——如果 `global_array` 改名或改类型，需要修改所有 extern 声明处。更好的做法是统一在头文件中声明，或者在 `kvs_globals.h` 中集中管理。

#### 8.3 static 封装内部实现

`kvs_rdb.c` 中大量使用 `static` 函数隐藏实现细节：
```c
static int rdb_write_exact(FILE *fp, const void *buf, size_t size);
static int rdb_read_exact(FILE *fp, void *buf, size_t size);
static int rdb_write_u8(FILE *fp, uint8_t value);
// ... 所有序列化 helper 都是 static
```

这些函数只在本文件内使用，用 `static` 限制作用域避免了污染全局符号表，也向使用者明确"这不是公共 API"。

`kvs_aof_rewrite.c` 中的 `safe_write`、`write_command_parts`、`atomic_replace_file` 等都是 static。

AI Chat Schema（`include/kvs_ai_chat_schema.h:46`）中的辅助函数也被声明为 `static inline`，便于头文件中使用同时避免 multiple definition。

#### 8.4 编译系统（Makefile）

**多模块独立编译**：每个 .c 文件编译为 .o 文件，最后统一链接。

```makefile
SRCS = $(BASE_SRCS) $(SRC_DIST)/sync_backend_tcp.c
OBJS = $(SRCS:.c=.o)
$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) -lpthread
```

**第三方库链接**：
- **NtyCo**（协程库）：编译为静态库 `libntyco.a`（`ar rcs $@ $^`），链接时用 `-lntyco`
- **jemalloc**：链接静态库 `libjemalloc.a`
- **liburing**：动态链接（`-luring`）——io_uring 支持
- **libbpf**：动态链接（`-lbpf`）——eBPF 支持
- **pthread / dl / m**：系统库

**编译控制**：
- 通过 `NETWORK_SELECT` 宏选择网络后端（Reactor/Proactor/NtyCo）
- `-DSYNC_MODE=$(SYNC_MODE)` 控制同步模式
- 编译期特性开关（`ENABLE_ARRAY`、`ENABLE_AOF` 等）在 `kvs_config.h` 中定义

#### 8.5 eBPF 编译边界

eBPF 是最特殊的编译场景（Makefile 第 142-257 行）：

- **内核态**（`ebpf_kern.c`）：用 Clang 编译，target 为 BPF（`-target bpf`），生成 `ebpf_kern.o`（BPF 字节码）
- **用户态守护进程**（`ebpf_syncd`）：用 GCC 编译为标准 ELF，链接 libbpf 加载内核态字节码

这展示了 C 项目在"跨编译目标"方面的处理能力——同一套代码仓库，GCC 编译用户态服务，Clang 编译内核态 BPF 程序。

#### 8.6 AVX2 的编译隔离

`Makefile` 第 261-262 行：
```makefile
src/vector/kvs_vector_distance_avx2.o: src/vector/kvs_vector_distance_avx2.c
	$(CC) $(CFLAGS) $(AVX2_CFLAGS) -c $< -o $@
```

只有这一个文件编译时加 `-mavx2`，其他所有 .c 文件不加。这样做的好处：
1. 其他模块不依赖 AVX2 指令，可以在不支持 AVX2 的 CPU 上正常运行
2. AVX2 代码内部通过运行时 CPUID 检测选择路径，确保兼容性
3. 这是 SIMD 优化的标准工程实践

### 面试中可以这样说

"KVLite 是一个中等规模的多文件 C 工程。include 目录暴露公共接口，src 目录实现模块逻辑。头文件保护用 #pragma once。全局变量通过 extern 在头文件中声明、在单一 .c 文件中定义。内部辅助函数用 static 限制作用域，比如 RDB 模块的序列化 helpers、AOF rewrite 的 safe_write 都是 static，只在本文件内可见。

编译用 Makefile，每个 .c 编译为独立 .o，最后统一链接。链接了 NtyCo 和 jemalloc 两个静态库，以及 liburing、libbpf 等动态库。特殊处理包括：eBPF 内核态用 Clang + BPF target 编译，与用户态 GCC 完全隔开；AVX2 优化代码只对一个文件加 -mavx2 编译，其余文件保持标准指令集，通过运行时 CPUID 检测选择路径。这种编译隔离保证了可移植性。"

### 可能被追问

**Q: 声明和定义区别？**
A: 声明说"有这个变量/函数"，不分配空间。定义分配空间。`extern int x;` 是声明，`int x = 0;` 是定义。函数体 `{ }` 是定义，函数签名 `;` 结尾是声明。

**Q: 为什么头文件里普通函数定义会 multiple definition？**
A: 头文件被多个 .c include，每个 .c 编译成一个 .o，于是同一个函数在多个 .o 中都有定义。链接时链接器发现多个同名符号，报 multiple definition。加 `static` 或 `static inline` 可以让每个 .o 拥有自己的副本，或者只在 .c 中定义。

**Q: static 函数有什么作用？**
A: 限制符号可见性在当前编译单元，对外链接时不可见。好处是避免符号名冲突，也明确告诉使用者"这是内部实现，不要依赖"。

**Q: undefined reference 常见原因？**
A: 最常见是链接时漏了 .o 文件或库。也可能是指针/数组类型错误（改了 .h 中的类型但 .c 没重新编译），或是条件编译导致某些函数在链接时不存在。

---

## 9. 位运算、标志位与协议/配置开关

### 八股知识点

- **bitmask 模式**：`|=` 设置位，`&` 检测位，`&= ~` 清除位，`^` 翻转位
- **bitmask vs 多个 bool**：bitmask 节省内存（一个 int 可以存 32 个 flag），且可以通过一次 `&` 操作同时检查多个 flag
- **epoll events** 就是 bitmask：`EPOLLIN` (0x001)、`EPOLLOUT` (0x004)、`EPOLLERR` (0x008) 等可以组合使用
- **有符号数位移风险**：对有符号负数做右移是实现定义的行为；左移有符号数可能溢出导致 UB

### KVLite 中的体现

#### 9.1 epoll 事件标志位（`src/network/reactor.c`）

这是项目中最明显的位运算应用。`set_event` 函数（第 89 行）：

```c
int set_event(int fd, int event, int flag) {
    struct epoll_event ev;
    ev.events = event;           // event 是多个标志的 OR 组合
    ev.data.fd = fd;
    if (flag) {
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    } else {
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    }
}
```

调用时的事件组合：
- `set_event(fd, EPOLLIN | EPOLLET, 1)` — 添加：可读 + 边缘触发（第 167 行）
- `set_event(fd, EPOLLIN | EPOLLOUT, 0)` — 修改：可读 + 可写（第 381 行）
- `set_event(fd, EPOLLIN, 0)` — 修改：只监听可读，取消可写（第 405 行）

事件检测（第 544-554 行）：
```c
if (events[i].events & EPOLLIN) {         // 用 & 检测可读
    recv_callback(connfd);
}
if (events[i].events & EPOLLOUT) {        // 用 & 检测可写
    send_callback(connfd);
}
if (events[i].events & (EPOLLERR | EPOLLHUP)) {  // 同时检测两种错误
    event_unregister(connfd);
}
```

`EPOLLERR | EPOLLHUP` 用 `|` 组合成 mask，再用 `&` 与当前 events 做交集检测——如果任一错误位被设置，条件就成立。

#### 9.2 命令属性标志位

`command_info` 结构体（`include/kvs_command.h:43-50`）中有 `write_command` 字段和 `lock_mode` 字段：

```c
typedef struct {
    const char *name;
    command_handler handler;
    int arity;
    int write_command;          // 0=读命令, 1=写命令
    int data_structure;        // 哪种数据结构
    kvs_cmd_lock_mode_t lock_mode;  // 需要什么级别的锁
} command_info;
```

这里的 `write_command` 用的是 0/1 而非位标志，这是一个简单的二值属性。如果需要更多的命令属性（如：只读 | 可写 | 管理命令 | 阻塞命令 | 慢命令），可以用 bitmask 在一个字段中编码多种属性。目前项目没有这个需求，0/1 够用。

但 `lock_mode` 使用了一个专门的 enum（`KVS_CMD_LOCK_NONE/KVS_CMD_LOCK_READ/KVS_CMD_LOCK_WRITE`），这是更清晰的表达方式。

#### 9.3 eBPF 守护进程的事件掩码

`src/eBPF/ebpf_syncd.c` 中的 poll 事件处理（与 epoll 类似）：

```c
pfd.events = POLLIN;
if (send_buf->size > 0) {
    pfd.events |= POLLOUT;      // 用 |= 添加可写关注
}
// 检测
if (pfd.revents & (POLLERR | POLLHUP)) { ... }
```

#### 9.4 项目中位运算的其他使用

- **hash 计算**：`src/vector/kvs_vector_index.c:46` 的 `hash ^= ((hash << 5) + ch + (hash >> 2))` 使用了移位运算组合哈希
- **2 的幂计算**：`src/vector/kvs_vector_index.c:14-26` 的 `kvs_vector_next_pow2()` 使用 `n <<= 1` 循环左移求下一个 2 的幂，用于 id_map 的容量对齐
- **容量掩码**：`include/msg_queue.h:26` 的 `capacity_mask = capacity - 1`，用于 `index & capacity_mask` 快速取模（仅当 capacity 是 2 的幂时有效）
- **信号集**：`sigemptyset(&sa.sa_mask)` 等 sigset 操作——底层也是 bitmask

### 面试中可以这样说

"KVLite 中位运算最核心的应用在 epoll 事件管理。添加连接时注册 `EPOLLIN | EPOLLET`，收到数据需要发送响应时 MOD 为 `EPOLLIN | EPOLLOUT`，发完后回到 `EPOLLIN`。事件分发时用 `events & EPOLLIN` 检测可读、`events & (EPOLLERR | EPOLLHUP)` 检测异常断开。这种 bitmask 方式比多个 bool 变量更紧凑，也能在一次位运算中同时检查多个条件。

另外 msg_queue 用了 capacity_mask 快速取模——容量取 2 的幂后，`index & (cap-1)` 替代 `index % cap`，性能更好。向量模块的 hash 和 next_pow2 也用了移位运算优化。"

### 可能被追问

**Q: 如何判断某个 flag 是否存在？**
A: `flags & FLAG` —— 如果结果非零，说明 FLAG 被设置了。项目里 `events[i].events & EPOLLIN` 就是这么判断的。

**Q: `&` 和 `&&` 区别？**
A: `&` 是位与，对两个整数的每一位做 AND。`&&` 是逻辑与，判断两个表达式的真假值，并且有短路求值（左边为假就不计算右边）。`flags & FLAG` 必须用 `&` 不能用 `&&`。

**Q: 为什么 epoll events 可以同时包含 EPOLLIN 和 EPOLLOUT？**
A: 因为 epoll events 是 bitmask 设计。`EPOLLIN = 0x001`，`EPOLLOUT = 0x004`，二进制位不同，`EPOLLIN | EPOLLOUT = 0x005` 不冲突。内核在事件触发时也是按位检测——可读就设置 bit 0，可写就设置 bit 2。

---

## 10. 单元测试、压测与 C 项目调试：gdb/strace/perf

### 八股知识点

- **gdb**：运行时调试器，设置断点、查看变量、回溯调用栈。`gdb -batch -ex "bt full"` 可以做非交互式崩溃分析
- **core dump**：程序崩溃时操作系统保存的内存快照，用 gdb 加载可以事后定位崩溃位置
- **strace**：跟踪系统调用，能看到程序调了什么 syscall、返回值和耗时
- **perf**：Linux 性能分析工具，采样 CPU 热点函数，生成火焰图
- **ASan（AddressSanitizer）**：编译器插桩检测内存错误（use-after-free、buffer overflow、double-free 等），比 Valgrind 快但需要重新编译
- **assert**：开发期检查不变量，release 编译通常禁用。不应该用于处理线上可恢复的错误（因为禁用后这些检查会消失）
- **benchmark**：验证性能是否符合预期，关注吞吐（QPS）、延迟（P50/P99）、资源占用（CPU/内存）

### KVLite 中的体现

#### 10.1 测试体系

Makefile 中列举了 13 个测试/benchmark 可执行文件：

| 测试 | 用途 |
|------|------|
| `testcase` | 基础 Pipeline 功能测试 |
| `write_qps_test` | 写入 QPS 压测 |
| `write_ttl_qps_test` | 带 TTL 的写入压测 |
| `ttl_expired_path_bench` | TTL 过期删除路径基准测试 |
| `kvstore_client` | 交互式客户端（带 readline） |
| `mempool_test` | 内存池单元测试 |
| `test_paper` / `test_paper_sync` | 论文数据同步测试 |
| `test_bigvalue` | 大 value 边界测试 |
| `ai_chat_schema_test` | AI Chat 业务逻辑测试 |
| `vector_testcase` | 向量 CRUD 功能测试 |
| `vector_import_bench` | 向量导入性能基准 |
| `vector_benchmark` | 向量搜索性能基准 |

加上 shell 脚本（`bench_all.sh`、`bench_vector_ef_compare.sh`、`bench_vector_rw.sh`、`test_ttl_all_backends.sh` 等），覆盖了功能正确性、性能基准、边界条件、回归测试。

#### 10.2 ASan 使用记录

`docs/shutdown_segfault_fix_report.md` 详细记录了使用 ASan 排查 shutdown 段错误的过程：

```
| 测试 | ASan 结果 |
| test_binary_key | 36 passed, 0 failed, 无 sanitizer 错误 |
| mempool_test | PASS, 无 sanitizer 错误 |
| testcase mode 5 pipeline | PASS, qps=125,391, 无 sanitizer 错误 |
| 正常 shutdown (SIGTERM) | 干净退出, 无 sanitizer 错误 |
```

**ASan 定位的问题**：偶发 segfault，原因是在对象释放后继续调用（use-after-free）。通过 ASan 编译运行，成功捕获了释放后访问的堆栈。

#### 10.3 崩溃分析

`tests/repro_shutdown_segfault.sh` 第 226-228 行使用 gdb 自动提取 core dump 的调用栈：

```bash
# Try to get backtrace if gdb available
gdb -batch -ex "bt full" -ex "quit" "$KVSTORE_BIN" "$corefile" 2>&1 | head -60 > "${LOG_DIR}/backtrace_round_${round}.txt"
```

这个脚本做的是"循环启动 kvstore、并发读写、随机 SIGTERM，验证不 crash"的压力回归测试。

#### 10.4 性能分析

项目中有 `scripts/profile_benchmark_perf.sh` 用于 perf 性能分析。

文档中提到过使用 `strace` 查看系统调用模式、`perf` 定位 CPU 热点。

#### 10.5 日志级别体系

`include/kvs_log.h:10-15` 定义了四级日志：
```c
typedef enum {
    KVS_LOG_LEVEL_DEBUG = 0,
    KVS_LOG_LEVEL_INFO  = 1,
    KVS_LOG_LEVEL_WARN  = 2,
    KVS_LOG_LEVEL_ERROR = 3,
} kvs_log_level_t;
```

热路径上使用 `KVS_LOG_DEBUG`（release 关闭），错误路径使用 `KVS_LOG_ERROR_ERRNO`（带 errno）。日志模块还支持异步写入（`log_async` 配置项），避免日志 IO 阻塞主线程。

#### 10.6 assert 的使用

项目中只在 `resp_protocol.h` 和 `kvstore.h` 中 include 了 `<assert.h>`。assert 的使用非常克制——这符合"assert 用于开发期检查不变量，不用于线上错误处理"的原则。

**风险提示**：压测脚本中提到（`docs/shutdown_segfault_fix_report.md:134`）"ASan 使 kvstore 启动/关闭慢 3-5 倍，100 轮 ASan 压测预计需 2 小时以上"。这是 ASan 的固有开销，说明项目团队对工具的代价是有认知的。

### 面试中可以这样说

"KVLite 的测试和调试体系包括几个层面。功能层面有 10+ 个单元/集成测试覆盖四种数据结构、TTL、AOF/RDB 持久化、主从同步和向量检索。性能层面有 QPS 压测脚本，可以指定并发数和请求数，对比 KVLite 和 Redis 的差距并逐步缩小。调试方面，最有效的是 ASan 编译——之前碰到一个偶发 segfault，就是开 ASan 重新编译后抓到了 use-after-free 的堆栈。gdb + core dump 做崩溃后分析，strace 看系统调用，perf 找 CPU 热点。日志分四级，热路径用 DEBUG，错误用 ERROR_ERRNO 自动带 errno 信息。assert 只用于开发期不变量检查。"

### 可能被追问

**Q: gdb 和 ASan 定位问题有什么区别？**
A: gdb 是事后调试工具——崩溃了去看 core dump 或 attach 到进程。ASan 是预防性工具——编译时插桩，运行时在出现内存错误的第一时间报错，比崩溃后分析快得多。但 ASan 有 2-5 倍的性能开销。

**Q: strace 能看什么？**
A: 所有系统调用——open/read/write/close/epoll_wait/fork/fsync 等。排查"文件是否打开成功""write 返回了多少字节""为什么程序卡住了"这种问题时非常有用。

**Q: assert 可以用于线上错误处理吗？**
A: 不可以。assert 在 NDEBUG 宏定义时会被完全移除。线上错误（如文件打开失败、网络断开）应该用 if(err) return -1 这种正常错误处理路径。assert 只用于"理论上不应该发生"的不变量检查。

---

## 总结表格

| C 八股点 | KVLite 对应模块 | 核心代码位置 | 面试表达关键词 | 风险/不足 |
|---|---|---|---|---|
| 1. 错误处理 | 持久化、网络层 | `kvs_rdb.c:47,433` `reactor.c:340-352` `kvs_aof.c:37-176` `kvs_vector.c:678-701` | 返回码体系、goto cleanup、errno 快照、EINTR 重试 | AOF 初始化用 if-return 逐层清理而非 goto；成功路径偶有 KVS_LOG_ERROR_ERRNO 误用 |
| 2. 文件 IO | AOF、RDB | `kvs_aof.c:77,332-362` `kvs_aof_rewrite.c:91-128,575-597` `kvs_rdb.c:47-103,416-503` | fopen+fsync、rename 原子替换、safe_write 循环写、AOF 三种同步策略 | AOF 初始化混用了 strdup(malloc) 和 kvs_strdup；没有使用 fdatasync |
| 3. fork/COW | AOF Rewrite | `kvs_aof_rewrite.c:42-81,830-859,522-553,762-819` | fork+COW、rewrite buffer、临时文件 rename、三级超时终止、Proactor 降级为线程 | COW 内存放大无监控；多个 int 字段拼状态机不如 enum 清晰 |
| 4. 非阻塞 IO | Reactor/Proactor/NtyCo | `reactor.c:66-70,306-386,394-449,530-557` `ntyco.c:178-254` | ET 模式、循环读到 EAGAIN、EPOLLOUT 注册、MSG_NOSIGNAL、EINTR 重试 | 无显式单连接 Pipeline 命令数上限，长连接可能饿死其他连接 |
| 5. pthread 多线程 | 全局数据锁、AOF、TTL | `kvstore.c:16,94-118` `kvs_aof.c:97-150,622-648` `kvs_ttl_background.c:199` | 读写锁+重试升级、mutex+cond 生产者消费者、锁外 fsync 防死锁、TTL budget 限持锁时间 | 全局锁粒度较粗；msg_queue 的原子变量和 mutex 混用 |
| 6. 字符串安全 | 全局 | `kvs_aof.c:192-195` `kvs_mempool.c:156` `kvs_aof_rewrite.c:100` `kvs_log.c:119` | snprintf 全面替代 sprintf、kvs_strdup 统一内存、binary-safe token_lens、日志转义防注入 | 有 strdup/free 和 kvs_strdup/kvs_free 混用的历史遗留 |
| 7. enum/状态机 | RESP 解析、AOF Rewrite | `resp_protocol.h:43-67` `kvs_aof_rewrite.c:33-38` `kvs_vector.h:33` `kvs_config.h:46-63` | 6 状态 RESP 解析器、半包保留、操作类型 enum、向量索引 enum | AOF rewrite 状态用 int 组合而非 enum，状态判断分散 |
| 8. 编译链接 | Makefile、include、eBPF | `Makefile:1-265` `include/*.h` `kvs_rdb.c` 的 static 函数 | #pragma once、static 封装、extern 全局变量、NtyCo 静态库、eBPF 双目标编译、AVX2 编译隔离 | extern 声明散布多文件未集中管理 |
| 9. 位运算 | 网络层、msg_queue | `reactor.c:89-100,380-383,544-554` `msg_queue.h:26` `kvs_vector_index.c:14-26` | EPOLLIN/OUT bitmask 组合、capacity_mask 快速取模、hash 移位混合 | 位运算在项目中集中在网络层，其他模块使用较少 |
| 10. 测试调试 | tests/、docs/ | `Makefile:106-119` `shutdown_segfault_fix_report.md` `repro_shutdown_segfault.sh:226` | 13 个测试二进制、ASan 定位 use-after-free、gdb core dump、perf 热点、四级日志 | ASan 性能开销大不能用于长期压测；缺少 CI 自动化 |

---

## 2 分钟面试总述

（用于回答："除了昨天讲的函数指针、内存管理、memmove、二进制 key 这些点，你在 KVLite 里还体现了哪些 C 语言工程能力？"）

> KVLite 基础版本只有简单网络层和四种数据结构，后面逐步迭代了持久化、TTL、主从同步、eBPF 和向量检索。在这个过程中，我在以下几个方面有比较深的工程实践。
>
> **错误处理方面**，持久化模块统一用返回码体系（RDB_OK/RDB_ERR），多资源申请的函数用 goto cleanup 统一释放，RDB 保存函数写了两个清理标签处理"fp 开着"和"fp 已关闭"两种路径。网络层对 recv/send 的返回区分了"对端关闭"、"EAGAIN 非阻塞正常"、"EINTR 可重试"、"真正错误"四种情况，日志模块对 errno 先做快照再使用。
>
> **文件 IO 和持久化可靠性**，AOF 实现了三种 fsync 策略、后台定时刷盘线程、大命令直写优化。AOF rewrite 是核心工程难点——fork 子进程利用 COW 零拷贝生成数据快照，safe_write 循环写处理 short write 和 EINTR，rename 原子替换保证不出现半成品文件。Proactor 模式因为 io_uring 不兼容 fork，我做了降级用线程+读锁，这体现了工程上的务实判断。
>
> **非阻塞 IO 和 epoll**，Reactor 网络层用 epoll ET 模式，recv 循环读到 EAGAIN 保证不丢事件，send 遇到 EAGAIN 注册 EPOLLOUT 等内核通知而非忙等，用 MSG_NOSIGNAL 防止 SIGPIPE。三个网络后端对这一套规范保持一致。
>
> **多线程并发安全**，全局读写锁做 KV 命令保护，锁管理层实现了"释放读锁→获取写锁→重试执行"的安全升级机制，避免死锁。AOF 的 mutex+cond 生产者消费者模式、TTL 后台扫描的 budget 限持锁时间、锁外 fsync 防嵌套死锁——这些都是实际写代码时踩坑后积累的经验。
>
> **字符串安全**，全项目用 snprintf 替代 sprintf，kvs_strdup 统一内存管理避免 free/kvs_free 混用崩溃。RESP 解析层记录 token_lens 支持二进制 key，与 strlen 依赖 '\0' 的字符串路径做了隔离。
>
> **工程组织**，Makefile 分模块编译、include 目录暴露公共 API、static 封装内部实现、eBPF 用 Clang+BPF target 与用户态 GCC 隔开、AVX2 只对一个文件加 -mavx2 编译保证兼容性。
>
> **调试和验证**，ASan 编译定位 use-after-free 的段错误，gdb batch 模式自动提取 core dump 调用栈，perf 和 strace 定位性能瓶颈，10+ 个测试覆盖功能和压测场景。
