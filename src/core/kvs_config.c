#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "kvs_config.h"

#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef KVS_ENABLE_EBPF
#define KVS_ENABLE_EBPF 0
#endif

#define KVS_CONFIG_NO_FIELD ((kvs_config_field_id_t)-1)
#define KVS_CONFIG_NO_OFFSET ((size_t)-1)

#define CFG_FLAG_LEGACY_HNSW_M      (1u << 0)
#define CFG_FLAG_LEGACY_HNSW_EFC    (1u << 1)
#define CFG_FLAG_LEGACY_HNSW_EFS    (1u << 2)
#define CFG_FLAG_LEGACY_INDEX       (1u << 3)
#define CFG_FLAG_LEGACY_PROVIDER    (1u << 4)
#define CFG_FLAG_LEGACY_SYNC_MODE   (1u << 5)
#define CFG_FLAG_LEGACY_DATA_PATH   (1u << 6)
#define CFG_FLAG_LEGACY_INITIAL_CAP (1u << 7)

typedef struct kvs_config_descriptor_internal {
	kvs_config_field_descriptor_t public_desc;
	size_t offset;
	size_t storage_size;
	long double min_value;
	long double max_value;
	int has_range;
	const int *enum_codes;
	size_t enum_count;
	unsigned int flags;
} kvs_config_descriptor_internal_t;

static const int role_codes[] = {KVS_ROLE_MASTER, KVS_ROLE_SLAVE};
static const int network_codes[] = {NETWORK_REACTOR, NETWORK_PROACTOR, NETWORK_NTYCO};
static const int mem_codes[] = {
	KVS_MEM_BACKEND_POOL,
	KVS_MEM_BACKEND_SYSTEM,
	KVS_MEM_BACKEND_JEMALLOC
};
static const int aof_codes[] = {
	KVS_AOF_FSYNC_NO,
	KVS_AOF_FSYNC_EVERYSEC,
	KVS_AOF_FSYNC_ALWAYS
};
static const int replication_transport_codes[] = {
	KVS_REPLICATION_DISABLED,
	KVS_REPLICATION_TCP
};
static const int ebpf_mode_codes[] = {
	KVS_EBPF_DISABLED,
	KVS_EBPF_OBSERVE,
	KVS_EBPF_EXPERIMENTAL_MIRROR
};

#define DESC_FIELD(fid, sec, key, legacy, vtype, def, limits, enums, cli, state, field, minv, maxv, ranged, codes, code_count, desc_flags) \
	{ \
		{fid, sec, key, sec "." key, legacy, vtype, def, limits, enums, cli, state}, \
		offsetof(kvs_config_t, field), sizeof(((kvs_config_t *)0)->field), \
		minv, maxv, ranged, codes, code_count, desc_flags \
	}

#define DESC_NONE(sec, key, vtype, def, limits, enums, cli, state) \
	{ \
		{KVS_CONFIG_NO_FIELD, sec, key, sec "." key, "", vtype, def, limits, enums, cli, state}, \
		KVS_CONFIG_NO_OFFSET, 0, 0, 0, 0, NULL, 0, 0 \
	}

