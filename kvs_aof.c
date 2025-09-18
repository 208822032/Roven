#include "kvstore.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include "distributed.h"

aof_state_t aof_state;

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

extern distributed_config_t global_dist_config;

//创建管道
static int aof_create_pipe(void){
    if(pipe(aof_state.pipe_fd) == -1){
        perror("Failed to create pipe");
        return -1;
    }

    //设置非阻塞管道
    int flags = fcntl(aof_state.pipe_fd[0], F_GETFL, 0);
    fcntl(aof_state.pipe_fd[0], F_SETFL, flags | O_NONBLOCK);

    flags = fcntl(aof_state.pipe_fd[1], F_GETFL, 0);
    fcntl(aof_state.pipe_fd[1], F_SETFL, flags | O_NONBLOCK);

    return 0;
}

//关闭管道
static void aof_close_pipe(void){
    close(aof_state.pipe_fd[0]);
    close(aof_state.pipe_fd[1]);
}

//管道通信线程函数
static void *aof_pipe_thread_func(void *arg){
    int pipe_fd = aof_state.pipe_fd[1]; //写端
    char buffer[4096];
    ssize_t bytes_read;

    while(!aof_state.pipe_stop){
        pthread_mutex_lock(&aof_state.rewrite_mutex);

        //检查写缓冲区是否有数据
        if(aof_state.rewrite_buf_len > 0){
            //计算要发送的数据量
            size_t to_send = aof_state.rewrite_buf_len;
            if(to_send > sizeof(buffer)){
                to_send = sizeof(buffer);//是否需要优化
            }

            //复制数据到临时缓冲区
            memcpy(buffer, aof_state.rewrite_buf, to_send);

            //尝试通过管道发送数据
            ssize_t bytes_written = write(pipe_fd, buffer, to_send);

            if(bytes_written > 0){
                //成功发送数据 更新重写缓冲区
                memmove(aof_state.rewrite_buf, aof_state.rewrite_buf + bytes_written, aof_state.rewrite_buf_len - bytes_written);
                aof_state.rewrite_buf_len -= bytes_written;
            }else if(bytes_written == -1 && errno != EAGAIN && errno != EWOULDBLOCK){
                perror("Failed to write to pipe");
                break;
            }
        }

        pthread_mutex_unlock(&aof_state.rewrite_mutex);

        //休眠 避免cpu占用率过高
        usleep(10000);
    }

    return NULL;
}

//启动管道通信线程
static int aof_start_pipe_thread(void){
    aof_state.pipe_stop = 0;
    
    if(pthread_create(&aof_state.pipe_thread, NULL, aof_pipe_thread_func, NULL) != 0){
        perror("Failed to create pipe thread");
        return -1;
    }

    return 0;
}

//停止管道通信线程
static void aof_stop_pipe_thread(void){
    aof_state.pipe_stop = 1;
    pthread_join(aof_state.pipe_thread, NULL);
}

