//解析配置文件 得到主从ip:port
//加载ebpf_kern.o 写入cfg_map 的master ip和port
//建立从节点连接
//从ringbuf中取数据 将其按照原样发送给从节点
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/time.h>

#include "kvs_memory.h"
#include "msg_queue.h"
#include "kvs_config.h" 
#include <pthread.h>
extern kvs_config_t g_cfg;

//消息队列成员
typedef struct ebpf_sync_msg {
    size_t len;
    unsigned char data[];
} ebpf_sync_msg_t;

#define EBPFSYNC_QUEUE_CAPACITY 65535
#define MAX_SLAVES   10

//批量发送
#define EBPFSYNC_BATCH_MAX_BYTES (128 * 1024) // 单次批量发送的最大字节数
#define EBPFSYNC_BATCH_MAX_MSGS  4096  //单批最多聚合多少条消息

typedef struct slave_conf {
    char ip[64];
    int port;
} slave_conf_t;

/*
gcc -O2 -g ebpf_syncd.c ../utils/msg_queue.c  -o ebpf_syncd -lbpf -lelf -lz -lpthread -I ../../include
*/

#define MAX_PAYLOAD 65535 //1MB
#define CHUNK_SIZE 4096   // 8192

struct event {
    __u32 pid;
    __u32 len;
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
    __u32 chunk_idx; //当前tcp_sendmsg调用的分片索引
    __u8 data[CHUNK_SIZE]; //固定大小
};

//配置map 通过用户态写入 master的ip和port
struct config_t {
    __u32 master_ip;
    __u16 master_port;
};

static volatile sig_atomic_t stop = 0;

typedef struct daemon_conf{
    char master_ip[64];
    int master_port;
    char slave_ip[64];
    int slave_port;
    int max_payload;

    int slave_count;
    slave_conf_t slaves[MAX_SLAVES];
} daemon_conf_t;

//每个slave 对应一个ctx 独立fd + 队列 + 线程
typedef struct slave_ctx {
    int id;
    char ip[64];
    int port;
    int fd;

    msg_queue_t queue;
    pthread_t worker;
    volatile int running;
} slave_ctx_t;

// struct stats_t {
//     __u64 hook_calls;     // 命中过 target_master 的 tcp_sendmsg 调用次数
//     __u64 hook_bytes;     // 这些调用请求发送的总字节数(size累加)
//     __u64 mirror_events;  // 实际 reserve 成功并 submit 的 ringbuf event 数
//     __u64 mirror_bytes;   // 实际镜像出去的字节数(按 frag_len 累加)
//     __u64 ringbuf_drops;  // ringbuf_reserve 失败次数(至少粒度是“分片”)
// };


//全局状态
static volatile sig_atomic_t g_stop = 0; //进程若初标记
static volatile int          g_running = 1; //逻辑退出标记

static slave_ctx_t g_slaves[MAX_SLAVES];
static int g_slave_count = 0;

static uint64_t g_capture_total = 0;  // 统计 ringbuf 事件总数（即抓到的分片总数）
static uint64_t g_send_total    = 0;  // 统计发送给所有 slaves 的总次数（按每次 send_all 记一次）

static struct timeval g_qps_start; 

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)


//信号处理
static void handle_sigint(int sig){
    (void)sig;
    g_stop = 1;
    g_running = 0;
}