static const kvs_config_descriptor_internal_t config_descriptors[] = {
	DESC_FIELD(KVS_CFG_SERVER_ROLE, "server", "role", "role",
		KVS_CONFIG_TYPE_ENUM_INT, "master", "master|slave", "master|slave", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, role, 0, 0, 0,
		role_codes, sizeof(role_codes) / sizeof(role_codes[0]), 0),
	DESC_FIELD(KVS_CFG_SERVER_CONFIG_ROOT, "server", "config_root", "config_root",
		KVS_CONFIG_TYPE_STRING, ".", "path", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, config_root, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_SERVER_DATA_DIR, "server", "data_dir", "data_dir",
		KVS_CONFIG_TYPE_STRING, "data", "path", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, data_dir, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_SERVER_TEST_MODE, "server", "test_mode", "test_mode",
		KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, test_mode, 0, 1, 1, NULL, 0, 0),

	DESC_FIELD(KVS_CFG_NETWORK_PORT, "network", "port", "port",
		KVS_CONFIG_TYPE_INT, "8080", "1..65535", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, port, 1, 65535, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_NETWORK_MODE, "network", "mode", "network_mode",
		KVS_CONFIG_TYPE_ENUM_INT, "reactor", "reactor|proactor|ntyco",
		"reactor|proactor|ntyco", 1, KVS_CONFIG_STATUS_IMPLEMENTED,
		network_mode, 0, 0, 0, network_codes,
		sizeof(network_codes) / sizeof(network_codes[0]), 0),
	DESC_FIELD(KVS_CFG_NETWORK_LISTEN_ADDRESS, "network", "listen_address", "listen_address",
		KVS_CONFIG_TYPE_STRING, "0.0.0.0", "IPv4 address", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, listen_address, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_NETWORK_WORKER_THREADS, "network", "worker_threads", "network_worker_threads",
		KVS_CONFIG_TYPE_INT, "1", "1 (current single-executor model)", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, network_worker_threads, 1, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_NETWORK_MAX_CONNECTIONS, "network", "max_connections", "network_max_connections",
		KVS_CONFIG_TYPE_INT, "1024", "1..1024", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, network_max_connections, 1, 1024, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_NETWORK_LISTEN_BACKLOG, "network", "listen_backlog", "network_listen_backlog",
		KVS_CONFIG_TYPE_INT, "1024", "1..65535", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, network_listen_backlog, 1, 65535, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_NETWORK_MAX_REQUEST_BYTES, "network", "max_request_bytes", "network_max_request_bytes",
		KVS_CONFIG_TYPE_SIZE, "10485760", "1024..67108864", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, network_max_request_bytes, 1024, 67108864, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_NETWORK_MAX_RESPONSE_BYTES, "network", "max_response_bytes", "network_max_response_bytes",
		KVS_CONFIG_TYPE_SIZE, "2097152", "1024..2097152", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, network_max_response_bytes, 1024, KVS_MAX_RESPONSE_LENGTH, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_NETWORK_MAX_COMMANDS_PER_READ, "network", "max_commands_per_read", "network_max_commands_per_read",
		KVS_CONFIG_TYPE_INT, "1024", "1..1000000", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, network_max_commands_per_read, 1, 1000000, 1, NULL, 0, 0),

	DESC_FIELD(KVS_CFG_STORAGE_MEMORY_BACKEND, "storage", "memory_backend", "mem_backend",
		KVS_CONFIG_TYPE_ENUM_INT, "pool", "pool|system|jemalloc", "pool|system|jemalloc", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, mem_backend, 0, 0, 0, mem_codes,
		sizeof(mem_codes) / sizeof(mem_codes[0]), 0),
	DESC_FIELD(KVS_CFG_STORAGE_MAX_VALUE_BYTES, "storage", "max_value_bytes", "max_value_size",
		KVS_CONFIG_TYPE_SIZE, "10485760", "0..2147483647", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, max_value_size, 0, INT_MAX, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_STORAGE_DEFAULT_ENGINE, "storage", "default_engine", "default_engine",
		KVS_CONFIG_TYPE_ENUM_STRING, "array",
		"array|rbtree|hash|skiptable", "array|rbtree|hash|skiptable", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, storage_default_engine, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_STORAGE_ENABLED_ENGINES, "storage", "enabled_engines", "enabled_engines",
		KVS_CONFIG_TYPE_STRING, "array,rbtree,hash,skiptable", "comma-separated unique engines", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, storage_enabled_engines, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_STORAGE_ARRAY_INITIAL_CAPACITY, "storage", "array_initial_capacity", "array_initial_capacity",
		KVS_CONFIG_TYPE_INT, "2048", "1..10000000", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, storage_array_initial_capacity, 1, 10000000, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_STORAGE_HASH_INITIAL_CAPACITY, "storage", "hash_initial_capacity", "hash_initial_capacity",
		KVS_CONFIG_TYPE_INT, "1048576", "1..10000000", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, storage_hash_initial_capacity, 1, 10000000, 1, NULL, 0, 0),

	DESC_FIELD(KVS_CFG_TTL_ENABLED, "ttl", "enabled", "ttl_enabled",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ttl_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_TTL_SCAN_ENABLED, "ttl", "scan_enabled", "ttl_scan_enabled",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ttl_scan_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_TTL_SCAN_INTERVAL_MS, "ttl", "scan_interval_ms", "ttl_scan_interval_ms",
		KVS_CONFIG_TYPE_INT, "1000", "10..86400000", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ttl_scan_interval_ms, 10, 86400000, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_TTL_SCAN_BUDGET, "ttl", "scan_budget", "ttl_scan_budget",
		KVS_CONFIG_TYPE_INT, "32", "1..1000000", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ttl_scan_budget, 1, 1000000, 1, NULL, 0, 0),

	DESC_FIELD(KVS_CFG_PERSISTENCE_AOF_ENABLED, "persistence", "aof_enabled", "enable_aof",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, enable_aof, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_PERSISTENCE_AOF_FSYNC, "persistence", "aof_fsync",
		"aof_fsync_mode|aof_fsync", KVS_CONFIG_TYPE_ENUM_INT, "everysec",
		"no|everysec|always", "no|everysec|always", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, aof_fsync_mode, 0, 0, 0, aof_codes,
		sizeof(aof_codes) / sizeof(aof_codes[0]), 0),
	DESC_FIELD(KVS_CFG_PERSISTENCE_AOF_PATH, "persistence", "aof_path", "aof_filename",
		KVS_CONFIG_TYPE_STRING, "kvstore.aof", "path", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, aof_filename, 0, 0, 0, NULL, 0, CFG_FLAG_LEGACY_DATA_PATH),
	DESC_FIELD(KVS_CFG_PERSISTENCE_RDB_PATH, "persistence", "rdb_path", "rdb_filename",
		KVS_CONFIG_TYPE_STRING, "kvstore.rdb", "path", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, rdb_filename, 0, 0, 0, NULL, 0, CFG_FLAG_LEGACY_DATA_PATH),
	DESC_FIELD(KVS_CFG_PERSISTENCE_LEGACY_DUMP_PATH, "persistence", "legacy_dump_path", "dump_filename",
		KVS_CONFIG_TYPE_STRING, "kvstore.dat", "path", "", 1,
		KVS_CONFIG_STATUS_INACTIVE, dump_filename, 0, 0, 0, NULL, 0, CFG_FLAG_LEGACY_DATA_PATH),
	DESC_FIELD(KVS_CFG_PERSISTENCE_RDB_ENABLED, "persistence", "rdb_enabled", "enable_rdb",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, enable_rdb, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_PERSISTENCE_AOF_REWRITE_ENABLED, "persistence", "aof_rewrite_enabled", "aof_rewrite_enabled",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, aof_rewrite_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_PERSISTENCE_AOF_REWRITE_MIN_BYTES, "persistence", "aof_rewrite_min_bytes", "aof_rewrite_min_bytes",
		KVS_CONFIG_TYPE_SIZE, "29360128", "1..INT64_MAX", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, aof_rewrite_min_bytes, 1, INT64_MAX, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_PERSISTENCE_AOF_REWRITE_MIN_GROWTH_BYTES, "persistence", "aof_rewrite_min_growth_bytes", "aof_rewrite_min_growth_bytes",
		KVS_CONFIG_TYPE_SIZE, "67108864", "1..INT64_MAX", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, aof_rewrite_min_growth_bytes, 1, INT64_MAX, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_PERSISTENCE_AOF_REWRITE_GROWTH_PERCENT, "persistence", "aof_rewrite_growth_percent", "aof_rewrite_growth_percent",
		KVS_CONFIG_TYPE_INT, "50", "1..1000", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, aof_rewrite_growth_percent, 1, 1000, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_PERSISTENCE_STARTUP_RECOVERY_ORDER, "persistence", "startup_recovery_order", "startup_recovery_order",
		KVS_CONFIG_TYPE_ENUM_STRING, "rdb_then_aof_then_index", "fixed", "rdb_then_aof_then_index", 0,
		KVS_CONFIG_STATUS_IMPLEMENTED, startup_recovery_order, 0, 0, 0, NULL, 0, 0),

	DESC_FIELD(KVS_CFG_VECTOR_ENABLED, "vector", "enabled", "vector_enabled",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_INDEX_TYPE, "vector", "index_type", "vector_index_type",
		KVS_CONFIG_TYPE_ENUM_STRING, "core_hnsw_v2", "core_flat|core_hnsw_v2",
		"core_flat|core_hnsw_v2", 1, KVS_CONFIG_STATUS_IMPLEMENTED,
		vector_index_type, 0, 0, 0, NULL, 0, CFG_FLAG_LEGACY_INDEX),
	DESC_FIELD(KVS_CFG_VECTOR_LEGACY_V2_EXPERIMENTAL, "vector", "legacy_v2_experimental_enabled",
		"vector_hnsw_v2_experimental_enabled", KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_INACTIVE, vector_hnsw_v2_experimental_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_DIMENSION, "vector", "dimension", "vector_dimension",
		KVS_CONFIG_TYPE_INT, "64", "1..1048576", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_dimension, 1, 1048576, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_METRIC, "vector", "metric", "vector_metric",
		KVS_CONFIG_TYPE_ENUM_STRING, "ip", "ip|l2|cosine", "ip|l2|cosine", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_metric, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_NORMALIZE, "vector", "normalize", "vector_normalize",
		KVS_CONFIG_TYPE_BOOL, "true", "true (current semantic contract)", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_normalize, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_HNSW_M, "vector", "hnsw_m", "vector_hnsw_m",
		KVS_CONFIG_TYPE_INT, "48", "48..65535", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_hnsw_m, 48, 65535, 1, NULL, 0,
		CFG_FLAG_LEGACY_HNSW_M),
	DESC_FIELD(KVS_CFG_VECTOR_HNSW_M0, "vector", "hnsw_m0", "vector_hnsw_m0",
		KVS_CONFIG_TYPE_INT, "96", "M..65535", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_hnsw_m0, 1, 65535, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_HNSW_EF_CONSTRUCTION, "vector", "hnsw_ef_construction",
		"vector_hnsw_ef_construction", KVS_CONFIG_TYPE_INT, "512", "1..2147483647", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_hnsw_ef_construction, 1, INT_MAX, 1, NULL, 0,
		CFG_FLAG_LEGACY_HNSW_EFC),
	DESC_FIELD(KVS_CFG_VECTOR_HNSW_EF_SEARCH, "vector", "hnsw_ef_search",
		"vector_hnsw_ef_search", KVS_CONFIG_TYPE_INT, "1024", "1..2147483647", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_hnsw_ef_search, 1, INT_MAX, 1, NULL, 0,
		CFG_FLAG_LEGACY_HNSW_EFS),
	DESC_FIELD(KVS_CFG_VECTOR_INITIAL_CAPACITY, "vector", "initial_capacity",
		"vector_hnsw_initial_capacity", KVS_CONFIG_TYPE_INT, "16", "1..2147483647", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_hnsw_initial_capacity, 1, INT_MAX, 1, NULL, 0,
		CFG_FLAG_LEGACY_INITIAL_CAP),
	DESC_FIELD(KVS_CFG_VECTOR_DEFAULT_TOPK, "vector", "default_topk", "vector_default_topk",
		KVS_CONFIG_TYPE_INT, "1", "1..1024", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_default_topk, 1, 1024, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_DEFAULT_THRESHOLD, "vector", "default_threshold",
		"vector_score_threshold", KVS_CONFIG_TYPE_FLOAT, "0.85", "-1.0..1.0", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_score_threshold, -1.0, 1.0, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_SCOPE_OVERFETCH_FACTOR, "vector", "scope_overfetch_factor",
		"vector_scope_overfetch_factor", KVS_CONFIG_TYPE_INT, "4", "1..32", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_scope_overfetch_factor, 1, 32, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_DISTANCE_BACKEND, "vector", "distance_backend",
		"vector_distance_backend", KVS_CONFIG_TYPE_ENUM_STRING, "auto",
		"auto|fallback|avx2", "auto|fallback|avx2", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_distance_backend, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_PROVIDER, "embedding", "provider", "embedding_provider",
		KVS_CONFIG_TYPE_ENUM_STRING, "hash", "hash|local_c", "hash|local_c", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_provider, 0, 0, 0, NULL, 0,
		CFG_FLAG_LEGACY_PROVIDER),
	DESC_FIELD(KVS_CFG_EMBEDDING_MODEL_PATH, "embedding", "model_path", "embedding_model_path",
		KVS_CONFIG_TYPE_STRING, "", "path", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_model_path, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_VOCAB_PATH, "embedding", "vocab_path", "embedding_vocab_path",
		KVS_CONFIG_TYPE_STRING, "", "path", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_vocab_path, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_OUTPUT_DIMENSION, "embedding", "output_dimension", "embedding_dim",
		KVS_CONFIG_TYPE_INT, "64", "1..1048576", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_dim, 1, 1048576, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_MODEL_VERSION, "embedding", "model_version", "embedding_model_version",
		KVS_CONFIG_TYPE_STRING, "auto", "non-empty label", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_model_version, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_INTRA_THREADS, "embedding", "intra_op_threads",
		"embedding_num_threads", KVS_CONFIG_TYPE_INT, "1", "1..256", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_num_threads, 1, 256, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_MAX_TEXT_LEN, "embedding", "max_text_bytes",
		"embedding_max_text_len", KVS_CONFIG_TYPE_INT, "4096", "1..2147483647", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_max_text_len, 1, INT_MAX, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_MAX_TOKENS, "embedding", "max_tokens", "embedding_max_tokens",
		KVS_CONFIG_TYPE_INT, "512", "2..512", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_max_tokens, 2, 512, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_ORT_GRAPH_OPT, "embedding", "ort_graph_optimization",
		"embedding_ort_graph_opt_level", KVS_CONFIG_TYPE_ENUM_STRING, "basic",
		"disable|basic|extended|all", "disable|basic|extended|all", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_ort_graph_opt_level, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_INTER_THREADS, "embedding", "inter_op_threads",
		"embedding_inter_threads", KVS_CONFIG_TYPE_INT, "0", "0..256", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_inter_threads, 0, 256, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_ORT_EXECUTION_MODE, "embedding", "ort_execution_mode",
		"embedding_ort_execution_mode", KVS_CONFIG_TYPE_ENUM_STRING, "sequential",
		"sequential|parallel", "sequential|parallel", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_ort_execution_mode, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_WORKER_POOL_ENABLED, "embedding", "worker_pool_enabled",
		"embedding_worker_pool_enabled", KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_worker_pool_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_WORKER_THREADS, "embedding", "worker_threads",
		"embedding_worker_threads", KVS_CONFIG_TYPE_INT, "1", "1..64", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_worker_threads, 1, 64, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_WORKER_QUEUE_CAPACITY, "embedding", "worker_queue_capacity",
		"embedding_worker_queue_capacity|embedding_prepare_queue_capacity",
		KVS_CONFIG_TYPE_INT, "1024", "1..1048576", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_worker_queue_capacity, 1, 1048576, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_PER_THREAD_RUNTIME_ENABLED, "embedding", "per_thread_runtime_enabled",
		"embedding_per_thread_runtime_enabled", KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_per_thread_runtime_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_PER_THREAD_FALLBACK_MUTEX, "embedding", "per_thread_runtime_fallback_mutex",
		"embedding_per_thread_runtime_fallback_mutex", KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_per_thread_runtime_fallback_mutex, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_SHARED_ORT_ENV_ENABLED, "embedding", "shared_ort_env_enabled",
		"embedding_shared_ort_env_enabled", KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_shared_ort_env_enabled, 0, 1, 1, NULL, 0, 0),

	DESC_FIELD(KVS_CFG_EMBEDDING_BATCH_ENABLED, "embedding", "batch_enabled", "embedding_batch_enabled",
		KVS_CONFIG_TYPE_BOOL, "false", "unsupported", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED, embedding_batch_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_BATCH_MAX_SIZE, "embedding", "batch_size", "embedding_batch_max_size",
		KVS_CONFIG_TYPE_INT, "4", "unsupported", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED, embedding_batch_max_size, 1, 64, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_BATCH_MAX_WAIT_US, "embedding", "batch_max_wait_us",
		"embedding_batch_max_wait_us", KVS_CONFIG_TYPE_INT, "500", "unsupported", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED, embedding_batch_max_wait_us, 0, 1000000, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_BATCH_VSET_ENABLED, "embedding", "batch_vset_enabled",
		"embedding_batch_vset_enabled", KVS_CONFIG_TYPE_BOOL, "false", "unsupported", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED, embedding_batch_vset_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_BATCH_VSEARCH_ENABLED, "embedding", "batch_vsearch_enabled",
		"embedding_batch_vsearch_enabled", KVS_CONFIG_TYPE_BOOL, "true", "unsupported", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED, embedding_batch_vsearch_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_BATCH_VSET_MAX_SIZE, "embedding", "batch_vset_max_size",
		"embedding_batch_vset_max_size", KVS_CONFIG_TYPE_INT, "2", "unsupported", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED, embedding_batch_vset_max_size, 1, 64, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_BATCH_VSET_MAX_WAIT_US, "embedding", "batch_vset_max_wait_us",
		"embedding_batch_vset_max_wait_us", KVS_CONFIG_TYPE_INT, "0", "unsupported", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED, embedding_batch_vset_max_wait_us, 0, 1000000, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_BATCH_VSEARCH_MAX_SIZE, "embedding", "batch_vsearch_max_size",
		"embedding_batch_vsearch_max_size", KVS_CONFIG_TYPE_INT, "4", "unsupported", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED, embedding_batch_vsearch_max_size, 1, 64, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_BATCH_VSEARCH_MAX_WAIT_US, "embedding", "batch_vsearch_max_wait_us",
		"embedding_batch_vsearch_max_wait_us", KVS_CONFIG_TYPE_INT, "500", "unsupported", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED, embedding_batch_vsearch_max_wait_us, 0, 1000000, 1, NULL, 0, 0),

	DESC_FIELD(KVS_CFG_EMBEDDING_CACHE_ENABLED, "embedding", "cache_enabled", "embedding_cache_enabled",
		KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_cache_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_CACHE_CAPACITY, "embedding", "cache_capacity", "embedding_cache_capacity",
		KVS_CONFIG_TYPE_INT, "10000", "1..10000000", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_cache_capacity, 1, 10000000, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_CACHE_TTL_MS, "embedding", "cache_ttl_ms", "embedding_cache_ttl_ms",
		KVS_CONFIG_TYPE_INT, "3600000", "1..2147483647", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_cache_ttl_ms, 1, INT_MAX, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EMBEDDING_CACHE_POLICY, "embedding", "cache_policy", "embedding_cache_policy",
		KVS_CONFIG_TYPE_ENUM_STRING, "lru", "lru", "lru", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, embedding_cache_policy, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_OBSERVABILITY_LOG_LEVEL, "observability", "log_level", "log_level",
		KVS_CONFIG_TYPE_ENUM_STRING, "info", "error|warn|info|debug", "error|warn|info|debug", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, log_level, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_OBSERVABILITY_LOG_ASYNC, "observability", "log_async", "log_async",
		KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, log_async, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_OBSERVABILITY_LOG_PATH, "observability", "log_path", "log_file",
		KVS_CONFIG_TYPE_STRING, "", "path", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, log_file, 0, 0, 0, NULL, 0, 0),
	DESC_NONE("observability", "log_queue_capacity", KVS_CONFIG_TYPE_INT, "256", "positive", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED),
	DESC_NONE("observability", "vinfo_stats_enabled", KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED),
	DESC_FIELD(KVS_CFG_OBSERVABILITY_VSEARCH_STAGE_STATS_ENABLED, "observability", "vsearch_stage_stats_enabled",
		"vsearch_stage_stats_enabled", KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, observability_vsearch_stage_stats_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_NONE("observability", "replication_stats_enabled", KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_FUTURE),
	DESC_FIELD(KVS_CFG_OBSERVABILITY_EMBEDDING_PROFILE_ENABLED, "observability", "embedding_profile_enabled",
		"embedding_profile_enabled", KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, observability_embedding_profile_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_OBSERVABILITY_HNSW_BUILD_PROFILE_ENABLED, "observability", "hnsw_build_profile_enabled",
		"hnsw_build_profile_enabled", KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, observability_hnsw_build_profile_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_OBSERVABILITY_HNSW_SEARCH_PROFILE_ENABLED, "observability", "hnsw_search_profile_enabled",
		"hnsw_search_profile_enabled", KVS_CONFIG_TYPE_BOOL, "false", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, observability_hnsw_search_profile_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_NONE("observability", "owner_lock_mode", KVS_CONFIG_TYPE_ENUM_STRING, "weak", "weak|real", "weak|real", 1,
		KVS_CONFIG_STATUS_UNSUPPORTED),

	DESC_FIELD(KVS_CFG_REPLICATION_MASTER_HOST, "replication", "master_host", "master_ip",
		KVS_CONFIG_TYPE_STRING, "", "host", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, replication_master_host, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_REPLICATION_MASTER_SERVICE_PORT, "replication", "master_service_port", "master_port",
		KVS_CONFIG_TYPE_INT, "0", "0..65535", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, replication_master_service_port, 0, 65535, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_REPLICATION_MASTER_SYNC_PORT, "replication", "master_sync_port", "",
		KVS_CONFIG_TYPE_INT, "0", "0..65535", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, replication_master_sync_port, 0, 65535, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_REPLICATION_SYNC_LISTEN_PORT, "replication", "sync_listen_port", "sync_port",
		KVS_CONFIG_TYPE_INT, "0", "0..65535", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, replication_sync_listen_port, 0, 65535, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_REPLICATION_TRANSPORT, "replication", "transport", "sync_mode",
		KVS_CONFIG_TYPE_ENUM_INT, "disabled", "disabled|tcp (legacy alias also accepts ebpf)", "disabled|tcp", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, replication_transport, 0, 0, 0,
		replication_transport_codes, sizeof(replication_transport_codes) / sizeof(replication_transport_codes[0]),
		CFG_FLAG_LEGACY_SYNC_MODE),
	DESC_FIELD(KVS_CFG_REPLICATION_FULL_SYNC_POLICY, "replication", "full_sync_policy", "",
		KVS_CONFIG_TYPE_ENUM_STRING, "required", "required", "required", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, replication_full_sync_policy, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_REPLICATION_NOT_READY_QUERY_POLICY, "replication", "not_ready_query_policy", "",
		KVS_CONFIG_TYPE_ENUM_STRING, "reject", "reject", "reject", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, replication_not_ready_query_policy, 0, 0, 0, NULL, 0, 0),
	DESC_NONE("replication", "backlog_bytes", KVS_CONFIG_TYPE_SIZE, "0", "future", "", 1,
		KVS_CONFIG_STATUS_FUTURE),
	DESC_NONE("replication", "ack_interval_ms", KVS_CONFIG_TYPE_INT, "0", "future", "", 1,
		KVS_CONFIG_STATUS_FUTURE),
	DESC_NONE("replication", "reconnect_backoff_ms", KVS_CONFIG_TYPE_INT, "0", "future", "", 1,
		KVS_CONFIG_STATUS_FUTURE),
	DESC_NONE("replication", "offset", KVS_CONFIG_TYPE_SIZE, "0", "future", "", 0,
		KVS_CONFIG_STATUS_FUTURE),
	DESC_NONE("replication", "partial_resync", KVS_CONFIG_TYPE_BOOL, "false", "future", "", 0,
		KVS_CONFIG_STATUS_FUTURE),
	DESC_NONE("replication", "promotion_policy", KVS_CONFIG_TYPE_STRING, "", "future", "", 0,
		KVS_CONFIG_STATUS_FUTURE),

	DESC_FIELD(KVS_CFG_EBPF_LEGACY_CONF, "ebpf", "legacy_conf", "ebpf_conf",
		KVS_CONFIG_TYPE_STRING, "config/ebpf_sync.conf", "path", "", 1,
		KVS_CONFIG_STATUS_INACTIVE, ebpf_conf, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EBPF_MODE, "ebpf", "mode", "",
		KVS_CONFIG_TYPE_ENUM_INT, "disabled", "disabled|observe|experimental_mirror",
		"disabled|observe|experimental_mirror", 1, KVS_CONFIG_STATUS_IMPLEMENTED, ebpf_mode,
		0, 0, 0, ebpf_mode_codes, sizeof(ebpf_mode_codes) / sizeof(ebpf_mode_codes[0]), 0),
	DESC_FIELD(KVS_CFG_EBPF_OBJECT_PATH, "ebpf", "object_path", "",
		KVS_CONFIG_TYPE_STRING, "src/eBPF/ebpf_kern.o", "path relative to config_root", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ebpf_object_path, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EBPF_OBSERVE_HOST, "ebpf", "observe_host", "",
		KVS_CONFIG_TYPE_STRING, "127.0.0.1", "IPv4 host", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ebpf_observe_host, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EBPF_OBSERVE_PORT, "ebpf", "observe_port", "",
		KVS_CONFIG_TYPE_INT, "8080", "1..65535", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ebpf_observe_port, 1, 65535, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EBPF_MIRROR_TARGETS, "ebpf", "mirror_targets", "",
		KVS_CONFIG_TYPE_STRING, "", "comma-separated IPv4:port targets", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ebpf_mirror_targets, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EBPF_RING_BUFFER_BYTES, "ebpf", "ring_buffer_bytes", "",
		KVS_CONFIG_TYPE_SIZE, "16777216", "4096..1073741824 power of two", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ebpf_ring_buffer_bytes, 4096, 1073741824, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EBPF_MAX_EVENT_BYTES, "ebpf", "max_event_bytes", "",
		KVS_CONFIG_TYPE_SIZE, "4096", "1..4096", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ebpf_max_event_bytes, 1, 4096, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EBPF_USER_QUEUE_CAPACITY, "ebpf", "user_queue_capacity", "",
		KVS_CONFIG_TYPE_INT, "65535", "1..1048576", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ebpf_user_queue_capacity, 1, 1048576, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_EBPF_FLUSH_INTERVAL_US, "ebpf", "flush_interval_us", "",
		KVS_CONFIG_TYPE_INT, "10000", "0..10000000", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, ebpf_flush_interval_us, 0, 10000000, 1, NULL, 0, 0),

	DESC_FIELD(KVS_CFG_VECTOR_SNAPSHOT_ENABLED, "vector_snapshot", "enabled", "vector_snapshot_enabled",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_snapshot_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_SNAPSHOT_MANIFEST_PATH, "vector_snapshot", "manifest_path", "vector_snapshot_manifest_path",
		KVS_CONFIG_TYPE_STRING, "vector_index.manifest", "path relative to server.data_dir", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_snapshot_manifest_path, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_SNAPSHOT_INDEX_PATH, "vector_snapshot", "index_path", "vector_snapshot_index_path",
		KVS_CONFIG_TYPE_STRING, "vector_index.snapshot", "path relative to server.data_dir", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_snapshot_index_path, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_SNAPSHOT_LOAD_ON_START, "vector_snapshot", "load_on_start", "vector_snapshot_load_on_start",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_snapshot_load_on_start, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_SNAPSHOT_INVALID_ACTION, "vector_snapshot", "invalid_action", "vector_snapshot_invalid_action",
		KVS_CONFIG_TYPE_ENUM_STRING, "rebuild", "fail|rebuild", "fail|rebuild", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_snapshot_invalid_action, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_SNAPSHOT_WRITE_AFTER_REBUILD, "vector_snapshot", "write_after_rebuild", "vector_snapshot_write_after_rebuild",
		KVS_CONFIG_TYPE_ENUM_STRING, "best_effort", "off|best_effort", "off|best_effort", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_snapshot_write_after_rebuild, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_SNAPSHOT_CROSS_NODE_REUSE, "vector_snapshot", "cross_node_reuse", "vector_snapshot_cross_node_reuse",
		KVS_CONFIG_TYPE_BOOL, "false", "false (current node-local policy)", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_snapshot_cross_node_reuse, 0, 1, 1, NULL, 0, 0),

	DESC_FIELD(KVS_CFG_VECTOR_REBUILD_ASYNC_ENABLED, "vector_rebuild", "async_enabled", "vector_rebuild_async_enabled",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_rebuild_async_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_REBUILD_DELTA_INITIAL_CAPACITY, "vector_rebuild", "delta_initial_capacity", "vector_rebuild_delta_initial_capacity",
		KVS_CONFIG_TYPE_INT, "16", "1..2147483647", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_rebuild_delta_initial_capacity, 1, INT_MAX, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_REBUILD_DELTA_MAX_ENTRIES, "vector_rebuild", "delta_max_entries", "vector_rebuild_delta_max_entries",
		KVS_CONFIG_TYPE_INT, "1000000", "1..2147483647", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_rebuild_delta_max_entries, 1, INT_MAX, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_REBUILD_RETRY_POLICY, "vector_rebuild", "retry_policy", "vector_rebuild_retry_policy",
		KVS_CONFIG_TYPE_ENUM_STRING, "manual", "manual", "manual", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_rebuild_retry_policy, 0, 0, 0, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_REBUILD_CANCEL_ENABLED, "vector_rebuild", "cancel_enabled", "vector_rebuild_cancel_enabled",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_rebuild_cancel_enabled, 0, 1, 1, NULL, 0, 0),
	DESC_FIELD(KVS_CFG_VECTOR_REBUILD_WRITE_SNAPSHOT_AFTER_PUBLISH, "vector_rebuild", "write_snapshot_after_publish", "vector_rebuild_write_snapshot_after_publish",
		KVS_CONFIG_TYPE_BOOL, "true", "boolean", "", 1,
		KVS_CONFIG_STATUS_IMPLEMENTED, vector_rebuild_write_snapshot_after_publish, 0, 1, 1, NULL, 0, 0)
};

static const size_t config_descriptor_count =
	sizeof(config_descriptors) / sizeof(config_descriptors[0]);

static void config_error_clear(kvs_config_error_t *error) {
	if (error) {
		memset(error, 0, sizeof(*error));
	}
}

static int config_error_set(
	kvs_config_error_t *error,
	const char *path,
	unsigned int line,
	const char *fmt,
	...
) {
	va_list ap;

	if (error) {
		memset(error, 0, sizeof(*error));
		if (path) {
			snprintf(error->path, sizeof(error->path), "%s", path);
		}
		error->line = line;
		va_start(ap, fmt);
		vsnprintf(error->message, sizeof(error->message), fmt, ap);
		va_end(ap);
	}
	return -1;
}

static char *config_trim(char *text) {
	char *end;

	if (!text) {
		return NULL;
	}
	while (*text && isspace((unsigned char)*text)) {
		text++;
	}
	end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1])) {
		end--;
	}
	*end = '\0';
	return text;
}