//初始化函数
int aof_init(const char *filename, int fsync_mode){

    // 验证同步模式参数
    if (fsync_mode != AOF_FSYNC_NO && 
        fsync_mode != AOF_FSYNC_EVERYSEC && 
        fsync_mode != AOF_FSYNC_ALWAYS) {
        fprintf(stderr, "Invalid AOF sync mode: %d\n", fsync_mode);
        return -1;
    }

    memset(&aof_state, 0, sizeof(aof_state));

    aof_state.filename = strdup(filename);
    if(!aof_state.filename){
        perror("Failed to allocate memory for AOF filename");
        return -1;
    }

    //分配AOF缓冲区
    aof_state.buf_size = AOF_BUFFER_SIZE;
    aof_state.buf = kvs_malloc(aof_state.buf_size);
    if(!aof_state.buf){
       perror("Failed to allocate AOF buffer");
       free(aof_state.filename);
       return -1;
    }

    //初始化临时文件名
    aof_state.temp_filename = kvs_malloc(strlen(filename) + 10);
    if(!aof_state.temp_filename){
        perror("Failed to allocate memory for temp filename");
        free(aof_state.filename);
        kvs_free(aof_state.buf);
        return -1;
    }
    sprintf(aof_state.temp_filename, "%s.tmp", filename);

    //打开或创建AOF文件
    aof_state.fp = fopen(filename, "a+");
    if(!aof_state.fp){
        perror("Failed to open AOF file");
        free(aof_state.filename);
        kvs_free(aof_state.temp_filename);
        kvs_free(aof_state.buf);
        return -1;
    }

    //获取当前文件大小
    fseek(aof_state.fp, 0, SEEK_END);
    aof_state.current_size = ftell(aof_state.fp);
    aof_state.rewrite_base_size = aof_state.current_size;
    rewind(aof_state.fp);

    //设置默认同步模式为每秒同步
    aof_state.fsync_mode = fsync_mode;
    aof_state.last_fsync = time(NULL);

    //初始化互斥锁和条件变量
    if(pthread_mutex_init(&aof_state.mutex, NULL) != 0){
        perror("Failed to initialize AOF mutex");
        fclose(aof_state.fp);
        free(aof_state.filename);
        kvs_free(aof_state.temp_filename);
        kvs_free(aof_state.buf);
        return -1;
    }

    if(pthread_cond_init(&aof_state.cond, NULL) != 0){
        perror("Failed to initialize AOF condition variable");
        pthread_mutex_destroy(&aof_state.mutex);
        fclose(aof_state.fp);
        free(aof_state.filename);
        kvs_free(aof_state.temp_filename);
        kvs_free(aof_state.buf);
        return -1;
    }

    //初始化重写缓冲区互斥锁
    if(pthread_mutex_init(&aof_state.rewrite_mutex, NULL) != 0){
        perror("Failed to initialize AOF rewrite mutex");
        // 清理已分配资源
        pthread_mutex_destroy(&aof_state.mutex);
        pthread_cond_destroy(&aof_state.cond);
        fclose(aof_state.fp);
        free(aof_state.filename);
        kvs_free(aof_state.temp_filename);
        kvs_free(aof_state.buf);
        return -1;
    }

    //创建后台同步线程
    aof_state.stop = 0;
    if(pthread_create(&aof_state.bgthread, NULL, (void*(*)(void *))aof_background_fsync, NULL) != 0){
        perror("Failed to create AOF background thread");
        pthread_mutex_destroy(&aof_state.mutex);
        pthread_cond_destroy(&aof_state.cond);
        fclose(aof_state.fp);
        free(aof_state.filename);
        kvs_free(aof_state.temp_filename);
        kvs_free(aof_state.buf);
        return -1;
    }

    const char *sync_mode_str;
    switch (fsync_mode) {
        case AOF_FSYNC_NO: sync_mode_str = "no"; break;
        case AOF_FSYNC_EVERYSEC: sync_mode_str = "everysec"; break;
        case AOF_FSYNC_ALWAYS: sync_mode_str = "always"; break;
        default: sync_mode_str = "unknown"; break;
    }

    //初始化管道停止标识
    aof_state.pipe_stop = 0;
    
    printf("AOF initialized: %s (%ld bytes), sync mode: %s\n", 
           filename, aof_state.current_size, sync_mode_str);

    return 0;
}

