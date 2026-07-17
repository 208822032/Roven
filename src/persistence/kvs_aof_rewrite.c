

//处理AOF重写功能
#define KVS_VECTOR_INTERNAL
#include "kvs_api.h"
#include "kvs_aof_internal.h"
#include "kvs_command_executor.h"
#include "kvs_globals.h"
#include "kvs_log.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/*
Redis通过fork子进程+写时复制(COW)机制来解决父子进程同时操作数据结构的问题，
同时使用重写缓冲区来记录重写期间的新命令
*/


extern aof_state_t aof_state;

//COW快照保护 在fork前调用
static int acquire_cow_snapshot(void){
    if(!aof_state.rewrite_in_progress) return -1;

    // Proactor 运行时使用 snapshot + rewrite buffer，不进入 fork/COW 路径。
    if (g_cfg.network_mode == NETWORK_PROACTOR) {
        aof_state.cow_snapshot_ready = 0;
        KVS_LOG_INFO("aof", "Proactor mode: COW snapshot disabled");
        return 0;
    }

    //获取写锁阻塞新的写操作
    if(pthread_rwlock_wrlock(&aof_state.data_rwlock) != 0){
        KVS_LOG_ERROR_ERRNO("aof", "failed to acquire data write lock for COW snapshot");
        return -1;
    }

    //刷新所有待处理操作
    aof_fsync(1);

    //设置快照就绪标志
    aof_state.cow_snapshot_ready = 1;
    aof_state.snapshot_timestamp = time(NULL);

    // printf("COW snapshot acquired at timestamp: %ld\n", 
    //        aof_state.snapshot_timestamp);
    
    return 0;
}

//释放COW快照保护 在子进程fork后调用
static void release_cow_snapshot(void){
    if (g_cfg.network_mode == NETWORK_PROACTOR) {
        aof_state.cow_snapshot_ready = 0;
        return;
    }

    aof_state.cow_snapshot_ready = 0;
    pthread_rwlock_unlock(&aof_state.data_rwlock);
    // printf("COW snapshot released\n");
}

// 检查写操作是否应该进入重写缓冲区
int should_buffer_rewrite_command(void) {
    return aof_state.rewrite_in_progress && aof_state.cow_snapshot_ready;
}

//创建临时重写文件
static int create_rewrite_tempfile(void){
    char tmp_filename[256];
    snprintf(tmp_filename, sizeof(tmp_filename), "%s.tmp", aof_state.filename);
    int fd = open(tmp_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        KVS_LOG_ERROR_ERRNO("aof", "failed to create temporary AOF file: %s", tmp_filename);
        return -1;
    }

    //更新临时文件名
    pthread_mutex_lock(&aof_state.mutex);
    // 不再混用 strdup/free，统一内存管理
    if(aof_state.temp_filename){
        kvs_free(aof_state.temp_filename);
        aof_state.temp_filename = NULL;
    }
    aof_state.temp_filename = kvs_strdup(tmp_filename);
    pthread_mutex_unlock(&aof_state.mutex);

    return fd;
}

//子进程信号处理器
static void child_signal_handler(int sig){
    _exit(1);//直接退出子进程
}

//安全的文件写入
static int safe_write(int fd, const char *buf, size_t count){
    size_t written = 0;
    while(written < count){
        ssize_t n = write(fd, buf + written, count - written);
        if(n <= 0){
            if(n == -1 && errno == EINTR) continue;
            return -1;
        }
        written += n;
    }
    return 0;
}