static int load_daemon_conf(const char* path, daemon_conf_t* conf){
    
    FILE* fp = fopen(path, "r");
    if(!fp){
        perror("fopen");
        return -1;
    }

    memset(conf, 0, sizeof(*conf));
    conf->max_payload = 4096;

    char line[256];
    while(fgets(line, sizeof(line), fp)){
        char key[64], val[128];
        if(sscanf(line, " %63[^=]=%127s ", key, val) != 2){
            continue;
        }
        if(strcmp(key, "master_ip") == 0){
            strncpy(conf->master_ip, val, sizeof(conf->master_ip));
        }else if(strcmp(key, "master_port") == 0){
            conf->master_port = atoi(val);
        }else if(strcmp(key, "max_payload") == 0){
            conf->max_payload = atoi(val);
        } else if(strcmp(key , "slave_count") == 0){
            conf->slave_count = atoi(val);
            if(conf->slave_count < 0) conf->slave_count = 0;
            if(conf->slave_count > MAX_SLAVES) conf->slave_count = MAX_SLAVES;
        }else if(strcmp(key, "slave_ip") == 0 ){
            if(conf->slave_count == 0) conf->slave_count = 1;
            strncpy(conf->slaves[0].ip, val, sizeof(conf->slaves[0].ip));
        }else if(strcmp(key, "slave_port") == 0){
            if(conf->slave_count == 0) conf->slave_count = 1;
            conf->slaves[0].port = atoi(val);
        }else{
            int idx = 0;
            if(sscanf(key, "slave%d_ip", &idx) == 1){
                if(idx >= 1 && idx <= MAX_SLAVES){
                    int i = idx - 1;
                    strncpy(conf->slaves[i].ip, val, sizeof(conf->slaves[i].ip));
                    if(conf->slave_count < idx) conf->slave_count = idx;
                }
            }else if(sscanf(key, "slave%d_port", &idx) == 1){
                if(idx >= 1 && idx <= MAX_SLAVES){
                    int i = idx - 1;
                    conf->slaves[i].port = atoi(val);
                    if(conf->slave_count < idx) conf->slave_count = idx;
                }
            }
        }
    }

    fclose(fp);
    if(conf->max_payload <= 0){
        conf->max_payload = 4096; //默认4096
    }

    if(conf->master_ip[0] == '\0' || conf->master_port == 0){
        fprintf(stderr, "Incomplete daemon configuration\n");
        return -1;
    }

    if(conf->slave_count == 0){
        fprintf(stderr, "No slave configured\n");
        return -1;
    }

    for(int i = 0;i < conf->slave_count; i++){
        if(conf->slaves[i].ip[0] == '\0' || conf->slaves[i].port == 0){
            fprintf(stderr, "Incomplete slave %d configuration\n", i+1);
            return -1;
        }
    }

    return 0;
}


//发送握手命令
static int send_replsync_handshake(int slave_fd){

    const char* cmd = "*1\r\n$8\r\nREPLSYNC\r\n";
    ssize_t sent = send(slave_fd, cmd, strlen(cmd), MSG_NOSIGNAL);
    if(sent < 0){
        perror("send REPLSYNC");
        return -1;
    }

    //简单读回一个响应
    char buf[128];
    int ret = recv(slave_fd, buf, sizeof(buf)-1, 0);
    if(ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK){
        perror("recv REPLSYNC response");
        return -1;
    }
    return 0;
}

//要求发送必须保证完整发送
static int send_all(int fd, const void* buf, size_t len){
    const char* p = buf;
    size_t left = len;

    while(left > 0){
        ssize_t sent = send(fd, p, left, MSG_NOSIGNAL);
        if(sent < 0){
            if(errno == EINTR) continue;
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                //非阻塞情况下无法发送更多
                usleep(10); //稍等
                continue;
            }
            return -1;
        }
        if(sent == 0){
            //连接关闭
            return -1;
        }
        p += sent;
        left -= sent;
    }
    return 0;
}

//为slave建立TCP连接并发送REPLSYNC
static int connect_sand_handshake_slave(slave_ctx_t* ctx){

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx->port);
    if(inet_pton(AF_INET, ctx->ip, &addr.sin_addr) <= 0){
        perror("inet_pton");
        close(fd);
        return -1;
    }
    if(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        perror("connect");
        close(fd);
        return -1;
    }

    printf("Connected to slave %s:%d\n", ctx->ip, ctx->port);

    if(send_replsync_handshake(fd) < 0){
        printf("Failed to send REPLSYNC to slave %s:%d\n", ctx->ip, ctx->port);
        close(fd);
        return -1;
    }

    printf("REPLSYNC handshake sent to slave %s:%d\n", ctx->ip, ctx->port);

    ctx->fd = fd;

    return 0;
}

