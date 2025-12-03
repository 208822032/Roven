// 用户态 eBPF 加载与守护进程：抓 tcp_sendmsg 包，入队，子线程分发
#include "sync_mode.h"


#if (SYNC_MODE == SYNC_MODE_EBPF)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "distributed.h"
#include "msg_queue.h"
#include "kvs_config.h"
#include "arpa/inet.h" 
#include "kvs_memory.h" 

// 这里将 EBPF_KVS_MAX_RESPONSE_SIZE 绑定到配置中的 KVS_MAX_RESPONSE_SIZE
#ifndef EBPF_KVS_MAX_RESPONSE_SIZE
#define EBPF_KVS_MAX_RESPONSE_SIZE  KVS_MAX_RESPONSE_SIZE
#endif

#include "ebpf_sync_shared.h"


// 全局 BPF 对象
static struct bpf_object *bpf_obj = NULL;
static struct bpf_link   *kprobe_link = NULL;
static struct ring_buffer *rb = NULL;

static pthread_t rb_thread;
static pthread_t dispatch_thread;
static volatile int running = 0;

extern distributed_config_t global_dist_config;

//使用msg_queue作为本地队列
static msg_queue_t g_ebpf_queue;
static int        g_ebpf_running = 0;

//通过队列传递的消息
typedef struct sync_msg {
    size_t len;
    char data[KVS_MAX_RESPONSE_SIZE];
} ebpf_sync_msg_t;


// libbpf 调试输出（可按需开启）
static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
    if (level == LIBBPF_DEBUG)
        return 0; // 太吵可以屏蔽
    return vfprintf(stderr, format, args);
}


// 用户态 stub：直接把“同步事件”投递到本地队列，
// 供 testbpf 等单元测试调用，而不依赖真实 eBPF 事件。
int ebpf_emit_sync_event(const char *buf, size_t len)
{
    if (!buf || len == 0) return -1;

    ebpf_sync_msg_t *msg = (ebpf_sync_msg_t *)kvs_malloc(sizeof(ebpf_sync_msg_t));
    if (!msg) {
        fprintf(stderr, "[EBPF_SYNC] ebpf_emit_sync_event malloc failed\n");
        return -1;
    }

    msg->len = len <= KVS_MAX_RESPONSE_SIZE ? len : KVS_MAX_RESPONSE_SIZE;
    if (msg->len > 0) {
        memcpy(msg->data, buf, msg->len);
    }

    if (msg_queue_push(&g_ebpf_queue, msg) != 0) {
        fprintf(stderr, "[EBPF_SYNC] ebpf_emit_sync_event msg_queue_push failed\n");
        kvs_free(msg);
        return -1;
    }
    return 0;
}

// ringbuf 回调：把内核事件放入本地队列
static int handle_sync_event(void *ctx, void *data, size_t size)
{
    (void)ctx;

    //头部
    size_t headr_size = offsetof(struct net_sync_event, data);
    if (size < headr_size) {
        fprintf(stderr, "[EBPF_SYNC] event size too small=%zu, expect at least=%zu\n",
                size, headr_size);
        return 0;
    }

    struct net_sync_event *evt = (struct net_sync_event *)data;

    //实际长度 payload = 事件总大小 - 头部
    size_t payload_len = size - headr_size;

    //需要复制的长度
    size_t copy_len = evt->len;
    if(copy_len > payload_len){
        copy_len = payload_len;
    }
    if(copy_len > EBPF_KVS_MAX_RESPONSE_SIZE){
        copy_len = EBPF_KVS_MAX_RESPONSE_SIZE;
    }

    // fprintf(stderr, "[EBPF_SYNC] ringbuf event: len=%u dport=%u pid=%u\n",
    //         evt->len, evt->dport, evt->pid);
    
    ebpf_sync_msg_t *msg = (ebpf_sync_msg_t *)kvs_malloc(sizeof(ebpf_sync_msg_t));
    if (!msg) {
        fprintf(stderr, "[EBPF_SYNC] alloc sync_msg failed\n");
        return 0;
    }

    msg->len = copy_len;
    if(copy_len > 0){
        memcpy(msg->data, evt->data, copy_len);
    }

    if (msg_queue_push(&g_ebpf_queue, msg) != 0) {
        fprintf(stderr, "[EBPF_SYNC] msg_queue_push failed, drop event\n");
        kvs_free(msg);
    }
    return 0;
}

// 轮询 ringbuf 的线程
static void *ebpf_sync_daemon(void *arg)
{
    (void)arg;
    // printf("[EBPF_SYNC] ringbuf daemon started\n");
    while (running) {
        int err = ring_buffer__poll(rb, 100); // 100ms
        if (err < 0 && err != -EINTR) {
            fprintf(stderr, "[EBPF_SYNC] ring_buffer__poll error: %d\n", err);
            break;
        }
    }
    // printf("[EBPF_SYNC] ringbuf daemon stopped\n");
    return NULL;
}

