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
#include "sync_backend.h"
#include <stdint.h>

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

#if ENABLE_AOF
// 将 master-slave 协议 op 映射为用户命令字符串，并写入 AOF（RESP）
static void slave_append_aof_from_op(ms_op_t op, const char *key, const char *val)
{
    if (!key) return;

    const char *cmd = NULL;

    switch (op) {
        case MS_OP_ARRAY_SET:     cmd = "SET";  break;
        case MS_OP_RBTREE_RSET:   cmd = "RSET"; break;
        case MS_OP_HASH_HSET:     cmd = "HSET"; break;
        case MS_OP_SKIPTABLE_SSET:cmd = "SSET"; break;

        case MS_OP_ARRAY_DEL:     cmd = "DEL";  break;
        case MS_OP_RBTREE_RDEL:   cmd = "RDEL"; break;
        case MS_OP_HASH_HDEL:     cmd = "HDEL"; break;
        case MS_OP_SKIPTABLE_SDEL:cmd = "SDEL"; break;

        case MS_OP_ARRAY_MOD:     cmd = "MOD";  break;
        case MS_OP_RBTREE_RMOD:   cmd = "RMOD"; break;
        case MS_OP_HASH_HMOD:     cmd = "HMOD"; break;
        case MS_OP_SKIPTABLE_SMOD:cmd = "SMOD"; break;
        case MS_OP_PAPERSET:      cmd = "PAPERSET"; break;
        case MS_OP_PAPERDEL:      cmd = "PAPERDEL"; break;

        default:
            // 非写类 / 未知操作，不写入 AOF
            return;
    }

    // DEL/RDEL/HDEL/SDEL 只有两个参数，其余都是三个
    char *argv[3];
    int   argc = 0;

    argv[0] = (char *)cmd;
    argv[1] = (char *)key;

    if (op == MS_OP_ARRAY_DEL ||
        op == MS_OP_RBTREE_RDEL ||
        op == MS_OP_HASH_HDEL  ||
        op == MS_OP_SKIPTABLE_SDEL) {
        argc = 2;
    } else {
        argv[2] = (char *)(val ? val : "");
        argc = 3;
    }

    extern void aof_append(int argc, char **argv);
    aof_append(argc, argv);
}
#endif // ENABLE_AOF

