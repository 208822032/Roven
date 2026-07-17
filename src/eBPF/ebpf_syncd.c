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
#include <netinet/tcp.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <time.h>
#include <strings.h>


#include "msg_queue.h"
#include "tx_batcher.h"
#include "kvs_memory.h"
#include "kvs_types.h"
#include "kvs_config.h"
#include "resp_protocol.h"
#include <pthread.h>
extern kvs_config_t g_cfg;


//消息队列成员
typedef struct ebpf_sync_msg {
    atomic_int refcnt;
    size_t len;
    unsigned char data[];
} ebpf_sync_msg_t;

#define MAX_SLAVES   10

typedef struct slave_conf {
    char ip[64];
    int port;
} slave_conf_t;

/*
gcc -O2 -g ebpf_syncd.c ../utils/msg_queue.c  -o ebpf_syncd -lbpf -lelf -lz -lpthread -I ../../include
*/

#define CHUNK_SIZE 4096

#define TX_BUF_SIZE (128 * 1024)
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

    struct tx_batcher txb;
    unsigned char tx_buf[TX_BUF_SIZE];
} slave_ctx_t;


//全局状态
static volatile sig_atomic_t g_stop = 0; //进程若初标记
static volatile int          g_running = 1; //逻辑退出标记

static slave_ctx_t g_slaves[MAX_SLAVES];
static int g_slave_count = 0;
static unsigned char *g_rx_stream = NULL;
static size_t g_rx_stream_len = 0;
static size_t g_rx_stream_cap = 0;
static kvs_ebpf_mode_t g_ebpf_mode = KVS_EBPF_DISABLED;

static void broadcast_to_all_slaves(const void *data, size_t len);

//信号处理
static void handle_sigint(int sig){
    (void)sig;
    g_stop = 1;
    g_running = 0;
}

struct send_stats {
    atomic_ulong send_calls;      // send() 调用次数（含 send_all 内部每次）
    atomic_ulong send_bytes;      // send() 实际发出的字节数累计
    atomic_ulong send_all_calls;  // send_all() 被调用次数
    atomic_ulong send_all_bytes;  // send_all() 期望发送的字节数累计
    atomic_ulong eagain_cnt;      // EAGAIN/EWOULDBLOCK 次数
    atomic_ulong eintr_cnt;       // EINTR 次数
    atomic_ulong retry_loops;     // send_all while 循环迭代次数累计
    atomic_ulong fail_cnt;        // send 失败（非EINTR/EAGAIN）次数
};

static struct send_stats g_stats;

static inline unsigned long atomic_xchg_ul(atomic_ulong *v, unsigned long nv) {
    return atomic_exchange(v, nv);
}

static ebpf_sync_msg_t* ebpf_sync_msg_create(const void* data, size_t len,int refcnt){

    if(!data || len == 0 || refcnt < 0) return NULL;

    ebpf_sync_msg_t* msg = 
        (ebpf_sync_msg_t*)kvs_malloc(sizeof(ebpf_sync_msg_t) + len);

    atomic_init(&msg->refcnt, refcnt);
    msg->len = len;
    memcpy(msg->data, data, len);
    return msg;
}

static inline void ebpf_sync_msg_put(ebpf_sync_msg_t* msg){
    if(!msg) return;
	if(atomic_fetch_sub_explicit(&msg->refcnt, 1, memory_order_acq_rel) == 1)
        kvs_free(msg);
}


