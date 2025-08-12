#include "distributed.h"
#include "kvstore.h"
#include <arpa/inet.h>
#include <sys/socket.h>

distributed_config_t global_dist_config;
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

//发送命令到指定socket
void send_cmd(int sockfd, const char *cmd, const char *key, const char *value){
    char buffer[KVS_MAX_RESPONSE_SIZE];

    int len = snprintf(buffer, sizeof(buffer), "%s|%s|%s\n", cmd, key, value ? value : "");
    send(sockfd, buffer, len, 0);
}

//处理从节点连接线程 全量同步
void *slave_connection_thread(void *arg){
    slave_info_t *slave = (slave_info_t*)arg;
    printf("Slave connected from %s:%d\n", inet_ntoa(slave->addr.sin_addr), ntohs(slave->addr.sin_port));

    //全量同步
    #if ENABLE_ARRAY
    int i;
    for(i = 0;i < global_array.total; i++){
        if(global_array.table[i].key && global_array.table[i].value){
            send_cmd(slave->sockfd, "SET", global_array.table[i].key, global_array.table[i].value);
        }
    }
    #endif

    #if ENABLE_RBTREE
    //中序遍历红黑树发送数据
    void send_rbtree(rbtree_node *node){
        if(node == global_rbtree.nil) return;
        send_rbtree(node->left);
        send_cmd(slave->sockfd, "RSET", node->key, (char*)node->value);
        send_rbtree(node->right);
    }
    send_rbtree(global_rbtree.root);
    #endif

    #if ENABLE_HASH
    for(i = 0; i < global_hash.max_slots; i++){
        hashnode_t *node = global_hash.nodes[i];
        while(node){
            send_cmd(slave->sockfd, "HSET", node->key, node->value);
            node = node->next;
        }
    }
    #endif

    #if ENABLE_SKIPTABLE
    Node *current = global_skiptable.header->forward[0];
    while(current){
        send_cmd(slave->sockfd, "SSET", current->key, current->value);
        current = current->forward[0];
    }
    #endif

    printf("Full sync completed for slave %s:%d\n", inet_ntoa(slave->addr.sin_addr), ntohs(slave->addr.sin_port));


    while(1){
        sleep(1);
    }

    close(slave->sockfd);
    free(slave);
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
            slave_info_t *slave = malloc(sizeof(slave_info_t));
            slave->sockfd = client_fd;
            memcpy(&slave->addr, &address, sizeof(address));
            
            //开辟线程处理从节点
            pthread_create(&slave->thread, NULL, slave_connection_thread, slave); 

            global_dist_config.slaves[global_dist_config.slave_count++] = *slave;
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

//初始化分布式系统
int distributed_init(node_role_t role, const char *master_ip, int master_port, int sync_port){
    memset(&global_dist_config, 0, sizeof(distributed_config_t));

    global_dist_config.role = role;
    global_dist_config.sync_port = sync_port;
    pthread_mutex_init(&global_dist_config.slave_mutex, NULL);

    if(role == ROLE_MASTER){
        global_dist_config.max_slaves = 10;
        global_dist_config.slaves = malloc(global_dist_config.max_slaves * sizeof(slave_info_t));

        //启动同步服务器
        pthread_t sync_thread;
        pthread_create(&sync_thread, NULL, master_sync_server, NULL);

        printf("Running ad MASTER, sync port :%d\n", sync_port);
    }else{
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
    char buffer[KVS_MAX_RESPONSE_SIZE] = {0};
    char *response = malloc(KVS_MAX_RESPONSE_SIZE);


    while(1){
        //处理接收到的数据
        size_t bytes_read;
        bytes_read = recv(sock, buffer, sizeof(buffer)-1, 0);
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                printf("Master disconnected\n");
            } else {
                perror("recv error");
            }
            break;
        }
        

        buffer[bytes_read] = '\0';
        printf("recv buffer: %s\n", buffer);
        printf("同步主节点的写信息\n");
        //处理同步命令
        //分割命令
        char *line = strtok(buffer, "\n");
        while(line){
            char *cmd = strtok(line, "|");
            char *key = strtok(NULL, "|");
            char *value = strtok(NULL, "|");
            printf("cmd: %s, key: %s,value: %s\n", cmd, key, value);
            //检查是否为有效写命令
            int is_valid_write_cmd = 0;
            if(cmd){
                //只处理写命令 SET DEL MOD
                if (strcmp(cmd, "SET") == 0 || strcmp(cmd, "RSET") == 0 ||
                    strcmp(cmd, "HSET") == 0 || strcmp(cmd, "SSET") == 0 ||
                    strcmp(cmd, "DEL") == 0 || strcmp(cmd, "RDEL") == 0 ||
                    strcmp(cmd, "HDEL") == 0 || strcmp(cmd, "SDEL") == 0 ||
                    strcmp(cmd, "MOD") == 0 || strcmp(cmd, "RMOD") == 0 ||
                    strcmp(cmd, "HMOD") == 0 || strcmp(cmd, "SMOD") == 0) {
                    is_valid_write_cmd = 1;
                }
            }

            printf("本地执行写命令\n");
            if(cmd && key && is_valid_write_cmd){
                set_sync_context(1);
                //在本地执行命令
                char *tokens[3] = {cmd, key, value};
                kvs_filter_protocol(tokens, value ? 3 : 2, response);
                set_sync_context(0);
            }
            printf("本地执行写命令end\n");

            line = strtok(NULL, "\n");
        }
    }

    free(response);
    close(sock);
    printf("Disconnected from master\n");
}

