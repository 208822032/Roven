#define KVS_VECTOR_INTERNAL
#include "kvstore.h"
#include "distributed.h"
#include "resp_protocol.h"
#include "kvs_memory.h"
#include "sync_mode.h"
#include "kvs_config.h"
#include "kvs_globals.h"
#include "jemalloc_wrapper.h"
#include "kvs_memory_internal.h"
#include "kvs_log.h"
#include "kvs_vector.h"
#include "kvs_command_task.h"
#include "kvs_command_executor.h"
#include "kvs_owner_lock.h"
#include "kvs_response_task.h"
#include "kvs_server.h"

#include <strings.h>

pthread_rwlock_t g_kv_data_rwlock = PTHREAD_RWLOCK_INITIALIZER;

static int g_shutdown_flag = 0;

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

extern distributed_config_t global_dist_config;

static __thread int sync_context = 0;//每个线程都有的局部变量

//0 正常命令执行上下文 1 主从同步命令上下文 2 从节点应用主节点命令上下文（跳过从节点只读检查）
void set_sync_context(int context) {
	sync_context = context;
}
int get_sync_context() {
	return sync_context;
}

int kvs_shutdown_requested(void) {
	return __atomic_load_n(&g_shutdown_flag, __ATOMIC_ACQUIRE);
}

void kvs_shutdown_request(void) {
	__atomic_store_n(&g_shutdown_flag, 1, __ATOMIC_RELEASE);
	ntyco_request_shutdown();
}

void kvs_shutdown_reset_for_init(void) {
	__atomic_store_n(&g_shutdown_flag, 0, __ATOMIC_RELEASE);
}


int kvs_filter_protocol(char **tokens, int count, size_t *token_lens, char *response){
	kvs_response_task_t response_task;
	kvs_command_task_t task;
	int ret;
	int prep_ret;

	kvs_response_task_init_borrowed(&response_task, response, KVS_MAX_RESPONSE_LENGTH);

	if (kvs_command_task_build(&task, tokens, count, token_lens, &response_task) < 0) {
		ret = task.status;
		kvs_response_task_finalize(&response_task, ret);
		kvs_command_task_destroy(&task);
		kvs_response_task_destroy(&response_task);
		return ret;
	}

	prep_ret = kvs_command_task_prepare(&task);
	if (prep_ret != 0) {
		ret = prep_ret;
		kvs_response_task_finalize(&response_task, ret);
		kvs_command_task_destroy(&task);
		kvs_response_task_destroy(&response_task);
		return ret;
	}

	ret = kvs_command_task_execute_sync(&task);
	kvs_command_task_post_commit(&task, ret);
	kvs_response_task_finalize(&response_task, ret);
	ret = kvs_response_task_status(&response_task);
	kvs_command_task_destroy(&task);
	kvs_response_task_destroy(&response_task);
	return ret;
}


int init_kvengine(void) {

	//  mem_init();  // 初始化内存管理组件（内部会根据配置选择后端）

#if ENABLE_ARRAY
	memset(&global_array, 0, sizeof(kvs_array_t));
	if (kvs_array_create_with_capacity(&global_array, g_cfg.storage_array_initial_capacity) != 0) {
		return -1;
	}
#endif

#if ENABLE_RBTREE
	memset(&global_rbtree, 0, sizeof(kvs_rbtree_t));
	if (kvs_rbtree_create(&global_rbtree) != 0) {
		return -1;
	}
#endif

#if ENABLE_HASH
	memset(&global_hash, 0, sizeof(kvs_hash_t));
	if (kvs_hash_create_with_capacity(&global_hash, g_cfg.storage_hash_initial_capacity) != 0) {
		return -1;
	}
#endif

#if ENABLE_SKIPTABLE
	memset(&global_skiptable, 0, sizeof(kvs_skiptable_t));
	if (kvs_skiptable_create(&global_skiptable) != 0) {
		return -1;
	}
#endif

	if (g_cfg.vector_enabled && kvs_vector_init() != KVS_VECTOR_OK) return -1;

	return 0;
}

void dest_kvengine(void) {

	// 1. 先停止所有后台线程/使用者，确保不再访问全局数据结构
	if (g_cfg.replication_transport == KVS_REPLICATION_TCP) {
		distributed_shutdown();
	}

#if ENABLE_AOF
	aof_free();
#endif

	// 2. 加写锁，确保销毁期间无命令线程并发访问数据结构
	// Stage 13: route through owner rwlock wrapper
	kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "shutdown.destroy.write");

#if ENABLE_ARRAY
	kvs_array_destory(&global_array);