//分块写入命令 
static int write_command_parts(int fd, const char *cmd_type, const char *key, const char *value){

    char buf[64];
    size_t key_len = strlen(key);
    size_t value_len = strlen(value);
    size_t cmd_len = strlen(cmd_type);

    // 1. 写入数组头: *3\r\n
    if (safe_write(fd, "*3\r\n", 4) == -1) return -1;
    
    // 2. 写入命令: $<cmd_len>\r\n<cmd>\r\n
    int n = snprintf(buf, sizeof(buf), "$%zu\r\n%s\r\n", cmd_len, cmd_type);
    if (safe_write(fd, buf, n) == -1) return -1;
    
    // 3. 写入 Key: $<key_len>\r\n<key>\r\n
    n = snprintf(buf, sizeof(buf), "$%zu\r\n", key_len);
    if (safe_write(fd, buf, n) == -1) return -1;
    if (safe_write(fd, key, key_len) == -1) return -1; // 直接写入 Key 内容
    if (safe_write(fd, "\r\n", 2) == -1) return -1;
    
    // 4. 写入 Value: $<value_len>\r\n<value>\r\n
    n = snprintf(buf, sizeof(buf), "$%zu\r\n", value_len);
    if (safe_write(fd, buf, n) == -1) return -1;
    if (safe_write(fd, value, value_len) == -1) return -1; // 直接写入 Value 内容
    if (safe_write(fd, "\r\n", 2) == -1) return -1;
    
    return 0;
}

static int write_command_parts_len(int fd,
                                   const char *cmd_type,
                                   size_t cmd_len,
                                   const char *key,
                                   size_t key_len,
                                   const char *value,
                                   size_t value_len) {
    char buf[64];

    if (safe_write(fd, "*3\r\n", 4) == -1) return -1;

    int n = snprintf(buf, sizeof(buf), "$%zu\r\n%s\r\n", cmd_len, cmd_type);
    if (safe_write(fd, buf, n) == -1) return -1;

    n = snprintf(buf, sizeof(buf), "$%zu\r\n", key_len);
    if (safe_write(fd, buf, n) == -1) return -1;
    if (safe_write(fd, key, key_len) == -1) return -1;
    if (safe_write(fd, "\r\n", 2) == -1) return -1;

    n = snprintf(buf, sizeof(buf), "$%zu\r\n", value_len);
    if (safe_write(fd, buf, n) == -1) return -1;
    if (safe_write(fd, value, value_len) == -1) return -1;
    if (safe_write(fd, "\r\n", 2) == -1) return -1;

    return 0;
}

static int write_ttl_command_if_needed(int fd, const char *key, int64_t expire_at_ms) {
    if (expire_at_ms < 0) {
        return 0;
    }

    char expire_buf[32];
    snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)expire_at_ms);
    return write_command_parts(fd, "PEXPIREAT", key, expire_buf);
}

static int write_bulk_len(int fd, const char *data, size_t len) {
    char header[64];
    int n;

    n = snprintf(header, sizeof(header), "$%zu\r\n", len);
    if (n < 0 || (size_t)n >= sizeof(header)) return -1;
    if (safe_write(fd, header, (size_t)n) == -1) return -1;

    if (len > 0 && data) {
        if (safe_write(fd, data, len) == -1) return -1;
    }

    if (safe_write(fd, "\r\n", 2) == -1) return -1;
    return 0;
}

static int write_vset_command_snapshot(
    int fd,
    const kvs_aof_rewrite_snapshot_vector_entry_t *rec
) {
    char header[32];
    int n;
    int scope_argc = (rec->scope_len > 0) ? 1 : 0;
    size_t argc = (size_t)(3 + scope_argc); /* VSET question answer [scope] */

    n = snprintf(header, sizeof(header), "*%zu\r\n", argc);
    if (n < 0 || (size_t)n >= sizeof(header)) return -1;
    if (safe_write(fd, header, (size_t)n) == -1) return -1;

    if (safe_write(fd, "$4\r\nVSET\r\n", 10) == -1) return -1;
    if (write_bulk_len(fd, rec->question, rec->question_len) == -1) return -1;
    if (write_bulk_len(fd, rec->answer, rec->answer_len) == -1) return -1;

    if (rec->scope_len > 0) {
        if (write_bulk_len(fd, rec->scope, rec->scope_len) == -1) return -1;
    }

    return 0;
}

static int write_vector_snapshot_to_tempfile(
    int fd,
    const kvs_aof_rewrite_snapshot_t *snapshot
) {
    size_t total_commands = 0;
    size_t i;

    if (!snapshot) {
        return -1;
    }

    if (snapshot->vector_count == 0) {
        return 0;
    }

    for (i = 0; i < snapshot->vector_count; ++i) {
        if (write_vset_command_snapshot(fd, &snapshot->vector_entries[i]) == -1) {
            return -1;
        }
        total_commands++;
    }

    KVS_LOG_INFO("aof", "AOF rewrite: wrote %zu VSET commands", total_commands);

    return (int)total_commands;
}