//批量发送相关结构体和函数
static void flush_batch_to_slave(slave_ctx_t *ctx,
                                 unsigned char *batch_buf,
                                 size_t batch_len,
                                 ebpf_sync_msg_t **msgs,
                                 int msg_count)
{

    if(batch_len == 0 || msg_count == 0) return;

    //确保连接可用
    if(ctx->fd < 0){
        if(connect_sand_handshake_slave(ctx) < 0){
            //连接失败，丢弃消息
            for(int i = 0;i < msg_count;i++){
                kvs_free(msgs[i]);
            }
            return;
        }
    }

    if(send_all(ctx->fd, batch_buf, batch_len) < 0){
        perror("send_all to slave (batch)");
        close(ctx->fd);
        ctx->fd = -1;
    }
    // else{
    //     // 全程统计：每个 slave 成功发送一次就 +1
    //     __sync_fetch_and_add(&g_send_total, 1);
    // }

    //无论是否成功 批内消息都释放
    for(int i = 0;i < msg_count;i++){
        if(msgs[i]) kvs_free(msgs[i]);
    }

}

//发送线程 每个slave对应一个
static void* slave_sender_worker(void* arg){
    slave_ctx_t* ctx = (slave_ctx_t* )arg;

    //建立连接
    if(ctx->fd < 0){
        connect_sand_handshake_slave(ctx);
    }

    unsigned char* batch_buf = (unsigned char* )kvs_malloc(EBPFSYNC_BATCH_MAX_BYTES);
    if(!batch_buf){
        fprintf(stderr, "Failed to allocate batch_buf for slave %d\n", ctx->id);
        //降级为逐条发送
        while(g_running && ctx->running){
            void* item = NULL;

            if(msg_queue_pop(&ctx->queue, &item) != 0){
                //队列可能暂时为空
                usleep(10);
                continue;
            }

            ebpf_sync_msg_t* msg = (ebpf_sync_msg_t*)item;
            if(!msg) continue;  

            //确保连接可用
            if(ctx->fd < 0){
                if(connect_sand_handshake_slave(ctx) < 0){
                    //连接失败，丢弃消息
                    free(msg);
                    continue;
                }
            }

            if(send_all(ctx->fd, msg->data, msg->len) < 0){
                perror("send_all to slave");
                close(ctx->fd);
                ctx->fd = -1;
            }
            // else{
            //     // 全程统计：每个 slave 成功发送一次就 +1
            //     __sync_fetch_and_add(&g_send_total, 1);
            // }

            kvs_free(msg);
        }

        //退出前清理队列剩余数据
        void* item = NULL;
        while(msg_queue_pop(&ctx->queue, &item) == 0){
            ebpf_sync_msg_t* msg = (ebpf_sync_msg_t*)item;
            if(msg) free(msg);
        }

        return NULL;
    }

    size_t batch_len = 0;
    ebpf_sync_msg_t* pending_msgs[EBPFSYNC_BATCH_MAX_MSGS];
    int pending_count = 0;

    while(g_running && ctx->running){
        void* item = NULL;

        if(msg_queue_pop(&ctx->queue, &item) != 0){
            //队列可能暂时为空 如果当前有为发送的batch flush一次
            if(batch_len > 0 && pending_count > 0){
                flush_batch_to_slave(ctx, batch_buf, batch_len,
                                     pending_msgs, pending_count);
                batch_len = 0;
                pending_count = 0;
            }else{
                usleep(10);
            }
            continue;
        }

        ebpf_sync_msg_t* msg = (ebpf_sync_msg_t*)item;
        if (!msg) {
            continue;
        }

        //超大消息 直接单独发送
        if(msg->len > EBPFSYNC_BATCH_MAX_BYTES){
            //先flush
            if(batch_len > 0 && pending_count > 0){
                flush_batch_to_slave(ctx, batch_buf, batch_len,
                                     pending_msgs, pending_count);
                batch_len = 0;
                pending_count = 0;
            }

            if (ctx->fd < 0) {
                if (connect_sand_handshake_slave(ctx) < 0) {
                    kvs_free(msg);
                    continue;
                }
            }

            if (send_all(ctx->fd, msg->data, msg->len) < 0) {
                perror("send_all(large msg) to slave");
                close(ctx->fd);
                ctx->fd = -1;
            } else {
                __sync_fetch_and_add(&g_send_total, 1);
            }

            kvs_free(msg);
            continue;
        }

        // 如果当前 batch 放不下这条消息，或者条数已达上限，先 flush 再开始新的 batch
        if (batch_len + msg->len > EBPFSYNC_BATCH_MAX_BYTES ||
            pending_count >= EBPFSYNC_BATCH_MAX_MSGS) {

            flush_batch_to_slave(ctx, batch_buf, batch_len,
                                 pending_msgs, pending_count);
            batch_len = 0;
            pending_count = 0;
        }

        // 把该消息 append 到 batch buffer 末尾
        memcpy(batch_buf + batch_len, msg->data, msg->len);
        batch_len += msg->len;
        pending_msgs[pending_count++] = msg;
    }

    //线程退出前 flush最后一次
    if(batch_len > 0 && pending_count > 0){
        flush_batch_to_slave(ctx, batch_buf, batch_len,
                             pending_msgs, pending_count);
    }

    //清理数据
    void* item = NULL;
    while (msg_queue_pop(&ctx->queue, &item) == 0) {
        ebpf_sync_msg_t* msg = (ebpf_sync_msg_t*)item;
        if (msg) {
            kvs_free(msg);
        }
    }

    kvs_free(batch_buf);
    return NULL;

}