static int config_token_equal(const char *left, const char *right) {
	return left && right && strcasecmp(left, right) == 0;
}

static int config_name_in_list(const char *name, const char *list) {
	const char *start;
	const char *end;
	size_t length;

	if (!name || !list || list[0] == '\0') {
		return 0;
	}
	start = list;
	while (*start) {
		end = strchr(start, '|');
		length = end ? (size_t)(end - start) : strlen(start);
		if (strlen(name) == length && strncasecmp(name, start, length) == 0) {
			return 1;
		}
		if (!end) {
			break;
		}
		start = end + 1;
	}
	return 0;
}

static const kvs_config_descriptor_internal_t *config_descriptor_by_id(
	kvs_config_field_id_t id
) {
	size_t i;

	for (i = 0; i < config_descriptor_count; ++i) {
		if (config_descriptors[i].public_desc.id == id) {
			return &config_descriptors[i];
		}
	}
	return NULL;
}

static const kvs_config_descriptor_internal_t *config_find_canonical(
	const char *section,
	const char *name
) {
	size_t i;

	for (i = 0; i < config_descriptor_count; ++i) {
		const kvs_config_field_descriptor_t *desc = &config_descriptors[i].public_desc;
		if (config_token_equal(desc->section, section) && config_token_equal(desc->name, name)) {
			return &config_descriptors[i];
		}
	}
	return NULL;
}

static const kvs_config_descriptor_internal_t *config_find_legacy(const char *name) {
	size_t i;

	for (i = 0; i < config_descriptor_count; ++i) {
		if (config_name_in_list(name, config_descriptors[i].public_desc.legacy_names)) {
			return &config_descriptors[i];
		}
	}
	return NULL;
}

static int config_section_known(const char *section) {
	size_t i;

	for (i = 0; i < config_descriptor_count; ++i) {
		if (config_token_equal(section, config_descriptors[i].public_desc.section)) {
			return 1;
		}
	}
	return 0;
}

static const char *config_status_name(kvs_config_field_status_t status) {
	switch (status) {
		case KVS_CONFIG_STATUS_IMPLEMENTED: return "implemented";
		case KVS_CONFIG_STATUS_INACTIVE: return "inactive";
		case KVS_CONFIG_STATUS_UNSUPPORTED: return "unsupported";
		case KVS_CONFIG_STATUS_FUTURE: return "future";
		default: return "unknown";
	}
}

static void config_warn_legacy(
	FILE *warnings,
	const char *legacy_name,
	const kvs_config_descriptor_internal_t *desc,
	const char *origin,
	unsigned int line
) {
	if (!warnings || !legacy_name || !desc) {
		return;
	}
	if (line) {
		fprintf(
			warnings,
			"config warning: deprecated legacy alias '%s' at %s:%u; use '%s'\n",
			legacy_name,
			origin ? origin : "config",
			line,
			desc->public_desc.canonical_name
		);
	} else {
		fprintf(
			warnings,
			"config warning: deprecated legacy alias '%s' at %s; use '%s'\n",
			legacy_name,
			origin ? origin : "cli",
			desc->public_desc.canonical_name
		);
	}
}

static void config_warn_normalized(
	FILE *warnings,
	const char *legacy_name,
	const char *old_value,
	const char *new_value
) {
	if (warnings) {
		fprintf(
			warnings,
			"config warning: legacy '%s=%s' normalized to current effective value '%s'\n",
			legacy_name,
			old_value,
			new_value
		);
	}
}

static int config_parse_bool(const char *value, int *out) {
	if (config_token_equal(value, "yes") || config_token_equal(value, "true") ||
	    strcmp(value, "1") == 0) {
		*out = 1;
		return 0;
	}
	if (config_token_equal(value, "no") || config_token_equal(value, "false") ||
	    strcmp(value, "0") == 0) {
		*out = 0;
		return 0;
	}
	return -1;
}

static int config_parse_int64(const char *value, int64_t *out) {
	char *end = NULL;
	long long parsed;

	errno = 0;
	parsed = strtoll(value, &end, 10);
	if (errno == ERANGE || end == value || !end || *end != '\0') {
		return -1;
	}
	*out = (int64_t)parsed;
	return 0;
}

static int config_parse_uint64(const char *value, uint64_t *out) {
	char *end = NULL;
	unsigned long long parsed;

	if (!value || value[0] == '-') {
		return -1;
	}
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno == ERANGE || end == value || !end || *end != '\0') {
		return -1;
	}
	*out = (uint64_t)parsed;
	return 0;
}

static int config_parse_double(const char *value, double *out) {
	char *end = NULL;
	double parsed;

	errno = 0;
	parsed = strtod(value, &end);
	if (errno == ERANGE || end == value || !end || *end != '\0' || !isfinite(parsed)) {
		return -1;
	}
	*out = parsed;
	return 0;
}

