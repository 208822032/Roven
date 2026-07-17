#ifndef KVS_CONFIG_H
#define KVS_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "kvs_log.h"

#define KVS_MAX_TOKENS              128
#define KVS_MAX_RESPONSE_SIZE       (1024 * 1024)
#define KVS_MAX_LINE                1000
#define KVS_MAX_RESPONSE_LENGTH     (2 * 1024 * 1024)
#define KVS_CONFIG_PATH_MAX         1024
#define KVS_CONFIG_ERROR_MAX        512
#define KVS_CONFIG_MAX_MIRROR_TARGETS 10

#define KVS_STORAGE_ENGINE_ARRAY     (1u << 0)
#define KVS_STORAGE_ENGINE_RBTREE    (1u << 1)
#define KVS_STORAGE_ENGINE_HASH      (1u << 2)
#define KVS_STORAGE_ENGINE_SKIPTABLE (1u << 3)

#define NETWORK_REACTOR         0
#define NETWORK_PROACTOR        1
#define NETWORK_NTYCO           2
#define NETWORK_SELECT          NETWORK_REACTOR

#define ENABLE_ARRAY            1
#define ENABLE_RBTREE           1
#define ENABLE_HASH             1
#define ENABLE_SKIPTABLE        1
#define ENABLE_RDB              1
#define ENABLE_AOF              1

#ifndef KVS_RANGE_VISITED_DEBUG
#define KVS_RANGE_VISITED_DEBUG 0
#endif

typedef enum {
	KVS_ROLE_MASTER = 0,
	KVS_ROLE_SLAVE  = 1
} kvs_role_t;

typedef enum {
	KVS_AOF_FSYNC_NO = 0,
	KVS_AOF_FSYNC_EVERYSEC = 1,
	KVS_AOF_FSYNC_ALWAYS = 2
} kvs_aof_fsync_mode_t;

typedef enum {
	KVS_MEM_BACKEND_POOL = 0,
	KVS_MEM_BACKEND_SYSTEM = 1,
	KVS_MEM_BACKEND_JEMALLOC = 2
} kvs_mem_backend_t;

typedef enum {
	KVS_REPLICATION_DISABLED = 0,
	KVS_REPLICATION_TCP = 1
} kvs_replication_transport_t;

typedef enum {
	KVS_EBPF_DISABLED = 0,
	KVS_EBPF_OBSERVE = 1,
	KVS_EBPF_EXPERIMENTAL_MIRROR = 2
} kvs_ebpf_mode_t;

typedef enum {
	KVS_CONFIG_TYPE_BOOL = 0,
	KVS_CONFIG_TYPE_INT,
	KVS_CONFIG_TYPE_SIZE,
	KVS_CONFIG_TYPE_FLOAT,
	KVS_CONFIG_TYPE_STRING,
	KVS_CONFIG_TYPE_ENUM_INT,
	KVS_CONFIG_TYPE_ENUM_STRING
} kvs_config_value_type_t;

typedef enum {
	KVS_CONFIG_STATUS_IMPLEMENTED = 0,
	KVS_CONFIG_STATUS_INACTIVE,
	KVS_CONFIG_STATUS_UNSUPPORTED,
	KVS_CONFIG_STATUS_FUTURE
} kvs_config_field_status_t;

typedef enum {
	KVS_CONFIG_SOURCE_DEFAULT = 0,
	KVS_CONFIG_SOURCE_FILE,
	KVS_CONFIG_SOURCE_CLI,
	KVS_CONFIG_SOURCE_DERIVED
} kvs_config_source_kind_t;

typedef enum {
	KVS_CONFIG_PATH_CONFIG = 0,
	KVS_CONFIG_PATH_DATA,
	KVS_CONFIG_PATH_ASSET
} kvs_config_path_kind_t;