//广播函数
static void broadcast_to_all_slaves(const void* data, size_t len){

    if(!data || len == 0) return;

    for(int i = 0;i < g_slave_count;i++){
        slave_ctx_t* s = &g_slaves[i];
        if(!s->running) continue;

        ebpf_sync_msg_t* msg = 
            (ebpf_sync_msg_t* )kvs_malloc(sizeof(ebpf_sync_msg_t) + len);
        if(!msg){
            fprintf(stderr, "Failed to allocate ebpf_sync_msg_t for slave %d\n",
                    s->id);
            continue;//内存不足
        }

        msg->len = len;
        memcpy(msg->data, data, len);

        if(msg_queue_push(&s->queue, msg) != 0){
            fprintf(stderr, "msg_queue_push failed for slave %d\n", s->id);
            kvs_free(msg);
            //当前队列已满
        }
    }

}

//建立与从节点的连接
static int connect_slave(const struct daemon_conf* conf){

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(conf->slave_port);
    if(inet_pton(AF_INET, conf->slave_ip, &addr.sin_addr) <= 0){
        perror("inet_pton");
        close(fd);
        return -1;
    }
    if(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        perror("connect");
        close(fd);
        return -1;
    }
    printf("Connected to slave %s:%d\n", conf->slave_ip, conf->slave_port);
    return fd;
}

static void hexdump_prefix(const unsigned char *p, size_t len) {
    size_t n = len > 32 ? 32 : len;
    fprintf(stderr, "[ebpf_syncd] payload prefix (%zu bytes): ", n);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = p[i];
        if (c >= 32 && c <= 126) fputc(c, stderr);
        else fprintf(stderr, "\\x%02X", c);
    }
    fputc('\n', stderr);
}