//追加函数
void aof_append(int argc, char **argv){
    if(!aof_state.fp || aof_state.rewrite_in_progress) return;

    pthread_mutex_lock(&aof_state.mutex);

    //构建RESP格式命令
    /*
    在aof_state的缓冲区中，从当前已用长度的末尾开始，写入格式为"*参数数量\r\n"的字符串（如*3\r\n），
    写入时限制在缓冲区剩余空间内，并将实际写入的字符数保存到len变量中。
    后续通常会用len更新aof_state.buf_len（如aof_state.buf_len += len），确保下一次写入从新的末尾开始。
    */
   
    int len = snprintf(aof_state.buf + aof_state.buf_len, aof_state.buf_size - aof_state.buf_len, "*%d\r\n", argc);
    for(int i = 0;i < argc;i++){
        len += snprintf(aof_state.buf + aof_state.buf_len + len, aof_state.buf_size - aof_state.buf_len - len, 
            "$%zu\r\n%s\r\n", strlen(argv[i]), argv[i]);
    }
    aof_state.buf_len += len;
    aof_state.current_size += len;

    //如果重写正在进行 同时将命令追加到重写缓冲区
    if(aof_state.rewrite_in_progress){
        pthread_mutex_lock(&aof_state.rewrite_mutex);

        //检查重写缓冲区是否需要扩容
        if(aof_state.rewrite_buf_len + len > aof_state.rewrite_buf_size){
            size_t new_size = aof_state.rewrite_buf_size * 2;
            if(new_size < aof_state.rewrite_buf_len + len){
                new_size = aof_state.rewrite_buf_size + len + AOF_BUFFER_SIZE;
            }

            char *new_buf = kvs_realloc(aof_state.rewrite_buf, new_size);
            if(new_size){
                aof_state.rewrite_buf = new_buf;
                aof_state.rewrite_buf_size = new_size;
            }else{
                perror("Failed to expand rewrite buffer");
                // 继续执行，但不追加到重写缓冲区
                pthread_mutex_unlock(&aof_state.rewrite_mutex);
                pthread_mutex_unlock(&aof_state.mutex);
                return;
            }
        }

        //将命令追加到重写缓冲区
        memcpy(aof_state.rewrite_buf + aof_state.rewrite_buf_len, aof_state.rewrite_buf + (aof_state.buf_len - len), len);
        aof_state.rewrite_buf_len += len;

        pthread_mutex_unlock(&aof_state.rewrite_mutex);
    }

    //检查是否需要立即刷新
    int should_fsync = 0;
    if(aof_state.fsync_mode == AOF_FSYNC_ALWAYS){
        //aof_fsync(1);  会导致死锁
        should_fsync = 1;
    }else if(aof_state.buf_len >= aof_state.buf_size/2){
        //缓冲区过半 唤醒后台线程
        pthread_cond_signal(&aof_state.cond);
    }

    //检查是否需要重写AOF
    //有无重写 对加载的时间
    int should_rewrite = 0;
    off_t growth = aof_state.current_size - aof_state.rewrite_base_size;
    if(aof_state.current_size >= AOF_REWRITE_MIN_SIZE &&
        growth >= AOF_REWRITE_MIN_GROWTH &&
        growth >= (aof_state.rewrite_base_size * AOF_REWRITE_PERCENTAGE/100)&&
        !aof_state.rewrite_in_progress){
            //aof_rewrite_background();//开始重写
            should_rewrite = 1;
        }

    pthread_mutex_unlock(&aof_state.mutex);

    // 在锁外执行可能阻塞的操作
    if (should_fsync) {
        aof_fsync(1);
    }
    if (should_rewrite) {
        aof_rewrite_background();
    }
    
    //主节点需要将命令分发给从节点
    if(global_dist_config.role == ROLE_MASTER){
        master_sync_command(argv[0], argv[1], (argc > 2) ? argv[2] : NULL);
    }
}

//同步函数
void aof_fsync(int force){
    if(!aof_state.fp || aof_state.buf_len == 0) return;

    pthread_mutex_lock(&aof_state.mutex);

    if (aof_state.buf_len == 0) {
        pthread_mutex_unlock(&aof_state.mutex);
        return;
    }

    size_t written = fwrite(aof_state.buf, 1, aof_state.buf_len, aof_state.fp);
    if(written != aof_state.buf_len){
        perror("Failed to write AOF buffer");
    }else{
        fflush(aof_state.fp);

        //根据同步模式决定是否调用fsync
        if(force || aof_state.fsync_mode == AOF_FSYNC_ALWAYS){
            int fd = fileno(aof_state.fp);
            if(fsync(fd) == -1){
                //fsync 将文件描述符 fd 对应的文件数据从操作系统内核缓冲区强制同步到物理磁盘
                perror("Failed to fsync AOF file");
            }
            aof_state.last_fsync = time(NULL);
        }
    }

    //重置缓冲区
    aof_state.buf_len = 0;

    pthread_mutex_unlock(&aof_state.mutex);
}

//重写函数
void aof_rewrite(void){
    if(aof_state.rewrite_in_progress){
        fprintf(stderr, "AOF rewrite already in progress\n");
        return;
    }

    aof_rewrite_background();
}

static int file_exists(const char *filename){

    struct stat buffer;
    return (stat(filename, &buffer) == 0);
}

