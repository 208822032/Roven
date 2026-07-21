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

/*
 * 把 RESP 解析器给出的命令参数，按同步路径执行一次。
 *
 * 这是一个同步入口：函数不会把 task 放进 command executor 队列，
 * 而是在当前线程依次完成“建任务、准备、执行、提交后处理、生成响应”。
 * 因此，tokens 和 token_lens 只需要在本函数调用期间有效；task 销毁后
 * 不能继续使用它们。AOF 回放等同步调用场景会使用这条路径。
 *
 * 参数说明：
 * - tokens：RESP 解析后的参数数组。tokens[0] 是命令名，例如 "SET"。
 * - count：参数个数，必须与 tokens 和 token_lens 的元素个数一致。
 * - token_lens：每个参数的真实字节长度。不能只依赖 '\0'，因为 value
 *   可能包含二进制数据。
 * - response：调用方提供的响应缓冲区，容量按 KVS_MAX_RESPONSE_LENGTH
 *   使用。该函数只借用它，绝不会释放它。
 *
 * 返回值：
 * - 大于 0：最终 RESP 响应的字节数，数据已经写入 response。
 * - 小于等于 0：建任务、准备或执行失败，response_task 会把有效长度设为 0。
 */
int kvs_filter_protocol(char **tokens, int count, size_t *token_lens, char *response){
	/*
	 * 保存本次调用的响应状态和响应缓冲区信息。
	 * 它借用 response，不能释放 response 本身。
	 */
	kvs_response_task_t response_task;

	/*
	 * 保存本次命令的参数、命令表匹配结果、锁状态和提交状态。
	 * task 只在本函数内使用，返回前必须销毁。
	 */
	kvs_command_task_t task;

	/*
	 * 保存当前阶段的最终结果。
	 * 成功时通常是 RESP 响应字节数；失败时是小于等于 0 的错误结果。
	 */
	int ret;

	/* 只保存 prepare 阶段的返回值，用来区分“准备失败”和“执行失败”。 */
	int prep_ret;

	/*
	 * response 由调用方申请，所以使用 borrowed 初始化。
	 * destroy 时只会重置 response_task，不会释放 response。
	 */
	kvs_response_task_init_borrowed(&response_task, response, KVS_MAX_RESPONSE_LENGTH);

	/*
	 * 第一步：根据参数查命令表、校验参数数量，并初始化 command task。
	 * build 失败时，task.status 中保存可返回给调用方的错误状态。
	 */
	if (kvs_command_task_build(&task, tokens, count, token_lens, &response_task) < 0) {
		ret = task.status;
		/* 统一写入最终 status 和响应长度。 */
		kvs_response_task_finalize(&response_task, ret);
		/* 即使 build 失败，也要清理 build 阶段已申请的局部资源。 */
		kvs_command_task_destroy(&task);
		kvs_response_task_destroy(&response_task);
		return ret;
	}

	/*
	 * 第二步：执行真正命令前的准备工作。
	 * 例如，检查锁模式、处理 TTL 相关准备，或准备向量命令需要的数据。
	 * 准备失败时不能继续执行命令，否则可能在不满足前提时修改存储。
	 */
	prep_ret = kvs_command_task_prepare(&task);
	if (prep_ret != 0) {
		ret = prep_ret;
		kvs_response_task_finalize(&response_task, ret);
		kvs_command_task_destroy(&task);
		kvs_response_task_destroy(&response_task);
		return ret;
	}

	/*
	 * 第三步：同步执行命令。handler 会读取 task 中的参数，
	 * 并把 RESP 响应写入 response_task 借用的 response 缓冲区。
	 */
	ret = kvs_command_task_execute_sync(&task);

	/*
	 * 第四步：执行提交后的派生动作，例如 AOF 追加和普通 KV/TTL 复制。
	 * 该函数根据 ret 和 task 的提交状态决定哪些动作应真正发生。
	 */
	kvs_command_task_post_commit(&task, ret);

	/*
	 * 第五步：把执行结果固定下来，同时记录最终响应长度。
	 * 若响应超过配置的网络响应上限，finalize 会改写为 RESP 错误响应。
	 */
	kvs_response_task_finalize(&response_task, ret);

	/*
	 * 之后只从 response_task 读取最终状态，避免继续使用 execute_sync 的
	 * 临时返回值。随后释放 task 的局部资源；response 是借用内存，不会被释放。
	 */
	ret = kvs_response_task_status(&response_task);
	kvs_command_task_destroy(&task);
	kvs_response_task_destroy(&response_task);
	return ret;
}