static int write_kv_snapshot_entries(
    int fd,
    const char *cmd,
    size_t cmd_len,
    const kvs_rdb_snapshot_kv_entry_t *entries,
    size_t count
) {
    size_t i;

    if (!entries && count > 0) {
        return -1;
    }
    for (i = 0; i < count; ++i) {
        if (write_command_parts_len(
                fd,
                cmd,
                cmd_len,
                entries[i].key,
                entries[i].key_len,
                entries[i].value,
                entries[i].value_len) == -1) {
            return -1;
        }
        if (write_ttl_command_if_needed(fd, entries[i].key, entries[i].expire_at_ms) == -1) {
            return -1;
        }
    }
    return 0;
}

static int write_snapshot_to_tempfile(
    int fd,
    const kvs_aof_rewrite_snapshot_t *snapshot
) {
    // 设置子进程信号处理
    struct sigaction sa;
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int total_commands = 0;

    // 检查文件描述符有效性
    if (fd < 0 || !snapshot) {
        KVS_LOG_ERROR("aof", "invalid file descriptor in child process");
        return -1;
    }

    if (write_kv_snapshot_entries(fd, "SET", 3, snapshot->array_entries, snapshot->array_count) == -1 ||
        write_kv_snapshot_entries(fd, "RSET", 4, snapshot->rbtree_entries, snapshot->rbtree_count) == -1 ||
        write_kv_snapshot_entries(fd, "HSET", 4, snapshot->hash_entries, snapshot->hash_count) == -1 ||
        write_kv_snapshot_entries(fd, "SSET", 4, snapshot->skiptable_entries, snapshot->skiptable_count) == -1) {
        KVS_LOG_ERROR_ERRNO("aof", "failed to write KV snapshot to temp file");
        return -1;
    }

    total_commands += (int)(snapshot->array_count +
                            snapshot->rbtree_count +
                            snapshot->hash_count +
                            snapshot->skiptable_count);

    {
        int vector_commands = write_vector_snapshot_to_tempfile(fd, snapshot);
        if (vector_commands < 0) {
            return -1;
        }
        total_commands += vector_commands;
    }

    // 最终同步到磁盘
    // if (fsync(fd) == -1) {
    //     perror("Final fsync failed in child process");
    //     return -1;
    // }

    return total_commands;
}


//等待子进程完成 包含超时机制
static int wait_for_child_with_timeout(pid_t pid, int timeout_secondes){
    int status;
    time_t start_time = time(NULL);

    while(1){
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == -1) {
            KVS_LOG_ERROR_ERRNO("aof", "waitpid failed");
            return -1;
        }else if(result == 0){
            // 子进程还在运行
            if (time(NULL) - start_time > timeout_secondes) {
                KVS_LOG_ERROR("aof", "AOF rewrite timeout, killing child process %d", pid);

                kill(pid, SIGTERM);

                // 给子进程一些时间退出
                sleep(1);
                if (waitpid(pid, &status, WNOHANG) == 0) {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                }
                return -1;
            }
            sleep(1); // 等待1秒再检查
        }else{
            // 子进程已退出
            return status;
        }
    }
}

//完成重写 将重写缓冲区的内容追加到新文件
static int finish_rewrite(int temp_fd){
    pthread_mutex_lock(&aof_state.rewrite_mutex);

    if (aof_state.rewrite_buf_len > 0) {
        // printf("AOF rewrite: appending %zu bytes from rewrite buffer\n", aof_state.rewrite_buf_len);

        if (safe_write(temp_fd, aof_state.rewrite_buf, aof_state.rewrite_buf_len) == -1) {
            KVS_LOG_ERROR_ERRNO("aof", "failed to write rewrite buffer to temp file");
            pthread_mutex_unlock(&aof_state.rewrite_mutex);
            return -1;
        }

        //清空重写缓冲区
        aof_state.rewrite_buf_len = 0;
    }
    pthread_mutex_unlock(&aof_state.rewrite_mutex);
    return 0;
}