#endif
#if ENABLE_RBTREE
	kvs_rbtree_destory(&global_rbtree);
#endif
#if ENABLE_HASH
	kvs_hash_destory(&global_hash);
#endif
#if ENABLE_SKIPTABLE
	kvs_skiptable_destroy(&global_skiptable);
#endif

	kvs_vector_destroy();

	kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "shutdown.destroy.write");

	// 3. 最后销毁内存池（确保 distributed_shutdown / aof_free 中的 kvs_free 先执行）
	mem_destroy();

}



extern kvs_config_t g_cfg;

static kvs_log_level_t kvs_parse_log_level_value(const char *value, int *valid) {
	if (valid) *valid = 1;
	if (!value || value[0] == '\0') {
		return KVS_LOG_LEVEL_INFO;
	}
	if (strcasecmp(value, "error") == 0) return KVS_LOG_LEVEL_ERROR;
	if (strcasecmp(value, "warn") == 0 || strcasecmp(value, "warning") == 0) return KVS_LOG_LEVEL_WARN;
	if (strcasecmp(value, "info") == 0) return KVS_LOG_LEVEL_INFO;
	if (strcasecmp(value, "debug") == 0) return KVS_LOG_LEVEL_DEBUG;
	if (valid) *valid = 0;
	return KVS_LOG_LEVEL_INFO;
}

// 信号处理函数
void handle_signal(int sig) {
	(void)sig;
	kvs_shutdown_request();
}