/*
 * 初始化 KV 引擎中的全局数据结构。
 *
 * 这个函数只负责创建内存中的存储对象，不负责加载 RDB、回放 AOF、
 * 启动复制线程或启动网络监听。这些工作由 main 在本函数成功后继续完成。
 * 调用时应还没有客户端命令在运行，因此可以直接初始化全局对象。
 *
 * 返回值：
 * - 0：所有已启用的存储后端和可选向量模块都初始化成功。
 * - -1：任一步骤失败，函数立即返回。这里不会回滚此前已创建的对象，
 *   调用方需要进入销毁流程，或由进程退出时回收资源。
 */
int init_kvengine(void) {

	/* 内存分配器已在 main 中根据配置初始化，这里不重复初始化。 */
	// mem_init();

#if ENABLE_ARRAY
	/*
	 * global_array 是 Array 后端的全局实例。
	 * 先清零，避免旧的指针或计数影响 create；初始容量来自统一配置。
	 */
	memset(&global_array, 0, sizeof(kvs_array_t));
	if (kvs_array_create_with_capacity(&global_array, g_cfg.storage_array_initial_capacity) != 0) {
		/* Array 无法创建时，后续后端和向量模块都不能安全继续初始化。 */
		return -1;
	}
#endif

#if ENABLE_RBTREE
	/*
	 * global_rbtree 是红黑树后端的全局实例。
	 * 红黑树创建时会建立根节点和哨兵等基础状态，不依赖数组初始容量。
	 */
	memset(&global_rbtree, 0, sizeof(kvs_rbtree_t));
	if (kvs_rbtree_create(&global_rbtree) != 0) {
		return -1;
	}
#endif

#if ENABLE_HASH
	/*
	 * global_hash 是 Hash 后端的全局实例。
	 * 初始容量影响开放寻址表的开始大小，也会影响前期扩容次数。
	 */
	memset(&global_hash, 0, sizeof(kvs_hash_t));
	if (kvs_hash_create_with_capacity(&global_hash, g_cfg.storage_hash_initial_capacity) != 0) {
		return -1;
	}
#endif

#if ENABLE_SKIPTABLE
	/*
	 * global_skiptable 是跳表后端的全局实例。
	 * create 会准备头节点和各层索引，后续 SET 才会插入具体 key/value。
	 */
	memset(&global_skiptable, 0, sizeof(kvs_skiptable_t));
	if (kvs_skiptable_create(&global_skiptable) != 0) {
		return -1;
	}
#endif

	/*
	 * 向量功能不是所有部署都需要。只有配置明确启用时才初始化。
	 * kvs_vector_init 会创建向量记录管理器和当前配置选择的索引后端。
	 */
	if (g_cfg.vector_enabled && kvs_vector_init() != KVS_VECTOR_OK) return -1;

	/* 所有编译启用且配置启用的内存存储模块均已可用。 */
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

/*
 * 把配置中的日志级别字符串转换为内部枚举值。
 *
 * 参数说明：
 * - value：配置读取到的字符串，例如 "error"、"warn"、"info" 或 "debug"。
 *   比较时不区分大小写，因此 "INFO" 和 "info" 的效果相同。
 * - valid：可选输出参数。非 NULL 时，函数会写入 1 或 0：
 *   1 表示 value 为空或是支持的日志级别；0 表示字符串不被支持。
 *
 * 返回值始终是一个可用的 kvs_log_level_t。为了保证日志系统可以启动，
 * 空值和非法值都会返回 INFO；两者的区别由 valid 区分：空值是正常默认值，
 * 非法值会让调用方记录一条配置错误日志。
 */
static kvs_log_level_t kvs_parse_log_level_value(const char *value, int *valid) {
	/* 先假定输入合法；后面遇到未知字符串时再改为 0。 */
	if (valid) *valid = 1;

	/*
	 * 没有配置日志级别时使用默认 INFO。
	 * 这是“缺省值”，不是配置错误，所以 valid 保持为 1。
	 */
	if (!value || value[0] == '\0') {
		return KVS_LOG_LEVEL_INFO;
	}

	/* 只输出错误日志，适合生产环境的最小日志量。 */
	if (strcasecmp(value, "error") == 0) return KVS_LOG_LEVEL_ERROR;

	/* warn 和完整拼写 warning 都表示警告级别。 */
	if (strcasecmp(value, "warn") == 0 || strcasecmp(value, "warning") == 0) return KVS_LOG_LEVEL_WARN;

	/* 输出错误、警告和常规运行信息，是未配置时的默认级别。 */
	if (strcasecmp(value, "info") == 0) return KVS_LOG_LEVEL_INFO;

	/* 额外输出调试细节，通常只在排查问题时启用。 */
	if (strcasecmp(value, "debug") == 0) return KVS_LOG_LEVEL_DEBUG;

	/*
	 * 未识别的字符串不能传给日志模块。
	 * 回退到 INFO 保证服务仍能启动，同时通知调用方输出配置错误信息。
	 */
	if (valid) *valid = 0;
	return KVS_LOG_LEVEL_INFO;
}

// 信号处理函数
void handle_signal(int sig) {
	(void)sig;
	kvs_shutdown_request();
}

#if 1
/*
 * KVLite 进程入口。
 *
 * 启动顺序：
 * 1. 读取并校验配置，初始化日志、内存分配器和全局 KV 引擎。
 * 2. 恢复 RDB/AOF 数据，并为向量记录加载或重建派生索引。
 * 3. 初始化复制、命令 executor、TTL 扫描线程和选定的网络模型。
 * 4. 网络循环退出后，停止后台工作、按条件持久化，再依次释放资源。
 *
 * 返回 0 表示网络循环正常结束；返回 1 表示配置、初始化或网络运行失败。
 */
int main(int argc, char *argv[]) {
	/* 保存配置加载失败时的文件、行号和错误文字。 */
	kvs_config_error_t config_error;
	/* 保存网络循环的退出结果，后面据此决定是否还执行落盘。 */
	int network_status = 0;

	/* 清掉上一次运行留下的退出标记，并登记当前线程为启动阶段的 owner。 */
	kvs_shutdown_reset_for_init();
	kvs_command_owner_register_startup_thread();

	/*
	 * 合并内置默认值、配置文件和 CLI 覆盖项，并写入全局有效配置 g_cfg。
	 * 配置不合法时，此时日志系统尚未初始化，所以直接写 stderr 后退出。
	 */
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

	/*
	 * 把文本日志级别转换为内部枚举。log_level_valid 单独记录配置是否合法，
	 * 因为非法值会回退到 INFO，服务仍然可以继续启动。
	 */
	int log_level_valid = 1;
	kvs_log_level_t log_level = kvs_parse_log_level_value(g_cfg.log_level, &log_level_valid);
	/* 空字符串表示不写日志文件，日志模块会使用 stderr。 */
	const char *log_path = (g_cfg.log_file[0] != '\0') ? g_cfg.log_file : NULL;

	/* 初始化同步或异步日志；文件打开失败时日志模块回退到 stderr。 */
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
		/*
		 * 下面五个缓冲区只用于启动日志：记录关键配置最终来自默认值、
		 * 配置文件还是 CLI，便于排查“为什么实际配置与预期不同”。
		 */
		/* RDB 路径最终取值的来源文本。 */
		char rdb_source[KVS_CONFIG_PATH_MAX + 64] = "unknown";
		/* AOF 路径最终取值的来源文本。 */
		char aof_source[KVS_CONFIG_PATH_MAX + 64] = "unknown";
		/* 向量索引类型最终取值的来源文本。 */
		char index_source[KVS_CONFIG_PATH_MAX + 64] = "unknown";
		/* embedding 模型路径最终取值的来源文本。 */
		char model_source[KVS_CONFIG_PATH_MAX + 64] = "unknown";
		/* 向量索引快照路径最终取值的来源文本。 */
		char snapshot_source[KVS_CONFIG_PATH_MAX + 64] = "unknown";
		/* 临时指向统一配置中某个字段的来源元数据。 */
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

	/*
	 * distributed 模块仍使用 global_dist_config.role。
	 * 这里把统一配置 g_cfg.role 转换为该模块使用的 master/slave 枚举。
	 */
	memset(&global_dist_config, 0, sizeof(global_dist_config));
	if(g_cfg.role == KVS_ROLE_MASTER){
		global_dist_config.role = ROLE_MASTER;
	}else{
		global_dist_config.role = ROLE_SLAVE;
	}

	/*
	 * 三种信号目前都复用 handle_signal，效果都是设置 shutdown 标记并请求
	 * NtyCo 退出。注意：SIGUSR1 当前也会触发退出，并不会打印内存统计。
	 */
	signal(SIGUSR1, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

	/*
	 * 先选择内存后端，再初始化内存管理器。
	 * jemalloc 模式还需要注册实际 malloc/free/realloc 包装函数；其他模式
	 * 分别使用系统分配器或项目内存池。
	 */
	if(g_cfg.mem_backend == KVS_MEM_BACKEND_JEMALLOC){
		mem_set_backend(KVS_MEM_BACKEND_JEMALLOC);
		// 绑定 jemalloc 函数指针
		mem_set_jemalloc_funcs(jemalloc_alloc_impl, jemalloc_free_impl, jemalloc_realloc_impl);
	}else if(g_cfg.mem_backend == KVS_MEM_BACKEND_SYSTEM){
		mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	}else{
		mem_set_backend(KVS_MEM_BACKEND_POOL);
	}

	/* 所有后续存储、任务和响应对象都会通过该内存管理器申请内存。 */
	mem_init();

	/*
	 * KV 数据锁保护全局存储。命令 task 根据命令表选择读锁或写锁。
	 * 初始化成功后，销毁前必须保证所有使用者都已经停止。
	 */
	pthread_rwlock_init(&g_kv_data_rwlock, NULL);

	/* 创建四类编译启用的 KV 后端，以及配置启用的向量管理器。 */
	if (init_kvengine() != 0) {
		KVS_LOG_ERROR("core", "failed to initialize kv engine");
		pthread_rwlock_destroy(&g_kv_data_rwlock);
		mem_destroy();
		kvs_log_shutdown();
		return 1;
	}

	/*
	 * 先恢复 RDB 基础快照。当前代码未检查 kvs_rdb_load 的返回值，
	 * 即 RDB 加载错误不会在这里阻止后续 AOF 和网络启动。
	 */
	#if ENABLE_RDB
	if (g_cfg.enable_rdb) {
		kvs_rdb_load(g_cfg.rdb_filename);
	}
	#endif

	
	/*
	 * 再打开并回放 AOF，使数据状态前进到快照之后的最新 mutation。
	 * aof_init 失败会清理 KV 引擎并退出；当前 aof_load 的返回值未在此检查。
	 */
	if (g_cfg.enable_aof) {
		if (aof_init(g_cfg.aof_filename, g_cfg.aof_fsync_mode) != 0) {
			KVS_LOG_ERROR("core", "failed to initialize AOF: %s", g_cfg.aof_filename);
			dest_kvengine();
			return 1;
		}
		aof_load(g_cfg.aof_filename);
	}

	/*
	 * 向量记录已由 RDB/AOF 恢复后，再加载索引快照或从记录重建派生索引。
	 * 这一步失败意味着向量记录无法获得可服务的本地索引，因此停止启动。
	 */
	if (g_cfg.vector_enabled &&
	    kvs_vector_rebuild_after_load() != KVS_VECTOR_OK) {
		KVS_LOG_ERROR("vector", "vector rebuild after load failed");
		dest_kvengine();
		return 1;
	}

	#if 1
	/*
	 * 仅在 TCP 复制模式下启动 distributed 模块。
	 * slave 需要主节点地址和同步端口；master 则监听自己的同步端口。
	 * 复制关闭时，role 仍保留在 global_dist_config，用于 slave 写保护判断。
	 */
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
				/* master 用这个端口接收 slave 的同步连接。 */
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

	/*
	 * 启动唯一的命令提交线程。网络线程、TTL 和复制线程都通过它提交
	 * 数据库可见的 mutation；启动失败时不能接入客户端请求。
	 */
	if (kvs_command_executor_start() != 0) {
		KVS_LOG_ERROR("core", "failed to start command executor");
		dest_kvengine();
		return 1;
	}

	/*
	 * TTL 后台线程只定期提交扫描任务，真正的过期删除仍由 command owner 完成。
	 * 启动失败记录警告，但当前服务会继续运行，并依赖访问时的惰性过期。
	 */
	if (kvs_ttl_background_start() != 0) {
		KVS_LOG_WARN("ttl", "failed to start TTL background scan thread");
	}

	/* 从统一配置读取网络模型；该变量决定下面启动哪个监听实现。 */
    int mode = g_cfg.network_mode;

	/*
	 * 网络启动调用会进入对应事件循环，通常直到收到退出请求或发生网络错误才返回。
	 * 三种网络模型都把 kvs_filter_protocol 作为同步兼容回调传入。
	 */
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
       
	/*
	 * 网络循环退出后开始关闭流程。
	 * 如果网络因错误退出，当前代码跳过 AOF/RDB 最终落盘，避免在异常状态下
	 * 把可能不完整的内存状态覆盖到持久化文件。
	 */
	KVS_LOG_INFO("core", "shutdown requested%s",
		network_status < 0 ? " after network failure; persistence save skipped" : ", saving data...");

	kvs_ttl_background_stop();

	/* 正常退出时强制把 AOF 用户态缓冲区和内核页缓存写入磁盘。 */
	#if ENABLE_AOF
	if (network_status >= 0 && g_cfg.enable_aof) aof_fsync(1);
	#endif

	/* 正常退出时创建最终 RDB 快照。 */
#if ENABLE_RDB
	if (network_status >= 0 && g_cfg.enable_rdb) kvs_rdb_save(g_cfg.rdb_filename);
#endif

	/* 停止 executor 并排空其任务后，把当前线程登记为已排空的 shutdown owner。 */
	kvs_command_executor_stop();
	kvs_command_owner_register_shutdown_thread_after_drain();
	KVS_LOG_INFO("core", "shutdown complete");

	/* 依赖 executor 和网络线程已停止，销毁复制、AOF、存储、向量和内存后端。 */
    dest_kvengine();
	/* 数据锁只能在所有使用者停止且 KV 引擎销毁后释放。 */
	pthread_rwlock_destroy(&g_kv_data_rwlock);
	/* 最后关闭日志，确保前面的 shutdown 日志能够输出。 */
	kvs_log_shutdown();

	/* 将网络循环结果转换为进程退出码。 */
    return network_status < 0 ? 1 : 0;
    
}
#endif