//原子替换文件
static int atomic_replace_file(const char *temp_file, const char *final_file){
    char backup_file[1024];
    snprintf(backup_file, sizeof(backup_file), "%s.bak", final_file);

    // 首先备份原文件
    if (rename(final_file, backup_file) == -1 && errno != ENOENT) {
        KVS_LOG_ERROR_ERRNO("aof", "failed to backup original AOF file %s", final_file);
        return -1;
    }

    // 然后重命名临时文件
    if (rename(temp_file, final_file) == -1) {
        KVS_LOG_ERROR_ERRNO("aof", "failed to rename temp AOF file %s -> %s", temp_file, final_file);
        // 尝试恢复备份
        rename(backup_file, final_file);
        return -1;
    }

    // 删除备份文件
    unlink(backup_file);
    return 0;
}


// 重写完成后：关闭旧 fp 并重新打开新 AOF 文件（否则可能继续写到旧 inode，重启丢增量）
// 需要在持有 aof_state.mutex 的情况下调用
static int aof_reopen_after_rewrite_locked(void){
    if(aof_state.fp){
        fflush(aof_state.fp);
        fclose(aof_state.fp);
        aof_state.fp = NULL;
    }

    aof_state.fp = fopen(aof_state.filename, "a");
    if(!aof_state.fp){
        KVS_LOG_ERROR_ERRNO("aof", "failed to reopen AOF file after rewrite: %s", aof_state.filename);
        return -1;
    }

    fseek(aof_state.fp, 0, SEEK_END);
    long pos = ftell(aof_state.fp);
    if(pos < 0){
        KVS_LOG_ERROR_ERRNO("aof", "ftell after reopen AOF failed");
        return -1;
    }

    aof_state.current_size = (off_t)pos;
    aof_state.rewrite_base_size = aof_state.current_size;
    return 0;
}

//重写缓冲区管理
static int rewrite_buffer_append(int argc, char **argv){
    if(!aof_state.rewrite_in_progress || !aof_state.rewrite_buf) {
        return -1;  // 重写未进行或缓冲区未初始化
    }
    pthread_mutex_lock(&aof_state.rewrite_mutex);

    //计算需要的空间
    size_t needed = 0;
    needed += snprintf(NULL, 0, "*%d\r\n", argc);
    for(int i = 0;i < argc;i++){
        needed += snprintf(NULL, 0, "$%zu\r\n%s\r\n", strlen(argv[i]), argv[i]);
    }

    //检查缓冲区是否需要扩容
    if(aof_state.rewrite_buf_len + needed >= aof_state.rewrite_buf_size){
        size_t new_size = aof_state.rewrite_buf_size * 2;
        while(aof_state.rewrite_buf_len + needed >= new_size){
            new_size *= 2;
        }

        char *new_buf = kvs_realloc(aof_state.rewrite_buf, new_size);
        if(!new_buf){
            KVS_LOG_ERROR_ERRNO("aof", "failed to reallocate rewrite buffer");
            pthread_mutex_unlock(&aof_state.rewrite_mutex);
            return -1;
        }
        aof_state.rewrite_buf = new_buf;
        aof_state.rewrite_buf_size = new_size;
    }

    //将命令写入重写缓冲区
    char *buf_ptr = aof_state.rewrite_buf + aof_state.rewrite_buf_len;
    int len = snprintf(buf_ptr, aof_state.rewrite_buf_size - aof_state.rewrite_buf_len, "*%d\r\n", argc);
    for(int i = 0;i < argc;i++){
        len += snprintf(buf_ptr + len, aof_state.rewrite_buf_size - aof_state.rewrite_buf_len - len, 
            "$%zu\r\n%s\r\n", strlen(argv[i]), argv[i]);
    }
    aof_state.rewrite_buf_len += len;
    pthread_mutex_unlock(&aof_state.rewrite_mutex);
    return 0;
}

