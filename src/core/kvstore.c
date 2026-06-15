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

#include <strings.h>

pthread_rwlock_t g_kv_data_rwlock = PTHREAD_RWLOCK_INITIALIZER;

int g_shutdown_flag = 0;

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

static int kvs_vector_status_to_resp(int status, char *response) {
	switch (status) {
		case KVS_VECTOR_OK:
			return resp_generate_simple_string_n(response, '+', "OK", sizeof("OK") - 1);
		case KVS_VECTOR_ERR_INVALID_ARG:
			return resp_generate_simple_string_n(response, '-', "ERR invalid vector argument", sizeof("ERR invalid vector argument") - 1);
		case KVS_VECTOR_ERR_NOT_FOUND:
			return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
		case KVS_VECTOR_ERR_NOT_INITIALIZED:
			return resp_generate_simple_string_n(response, '-', "ERR vector index not initialized", sizeof("ERR vector index not initialized") - 1);
		case KVS_VECTOR_ERR_NOT_IMPLEMENTED:
			return resp_generate_simple_string_n(response, '-', "ERR vector command not implemented", sizeof("ERR vector command not implemented") - 1);
		default:
			return resp_generate_simple_string_n(response, '-', "ERR vector operation failed", sizeof("ERR vector operation failed") - 1);
	}
}

static int kvs_vector_search_prepared_to_resp(
	const command_context_t *ctx,
	const kvs_vector_search_prepared_t *prepared,
	char *response
) {
	int topk;
	size_t results_capacity;
	kvs_vector_search_result_t *results = NULL;
	size_t results_count = 0;
	size_t i;
	int status;
	int off = 0;

	if (!ctx || !prepared) {
		return kvs_vector_status_to_resp(KVS_VECTOR_ERR_INVALID_ARG, response);
	}

	topk = ctx->topk;
	if (topk <= 0) {
		topk = 1;
	}
	if (topk > 1024) {
		return kvs_vector_status_to_resp(KVS_VECTOR_ERR_INVALID_ARG, response);
	}

	results_capacity = (size_t)topk;
	results = (kvs_vector_search_result_t *)kvs_malloc(
		results_capacity * sizeof(kvs_vector_search_result_t)
	);
	if (!results) {
		return kvs_vector_status_to_resp(KVS_VECTOR_ERR, response);
	}

	kvs_vector_search_results_reset(results, results_capacity);
	status = kvs_vector_search_with_prepared(
		prepared,
		topk,
		ctx->has_threshold ? ctx->threshold : g_cfg.vector_score_threshold,
		results,
		results_capacity,
		&results_count
	);
	if (status != KVS_VECTOR_OK) {
		kvs_free(results);
		return kvs_vector_status_to_resp(status, response);
	}

	off += resp_generate_array_start(response + off, (int)results_count);
	for (i = 0; i < results_count; ++i) {
		char *item = NULL;
		int item_cap;
		int n;

		item_cap = snprintf(
			NULL, 0,
			"score:%.6f|scope:%.*s|question:%.*s|answer:%.*s",
			results[i].score,
			(int)results[i].scope_len,
			results[i].scope ? results[i].scope : "",
			(int)results[i].question_len,
			results[i].question ? results[i].question : "",
			(int)results[i].answer_len,
			results[i].answer ? results[i].answer : ""
		);
		if (item_cap < 0) {
			kvs_free(results);
			return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
		}

		item_cap += 1;
		item = (char *)kvs_malloc((size_t)item_cap);
		if (!item) {
			kvs_free(results);
			return resp_generate_simple_string_n(response, '-', "ERR out of memory", sizeof("ERR out of memory") - 1);
		}

		n = snprintf(
			item, (size_t)item_cap,
			"score:%.6f|scope:%.*s|question:%.*s|answer:%.*s",
			results[i].score,
			(int)results[i].scope_len,
			results[i].scope ? results[i].scope : "",
			(int)results[i].question_len,
			results[i].question ? results[i].question : "",
			(int)results[i].answer_len,
			results[i].answer ? results[i].answer : ""
		);
		if (n < 0 || n >= item_cap) {
			kvs_free(item);
			kvs_free(results);
			return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
		}

		off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
		kvs_free(item);
	}

	kvs_free(results);
	return off;
}