typedef enum {
	KVS_CFG_SERVER_ROLE = 0,
	KVS_CFG_SERVER_CONFIG_ROOT,
	KVS_CFG_SERVER_DATA_DIR,
	KVS_CFG_SERVER_TEST_MODE,
	KVS_CFG_NETWORK_PORT,
	KVS_CFG_NETWORK_MODE,
	KVS_CFG_NETWORK_LISTEN_ADDRESS,
	KVS_CFG_NETWORK_WORKER_THREADS,
	KVS_CFG_NETWORK_MAX_CONNECTIONS,
	KVS_CFG_NETWORK_LISTEN_BACKLOG,
	KVS_CFG_NETWORK_MAX_REQUEST_BYTES,
	KVS_CFG_NETWORK_MAX_RESPONSE_BYTES,
	KVS_CFG_NETWORK_MAX_COMMANDS_PER_READ,
	KVS_CFG_REPLICATION_MASTER_HOST,
	KVS_CFG_REPLICATION_MASTER_SERVICE_PORT,
	KVS_CFG_REPLICATION_MASTER_SYNC_PORT,
	KVS_CFG_REPLICATION_SYNC_LISTEN_PORT,
	KVS_CFG_REPLICATION_TRANSPORT,
	KVS_CFG_REPLICATION_FULL_SYNC_POLICY,
	KVS_CFG_REPLICATION_NOT_READY_QUERY_POLICY,
	KVS_CFG_PERSISTENCE_AOF_ENABLED,
	KVS_CFG_PERSISTENCE_AOF_FSYNC,
	KVS_CFG_PERSISTENCE_AOF_PATH,
	KVS_CFG_PERSISTENCE_RDB_PATH,
	KVS_CFG_PERSISTENCE_LEGACY_DUMP_PATH,
	KVS_CFG_PERSISTENCE_RDB_ENABLED,
	KVS_CFG_PERSISTENCE_AOF_REWRITE_ENABLED,
	KVS_CFG_PERSISTENCE_AOF_REWRITE_MIN_BYTES,
	KVS_CFG_PERSISTENCE_AOF_REWRITE_MIN_GROWTH_BYTES,
	KVS_CFG_PERSISTENCE_AOF_REWRITE_GROWTH_PERCENT,
	KVS_CFG_PERSISTENCE_STARTUP_RECOVERY_ORDER,
	KVS_CFG_STORAGE_MEMORY_BACKEND,
	KVS_CFG_STORAGE_MAX_VALUE_BYTES,
	KVS_CFG_STORAGE_DEFAULT_ENGINE,
	KVS_CFG_STORAGE_ENABLED_ENGINES,
	KVS_CFG_STORAGE_ARRAY_INITIAL_CAPACITY,
	KVS_CFG_STORAGE_HASH_INITIAL_CAPACITY,
	KVS_CFG_TTL_ENABLED,
	KVS_CFG_TTL_SCAN_ENABLED,
	KVS_CFG_TTL_SCAN_INTERVAL_MS,
	KVS_CFG_TTL_SCAN_BUDGET,
	KVS_CFG_OBSERVABILITY_LOG_LEVEL,
	KVS_CFG_OBSERVABILITY_LOG_ASYNC,
	KVS_CFG_OBSERVABILITY_LOG_PATH,
	KVS_CFG_EBPF_LEGACY_CONF,
	KVS_CFG_EBPF_MODE,
	KVS_CFG_EBPF_OBJECT_PATH,
	KVS_CFG_EBPF_OBSERVE_HOST,
	KVS_CFG_EBPF_OBSERVE_PORT,
	KVS_CFG_EBPF_MIRROR_TARGETS,
	KVS_CFG_EBPF_RING_BUFFER_BYTES,
	KVS_CFG_EBPF_MAX_EVENT_BYTES,
	KVS_CFG_EBPF_USER_QUEUE_CAPACITY,
	KVS_CFG_EBPF_FLUSH_INTERVAL_US,
	KVS_CFG_EMBEDDING_PROVIDER,
	KVS_CFG_EMBEDDING_MODEL_PATH,
	KVS_CFG_EMBEDDING_VOCAB_PATH,
	KVS_CFG_EMBEDDING_OUTPUT_DIMENSION,
	KVS_CFG_EMBEDDING_MODEL_VERSION,
	KVS_CFG_EMBEDDING_INTRA_THREADS,
	KVS_CFG_EMBEDDING_MAX_TEXT_LEN,
	KVS_CFG_EMBEDDING_MAX_TOKENS,
	KVS_CFG_EMBEDDING_ORT_GRAPH_OPT,
	KVS_CFG_EMBEDDING_INTER_THREADS,
	KVS_CFG_EMBEDDING_ORT_EXECUTION_MODE,
	KVS_CFG_EMBEDDING_WORKER_POOL_ENABLED,
	KVS_CFG_EMBEDDING_WORKER_THREADS,
	KVS_CFG_EMBEDDING_WORKER_QUEUE_CAPACITY,
	KVS_CFG_EMBEDDING_PER_THREAD_RUNTIME_ENABLED,
	KVS_CFG_EMBEDDING_PER_THREAD_FALLBACK_MUTEX,
	KVS_CFG_EMBEDDING_SHARED_ORT_ENV_ENABLED,
	KVS_CFG_EMBEDDING_BATCH_ENABLED,
	KVS_CFG_EMBEDDING_BATCH_MAX_SIZE,
	KVS_CFG_EMBEDDING_BATCH_MAX_WAIT_US,
	KVS_CFG_EMBEDDING_BATCH_VSET_ENABLED,
	KVS_CFG_EMBEDDING_BATCH_VSEARCH_ENABLED,
	KVS_CFG_EMBEDDING_BATCH_VSET_MAX_SIZE,
	KVS_CFG_EMBEDDING_BATCH_VSET_MAX_WAIT_US,
	KVS_CFG_EMBEDDING_BATCH_VSEARCH_MAX_SIZE,
	KVS_CFG_EMBEDDING_BATCH_VSEARCH_MAX_WAIT_US,
	KVS_CFG_EMBEDDING_CACHE_ENABLED,
	KVS_CFG_EMBEDDING_CACHE_CAPACITY,
	KVS_CFG_EMBEDDING_CACHE_TTL_MS,
	KVS_CFG_EMBEDDING_CACHE_POLICY,
	KVS_CFG_VECTOR_ENABLED,
	KVS_CFG_VECTOR_INDEX_TYPE,
	KVS_CFG_VECTOR_LEGACY_V2_EXPERIMENTAL,
	KVS_CFG_VECTOR_DIMENSION,
	KVS_CFG_VECTOR_METRIC,
	KVS_CFG_VECTOR_NORMALIZE,
	KVS_CFG_VECTOR_HNSW_M,
	KVS_CFG_VECTOR_HNSW_M0,
	KVS_CFG_VECTOR_HNSW_EF_CONSTRUCTION,
	KVS_CFG_VECTOR_HNSW_EF_SEARCH,
	KVS_CFG_VECTOR_INITIAL_CAPACITY,
	KVS_CFG_VECTOR_DEFAULT_TOPK,
	KVS_CFG_VECTOR_DEFAULT_THRESHOLD,
	KVS_CFG_VECTOR_SCOPE_OVERFETCH_FACTOR,
	KVS_CFG_VECTOR_DISTANCE_BACKEND,
	KVS_CFG_VECTOR_SNAPSHOT_ENABLED,
	KVS_CFG_VECTOR_SNAPSHOT_MANIFEST_PATH,
	KVS_CFG_VECTOR_SNAPSHOT_INDEX_PATH,
	KVS_CFG_VECTOR_SNAPSHOT_LOAD_ON_START,
	KVS_CFG_VECTOR_SNAPSHOT_INVALID_ACTION,
	KVS_CFG_VECTOR_SNAPSHOT_WRITE_AFTER_REBUILD,
	KVS_CFG_VECTOR_SNAPSHOT_CROSS_NODE_REUSE,
	KVS_CFG_VECTOR_REBUILD_ASYNC_ENABLED,
	KVS_CFG_VECTOR_REBUILD_DELTA_INITIAL_CAPACITY,
	KVS_CFG_VECTOR_REBUILD_DELTA_MAX_ENTRIES,
	KVS_CFG_VECTOR_REBUILD_RETRY_POLICY,
	KVS_CFG_VECTOR_REBUILD_CANCEL_ENABLED,
	KVS_CFG_VECTOR_REBUILD_WRITE_SNAPSHOT_AFTER_PUBLISH,
	KVS_CFG_OBSERVABILITY_VSEARCH_STAGE_STATS_ENABLED,
	KVS_CFG_OBSERVABILITY_EMBEDDING_PROFILE_ENABLED,
	KVS_CFG_OBSERVABILITY_HNSW_BUILD_PROFILE_ENABLED,
	KVS_CFG_OBSERVABILITY_HNSW_SEARCH_PROFILE_ENABLED,
	KVS_CONFIG_FIELD_COUNT
} kvs_config_field_id_t;