//在重写期间追加命令
void aof_rewrite_append_command(int argc, char **argv){
    if(!aof_state.rewrite_in_progress) return;

    if(rewrite_buffer_append(argc, argv) == -1){
        KVS_LOG_ERROR("aof", "failed to append command to rewrite buffer");
    }
}

//检测是否使用 proactor 模式
static int is_proactor_mode(void) {
    return g_cfg.network_mode == NETWORK_PROACTOR;
}

//线程方式的 AOF 重写（用于 Proactor 模式）
typedef struct aof_rewrite_thread_ctx_s {
    kvs_aof_rewrite_snapshot_t *snapshot;
} aof_rewrite_thread_ctx_t;

static void* aof_rewrite_thread(void *arg) {
    aof_rewrite_thread_ctx_t *ctx = (aof_rewrite_thread_ctx_t *)arg;
    // printf("AOF rewrite thread started (TID: %ld)\n", (long)pthread_self());
    
    // 创建临时文件
    int fd = create_rewrite_tempfile();
    if (fd == -1) {
        aof_state.rewrite_in_progress = 0;
        pthread_exit(NULL);
    }

    // 仅保护 AOF rewrite 自身状态，不保护 global store。
    if (is_proactor_mode() &&
        pthread_rwlock_rdlock(&aof_state.data_rwlock) != 0) {
        KVS_LOG_ERROR_ERRNO("aof", "rewrite thread: rdlock failed");
        close(fd);
        aof_state.rewrite_in_progress = 0;
        pthread_exit(NULL);
    }

    // 写入 snapshot 到临时文件
    if (!ctx || !ctx->snapshot || write_snapshot_to_tempfile(fd, ctx->snapshot) == -1) {
        if (is_proactor_mode()) {
            pthread_rwlock_unlock(&aof_state.data_rwlock);
        }
        close(fd);
        aof_state.rewrite_in_progress = 0;
        pthread_exit(NULL);
    }

    // 遍历完成后释放读锁
    if (is_proactor_mode()) {
        pthread_rwlock_unlock(&aof_state.data_rwlock);
    }

    // 确保数据刷到磁盘
    if (fsync(fd) == -1) {
        KVS_LOG_ERROR_ERRNO("aof", "fsync failed in rewrite thread");
        close(fd);
        aof_state.rewrite_in_progress = 0;
        pthread_exit(NULL);
    }

    close(fd);
    // printf("AOF rewrite thread completed\n");
    pthread_exit(NULL);
}