//主节点同步写操作到所有从节点
void master_sync_command(const char *cmd, const char *key, const char *value){
    if(global_dist_config.role != ROLE_MASTER) return;

    //检查是否为有效写命令
    int is_valid_write_cmd = (
        strcmp(cmd, "SET") == 0 || strcmp(cmd, "RSET") == 0 ||
        strcmp(cmd, "HSET") == 0 || strcmp(cmd, "SSET") == 0 ||
        strcmp(cmd, "DEL") == 0 || strcmp(cmd, "RDEL") == 0 ||
        strcmp(cmd, "HDEL") == 0 || strcmp(cmd, "SDEL") == 0 ||
        strcmp(cmd, "MOD") == 0 || strcmp(cmd, "RMOD") == 0 ||
        strcmp(cmd, "HMOD") == 0 || strcmp(cmd, "SMOD") == 0
    );
    printf("主节点向从节点同步信息\n");

    if (!is_valid_write_cmd) return;

    // 检查是否为DEL命令（包括所有数据结构的DEL命令）
    // int is_del_command = (strcmp(cmd, "DEL") == 0) || 
    //                      (strcmp(cmd, "RDEL") == 0) || 
    //                      (strcmp(cmd, "HDEL") == 0) || 
    //                      (strcmp(cmd, "SDEL") == 0);

    pthread_mutex_lock(&global_dist_config.slave_mutex);
    
    int i;
    for (i = 0; i < global_dist_config.slave_count; i++) {
        slave_info_t *slave = &global_dist_config.slaves[i];

        //同步给已连接的从节点
        //if(slave->connected){
            // // 对于DEL命令，value参数为NULL，需要特殊处理
            // if (is_del_command) {
            //     // 发送格式：COMMAND|KEY| (value部分为空)
            //     send_cmd(global_dist_config.slaves[i].sockfd, cmd, key, "");
            //     // 更新复制偏移量
            //     slave->replication_offset++;
            // } else {
            //     // 其他命令正常发送
            //     send_cmd(global_dist_config.slaves[i].sockfd, cmd, key, value);
            //     // 更新复制偏移量
            //     slave->replication_offset++;
            // }

            // 其他命令正常发送
            send_cmd(global_dist_config.slaves[i].sockfd, cmd, key, value);

        //}
        
    }

    pthread_mutex_unlock(&global_dist_config.slave_mutex);
}