int kvs_filter_protocol(char **tokens, int count, size_t *token_lens, char *response){

	if (tokens[0] == NULL || count == 0) {
        return resp_generate_simple_string_n(response, '-', "Invalid command", sizeof("Invalid command") - 1);
    }

	//查找命令
	const command_info *cmd = lookup_command(tokens[0]);
	if(cmd == NULL) return resp_generate_simple_string_n(response, '-', "Unknown command", sizeof("Unknown command") - 1);

	int cmd_index = get_command_index(tokens[0]);
	if(cmd_index == -1) return resp_generate_simple_string_n(response, '-', "Unknown command", sizeof("Unknown command") - 1);
	//检查参数数量
	if(!check_arity(cmd, count)) return resp_generate_simple_string_n(response, '-', "Wrong number of arguments", sizeof("Wrong number of arguments") - 1);
	
	if (cmd->write_command && global_dist_config.role == ROLE_SLAVE && get_sync_context() == 0){
		return resp_generate_simple_string_n(response, '-', "ERROR SLAVE READ ONLY", sizeof("ERROR SLAVE READ ONLY") - 1);
	}
	
	//解析命令参数
	command_context_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	if(parse_command_argument(cmd_index, tokens, count, token_lens, &ctx) < 0){
		return resp_generate_simple_string_n(response, '-', "Invalid arguments", sizeof("Invalid arguments") - 1);
	}

	// int use_read_lock = (cmd->handler == handle_get || cmd->handler == handle_exist);
	// int lock_held = 0;
	int use_read_lock = 0;
	int use_write_lock = 0;
	int lock_held = 0;
	int vector_prepared_path = 0;
	int command_committed = 0;
	kvs_vector_set_prepared_t prepared_set;
	kvs_vector_search_prepared_t prepared_search;

	kvs_vector_set_prepared_reset(&prepared_set);
	kvs_vector_search_prepared_reset(&prepared_search);

	switch (cmd->lock_mode)
	{
		case KVS_CMD_LOCK_READ:
			use_read_lock = 1;
			break;
		case KVS_CMD_LOCK_WRITE:
		default:
			use_write_lock = 1;
			break;
		case KVS_CMD_LOCK_NONE:
			break;
	}

	if (strcmp(cmd->name, "VSET") == 0) {
		int status = kvs_vector_set_prepare(
			ctx.query_text,
			ctx.query_text_len,
			ctx.value,
			ctx.value ? strlen(ctx.value) : 0,
			ctx.scope,
			ctx.scope_len,
			&prepared_set
		);
		vector_prepared_path = 1;
		if (status != KVS_VECTOR_OK) {
			return kvs_vector_status_to_resp(status, response);
		}
	} else if (strcmp(cmd->name, "VSEARCH") == 0) {
		int status = kvs_vector_search_prepare(
			ctx.query_text,
			ctx.query_text_len,
			ctx.scope,
			ctx.scope_len,
			&prepared_search
		);
		vector_prepared_path = 1;
		if (status != KVS_VECTOR_OK) {
			return kvs_vector_status_to_resp(status, response);
		}
	}

	if (use_read_lock) {
		if (pthread_rwlock_rdlock(&g_kv_data_rwlock) != 0) {
			kvs_vector_set_prepared_destroy(&prepared_set);
			kvs_vector_search_prepared_destroy(&prepared_search);
			return resp_generate_simple_string_n(response, '-', "Failed to acquire data lock", sizeof("Failed to acquire data lock") - 1);
		}
		lock_held = 1;
		ctx.lock_mode = KVS_LOCK_MODE_READ;
	}else if(use_write_lock){
		if (pthread_rwlock_wrlock(&g_kv_data_rwlock) != 0) {
			kvs_vector_set_prepared_destroy(&prepared_set);
			kvs_vector_search_prepared_destroy(&prepared_search);
			return resp_generate_simple_string_n(response, '-', "Failed to acquire data lock", sizeof("Failed to acquire data lock") - 1);
		}
		lock_held = 1;
		ctx.lock_mode = KVS_LOCK_MODE_WRITE;
	}

	int ret;
	if (strcmp(cmd->name, "VSET") == 0) {
		int status = kvs_vector_set_commit_prepared(&prepared_set);
		command_committed = (status == KVS_VECTOR_OK);
		ret = kvs_vector_status_to_resp(status, response);
	} else if (strcmp(cmd->name, "VSEARCH") == 0) {
		ret = kvs_vector_search_prepared_to_resp(&ctx, &prepared_search, response);
		command_committed = (ret >= 0);
	} else {
		ret = cmd->handler(&ctx, response);
		command_committed = (ret >= 0);
	}
	if (use_read_lock && ret == KVS_CMD_RETRY_WITH_WRITE_LOCK) {
		pthread_rwlock_unlock(&g_kv_data_rwlock);
		lock_held = 0;
		if (pthread_rwlock_wrlock(&g_kv_data_rwlock) != 0) {
			kvs_vector_set_prepared_destroy(&prepared_set);
			kvs_vector_search_prepared_destroy(&prepared_search);
			return resp_generate_simple_string_n(response, '-', "Failed to acquire data lock", sizeof("Failed to acquire data lock") - 1);
		}
		lock_held = 1;
		ctx.lock_mode = KVS_LOCK_MODE_WRITE;
		ret = cmd->handler(&ctx, response);
		command_committed = (ret >= 0);
	}

	int64_t expire_at_ms = 0;
	int has_expire_at = 0;
	int need_del_sync = 0;
	int need_persist_sync = 0;
	if (command_committed && cmd->write_command) {
		if (strcmp(cmd->name, "EXPIRE") == 0 || strcmp(cmd->name, "PEXPIREAT") == 0) {
			if (ttl_get_any_key_expire_at_ms(ctx.key, ctx.key_len, &expire_at_ms) > 0 && expire_at_ms >= 0) {
				has_expire_at = 1;
			} else {
				need_del_sync = (ret > 0);
			}
		} else if (strcmp(cmd->name, "PERSIST") == 0) {
			need_persist_sync = (ret > 0);
		}
	}

	if (lock_held) {
		pthread_rwlock_unlock(&g_kv_data_rwlock);
	}
	if (vector_prepared_path) {
		kvs_vector_set_prepared_destroy(&prepared_set);
		kvs_vector_search_prepared_destroy(&prepared_search);
	}

	//命令执行成功后记录写命令
	if(command_committed && cmd->write_command){
		#if ENABLE_AOF// 添加日志打印命令
			if(!aof_is_loading()){
				if (strcmp(cmd->name, "EXPIRE") == 0) {
					if (has_expire_at) {
						char expire_buf[32];
						snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)expire_at_ms);
						char *aof_tokens[] = {"PEXPIREAT", ctx.key, expire_buf};
						aof_append(3, aof_tokens, NULL);
					} else if (need_del_sync) {
						char *aof_tokens[] = {"DEL", ctx.key};
						aof_append(2, aof_tokens, NULL);
					}
				} else {
					aof_append(count, tokens, token_lens);//正在加载AOF时不记录
				}
			}
		#endif

		if(get_sync_context() != 2){
			// 主节点需要将命令分发给从节点 仅在TCP同步模式下
			if (global_dist_config.role == ROLE_MASTER && g_cfg.sync_mode == KVS_SYNC_TCP) {
				if (strcmp(cmd->name, "EXPIRE") == 0 || strcmp(cmd->name, "PEXPIREAT") == 0) {
					if (ret > 0) {
						if (has_expire_at) {
							char expire_buf[32];
							snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)expire_at_ms);
							master_sync_command("PEXPIREAT", ctx.key, ctx.key_len, expire_buf, strlen(expire_buf));
						} else {
							master_sync_command("DEL", ctx.key, ctx.key_len, NULL, 0);
						}
					}
				} else if (strcmp(cmd->name, "PERSIST") == 0) {
					if (need_persist_sync) {
						master_sync_command("PERSIST", ctx.key, ctx.key_len, NULL, 0);
					}
				} else {
					// 其他写命令保持原样广播
					master_sync_command(tokens[0], tokens[1], token_lens[1], (count > 2) ? tokens[2] : NULL, (count > 2) ? token_lens[2] : 0);
				}
			}
		}
		
	}
	return ret;

}