static int config_enum_index(const char *value, const char *values) {
	const char *start = values;
	const char *end;
	size_t length;
	int index = 0;

	while (start && *start) {
		end = strchr(start, '|');
		length = end ? (size_t)(end - start) : strlen(start);
		if (strlen(value) == length && strncasecmp(value, start, length) == 0) {
			return index;
		}
		if (!end) {
			break;
		}
		start = end + 1;
		index++;
	}
	return -1;
}

static int config_enum_copy_canonical(
	const char *value,
	const char *values,
	char *out,
	size_t out_size
) {
	const char *start = values;
	const char *end;
	size_t length;

	while (start && *start) {
		end = strchr(start, '|');
		length = end ? (size_t)(end - start) : strlen(start);
		if (strlen(value) == length && strncasecmp(value, start, length) == 0) {
			if (length + 1 > out_size) {
				return -1;
			}
			memcpy(out, start, length);
			out[length] = '\0';
			return 0;
		}
		if (!end) {
			break;
		}
		start = end + 1;
	}
	return -1;
}

static void config_source_set(
	kvs_config_t *cfg,
	kvs_config_field_id_t id,
	kvs_config_source_kind_t kind,
	int legacy_alias,
	const char *origin,
	unsigned int line
) {
	kvs_config_source_t *source;

	if (!cfg || id < 0 || id >= KVS_CONFIG_FIELD_COUNT) {
		return;
	}
	source = &cfg->sources[id];
	memset(source, 0, sizeof(*source));
	source->kind = kind;
	source->legacy_alias = legacy_alias ? 1 : 0;
	source->line = line;
	if (origin) {
		snprintf(source->origin, sizeof(source->origin), "%s", origin);
	}
}

static int config_assign_value(
	kvs_config_t *cfg,
	const kvs_config_descriptor_internal_t *desc,
	const char *value,
	int legacy_alias,
	kvs_config_source_kind_t source_kind,
	const char *origin,
	unsigned int line,
	FILE *warnings,
	kvs_config_error_t *error,
	int applying_default
) {
	void *target;
	int64_t int_value;
	uint64_t uint_value;
	double double_value;
	int bool_value;
	int enum_index;
	const char *effective_value = value;

	if (!cfg || !desc || !value) {
		return config_error_set(error, origin, line, "invalid configuration assignment");
	}
	if (!applying_default &&
	    (desc->public_desc.status == KVS_CONFIG_STATUS_UNSUPPORTED ||
	     desc->public_desc.status == KVS_CONFIG_STATUS_FUTURE)) {
		return config_error_set(
			error,
			origin,
			line,
			"configuration field '%s' is %s and cannot be configured",
			desc->public_desc.canonical_name,
			config_status_name(desc->public_desc.status)
		);
	}
	if (desc->offset == KVS_CONFIG_NO_OFFSET) {
		return config_error_set(error, origin, line, "field '%s' has no storage", desc->public_desc.canonical_name);
	}

	if (legacy_alias && (desc->flags & CFG_FLAG_LEGACY_INDEX)) {
		if (config_token_equal(value, "flat")) {
			effective_value = "core_flat";
			config_warn_normalized(warnings, "vector_index_type", value, effective_value);
		} else if (!config_token_equal(value, "core_flat") &&
		           !config_token_equal(value, "core_hnsw_v2")) {
			return config_error_set(
				error,
				origin,
				line,
				"removed vector backend '%s' is not supported; use core_flat or core_hnsw_v2",
				value
			);
		}
	}
	if (legacy_alias && (desc->flags & CFG_FLAG_LEGACY_PROVIDER) &&
	    config_token_equal(value, "c_api")) {
		effective_value = "local_c";
		config_warn_normalized(warnings, "embedding_provider", value, effective_value);
	}
	if (legacy_alias && (desc->flags & CFG_FLAG_LEGACY_DATA_PATH) &&
	    strncmp(value, "data/", 5) == 0 && value[5] != '\0') {
		effective_value = value + 5;
		config_warn_normalized(warnings, desc->public_desc.legacy_names, value, effective_value);
	}
	if (legacy_alias && (desc->flags & CFG_FLAG_LEGACY_SYNC_MODE) &&
	    config_token_equal(value, "ebpf")) {
		if (cfg->sources[KVS_CFG_EBPF_MODE].kind != KVS_CONFIG_SOURCE_DEFAULT) {
			return config_error_set(error, origin, line,
				"legacy sync_mode=ebpf conflicts with explicit ebpf.mode");
		}
		*((kvs_replication_transport_t *)((char *)cfg + desc->offset)) =
			KVS_REPLICATION_DISABLED;
		cfg->ebpf_mode = KVS_EBPF_EXPERIMENTAL_MIRROR;
		config_source_set(cfg, desc->public_desc.id, source_kind, 1, origin, line);
		config_source_set(cfg, KVS_CFG_EBPF_MODE, source_kind, 1, origin, line);
		config_warn_normalized(warnings, "sync_mode", value,
			"replication.transport=disabled + ebpf.mode=experimental_mirror");
		if (warnings) {
			fprintf(warnings,
				"[kvs_config] HIGH: legacy sync_mode=ebpf is experimental and has no offset, ACK, backlog or READY semantics\n");
		}
		return 0;
	}
	if (!legacy_alias && desc->public_desc.id == KVS_CFG_EBPF_MODE &&
	    cfg->sources[KVS_CFG_EBPF_MODE].legacy_alias) {
		return config_error_set(error, origin, line,
			"ebpf.mode duplicates legacy sync_mode=ebpf");
	}
	if (legacy_alias && (desc->flags & CFG_FLAG_LEGACY_HNSW_M)) {
		if (config_parse_int64(value, &int_value) != 0 || int_value < 4 || int_value > 65535) {
			return config_error_set(error, origin, line, "invalid legacy vector_hnsw_m '%s'", value);
		}
		if (int_value < 48) {
			effective_value = "48";
			config_warn_normalized(warnings, "vector_hnsw_m", value, effective_value);
		}
	}
	if (legacy_alias && (desc->flags & CFG_FLAG_LEGACY_HNSW_EFC)) {
		if (config_parse_int64(value, &int_value) != 0 || int_value <= 0 || int_value > INT_MAX) {
			return config_error_set(error, origin, line, "invalid legacy vector_hnsw_ef_construction '%s'", value);
		}
		if (int_value < 512) {
			effective_value = "512";
			config_warn_normalized(warnings, "vector_hnsw_ef_construction", value, effective_value);
		}
	}
	if (legacy_alias && (desc->flags & CFG_FLAG_LEGACY_HNSW_EFS)) {
		if (config_parse_int64(value, &int_value) != 0 || int_value <= 0 || int_value > INT_MAX) {
			return config_error_set(error, origin, line, "invalid legacy vector_hnsw_ef_search '%s'", value);
		}
		if (int_value < 1024) {
			effective_value = "1024";
			config_warn_normalized(warnings, "vector_hnsw_ef_search", value, effective_value);
		}
	}
	if (legacy_alias && (desc->flags & CFG_FLAG_LEGACY_INITIAL_CAP)) {
		if (config_parse_int64(value, &int_value) != 0 || int_value < 0 || int_value > INT_MAX) {
			return config_error_set(error, origin, line, "invalid legacy vector_hnsw_initial_capacity '%s'", value);
		}
		if (int_value == 0) {
			effective_value = "16";
			config_warn_normalized(warnings, "vector_hnsw_initial_capacity", value, effective_value);
		}
	}

	target = (char *)cfg + desc->offset;
	switch (desc->public_desc.type) {
		case KVS_CONFIG_TYPE_BOOL:
			if (config_parse_bool(effective_value, &bool_value) != 0) {
				return config_error_set(error, origin, line, "invalid boolean for '%s': '%s'",
					desc->public_desc.canonical_name, effective_value);
			}
			*(int *)target = bool_value;
			break;
		case KVS_CONFIG_TYPE_INT:
			if (config_parse_int64(effective_value, &int_value) != 0 ||
			    int_value < INT_MIN || int_value > INT_MAX) {
				return config_error_set(error, origin, line, "invalid integer for '%s': '%s'",
					desc->public_desc.canonical_name, effective_value);
			}
			if (desc->has_range &&
			    ((long double)int_value < desc->min_value || (long double)int_value > desc->max_value)) {
				return config_error_set(error, origin, line, "value for '%s' is out of range (%s): '%s'",
					desc->public_desc.canonical_name, desc->public_desc.constraints, effective_value);
			}
			*(int *)target = (int)int_value;
			break;
		case KVS_CONFIG_TYPE_SIZE:
			if (config_parse_uint64(effective_value, &uint_value) != 0 || uint_value > SIZE_MAX) {
				return config_error_set(error, origin, line, "invalid size for '%s': '%s'",
					desc->public_desc.canonical_name, effective_value);
			}
			if (desc->has_range &&
			    ((long double)uint_value < desc->min_value || (long double)uint_value > desc->max_value)) {
				return config_error_set(error, origin, line, "value for '%s' is out of range (%s): '%s'",
					desc->public_desc.canonical_name, desc->public_desc.constraints, effective_value);
			}
			*(size_t *)target = (size_t)uint_value;
			break;
		case KVS_CONFIG_TYPE_FLOAT:
			if (config_parse_double(effective_value, &double_value) != 0) {
				return config_error_set(error, origin, line, "invalid floating-point value for '%s': '%s'",
					desc->public_desc.canonical_name, effective_value);
			}
			if (desc->has_range &&
			    ((long double)double_value < desc->min_value || (long double)double_value > desc->max_value)) {
				return config_error_set(error, origin, line, "value for '%s' is out of range (%s): '%s'",
					desc->public_desc.canonical_name, desc->public_desc.constraints, effective_value);
			}
			*(float *)target = (float)double_value;
			break;
		case KVS_CONFIG_TYPE_STRING:
			if (!applying_default && effective_value[0] == '\0') {
				return config_error_set(error, origin, line, "missing value for '%s'", desc->public_desc.canonical_name);
			}
			if (strlen(effective_value) >= desc->storage_size) {
				return config_error_set(error, origin, line, "value for '%s' is too long",
					desc->public_desc.canonical_name);
			}
			snprintf((char *)target, desc->storage_size, "%s", effective_value);
			break;
		case KVS_CONFIG_TYPE_ENUM_INT:
			enum_index = config_enum_index(effective_value, desc->public_desc.enum_values);
			if (enum_index < 0 || (size_t)enum_index >= desc->enum_count) {
				return config_error_set(error, origin, line, "invalid enum for '%s' (%s): '%s'",
					desc->public_desc.canonical_name, desc->public_desc.enum_values, effective_value);
			}
			*(int *)target = desc->enum_codes[enum_index];
			break;
		case KVS_CONFIG_TYPE_ENUM_STRING:
			if (config_enum_copy_canonical(effective_value, desc->public_desc.enum_values,
				(char *)target, desc->storage_size) != 0) {
				return config_error_set(error, origin, line, "invalid enum for '%s' (%s): '%s'",
					desc->public_desc.canonical_name, desc->public_desc.enum_values, effective_value);
			}
			break;
		default:
			return config_error_set(error, origin, line, "unknown type for '%s'",
				desc->public_desc.canonical_name);
	}

	config_source_set(cfg, desc->public_desc.id, source_kind, legacy_alias, origin, line);
	if (desc->public_desc.id == KVS_CFG_EMBEDDING_WORKER_QUEUE_CAPACITY) {
		cfg->embedding_prepare_queue_capacity = cfg->embedding_worker_queue_capacity;
	}
	return 0;
}

void kvs_config_init_default(kvs_config_t *cfg) {
	size_t i;
	kvs_config_error_t ignored;

	if (!cfg) {
		return;
	}
	memset(cfg, 0, sizeof(*cfg));
	for (i = 0; i < config_descriptor_count; ++i) {
		const kvs_config_descriptor_internal_t *desc = &config_descriptors[i];
		if (desc->offset == KVS_CONFIG_NO_OFFSET || desc->public_desc.default_value == NULL) {
			continue;
		}
		config_error_clear(&ignored);
		(void)config_assign_value(
			cfg,
			desc,
			desc->public_desc.default_value,
			0,
			KVS_CONFIG_SOURCE_DEFAULT,
			"default",
			0,
			NULL,
			&ignored,
			1
		);
	}
	cfg->embedding_prepare_queue_capacity = cfg->embedding_worker_queue_capacity;
	cfg->storage_enabled_engine_mask =
		KVS_STORAGE_ENGINE_ARRAY |
		KVS_STORAGE_ENGINE_RBTREE |
		KVS_STORAGE_ENGINE_HASH |
		KVS_STORAGE_ENGINE_SKIPTABLE;
}