//加载函数
int aof_load(const char *filename){
    if(!file_exists(filename)){
        printf("No AOF file found\n");
        return 0;
    }

    FILE *fp = fopen(filename, "r");
    if(!fp){
        perror("Failed to open AOF file for reading");
        return -1;
    }

    printf("Loading AOF file %s\n", filename);

    resp_parser_t parser;
    resp_parser_init(&parser);

    char buffer[4096];
    size_t total_read = 0;
    int commands_processed = 0;

    //设置sync_context为2 禁用AOF追加
    set_sync_context(2);

    while(!feof(fp)){
        //文件流中读取数据到缓冲区
        size_t read = fread(buffer, 1, sizeof(buffer), fp);
        if(read <= 0) break;

        size_t parsed = 0;

        while(parsed < read){
            int result = resp_parser(&parser, buffer + parsed, read - parsed);
            if(result > 0){
                parsed += result;

                if(parser.state == RESP_PARSE_COMPLETE){
                    //成功解析命令
                    char response[KVS_MAX_RESPONSE_SIZE];
                    kvs_filter_protocol(parser.tokens, parser.arg_index, response);

                    commands_processed++;
                    resp_parser_reset(&parser);
                }
            }else if(result == 0){
                //需要更多数据
                break;
            }else{
                //解析错误
                fprintf(stderr, "AOF file parse error at offset %ld\n", total_read + parsed);
                fclose(fp);
                resp_parser_free(&parser);
                return -1;
            }
        }

        total_read += read;
    }

    fclose(fp);
    resp_parser_free(&parser);

    //重置sync_context
    set_sync_context(0);

    printf("AOF load completed: %ld bytes processed, %d commands executed\n", 
           total_read, commands_processed);
    return 0;
}

//释放资源
void aof_free(void){
    if(aof_state.stop) return;

    //停止后台线程
    aof_state.stop = 1;
    pthread_cond_signal(&aof_state.cond);
    pthread_join(aof_state.bgthread, NULL);

    //如果重写正在进行 停止管道线程 并等待子进程结束
    if(aof_state.rewrite_in_progress){
        //停止管道通信线程
        aof_state.pipe_stop = 1;
        pthread_join(aof_state.pipe_thread, NULL);
        
        //关闭管道
        aof_close_pipe();

        // 等待子进程结束
        if(aof_state.rewrite_child_pid > 0) {
            printf("Waiting for AOF rewrite to finish...\n");
            int status;
            waitpid(aof_state.rewrite_child_pid, &status, 0);
        }
    }

    //最后进行一次同步
    aof_fsync(1);

    if (aof_state.fp) {
        fclose(aof_state.fp);
        aof_state.fp = NULL;
    }

    if (aof_state.filename) {
        free(aof_state.filename);
        aof_state.filename = NULL;
    }

    if (aof_state.temp_filename) {
        kvs_free(aof_state.temp_filename);
        aof_state.temp_filename = NULL;
    }

    if (aof_state.buf) {
        kvs_free(aof_state.buf);
        aof_state.buf = NULL;
    }

    // 释放重写缓冲区
    pthread_mutex_lock(&aof_state.rewrite_mutex);
    if (aof_state.rewrite_buf) {
        kvs_free(aof_state.rewrite_buf);
        aof_state.rewrite_buf = NULL;
        aof_state.rewrite_buf_size = 0;
        aof_state.rewrite_buf_len = 0;
    }
    pthread_mutex_unlock(&aof_state.rewrite_mutex);

    pthread_mutex_destroy(&aof_state.mutex);
    pthread_cond_destroy(&aof_state.cond);
    pthread_mutex_destroy(&aof_state.rewrite_mutex);
    
    memset(&aof_state, 0, sizeof(aof_state_t));
}

//后台同步线程函数
void aof_background_fsync(void *arg){
    while(!aof_state.stop){
        pthread_mutex_lock(&aof_state.mutex);
        
        //等待条件变量或超时
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;

        pthread_cond_timedwait(&aof_state.cond, &aof_state.mutex, &ts);

        //检查是否需要同步
        if(aof_state.buf_len > 0
            && (aof_state.fsync_mode == AOF_FSYNC_EVERYSEC 
            || aof_state.buf_len >= aof_state.buf_size/2)){
                //临时释放锁
                pthread_mutex_unlock(&aof_state.mutex);

                aof_fsync(0);

                pthread_mutex_lock(&aof_state.mutex);
            }
        
        pthread_mutex_unlock(&aof_state.mutex);
    }
}

