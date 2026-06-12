#ifndef KVS_CONFIG_H
#define KVS_CONFIG_H

#include <stddef.h>

#include "kvs_log.h"

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

	// 日志配置
	char log_level[16];
	int  log_async;
	char log_file[256];
	char ebpf_conf[256];          // eBPF 配置文件路径

	// embedding_provider:
	//   hash    : built-in deterministic test provider
	//   local_c : reserved for a future in-process C ABI model provider
	char embedding_provider[16];
	char embedding_model_path[256];
	int embedding_dim;
	int embedding_num_threads;
	int embedding_max_text_len;

	// vector index 配置。正式语义缓存主路径固定为 HNSW；
	// flat 仅作为 debug / 单元测试 baseline，sq8_flat 为实验性压缩索引。
	// vector_index_type:
	//   hnsw: semantic-cache main path
	//   flat: debug / unit-test baseline
	//   sq8_flat: experimental compressed flat scan
	char vector_index_type[16];

	/*
	 * vector_metric:
	 *   Official semantic-cache path uses L2-normalized embeddings + ip.
	 *   The score is interpreted as cosine/IP similarity, larger is better.
	 *
	 *   l2 / cosine remain accepted for legacy/debug experiments, but are
	 *   not the main project capability.
	 *
	 * Default: ip
	 */
	char vector_metric[16];

	// HNSW 参数
	int vector_hnsw_m;
	int vector_hnsw_ef_construction;
	int vector_hnsw_ef_search;

	/*
	 * Default semantic-cache hit threshold.
	 *
	 * Embeddings are L2-normalized and the main metric is inner product,
	 * so this value is interpreted as cosine/IP similarity.
	 */
	float vector_score_threshold;

	/*
	 * Scope overfetch factor for HNSW scoped search.
	 *
	 * When scope is non-empty, HNSW expands candidate collection:
	 *
	 *   effective_ef = min(ef_search * vector_scope_overfetch_factor, 2048)
	 *
	 * The final returned result count is still limited by user topK.
	 *
	 * Range: 1 ~ 32
	 * Default: 4
	 * 1 means disabled.
	 */
	int vector_scope_overfetch_factor;

	/*
	 * vector_distance_backend:
	 *   auto     : use AVX2 when supported, otherwise fallback
	 *   fallback : force scalar C implementation
	 *   avx2     : prefer AVX2, fallback when CPU does not support it
	 *
	 * Default: auto
	 */
	char vector_distance_backend[16];

	/*
	 * IVF Flat parameters.
	 *
	 *   number of centroid buckets.
	 *
	 *   number of buckets to scan during search.
	 *
	 *   max k-means iterations during VINDEX TRAIN.
	 *
	 * Defaults:
	 *   nlist = 64
	 *   nprobe = 4
	 *   max_iterations = 20
	 */
} kvs_config_t;

// 默认配置初始化
void kvs_config_init_default(kvs_config_t *cfg);

// 从配置文件加载，简单 key=value 文本格式
// 返回 0 表示成功，<0 表示失败或文件不存在（此时保留默认值）。
int kvs_config_load_file(kvs_config_t *cfg, const char *path);

// 从命令行覆盖关键配置项（保持兼容）
// 支持：
//  --config <path>
//  -c <path>
//  --slave
//  --master <ip> <port> <listen_port>
//  <port>                // 仅 master：第一个非选项参数
//  --aof_fsync [no|everysec|always]
//  --aof_filename <name>
//  --log_level <error|warn|info|debug>
//  --log_async <yes|no>
//  --log_file <path>
//  --embedding_provider <hash>
//  --vector_metric <ip|l2|cosine>
	//  --vector_distance_backend <auto|fallback|avx2>
//  --vector_scope_overfetch_factor <1-32>
void kvs_config_apply_cmdline(kvs_config_t *cfg, int argc, char *argv[], const char **config_path_out);

#endif // KVS_CONFIG_H
