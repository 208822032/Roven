#include "distributed.h"
#include "kvs_api.h"
#include "resp_protocol.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "sync_mode.h"
#include "msg_queue.h"
#include "master_slave_proto.h"
#include "kvs_memory.h"
#include "kvs_globals.h"
#include "kvstore.h"
#include "kvs_command_executor.h"
#include "sync_backend.h"
#include <stdint.h>
#include "kvs_log.h"

distributed_config_t global_dist_config;

typedef struct sync_msg {
    size_t len;
    uint8_t payload[];
} sync_msg_t;

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

static int master_sync_key_expire_at(const char *key, size_t key_len, int64_t expire_at_ms)
{
    if (!key || expire_at_ms < 0) {
        return -1;
    }

    char expire_buf[32];
    snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)expire_at_ms);
    return master_sync_command("PEXPIREAT", key, key_len, expire_buf, strlen(expire_buf));
}

static void distributed_set_slave_master_sockfd(int sockfd) {
    pthread_mutex_lock(&global_dist_config.slave_mutex);
    global_dist_config.slave_master_sockfd = sockfd;
    pthread_mutex_unlock(&global_dist_config.slave_mutex);
}

static void distributed_clear_slave_master_sockfd(int sockfd) {
    pthread_mutex_lock(&global_dist_config.slave_mutex);
    if (global_dist_config.slave_master_sockfd == sockfd) {
        global_dist_config.slave_master_sockfd = -1;
    }
    pthread_mutex_unlock(&global_dist_config.slave_mutex);
}

static const char *master_full_sync_snapshot_entry_command(
    kvs_full_sync_snapshot_backend_t backend
) {
    switch (backend) {
        case KVS_FULL_SYNC_SNAPSHOT_BACKEND_ARRAY:
            return "SET";
        case KVS_FULL_SYNC_SNAPSHOT_BACKEND_RBTREE:
            return "RSET";
        case KVS_FULL_SYNC_SNAPSHOT_BACKEND_HASH:
            return "HSET";
        case KVS_FULL_SYNC_SNAPSHOT_BACKEND_SKIPTABLE:
            return "SSET";
        default:
            return NULL;
    }
}

static int master_send_full_sync_snapshot(
    const kvs_full_sync_snapshot_t *snapshot
) {
    size_t i;

    if (!snapshot) {
        return -1;
    }

    for (i = 0; i < snapshot->entry_count; ++i) {
        const kvs_full_sync_snapshot_entry_t *entry = &snapshot->entries[i];
        const char *cmd = master_full_sync_snapshot_entry_command(entry->backend);

        if (!cmd) {
            return -1;
        }
        if (master_sync_command(
                cmd,
                entry->key,
                entry->key_len,
                entry->value,
                entry->value_len) != 0) {
            return -1;
        }
        if (entry->expire_at_ms >= 0 &&
            master_sync_key_expire_at(
                entry->key,
                entry->key_len,
                entry->expire_at_ms) != 0) {
            return -1;
        }
    }

    return 0;
}

