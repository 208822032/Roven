#ifndef KVS_CONFIG_H
#define KVS_CONFIG_H

#include <stddef.h>

// 集中管理基础配置 + 运行时配置结构

// 基础常量
#define KVS_MAX_TOKENS              128
#define KVS_MAX_RESPONSE_SIZE       (1024 * 1024)
#define KVS_MAX_LINE                1000
// 统一的最大 RESP 响应长度上限（用于防御性检查）/
#define KVS_MAX_RESPONSE_LENGTH     (2 * 1024 * 1024)

// 网络模式
#define NETWORK_REACTOR         0
#define NETWORK_PROACTOR        1
#define NETWORK_NTYCO           2

#define NETWORK_SELECT          NETWORK_REACTOR

// 组件启用选项
#define ENABLE_ARRAY            1
#define ENABLE_RBTREE           1
#define ENABLE_HASH             1
#define ENABLE_SKIPTABLE        1
#define ENABLE_RDB              1
#define ENABLE_AOF              1

// 调试开关：统计并打印 RANGE 遍历访问节点数
// 0: 关闭（默认，不影响兼容）
// 1: 开启（用于测试/bench 观测）
#ifndef KVS_RANGE_VISITED_DEBUG
#define KVS_RANGE_VISITED_DEBUG 0
#endif

// 运行模式
typedef enum {
	KVS_ROLE_MASTER = 0,
	KVS_ROLE_SLAVE  = 1
} kvs_role_t;

// AOF fsync 模式
typedef enum {
	KVS_AOF_FSYNC_NO = 0,
	KVS_AOF_FSYNC_EVERYSEC = 1,
	KVS_AOF_FSYNC_ALWAYS = 2
} kvs_aof_fsync_mode_t;

// 内存分配策略
typedef enum {
	KVS_MEM_BACKEND_POOL = 0,   // 自定义内存池
	KVS_MEM_BACKEND_SYSTEM = 1, // 系统 malloc/free
	KVS_MEM_BACKEND_JEMALLOC = 2 // jemalloc 内存池
} kvs_mem_backend_t;

// 主从同步模式
typedef enum{
	KVS_SYNC_TCP = 0,    // 通过 TCP 同步
	KVS_SYNC_EBPF = 1    // 通过 eBPF 同步
} kvs_sync_mode_t;

// 全局配置结构
typedef struct kvs_config {
	int   port;                     // 服务端口
	kvs_role_t role;                // 角色：主/从

	// 从节点相关
	char  master_ip[64];
	int   master_port;             // 主节点服务端口

	// 同步端口（主：本地监听端口；从：主节点同步端口）
	int   sync_port;

	// AOF 配置
	int   enable_aof;              // 是否启用 AOF（与编译期 ENABLE_AOF 配合）
	kvs_aof_fsync_mode_t aof_fsync_mode;
	char  aof_filename[256];

	// RDB/旧格式文件名
	char  rdb_filename[256];
	char  dump_filename[256];

	// 预留扩展字段（网络模型等）
	int   network_mode;            // 目前仍由 NETWORK_SELECT 控制，仅占位

	// 内存管理后端选择
	kvs_mem_backend_t mem_backend;

	// 逻辑上 value 最大长度（0 表示不限制，由编译期常量决定）
	size_t max_value_size;

	//同步模式选择
	kvs_sync_mode_t sync_mode;
	char ebpf_conf[256];          // eBPF 配置文件路径
} kvs_config_t;

// 默认配置初始化
void kvs_config_init_default(kvs_config_t *cfg);

// 从配置文件加载，简单 key=value 文本格式
// 返回 0 表示成功，<0 表示失败或文件不存在（此时保留默认值）。
int kvs_config_load_file(kvs_config_t *cfg, const char *path);

// 从命令行覆盖关键配置项（保持兼容）
// 支持：
//  --config <path>
//  --slave
//  --master <ip> <port> <listen_port>
//  <port>                // 仅 master：第一个非选项参数
//  --aof_fsync [no|everysec|always]
//  --aof_filename <name>
void kvs_config_apply_cmdline(kvs_config_t *cfg, int argc, char *argv[], const char **config_path_out);

#endif // KVS_CONFIG_H