static int config_parse_file(
	kvs_config_t *cfg,
	const char *path,
	FILE *warnings,
	kvs_config_error_t *error
) {
	FILE *fp;
	char raw[KVS_MAX_LINE + 2];
	char section[64] = "";
	unsigned char seen[sizeof(config_descriptors) / sizeof(config_descriptors[0])];
	unsigned int line_number = 0;

	memset(seen, 0, sizeof(seen));
	fp = fopen(path, "r");
	if (!fp) {
		return config_error_set(error, path, 0, "failed to open configuration file: %s", strerror(errno));
	}

	while (fgets(raw, sizeof(raw), fp)) {
		char *line;
		char *comment;
		char *equals;
		char *key;
		char *value;
		const kvs_config_descriptor_internal_t *desc;
		size_t descriptor_index;
		int legacy_alias;

		line_number++;
		if (!strchr(raw, '\n') && !feof(fp)) {
			fclose(fp);
			return config_error_set(error, path, line_number, "configuration line exceeds %d bytes", KVS_MAX_LINE);
		}
		comment = strchr(raw, '#');
		if (comment) {
			*comment = '\0';
		}
		line = config_trim(raw);
		if (!line || line[0] == '\0') {
			continue;
		}
		if (line[0] == '[') {
			char *closing = strchr(line, ']');
			char *trailing;
			if (!closing) {
				fclose(fp);
				return config_error_set(error, path, line_number, "unterminated configuration section");
			}
			*closing = '\0';
			trailing = config_trim(closing + 1);
			if (trailing[0] != '\0') {
				fclose(fp);
				return config_error_set(error, path, line_number, "trailing content after section name");
			}
			line = config_trim(line + 1);
			if (line[0] == '\0' || strlen(line) >= sizeof(section) || !config_section_known(line)) {
				fclose(fp);
				return config_error_set(error, path, line_number, "unknown configuration section '%s'", line);
			}
			snprintf(section, sizeof(section), "%s", line);
			continue;
		}

		equals = strchr(line, '=');
		if (!equals) {
			fclose(fp);
			return config_error_set(error, path, line_number, "configuration entry is missing '='");
		}
		*equals = '\0';
		key = config_trim(line);
		value = config_trim(equals + 1);
		if (!key || key[0] == '\0') {
			fclose(fp);
			return config_error_set(error, path, line_number, "configuration key is empty");
		}
		if (!value || value[0] == '\0') {
			fclose(fp);
			return config_error_set(error, path, line_number, "configuration value for '%s' is empty", key);
		}

		legacy_alias = section[0] == '\0';
		desc = legacy_alias ? config_find_legacy(key) : config_find_canonical(section, key);
		if (!desc) {
			fclose(fp);
			return config_error_set(
				error,
				path,
				line_number,
				"unknown configuration item '%s%s%s'",
				section[0] ? section : "",
				section[0] ? "." : "",
				key
			);
		}
		descriptor_index = (size_t)(desc - config_descriptors);
		if (seen[descriptor_index]) {
			fclose(fp);
			return config_error_set(error, path, line_number,
				"duplicate configuration for canonical field '%s'", desc->public_desc.canonical_name);
		}
		seen[descriptor_index] = 1;
		if (legacy_alias) {
			config_warn_legacy(warnings, key, desc, path, line_number);
		}
		if (config_assign_value(cfg, desc, value, legacy_alias, KVS_CONFIG_SOURCE_FILE,
			path, line_number, warnings, error, 0) != 0) {
			fclose(fp);
			return -1;
		}
	}

	if (ferror(fp)) {
		fclose(fp);
		return config_error_set(error, path, line_number, "failed while reading configuration file");
	}
	fclose(fp);
	return 0;
}

int kvs_config_prescan_cmdline(
	int argc,
	char *argv[],
	kvs_config_bootstrap_t *bootstrap,
	kvs_config_error_t *error
) {
	int i;

	if (!bootstrap) {
		return config_error_set(error, NULL, 0, "bootstrap output is null");
	}
	config_error_clear(error);
	memset(bootstrap, 0, sizeof(*bootstrap));
	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) {
			if (i + 1 >= argc || argv[i + 1][0] == '\0') {
				return config_error_set(error, NULL, 0, "%s requires a path", argv[i]);
			}
			if (bootstrap->explicit_config) {
				return config_error_set(error, NULL, 0, "configuration path specified more than once");
			}
			bootstrap->explicit_config = 1;
			snprintf(bootstrap->config_path, sizeof(bootstrap->config_path), "%s", argv[++i]);
		} else if (strcmp(argv[i], "--no-config") == 0) {
			if (bootstrap->no_config) {
				return config_error_set(error, NULL, 0, "--no-config specified more than once");
			}
			bootstrap->no_config = 1;
		}
	}
	if (bootstrap->no_config && bootstrap->explicit_config) {
		return config_error_set(error, NULL, 0, "--no-config cannot be combined with --config");
	}
	return 0;
}

static int config_cli_assign(
	kvs_config_t *cfg,
	const kvs_config_descriptor_internal_t *desc,
	const char *option_name,
	const char *value,
	int legacy_alias,
	unsigned char *seen,
	FILE *warnings,
	kvs_config_error_t *error
) {
	size_t descriptor_index = (size_t)(desc - config_descriptors);

	if (!desc->public_desc.cli_override) {
		return config_error_set(error, NULL, 0, "field '%s' cannot be overridden by CLI",
			desc->public_desc.canonical_name);
	}
	if (seen[descriptor_index]) {
		return config_error_set(error, NULL, 0, "duplicate CLI override for '%s'",
			desc->public_desc.canonical_name);
	}
	seen[descriptor_index] = 1;
	if (legacy_alias) {
		config_warn_legacy(warnings, option_name, desc, "cli", 0);
	}
	return config_assign_value(cfg, desc, value, legacy_alias, KVS_CONFIG_SOURCE_CLI,
		"cli", 0, warnings, error, 0);
}

static int config_apply_cli(
	kvs_config_t *cfg,
	int argc,
	char *argv[],
	FILE *warnings,
	kvs_config_error_t *error
) {
	unsigned char seen[sizeof(config_descriptors) / sizeof(config_descriptors[0])];
	int i;

	memset(seen, 0, sizeof(seen));
	for (i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		const char *name;
		const kvs_config_descriptor_internal_t *desc;
		int legacy_alias;

		if (strcmp(arg, "--config") == 0 || strcmp(arg, "-c") == 0) {
			i++;
			continue;
		}
		if (strcmp(arg, "--no-config") == 0) {
			continue;
		}
		if (strcmp(arg, "--slave") == 0) {
			const kvs_config_descriptor_internal_t *transport_desc =
				config_descriptor_by_id(KVS_CFG_REPLICATION_TRANSPORT);
			desc = config_descriptor_by_id(KVS_CFG_SERVER_ROLE);
			if (config_cli_assign(cfg, desc, "--slave", "slave", 1, seen, warnings, error) != 0 ||
			    config_cli_assign(cfg, transport_desc, "--slave", "tcp", 1,
				seen, warnings, error) != 0) {
				return -1;
			}
			continue;
		}
		if (strcmp(arg, "--master") == 0) {
			const kvs_config_descriptor_internal_t *host_desc;
			const kvs_config_descriptor_internal_t *master_port_desc;
			const kvs_config_descriptor_internal_t *port_desc;
			const kvs_config_descriptor_internal_t *role_desc;
			const kvs_config_descriptor_internal_t *transport_desc;
			if (i + 3 >= argc) {
				return config_error_set(error, NULL, 0, "--master requires <ip> <master_port> <listen_port>");
			}
			host_desc = config_descriptor_by_id(KVS_CFG_REPLICATION_MASTER_HOST);
			master_port_desc = config_descriptor_by_id(KVS_CFG_REPLICATION_MASTER_SERVICE_PORT);
			port_desc = config_descriptor_by_id(KVS_CFG_NETWORK_PORT);
			role_desc = config_descriptor_by_id(KVS_CFG_SERVER_ROLE);
			transport_desc = config_descriptor_by_id(KVS_CFG_REPLICATION_TRANSPORT);
			if (config_cli_assign(cfg, host_desc, "--master", argv[++i], 1, seen, warnings, error) != 0 ||
			    config_cli_assign(cfg, master_port_desc, "--master", argv[++i], 1, seen, warnings, error) != 0 ||
			    config_cli_assign(cfg, port_desc, "--master", argv[++i], 1, seen, warnings, error) != 0 ||
			    config_cli_assign(cfg, role_desc, "--master", "slave", 1, seen, warnings, error) != 0 ||
			    config_cli_assign(cfg, transport_desc, "--master", "tcp", 1,
				seen, warnings, error) != 0) {
				return -1;
			}
			continue;
		}
		if (strncmp(arg, "--", 2) != 0 || arg[2] == '\0') {
			return config_error_set(error, NULL, 0, "unexpected command-line argument '%s'", arg);
		}
		name = arg + 2;
		legacy_alias = strchr(name, '.') == NULL;
		if (legacy_alias) {
			desc = config_find_legacy(name);
		} else {
			char canonical[KVS_CONFIG_PATH_MAX];
			char *dot;
			if (strlen(name) >= sizeof(canonical)) {
				return config_error_set(error, NULL, 0, "CLI option is too long: '%s'", arg);
			}
			snprintf(canonical, sizeof(canonical), "%s", name);
			dot = strchr(canonical, '.');
			*dot = '\0';
			desc = config_find_canonical(canonical, dot + 1);
		}
		if (!desc) {
			return config_error_set(error, NULL, 0, "unknown command-line option '%s'", arg);
		}
		if (i + 1 >= argc) {
			return config_error_set(error, NULL, 0, "%s requires a value", arg);
		}
		if (config_cli_assign(cfg, desc, name, argv[++i], legacy_alias,
			seen, warnings, error) != 0) {
			return -1;
		}
	}
	return 0;
}

static int config_normalize_absolute(
	const char *path,
	char *out,
	size_t out_size
) {
	char copy[KVS_CONFIG_PATH_MAX * 2];
	char *segments[256];
	size_t segment_count = 0;
	char *saveptr = NULL;
	char *token;
	size_t used = 0;
	size_t i;

	if (!path || path[0] != '/' || !out || out_size < 2 || strlen(path) >= sizeof(copy)) {
		return -1;
	}
	snprintf(copy, sizeof(copy), "%s", path);
	token = strtok_r(copy, "/", &saveptr);
	while (token) {
		if (strcmp(token, ".") == 0 || token[0] == '\0') {
			/* skip */
		} else if (strcmp(token, "..") == 0) {
			if (segment_count > 0) {
				segment_count--;
			}
		} else {
			if (segment_count >= sizeof(segments) / sizeof(segments[0])) {
				return -1;
			}
			segments[segment_count++] = token;
		}
		token = strtok_r(NULL, "/", &saveptr);
	}
	out[used++] = '/';
	out[used] = '\0';
	for (i = 0; i < segment_count; ++i) {
		size_t length = strlen(segments[i]);
		if (used + length + (i + 1 < segment_count ? 1 : 0) + 1 > out_size) {
			return -1;
		}
		memcpy(out + used, segments[i], length);
		used += length;
		if (i + 1 < segment_count) {
			out[used++] = '/';
		}
		out[used] = '\0';
	}
	return 0;
}

static int config_resolve_against(
	const char *base,
	const char *path,
	char *out,
	size_t out_size
) {
	char joined[KVS_CONFIG_PATH_MAX * 2];

	if (!base || !path || !out || out_size == 0) {
		return -1;
	}
	if (path[0] == '/') {
		return config_normalize_absolute(path, out, out_size);
	}
	if (snprintf(joined, sizeof(joined), "%s/%s", base, path) >= (int)sizeof(joined)) {
		return -1;
	}
	return config_normalize_absolute(joined, out, out_size);
}

static int config_directory_name(const char *path, char *out, size_t out_size) {
	const char *slash;
	size_t length;

	if (!path || path[0] != '/' || !out || out_size == 0) {
		return -1;
	}
	slash = strrchr(path, '/');
	if (!slash) {
		return -1;
	}
	length = slash == path ? 1 : (size_t)(slash - path);
	if (length + 1 > out_size) {
		return -1;
	}
	memcpy(out, path, length);
	out[length] = '\0';
	return 0;
}

static unsigned int config_engine_bit(const char *name) {
	if (config_token_equal(name, "array")) return KVS_STORAGE_ENGINE_ARRAY;
	if (config_token_equal(name, "rbtree")) return KVS_STORAGE_ENGINE_RBTREE;
	if (config_token_equal(name, "hash")) return KVS_STORAGE_ENGINE_HASH;
	if (config_token_equal(name, "skiptable")) return KVS_STORAGE_ENGINE_SKIPTABLE;
	return 0;
}

static int config_parse_engine_mask(
	const char *value,
	unsigned int *mask_out,
	kvs_config_error_t *error,
	const char *path
) {
	char copy[64];
	char *saveptr = NULL;
	char *token;
	unsigned int mask = 0;

	if (!value || value[0] == '\0' || strlen(value) >= sizeof(copy)) {
		return config_error_set(error, path, 0, "storage.enabled_engines is empty or too long");
	}
	snprintf(copy, sizeof(copy), "%s", value);
	token = strtok_r(copy, ",", &saveptr);
	while (token) {
		unsigned int bit;
		token = config_trim(token);
		bit = config_engine_bit(token);
		if (bit == 0) {
			return config_error_set(error, path, 0, "unknown storage engine '%s'", token);
		}
		if (mask & bit) {
			return config_error_set(error, path, 0, "duplicate storage engine '%s'", token);
		}
		mask |= bit;
		token = strtok_r(NULL, ",", &saveptr);
	}
	if (mask == 0) {
		return config_error_set(error, path, 0, "storage.enabled_engines must not be empty");
	}
	*mask_out = mask;
	return 0;
}

