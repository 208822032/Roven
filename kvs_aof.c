#include "kvstore.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
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
    
    printf("AOF initialized: %s (%ld bytes), sync mode: %s\n", 
           filename, aof_state.current_size, sync_mode_str);

    return 0;
}

//追加函数
void aof_append(int argc, char **argv){
    if(!aof_state.fp || aof_state.rewrite_in_progress) return;

    pthread_mutex_lock(&aof_state.mutex);

    int len = snprintf(aof_state.buf + aof_state.buf_len, aof_state.buf_size - aof_state.buf_len, "*%d\r\n", argc);
    for(int i = 0;i < argc;i++){
        len += snprintf(aof_state.buf + aof_state.buf_len + len, aof_state.buf_size - aof_state.buf_len - len, 
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
    if(aof_state.current_size >= AOF_REWRITE_MIN_SIZE &&
        growth >= AOF_REWRITE_MIN_GROWTH &&
        growth >= (aof_state.rewrite_base_size * AOF_REWRITE_PERCENTAGE/100)&&
        !aof_state.rewrite_in_progress){
            //aof_rewrite_background();//开始重写
            should_rewrite = 1;
        }

    pthread_mutex_unlock(&aof_state.mutex);

    // 在aof_append函数中添加
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