//ringbuf事件处理回调 增量同步
static int handle_event(void* ctx, void* data, size_t data_sz){
    (void)ctx;
    const struct event* e = data;

    if(data_sz < sizeof(struct event)){
        fprintf(stderr, "Invalid event size: %zu\n", data_sz);
        return -1;
    }

    size_t header_size = offsetof(struct event, data);
    size_t payload_sz = data_sz - header_size;

    size_t len = e->len;
    if(len > payload_sz){
        len = payload_sz;
    }
    if(len == 0) return 0;

    // 只在首字节不是典型 RESP 前缀时打印 dump
    if (e->data[0] != '*' && e->data[0] != '+' &&
        e->data[0] != '-' && e->data[0] != ':' &&
        e->data[0] != '$') {
        fprintf(stderr, "[ebpf_syncd] suspicious event: len=%zu pid=%u\n", len, e->pid);
        hexdump_prefix(e->data, len);
    }

    // 全程统计：一次 ringbuf 事件记一次
    g_capture_total++;

    //统一广播函数
    broadcast_to_all_slaves(e->data, len);

    return 0;
}

//从主节点做一次全量同步
//连接主节点
//发送 "*1\r\n$8\r\nFULLDUMP\r\n"
//接收数据并发送给所有从节点
//  4. 循环 recv：
//      - n > 0  : 收到数据，广播给所有 slave
//      - n == 0 : master 主动关闭连接，认为 FULLDUMP 结束
//      - n < 0 && errno == EAGAIN：
//          - 如果之前还没收到过任何数据：说明 FULLDUMP 还没开始发，稍微 sleep 后重试
//          - 如果已经收到过数据：说明当前数据已经全部读完，认为 FULLDUMP 完成，退出循环
//      - 其他错误：视为失败，返回 -1
static int do_fullsync_from_master(const daemon_conf_t* conf){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(conf->master_port);
    if(inet_pton(AF_INET, conf->master_ip, &addr.sin_addr)
        <= 0){
        perror("inet_pton");
        close(fd);
        return -1;
    }

    if(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        perror("connect");
        close(fd);
        return -1;
    }

    printf("Connected to master %s:%d for FULLDUMP\n",
           conf->master_ip,  conf->master_port);

    //发送 FULLDUMP 命令
    const char* cmd = "*1\r\n$8\r\nFULLDUMP\r\n";
    size_t total = strlen(cmd);
    ssize_t off = 0;
    while(off < total){
        ssize_t w = send(fd, cmd + off, total - off, MSG_NOSIGNAL);
        if(w < 0){
            if(errno == EINTR) continue;
            perror("send FULLDUMP");
            close(fd);
            return -1;
        }
        if(w == 0){
            fprintf(stderr, "Connection closed while sending FULLDUMP\n");
            close(fd);
            return -1;
        }
        off += w;
    }

    // 把 socket 设成非阻塞
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("[fullsync] fcntl(F_GETFL)");
        close(fd);
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("[fullsync] fcntl(F_SETFL, O_NONBLOCK)");
        close(fd);
        return -1;
    }

    //接收数据并广播
    int buf_size = conf->max_payload > 0 ? conf->max_payload : MAX_PAYLOAD;
    unsigned char *buffer = (unsigned char *)malloc(buf_size);
    if (!buffer) {
        fprintf(stderr, "[fullsync] malloc(%d) failed\n", buf_size);
        close(fd);
        return -1;
    }

    int received_any = 0;

    while(!g_stop){
        ssize_t n = recv(fd, buffer, buf_size, 0);
        if (n > 0) {
            // 收到 FULLDUMP 的一块数据，广播给所有 slave
            received_any = 1;
            broadcast_to_all_slaves(buffer, (size_t)n);
            continue;
        }

        if (n == 0) {
            // master 主动关闭连接：认为 FULLDUMP 完成
            printf("Master closed connection during FULLDUMP\n");
            break;
        }

        // n < 0
        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!received_any) {
                // 还没收到任何数据，可能 master 正在准备 FULLDUMP，等一会儿再试
                usleep(1000);
                continue;
            } else {
                // 已经收到过数据了，现在 socket 暂时无数据可读：
                // 认为 FULLDUMP 的这一批数据已经发完，结束全量同步
                printf("FULLDUMP recv finished (no more data from master)\n");
                break;
            }
        }

        // 其他错误：视为 FULLDUMP 失败
        perror("[fullsync] recv FULLDUMP data");
        free(buffer);
        close(fd);
        return -1;
    }
    free(buffer);
    close(fd);
    return 0;
}