//后台重写函数
#if 0
void aof_rewrite_background(void){
    pid_t childpid;

    if((childpid = fork()) == 0){
        //子进程
        int fd = aof_rewrite_tmpfile();
        if(fd == -1){
            exit(1);
        }

        //重写AOF文件
        if(aof_rewrite_buffer_write(fd) == -1){
            close(fd);
            exit(1);
        }

        close(fd);
        exit(0);
    }else if(childpid > 0){
        //父进程
        aof_state.rewrite_child_pid = childpid;
        aof_state.rewrite_in_progress = 1;

        //等待子进程完成
        int status;
        waitpid(childpid, &status, 0);

        if(WIFEXITED(status) && WEXITSTATUS(status) == 0){//判断子进程是否正常退出且退出状态为成功
            //重写成功 替换原文件
            if(rename(aof_state.temp_filename, aof_state.filename) != 0){
                perror("Failed to rename AOF file");
            }else{
                //更新基准大小
                aof_state.rewrite_base_size = aof_state.current_size;
            }
        }else{
            fprintf(stderr, "AOF rewrite failed\n");
        }

        aof_state.rewrite_in_progress = 0;
    }else{
        perror("Failed to fork for AOF rewrite");
    }
}
#else
void aof_rewrite_background(void){
    if(aof_state.rewrite_in_progress){
        fprintf(stderr, "AOF rewrite already in progress\n");
        return;
    }

    //初始化重写缓冲区
    pthread_mutex_lock(&aof_state.rewrite_mutex);
    aof_state.rewrite_buf_size = AOF_BUFFER_SIZE;
    aof_state.rewrite_buf = kvs_malloc(aof_state.rewrite_buf_size);
    if(!aof_state.rewrite_buf){
        perror("Failed to allocate rewrite buffer");
        pthread_mutex_unlock(&aof_state.rewrite_mutex);
        return;
    }
    aof_state.rewrite_buf_len = 0;
    pthread_mutex_unlock(&aof_state.rewrite_mutex);

    //创建管道
    if(aof_create_pipe() == -1){
        //清理重写缓冲区
        pthread_mutex_lock(&aof_state.rewrite_mutex);
        kvs_free(aof_state.rewrite_buf);
        aof_state.rewrite_buf = NULL;
        aof_state.rewrite_buf_size = 0;
        aof_state.rewrite_buf_len = 0;
        pthread_mutex_unlock(&aof_state.rewrite_mutex);
        return;
    }

    pid_t childpid = fork();
    if(childpid == 0){
        //子进程 执行重写
        //关闭管道写端
        close(aof_state.pipe_fd[1]);

        int fd = aof_rewrite_tmpfile();
        if(fd == -1) exit(1);

        //重写当前数据快照
        if(aof_rewrite_buffer_write(fd) == -1){
            close(fd);
            exit(1);
        }

        //从管道读取增量命令 并写入文件
        char buffer[4096];
        ssize_t bytes_read;

        while((bytes_read = read(aof_state.pipe_fd[0], buffer, sizeof(buffer))) > 0){
            if(write(fd, buffer, bytes_read) != bytes_read){
                perror("Failed to write to AOF temp file from pipe");
                close(fd);
                exit(1);
            }
        }

        //关闭管道端和文件
        close(aof_state.pipe_fd[0]);
        close(fd);
        exit(0);
    }else if(childpid > 0){
        //父进程
        //关闭管道的读端
        close(aof_state.pipe_fd[0]);

        aof_state.rewrite_child_pid = childpid;
        aof_state.rewrite_in_progress = 1;

        //启动管道通信线程
        if(aof_start_pipe_thread() == -1){
            //清理资源
            aof_close_pipe();
            aof_state.rewrite_in_progress = 0;
            aof_state.rewrite_child_pid = 0;
            
            pthread_mutex_lock(&aof_state.rewrite_mutex);
            kvs_free(aof_state.rewrite_buf);
            aof_state.rewrite_buf = NULL;
            aof_state.rewrite_buf_size = 0;
            aof_state.rewrite_buf_len = 0;
            pthread_mutex_unlock(&aof_state.rewrite_mutex);

            return;
        }

        //printf("AOF rewrite started with pid %d\n", childpid);
    }else{
        perror("Failed to fork for AOF rewrite");
        // 清理重写缓冲区
        pthread_mutex_lock(&aof_state.rewrite_mutex);
        kvs_free(aof_state.rewrite_buf);
        aof_state.rewrite_buf = NULL;
        aof_state.rewrite_buf_size = 0;
        aof_state.rewrite_buf_len = 0;
        pthread_mutex_unlock(&aof_state.rewrite_mutex);
    }
}
#endif

//创建临时重写文件
int aof_rewrite_tmpfile(void){
    int fd = open(aof_state.temp_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd == -1){
        perror("Failed to create temporary AOF file");
        return -1;
    }

    return fd;
}