int init_kvengine(void) {

	//  mem_init();  // 初始化内存管理组件（内部会根据配置选择后端）

#if ENABLE_ARRAY
	memset(&global_array, 0, sizeof(kvs_array_t));
	kvs_array_create(&global_array);
#endif

#if ENABLE_RBTREE
	memset(&global_rbtree, 0, sizeof(kvs_rbtree_t));
	kvs_rbtree_create(&global_rbtree);
#endif

#if ENABLE_HASH
	memset(&global_hash, 0, sizeof(kvs_hash_t));
	kvs_hash_create(&global_hash);
#endif

#if ENABLE_SKIPTABLE
	memset(&global_skiptable, 0, sizeof(kvs_skiptable_t));
	kvs_skiptable_create(&global_skiptable);
#endif

	if(kvs_vector_init() != KVS_VECTOR_OK) return -1;

	return 0;
}

void dest_kvengine(void) {

	// 1. 先停止所有后台线程/使用者，确保不再访问全局数据结构
	if (g_cfg.sync_mode == KVS_SYNC_TCP) {
		distributed_shutdown();
	}

#if ENABLE_AOF
	aof_free();
#endif

	// 2. 加写锁，确保销毁期间无命令线程并发访问数据结构
	pthread_rwlock_wrlock(&g_kv_data_rwlock);

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

	pthread_rwlock_unlock(&g_kv_data_rwlock);

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
	g_shutdown_flag = 1;
}

