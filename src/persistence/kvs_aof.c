#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include "kvs_api.h"
#include "kvs_aof_internal.h"
#include "resp_protocol.h"
#include "distributed.h"




//负责AOF初始化 追加命令 同步和加载功能

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

//初始化函数
int aof_init(const char *filename, int fsync_mode){
    // 先将整体状态清零，并标记为未初始化
    memset(&aof_state, 0, sizeof(aof_state));
    aof_state.initialized = 0;

    // 验证同步模式参数
    if (fsync_mode != AOF_FSYNC_NO && 
        fsync_mode != AOF_FSYNC_EVERYSEC && 
        fsync_mode != AOF_FSYNC_ALWAYS) {
        fprintf(stderr, "Invalid AOF sync mode: %d\n", fsync_mode);
        return -1;
    }

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

    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&aof_state.mutex, NULL) != 0) {
        perror("Failed to initialize AOF mutex");
        fclose(aof_state.fp);
        free(aof_state.filename);
        kvs_free(aof_state.temp_filename);
        kvs_free(aof_state.buf);
        return -1;
    }

    if (pthread_cond_init(&aof_state.cond, NULL) != 0) {
        perror("Failed to initialize AOF condition variable");
        pthread_mutex_destroy(&aof_state.mutex);
        fclose(aof_state.fp);
        free(aof_state.filename);
        kvs_free(aof_state.temp_filename);
        kvs_free(aof_state.buf);
        return -1;
    }

    // 初始化重写互斥锁
    if (pthread_mutex_init(&aof_state.rewrite_mutex, NULL) != 0) {
        perror("Failed to initialize AOF rewrite mutex");
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
        pthread_mutex_destroy(&aof_state.rewrite_mutex);
        pthread_mutex_destroy(&aof_state.mutex);
        pthread_cond_destroy(&aof_state.cond);
        fclose(aof_state.fp);
        free(aof_state.filename);
        kvs_free(aof_state.temp_filename);
        kvs_free(aof_state.buf);
        return -1;
    }

    // 初始化 COW 机制
    if (pthread_rwlock_init(&aof_state.data_rwlock, NULL) != 0) {
        perror("Failed to initialize data RW lock");
        aof_state.stop = 1;
        pthread_cond_signal(&aof_state.cond);
        pthread_join(aof_state.bgthread, NULL);
        pthread_mutex_destroy(&aof_state.rewrite_mutex);
        pthread_mutex_destroy(&aof_state.mutex);
        pthread_cond_destroy(&aof_state.cond);
        fclose(aof_state.fp);
        free(aof_state.filename);
        kvs_free(aof_state.temp_filename);
        kvs_free(aof_state.buf);
        return -1;
    }
    
    aof_state.cow_enabled = 1;
    aof_state.cow_snapshot_ready = 0;
    aof_state.snapshot_timestamp = 0;

    const char *sync_mode_str;
    switch (fsync_mode) {
        case AOF_FSYNC_NO: sync_mode_str = "no"; break;
        case AOF_FSYNC_EVERYSEC: sync_mode_str = "everysec"; break;
        case AOF_FSYNC_ALWAYS: sync_mode_str = "always"; break;
        default: sync_mode_str = "unknown"; break;
    }
    
        printf("AOF initialized: %s (%ld bytes), sync mode: %s\n", 
            filename, aof_state.current_size, sync_mode_str);

        // 标记为已初始化
        aof_state.initialized = 1;
        aof_state.loading = 0;
        return 0;
}