typedef struct kvs_config_source {
	kvs_config_source_kind_t kind;
	int legacy_alias;
	unsigned int line;
	char origin[KVS_CONFIG_PATH_MAX];
} kvs_config_source_t;

typedef struct kvs_config_field_descriptor {
	kvs_config_field_id_t id;
	const char *section;
	const char *name;
	const char *canonical_name;
	const char *legacy_names;
	kvs_config_value_type_t type;
	const char *default_value;
	const char *constraints;
	const char *enum_values;
	int cli_override;
	kvs_config_field_status_t status;
} kvs_config_field_descriptor_t;

typedef struct kvs_config_error {
	char message[KVS_CONFIG_ERROR_MAX];
	char path[KVS_CONFIG_PATH_MAX];
	unsigned int line;
} kvs_config_error_t;

typedef struct kvs_config_bootstrap {
	int no_config;
	int explicit_config;
	char config_path[KVS_CONFIG_PATH_MAX];
} kvs_config_bootstrap_t;

typedef struct kvs_config_endpoint {
	char host[64];
	int port;
} kvs_config_endpoint_t;

typedef struct kvs_config {
	int port;
	kvs_role_t role;
	int test_mode;

	char listen_address[64];
	int network_worker_threads;
	int network_max_connections;
	int network_listen_backlog;
	size_t network_max_request_bytes;
	size_t network_max_response_bytes;
	int network_max_commands_per_read;

	int enable_rdb;
	int enable_aof;
	kvs_aof_fsync_mode_t aof_fsync_mode;
	char aof_filename[256];
	char rdb_filename[256];
	char dump_filename[256];
	int aof_rewrite_enabled;
	size_t aof_rewrite_min_bytes;
	size_t aof_rewrite_min_growth_bytes;
	int aof_rewrite_growth_percent;
	char startup_recovery_order[32];

	int network_mode;
	kvs_mem_backend_t mem_backend;
	size_t max_value_size;
	char storage_default_engine[16];
	char storage_enabled_engines[64];
	unsigned int storage_enabled_engine_mask;
	int storage_array_initial_capacity;
	int storage_hash_initial_capacity;
	int ttl_enabled;
	int ttl_scan_enabled;
	int ttl_scan_interval_ms;
	int ttl_scan_budget;

	kvs_replication_transport_t replication_transport;
	char replication_master_host[64];
	int replication_master_service_port;
	int replication_master_sync_port;
	int replication_sync_listen_port;
	char replication_full_sync_policy[16];
	char replication_not_ready_query_policy[16];

	char log_level[16];
	int log_async;
	char log_file[256];
	char ebpf_conf[256];
	kvs_ebpf_mode_t ebpf_mode;
	char ebpf_object_path[KVS_CONFIG_PATH_MAX];
	char ebpf_observe_host[64];
	int ebpf_observe_port;
	char ebpf_mirror_targets[1024];
	kvs_config_endpoint_t ebpf_mirror_endpoints[KVS_CONFIG_MAX_MIRROR_TARGETS];
	size_t ebpf_mirror_target_count;
	size_t ebpf_ring_buffer_bytes;
	size_t ebpf_max_event_bytes;
	int ebpf_user_queue_capacity;
	int ebpf_flush_interval_us;

	char embedding_provider[16];
	char embedding_model_path[KVS_CONFIG_PATH_MAX];
	char embedding_vocab_path[KVS_CONFIG_PATH_MAX];
	int embedding_dim;
	char embedding_model_version[128];
	uint64_t embedding_model_identity;
	uint32_t embedding_legacy_model_identity;
	int embedding_num_threads;
	int embedding_max_text_len;
	int embedding_max_tokens;
	char embedding_ort_graph_opt_level[16];
	int embedding_inter_threads;
	char embedding_ort_execution_mode[16];
	int embedding_worker_pool_enabled;
	int embedding_worker_threads;
	int embedding_worker_queue_capacity;
	int embedding_prepare_queue_capacity;
	int embedding_per_thread_runtime_enabled;
	int embedding_per_thread_runtime_fallback_mutex;
	int embedding_shared_ort_env_enabled;
	int embedding_batch_enabled;
	int embedding_batch_max_size;
	int embedding_batch_max_wait_us;
	int embedding_batch_vset_enabled;
	int embedding_batch_vsearch_enabled;
	int embedding_batch_vset_max_size;
	int embedding_batch_vset_max_wait_us;
	int embedding_batch_vsearch_max_size;
	int embedding_batch_vsearch_max_wait_us;
	int embedding_cache_enabled;
	int embedding_cache_capacity;
	int embedding_cache_ttl_ms;
	char embedding_cache_policy[16];

	int vector_enabled;
	char vector_index_type[16];
	int vector_hnsw_v2_experimental_enabled;
	int vector_dimension;
	char vector_metric[16];
	int vector_normalize;
	int vector_hnsw_m;
	int vector_hnsw_m0;
	int vector_hnsw_ef_construction;
	int vector_hnsw_ef_search;
	int vector_hnsw_initial_capacity;
	int vector_default_topk;
	float vector_score_threshold;
	int vector_scope_overfetch_factor;
	char vector_distance_backend[16];

	int vector_snapshot_enabled;
	char vector_snapshot_manifest_path[KVS_CONFIG_PATH_MAX];
	char vector_snapshot_index_path[KVS_CONFIG_PATH_MAX];
	int vector_snapshot_load_on_start;
	char vector_snapshot_invalid_action[16];
	char vector_snapshot_write_after_rebuild[16];
	int vector_snapshot_cross_node_reuse;

	int vector_rebuild_async_enabled;
	int vector_rebuild_delta_initial_capacity;
	int vector_rebuild_delta_max_entries;
	char vector_rebuild_retry_policy[16];
	int vector_rebuild_cancel_enabled;
	int vector_rebuild_write_snapshot_after_publish;

	int observability_vsearch_stage_stats_enabled;
	int observability_embedding_profile_enabled;
	int observability_hnsw_build_profile_enabled;
	int observability_hnsw_search_profile_enabled;

	char config_path[KVS_CONFIG_PATH_MAX];
	char config_root[KVS_CONFIG_PATH_MAX];
	char data_dir[KVS_CONFIG_PATH_MAX];
	int no_config;
	kvs_config_source_t sources[KVS_CONFIG_FIELD_COUNT];
} kvs_config_t;