#if 1
int main(int argc, char *argv[]) {
	kvs_config_error_t config_error;
	int network_status = 0;

	kvs_shutdown_reset_for_init();

	if (kvs_config_load(
			&g_cfg,
			argc,
			argv,
			"config/kvstore.conf",
			stderr,
			&config_error) != 0) {
		if (config_error.path[0] && config_error.line) {
			fprintf(stderr, "configuration error: %s:%u: %s\n",
				config_error.path, config_error.line, config_error.message);
		} else if (config_error.path[0]) {
			fprintf(stderr, "configuration error: %s: %s\n",
				config_error.path, config_error.message);
		} else {
			fprintf(stderr, "configuration error: %s\n", config_error.message);
		}
		return 1;
	}

	int log_level_valid = 1;
	kvs_log_level_t log_level = kvs_parse_log_level_value(g_cfg.log_level, &log_level_valid);
	const char *log_path = (g_cfg.log_file[0] != '\0') ? g_cfg.log_file : NULL;
	if (kvs_log_init(log_path, log_level, g_cfg.log_async) != 0 && log_path) {
		KVS_LOG_ERROR("core", "failed to open log file %s, falling back to stderr", log_path);
	}
	if (!log_level_valid) {
		KVS_LOG_ERROR("core", "invalid log_level=%s, using info", g_cfg.log_level);
	}
	KVS_LOG_INFO("core", "logging initialized: mode=%s level=%s output=%s",
		kvs_log_is_async() ? "async" : "sync",
		kvs_log_level_name(log_level),
		log_path ? log_path : "stderr");
	{
		char rdb_source[KVS_CONFIG_PATH_MAX + 64] = "unknown";
		char aof_source[KVS_CONFIG_PATH_MAX + 64] = "unknown";
		char index_source[KVS_CONFIG_PATH_MAX + 64] = "unknown";
		char model_source[KVS_CONFIG_PATH_MAX + 64] = "unknown";
		char snapshot_source[KVS_CONFIG_PATH_MAX + 64] = "unknown";
		const kvs_config_source_t *source;
		source = kvs_config_source_get(&g_cfg, "persistence.rdb_path");
		if (source) (void)kvs_config_source_format(source, rdb_source, sizeof(rdb_source));
		source = kvs_config_source_get(&g_cfg, "persistence.aof_path");
		if (source) (void)kvs_config_source_format(source, aof_source, sizeof(aof_source));
		source = kvs_config_source_get(&g_cfg, "vector.index_type");
		if (source) (void)kvs_config_source_format(source, index_source, sizeof(index_source));
		source = kvs_config_source_get(&g_cfg, "embedding.model_path");
		if (source) (void)kvs_config_source_format(source, model_source, sizeof(model_source));
		source = kvs_config_source_get(&g_cfg, "vector_snapshot.index_path");
		if (source) (void)kvs_config_source_format(source, snapshot_source, sizeof(snapshot_source));
		KVS_LOG_INFO("config",
			"effective server role=%s test_mode=%d listen=%s:%d network_mode=%d worker_threads=%d max_connections=%d backlog=%d request_bytes=%zu response_bytes=%zu commands_per_read=%d",
			g_cfg.role == KVS_ROLE_MASTER ? "master" : "slave",
			g_cfg.test_mode,
			g_cfg.listen_address,
			g_cfg.port,
			g_cfg.network_mode,
			g_cfg.network_worker_threads,
			g_cfg.network_max_connections,
			g_cfg.network_listen_backlog,
			g_cfg.network_max_request_bytes,
			g_cfg.network_max_response_bytes,
			g_cfg.network_max_commands_per_read);
		KVS_LOG_INFO("config",
			"effective persistence rdb_enabled=%d rdb_path=%s rdb_source=%s aof_enabled=%d aof_path=%s aof_source=%s rewrite_enabled=%d",
			g_cfg.enable_rdb, g_cfg.rdb_filename, rdb_source,
			g_cfg.enable_aof, g_cfg.aof_filename, aof_source,
			g_cfg.aof_rewrite_enabled);
		KVS_LOG_INFO("config",
			"effective replication transport=%s master=%s service_port=%d master_sync_port=%d sync_listen_port=%d full_sync=%s not_ready_query=%s ebpf_mode=%d ebpf_object=%s mirror_targets=%zu",
			g_cfg.replication_transport == KVS_REPLICATION_TCP ? "tcp" : "disabled",
			g_cfg.replication_master_host,
			g_cfg.replication_master_service_port,
			g_cfg.replication_master_sync_port,
			g_cfg.replication_sync_listen_port,
			g_cfg.replication_full_sync_policy,
			g_cfg.replication_not_ready_query_policy,
			(int)g_cfg.ebpf_mode,
			g_cfg.ebpf_object_path,
			g_cfg.ebpf_mirror_target_count);
		KVS_LOG_INFO("config",
			"effective vector enabled=%d index=%s index_source=%s dimension=%d metric=%s normalize=%d M=%d M0=%d efConstruction=%d efSearch=%d initial_capacity=%d provider=%s model_path=%s model_source=%s model_identity=%016llx snapshot_enabled=%d snapshot_path=%s snapshot_source=%s rebuild_async=%d delta_initial=%d delta_max=%d",
			g_cfg.vector_enabled, g_cfg.vector_index_type, index_source,
			g_cfg.vector_dimension, g_cfg.vector_metric, g_cfg.vector_normalize,
			g_cfg.vector_hnsw_m, g_cfg.vector_hnsw_m0,
			g_cfg.vector_hnsw_ef_construction, g_cfg.vector_hnsw_ef_search,
			g_cfg.vector_hnsw_initial_capacity, g_cfg.embedding_provider,
			g_cfg.embedding_model_path, model_source,
			(unsigned long long)g_cfg.embedding_model_identity,
			g_cfg.vector_snapshot_enabled, g_cfg.vector_snapshot_index_path,
			snapshot_source, g_cfg.vector_rebuild_async_enabled,
			g_cfg.vector_rebuild_delta_initial_capacity,
			g_cfg.vector_rebuild_delta_max_entries);
	}

	//根据 kvs_config role 初始化global_dist_config.role
	memset(&global_dist_config, 0, sizeof(global_dist_config));
	if(g_cfg.role == KVS_ROLE_MASTER){
		global_dist_config.role = ROLE_MASTER;
	}else{
		global_dist_config.role = ROLE_SLAVE;
	}

	// 注册信号处理函数，使Ctrl+C也能保存数据
		// 注册信号处理函数：Ctrl+C / kill TERM 触发优雅退出，USR1 打印内存统计
		signal(SIGUSR1, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

	// 根据配置绑定内存池
	if(g_cfg.mem_backend == KVS_MEM_BACKEND_JEMALLOC){
		mem_set_backend(KVS_MEM_BACKEND_JEMALLOC);
		// 绑定 jemalloc 函数指针
		mem_set_jemalloc_funcs(jemalloc_alloc_impl, jemalloc_free_impl, jemalloc_realloc_impl);
	}else if(g_cfg.mem_backend == KVS_MEM_BACKEND_SYSTEM){
		mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	}else{
		mem_set_backend(KVS_MEM_BACKEND_POOL);
	}

	//初始化内存管理组件
	mem_init();

	pthread_rwlock_init(&g_kv_data_rwlock, NULL);

	if (init_kvengine() != 0) {
		KVS_LOG_ERROR("core", "failed to initialize kv engine");
		pthread_rwlock_destroy(&g_kv_data_rwlock);
		mem_destroy();
		kvs_log_shutdown();
		return 1;
	}

	//先加载RDB文件
	#if ENABLE_RDB
	if (g_cfg.enable_rdb) {
		kvs_rdb_load(g_cfg.rdb_filename);
	}
	#endif

	
	// 加载 AOF
	if (g_cfg.enable_aof) {
		if (aof_init(g_cfg.aof_filename, g_cfg.aof_fsync_mode) != 0) {
			KVS_LOG_ERROR("core", "failed to initialize AOF: %s", g_cfg.aof_filename);
			dest_kvengine();
			return 1;
		}
		aof_load(g_cfg.aof_filename);
	}

	if (g_cfg.vector_enabled &&
	    kvs_vector_rebuild_after_load() != KVS_VECTOR_OK) {
		KVS_LOG_ERROR("vector", "vector rebuild after load failed");
		dest_kvengine();
		return 1;
	}

	#if 1
	// 分布式初始化
	//仅在 TCP 同步模式下启用分布式功能
		if (g_cfg.replication_transport == KVS_REPLICATION_TCP) {
			if (g_cfg.role == KVS_ROLE_SLAVE) {
				if (g_cfg.replication_master_host[0] == '\0' ||
				    g_cfg.replication_master_sync_port == 0) {
					KVS_LOG_ERROR("core", "TCP slave requires master host and sync port");
					return 1;
				}

				distributed_init(ROLE_SLAVE, g_cfg.replication_master_host,
					g_cfg.replication_master_sync_port, g_cfg.port);
				KVS_LOG_INFO("core", "starting as SLAVE on port %d, connecting to master %s:%d",
					g_cfg.port, g_cfg.replication_master_host,
					g_cfg.replication_master_sync_port);
			} else {
				int sync_port = g_cfg.replication_sync_listen_port;
				distributed_init(ROLE_MASTER, NULL, 0, sync_port);
			KVS_LOG_INFO("core", "starting as MASTER on port %d, sync port %d", g_cfg.port, sync_port);
			KVS_LOG_INFO("core", "AOF enabled: %s (%d), fsync mode: %d",
				g_cfg.aof_filename, g_cfg.enable_aof, g_cfg.aof_fsync_mode);
		}
		}else{
			KVS_LOG_INFO("core",
				"TCP replication disabled; role enforcement remains active, ebpf_mode=%d",
				(int)g_cfg.ebpf_mode);
	}
	#endif

	if (kvs_command_executor_start() != 0) {
		KVS_LOG_ERROR("core", "failed to start command executor");
		dest_kvengine();
		return 1;
	}

	if (kvs_ttl_background_start() != 0) {
		KVS_LOG_WARN("ttl", "failed to start TTL background scan thread");
	}

    int mode = g_cfg.network_mode;

	if (mode == NETWORK_NTYCO) {
		KVS_LOG_INFO("core",
			"network_mode=ntyco routes commands through single command executor.");
		KVS_LOG_INFO("core", "starting in NTYCO mode on port %d", g_cfg.port);
        network_status = ntyco_start(g_cfg.port, kvs_filter_protocol);
    } else if (mode == NETWORK_PROACTOR) {
		KVS_LOG_INFO("core", "starting in PROACTOR mode on port %d", g_cfg.port);
        network_status = proactor_start(g_cfg.port, kvs_filter_protocol);
    } else if(mode == NETWORK_REACTOR){
		KVS_LOG_INFO("core", "starting in REACTOR mode on port %d", g_cfg.port);
        network_status = reactor_start(g_cfg.port, kvs_filter_protocol);
    }else {
		KVS_LOG_INFO("core", "network_mode=%d, no network listener started (offline mode)", mode);
		network_status = -1;
	}
	if (network_status < 0) {
		KVS_LOG_ERROR("core", "network listener failed to start or terminated with an error");
	}
       
    // 保存数据并退出
	KVS_LOG_INFO("core", "shutdown requested%s",
		network_status < 0 ? " after network failure; persistence save skipped" : ", saving data...");

	kvs_ttl_background_stop();

	#if ENABLE_AOF
	if (network_status >= 0 && g_cfg.enable_aof) aof_fsync(1);
	#endif

#if ENABLE_RDB
	if (network_status >= 0 && g_cfg.enable_rdb) kvs_rdb_save(g_cfg.rdb_filename);
#endif
	kvs_command_executor_stop();
	KVS_LOG_INFO("core", "shutdown complete");
    dest_kvengine();
	pthread_rwlock_destroy(&g_kv_data_rwlock);
	kvs_log_shutdown();

    return network_status < 0 ? 1 : 0;
    
}
#endif