//重写缓冲区写入
#if 0
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

        // 中序遍历红黑树并写入SET命令
        #if 0
        void write_rbtree_node(rbtree_node *node, void *data) {
            int fd = *(int *)data;
            char cmd[2048];
            int len = snprintf(cmd, sizeof(cmd), "*3\r\n$4\r\nRSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                            strlen(node->key), node->key,
                            strlen((char*)node->value), (char*)node->value);
            
            if (write(fd, cmd, len) != len) {
                perror("Failed to write to AOF temp file");
                exit(1); // 子进程中直接退出
            }
        }
        rbtree_traversal_with_callback(&global_rbtree, global_rbtree.root, write_rbtree_node, &fd);
        #endif
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

    //printf("AOF rewrite: wrote %d commands to temp file\n", total_commands);
    return 0;
}
#else
int aof_rewrite_buffer_write(int fd){
    int total_commands = 0;

    //保存当前时间 用于记录重写开始的时间点

    //使用写时复制机制遍历数据结构
    //在避免阻塞父进程的基础上 同时保持数据一致性
    #if ENABLE_ARRAY
    //创建数组副本进行遍历
    kvs_array_t array_copy;
    memcpy(&array_copy, &global_array, sizeof(kvs_array_t));

    for(int i = 0;i < array_copy.total;i++){
        if(array_copy.table[i].key && array_copy.table[i].value){
            char cmd[2048];
            int len = snprintf(cmd, sizeof(cmd), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                strlen(array_copy.table[i].key), array_copy.table[i].key,
                strlen(array_copy.table[i].value), array_copy.table[i].value);
            if(write(fd, cmd, len) != len){
                perror("Failed to write to AOF temp file");
                return -1;
            }
            total_commands++;
        }
    }
    #endif

    //rbtree
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

    //hash
    #if ENABLE_HASH
    //遍历哈希表
    for(int i = 0;i < global_hash.max_slots;i++){
        hashnode_t *node = global_hash.nodes[i];
        while(node){
            char cmd[2048];
            int len = snprintf(cmd, sizeof(cmd), "*3\r\n$4\r\nHSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                strlen(node->key), node->key,
                strlen(node->value), node->value);
            if(write(fd, cmd, len) != len){
                perror("Failed to write to AOF temp file");
                return -1;
            }

            total_commands++;
            node = node->next;
        }
    }
    #endif

    #if ENABLE_SKIPTABLE
    //遍历跳表
    Node *current = global_skiptable.header->forward[0];
    while(current){
        char cmd[2048];
        int len = snprintf(cmd, sizeof(cmd), "*3\r\n$4\r\nSSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
            strlen(current->key), current->key,
            strlen(current->value), current->value);
        if(write(fd, cmd, len) != len){
            perror("Failed to write to AOF temp file");
            return -1;
        }
        total_commands++;
        current = current->forward[0];
    }
    #endif
    //printf("AOF rewrite: wrote %d commands to temp file\n", total_commands);
    return 0;
}
#endif

//重写完成处理函数
void aof_rewrite_done_handler(int sig){
    int status;
    pid_t pid;

    //等待子进程退出
    while((pid = waitpid(-1, &status, WNOHANG)) > 0){
        if(pid == aof_state.rewrite_child_pid){
            pthread_mutex_lock(&aof_state.mutex);

            //停止管道通信线程
            aof_stop_pipe_thread();

            //关闭管道
            aof_close_pipe();

            if(WIFEXITED(status) && WEXITSTATUS(status) == 0){
                //重写成功 替换原文件
                if(rename(aof_state.temp_filename, aof_state.filename) != 0){
                    perror("Failed to rename AOF file");
                }else{
                    // 更新基准大小
                    aof_state.rewrite_base_size = aof_state.current_size;
                    printf("AOF rewrite completed successfully\n");
                }
            }else{
                fprintf(stderr, "AOF rewrite failed\n");
            }

            pthread_mutex_unlock(&aof_state.mutex);
        }

        //清理重写缓冲区
        pthread_mutex_lock(&aof_state.rewrite_mutex);
        if(aof_state.rewrite_buf){
            kvs_free(aof_state.rewrite_buf);
            aof_state.rewrite_buf = NULL;
            aof_state.rewrite_buf_size = 0;
            aof_state.rewrite_buf_len = 0;
        }
        pthread_mutex_unlock(&aof_state.rewrite_mutex);

        aof_state.rewrite_in_progress = 0;
        aof_state.rewrite_child_pid = 0;

        pthread_mutex_unlock(&aof_state.mutex);
    }
}