static void* stats_printer_thread(void* arg) {
    (void)arg;
    while (1) {
        sleep(1);

        unsigned long sc  = atomic_xchg_ul(&g_stats.send_calls, 0);
        unsigned long sb  = atomic_xchg_ul(&g_stats.send_bytes, 0);
        unsigned long sac = atomic_xchg_ul(&g_stats.send_all_calls, 0);
        unsigned long sab = atomic_xchg_ul(&g_stats.send_all_bytes, 0);
        unsigned long ea  = atomic_xchg_ul(&g_stats.eagain_cnt, 0);
        unsigned long ei  = atomic_xchg_ul(&g_stats.eintr_cnt, 0);
        unsigned long rl  = atomic_xchg_ul(&g_stats.retry_loops, 0);
        unsigned long fc  = atomic_xchg_ul(&g_stats.fail_cnt, 0);

        double avg_send = sc ? (double)sb / (double)sc : 0.0;
        double avg_all  = sac ? (double)sab / (double)sac : 0.0;

        fprintf(stderr,
            "[stats] send_all: calls=%lu bytes=%lu avg=%.1f | "
            "send(): calls=%lu bytes=%lu avg=%.1f | "
            "EAGAIN=%lu EINTR=%lu loops=%lu fail=%lu\n",
            sac, sab, avg_all,
            sc, sb, avg_send,
            ea, ei, rl, fc
        );
    }
    return NULL;
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
#if 0
static int send_all(int fd, const void* buf, size_t len){
    const char* p = buf;
    size_t left = len;

    while(left > 0){
        ssize_t sent = send(fd, p, left, MSG_NOSIGNAL);
        if(sent < 0){
            if(errno == EINTR) continue;
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                //非阻塞情况下无法发送更多
                usleep(1000); //稍等
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
#else
static int send_all(int fd, const void* buf, size_t len) {
    atomic_fetch_add(&g_stats.send_all_calls, 1);
    atomic_fetch_add(&g_stats.send_all_bytes, (unsigned long)len);

    const char* p = (const char*)buf;
    size_t left = len;

    while (left > 0) {
        atomic_fetch_add(&g_stats.retry_loops, 1);

        ssize_t sent = send(fd, p, left, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                atomic_fetch_add(&g_stats.eintr_cnt, 1);
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                atomic_fetch_add(&g_stats.eagain_cnt, 1);
                // 如果是阻塞 socket，这里一般不会出现；出现说明你 socket 可能被设成非阻塞了
                usleep(1000);
                continue;
            }
            atomic_fetch_add(&g_stats.fail_cnt, 1);
            return -1;
        }
        if (sent == 0) {
            atomic_fetch_add(&g_stats.fail_cnt, 1);
            return -1;
        }

        atomic_fetch_add(&g_stats.send_calls, 1);
        atomic_fetch_add(&g_stats.send_bytes, (unsigned long)sent);

        p += sent;
        left -= (size_t)sent;
    }
    return 0;
}
#endif

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int txb_send_fn(void *ctx, const void *buf, size_t len) {
    slave_ctx_t *s = (slave_ctx_t *)ctx;
    if (s->fd < 0) return -1;
    return send_all(s->fd, buf, len);
}

static int rx_stream_reserve(size_t need) {
    if (need <= g_rx_stream_cap) return 0;

    size_t new_cap = g_rx_stream_cap ? g_rx_stream_cap : (64 * 1024);
    while (new_cap < need) {
        if (new_cap > (SIZE_MAX / 2)) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }

    unsigned char *tmp = (unsigned char *)kvs_realloc(g_rx_stream, new_cap);
    if (!tmp) {
        return -1;
    }

    g_rx_stream = tmp;
    g_rx_stream_cap = new_cap;
    return 0;
}

static int broadcast_resp_array(const char *const *argv, int argc) {
    if (!argv || argc <= 0) return -1;

    size_t need = (size_t)resp_generate_array_start(NULL, argc);
    for (int i = 0; i < argc; ++i) {
        need += (size_t)resp_generate_array_item(NULL, argv[i]);
    }

    unsigned char *buf = (unsigned char *)kvs_malloc(need);
    if (!buf) return -1;

    size_t off = 0;
    off += (size_t)resp_generate_array_start((char *)buf + off, argc);
    for (int i = 0; i < argc; ++i) {
        off += (size_t)resp_generate_array_item((char *)buf + off, argv[i]);
    }

    broadcast_to_all_slaves(buf, off);
    kvs_free(buf);
    return 0;
}

static int is_replicated_write_command(const char *cmd) {
    if (!cmd) return 0;

    return strcasecmp(cmd, "SET") == 0 ||
           strcasecmp(cmd, "RSET") == 0 ||
           strcasecmp(cmd, "HSET") == 0 ||
           strcasecmp(cmd, "SSET") == 0 ||
           strcasecmp(cmd, "MOD") == 0 ||
           strcasecmp(cmd, "RMOD") == 0 ||
           strcasecmp(cmd, "HMOD") == 0 ||
           strcasecmp(cmd, "SMOD") == 0 ||
           strcasecmp(cmd, "DEL") == 0 ||
           strcasecmp(cmd, "RDEL") == 0 ||
           strcasecmp(cmd, "HDEL") == 0 ||
           strcasecmp(cmd, "SDEL") == 0 ||
           strcasecmp(cmd, "EXPIRE") == 0 ||
           strcasecmp(cmd, "PEXPIREAT") == 0 ||
           strcasecmp(cmd, "PERSIST") == 0 ||
           strcasecmp(cmd, "PAPERSET") == 0 ||
           strcasecmp(cmd, "PAPERDEL") == 0 ||
           strcasecmp(cmd, "PAPERMOD") == 0;
}

static int normalize_and_broadcast_command(const unsigned char *cmd_buf, size_t cmd_len) {
    if (!cmd_buf || cmd_len == 0) return -1;

    resp_parser_t parser;
    resp_parser_init(&parser);

    int parsed = resp_parser(&parser, (const char *)cmd_buf, cmd_len);
    if (parsed <= 0 || parser.state != RESP_PARSE_COMPLETE || parser.arg_index <= 0 || !parser.tokens[0]) {
        resp_parser_free(&parser);
        return -1;
    }

    const char *cmd = parser.tokens[0];
    int rc = 0;

    if (strcasecmp(cmd, "EXPIRE") == 0 && parser.arg_index >= 3 && parser.tokens[1] && parser.tokens[2]) {
        char *endptr = NULL;
        long long seconds = strtoll(parser.tokens[2], &endptr, 10);
        if (!endptr || *endptr != '\0') {
            resp_parser_free(&parser);
            return -1;
        }

        if (seconds <= 0) {
            const char *argv[] = {"DEL", parser.tokens[1]};
            rc = broadcast_resp_array(argv, 2);
        } else {
            int64_t expire_at_ms = kv_now_ms() + (int64_t)seconds * 1000LL;
            char expire_buf[32];
            snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)expire_at_ms);
            const char *argv[] = {"PEXPIREAT", parser.tokens[1], expire_buf};
            rc = broadcast_resp_array(argv, 3);
        }
        resp_parser_free(&parser);
        return rc;
    }

    if (!is_replicated_write_command(cmd)) {
        resp_parser_free(&parser);
        return 0;
    }

    broadcast_to_all_slaves(cmd_buf, cmd_len);
    resp_parser_free(&parser);
    return 0;
}

static void process_rx_stream(void) {
    size_t offset = 0;

    while (offset < g_rx_stream_len) {
        int cmd_len = resp_peek_command((const char *)g_rx_stream, g_rx_stream_len, offset);
        if (cmd_len == 0) {
            break;
        }
        if (cmd_len < 0) {
            fprintf(stderr, "[ebpf_syncd] RESP parse error in replication stream, dropping buffer\n");
            g_rx_stream_len = 0;
            return;
        }

        if (normalize_and_broadcast_command(g_rx_stream + offset, (size_t)cmd_len) < 0) {
            fprintf(stderr, "[ebpf_syncd] failed to normalize replication command, dropping command\n");
        }

        offset += (size_t)cmd_len;
    }

    if (offset > 0) {
        size_t remain = g_rx_stream_len - offset;
        if (remain > 0) {
            memmove(g_rx_stream, g_rx_stream + offset, remain);
        }
        g_rx_stream_len = remain;
    }
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

    int mss = 1460;
    // 设置 TCP MSS
    if (setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss)) < 0) {
        perror("Failed to set TCP_MAXSEG");
        return -1;
    }

    int enable = 1;
    
    // 开启 TCP_NODELAY（禁用 Nagle 算法）
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) < 0) {
        perror("Failed to set TCP_NODELAY");
    }

    // 3) 验证真实 MSS
    int mss_cur = -1;
    socklen_t sl = sizeof(mss_cur);
    if (getsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss_cur, &sl) < 0) {
        perror("getsockopt TCP_MAXSEG");
    }

    // 4) 验证 TCP_NODELAY
    int nd = 0;
    sl = sizeof(nd);
    if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, &sl) == 0) {
        // fprintf(stderr, "[slave] TCP_NODELAY=%d\n", nd);
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

#if 0
//发送线程 每个slave对应一个
static void* slave_sender_worker(void* arg){
    slave_ctx_t* ctx = (slave_ctx_t* )arg;

    //建立连接
    if(ctx->fd < 0){
        connect_sand_handshake_slave(ctx);
    }

    while(g_running && ctx->running){
        void* item = NULL;

        if(msg_queue_pop(&ctx->queue, &item) != 0){
            //队列可能暂时为空
            usleep(1000);
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

        // free(msg);
    }

    //退出前清理队列剩余数据
    void* item = NULL;
    while(msg_queue_pop(&ctx->queue, &item) == 0){
        ebpf_sync_msg_t* msg = (ebpf_sync_msg_t*)item;
        if(msg) free(msg);
    }

    return NULL;
}
#else
static void* slave_sender_worker(void* arg){
    slave_ctx_t* ctx = (slave_ctx_t*)arg;

    // 确保连接
    if (ctx->fd < 0) {
        if (connect_sand_handshake_slave(ctx) < 0) {
            // 连接失败也别退出线程：后面继续尝试重连
            ctx->fd = -1;
        }
    }

    // 初始化 batcher（buf 由 ctx 提供，无 malloc）
    tx_batcher_init(&ctx->txb,
                    ctx->tx_buf, sizeof(ctx->tx_buf),
                    (uint64_t)g_cfg.ebpf_flush_interval_us * 1000ull,
                    txb_send_fn, ctx);
    
    int idle = 0;

    while (g_running && ctx->running) {
        void* item = NULL;

        if (msg_queue_pop(&ctx->queue, &item) != 0) {
            idle++;

            // 队列空时，降频检查 flush（避免把小尾巴刷得太勤）
            if ((idle & 0xF) == 0) { // 每16次空转检查一次
                if (ctx->fd >= 0) {
                    if (tx_batcher_maybe_flush(&ctx->txb) < 0) {
                        close(ctx->fd);
                        ctx->fd = -1;
                    }
                }
            }
            usleep(0); // 空转别太猛
            continue;
        }

        idle = 0;

        ebpf_sync_msg_t* msg = (ebpf_sync_msg_t*)item;
        if (!msg) continue;

        // 确保连接：重连不成功就别丢 msg，拿着它等下一轮
        while ((g_running && ctx->running) && ctx->fd < 0) {
            if (connect_sand_handshake_slave(ctx) == 0) break;
            usleep(500); // backoff
        }
        // 确保连接可用
        if (ctx->fd < 0) {
            // kvs_free(msg);
            ebpf_sync_msg_put(msg);
            break;
        }

        // 只做字节聚合（不解析）
        if (tx_batcher_append(&ctx->txb, msg->data, msg->len) < 0) {
            // 发送失败：断开，等待重连
            perror("tx_batcher_append/send");
            close(ctx->fd);
            ctx->fd = -1;
        }

        // kvs_free(msg);
        ebpf_sync_msg_put(msg);
    }

    // 退出前尽量发掉尾巴（可选重连）
    if (ctx->fd < 0) (void)connect_sand_handshake_slave(ctx);
    if (ctx->fd >= 0) (void)tx_batcher_flush(&ctx->txb);

    // 清理队列剩余消息
    void* item = NULL;
    while (msg_queue_pop(&ctx->queue, &item) == 0) {
        ebpf_sync_msg_t* msg = (ebpf_sync_msg_t*)item;
        // if (msg) kvs_free(msg);
        if(msg) ebpf_sync_msg_put(msg);
    }
    return NULL;
}

#endif

//广播函数
static void broadcast_to_all_slaves(const void* data, size_t len){

    if(!data || len == 0) return;

    int targets = 0;
    for(int i = 0;i < g_slave_count; i++){
        if(g_slaves[i].running) targets++;
    }
    if(targets == 0) return;

    ebpf_sync_msg_t* msg = ebpf_sync_msg_create(data, len, targets);
    if(!msg){
        fprintf(stderr, "Failed to allocate ebpf_sync_msg_t (len=%zu)\n", len);
        return;
    }

    for(int i = 0;i < g_slave_count;i++){
        slave_ctx_t* s = &g_slaves[i];
        // if(!s->running) continue;

        // ebpf_sync_msg_t* msg = 
        //     (ebpf_sync_msg_t* )kvs_malloc(sizeof(ebpf_sync_msg_t) + len);
        // if(!msg){
        //     fprintf(stderr, "Failed to allocate ebpf_sync_msg_t for slave %d\n",
        //             s->id);
        //     continue;//内存不足
        // }

        // msg->len = len;
        // memcpy(msg->data, data, len);

        if(!s->running){
            ebpf_sync_msg_put(msg);
            continue;
        }

        if(msg_queue_push(&s->queue, msg) != 0){
            fprintf(stderr, "msg_queue_push failed for slave %d\n", s->id);
            // kvs_free(msg);
            ebpf_sync_msg_put(msg);
            //当前队列已满
        }
    }

}

//建立与从节点的连接
#if 0
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
    int mss = 1460;
    // 设置 TCP MSS
    if (setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss)) < 0) {
        perror("Failed to set TCP_MAXSEG");
        return -1;
    }

    int enable = 1;
    
    // 开启 TCP_NODELAY（禁用 Nagle 算法）
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) < 0) {
        perror("Failed to set TCP_NODELAY");
    }

    // 3) 验证真实 MSS
    int mss_cur = -1;
    socklen_t sl = sizeof(mss_cur);
    if (getsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss_cur, &sl) < 0) {
        perror("getsockopt TCP_MAXSEG");
    }

    // 4) 验证 TCP_NODELAY
    int nd = 0;
    sl = sizeof(nd);
    if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, &sl) == 0) {
        fprintf(stderr, "[slave] TCP_NODELAY=%d\n", nd);
    }

    printf("Connected to slave %s:%d\n", conf->slave_ip, conf->slave_port);
    return fd;
}
#endif

