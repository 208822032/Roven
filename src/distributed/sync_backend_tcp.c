#include "sync_backend.h"
#include "msg_queue.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <kvs_config.h>
#include "distributed.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include "kvs_memory.h"


// 同步队列元素
typedef struct sync_msg {
    size_t len;
    char data[KVS_MAX_RESPONSE_SIZE];
} sync_msg_t;

// TCP backend 的全局队列和线程
static msg_queue_t g_tcp_queue;
static int         g_tcp_running = 0;
static pthread_t   g_tcp_thread;

extern distributed_config_t global_dist_config;


// TCP 后端广播线程：从 g_tcp_queue 取出消息，发给所有 slave
static void *tcp_backend_worker(void *arg)
{
    (void)arg;
    // printf("[SYNC_TCP] backend worker started\n");

    while (g_tcp_running) {
        void *item = NULL;
        int r = msg_queue_pop(&g_tcp_queue, &item);
        if (r != 0) {
            // 队列关闭或出错，退出
            break;
        }
        if (!item) continue;

        sync_msg_t *msg = (sync_msg_t*)item;

        pthread_mutex_lock(&global_dist_config.slave_mutex);
        for (int i = 0; i < global_dist_config.slave_count; ++i) {
            slave_info_t *slave = &global_dist_config.slaves[i];
            ssize_t n = send(slave->sockfd, msg->data, msg->len, MSG_NOSIGNAL);
            // printf("[SYNC_TCP] sent %zd bytes to slave %s:%d\n", n,
            //        inet_ntoa(slave->addr.sin_addr),
            //        ntohs(slave->addr.sin_port));
            if (n < 0) {
                int err = errno;
                fprintf(stderr, "[SYNC_TCP] send to slave %s:%d failed: %s\n",
                        inet_ntoa(slave->addr.sin_addr),
                        ntohs(slave->addr.sin_port),
                        strerror(err));
                // 连接断开，具体清理由别处完成（如 slave_sync_thread/health_check）
                continue;
            }
        }
        pthread_mutex_unlock(&global_dist_config.slave_mutex);

        kvs_free(msg);
    }

    // printf("[SYNC_TCP] backend worker stopped\n");
    return NULL;
}

// sync_backend 接口（TCP 版本）
int sync_backend_init(int sync_port){
    (void)sync_port; // TCP backend 不需要额外端口配置

    if (msg_queue_init(&g_tcp_queue, 4096) != 0) {
        fprintf(stderr, "[SYNC_TCP] msg_queue_init failed\n");
        return -1;
    }
    g_tcp_running = 1;

    if (pthread_create(&g_tcp_thread, NULL, tcp_backend_worker, NULL) != 0) {
        fprintf(stderr, "[SYNC_TCP] create worker thread failed\n");
        g_tcp_running = 0;
        msg_queue_destroy(&g_tcp_queue);
        return -1;
    }
    return 0;
}

// 向所有 slave 广播一条已经编码好的 master_slave_proto 消息
int sync_backend_broadcast(const void *data, size_t len){
    if (!data || len == 0) return -1;
    if (!g_tcp_running)   return -1;

    sync_msg_t *msg = (sync_msg_t*)kvs_malloc(sizeof(sync_msg_t));
    if (!msg) {
        fprintf(stderr, "[SYNC_TCP] alloc sync_msg failed\n");
        return -1;
    }
    msg->len = len <= sizeof(msg->data) ? len : sizeof(msg->data);
    memcpy(msg->data, data, msg->len);

    if (msg_queue_push(&g_tcp_queue, msg) != 0) {
        fprintf(stderr, "[SYNC_TCP] msg_queue_push failed\n");
        kvs_free(msg);
        return -1;
    }
    return 0;
}

// 关闭 / 清理
void sync_backend_shutdown(void){
    g_tcp_running = 0;
    msg_queue_close(&g_tcp_queue);
    pthread_join(g_tcp_thread, NULL);
    msg_queue_destroy(&g_tcp_queue);
}