int main(int argc, char** argv){
    // 初始化全局配置为默认值（g_cfg 在 kvs_globals.c 中定义）
	kvs_config_init_default(&g_cfg);
    mem_init();

    const char* conf_path = "../../config/ebpf_sync.conf";
    if(argc > 1) conf_path = argv[1];

    printf("[ebpf_syncd] start, conf_path=%s\n", conf_path); // DEBUG

    struct daemon_conf conf = {0};
    if(load_daemon_conf(conf_path, &conf) != 0){
        fprintf(stderr, "Failed to load daemon config\n");
        return 1;
    }

    // printf("[ebpf_syncd] conf loaded: master=%s:%d, slave=%s:%d\n",
    //        conf.master_ip, conf.master_port, conf.slave_ip, conf.slave_port); // DEBUG
    for(int i = 0;i < conf.slave_count;i ++){
        printf("  slave %d: %s:%d\n", i+1, conf.slaves[i].ip, conf.slaves[i].port);
    }

    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    // printf("[ebpf_syncd] before ebpf_kern__open_and_load\n"); // DEBUG

    // // 全程 QPS 统计起点
    // gettimeofday(&g_qps_start, NULL);
    // g_capture_total = 0;
    // g_send_total    = 0;

    //初始化 slave_ctx + sender 线程
    g_slave_count = conf.slave_count;
    if(g_slave_count > MAX_SLAVES){
        g_slave_count = MAX_SLAVES;
    }

    for(int i = 0;i < g_slave_count;i++){
        slave_ctx_t* slave = &g_slaves[i];
        memset(slave, 0, sizeof(*slave));
        slave->id = i;
        slave->fd = -1;
        slave->port = conf.slaves[i].port;
        strncpy(slave->ip, conf.slaves[i].ip, sizeof(slave->ip)-1);
        slave->ip[sizeof(slave->ip)-1] = '\0';
        slave->running = 1;

        if(msg_queue_init(&slave->queue, EBPFSYNC_QUEUE_CAPACITY) != 0){
            fprintf(stderr, "msg_queue_init failed for slave %d\n", i);
            slave->running = 0;

            g_slave_count = i;
            g_running = 0;
            goto cleanup;
        }

        if(pthread_create(&slave->worker, NULL, slave_sender_worker, slave) != 0){
            perror("pthread_create for slave sender");
            msg_queue_destroy(&slave->queue);
            slave->running = 0;

            g_slave_count = i + 1;
            g_running = 0;
            goto cleanup;
        }
    }

    //先做一次全量同步
    if(do_fullsync_from_master(&conf) != 0){
        fprintf(stderr, "do_fullsync_from_master failed\n");
        g_running = 0;
        goto cleanup;
    }

    printf("[ebpf_syncd] Fullsync from master completed\n"); // DEBUG

    const char* obj_path = "ebpf_kern.o";

    // 增加路径自动查找逻辑：
    // 如果当前目录下找不到，尝试去 src/eBPF 下找（适配从项目根目录运行的情况）
    if (access(obj_path, F_OK) != 0) {
        if (access("src/eBPF/ebpf_kern.o", F_OK) == 0) {
            obj_path = "src/eBPF/ebpf_kern.o";
        }
    }

    struct bpf_object* obj = bpf_object__open_file(obj_path, NULL);
    if(libbpf_get_error(obj)){
        int err = -libbpf_get_error(obj);
        fprintf(stderr, "Failed to open BPF object file: %s, err=%d\n", obj_path, err);
        return 1;
    }

    //加载BPF对象
    int err = bpf_object__load(obj);
    if(err){
        fprintf(stderr, "Failed to load BPF object: %d\n", err);
        bpf_object__close(obj);
        return 1;
    }

    // printf("[ebpf_syncd] BPF object loaded\n"); // DEBUG

    //配置map 写入 master ip和port
    struct bpf_map* config_map = bpf_object__find_map_by_name(obj, "config_map");
    if(!config_map){
        fprintf(stderr, "Failed to find config_map\n");
        bpf_object__close(obj);
        return 1;
    }

    int cfg_map_fd = bpf_map__fd(config_map);
    if(cfg_map_fd < 0){
        fprintf(stderr, "Invalid config_map fd\n");
        bpf_object__close(obj);
        return 1;
    }
    // printf("[ebpf_syncd] config_map fd=%d\n", cfg_map_fd); // DEBUG

    //填写配置
    __u32 key = 0;
    struct config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    if(inet_pton(AF_INET, conf.master_ip, &cfg.master_ip) <= 0){
        fprintf(stderr, "Invalid master IP: %s\n", conf.master_ip);
        bpf_object__close(obj);
        return 1;
    }
    cfg.master_port = conf.master_port;

    if(bpf_map_update_elem(cfg_map_fd, &key, &cfg, BPF_ANY) != 0){
        perror("bpf_map_update_elem");
        bpf_object__close(obj);
        return 1;
    }
    // printf("[ebpf_syncd] config_map updated: master_ip=0x%x, master_port=%d\n",
    //        cfg.master_ip, cfg.master_port); // DEBUG
        
    struct bpf_map* events_map = bpf_object__find_map_by_name(obj, "events");
    if(!events_map){
        fprintf(stderr, "Failed to find events map\n");
        bpf_object__close(obj);
        return 1;
    }
    int events_map_fd = bpf_map__fd(events_map);
    if(events_map_fd < 0){
        fprintf(stderr, "Invalid events map fd\n");
        bpf_object__close(obj);
        return 1;
    }
    // printf("[ebpf_syncd] events map fd=%d\n", events_map_fd); // DEBUG

    // struct bpf_map *stats_map = bpf_object__find_map_by_name(obj, "stats_map");
    // if (!stats_map) {
    //     fprintf(stderr, "Failed to find stats_map\n");
    //     bpf_object__close(obj);
    //     return 1;
    // }
    // int stats_map_fd = bpf_map__fd(stats_map);
    // if (stats_map_fd < 0) {
    //     fprintf(stderr, "Invalid stats_map fd\n");
    //     bpf_object__close(obj);
    //     return 1;
    // }

    // struct stats_t start_stats = {0};
    // __u32 idx0 = 0;

    // PERCPU_ARRAY：返回的是所有 CPU 值的数组，你可以先简单假设单 CPU，
    // 或者按需要写个小函数做“percpu 求和”。这里为简化起见，先当作普通 ARRAY 说明思路。
    // if (bpf_map_lookup_elem(stats_map_fd, &idx0, &start_stats) != 0) {
    //     memset(&start_stats, 0, sizeof(start_stats));
    // }

    struct bpf_program* prog = bpf_object__find_program_by_name(obj, "kprobe_tcp_sendmsg");
    if(!prog){
        fprintf(stderr, "Failed to find program tcp_sendmsg\n");
        bpf_object__close(obj);
        return 1;
    }
    struct bpf_link* link = bpf_program__attach(prog);
    if(libbpf_get_error(link)){
        int err = -libbpf_get_error(link);
        fprintf(stderr, "Failed to attach program: %d\n", err);
        bpf_object__close(obj);
        return 1;
    }

    // printf("[ebpf_syncd] Program attached\n"); // DEBUG

    //创建ringbuffer
    struct ring_buffer* ringbuf = 
        ring_buffer__new(events_map_fd, handle_event, NULL, NULL);
    if(libbpf_get_error(ringbuf)){
        int err = -libbpf_get_error(ringbuf);
        fprintf(stderr, "Failed to create ring buffer: %d\n", err);
        g_running = 0;
    }else{
        printf("[ebpf_syncd] Ring buffer created\n"); // DEBUG
        while(!stop && g_running){
            int pret = ring_buffer__poll(ringbuf, 10);
            if(pret < 0 && pret != -EINTR){
                fprintf(stderr, "Error polling ring buffer: %d\n", pret);
                break;
            }
            // else pret == 0: timeout, just continue
        }
    }

    printf("[ebpf_syncd] Ring buffer created, entering main loop\n"); // DEBUG

    printf("[ebpf_syncd] Shutting down...\n"); // DEBUG
    
    g_running = 0;

cleanup:
    //释放ringbuffer
    if(ringbuf && !libbpf_get_error(ringbuf)){
        ring_buffer__free(ringbuf);
    }

    //停止所有slave sender线程
    for(int i = 0;i < g_slave_count;i++){
        slave_ctx_t* slave = &g_slaves[i];
        slave->running = 0;
    }

    for(int i = 0;i < g_slave_count;i++){
        slave_ctx_t* slave = &g_slaves[i];
        if(slave->worker){
            pthread_join(slave->worker, NULL);
            slave->worker = 0;
        }
        if(slave->fd >= 0){
            close(slave->fd);
            slave->fd = -1;
        }
        msg_queue_destroy(&slave->queue);
    }



     // 计算整个运行周期内的平均 QPS
//     struct timeval now;
//     gettimeofday(&now, NULL);
//     long elapsed_ms = TIME_SUB_MS(now, g_qps_start);
//     if (elapsed_ms <= 0) {
//         elapsed_ms = 1;
//     }

//     double capture_qps_avg = (double)g_capture_total * 1000.0 / (double)elapsed_ms;
//     double send_qps_avg    = (double)g_send_total    * 1000.0 / (double)elapsed_ms;

//     struct stats_t end_stats = {0};
// if (bpf_map_lookup_elem(stats_map_fd, &idx0, &end_stats) != 0) {
//         memset(&end_stats, 0, sizeof(end_stats));
//     }

//     struct stats_t delta;
//     delta.hook_calls    = end_stats.hook_calls    - start_stats.hook_calls;
//     delta.hook_bytes    = end_stats.hook_bytes    - start_stats.hook_bytes;
//     delta.mirror_events = end_stats.mirror_events - start_stats.mirror_events;
//     delta.mirror_bytes  = end_stats.mirror_bytes  - start_stats.mirror_bytes;
//     delta.ringbuf_drops = end_stats.ringbuf_drops - start_stats.ringbuf_drops;

//     double hook_qps   = (double)delta.hook_calls    * 1000.0 / (double)elapsed_ms;
//     double hook_Bps   = (double)delta.hook_bytes    * 1000.0 / (double)elapsed_ms;
//     double mirror_qps = (double)delta.mirror_events * 1000.0 / (double)elapsed_ms;
//     double mirror_Bps = (double)delta.mirror_bytes  * 1000.0 / (double)elapsed_ms;

//     printf("[kernel] hook_calls=%llu, hook_bytes=%llu\n",
//            (unsigned long long)delta.hook_calls,
//            (unsigned long long)delta.hook_bytes);

//     printf("[kernel] hook_qps=%.2f, hook_Bps=%.2f\n",
//            hook_qps, hook_Bps);

//     printf("[kernel] mirror_events=%llu, mirror_bytes=%llu\n",
//            (unsigned long long)delta.mirror_events,
//            (unsigned long long)delta.mirror_bytes);

//     printf("[kernel] mirror_qps=%.2f, mirror_Bps=%.2f\n",
//            mirror_qps, mirror_Bps);

//     printf("[kernel] ringbuf_drops=%llu\n",
//            (unsigned long long)delta.ringbuf_drops);
//     printf("[summary] elapsed=%ld ms\n", elapsed_ms);

    
    // printf("[summary] capture_total=%" PRIu64 ", avg_capture_qps=%.2f\n",
    //        g_capture_total, capture_qps_avg);
    // printf("[summary] send_total=%" PRIu64 ", avg_send_qps=%.2f\n",
    //        g_send_total, send_qps_avg);

    bpf_link__destroy(link);
    bpf_object__close(obj);
    mem_destroy();

    printf("[ebpf_syncd] Exited cleanly\n");
    return 0;
}