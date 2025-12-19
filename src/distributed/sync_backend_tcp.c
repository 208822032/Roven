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

typedef struct sync_msg {
    size_t len;
    uint8_t payload[];
} sync_msg_t;

// TCP backend 的全局队列和线程
static msg_queue_t g_tcp_queue;
static int         g_tcp_running = 0;
static pthread_t   g_tcp_thread;

extern distributed_config_t global_dist_config;

typedef struct {
    int sockfd;
    struct sockaddr_in addr;
} slave_target_t;

#if 1
// TCP 后端广播线程：从 g_tcp_queue 取出消息，发给所有 slave
static void *tcp_backend_worker(void *arg)
{
    (void)arg;
    // printf("[SYNC_TCP] backend worker started\n");

    while (g_tcp_running) {
        void *item = NULL;
        int r = msg_queue_pop(&g_tcp_queue, &item);
        if (r != 0) {
            //队列关闭或出错
            usleep(10);
            continue;
        }
        if (!item) continue;

        sync_msg_t *msg = (sync_msg_t*)item;

        int slave_count_snapshot = 0;
        slave_target_t* slaves_snapshot = NULL;
        pthread_mutex_lock(&global_dist_config.slave_mutex);
        if (global_dist_config.slave_count > 0) {
            slave_count_snapshot = global_dist_config.slave_count;
            slaves_snapshot = (slave_target_t *)kvs_malloc(
                sizeof(slave_target_t) * slave_count_snapshot);

            if (slaves_snapshot) {
                for (int i = 0; i < slave_count_snapshot; ++i) {
                    slave_info_t *slave = &global_dist_config.slaves[i];
                    slaves_snapshot[i].sockfd = slave->sockfd;
                    slaves_snapshot[i].addr   = slave->addr; // sockaddr_in 直接拷贝
                }
            } else {
                fprintf(stderr,
                        "[SYNC_TCP] WARNING: alloc slaves_snapshot failed, "
                        "fallback to dropping this msg\n");
            }
        }
        pthread_mutex_unlock(&global_dist_config.slave_mutex);

        /* ========= 2. 在锁外向各个 slave 发送数据 ========= */
        for (int i = 0; i < slave_count_snapshot; ++i) {
            slave_target_t *t = &slaves_snapshot[i];

            size_t total_sent = 0;
            while (total_sent < msg->len) {
                ssize_t n = send(t->sockfd,
                                 msg->payload + total_sent,
                                 msg->len - total_sent,
                                 MSG_NOSIGNAL);

                if (n < 0) {
                    int err = errno;
                    fprintf(stderr,
                            "[SYNC_TCP] send to slave %s:%d failed after "
                            "%zu/%zu bytes: %s\n",
                            inet_ntoa(t->addr.sin_addr),
                            ntohs(t->addr.sin_port),
                            total_sent, msg->len,
                            strerror(err));
                    // 出错就放弃该 slave，本线程不做清理，交给其它逻辑处理
                    break;
                }

                if (n == 0) {
                    fprintf(stderr,
                            "[SYNC_TCP] send returned 0 to slave %s:%d "
                            "(sent=%zu/%zu), closing stream\n",
                            inet_ntoa(t->addr.sin_addr),
                            ntohs(t->addr.sin_port),
                            total_sent, msg->len);
                    break;
                }

                total_sent += (size_t)n;
            }

            // 需要的话可以在这里加统计或 debug 日志
            // fprintf(stderr, "[SYNC_TCP] worker sent %zu/%zu bytes to slave %s:%d\n",
            //         total_sent, msg->len,
            //         inet_ntoa(t->addr.sin_addr),
            //         ntohs(t->addr.sin_port));
        }

        kvs_free(slaves_snapshot);

        kvs_free(msg);
    }

    // printf("[SYNC_TCP] backend worker stopped\n");
    return NULL;
}
#else
static void *tcp_backend_worker(void *arg)
{
    (void)arg;
    // printf("[SYNC_TCP] backend worker started\n");

    while (g_tcp_running) {
        void *item = NULL;
        int r = msg_queue_pop(&g_tcp_queue, &item);
        if (r != 0) {
            //队列关闭或出错
            usleep(100);
            continue;
        }
        if (!item) continue;

        sync_msg_t *msg = (sync_msg_t*)item;

        pthread_mutex_lock(&global_dist_config.slave_mutex);
        for (int i = 0; i < global_dist_config.slave_count; ++i) {
            slave_info_t *slave = &global_dist_config.slaves[i];

            size_t total_sent = 0;
            while (total_sent < msg->len) {
                ssize_t n = send(slave->sockfd,
                                 msg->payload + total_sent,
                                 msg->len - total_sent,
                                 MSG_NOSIGNAL);

                if (n < 0) {
                    int err = errno;
                    fprintf(stderr,
                            "[SYNC_TCP] send to slave %s:%d failed after %zu/%zu bytes: %s\n",
                            inet_ntoa(slave->addr.sin_addr),
                            ntohs(slave->addr.sin_port),
                            total_sent, msg->len,
                            strerror(err));
                    // 出错就放弃该 slave，交由其它逻辑清理
                    break;
                }

                if (n == 0) {
                    fprintf(stderr,
                            "[SYNC_TCP] send returned 0 to slave %s:%d (sent=%zu/%zu), closing stream\n",
                            inet_ntoa(slave->addr.sin_addr),
                            ntohs(slave->addr.sin_port),
                            total_sent, msg->len);
                    break;
                }

                total_sent += (size_t)n;
            }

            // fprintf(stderr,
            //         "[SYNC_TCP] worker sent %zu/%zu bytes to slave %s:%d\n",
            //         total_sent, msg->len,
            //         inet_ntoa(slave->addr.sin_addr),
            //         ntohs(slave->addr.sin_port));
        }
        pthread_mutex_unlock(&global_dist_config.slave_mutex);

        kvs_free(msg);
    }

    // printf("[SYNC_TCP] backend worker stopped\n");
    return NULL;
}

#endif

// sync_backend 接口（TCP 版本）
int sync_backend_init(int sync_port){
    (void)sync_port; // TCP backend 不需要额外端口配置

    if (msg_queue_init(&g_tcp_queue, 1048576) != 0) {
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

    if(len > KVS_MAX_RESPONSE_SIZE){
        fprintf(stderr, "[SYNC_TCP] message too large to broadcast: %zu bytes\n", len);
        return -1;
    }

    sync_msg_t *msg = (sync_msg_t*)kvs_malloc(sizeof(sync_msg_t) + len);
    if (!msg) {
        fprintf(stderr, "[SYNC_TCP] alloc sync_msg failed\n");
        return -1;
    }
    // msg->len = len <= sizeof(msg->data) ? len : sizeof(msg->data);
    // memcpy(msg->data, data, msg->len);
    msg->len = len;
    memcpy(msg->payload, data, len);

    if (msg_queue_push(&g_tcp_queue, msg) != 0) {
        fprintf(stderr, "[SYNC_TCP] msg_queue_push failed\n");
        kvs_free(msg);
        return -1;
    }
    // fprintf(stderr, "[SYNC_TCP] broadcast push len=%zu\n", len);
    return 0;
}

// 关闭 / 清理
void sync_backend_shutdown(void){
    g_tcp_running = 0;
    pthread_join(g_tcp_thread, NULL);
    msg_queue_destroy(&g_tcp_queue);
}
