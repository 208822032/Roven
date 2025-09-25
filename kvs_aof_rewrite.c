

//处理AOF重写功能
#include "kvstore.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
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
    kvs_free(aof_state.temp_filename);
    aof_state.temp_filename = strdup(tmp_filename);
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

//写入数据结构到文件 子进程专用
static int write_data_to_tempfile(int fd){
    //设置子进程信号处理
    struct sigaction sa;
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int total_commands = 0;

    #if ENABLE_ARRAY
    //使用内存映射来安全访问数组
    for(int i = 0;i < global_array.total; i++){
        //检查是否收到终止信号
        if(aof_state.stop) break;

        if(global_array.table[i].key && global_array.table[i].value){
            char cmd[2048];
            int len = snprintf(cmd, sizeof(cmd), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                            strlen(global_array.table[i].key), global_array.table[i].key,
                            strlen(global_array.table[i].value), global_array.table[i].value);
            if (safe_write(fd, cmd, len) == -1) {
                perror("Failed to write array data to temp file");
                return -1;
            }
            total_commands++;
        }
    }
    #endif

    #if ENABLE_RBTREE
    //使用迭代方式非递归 避免栈溢出
    if(global_rbtree.root != global_rbtree.nil){
        rbtree_node *stack[1024];
        int stack_ptr = 0;
        rbtree_node *current = global_rbtree.root;

        while (current != global_rbtree.nil || stack_ptr > 0) {
            while (current != global_rbtree.nil) {
                stack[stack_ptr++] = current;
                current = current->left;
            }
            current = stack[--stack_ptr];

            char cmd[2048];
            int len = snprintf(cmd, sizeof(cmd), "*3\r\n$4\r\nRSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                            strlen(current->key), current->key,
                            strlen((char*)current->value), (char*)current->value);
            if (safe_write(fd, cmd, len) == -1) {
                perror("Failed to write rbtree data to temp file");
                return -1;
            }
            total_commands++;

            current = current->right;
        }  
    }
    #endif

    #if ENABLE_HASH
    for (int i = 0; i < global_hash.max_slots; i++) {
        if (aof_state.stop) break;
        
        hashnode_t *node = global_hash.nodes[i];
        while (node) {
            char cmd[2048];
            int len = snprintf(cmd, sizeof(cmd), "*3\r\n$4\r\nHSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                            strlen(node->key), node->key,
                            strlen(node->value), node->value);
            
            if (safe_write(fd, cmd, len) == -1) {
                perror("Failed to write hash data to temp file");
                return -1;
            }
            total_commands++;
            node = node->next;
        }
    }
    #endif

    #if ENABLE_SKIPTABLE
    Node *current = global_skiptable.header->forward[0];
    while (current) {
        if (aof_state.stop) break;
        
        char cmd[2048];
        int len = snprintf(cmd, sizeof(cmd), "*3\r\n$4\r\nSSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                        strlen(current->key), current->key,
                        strlen(current->value), current->value);
        
        if (safe_write(fd, cmd, len) == -1) {
            perror("Failed to write skiplist data to temp file");
            return -1;
        }
        total_commands++;
        current = current->forward[0];
    }
    #endif

    printf("AOF rewrite: wrote %d commands from data structures\n", total_commands);
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
        printf("AOF rewrite: appending %zu bytes from rewrite buffer\n", aof_state.rewrite_buf_len);

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

//重写缓冲区管理
static int rewrite_buffer_append(int argc, char **argv){
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

//后台重写函数
void aof_rewrite_background(void){
    if (aof_state.rewrite_in_progress) {
        fprintf(stderr, "AOF rewrite already in progress\n");
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

    pid_t childpid = fork();
    if(childpid == 0){
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
        _exit(0);
    }else if(childpid > 0){
        //父进程
        aof_state.rewrite_child_pid = childpid;
        aof_state.rewrite_in_progress = 1;

        printf("AOF rewrite started with pid %d\n", childpid);

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
                aof_state.rewrite_base_size = aof_state.current_size;
                pthread_mutex_unlock(&aof_state.mutex);

                printf("AOF rewrite completed successfully\n");
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
            hashnode_t *node = global_hash.nodes[i];
            while (node) {
                char cmd[2048];
                int len = snprintf(cmd, sizeof(cmd), "*3\r\n$4\r\nHSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                                strlen(node->key), node->key,
                                strlen(node->value), node->value);
                
                if (write(fd, cmd, len) != len) {
                    perror("Failed to write to AOF temp file");
                    return -1;
                }
                total_commands++;
                node = node->next;
            }
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