//后台重写函数
void aof_rewrite_background(void){
	if (!g_cfg.enable_aof || !g_cfg.aof_rewrite_enabled) {
		KVS_LOG_WARN("aof", "AOF rewrite requested while disabled");
		return;
	}
    if (aof_state.rewrite_in_progress) {
        // fprintf(stderr, "AOF rewrite already in progress\n");
        return;
    }

    // 初始化重写缓冲区
    pthread_mutex_lock(&aof_state.rewrite_mutex);
    if (!aof_state.rewrite_buf) {
        aof_state.rewrite_buf_size = AOF_BUFFER_SIZE;
        aof_state.rewrite_buf = kvs_malloc(aof_state.rewrite_buf_size);
        if (!aof_state.rewrite_buf) {
            pthread_mutex_unlock(&aof_state.rewrite_mutex);
            KVS_LOG_ERROR("aof", "failed to allocate rewrite buffer");
            return;
        }
    }
    aof_state.rewrite_buf_len = 0;
    pthread_mutex_unlock(&aof_state.rewrite_mutex);

    kvs_aof_rewrite_snapshot_t *snapshot = NULL;

    if (kvs_command_executor_build_aof_rewrite_snapshot(&snapshot) != 0 || !snapshot) {
        KVS_LOG_ERROR("aof", "failed to build AOF rewrite snapshot via executor");
        return;
    }

    // Proactor 模式使用线程而不是 fork，避免 io_uring 冲突
    if (is_proactor_mode()) {
        aof_rewrite_thread_ctx_t thread_ctx;
        // printf("Using thread-based AOF rewrite for Proactor mode\n");

        memset(&thread_ctx, 0, sizeof(thread_ctx));
        thread_ctx.snapshot = snapshot;

        // 设置重写进行中标志
        aof_state.rewrite_in_progress = 1;
        // 允许写命令进入重写缓冲区
        aof_state.cow_snapshot_ready = 1;
        
        pthread_t rewrite_tid;
        if (pthread_create(&rewrite_tid, NULL, aof_rewrite_thread, &thread_ctx) != 0) {
            KVS_LOG_ERROR_ERRNO("aof", "failed to create rewrite thread");
            aof_state.rewrite_in_progress = 0;
            aof_state.cow_snapshot_ready = 0;
            kvs_aof_rewrite_snapshot_destroy(snapshot);
            return;
        }
        
        // 等待线程完成
        void *thread_result;
        if (pthread_join(rewrite_tid, &thread_result) != 0) {
            KVS_LOG_ERROR_ERRNO("aof", "failed to join rewrite thread");
            aof_state.rewrite_in_progress = 0;
            aof_state.cow_snapshot_ready = 0;
            kvs_aof_rewrite_snapshot_destroy(snapshot);
            return;
        }
        kvs_aof_rewrite_snapshot_destroy(snapshot);
        
        // 追加重写期间的新命令
        int temp_fd = open(aof_state.temp_filename, O_WRONLY | O_APPEND);
        if (temp_fd == -1) {
            KVS_LOG_ERROR_ERRNO("aof", "failed to open temp file for appending: %s", aof_state.temp_filename);
            aof_state.rewrite_in_progress = 0;
            aof_state.cow_snapshot_ready = 0;
            return;
        }

        if (finish_rewrite(temp_fd) == -1) {
            close(temp_fd);
            aof_state.rewrite_in_progress = 0;
            return;
        }

        if (fsync(temp_fd) == -1) {
            KVS_LOG_ERROR_ERRNO("aof", "failed to fsync temp file");
            close(temp_fd);
            aof_state.rewrite_in_progress = 0;
            return;
        }

        close(temp_fd);

        // 原子替换文件
        if (atomic_replace_file(aof_state.temp_filename, aof_state.filename) == 0) {
            pthread_mutex_lock(&aof_state.mutex);
            // aof_state.rewrite_base_size = aof_state.current_size;
            (void)aof_reopen_after_rewrite_locked(); // 重新打开新文件
            pthread_mutex_unlock(&aof_state.mutex);
            // printf("AOF rewrite completed successfully (thread mode)\n");
        } else {
            KVS_LOG_ERROR("aof", "AOF rewrite failed during file replacement");
        }

        aof_state.cow_snapshot_ready = 0;
        aof_state.rewrite_in_progress = 0;
        return;
    }

    // Reactor/NtyCo 模式使用 fork + COW 机制
    // 获取 COW 快照保护
    aof_state.rewrite_in_progress = 1;
    if (acquire_cow_snapshot() != 0) {
        kvs_aof_rewrite_snapshot_destroy(snapshot);
        aof_state.rewrite_in_progress = 0;
        KVS_LOG_ERROR("aof", "failed to acquire COW snapshot");
        return;
    }

    pid_t childpid = fork();
    if(childpid == 0){
        // 子进程 - 拥有父进程数据的 COW 副本
        // printf("AOF rewrite child process started (PID: %d)\n", getpid());

        // 解除 COW 保护（子进程有自己的锁副本）
        pthread_rwlock_unlock(&aof_state.data_rwlock);

        //子进程
        int fd = create_rewrite_tempfile();
        if (fd == -1) {
            _exit(1);
        }

        // 写入数据到临时文件
        if (write_snapshot_to_tempfile(fd, snapshot) == -1) {
            close(fd);
            kvs_aof_rewrite_snapshot_destroy(snapshot);
            _exit(1);
        }

        // 确保数据刷到磁盘
        if (fsync(fd) == -1) {
            KVS_LOG_ERROR_ERRNO("aof", "fsync failed in child process");
            close(fd);
            _exit(1);
        }

        close(fd);
        kvs_aof_rewrite_snapshot_destroy(snapshot);
        // printf("AOF rewrite child process completed\n");
        _exit(0);
    }else if(childpid > 0){
        kvs_aof_rewrite_snapshot_destroy(snapshot);
        snapshot = NULL;
        // 父进程 - 释放快照保护，允许继续处理写操作
        release_cow_snapshot();

        //父进程
        aof_state.rewrite_child_pid = childpid;

        // printf("AOF rewrite started with pid %d\n", childpid);

        // 等待子进程完成（带超时）
        int status = wait_for_child_with_timeout(childpid, 300); // 5分钟超时

        if (status == -1) {
            KVS_LOG_ERROR("aof", "AOF rewrite failed due to timeout or error");
            aof_state.rewrite_in_progress = 0;
            return;
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            // 子进程成功完成，现在追加重写期间的新命令
            int temp_fd = open(aof_state.temp_filename, O_WRONLY | O_APPEND);

            if (temp_fd == -1) {
                KVS_LOG_ERROR_ERRNO("aof", "failed to open temp file for appending: %s", aof_state.temp_filename);
                aof_state.rewrite_in_progress = 0;
                return;
            }

            if (finish_rewrite(temp_fd) == -1) {
                close(temp_fd);
                aof_state.rewrite_in_progress = 0;
                return;
            }

            // 最终同步
            if (fsync(temp_fd) == -1) {
                KVS_LOG_ERROR_ERRNO("aof", "failed to fsync temp file");
                close(temp_fd);
                aof_state.rewrite_in_progress = 0;
                return;
            }

            close(temp_fd);

            //原子替换文件
            if (atomic_replace_file(aof_state.temp_filename, aof_state.filename) == 0) {
                //更新基准大小
                pthread_mutex_lock(&aof_state.mutex);
                // aof_state.rewrite_base_size = aof_state.current_size;
                (void)aof_reopen_after_rewrite_locked(); // 重新打开新文件
                pthread_mutex_unlock(&aof_state.mutex);

                // printf("AOF rewrite completed successfully\n");
            }else {
                KVS_LOG_ERROR("aof", "AOF rewrite failed during file replacement");
            }
        }else {
            KVS_LOG_ERROR("aof", "AOF rewrite failed in child process");
            // 清理临时文件
            unlink(aof_state.temp_filename);
        }

        aof_state.rewrite_in_progress = 0;
        aof_state.rewrite_child_pid = 0;
        aof_state.cow_snapshot_ready = 0; // 重置 COW 快照标志

    }else{
        //错误
        kvs_aof_rewrite_snapshot_destroy(snapshot);
        release_cow_snapshot();
        aof_state.rewrite_in_progress = 0;
        KVS_LOG_ERROR_ERRNO("aof", "fork failed for AOF rewrite");
    }
}