static int config_parse_mirror_targets(
	kvs_config_t *cfg,
	kvs_config_error_t *error
) {
	char copy[sizeof(cfg->ebpf_mirror_targets)];
	char *saveptr = NULL;
	char *token;
	size_t count = 0;

	cfg->ebpf_mirror_target_count = 0;
	memset(cfg->ebpf_mirror_endpoints, 0, sizeof(cfg->ebpf_mirror_endpoints));
	if (cfg->ebpf_mirror_targets[0] == '\0') {
		return 0;
	}
	snprintf(copy, sizeof(copy), "%s", cfg->ebpf_mirror_targets);
	token = strtok_r(copy, ",", &saveptr);
	while (token) {
		char *colon;
		char *host;
		char *port_text;
		int64_t port;
		struct in_addr address;
		size_t i;

		token = config_trim(token);
		colon = strrchr(token, ':');
		if (!colon || colon == token || colon[1] == '\0') {
			return config_error_set(error, cfg->config_path, 0,
				"invalid ebpf.mirror_targets endpoint '%s'; expected IPv4:port", token);
		}
		*colon = '\0';
		host = config_trim(token);
		port_text = config_trim(colon + 1);
		if (inet_pton(AF_INET, host, &address) != 1 ||
		    config_parse_int64(port_text, &port) != 0 || port <= 0 || port > 65535) {
			return config_error_set(error, cfg->config_path, 0,
				"invalid ebpf.mirror_targets endpoint '%s:%s'", host, port_text);
		}
		if (count >= KVS_CONFIG_MAX_MIRROR_TARGETS) {
			return config_error_set(error, cfg->config_path, 0,
				"ebpf.mirror_targets exceeds %d entries", KVS_CONFIG_MAX_MIRROR_TARGETS);
		}
		for (i = 0; i < count; ++i) {
			if (strcmp(cfg->ebpf_mirror_endpoints[i].host, host) == 0 &&
			    cfg->ebpf_mirror_endpoints[i].port == (int)port) {
				return config_error_set(error, cfg->config_path, 0,
					"duplicate ebpf.mirror_targets endpoint '%s:%lld'",
					host, (long long)port);
			}
		}
		snprintf(cfg->ebpf_mirror_endpoints[count].host,
			sizeof(cfg->ebpf_mirror_endpoints[count].host), "%s", host);
		cfg->ebpf_mirror_endpoints[count].port = (int)port;
		count++;
		token = strtok_r(NULL, ",", &saveptr);
	}
	cfg->ebpf_mirror_target_count = count;
	return 0;
}

static int config_resolve_runtime_field(
	kvs_config_t *cfg,
	char *field,
	size_t field_size,
	const char *field_name,
	kvs_config_error_t *error
) {
	char resolved[KVS_CONFIG_PATH_MAX];

	if (config_resolve_against(cfg->data_dir, field, resolved, sizeof(resolved)) != 0 ||
	    strlen(resolved) >= field_size) {
		return config_error_set(error, cfg->config_path, 0, "failed to resolve %s", field_name);
	}
	snprintf(field, field_size, "%s", resolved);
	return 0;
}

