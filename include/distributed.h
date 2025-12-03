#ifndef __DISTRIBUTED_H__
#define __DISTRIBUTED_H__

#include <pthread.h>
#include <netinet/in.h>

#define MAX_SLAVES 20

//节点角色
typedef enum {
    ROLE_MASTER, //可读可写
    ROLE_SLAVE //只读
} node_role_t;

//从节点信息
typedef struct {
    int sockfd;
    struct sockaddr_in addr;
    pthread_t thread;
} slave_info_t;

//分布式配置
typedef struct {
    node_role_t role;
    char master_ip[16];
    int master_port;
    int sync_port;

    //主节点专用
    slave_info_t *slaves;
    int slave_count;
    int max_slaves;
    pthread_mutex_t slave_mutex;

} distributed_config_t;

extern distributed_config_t global_dist_config;

//初始化分布式系统
int distributed_init(node_role_t role, const char *master_ip, int master_port, int sync_port);

//主节点同步数据到从节点
//目前只支持一对 key value 
void master_sync_command(const char *cmd, const char *key, const char *value);

//从节点连接到主节点
void slave_connect_master();
void* slave_connect_thread(void *arg);

//关闭分布式系统
void distributed_shutdown(void);


#endif