//停止重写
void aof_stop_rewrite(void) {
    if (aof_state.rewrite_in_progress && aof_state.rewrite_child_pid > 0) {
        KVS_LOG_INFO("aof", "Stopping AOF rewrite process %d", aof_state.rewrite_child_pid);
        kill(aof_state.rewrite_child_pid, SIGTERM);
        
        // 设置停止标志
        aof_state.stop = 1;
        
        // 等待子进程退出
        int status;
        waitpid(aof_state.rewrite_child_pid, &status, 0);
        
        // 清理临时文件
        if (aof_state.temp_filename) {
            unlink(aof_state.temp_filename);
        }
        
        aof_state.rewrite_in_progress = 0;
        aof_state.rewrite_child_pid = 0;
        aof_state.stop = 0;
    }
}

//重写缓冲区写入
static int aof_rewrite_buffer_write_unlocked(int fd){
    kvs_aof_rewrite_snapshot_t *snapshot = NULL;
    int ret;

    if (kvs_command_executor_build_aof_rewrite_snapshot(&snapshot) != 0 || !snapshot) {
        return -1;
    }
    ret = write_snapshot_to_tempfile(fd, snapshot);
    kvs_aof_rewrite_snapshot_destroy(snapshot);
    return ret;
}

int aof_rewrite_buffer_write(int fd){
    return aof_rewrite_buffer_write_unlocked(fd);
}
