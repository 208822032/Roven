

//处理AOF重写功能
#include "kvs_api.h"
#include "kvs_aof_internal.h"
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

#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif

#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif

#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif

#if ENABLE_SKIPTABLE
extern kvs_skiptable_t global_skiptable;
#endif

//COW快照保护 在fork前调用
static int acquire_cow_snapshot(void){
    if(!aof_state.rewrite_in_progress) return -1;

    // Proactor 模式下不使用 COW 机制，直接返回成功
    #if (NETWORK_SELECT == NETWORK_PROACTOR)
    aof_state.cow_snapshot_ready = 0;
    printf("Proactor mode: COW snapshot disabled\n");
    return 0;
    #endif

    //获取写锁阻塞新的写操作
    if(pthread_rwlock_wrlock(&aof_state.data_rwlock) != 0){
        perror("Failed to acquire data write lock for COW snapshot");
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
    #if (NETWORK_SELECT == NETWORK_PROACTOR)
    aof_state.cow_snapshot_ready = 0;
    return;
    #endif
    
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
        perror("Failed to create temporary AOF file");
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


//写入数据结构到文件 子进程专用
static int write_data_to_tempfile(int fd) {
    // 设置子进程信号处理
    struct sigaction sa;
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int total_commands = 0;
    time_t start_time = time(NULL);

    // 检查文件描述符有效性
    if (fd < 0) {
        fprintf(stderr, "Invalid file descriptor in child process\n");
        return -1;
    }

#if ENABLE_ARRAY
    // 创建数组的安全快照
    
    // 使用原子方式读取数组大小，避免在遍历过程中数组被修改
    int array_size = __sync_fetch_and_add(&global_array.total, 0); // 原子读取
    
    // 分配临时空间存储键值对指针，避免在写入过程中数据结构变化
    struct array_item_snapshot {
        char *key;
        char *value;
    } *array_snapshot = NULL;
    
    int valid_count = 0;
    
    // 第一步：收集所有有效的键值对指针
    if (array_size > 0) {
        array_snapshot = kvs_malloc(array_size * sizeof(struct array_item_snapshot));
        if (array_snapshot) {
            for (int i = 0; i < array_size; i++) {
                if (global_array.table[i].key && global_array.table[i].value) {
                    // 原子读取指针（避免编译器重排序）
                    char *key_ptr = __atomic_load_n(&global_array.table[i].key, __ATOMIC_ACQUIRE);
                    char *value_ptr = __atomic_load_n(&global_array.table[i].value, __ATOMIC_ACQUIRE);
                    
                    if (key_ptr && value_ptr) {
                        array_snapshot[valid_count].key = key_ptr;
                        array_snapshot[valid_count].value = value_ptr;
                        valid_count++;
                    }
                }
                
                // 超时检查
                if (time(NULL) - start_time > 290) {
                    fprintf(stderr, "Child process: Rewrite timeout approaching during array snapshot\n");
                    break;
                }
            }
        }
    }
    
    // 第二步：写入收集到的数据
    for (int i = 0; i < valid_count; i++) {
        if (time(NULL) - start_time > 290) {
            fprintf(stderr, "Child process: Rewrite timeout approaching during array write\n");
            break;
        }
        
        if(write_command_parts(fd, "SET", array_snapshot[i].key, array_snapshot[i].value) == -1){
            perror("Failed to write array data to temp file");
            if(array_snapshot) kvs_free(array_snapshot);
            return -1;
        }

        total_commands++;
    }
    
    if (array_snapshot) kvs_free(array_snapshot);
    //printf("Child process: Wrote %d array commands\n", valid_count);
#endif

#if ENABLE_RBTREE 
    if (global_rbtree.root != global_rbtree.nil) {
        // 使用栈进行非递归中序遍历
        rbtree_node *stack[1024];
        int stack_ptr = 0;
        rbtree_node *current = global_rbtree.root;
        int tree_commands = 0;

        while (current != global_rbtree.nil || stack_ptr > 0) {
            // 超时检查
            if (time(NULL) - start_time > 290) {
                fprintf(stderr, "Child process: Rewrite timeout approaching during rbtree processing\n");
                break;
            }
            
            while (current != global_rbtree.nil) {
                if (stack_ptr >= 1024) {
                    fprintf(stderr, "Stack overflow in rbtree traversal\n");
                    break;
                }
                stack[stack_ptr++] = current;
                current = current->left;
            }
            
            if (stack_ptr > 0) {
                current = stack[--stack_ptr];
                
                // 原子读取键值
                char *key_ptr = __atomic_load_n(&current->key, __ATOMIC_ACQUIRE);
                char *value_ptr = __atomic_load_n(&current->value, __ATOMIC_ACQUIRE);
                
                if (key_ptr && value_ptr) {
                    // 使用 write_command_parts 替代原有的 snprintf
                    if (write_command_parts(fd, "RSET", key_ptr, value_ptr) == -1) {
                        perror("Failed to write rbtree data to temp file");
                        return -1;
                    }
                    tree_commands++;
                    total_commands++;
                }
                
                current = current->right;
            }
        }
        //printf("Child process: Wrote %d rbtree commands\n", tree_commands);
    }
#endif

#if ENABLE_HASH
    //printf("Child process: Processing hash table snapshot...\n");
    int hash_commands = 0;
    for (int i = 0; i < global_hash.max_slots; i++) {
        if (time(NULL) - start_time > 290) {
            // fprintf(stderr, "Child process: Rewrite timeout approaching during hash processing\n");
            break;
        }

        hashnode_t *slot = &global_hash.nodes[i];
        if (slot->used != 1) {
            continue; // 只处理有效槽
        }

        // 直接使用 slot 中的指针/数组，避免拷贝到栈上的固定大小缓冲区
        // 这样可以支持任意长度的 Key/Value (前提是 slot->key/value 是合法的 C 字符串)
        char *key_ptr = slot->key;
        char *value_ptr = slot->value;

        if (key_ptr && value_ptr && key_ptr[0] != '\0' && value_ptr[0] != '\0') {
            if (write_command_parts(fd, "HSET", key_ptr, value_ptr) == -1) {
                perror("Failed to write hash data to temp file");
                return -1;
            }
            hash_commands++;
            total_commands++;
        }
    }
#endif
    //printf("Child process: Wrote %d hash commands\n", hash_commands);


#if ENABLE_SKIPTABLE
    //printf("Child process: Processing skiplist snapshot...\n");
    int skiplist_commands = 0;
    
    if (global_skiptable.header) {
        // 原子读取头节点的第一层指针
        Node *current = __atomic_load_n(&global_skiptable.header->forward[0], __ATOMIC_ACQUIRE);
        
        while (current) {
            // 超时检查
            if (time(NULL) - start_time > 290) {
                fprintf(stderr, "Child process: Rewrite timeout approaching during skiplist processing\n");
                break;
            }
            
            // 直接访问节点的键值
            if (current->key && current->value && current->key[0] != '\0' && current->value[0] != '\0') {
                // 使用 write_command_parts 替代原有的 snprintf
                if (write_command_parts(fd, "SSET", current->key, current->value) == -1) {
                    perror("Failed to write skiplist data to temp file");
                    return -1;
                }
                skiplist_commands++;
                total_commands++;
            }
            
            // 原子读取下一个节点
            current = __atomic_load_n(&current->forward[0], __ATOMIC_ACQUIRE);
        }
    }
    //printf("Child process: Wrote %d skiplist commands\n", skiplist_commands);
#endif

    //printf("Child process: Completed writing %d total commands from data snapshot\n", total_commands);
    
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
            perror("waitpid failed");
            return -1;
        }else if(result == 0){
            // 子进程还在运行
            if (time(NULL) - start_time > timeout_secondes) {
                fprintf(stderr, "AOF rewrite timeout, killing child process\n");

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
            perror("Failed to write rewrite buffer to temp file");
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
        perror("Failed to backup original AOF file");
        return -1;
    }

    // 然后重命名临时文件
    if (rename(temp_file, final_file) == -1) {
        perror("Failed to rename temp AOF file");
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
        perror("Failed to reopen AOF file after rewrite");
        return -1;
    }

    fseek(aof_state.fp, 0, SEEK_END);
    long pos = ftell(aof_state.fp);
    if(pos < 0){
        perror("ftell after reopen AOF");
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
            perror("Failed to reallocate rewrite buffer");
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
        fprintf(stderr, "Failed to append command to rewrite buffer\n");
    }
}

//检测是否使用 proactor 模式
static int is_proactor_mode(void) {
#if (NETWORK_SELECT == NETWORK_PROACTOR)
    return 1;
#else
    return 0;
#endif
}

//线程方式的 AOF 重写（用于 Proactor 模式）
static void* aof_rewrite_thread(void *arg) {
    // printf("AOF rewrite thread started (TID: %ld)\n", (long)pthread_self());
    
    // 创建临时文件
    int fd = create_rewrite_tempfile();
    if (fd == -1) {
        aof_state.rewrite_in_progress = 0;
        pthread_exit(NULL);
    }

#if (NETWORK_SELECT == NETWORK_PROACTOR)
    // 读锁保护遍历，避免并发释放导致段错误
    if (pthread_rwlock_rdlock(&aof_state.data_rwlock) != 0) {
        perror("rewrite thread: rdlock failed");
        close(fd);
        aof_state.rewrite_in_progress = 0;
        pthread_exit(NULL);
    }
#endif

    // 写入数据到临时文件
    if (write_data_to_tempfile(fd) == -1) {
#if (NETWORK_SELECT == NETWORK_PROACTOR)
        pthread_rwlock_unlock(&aof_state.data_rwlock);
#endif
        close(fd);
        aof_state.rewrite_in_progress = 0;
        pthread_exit(NULL);
    }

#if (NETWORK_SELECT == NETWORK_PROACTOR)
    // 遍历完成后释放读锁
    pthread_rwlock_unlock(&aof_state.data_rwlock);
#endif   

    // 确保数据刷到磁盘
    if (fsync(fd) == -1) {
        perror("fsync failed in rewrite thread");
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
            fprintf(stderr, "Failed to allocate rewrite buffer\n");
            return;
        }
    }
    aof_state.rewrite_buf_len = 0;
    pthread_mutex_unlock(&aof_state.rewrite_mutex);

    // 设置重写进行中标志
    aof_state.rewrite_in_progress = 1;

    // Proactor 模式使用线程而不是 fork，避免 io_uring 冲突
    if (is_proactor_mode()) {
        // printf("Using thread-based AOF rewrite for Proactor mode\n");

        // 允许写命令进入重写缓冲区
        aof_state.cow_snapshot_ready = 1;
        
        pthread_t rewrite_tid;
        if (pthread_create(&rewrite_tid, NULL, aof_rewrite_thread, NULL) != 0) {
            perror("Failed to create rewrite thread");
            aof_state.rewrite_in_progress = 0;
            return;
        }
        
        // 等待线程完成
        void *thread_result;
        if (pthread_join(rewrite_tid, &thread_result) != 0) {
            perror("Failed to join rewrite thread");
            aof_state.rewrite_in_progress = 0;
            return;
        }
        
        // 追加重写期间的新命令
        int temp_fd = open(aof_state.temp_filename, O_WRONLY | O_APPEND);
        if (temp_fd == -1) {
            perror("Failed to open temp file for appending");
            aof_state.rewrite_in_progress = 0;
            return;
        }

        if (finish_rewrite(temp_fd) == -1) {
            close(temp_fd);
            aof_state.rewrite_in_progress = 0;
            return;
        }

        if (fsync(temp_fd) == -1) {
            perror("Failed to fsync temp file");
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
            fprintf(stderr, "AOF rewrite failed during file replacement\n");
        }

        aof_state.cow_snapshot_ready = 0;
        aof_state.rewrite_in_progress = 0;
        return;
    }

    // Reactor/NtyCo 模式使用 fork + COW 机制
    // 获取 COW 快照保护
    if (acquire_cow_snapshot() != 0) {
        aof_state.rewrite_in_progress = 0;
        fprintf(stderr, "Failed to acquire COW snapshot\n");
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
        if (write_data_to_tempfile(fd) == -1) {
            close(fd);
            _exit(1);
        }

        // 确保数据刷到磁盘
        if (fsync(fd) == -1) {
            perror("fsync failed in child process");
            close(fd);
            _exit(1);
        }

        close(fd);
        // printf("AOF rewrite child process completed\n");
        _exit(0);
    }else if(childpid > 0){
        // 父进程 - 释放 COW 保护，允许继续处理写操作
        release_cow_snapshot();

        //父进程
        aof_state.rewrite_child_pid = childpid;
        aof_state.rewrite_in_progress = 1;

        // printf("AOF rewrite started with pid %d\n", childpid);

        // 等待子进程完成（带超时）
        int status = wait_for_child_with_timeout(childpid, 300); // 5分钟超时

        if (status == -1) {
            fprintf(stderr, "AOF rewrite failed due to timeout or error\n");
            aof_state.rewrite_in_progress = 0;
            return;
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            // 子进程成功完成，现在追加重写期间的新命令
            int temp_fd = open(aof_state.temp_filename, O_WRONLY | O_APPEND);

            if (temp_fd == -1) {
                perror("Failed to open temp file for appending");
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
                perror("Failed to fsync temp file");
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
                fprintf(stderr, "AOF rewrite failed during file replacement\n");
            }
        }else {
            fprintf(stderr, "AOF rewrite failed in child process\n");
            // 清理临时文件
            unlink(aof_state.temp_filename);
        }

        aof_state.rewrite_in_progress = 0;
        aof_state.rewrite_child_pid = 0;
        aof_state.cow_snapshot_ready = 0; // 重置 COW 快照标志

    }else{
        //错误
        perror("Fork failed for AOF rewrite");
    }
}

//停止重写
void aof_stop_rewrite(void) {
    if (aof_state.rewrite_in_progress && aof_state.rewrite_child_pid > 0) {
        printf("Stopping AOF rewrite process %d\n", aof_state.rewrite_child_pid);
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
int aof_rewrite_buffer_write(int fd){
    int total_commands = 0;

    #if ENABLE_ARRAY
        for (int i = 0; i < global_array.total; i++) {
            if (global_array.table[i].key && global_array.table[i].value) {
                char cmd[2048];
                int len = snprintf(cmd, sizeof(cmd), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                                strlen(global_array.table[i].key), global_array.table[i].key,
                                strlen(global_array.table[i].value), global_array.table[i].value);
                
                if (write(fd, cmd, len) != len) {
                    perror("Failed to write to AOF temp file");
                    return -1;
                }
                total_commands++;
            }
        }
    #endif

    #if ENABLE_RBTREE
        //中序遍历保存
        void write_rbtree_node(rbtree_node *node, void *data){
            int fd = *(int *)data;
            char cmd[2048];
            if (node == global_rbtree.nil) return;
            write_rbtree_node(node->left, &fd);
            int len = snprintf(cmd, sizeof(cmd), "*3\r\n$4\r\nRSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                            strlen(node->key), node->key,
                            strlen((char*)node->value), (char*)node->value);
            if (write(fd, cmd, len) != len) {
                perror("Failed to write to AOF temp file");
                exit(1); // 子进程中直接退出
            }
            write_rbtree_node(node->right, &fd);
        }
        write_rbtree_node(global_rbtree.root, &fd);
    #endif

    #if ENABLE_HASH
        for (int i = 0; i < global_hash.max_slots; i++) {
            hashnode_t *slot = &global_hash.nodes[i];
            if (slot->used != 1) {
                continue;
            }

            char cmd[2048];
            int len = snprintf(cmd, sizeof(cmd),
                               "*3\r\n$4\r\nHSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                               strlen(slot->key), slot->key,
                               strlen(slot->value), slot->value);

            if (write(fd, cmd, len) != len) {
                perror("Failed to write to AOF temp file");
                return -1;
            }
            total_commands++;
        }
    #endif

    #if ENABLE_SKIPTABLE
        Node *current = global_skiptable.header->forward[0];
        while (current) {
            char cmd[2048];
            int len = snprintf(cmd, sizeof(cmd), "*3\r\n$4\r\nSSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                            strlen(current->key), current->key,
                            strlen(current->value), current->value);
            if (write(fd, cmd, len) != len) {
                perror("Failed to write to AOF temp file");
                return -1;
            }
            total_commands++;
            current = current->forward[0];
        }
    #endif

    return 0;
}