//追加函数
void aof_append(int argc, char **argv){
    if(!aof_state.fp) return;
    if(!aof_state.initialized) return;
    if(aof_state.loading) return; //正在加载AOF时不记录

    // Proactor 模式下禁用 COW 保护机制，避免阻塞异步 I/O
    #if (NETWORK_SELECT != NETWORK_PROACTOR)
    //COW保护 如果正在重写 添加COW保护
    if(aof_state.rewrite_in_progress && aof_state.cow_snapshot_ready){
        if(pthread_rwlock_rdlock(&aof_state.data_rwlock) == 0){
            pthread_mutex_lock(&aof_state.mutex);

            int len = snprintf(aof_state.buf + aof_state.buf_len, aof_state.buf_size - aof_state.buf_len, "*%d\r\n", argc);
            for(int i = 0;i < argc;i++){
                len += snprintf(aof_state.buf + aof_state.buf_len + len, aof_state.buf_size - aof_state.buf_len - len, 
                    "$%zu\r\n%s\r\n", strlen(argv[i]), argv[i]);
            }
            aof_state.buf_len += len;
            aof_state.current_size += len;

            pthread_mutex_unlock(&aof_state.mutex);
            
            //添加到重写缓冲区
            aof_rewrite_append_command(argc, argv);

            pthread_rwlock_unlock(&aof_state.data_rwlock);
        }
        return; //COW模式下提前返回
    }
    #endif
    // Proactor 重写期间：只写入重写缓冲区，避免与快照竞争
    #if (NETWORK_SELECT == NETWORK_PROACTOR)
    if (aof_state.rewrite_in_progress) {
        aof_rewrite_append_command(argc, argv);
        return;
    }
    #endif

    /* 预估本次命令写入长度，用于判断是否走直写路径 */
    size_t estimated_len = 0;
    estimated_len += 32; // 粗略预留数组头和换行
    for (int i = 0; i < argc; i++) {
        size_t arglen = strlen(argv[i]);
        estimated_len += 32;      // "$%zu\r\n" 等元数据开销
        estimated_len += arglen;  // 实际数据
        estimated_len += 2;       // 末尾 "\r\n"
    }

    /* 如果单条命令过大，直接写入文件，绕过内存缓冲区 */
    if (estimated_len > aof_state.buf_size / 2) {
        //先刷盘
        aof_fsync(0);

        pthread_mutex_lock(&aof_state.mutex);
        FILE *fp = aof_state.fp;
        if (!fp) {
            pthread_mutex_unlock(&aof_state.mutex);
            return;
        }

        /* 直接向文件写 RESP 编码，避免占用大缓冲区 */
        if (fprintf(fp, "*%d\r\n", argc) < 0) {
            perror("Failed to write big AOF command header");
            pthread_mutex_unlock(&aof_state.mutex);
            return;
        }
        for (int i = 0; i < argc; i++) {
            size_t arglen = strlen(argv[i]);
            if (fprintf(fp, "$%zu\r\n", arglen) < 0) {
                perror("Failed to write big AOF arg header");
                pthread_mutex_unlock(&aof_state.mutex);
                return;
            }
            if (fwrite(argv[i], 1, arglen, fp) != arglen) {
                perror("Failed to write big AOF arg body");
                pthread_mutex_unlock(&aof_state.mutex);
                return;
            }
            if (fwrite("\r\n", 1, 2, fp) != 2) {
                perror("Failed to write big AOF arg CRLF");
                pthread_mutex_unlock(&aof_state.mutex);
                return;
            }
        }
        fflush(fp);

        if (aof_state.fsync_mode == AOF_FSYNC_ALWAYS) {
            int fd = fileno(fp);
            if (fsync(fd) == -1) {
                perror("Failed to fsync big AOF command");
            }
        }

        aof_state.current_size += (off_t)estimated_len;
        pthread_mutex_unlock(&aof_state.mutex);
        return;
    }

    //非COW模式的正常处理 
    pthread_mutex_lock(&aof_state.mutex);

    int len = snprintf(aof_state.buf + aof_state.buf_len, 
                      aof_state.buf_size - aof_state.buf_len, 
                      "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        len += snprintf(aof_state.buf + aof_state.buf_len + len, 
                       aof_state.buf_size - aof_state.buf_len - len, 
                       "$%zu\r\n%s\r\n", strlen(argv[i]), argv[i]);
    }
    aof_state.buf_len += len;
    aof_state.current_size += len;

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
    if(!aof_state.loading && !aof_state.pause_rewrite && aof_state.current_size >= AOF_REWRITE_MIN_SIZE &&
        growth >= AOF_REWRITE_MIN_GROWTH &&
        growth >= (aof_state.rewrite_base_size * AOF_REWRITE_PERCENTAGE/100)&&
        !aof_state.rewrite_in_progress){
            //aof_rewrite_background();//开始重写
            should_rewrite = 1;
        }

    pthread_mutex_unlock(&aof_state.mutex);

    // 在非COW模式下 如果正在重写 也需要将命令添加到重写缓冲区
    if (aof_state.rewrite_in_progress) {
        aof_rewrite_append_command(argc, argv);
    }

    // 在锁外执行可能阻塞的操作
    if (should_fsync) {
        aof_fsync(1);
    }
    if (should_rewrite) {
        aof_rewrite_background();
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

    //进入load状态
    aof_state.loading = 1;
    aof_state.pause_rewrite = 1;

    resp_parser_t parser;
    resp_parser_init(&parser);

    // 累积缓冲区：使用堆动态分配，便于处理大 AOF
    size_t buf_cap = 1024 * 1024; // 初始 1MB
    char  *buf = (char *)kvs_malloc(buf_cap);
    if (!buf) {
        perror("AOF malloc buf");
        fclose(fp);
        return -1;
    }
    size_t buf_len = 0;        // 当前缓冲区有效数据长度
    size_t total_read = 0;
    int    commands_processed = 0;
    int    parse_errors = 0;

    // 禁用 AOF 追加，避免重放时再次写入 AOF
    set_sync_context(2);

    while (1) {
        // 先尽量从文件读数据填满 buf 剩余空间
        if (!feof(fp) && buf_len < buf_cap) {
            // 如果剩余空间太少，扩容缓冲区
            const size_t min_free = 64 * 1024; // 至少预留 64KB
            if (buf_cap - buf_len < min_free) {
                size_t new_cap = buf_cap * 2;
                if (new_cap > 512 * 1024 * 1024) { // 上限 512MB，防止失控
                    new_cap = 512 * 1024 * 1024;
                }
                char *nbuf = (char *)kvs_realloc(buf, new_cap);
                if (!nbuf) {
                    perror("AOF realloc buf");
                    break;
                }
                buf = nbuf;
                buf_cap = new_cap;
            }

            size_t n = fread(buf + buf_len, 1, buf_cap - buf_len, fp);
            if (n > 0) {
                buf_len    += n;
                total_read += n;
            } else {
                if (ferror(fp)) {
                    perror("AOF fread error");
                    break;
                }
                // n==0 且 !ferror，说明 EOF
            }
        }

        if (buf_len == 0) {
            // 缓冲区已经空，且没有更多数据
            break;
        }

        size_t offset = 0;

        while (offset < buf_len) {
            // 1) 在 [offset, buf_len) 中探测一条完整 RESP 命令长度
            int cmd_len = resp_peek_command(buf, buf_len, offset);
            if (cmd_len < 0) {
                // 协议错误：尝试跳过 1 字节，继续往前探测，避免整文件失败
                parse_errors++;

                offset += 1;
                // 为了防止 offset 超过 buf_len，跳出内层循环到“裁剪剩余数据”
                continue;
            } else if (cmd_len == 0) {
                // 半包：当前 offset 这一条还不完整，需要更多数据
                // 如果已经到达 EOF，说明整个缓冲都是单条完整命令，直接按整块处理，
                // 避免在大 bulk value 上无限输出 half-command 日志。
                if (feof(fp)) {
                    cmd_len = (int)(buf_len - offset);
                } else {
                    break;
                }
            }

            // 2) 现在 [offset, offset+cmd_len) 是一条完整 RESP 命令
            if (parser.state != RESP_PARSE_INIT &&
                parser.state != RESP_PARSE_COMPLETE) {
                // 理论不该发生，保险起见重置
                resp_parser_reset(&parser);
            }

                int parsed = resp_parser(&parser,
                             buf + offset,
                             (size_t)cmd_len);
                if (parsed < 0) {
                parse_errors++;
                // 尝试跳过 1 字节，从下一位置重新探测
                resp_parser_reset(&parser);
                offset += 1;
                continue;
            }

                if (parsed != cmd_len || parser.state != RESP_PARSE_COMPLETE) {
                parse_errors++;
                // 尝试跳过 1 字节
                resp_parser_reset(&parser);
                offset += 1;
                continue;
            }

        // 3) 一条完整命令解析成功，执行
            char *response = (char *)kvs_malloc(KVS_MAX_RESPONSE_LENGTH);
        if (!response) {
                resp_parser_reset(&parser);
                offset += (size_t)cmd_len;
                continue;
            }

                int ret = kvs_filter_protocol(parser.tokens,
                        parser.arg_index,
                        response);

                if (ret <= 0 || ret > (int)KVS_MAX_RESPONSE_LENGTH) {
                kvs_free(response);
                resp_parser_reset(&parser);
                offset += (size_t)cmd_len;
                continue;
            }

            kvs_free(response);
            commands_processed++;
            offset += (size_t)cmd_len;
            resp_parser_reset(&parser);
        }

        // 4) 将未处理的数据移动到缓冲区开头（半包保留）
        if (offset > 0) {
            size_t remaining = buf_len - offset;
            if (remaining > 0) {
                memmove(buf, buf + offset, remaining);
            }
            buf_len = remaining;
        }

        // EOF 时的尾部处理：丢弃任何残余数据，防止半包卡死
        if (feof(fp)) {
            if (buf_len > 0) {
                buf_len = 0;
            }
            break;
        }
    }

    fclose(fp);
    if (buf) kvs_free(buf);
    resp_parser_free(&parser);

    //退出load状态 对齐aof状态
    // 由于 load 期不会再 append，current_size 应该仍等于 init 时的真实文件大小
    // 但为了防御未来代码改动，最好重新 stat 一次对齐：
    // off_t real_size = get_file_size(filename);
    struct stat st;
    off_t real_size = 0;
    if (stat(filename, &st) == 0) {
        real_size = st.st_size;
    }

    pthread_mutex_lock(&aof_state.mutex);
    aof_state.current_size = real_size;
    aof_state.rewrite_base_size = real_size;  //  启动后不应立刻 rewrite
    pthread_mutex_unlock(&aof_state.mutex);

    aof_state.loading = 0;
    aof_state.pause_rewrite = 0;

    set_sync_context(0);
    return 0;
}