static uint32_t config_fnv1a32(const void *data, size_t length) {
	const unsigned char *bytes = (const unsigned char *)data;
	uint32_t hash = 2166136261u;
	size_t i;

	for (i = 0; i < length; ++i) {
		hash ^= (uint32_t)bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static uint64_t config_fnv1a64_update(uint64_t hash, const void *data, size_t length) {
	const unsigned char *bytes = (const unsigned char *)data;
	size_t i;

	for (i = 0; i < length; ++i) {
		hash ^= (uint64_t)bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

static int config_hash_file(
	const char *path,
	uint64_t *out,
	kvs_config_error_t *error,
	const char *config_path,
	const char *field_name
) {
	unsigned char buffer[8192];
	uint64_t hash = 1469598103934665603ull;
	FILE *fp;

	if (!path || !out) {
		return config_error_set(error, config_path, 0, "invalid %s hash request", field_name);
	}
	fp = fopen(path, "rb");
	if (!fp) {
		return config_error_set(error, config_path, 0, "%s is not readable: %s", field_name, path);
	}
	for (;;) {
		size_t count = fread(buffer, 1, sizeof(buffer), fp);
		hash = config_fnv1a64_update(hash, buffer, count);
		if (count < sizeof(buffer)) {
			if (ferror(fp)) {
				fclose(fp);
				return config_error_set(error, config_path, 0, "failed to read %s: %s", field_name, path);
			}
			break;
		}
	}
	fclose(fp);
	*out = hash;
	return 0;
}

static int config_resolve_asset_field(
	kvs_config_t *cfg,
	char *field,
	size_t field_size,
	const char *field_name,
	kvs_config_error_t *error
) {
	char resolved[KVS_CONFIG_PATH_MAX];

	if (!field || field[0] == '\0') {
		return 0;
	}
	if (config_resolve_against(cfg->config_root, field, resolved, sizeof(resolved)) != 0 ||
	    strlen(resolved) >= field_size) {
		return config_error_set(error, cfg->config_path, 0, "failed to resolve %s", field_name);
	}
	snprintf(field, field_size, "%s", resolved);
	return 0;
}

static int config_derive_embedding_identity(
	kvs_config_t *cfg,
	const char *legacy_model_path,
	kvs_config_error_t *error
) {
	char legacy[2 * KVS_CONFIG_PATH_MAX];
	uint64_t model_hash = 0;
	uint64_t vocab_hash = 0;
	uint64_t identity = 1469598103934665603ull;
	int written;

	written = snprintf(
		legacy,
		sizeof(legacy),
		"%s|%s|%d",
		cfg->embedding_provider,
		legacy_model_path ? legacy_model_path : "",
		cfg->embedding_dim
	);
	if (written < 0 || (size_t)written >= sizeof(legacy)) {
		return config_error_set(error, cfg->config_path, 0, "legacy embedding identity is too long");
	}
	cfg->embedding_legacy_model_identity = config_fnv1a32(legacy, (size_t)written);

	if (cfg->vector_enabled && config_token_equal(cfg->embedding_provider, "local_c")) {
		if (config_hash_file(cfg->embedding_model_path, &model_hash, error,
			cfg->config_path, "embedding.model_path") != 0 ||
		    config_hash_file(cfg->embedding_vocab_path, &vocab_hash, error,
			cfg->config_path, "embedding.vocab_path") != 0) {
			return -1;
		}
	}
	identity = config_fnv1a64_update(identity, cfg->embedding_provider,
		strlen(cfg->embedding_provider));
	identity = config_fnv1a64_update(identity, &model_hash, sizeof(model_hash));
	identity = config_fnv1a64_update(identity, &vocab_hash, sizeof(vocab_hash));
	identity = config_fnv1a64_update(identity, &cfg->embedding_dim,
		sizeof(cfg->embedding_dim));
	identity = config_fnv1a64_update(identity, &cfg->vector_normalize,
		sizeof(cfg->vector_normalize));
	identity = config_fnv1a64_update(identity, cfg->embedding_model_version,
		strlen(cfg->embedding_model_version));
	cfg->embedding_model_identity = identity;
	return 0;
}

int kvs_config_derive(kvs_config_t *cfg, kvs_config_error_t *error) {
	char base[KVS_CONFIG_PATH_MAX];
	char root[KVS_CONFIG_PATH_MAX];
	char data[KVS_CONFIG_PATH_MAX];
	char legacy_model_path[KVS_CONFIG_PATH_MAX];
	long derived_port;

	if (!cfg) {
		return config_error_set(error, NULL, 0, "configuration object is null");
	}
	config_error_clear(error);
	snprintf(legacy_model_path, sizeof(legacy_model_path), "%s", cfg->embedding_model_path);
	if (cfg->config_path[0]) {
		if (config_directory_name(cfg->config_path, base, sizeof(base)) != 0) {
			return config_error_set(error, cfg->config_path, 0, "failed to determine configuration directory");
		}
	} else if (!getcwd(base, sizeof(base))) {
		return config_error_set(error, NULL, 0, "getcwd failed: %s", strerror(errno));
	}
	if (config_resolve_against(base, cfg->config_root, root, sizeof(root)) != 0) {
		return config_error_set(error, cfg->config_path, 0, "failed to resolve server.config_root");
	}
	if (config_resolve_against(root, cfg->data_dir, data, sizeof(data)) != 0) {
		return config_error_set(error, cfg->config_path, 0, "failed to resolve server.data_dir");
	}
	snprintf(cfg->config_root, sizeof(cfg->config_root), "%s", root);
	snprintf(cfg->data_dir, sizeof(cfg->data_dir), "%s", data);
	if (config_parse_engine_mask(cfg->storage_enabled_engines,
			&cfg->storage_enabled_engine_mask, error, cfg->config_path) != 0) {
		return -1;
	}
	if (config_resolve_runtime_field(cfg, cfg->rdb_filename, sizeof(cfg->rdb_filename),
			"persistence.rdb_path", error) != 0 ||
	    config_resolve_runtime_field(cfg, cfg->aof_filename, sizeof(cfg->aof_filename),
			"persistence.aof_path", error) != 0 ||
	    config_resolve_runtime_field(cfg, cfg->dump_filename, sizeof(cfg->dump_filename),
			"persistence.legacy_dump_path", error) != 0) {
		return -1;
	}
	if (config_resolve_runtime_field(cfg, cfg->vector_snapshot_manifest_path,
			sizeof(cfg->vector_snapshot_manifest_path), "vector_snapshot.manifest_path", error) != 0 ||
	    config_resolve_runtime_field(cfg, cfg->vector_snapshot_index_path,
			sizeof(cfg->vector_snapshot_index_path), "vector_snapshot.index_path", error) != 0 ||
	    config_resolve_asset_field(cfg, cfg->embedding_model_path,
			sizeof(cfg->embedding_model_path), "embedding.model_path", error) != 0 ||
	    config_resolve_asset_field(cfg, cfg->ebpf_object_path,
			sizeof(cfg->ebpf_object_path), "ebpf.object_path", error) != 0) {
		return -1;
	}
	if (cfg->vector_enabled &&
	    config_token_equal(cfg->embedding_provider, "local_c") &&
	    cfg->embedding_vocab_path[0] == '\0' && cfg->embedding_model_path[0] != '\0') {
		char model_dir[KVS_CONFIG_PATH_MAX];
		if (config_directory_name(cfg->embedding_model_path, model_dir, sizeof(model_dir)) != 0 ||
		    snprintf(cfg->embedding_vocab_path, sizeof(cfg->embedding_vocab_path),
			"%s/vocab.txt", model_dir) >= (int)sizeof(cfg->embedding_vocab_path)) {
			return config_error_set(error, cfg->config_path, 0, "failed to derive embedding.vocab_path");
		}
		config_source_set(cfg, KVS_CFG_EMBEDDING_VOCAB_PATH, KVS_CONFIG_SOURCE_DERIVED,
			1, "legacy-model-directory/vocab.txt", 0);
		fprintf(stderr,
			"[kvs_config] deprecated: embedding.vocab_path was omitted; derived %s\n",
			cfg->embedding_vocab_path);
	} else if (config_resolve_asset_field(cfg, cfg->embedding_vocab_path,
			sizeof(cfg->embedding_vocab_path), "embedding.vocab_path", error) != 0) {
		return -1;
	}
	if (cfg->sources[KVS_CFG_EMBEDDING_OUTPUT_DIMENSION].legacy_alias &&
	    cfg->sources[KVS_CFG_VECTOR_DIMENSION].kind == KVS_CONFIG_SOURCE_DEFAULT) {
		cfg->vector_dimension = cfg->embedding_dim;
		config_source_set(cfg, KVS_CFG_VECTOR_DIMENSION, KVS_CONFIG_SOURCE_DERIVED,
			1, "legacy embedding_dim", 0);
	}
	if (cfg->sources[KVS_CFG_VECTOR_HNSW_M0].kind == KVS_CONFIG_SOURCE_DEFAULT) {
		if (cfg->vector_hnsw_m > INT_MAX / 2) {
			return config_error_set(error, cfg->config_path, 0, "derived vector.hnsw_m0 overflows");
		}
		cfg->vector_hnsw_m0 = cfg->vector_hnsw_m * 2;
		config_source_set(cfg, KVS_CFG_VECTOR_HNSW_M0, KVS_CONFIG_SOURCE_DERIVED,
			0, "vector.hnsw_m*2", 0);
	}
	if (config_derive_embedding_identity(cfg, legacy_model_path, error) != 0) {
		return -1;
	}

	if (cfg->role == KVS_ROLE_SLAVE &&
	    cfg->sources[KVS_CFG_REPLICATION_SYNC_LISTEN_PORT].legacy_alias) {
		if (cfg->sources[KVS_CFG_REPLICATION_MASTER_SYNC_PORT].kind != KVS_CONFIG_SOURCE_DEFAULT) {
			return config_error_set(error, cfg->config_path, 0,
				"legacy sync_port duplicates replication.master_sync_port");
		}
		cfg->replication_master_sync_port = cfg->replication_sync_listen_port;
		cfg->replication_sync_listen_port = 0;
		config_source_set(cfg, KVS_CFG_REPLICATION_MASTER_SYNC_PORT,
			KVS_CONFIG_SOURCE_DERIVED, 1, "legacy sync_port for slave", 0);
		config_source_set(cfg, KVS_CFG_REPLICATION_SYNC_LISTEN_PORT,
			KVS_CONFIG_SOURCE_DERIVED, 1, "role split to replication.master_sync_port", 0);
	}
	if (cfg->replication_transport == KVS_REPLICATION_TCP && cfg->role == KVS_ROLE_MASTER &&
	    cfg->replication_sync_listen_port == 0) {
		derived_port = (long)cfg->port + 1000L;
		if (derived_port <= 0 || derived_port > 65535) {
			return config_error_set(error, cfg->config_path, 0,
				"derived replication.sync_listen_port is out of range");
		}
		cfg->replication_sync_listen_port = (int)derived_port;
		config_source_set(cfg, KVS_CFG_REPLICATION_SYNC_LISTEN_PORT,
			KVS_CONFIG_SOURCE_DERIVED, 0, "network.port+1000", 0);
	}
	if (cfg->replication_transport == KVS_REPLICATION_TCP && cfg->role == KVS_ROLE_SLAVE &&
	    cfg->replication_master_sync_port == 0 && cfg->replication_master_service_port > 0) {
		derived_port = (long)cfg->replication_master_service_port + 1000L;
		if (derived_port <= 0 || derived_port > 65535) {
			return config_error_set(error, cfg->config_path, 0,
				"derived replication.master_sync_port is out of range");
		}
		cfg->replication_master_sync_port = (int)derived_port;
		config_source_set(cfg, KVS_CFG_REPLICATION_MASTER_SYNC_PORT,
			KVS_CONFIG_SOURCE_DERIVED, 1, "replication.master_service_port+1000", 0);
	}
	if (config_parse_mirror_targets(cfg, error) != 0) {
		return -1;
	}
	cfg->embedding_prepare_queue_capacity = cfg->embedding_worker_queue_capacity;
	return 0;
}

static int config_validate_runtime_path(
	const char *path,
	const char *field_name,
	kvs_config_error_t *error,
	const char *config_path
) {
	struct stat st;
	char parent[KVS_CONFIG_PATH_MAX];

	if (!path || path[0] != '/') {
		return config_error_set(error, config_path, 0, "%s must resolve to an absolute path", field_name);
	}
	if (stat(path, &st) == 0) {
		if (!S_ISREG(st.st_mode)) {
			return config_error_set(error, config_path, 0, "%s target is not a regular file: %s", field_name, path);
		}
		if (access(path, R_OK | W_OK) != 0) {
			return config_error_set(error, config_path, 0, "%s is not readable and writable: %s", field_name, path);
		}
		return 0;
	}
	if (errno != ENOENT) {
		return config_error_set(error, config_path, 0, "cannot inspect %s '%s': %s",
			field_name, path, strerror(errno));
	}
	if (config_directory_name(path, parent, sizeof(parent)) != 0 ||
	    stat(parent, &st) != 0 || !S_ISDIR(st.st_mode) || access(parent, W_OK | X_OK) != 0) {
		return config_error_set(error, config_path, 0,
			"parent directory for %s is missing or not writable: %s", field_name, parent);
	}
	return 0;
}

int kvs_config_validate(const kvs_config_t *cfg, kvs_config_error_t *error) {
	if (!cfg) {
		return config_error_set(error, NULL, 0, "configuration object is null");
	}
	config_error_clear(error);
	if (cfg->config_root[0] != '/' || cfg->data_dir[0] != '/') {
		return config_error_set(error, cfg->config_path, 0, "configuration roots must be absolute after derivation");
	}
	{
		struct in_addr address;
		if (inet_pton(AF_INET, cfg->listen_address, &address) != 1) {
			return config_error_set(error, cfg->config_path, 0,
				"network.listen_address must be a valid IPv4 address");
		}
	}
	if (cfg->network_worker_threads != 1) {
		return config_error_set(error, cfg->config_path, 0,
			"network.worker_threads other than 1 is unsupported by the current single-executor model");
	}
	if (cfg->network_max_response_bytes > KVS_MAX_RESPONSE_LENGTH) {
		return config_error_set(error, cfg->config_path, 0,
			"network.max_response_bytes exceeds the current response builder capability");
	}
	{
		unsigned int default_bit = config_engine_bit(cfg->storage_default_engine);
		unsigned int compiled_mask = 0;
#if ENABLE_ARRAY
		compiled_mask |= KVS_STORAGE_ENGINE_ARRAY;
#endif
#if ENABLE_RBTREE
		compiled_mask |= KVS_STORAGE_ENGINE_RBTREE;
#endif
#if ENABLE_HASH
		compiled_mask |= KVS_STORAGE_ENGINE_HASH;
#endif
#if ENABLE_SKIPTABLE
		compiled_mask |= KVS_STORAGE_ENGINE_SKIPTABLE;
#endif
		if ((cfg->storage_enabled_engine_mask & ~compiled_mask) != 0) {
			return config_error_set(error, cfg->config_path, 0,
				"storage.enabled_engines requests an engine not compiled into this binary");
		}
		if (default_bit == 0 || !(cfg->storage_enabled_engine_mask & default_bit)) {
			return config_error_set(error, cfg->config_path, 0,
				"storage.default_engine must be present in storage.enabled_engines");
		}
	}
	if (!config_token_equal(cfg->startup_recovery_order, "rdb_then_aof_then_index")) {
		return config_error_set(error, cfg->config_path, 0,
			"unsupported persistence.startup_recovery_order");
	}
	if (strcmp(cfg->rdb_filename, cfg->aof_filename) == 0 ||
	    strcmp(cfg->rdb_filename, cfg->dump_filename) == 0 ||
	    strcmp(cfg->aof_filename, cfg->dump_filename) == 0) {
		return config_error_set(error, cfg->config_path, 0,
			"RDB, AOF and legacy dump paths must be distinct");
	}
	if (cfg->enable_rdb && config_validate_runtime_path(cfg->rdb_filename,
			"persistence.rdb_path", error, cfg->config_path) != 0) {
		return -1;
	}
	if (cfg->enable_aof && config_validate_runtime_path(cfg->aof_filename,
			"persistence.aof_path", error, cfg->config_path) != 0) {
		return -1;
	}
	if (cfg->replication_transport == KVS_REPLICATION_TCP) {
		if (cfg->role == KVS_ROLE_MASTER) {
			if (cfg->replication_sync_listen_port <= 0) {
				return config_error_set(error, cfg->config_path, 0,
					"TCP master requires replication.sync_listen_port");
			}
			if (cfg->replication_sync_listen_port == cfg->port) {
				return config_error_set(error, cfg->config_path, 0,
					"replication.sync_listen_port conflicts with network.port");
			}
		} else {
			struct in_addr master_address;
			if (cfg->replication_master_host[0] == '\0' ||
			    inet_pton(AF_INET, cfg->replication_master_host, &master_address) != 1 ||
			    cfg->replication_master_sync_port <= 0) {
				return config_error_set(error, cfg->config_path, 0,
					"TCP slave requires an IPv4 replication.master_host and replication.master_sync_port");
			}
		}
	}
	if (cfg->ebpf_mode != KVS_EBPF_DISABLED) {
		struct stat st;
		struct in_addr observe_address;
#if !KVS_ENABLE_EBPF
		return config_error_set(error, cfg->config_path, 0,
			"ebpf.mode requires a binary built with KVS_ENABLE_EBPF=1");
#endif
		if (stat(cfg->ebpf_object_path, &st) != 0 || !S_ISREG(st.st_mode) ||
		    access(cfg->ebpf_object_path, R_OK) != 0) {
			return config_error_set(error, cfg->config_path, 0,
				"ebpf.object_path must be a readable regular file: %s", cfg->ebpf_object_path);
		}
		if (inet_pton(AF_INET, cfg->ebpf_observe_host, &observe_address) != 1) {
			return config_error_set(error, cfg->config_path, 0,
				"ebpf.observe_host must be a valid IPv4 address");
		}
		if ((cfg->ebpf_ring_buffer_bytes & (cfg->ebpf_ring_buffer_bytes - 1)) != 0) {
			return config_error_set(error, cfg->config_path, 0,
				"ebpf.ring_buffer_bytes must be a power of two");
		}
		if (cfg->ebpf_mode == KVS_EBPF_OBSERVE && cfg->ebpf_mirror_target_count != 0) {
			return config_error_set(error, cfg->config_path, 0,
				"ebpf.mirror_targets is invalid in observe mode");
		}
		if (cfg->ebpf_mode == KVS_EBPF_EXPERIMENTAL_MIRROR) {
			if (cfg->role == KVS_ROLE_MASTER && cfg->ebpf_mirror_target_count == 0) {
				return config_error_set(error, cfg->config_path, 0,
					"experimental_mirror requires ebpf.mirror_targets");
			}
			if (cfg->replication_transport == KVS_REPLICATION_TCP) {
				return config_error_set(error, cfg->config_path, 0,
					"replication.transport=tcp conflicts with ebpf.mode=experimental_mirror");
			}
		}
	}
	if (cfg->vector_enabled) {
		struct stat st;
		if (!config_token_equal(cfg->vector_index_type, "core_flat") &&
		    !config_token_equal(cfg->vector_index_type, "core_hnsw_v2")) {
			return config_error_set(error, cfg->config_path, 0, "unsupported vector backend '%s'", cfg->vector_index_type);
		}
		if (cfg->embedding_dim != cfg->vector_dimension) {
			return config_error_set(error, cfg->config_path, 0,
				"embedding.output_dimension must equal vector.dimension");
		}
		if (!cfg->vector_normalize) {
			return config_error_set(error, cfg->config_path, 0,
				"vector.normalize=false is incompatible with the current semantic vector contract");
		}
		if (cfg->vector_hnsw_m < 48 || cfg->vector_hnsw_m0 < cfg->vector_hnsw_m ||
		    cfg->vector_hnsw_m0 > 65535 ||
		    cfg->vector_hnsw_ef_construction < cfg->vector_hnsw_m0 ||
		    cfg->vector_hnsw_ef_search <= 0) {
			return config_error_set(error, cfg->config_path, 0,
				"invalid HNSW contract: require M>=48, M0>=M, M0<=65535, efConstruction>=M0 and efSearch>0");
		}
		if (cfg->vector_hnsw_initial_capacity <= 0) {
			return config_error_set(error, cfg->config_path, 0,
				"vector.initial_capacity must be positive");
		}
		if (config_token_equal(cfg->embedding_provider, "local_c")) {
			if (cfg->embedding_model_path[0] == '\0' || cfg->embedding_vocab_path[0] == '\0') {
				return config_error_set(error, cfg->config_path, 0,
					"local_c requires embedding.model_path and embedding.vocab_path");
			}
			if (stat(cfg->embedding_model_path, &st) != 0 || !S_ISREG(st.st_mode) ||
			    access(cfg->embedding_model_path, R_OK) != 0) {
				return config_error_set(error, cfg->config_path, 0,
					"embedding.model_path must be a readable regular file: %s", cfg->embedding_model_path);
			}
			if (stat(cfg->embedding_vocab_path, &st) != 0 || !S_ISREG(st.st_mode) ||
			    access(cfg->embedding_vocab_path, R_OK) != 0) {
				return config_error_set(error, cfg->config_path, 0,
					"embedding.vocab_path must be a readable regular file: %s", cfg->embedding_vocab_path);
			}
		}
		if (cfg->vector_rebuild_delta_max_entries <
		    cfg->vector_rebuild_delta_initial_capacity) {
			return config_error_set(error, cfg->config_path, 0,
				"vector_rebuild.delta_max_entries must be >= delta_initial_capacity");
		}
		if (cfg->vector_snapshot_cross_node_reuse) {
			return config_error_set(error, cfg->config_path, 0,
				"vector_snapshot.cross_node_reuse=true is unsupported by the current node-local snapshot policy");
		}
		if (strcmp(cfg->vector_snapshot_manifest_path, cfg->vector_snapshot_index_path) == 0) {
			return config_error_set(error, cfg->config_path, 0,
				"vector snapshot manifest and index paths must be distinct");
		}
		if (cfg->vector_snapshot_enabled &&
		    config_token_equal(cfg->vector_index_type, "core_hnsw_v2")) {
			if (strlen(cfg->vector_snapshot_manifest_path) >= 512 ||
			    strlen(cfg->vector_snapshot_index_path) >= 512) {
				return config_error_set(error, cfg->config_path, 0,
					"vector snapshot path exceeds the current 511-byte runtime limit");
			}
			if (config_validate_runtime_path(cfg->vector_snapshot_manifest_path,
					"vector_snapshot.manifest_path", error, cfg->config_path) != 0 ||
			    config_validate_runtime_path(cfg->vector_snapshot_index_path,
					"vector_snapshot.index_path", error, cfg->config_path) != 0) {
				return -1;
			}
			if (strcmp(cfg->vector_snapshot_manifest_path, cfg->rdb_filename) == 0 ||
			    strcmp(cfg->vector_snapshot_manifest_path, cfg->aof_filename) == 0 ||
			    strcmp(cfg->vector_snapshot_manifest_path, cfg->dump_filename) == 0 ||
			    strcmp(cfg->vector_snapshot_index_path, cfg->rdb_filename) == 0 ||
			    strcmp(cfg->vector_snapshot_index_path, cfg->aof_filename) == 0 ||
			    strcmp(cfg->vector_snapshot_index_path, cfg->dump_filename) == 0) {
				return config_error_set(error, cfg->config_path, 0,
					"vector snapshot paths must not conflict with persistence paths");
			}
		}
	}
	if (cfg->embedding_worker_queue_capacity != cfg->embedding_prepare_queue_capacity) {
		return config_error_set(error, cfg->config_path, 0, "embedding queue aliases are inconsistent");
	}
	return 0;
}

int kvs_config_load(
	kvs_config_t *cfg,
	int argc,
	char *argv[],
	const char *default_path,
	FILE *warnings,
	kvs_config_error_t *error
) {
	kvs_config_bootstrap_t bootstrap;
	char *absolute_path = NULL;
	const char *selected_path;

	if (!cfg) {
		return config_error_set(error, NULL, 0, "configuration object is null");
	}
	if (kvs_config_prescan_cmdline(argc, argv, &bootstrap, error) != 0) {
		return -1;
	}
	kvs_config_init_default(cfg);
	cfg->no_config = bootstrap.no_config;
	if (!bootstrap.no_config) {
		selected_path = bootstrap.explicit_config ? bootstrap.config_path : default_path;
		if (!selected_path || selected_path[0] == '\0') {
			return config_error_set(error, NULL, 0, "no configuration path and --no-config was not specified");
		}
		absolute_path = realpath(selected_path, NULL);
		if (!absolute_path) {
			return config_error_set(error, selected_path, 0, "configuration file does not exist or is not accessible: %s",
				strerror(errno));
		}
		if (strlen(absolute_path) >= sizeof(cfg->config_path)) {
			free(absolute_path);
			return config_error_set(error, selected_path, 0, "absolute configuration path is too long");
		}
		snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", absolute_path);
		free(absolute_path);
		if (config_parse_file(cfg, cfg->config_path, warnings, error) != 0) {
			return -1;
		}
	}
	if (config_apply_cli(cfg, argc, argv, warnings, error) != 0) {
		return -1;
	}
	if (kvs_config_derive(cfg, error) != 0) {
		return -1;
	}
	return kvs_config_validate(cfg, error);
}

int kvs_config_load_file(kvs_config_t *cfg, const char *path) {
	char *argv[3];
	kvs_config_error_t error;

	argv[0] = (char *)"kvs_config_load_file";
	argv[1] = (char *)"--config";
	argv[2] = (char *)path;
	return kvs_config_load(cfg, 3, argv, NULL, stderr, &error);
}

size_t kvs_config_descriptor_count(void) {
	return config_descriptor_count;
}

const kvs_config_field_descriptor_t *kvs_config_descriptor_at(size_t index) {
	if (index >= config_descriptor_count) {
		return NULL;
	}
	return &config_descriptors[index].public_desc;
}

const kvs_config_field_descriptor_t *kvs_config_descriptor_find(const char *canonical_name) {
	size_t i;

	if (!canonical_name) {
		return NULL;
	}
	for (i = 0; i < config_descriptor_count; ++i) {
		if (config_token_equal(canonical_name, config_descriptors[i].public_desc.canonical_name)) {
			return &config_descriptors[i].public_desc;
		}
	}
	return NULL;
}

const kvs_config_source_t *kvs_config_source_get(
	const kvs_config_t *cfg,
	const char *canonical_name
) {
	const kvs_config_field_descriptor_t *desc = kvs_config_descriptor_find(canonical_name);

	if (!cfg || !desc || desc->id < 0 || desc->id >= KVS_CONFIG_FIELD_COUNT) {
		return NULL;
	}
	return &cfg->sources[desc->id];
}

int kvs_config_source_format(
	const kvs_config_source_t *source,
	char *buf,
	size_t size
) {
	const char *prefix;
	int written;

	if (!source || !buf || size == 0) {
		return -1;
	}
	switch (source->kind) {
		case KVS_CONFIG_SOURCE_DEFAULT: prefix = "default"; break;
		case KVS_CONFIG_SOURCE_FILE: prefix = "file"; break;
		case KVS_CONFIG_SOURCE_CLI: prefix = "cli"; break;
		case KVS_CONFIG_SOURCE_DERIVED: prefix = "derived"; break;
		default: prefix = "unknown"; break;
	}
	if (source->kind == KVS_CONFIG_SOURCE_FILE) {
		written = snprintf(buf, size, "%s%s:%s:%u",
			source->legacy_alias ? "legacy-alias:" : "",
			prefix,
			source->origin,
			source->line);
	} else if (source->kind == KVS_CONFIG_SOURCE_CLI) {
		written = snprintf(buf, size, "%s%s",
			source->legacy_alias ? "legacy-alias:" : "",
			prefix);
	} else if (source->kind == KVS_CONFIG_SOURCE_DERIVED) {
		written = snprintf(buf, size, "derived:%s", source->origin);
	} else {
		written = snprintf(buf, size, "%s", prefix);
	}
	return written >= 0 && (size_t)written < size ? 0 : -1;
}

static int config_print_value(
	const kvs_config_t *cfg,
	const kvs_config_descriptor_internal_t *desc,
	char *buf,
	size_t size
) {
	const void *target;
	int value;
	size_t i;

	if (!cfg || !desc || !buf || size == 0 || desc->offset == KVS_CONFIG_NO_OFFSET) {
		return -1;
	}
	target = (const char *)cfg + desc->offset;
	switch (desc->public_desc.type) {
		case KVS_CONFIG_TYPE_BOOL:
			return snprintf(buf, size, "%s", *(const int *)target ? "true" : "false") < (int)size ? 0 : -1;
		case KVS_CONFIG_TYPE_INT:
			return snprintf(buf, size, "%d", *(const int *)target) < (int)size ? 0 : -1;
		case KVS_CONFIG_TYPE_SIZE:
			return snprintf(buf, size, "%zu", *(const size_t *)target) < (int)size ? 0 : -1;
		case KVS_CONFIG_TYPE_FLOAT:
			return snprintf(buf, size, "%.9g", (double)*(const float *)target) < (int)size ? 0 : -1;
		case KVS_CONFIG_TYPE_STRING:
		case KVS_CONFIG_TYPE_ENUM_STRING:
			return snprintf(buf, size, "%s", (const char *)target) < (int)size ? 0 : -1;
		case KVS_CONFIG_TYPE_ENUM_INT:
			value = *(const int *)target;
			for (i = 0; i < desc->enum_count; ++i) {
				if (desc->enum_codes[i] == value) {
					const char *start = desc->public_desc.enum_values;
					const char *end;
					size_t length;
					size_t j;
					for (j = 0; j < i; ++j) {
						start = strchr(start, '|') + 1;
					}
					end = strchr(start, '|');
					length = end ? (size_t)(end - start) : strlen(start);
					return snprintf(buf, size, "%.*s", (int)length, start) < (int)size ? 0 : -1;
				}
			}
			return snprintf(buf, size, "%d", value) < (int)size ? 0 : -1;
		default:
			return -1;
	}
}

static kvs_config_field_status_t config_effective_status(
	const kvs_config_t *cfg,
	const kvs_config_descriptor_internal_t *desc
) {
	kvs_config_field_id_t id = desc->public_desc.id;

	if (!cfg) return desc->public_desc.status;
	if (!cfg->enable_rdb && id == KVS_CFG_PERSISTENCE_RDB_PATH) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if (!cfg->enable_aof &&
	    (id == KVS_CFG_PERSISTENCE_AOF_FSYNC ||
	     id == KVS_CFG_PERSISTENCE_AOF_PATH ||
	     id == KVS_CFG_PERSISTENCE_AOF_REWRITE_ENABLED ||
	     id == KVS_CFG_PERSISTENCE_AOF_REWRITE_MIN_BYTES ||
	     id == KVS_CFG_PERSISTENCE_AOF_REWRITE_MIN_GROWTH_BYTES ||
	     id == KVS_CFG_PERSISTENCE_AOF_REWRITE_GROWTH_PERCENT)) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if ((!cfg->ttl_enabled || !cfg->ttl_scan_enabled) &&
	    (id == KVS_CFG_TTL_SCAN_INTERVAL_MS || id == KVS_CFG_TTL_SCAN_BUDGET)) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if (cfg->replication_transport != KVS_REPLICATION_TCP &&
	    (id == KVS_CFG_REPLICATION_MASTER_HOST ||
	     id == KVS_CFG_REPLICATION_MASTER_SERVICE_PORT ||
	     id == KVS_CFG_REPLICATION_MASTER_SYNC_PORT ||
	     id == KVS_CFG_REPLICATION_SYNC_LISTEN_PORT ||
	     id == KVS_CFG_REPLICATION_FULL_SYNC_POLICY ||
	     id == KVS_CFG_REPLICATION_NOT_READY_QUERY_POLICY)) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if (cfg->replication_transport == KVS_REPLICATION_TCP && cfg->role == KVS_ROLE_MASTER &&
	    (id == KVS_CFG_REPLICATION_MASTER_HOST ||
	     id == KVS_CFG_REPLICATION_MASTER_SERVICE_PORT ||
	     id == KVS_CFG_REPLICATION_MASTER_SYNC_PORT)) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if (cfg->replication_transport == KVS_REPLICATION_TCP && cfg->role == KVS_ROLE_SLAVE &&
	    id == KVS_CFG_REPLICATION_SYNC_LISTEN_PORT) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if (cfg->ebpf_mode == KVS_EBPF_DISABLED &&
	    id >= KVS_CFG_EBPF_OBJECT_PATH && id <= KVS_CFG_EBPF_FLUSH_INTERVAL_US) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if (cfg->ebpf_mode != KVS_EBPF_EXPERIMENTAL_MIRROR &&
	    id == KVS_CFG_EBPF_MIRROR_TARGETS) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if (!cfg->vector_enabled &&
	    id >= KVS_CFG_EMBEDDING_PROVIDER &&
	    id <= KVS_CFG_OBSERVABILITY_HNSW_SEARCH_PROFILE_ENABLED) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if (!cfg->embedding_worker_pool_enabled &&
	    (id == KVS_CFG_EMBEDDING_WORKER_THREADS ||
	     id == KVS_CFG_EMBEDDING_WORKER_QUEUE_CAPACITY)) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if (!cfg->embedding_cache_enabled &&
	    (id == KVS_CFG_EMBEDDING_CACHE_CAPACITY ||
	     id == KVS_CFG_EMBEDDING_CACHE_TTL_MS ||
	     id == KVS_CFG_EMBEDDING_CACHE_POLICY)) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if ((!cfg->vector_snapshot_enabled ||
	     !config_token_equal(cfg->vector_index_type, "core_hnsw_v2")) &&
	    (id == KVS_CFG_VECTOR_SNAPSHOT_MANIFEST_PATH ||
	     id == KVS_CFG_VECTOR_SNAPSHOT_INDEX_PATH ||
	     id == KVS_CFG_VECTOR_SNAPSHOT_LOAD_ON_START ||
	     id == KVS_CFG_VECTOR_SNAPSHOT_INVALID_ACTION ||
	     id == KVS_CFG_VECTOR_SNAPSHOT_WRITE_AFTER_REBUILD ||
	     id == KVS_CFG_VECTOR_SNAPSHOT_CROSS_NODE_REUSE)) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	if (!cfg->vector_rebuild_async_enabled &&
	    (id == KVS_CFG_VECTOR_REBUILD_DELTA_INITIAL_CAPACITY ||
	     id == KVS_CFG_VECTOR_REBUILD_DELTA_MAX_ENTRIES ||
	     id == KVS_CFG_VECTOR_REBUILD_RETRY_POLICY ||
	     id == KVS_CFG_VECTOR_REBUILD_CANCEL_ENABLED ||
	     id == KVS_CFG_VECTOR_REBUILD_WRITE_SNAPSHOT_AFTER_PUBLISH)) {
		return KVS_CONFIG_STATUS_INACTIVE;
	}
	return desc->public_desc.status;
}

int kvs_config_print_effective(const kvs_config_t *cfg, FILE *out) {
	size_t i;
	unsigned char printed[KVS_CONFIG_FIELD_COUNT];

	if (!cfg || !out) {
		return -1;
	}
	memset(printed, 0, sizeof(printed));
	for (i = 0; i < config_descriptor_count; ++i) {
		const kvs_config_descriptor_internal_t *desc = &config_descriptors[i];
		char value[1024];
		char source[1200];
		if (desc->public_desc.id < 0 || desc->public_desc.id >= KVS_CONFIG_FIELD_COUNT ||
		    printed[desc->public_desc.id] || desc->offset == KVS_CONFIG_NO_OFFSET) {
			continue;
		}
		printed[desc->public_desc.id] = 1;
		if (config_print_value(cfg, desc, value, sizeof(value)) != 0 ||
		    kvs_config_source_format(&cfg->sources[desc->public_desc.id], source, sizeof(source)) != 0) {
			return -1;
		}
		fprintf(out, "%s=%s source=%s status=%s\n",
			desc->public_desc.canonical_name,
			value,
			source,
			config_status_name(config_effective_status(cfg, desc)));
	}
	return ferror(out) ? -1 : 0;
}

int kvs_config_resolve_path(
	const kvs_config_t *cfg,
	kvs_config_path_kind_t kind,
	const char *path,
	char *out,
	size_t out_size
) {
	const char *base;

	if (!cfg || !path || path[0] == '\0' || !out || out_size == 0) {
		return -1;
	}
	if (path[0] == '/') {
		return config_normalize_absolute(path, out, out_size);
	}
	base = kind == KVS_CONFIG_PATH_DATA ? cfg->data_dir : cfg->config_root;
	return config_resolve_against(base, path, out, out_size);
}