//每个slave同步线程 先全量 再增量
void* slave_sync_thread(void *arg){

    slave_info_t *slave = (slave_info_t*)arg;
    int sockfd = slave->sockfd;
    kvs_full_sync_snapshot_t *snapshot = NULL;

    KVS_LOG_INFO("dist", "slave connected from %s:%d",
                 inet_ntoa(slave->addr.sin_addr),
                 ntohs(slave->addr.sin_port));

    if (kvs_command_executor_build_full_sync_snapshot(&snapshot) != 0 || !snapshot) {
        KVS_LOG_ERROR("dist", "failed to build full sync snapshot for slave %s:%d",
                      inet_ntoa(slave->addr.sin_addr),
                      ntohs(slave->addr.sin_port));
        goto cleanup_slave;
    }

    if (master_send_full_sync_snapshot(snapshot) != 0) {
        KVS_LOG_ERROR("dist", "failed to send full sync snapshot to slave %s:%d",
                      inet_ntoa(slave->addr.sin_addr),
                      ntohs(slave->addr.sin_port));
        goto cleanup_slave;
    }

    kvs_full_sync_snapshot_destroy(snapshot);
    snapshot = NULL;

    char tmp[16];
    while (1) {
        ssize_t n = recv(sockfd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
    }

cleanup_slave:
    if (snapshot) {
        kvs_full_sync_snapshot_destroy(snapshot);
        snapshot = NULL;
    }
    close(sockfd);

    // 从全局 slave 列表中移除
    pthread_mutex_lock(&global_dist_config.slave_mutex);
    for (int i = 0; i < global_dist_config.slave_count; ++i) {
        if (global_dist_config.slaves[i].sockfd == sockfd) {
            global_dist_config.slaves[i] =
                global_dist_config.slaves[global_dist_config.slave_count - 1];
            global_dist_config.slave_count--;
            break;
        }
    }
    pthread_mutex_unlock(&global_dist_config.slave_mutex);

    kvs_free(slave);
    return NULL;
}

//主节点同步服务器线程
void *master_sync_server(void *arg){
    int server_fd, client_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    if((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0){
        KVS_LOG_ERROR_ERRNO("dist", "socket failed in master sync server");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(global_dist_config.sync_port);

    if(bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0){
        KVS_LOG_ERROR_ERRNO("dist", "bind failed in master sync server on port %d", global_dist_config.sync_port);
        exit(EXIT_FAILURE);
    }

    if(listen(server_fd, 3) < 0){
        KVS_LOG_ERROR_ERRNO("dist", "listen failed in master sync server on port %d", global_dist_config.sync_port);
        exit(EXIT_FAILURE);
    }

    KVS_LOG_INFO("dist", "master sync server listening on port %d", global_dist_config.sync_port);

    global_dist_config.master_listen_fd = server_fd;

    while (!g_shutdown_flag) {
        if ((client_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            if (g_shutdown_flag) break;
            KVS_LOG_ERROR_ERRNO("dist", "accept failed in master sync server");
            continue;
        }

        pthread_mutex_lock(&global_dist_config.slave_mutex);

        //增加从节点
        if(global_dist_config.slave_count < global_dist_config.max_slaves){
            slave_info_t *slave = kvs_malloc(sizeof(slave_info_t));
            if (!slave) {
                KVS_LOG_ERROR("dist", "malloc slave_info failed");
                close(client_fd);
                pthread_mutex_unlock(&global_dist_config.slave_mutex);
                continue;
            }

            slave->sockfd = client_fd;
            memcpy(&slave->addr, &address, sizeof(address));

            // 先放入列表，便于管理
            global_dist_config.slaves[global_dist_config.slave_count++] = *slave;
            
            //开辟线程处理从节点
            // 启动该 slave 的「全量 + 增量」同步线程
            if (pthread_create(&slave->thread, NULL, slave_sync_thread, slave) != 0) {
                KVS_LOG_ERROR_ERRNO("dist", "create slave_sync_thread failed");
                global_dist_config.slave_count--;
                close(client_fd);
                kvs_free(slave);
            } 
        }else{
            close(client_fd);
            KVS_LOG_WARN("dist", "max slave reached, connection refused");
        }

        pthread_mutex_unlock(&global_dist_config.slave_mutex);
    }

    close(server_fd);
    global_dist_config.master_listen_fd = -1;
    KVS_LOG_INFO("dist", "master sync server stopped");
    return NULL;
}

void* slave_connect_thread(void *arg) {
    // 等待主节点初始化完成
    sleep(1);
    if (g_shutdown_flag) {
        return NULL;
    }
    slave_connect_master();
    return NULL;
}

void distributed_shutdown(void) {
    if (global_dist_config.role == ROLE_MASTER) {
        // 1. 唤醒并停止 master_sync_server 线程
        if (global_dist_config.master_listen_fd >= 0) {
            shutdown(global_dist_config.master_listen_fd, SHUT_RDWR);
            close(global_dist_config.master_listen_fd);
            global_dist_config.master_listen_fd = -1;
        }
        if (global_dist_config.master_thread_started) {
            pthread_join(global_dist_config.master_thread, NULL);
            global_dist_config.master_thread_started = 0;
        }

        // 2. 等待所有 slave 同步线程退出
        pthread_mutex_lock(&global_dist_config.slave_mutex);
        int count = global_dist_config.slave_count;
        pthread_t *threads = NULL;
        if (count > 0) {
            threads = kvs_malloc(sizeof(pthread_t) * count);
            if (threads) {
                for (int i = 0; i < count; ++i) {
                    threads[i] = global_dist_config.slaves[i].thread;
                }
            }
        }
        pthread_mutex_unlock(&global_dist_config.slave_mutex);

        for (int i = 0; i < count; ++i) {
            if (threads) pthread_join(threads[i], NULL);
        }
        if (threads) kvs_free(threads);

        sync_backend_shutdown();

        // 3. 释放 slave 数组
        if (global_dist_config.slaves) {
            kvs_free(global_dist_config.slaves);
            global_dist_config.slaves = NULL;
            global_dist_config.slave_count = 0;
        }
    } else if (global_dist_config.role == ROLE_SLAVE) {
        pthread_t connect_thread;
        int should_join = 0;
        int master_sockfd = -1;

        pthread_mutex_lock(&global_dist_config.slave_mutex);
        should_join = global_dist_config.slave_connect_thread_started;
        connect_thread = global_dist_config.slave_connect_thread;
        master_sockfd = global_dist_config.slave_master_sockfd;
        pthread_mutex_unlock(&global_dist_config.slave_mutex);

        if (master_sockfd >= 0) {
            shutdown(master_sockfd, SHUT_RDWR);
        }

        if (should_join) {
            pthread_join(connect_thread, NULL);
            pthread_mutex_lock(&global_dist_config.slave_mutex);
            global_dist_config.slave_connect_thread_started = 0;
            global_dist_config.slave_master_sockfd = -1;
            pthread_mutex_unlock(&global_dist_config.slave_mutex);
        }
    }

    pthread_mutex_destroy(&global_dist_config.slave_mutex);
}

//初始化分布式系统
int distributed_init(node_role_t role, const char *master_ip, int master_port, int sync_port){
    memset(&global_dist_config, 0, sizeof(distributed_config_t));

    global_dist_config.role = role;
    global_dist_config.sync_port = sync_port;
    global_dist_config.master_listen_fd = -1;
    global_dist_config.slave_master_sockfd = -1;
    pthread_mutex_init(&global_dist_config.slave_mutex, NULL);

    if(role == ROLE_MASTER){
        global_dist_config.max_slaves = 10;
        global_dist_config.slaves = kvs_malloc(global_dist_config.max_slaves * sizeof(slave_info_t));
        global_dist_config.master_thread_started = 0;

        //无论哪种模式 都启动同步服务器线程
        pthread_create(&global_dist_config.master_thread, NULL, master_sync_server, NULL);
        global_dist_config.master_thread_started = 1;

        // 初始化 sync backend（当前实现为 TCP backend）
        if (sync_backend_init(sync_port) != 0) {
            KVS_LOG_ERROR("dist", "sync_backend_init failed");
            return -1;
        }

        KVS_LOG_INFO("dist", "running as MASTER, sync port: %d", sync_port);

    }else{
        //从节点
        strncpy(global_dist_config.master_ip, master_ip, sizeof(global_dist_config.master_ip)-1);
        global_dist_config.master_port = master_port;

        KVS_LOG_INFO("dist", "running as SLAVE, connecting to master %s:%d", master_ip, master_port);

        // 启动连接线程
        if (pthread_create(&global_dist_config.slave_connect_thread, NULL, slave_connect_thread, NULL) == 0) {
            global_dist_config.slave_connect_thread_started = 1;
        } else {
            KVS_LOG_ERROR_ERRNO("dist", "create slave_connect_thread failed");
        }
    }

    return 0;
}

//从节点连接主节点
void slave_connect_master(){

    int sock = 0;
    struct sockaddr_in serv_addr;

    if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        KVS_LOG_ERROR_ERRNO("dist", "socket creation error while connecting to master");
        return;
    }
    distributed_set_slave_master_sockfd(sock);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(global_dist_config.master_port);

    if(inet_pton(AF_INET, global_dist_config.master_ip, &serv_addr.sin_addr) <= 0){
        KVS_LOG_ERROR("dist", "invalid master address %s", global_dist_config.master_ip);
        distributed_clear_slave_master_sockfd(sock);
        close(sock);
        return;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        KVS_LOG_ERROR_ERRNO("dist", "connection to master %s:%d failed", global_dist_config.master_ip, global_dist_config.master_port);
        distributed_clear_slave_master_sockfd(sock);
        close(sock);
        return;
    }

    KVS_LOG_INFO("dist", "connected to master, starting synchronization...");


    //接收同步数据
    // char buffer[KVS_MAX_RESPONSE_SIZE] = {0};
    size_t buf_cap = KVS_MAX_RESPONSE_SIZE;
    uint8_t *buffer = kvs_malloc(buf_cap);
    if(!buffer){
        KVS_LOG_ERROR("dist", "malloc buffer failed on slave sync thread");
        close(sock);
        return;
    }
    size_t recv_len = 0;

    //解析命令
    while(!g_shutdown_flag){
        if(recv_len >= buf_cap){
            size_t new_cap = buf_cap * 2;
            if(new_cap < buf_cap){
                KVS_LOG_ERROR("dist", "buffer size overflow on slave sync thread");
                break;
            }
            uint8_t* tmp = (uint8_t*)kvs_realloc(buffer, new_cap);
            if(!tmp){
                KVS_LOG_ERROR("dist", "realloc buffer failed on slave sync thread");
                break;
            }
            buffer = tmp;
            buf_cap = new_cap;
        }

        ssize_t n = recv(sock, buffer + recv_len, buf_cap - recv_len, 0);

        if(n == 0){
            KVS_LOG_WARN("dist", "master closed connection");
            break;
        }else if(n < 0){
            if (errno == EINTR) continue;
            KVS_LOG_ERROR_ERRNO("dist", "recv error on slave sync thread");
            break;
        }
        recv_len += (size_t)n;

        size_t parsed_total = 0;
        while(parsed_total < recv_len){
            ms_op_t op;
            const uint8_t *k, *v;
            size_t klen, vlen;

            int consumed = ms_decode_message(
                buffer + parsed_total,
                recv_len - parsed_total,
                &op,
                &k, &klen,
                &v, &vlen
            );

            if (consumed == 0) {
                // 数据不够一条完整消息，等待更多
                break;
            } else if (consumed < 0) {
                KVS_LOG_ERROR("dist", "ms_decode_message error, drop buffer");
                recv_len = 0;
                break;
            }

            
            // 解析出一条完整命令 (op, key, val)
            // 注意：k/v 不一定以 '\0' 结尾，这里临时拷贝+补 '\0'
            char *key  = (char *)kvs_malloc(klen + 1);
            char *val  = NULL;
            if (!key) {
                KVS_LOG_ERROR("dist", "malloc key failed on slave sync thread");
                parsed_total += (size_t)consumed;
                continue;
            }
            memcpy(key, k, klen);
            key[klen] = '\0';

            if (vlen > 0) {
                val = (char *)kvs_malloc(vlen + 1);
                if (!val) {
                    KVS_LOG_ERROR("dist", "malloc val failed on slave sync thread");
                    kvs_free(key);
                    parsed_total += (size_t)consumed;
                    continue;
                }
                memcpy(val, v, vlen);
                val[vlen] = '\0';
            }

            int should_apply = 1;
            int64_t parsed_expire_at = 0;

            if (op == MS_OP_TTL_PEXPIREAT) {
                char *endptr = NULL;
                errno = 0;
                long long parsed_expire = 0;
                if (!val) {
                    KVS_LOG_ERROR("dist", "missing PEXPIREAT payload for key=%s", key);
                    should_apply = 0;
                } else {
                    parsed_expire = strtoll(val, &endptr, 10);
                    if (endptr == val || *endptr != '\0' || errno == ERANGE) {
                        KVS_LOG_ERROR("dist", "invalid PEXPIREAT payload for key=%s", key);
                        should_apply = 0;
                    } else {
                        parsed_expire_at = (int64_t)parsed_expire;
                    }
                }
            }

            switch (op) {
                case MS_OP_ARRAY_SET:
                case MS_OP_RBTREE_RSET:
                case MS_OP_HASH_HSET:
                case MS_OP_SKIPTABLE_SSET:
                case MS_OP_ARRAY_DEL:
                case MS_OP_RBTREE_RDEL:
                case MS_OP_HASH_HDEL:
                case MS_OP_SKIPTABLE_SDEL:
                case MS_OP_ARRAY_MOD:
                case MS_OP_RBTREE_RMOD:
                case MS_OP_HASH_HMOD:
                case MS_OP_SKIPTABLE_SMOD:
                case MS_OP_PAPERSET:
                case MS_OP_PAPERDEL:
                case MS_OP_PAPERMOD:
                case MS_OP_TTL_PEXPIREAT:
                case MS_OP_TTL_PERSIST:
                    break;
                default:
                    KVS_LOG_WARN("dist", "unknown operation %d on slave sync thread", op);
                    should_apply = 0;
                    break;
            }

            if (should_apply && !g_shutdown_flag) {
                if (kvs_command_executor_submit_replicated(
                        op,
                        key,
                        klen,
                        val,
                        vlen,
                        parsed_expire_at) != 0) {
                    KVS_LOG_ERROR("dist", "submit replicated command failed, op=%d key=%s", op, key);
                }
            }

            kvs_free(key);
            if (val) kvs_free(val);

            parsed_total += (size_t)consumed;
        }

        if (parsed_total > 0 && parsed_total <= recv_len) {
            memmove(buffer, buffer + parsed_total, recv_len - parsed_total);
            recv_len -= parsed_total;
        }
    }

    kvs_free(buffer);
    distributed_clear_slave_master_sockfd(sock);
    close(sock);
    return;
}


//主节点同步写操作到所有从节点
int master_sync_command(const char *cmd,
                        const char *key, size_t key_len,
                        const char *value, size_t value_len){

    if(global_dist_config.role != ROLE_MASTER) return 0;
    if(!key || !cmd) return -1;

    ms_op_t op;
    if(strcmp(cmd, "SET") == 0){
        op = MS_OP_ARRAY_SET;
    }else if(strcmp(cmd, "RSET") == 0){
        op = MS_OP_RBTREE_RSET;
    }else if(strcmp(cmd, "HSET") == 0){
        op = MS_OP_HASH_HSET;
    }else if(strcmp(cmd, "SSET") == 0){ 
        op = MS_OP_SKIPTABLE_SSET;
    }else if(strcmp(cmd, "DEL") == 0){
        op = MS_OP_ARRAY_DEL;
    }else if(strcmp(cmd, "RDEL") == 0){
        op = MS_OP_RBTREE_RDEL;
    }else if(strcmp(cmd, "HDEL") == 0){
        op = MS_OP_HASH_HDEL;
    }else if(strcmp(cmd, "SDEL") == 0){
        op = MS_OP_SKIPTABLE_SDEL;
    }else if(strcmp(cmd, "MOD") == 0){
        op = MS_OP_ARRAY_MOD;
    }else if(strcmp(cmd, "RMOD") == 0){
        op = MS_OP_RBTREE_RMOD;
    }else if(strcmp(cmd, "HMOD") == 0){
        op = MS_OP_HASH_HMOD;
    }else if(strcmp(cmd, "SMOD") == 0){
        op = MS_OP_SKIPTABLE_SMOD;
    }else if(strcmp(cmd, "PAPERSET") == 0){
        op = MS_OP_PAPERSET;
    }else if(strcmp(cmd, "PAPERDEL") == 0){
        op = MS_OP_PAPERDEL;
    }else if(strcmp(cmd, "PAPERMOD") == 0){
        op = MS_OP_PAPERMOD;
    }else if(strcmp(cmd, "PEXPIREAT") == 0){
        op = MS_OP_TTL_PEXPIREAT;
    }else if(strcmp(cmd, "PERSIST") == 0){
        op = MS_OP_TTL_PERSIST;
    }else{
        //未知命令
        return -1;
    }

    size_t needed = (size_t)MS_HEADER_LEN + key_len + value_len;
    sync_msg_t *msg = (sync_msg_t *)kvs_malloc(sizeof(sync_msg_t) + needed);
    if (!msg) {
        KVS_LOG_ERROR("dist", "master_sync_command malloc failed, cmd=%s key=%s", cmd, key);
        return -1;
    }

    int len = ms_encode_message(
        msg->payload,
        needed,
        op,
        key, key_len,
        value, value_len
    );
    if (len <= 0) {
        KVS_LOG_ERROR("dist", "master_sync_command encode error, cmd=%s key=%s", cmd, key);
        kvs_free(msg);
        return -1;
    }

    msg->len = (size_t)len;

    //  fprintf(stderr, "[DIST] master_sync_command: op=%d cmd=%s key=%s\n",
    //         op, cmd, key);

    if (sync_backend_broadcast_owned(msg) != 0) {
        KVS_LOG_ERROR("dist", "sync_backend_broadcast failed, cmd=%s key=%s", cmd, key);
        kvs_free(msg);
        return -1;
    } else {
        // fprintf(stderr, "[DIST] sync_backend_broadcast ok, len=%d\n", len);
    }

    return 0;
}