void kvs_config_init_default(kvs_config_t *cfg);

int kvs_config_prescan_cmdline(
	int argc,
	char *argv[],
	kvs_config_bootstrap_t *bootstrap,
	kvs_config_error_t *error
);

int kvs_config_load(
	kvs_config_t *cfg,
	int argc,
	char *argv[],
	const char *default_path,
	FILE *warnings,
	kvs_config_error_t *error
);

int kvs_config_load_file(kvs_config_t *cfg, const char *path);
int kvs_config_derive(kvs_config_t *cfg, kvs_config_error_t *error);
int kvs_config_validate(const kvs_config_t *cfg, kvs_config_error_t *error);

size_t kvs_config_descriptor_count(void);
const kvs_config_field_descriptor_t *kvs_config_descriptor_at(size_t index);
const kvs_config_field_descriptor_t *kvs_config_descriptor_find(const char *canonical_name);
const kvs_config_source_t *kvs_config_source_get(
	const kvs_config_t *cfg,
	const char *canonical_name
);
int kvs_config_source_format(
	const kvs_config_source_t *source,
	char *buf,
	size_t size
);
int kvs_config_print_effective(const kvs_config_t *cfg, FILE *out);

int kvs_config_resolve_path(
	const kvs_config_t *cfg,
	kvs_config_path_kind_t kind,
	const char *path,
	char *out,
	size_t out_size
);

#endif