#if 1
int main(int argc, char *argv[]) {
	// 初始化全局配置为默认值（g_cfg 在 kvs_globals.c 中定义）
	kvs_config_init_default(&g_cfg);

	// 先从命令行解析 --config 等，获取配置文件路径和覆盖项
	const char *config_path = NULL;
	kvs_config_apply_cmdline(&g_cfg, argc, argv, &config_path);
	int config_load_failed = 0;

	// 如果指定了配置文件，则尝试加载
	if (config_path) {
		if (kvs_config_load_file(&g_cfg, config_path) != 0) {
			config_load_failed = 1;
		}
	} else {
		// 默认配置文件名：config/kvstore.conf
		kvs_config_load_file(&g_cfg, "config/kvstore.conf");
	}

	// 再次应用命令行，保证命令行优先级最高
	kvs_config_apply_cmdline(&g_cfg, argc, argv, NULL);

	int log_level_valid = 1;
	kvs_log_level_t log_level = kvs_parse_log_level_value(g_cfg.log_level, &log_level_valid);
	const char *log_path = (g_cfg.log_file[0] != '\0') ? g_cfg.log_file : NULL;
	if (kvs_log_init(log_path, log_level, g_cfg.log_async) != 0 && log_path) {
		KVS_LOG_ERROR("core", "failed to open log file %s, falling back to stderr", log_path);
	}
	if (config_load_failed) {
		KVS_LOG_ERROR("core", "failed to load config file %s, using defaults/CLI overrides", config_path);
	}
	if (!log_level_valid) {
		KVS_LOG_ERROR("core", "invalid log_level=%s, using info", g_cfg.log_level);
	}
	KVS_LOG_INFO("core", "logging initialized: mode=%s level=%s output=%s",
		kvs_log_is_async() ? "async" : "sync",
		kvs_log_level_name(log_level),
		log_path ? log_path : "stderr");

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
		kvs_rdb_load(g_cfg.rdb_filename);
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

	if (kvs_vector_rebuild_after_load() != KVS_VECTOR_OK) {
		KVS_LOG_ERROR("vector", "vector rebuild after load failed");
	}

	#if 1
	// 分布式初始化
	//仅在 TCP 同步模式下启用分布式功能
	if (g_cfg.sync_mode == KVS_SYNC_TCP) {
		if (g_cfg.role == KVS_ROLE_SLAVE) {
			if (g_cfg.master_ip[0] == '\0' || g_cfg.master_port == 0) {
				KVS_LOG_ERROR("core", "slave mode requires master IP and port");
				return 1;
			}

			// 主节点的同步端口已经在 g_cfg.sync_port 里计算好（默认 master_port+1000）
			distributed_init(ROLE_SLAVE, g_cfg.master_ip, g_cfg.sync_port, g_cfg.port);
			KVS_LOG_INFO("core", "starting as SLAVE on port %d, connecting to master %s:%d",
				g_cfg.port, g_cfg.master_ip, g_cfg.sync_port);
		} else {
			// 主节点同步端口 = 服务端口 + 1000（或配置文件提供）
			int sync_port = g_cfg.sync_port;
			if (sync_port == 0) sync_port = g_cfg.port + 1000;
			distributed_init(ROLE_MASTER, NULL, 0, sync_port);
			KVS_LOG_INFO("core", "starting as MASTER on port %d, sync port %d", g_cfg.port, sync_port);
			KVS_LOG_INFO("core", "AOF enabled: %s (%d), fsync mode: %d",
				g_cfg.aof_filename, g_cfg.enable_aof, g_cfg.aof_fsync_mode);
		}
	}else{
		// eBPF 模式：不启动 TCP 分布式模块，仅保留 role 用于“slave 只读”判断
		KVS_LOG_INFO("core", "Distributed sync disabled (sync_mode = eBPF). Slave role is still enforced as read-only.");
	}
	#endif

	if (kvs_ttl_background_start() != 0) {
		KVS_LOG_WARN("ttl", "failed to start TTL background scan thread");
	}

    int mode = g_cfg.network_mode;

	if (mode == NETWORK_NTYCO) {
		KVS_LOG_INFO("core", "starting in NTYCO mode on port %d", g_cfg.port);
        ntyco_start(g_cfg.port, kvs_filter_protocol);
    } else if (mode == NETWORK_PROACTOR) {
		KVS_LOG_INFO("core", "starting in PROACTOR mode on port %d", g_cfg.port);
        proactor_start(g_cfg.port, kvs_filter_protocol);
    } else if(mode == NETWORK_REACTOR){
		KVS_LOG_INFO("core", "starting in REACTOR mode on port %d", g_cfg.port);
        reactor_start(g_cfg.port, kvs_filter_protocol);
    }else {
		KVS_LOG_INFO("core", "network_mode=%d, no network listener started (offline mode)", mode);
	}
       
    // 保存数据并退出
	KVS_LOG_INFO("core", "shutdown requested, saving data...");

	#if ENABLE_AOF
	aof_fsync(1);
	#endif

#if ENABLE_RDB
	kvs_rdb_save(g_cfg.rdb_filename);
#endif
	kvs_ttl_background_stop();
	KVS_LOG_INFO("core", "data saved. exiting.");
    dest_kvengine();
	pthread_rwlock_destroy(&g_kv_data_rwlock);
	kvs_log_shutdown();

    return 0;    
    
}
#endif