// 分发线程：从队列拿出 RESP 命令，直接转发给所有 slave
static void *ebpf_sync_dispatcher(void *arg)
{
    (void)arg;
    // printf("[EBPF_SYNC] dispatcher started\n");

    while(g_ebpf_running){
        void* item = NULL;
        int recv_msg = msg_queue_pop(&g_ebpf_queue, &item);
        if(recv_msg != 0){
            //队列关闭或出错
            usleep(100);
            continue;
        }
        if(!item){
            continue;
        }

        ebpf_sync_msg_t *msg = (ebpf_sync_msg_t*)item;

            // fprintf(stderr, "[EBPF_SYNC] dispatcher got msg len=%zu, slaves=%d\n",
            //         msg->len, global_dist_config.slave_count);

            if (global_dist_config.role == ROLE_MASTER) {
                int local_count = 0;
                int fds[64];

                pthread_mutex_lock(&global_dist_config.slave_mutex);
                local_count = global_dist_config.slave_count;
                if (local_count > (int)(sizeof(fds) / sizeof(fds[0]))) {
                    local_count = (int)(sizeof(fds) / sizeof(fds[0]));
                }
                for (int i = 0; i < local_count; i++) {
                    fds[i] = global_dist_config.slaves[i].sockfd;
                }
                pthread_mutex_unlock(&global_dist_config.slave_mutex);

                for (int i = 0; i < local_count; i++) {
                    ssize_t n = send(fds[i], msg->data, msg->len, MSG_NOSIGNAL);
                    if (n < 0) {
                        int err = errno;
                        fprintf(stderr, "[EBPF_SYNC] send to slave fd=%d failed: %s\n",
                                fds[i], strerror(err));
                        if (err == EPIPE || err == ECONNRESET || err == ETIMEDOUT) {
                            continue;
                        }
                    }
                }
            }
            kvs_free(msg);
    }

    // printf("[EBPF_SYNC] dispatcher stopped\n");
    return NULL;
}


// 初始化 eBPF：挂 kprobe/tcp_sendmsg，并设置 sync_port
int ebpf_sync_init(int sync_port)
{
    int err;

    //初始化msg_queue
    if(msg_queue_init(&g_ebpf_queue, 65535) != 0){
        fprintf(stderr, "[EBPF_SYNC] msg_queue_init failed\n");
        return -1;
    }

    g_ebpf_running = 1;

    bpf_obj = bpf_object__open_file("src/distributed/ebpf_sync.bpf.o", NULL);
    if (libbpf_get_error(bpf_obj)) {
        fprintf(stderr, "[EBPF_SYNC] open BPF object failed\n");
        bpf_obj = NULL;
        return -1;
    }

    if (bpf_object__load(bpf_obj)) {
        fprintf(stderr, "[EBPF_SYNC] load BPF object failed\n");
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
        return -1;
    }

    // 查找并附加 kprobe 程序
    struct bpf_program *prog = bpf_object__find_program_by_name(bpf_obj, "handle_tcp_sendmsg");
    if (!prog) {
        fprintf(stderr, "[EBPF_SYNC] cannot find program handle_tcp_sendmsg\n");
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
        return -1;
    }

    kprobe_link = bpf_program__attach(prog);
    if (libbpf_get_error(kprobe_link)) {
        fprintf(stderr, "[EBPF_SYNC] attach kprobe failed: %ld\n",
                libbpf_get_error(kprobe_link));
        kprobe_link = NULL;
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
        return -1;
    }

    // 写入 sync_port 到 BPF map sync_conf[0]
    int conf_fd = bpf_object__find_map_fd_by_name(bpf_obj, "sync_conf");
    if (conf_fd < 0) {
        fprintf(stderr, "[EBPF_SYNC] cannot find map sync_conf\n");
        bpf_link__destroy(kprobe_link);
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
        return -1;
    }
    __u32 idx = 0;
    __u16 port = (__u16)sync_port;
    err = bpf_map_update_elem(conf_fd, &idx, &port, 0);
    if (err) {
        fprintf(stderr, "[EBPF_SYNC] update sync_conf failed: %d\n", err);
        bpf_link__destroy(kprobe_link);
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
        return -1;
    }

    // 创建 ringbuf
    int map_fd = bpf_object__find_map_fd_by_name(bpf_obj, "sync_events");
    if (map_fd < 0) {
        fprintf(stderr, "[EBPF_SYNC] cannot find map sync_events\n");
        bpf_link__destroy(kprobe_link);
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
        return -1;
    }

    rb = ring_buffer__new(map_fd, handle_sync_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "[EBPF_SYNC] create ring buffer failed\n");
        msg_queue_destroy(&g_ebpf_queue);
        g_ebpf_running = 0;
        bpf_link__destroy(kprobe_link);
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
        return -1;
    }

    running = 1;
    if (pthread_create(&rb_thread, NULL, ebpf_sync_daemon, NULL) != 0) {
        fprintf(stderr, "[EBPF_SYNC] create rb_thread failed\n");
        running = 0;
        ring_buffer__free(rb);
        bpf_link__destroy(kprobe_link);
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
        return -1;
    }

    if (pthread_create(&dispatch_thread, NULL, ebpf_sync_dispatcher, NULL) != 0) {
        fprintf(stderr, "[EBPF_SYNC] create dispatch_thread failed\n");
        running = 0;
        pthread_join(rb_thread, NULL);
        ring_buffer__free(rb);
        bpf_link__destroy(kprobe_link);
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
        return -1;
    }

    // printf("[EBPF_SYNC] initialized: sync_port=%d (kprobe tcp_sendmsg)\n", sync_port);
    return 0;
}

void ebpf_sync_cleanup(void)
{
    running = 0;    
    g_ebpf_running = 0;


    pthread_join(rb_thread, NULL);
    pthread_join(dispatch_thread, NULL);

    msg_queue_destroy(&g_ebpf_queue);

    if (rb) {
        ring_buffer__free(rb);
        rb = NULL;
    }
    if (kprobe_link) {
        bpf_link__destroy(kprobe_link);
        kprobe_link = NULL;
    }
    if (bpf_obj) {
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
    }

    // printf("[EBPF_SYNC] cleaned up\n");
}

#endif