//每个slave同步线程 先全量 再增量
void* slave_sync_thread(void *arg){

    slave_info_t *slave = (slave_info_t*)arg;
    int sockfd = slave->sockfd;

    printf("[DIST] Slave connected from %s:%d\n",
           inet_ntoa(slave->addr.sin_addr),
           ntohs(slave->addr.sin_port));

    // 全量同步：遍历本地所有数据结构，对每个 key 调用 master_sync_command()
    
#if ENABLE_ARRAY
    for (int i = 0; i < global_array.total; i++) {
        if (global_array.table[i].key && global_array.table[i].value) {
            master_sync_command("SET",
                                global_array.table[i].key,
                                global_array.table[i].value);
        }
    }
#endif

#if ENABLE_RBTREE
    /* 递归遍历 rbtree 执行全量同步 */
    void send_rbtree_node(rbtree_node *node) {
        if (node == global_rbtree.nil) return;
        send_rbtree_node(node->left);
        master_sync_command("RSET", node->key, (char*)node->value);
        send_rbtree_node(node->right);
    }
    send_rbtree_node(global_rbtree.root);
#endif

#if ENABLE_HASH
    for (int i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *slot = &global_hash.nodes[i];
        if (slot->used == 1) {
            master_sync_command("HSET", slot->key, slot->value);
        }
    }
#endif

#if ENABLE_SKIPTABLE
    Node *current = global_skiptable.header->forward[0];
    while (current) {
        master_sync_command("SSET", current->key, current->value);
        current = current->forward[0];
    }
#endif

    char tmp[16];
    while (1) {
        ssize_t n = recv(sockfd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
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
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(global_dist_config.sync_port);

    if(bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0){
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if(listen(server_fd, 3) < 0){
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Master sync server listening on port %d\n", global_dist_config.sync_port);

    while(1){
        if((client_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0){
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&global_dist_config.slave_mutex);

        //增加从节点
        if(global_dist_config.slave_count < global_dist_config.max_slaves){
            slave_info_t *slave = kvs_malloc(sizeof(slave_info_t));
            if (!slave) {
                fprintf(stderr, "[DIST] malloc slave_info failed\n");
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
                fprintf(stderr, "[DIST] create slave_sync_thread failed\n");
                global_dist_config.slave_count--;
                close(client_fd);
                kvs_free(slave);
            } 
        }else{
            close(client_fd);
            printf("Max slave reached, connection refused\n");
        }

        pthread_mutex_unlock(&global_dist_config.slave_mutex);
    }

    return NULL;
}

void* slave_connect_thread(void *arg) {
    // 等待主节点初始化完成
    sleep(1);
    slave_connect_master();
    return NULL;
}

void distributed_shutdown(void) {
    if (global_dist_config.role == ROLE_MASTER) {
        // 1. 停止增量同步
        // g_sync_running = 0;
        // msg_queue_close(&g_sync_queue);

        // 2. 等待所有 slave 同步线程退出
        pthread_mutex_lock(&global_dist_config.slave_mutex);
        int count = global_dist_config.slave_count;
        // 先复制一份线程句柄，避免在 join 过程中修改数组
        pthread_t *threads = NULL;
        if (count > 0) {
            threads = kvs_malloc(sizeof(pthread_t) * count);
            for (int i = 0; i < count; ++i) {
                threads[i] = global_dist_config.slaves[i].thread;
            }
        }
        pthread_mutex_unlock(&global_dist_config.slave_mutex);

        for (int i = 0; i < count; ++i) {
            pthread_join(threads[i], NULL);
        }
        kvs_free(threads);

        // // 3. 销毁消息队列
        // msg_queue_destroy(&g_sync_queue);

        sync_backend_shutdown();

        // 4. 释放 slave 数组
        if (global_dist_config.slaves) {
            kvs_free(global_dist_config.slaves);
            global_dist_config.slaves = NULL;
        }
    } else if (global_dist_config.role == ROLE_SLAVE) {
        // 目前 slave 只创建了一个连接线程和一个同步 socket，
        // 连接线程里在 master 断开后会自己退出，这里暂时不需要额外处理。
        // 如果以后在 SLAVE 增加更多后台线程，可在这里 join / 清理。
    }

    pthread_mutex_destroy(&global_dist_config.slave_mutex);
}

//初始化分布式系统
int distributed_init(node_role_t role, const char *master_ip, int master_port, int sync_port){
    memset(&global_dist_config, 0, sizeof(distributed_config_t));

    global_dist_config.role = role;
    global_dist_config.sync_port = sync_port;
    pthread_mutex_init(&global_dist_config.slave_mutex, NULL);

    if(role == ROLE_MASTER){
        global_dist_config.max_slaves = 10;
        global_dist_config.slaves = kvs_malloc(global_dist_config.max_slaves * sizeof(slave_info_t));

        //无论哪种模式 都启动同步服务器线程
        pthread_t sync_thread;
        pthread_create(&sync_thread, NULL, master_sync_server, NULL);

        // 初始化 sync backend（当前实现为 TCP backend）
        if (sync_backend_init(sync_port) != 0) {
            fprintf(stderr, "[DIST] sync_backend_init failed\n");
            return -1;
        }

        printf("[DISTRIBUTED] Running as MASTER, sync port: %d\n", sync_port);

    }else{
        //从节点
        strncpy(global_dist_config.master_ip, master_ip, sizeof(global_dist_config.master_ip)-1);
        global_dist_config.master_port = master_port;

        printf("Running ad SLAVE, connecting to master %s:%d\n", master_ip, master_port);

        // 启动连接线程
        pthread_t connect_thread;
        pthread_create(&connect_thread, NULL, slave_connect_thread, NULL);
    }

    return 0;
}

//从节点连接主节点
void slave_connect_master(){

    int sock = 0;
    struct sockaddr_in serv_addr;

    if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        printf("\n Socket creation error\n");
        return;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(global_dist_config.master_port);

    if(inet_pton(AF_INET, global_dist_config.master_ip, &serv_addr.sin_addr) <= 0){
        printf("\nInvalid address/ Address not supported \n");
        return;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n");
        return;
    }

    printf("Connected to master, starting synchronization...\n");


    //接收同步数据
    // char buffer[KVS_MAX_RESPONSE_SIZE] = {0};
    size_t buf_cap = KVS_MAX_RESPONSE_SIZE;
    uint8_t *buffer = kvs_malloc(buf_cap);
    if(!buffer){
        fprintf(stderr, "[SLAVE] malloc buffer failed\n");
        close(sock);
        return;
    }
    size_t recv_len = 0;

    //解析命令
    while(1){
        if(recv_len >= buf_cap){
            size_t new_cap = buf_cap * 2;
            if(new_cap < buf_cap){
                fprintf(stderr, "[SLAVE] buffer size overflow\n");
                break;
            }
            uint8_t* tmp = (uint8_t*)kvs_realloc(buffer, new_cap);
            if(!tmp){
                fprintf(stderr, "[SLAVE] realloc buffer failed\n");
                break;
            }
            buffer = tmp;
            buf_cap = new_cap;
        }

        ssize_t n = recv(sock, buffer + recv_len, buf_cap - recv_len, 0);

        if(n == 0){
            printf("Master closed connection\n");
            break;
        }else if(n < 0){
            if (errno == EINTR) continue;
            fprintf(stderr, "[SLAVE] recv error: %s\n", strerror(errno));
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
                fprintf(stderr, "[SLAVE] ms_decode_message error, drop buffer\n");
                recv_len = 0;
                break;
            }

            
            // 解析出一条完整命令 (op, key, val)
            // 注意：k/v 不一定以 '\0' 结尾，这里临时拷贝+补 '\0'
            char *key  = (char *)kvs_malloc(klen + 1);
            char *val  = NULL;
            if (!key) {
                fprintf(stderr, "[SLAVE] malloc key failed\n");
                parsed_total += (size_t)consumed;
                continue;
            }
            memcpy(key, k, klen);
            key[klen] = '\0';

            if (vlen > 0) {
                val = (char *)kvs_malloc(vlen + 1);
                if (!val) {
                    fprintf(stderr, "[SLAVE] malloc val failed\n");
                    kvs_free(key);
                    parsed_total += (size_t)consumed;
                    continue;
                }
                memcpy(val, v, vlen);
                val[vlen] = '\0';
            }

            set_sync_context(1); // 标记这是主从同步写，避免回写 master

            // printf("[SLAVE] decoded msg: op=%d key_len=%zu val_len=%zu\n",
    //    op, klen, vlen);

            switch(op){
                case MS_OP_ARRAY_SET:
#if ENABLE_ARRAY
                    kvs_array_set(&global_array, key, val ? val : "");
#endif
                    break;
                case MS_OP_RBTREE_RSET:
#if ENABLE_RBTREE
                    kvs_rbtree_set(&global_rbtree, key, val ? val : "");
#endif
                    break;
                case MS_OP_HASH_HSET:
#if ENABLE_HASH
                    kvs_hash_set(&global_hash, key, val ? val : "");
#endif
                    break;
                case MS_OP_SKIPTABLE_SSET:
#if ENABLE_SKIPTABLE
                    kvs_skiptable_set(&global_skiptable, key, val ? val : "");
#endif
                    break; 
                case MS_OP_ARRAY_DEL:
#if ENABLE_ARRAY
                    kvs_array_del(&global_array, key);
#endif
                    break;
                case MS_OP_RBTREE_RDEL:
#if ENABLE_RBTREE
                    kvs_rbtree_del(&global_rbtree, key);
#endif
                    break;  
                case MS_OP_HASH_HDEL:
#if ENABLE_HASH
                    kvs_hash_del(&global_hash, key);
#endif
                    break;
                case MS_OP_SKIPTABLE_SDEL:
#if ENABLE_SKIPTABLE
                    kvs_skiptable_del(&global_skiptable, key);
#endif
                    break;
                case MS_OP_ARRAY_MOD:
#if ENABLE_ARRAY
                    kvs_array_mod(&global_array, key, val ? val : "");
#endif  
                    break;
                case MS_OP_RBTREE_RMOD:
#if ENABLE_RBTREE
                    kvs_rbtree_mod(&global_rbtree, key, val ? val : "");
#endif
                    break;
                case MS_OP_HASH_HMOD:
#if ENABLE_HASH
                    kvs_hash_mod(&global_hash, key, val ? val : "");
#endif
                    break;
                case MS_OP_SKIPTABLE_SMOD:
#if ENABLE_SKIPTABLE
                    kvs_skiptable_mod(&global_skiptable, key, val ? val : "");
#endif
                    break;
#if ENABLE_RBTREE
                case MS_OP_PAPERSET:
                    kvs_rbtree_set(&global_rbtree, key, val ? val : "");
                    break;
                case MS_OP_PAPERDEL:
                    kvs_rbtree_del(&global_rbtree, key);
                    break;
                case MS_OP_PAPERMOD:
                    kvs_rbtree_mod(&global_rbtree, key, val ? val : "");
                    break;
#endif                   
                default:
                    fprintf(stderr, "[SLAVE] Unknown operation %d\n", op);
                    break;
            }

#if ENABLE_AOF
            // 写入 AOF（RESP 格式）
            slave_append_aof_from_op(op, key, val);
#endif

            set_sync_context(0);

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
    close(sock);
    return;
}


//主节点同步写操作到所有从节点
void master_sync_command(const char *cmd, const char *key, const char *value){
    
    if(global_dist_config.role != ROLE_MASTER) return;
    if(!key || !cmd) return;

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
    }else{
        //未知命令
        return;
    }

    size_t key_len = strlen(key);
    size_t val_len = value ? strlen(value) : 0;

    size_t needed = (size_t)MS_HEADER_LEN + key_len + val_len;
    uint8_t *buffer = (uint8_t *)kvs_malloc(needed);
    if (!buffer) {
        fprintf(stderr, "[DIST] master_sync_command malloc failed, cmd=%s key=%s\n",
                cmd, key);
        return;
    }

    int len = ms_encode_message(
        buffer,
        needed,
        op,
        key, key_len,
        value, val_len
    );
    if (len <= 0) {
        fprintf(stderr, "[DIST] master_sync_command encode error, cmd=%s key=%s\n",
                cmd, key);
        kvs_free(buffer);
        return;
    }

    //  fprintf(stderr, "[DIST] master_sync_command: op=%d cmd=%s key=%s\n",
    //         op, cmd, key);

    if (sync_backend_broadcast(buffer, (size_t)len) != 0) {
        fprintf(stderr, "[DIST] sync_backend_broadcast failed, cmd=%s key=%s\n",
                cmd, key);
    }else {
        // fprintf(stderr, "[DIST] sync_backend_broadcast ok, len=%d\n", len);
    }

    kvs_free(buffer);

}
