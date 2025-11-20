#ifndef KVS_CONFIG_H
#define KVS_CONFIG_H

//集中管理基础配置

//基础常量
#define KVS_MAX_TOKENS		128
#define KVS_MAX_RESPONSE_SIZE  2048
#define KVS_MAX_LINE        1000

//网络模式
#define NETWORK_REACTOR 	0
#define NETWORK_PROACTOR	1
#define NETWORK_NTYCO		2

#define NETWORK_SELECT		NETWORK_NTYCO

//组件启用选项
#define ENABLE_ARRAY		1
#define ENABLE_RBTREE		1
#define ENABLE_HASH			1
#define ENABLE_SKIPTABLE    1
#define ENABLE_RDB          1
#define ENABLE_AOF          1

#endif // KVS_CONFIG_H