//释放资源
void aof_free(void){
    // 如果从未成功初始化，直接返回
    if (!aof_state.initialized) return;

    // 防止重复进入
    aof_state.initialized = 0;

    // 停止重写（如果正在进行）
    aof_stop_rewrite();


    //停止后台线程
    aof_state.stop = 1;
    pthread_cond_signal(&aof_state.cond);
    pthread_join(aof_state.bgthread, NULL);

    //最后进行一次同步
    aof_fsync(1);

    //关闭 fp也必须在同一mutex下
    pthread_mutex_lock(&aof_state.mutex);
    if (aof_state.fp) {
        fclose(aof_state.fp);
        aof_state.fp = NULL;
    }
    pthread_mutex_unlock(&aof_state.mutex);

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

    // 清理重写缓冲区
    pthread_mutex_lock(&aof_state.rewrite_mutex);
    if(aof_state.rewrite_buf){
        kvs_free(aof_state.rewrite_buf);
        aof_state.rewrite_buf = NULL;
    }
    pthread_mutex_unlock(&aof_state.rewrite_mutex);

    pthread_mutex_destroy(&aof_state.rewrite_mutex);

    pthread_mutex_destroy(&aof_state.mutex);
    pthread_cond_destroy(&aof_state.cond);
    
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

int aof_is_loading(void){
    return aof_state.loading;
}