//ringbuf事件处理回调 增量同步
#if 0
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

    //统一广播函数
    broadcast_to_all_slaves(e->data, len);

    return 0;
}
#else
static int handle_event(void* ctx, void* data, size_t data_sz){
    (void)ctx;

    const size_t header_size = offsetof(struct event, data);
    if (data_sz < header_size) {
        fprintf(stderr, "Invalid event size (no header): %zu\n", data_sz);
        return 0; // 不要返回 -1，避免 ring_buffer__poll 直接失败退出
    }

    const struct event* e = (const struct event*)data;

    size_t payload_sz = data_sz - header_size;
    if (payload_sz > CHUNK_SIZE)
        payload_sz = CHUNK_SIZE;

    size_t len = (size_t)e->len;
    if (len > payload_sz)
        len = payload_sz;
	if (len > g_cfg.ebpf_max_event_bytes) {
		fprintf(stderr, "[ebpf_syncd] event exceeds configured max_event_bytes: %zu > %zu\n",
			len, g_cfg.ebpf_max_event_bytes);
		return 0;
	}

    if (len == 0)
        return 0;
	if (g_ebpf_mode == KVS_EBPF_OBSERVE) {
		return 0;
	}

    if (rx_stream_reserve(g_rx_stream_len + len) != 0) {
        fprintf(stderr, "[ebpf_syncd] rx stream allocation failed\n");
        return 0;
    }

    memcpy(g_rx_stream + g_rx_stream_len, e->data, len);
    g_rx_stream_len += len;

    process_rx_stream();
    return 0;
}
#endif

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
    int buf_size = conf->max_payload > 0 ? conf->max_payload : CHUNK_SIZE;
    unsigned char *buffer = (unsigned char *)kvs_malloc(buf_size);
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
        kvs_free(buffer);
        close(fd);
        return -1;
    }
    kvs_free(buffer);
    close(fd);
    return 0;
}

int main(int argc, char** argv){
	kvs_config_error_t config_error;
    struct daemon_conf conf = {0};
	struct bpf_object* obj = NULL;
	struct bpf_link* link_entry = NULL;
	struct bpf_link* link_ret = NULL;
	struct ring_buffer* ringbuf = NULL;
	struct bpf_map* events_map;
	int events_map_fd;
	int err;

	if (kvs_config_load(&g_cfg, argc, argv, "config/kvstore.conf", stderr,
		&config_error) != 0) {
		fprintf(stderr, "ebpf_syncd configuration error: %s\n", config_error.message);
        return 1;
    }
	if (g_cfg.ebpf_mode == KVS_EBPF_DISABLED) {
		printf("[ebpf_syncd] ebpf.mode=disabled; nothing to run\n");
		return 0;
	}
	g_ebpf_mode = g_cfg.ebpf_mode;
	snprintf(conf.master_ip, sizeof(conf.master_ip), "%s", g_cfg.ebpf_observe_host);
	conf.master_port = g_cfg.ebpf_observe_port;
	conf.max_payload = (int)g_cfg.ebpf_max_event_bytes;
	if (g_cfg.ebpf_mode == KVS_EBPF_EXPERIMENTAL_MIRROR) {
		conf.slave_count = (int)g_cfg.ebpf_mirror_target_count;
		for (int i = 0; i < conf.slave_count; ++i) {
			snprintf(conf.slaves[i].ip, sizeof(conf.slaves[i].ip), "%s",
				g_cfg.ebpf_mirror_endpoints[i].host);
			conf.slaves[i].port = g_cfg.ebpf_mirror_endpoints[i].port;
		}
    }

	mem_init();
	printf("[ebpf_syncd] effective config: mode=%s observe=%s:%d targets=%d object=%s\n",
		g_cfg.ebpf_mode == KVS_EBPF_OBSERVE ? "observe" : "experimental_mirror",
		conf.master_ip, conf.master_port, conf.slave_count, g_cfg.ebpf_object_path);

    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    // printf("[ebpf_syncd] before ebpf_kern__open_and_load\n"); // DEBUG

    // observe模式不创建sender，也不执行FULLDUMP。
    g_slave_count = g_cfg.ebpf_mode == KVS_EBPF_EXPERIMENTAL_MIRROR ? conf.slave_count : 0;

    for(int i = 0;i < g_slave_count;i++){
        slave_ctx_t* slave = &g_slaves[i];
        memset(slave, 0, sizeof(*slave));
        slave->id = i;
        slave->fd = -1;
        slave->port = conf.slaves[i].port;
        strncpy(slave->ip, conf.slaves[i].ip, sizeof(slave->ip)-1);
        slave->ip[sizeof(slave->ip)-1] = '\0';
        slave->running = 1;

        if(msg_queue_init(&slave->queue, g_cfg.ebpf_user_queue_capacity) != 0){
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

    if(g_cfg.ebpf_mode == KVS_EBPF_EXPERIMENTAL_MIRROR &&
	   do_fullsync_from_master(&conf) != 0){
        fprintf(stderr, "do_fullsync_from_master failed\n");
        g_running = 0;
        goto cleanup;
    }

	if (g_cfg.ebpf_mode == KVS_EBPF_EXPERIMENTAL_MIRROR) {
		printf("[ebpf_syncd] Fullsync from master completed\n");
	}

    obj = bpf_object__open_file(g_cfg.ebpf_object_path, NULL);
    if(libbpf_get_error(obj)){
        int err = -libbpf_get_error(obj);
        fprintf(stderr, "Failed to open BPF object file: %s, err=%d\n",
			g_cfg.ebpf_object_path, err);
        return 1;
    }
	events_map = bpf_object__find_map_by_name(obj, "events");
	if (!events_map || bpf_map__set_max_entries(events_map,
		(__u32)g_cfg.ebpf_ring_buffer_bytes) != 0) {
		fprintf(stderr, "Failed to configure events ring buffer size=%zu\n",
			g_cfg.ebpf_ring_buffer_bytes);
		bpf_object__close(obj);
		return 1;
	}

    //加载BPF对象
	err = bpf_object__load(obj);
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
        
    if(!events_map){
        fprintf(stderr, "Failed to find events map\n");
        bpf_object__close(obj);
        return 1;
    }
	events_map_fd = bpf_map__fd(events_map);
    if(events_map_fd < 0){
        fprintf(stderr, "Invalid events map fd\n");
        bpf_object__close(obj);
        return 1;
    }
    // printf("[ebpf_syncd] events map fd=%d\n", events_map_fd); // DEBUG

    struct bpf_program* prog_entry = 
        bpf_object__find_program_by_name(obj, "kprobe_tcp_recvmsg");
    if(!prog_entry){
        fprintf(stderr, "Failed to find program tcp_recvmsg\n");
        bpf_object__close(obj);
        return 1;
    }
    link_entry = bpf_program__attach(prog_entry);
    if(libbpf_get_error(link_entry)){
        int err = -libbpf_get_error(link_entry);
        fprintf(stderr, "Failed to attach program: %d\n", err);
        bpf_object__close(obj);
        return 1;
    }

    struct bpf_program* prog_ret =
    bpf_object__find_program_by_name(obj, "kretprobe_tcp_recvmsg");
    if (!prog_ret) {
        fprintf(stderr, "Failed to find program kretprobe_tcp_recvmsg\n");
        bpf_link__destroy(link_entry);
        bpf_object__close(obj);
        return 1;
    }
    link_ret = bpf_program__attach(prog_ret);
    if (libbpf_get_error(link_ret)) {
        int err = -libbpf_get_error(link_ret);
        fprintf(stderr, "Failed to attach kretprobe_tcp_recvmsg: %d\n", err);
        bpf_link__destroy(link_entry);
        bpf_object__close(obj);
        return 1;
    }

    // printf("[ebpf_syncd] Program attached\n"); // DEBUG

    // pthread_t tid_stats;
    // pthread_create(&tid_stats, NULL, stats_printer_thread, NULL);
    // pthread_detach(tid_stats);

    //创建ringbuffer
	ringbuf = ring_buffer__new(events_map_fd, handle_event, NULL, NULL);
    if(libbpf_get_error(ringbuf)){
        int err = -libbpf_get_error(ringbuf);
        fprintf(stderr, "Failed to create ring buffer: %d\n", err);
        g_running = 0;
    }else{
        // printf("[ebpf_syncd] Ring buffer created\n"); // DEBUG
        while(!stop && g_running){
            int pret = ring_buffer__poll(ringbuf, 0);
            if(pret < 0 && pret != -EINTR){
                fprintf(stderr, "Error polling ring buffer: %d\n", pret);
                break;
            }
        }
    }


    // printf("[ebpf_syncd] Ring buffer created, entering main loop\n"); // DEBUG


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

    if (g_rx_stream) {
        kvs_free(g_rx_stream);
        g_rx_stream = NULL;
        g_rx_stream_len = 0;
        g_rx_stream_cap = 0;
    }

    bpf_link__destroy(link_ret);
    bpf_link__destroy(link_entry);
	if (obj && !libbpf_get_error(obj)) {
		bpf_object__close(obj);
	}
    mem_destroy();

    printf("[ebpf_syncd] Exited cleanly\n");
    return 0;
}
