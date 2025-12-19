#include "kvstore.h"
#include "distributed.h"
#include "resp_protocol.h"
#include "kvs_memory.h"
#include "sync_mode.h"
#include "kvs_config.h"
#include "kvs_globals.h"
#include "jemalloc_wrapper.h"
#include "kvs_memory_internal.h"


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


int kvs_filter_protocol(char **tokens, int count, char *response){

	if (tokens[0] == NULL || count == 0) {
        return resp_generate_error(response, "Invalid command");
    }

	//查找命令
	const command_info *cmd = lookup_command(tokens[0]);
	if(cmd == NULL) return resp_generate_error(response, "Unknown command");

	int cmd_index = get_command_index(tokens[0]);
	if(cmd_index == -1) return resp_generate_error(response, "Unknown command");

	//检查参数数量
	if(!check_arity(cmd, count)) return resp_generate_error(response, "Wrong number of arguments");
	
	if (cmd->write_command && global_dist_config.role == ROLE_SLAVE && get_sync_context() == 0){
		return resp_generate_error(response, "ERROR SLAVE READ ONLY");
	}
	
	//解析命令参数
	command_context_t ctx;
	if(parse_command_argument(cmd_index, tokens, count, &ctx) < 0){
		return resp_generate_error(response, "Invalid arguments");
	}

    int ret = cmd->handler(&ctx, response);

	//命令执行成功后记录写命令
	if(ret >= 0 && cmd->write_command){
		#if ENABLE_AOF// 添加日志打印命令
			if(!aof_is_loading()){
				aof_append(count, tokens);//正在加载AOF时不记录
			}
		#endif

		if(get_sync_context() != 2){
			// 主节点需要将命令分发给从节点 仅在TCP同步模式下
			if (global_dist_config.role == ROLE_MASTER && g_cfg.sync_mode == KVS_SYNC_TCP) {
				// 无论 TCP 还是 eBPF 模式，依然通过 master_sync_command 发送；
				// eBPF 模式下，内核通过 kprobe/tcp_sendmsg 抓该同步端口上的数据包。
				master_sync_command(tokens[0], tokens[1], (count > 2) ? tokens[2] : NULL);
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

	return 0;
}

void dest_kvengine(void) {

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

	mem_destroy();  // 清理内存管理组件
	
	if(g_cfg.sync_mode == KVS_SYNC_TCP){
	distributed_shutdown(); //关闭分布式系统
	}


	#if ENABLE_AOF
	aof_free();
	#endif

}



extern kvs_config_t g_cfg;

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

	// 如果指定了配置文件，则尝试加载
	if (config_path) {
		if (kvs_config_load_file(&g_cfg, config_path) != 0) {
			fprintf(stderr, "Warning: failed to load config file %s, using defaults/CLI overrides\n", config_path);
		}
	} else {
		// 默认配置文件名：config/kvstore.conf
		kvs_config_load_file(&g_cfg, "config/kvstore.conf");
	}

	// 再次应用命令行，保证命令行优先级最高
	kvs_config_apply_cmdline(&g_cfg, argc, argv, NULL);

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

	init_kvengine();

	//先加载RDB文件
	#if ENABLE_RDB
		kvs_rdb_load(g_cfg.rdb_filename);
	#endif

	
	// 加载 AOF

	if (g_cfg.enable_aof) {
		if (aof_init(g_cfg.aof_filename, g_cfg.aof_fsync_mode) != 0) {
			fprintf(stderr, "Failed to initialize AOF: %s\n", g_cfg.aof_filename);
			dest_kvengine();
			return 1;
		}
		aof_load(g_cfg.aof_filename);
	}

	#if 1
	// 分布式初始化
	//仅在 TCP 同步模式下启用分布式功能
	if (g_cfg.sync_mode == KVS_SYNC_TCP) {
		if (g_cfg.role == KVS_ROLE_SLAVE) {
			if (g_cfg.master_ip[0] == '\0' || g_cfg.master_port == 0) {
				fprintf(stderr, "Error: Slave mode requires master IP and port\n");
				return 1;
			}

			// 主节点的同步端口已经在 g_cfg.sync_port 里计算好（默认 master_port+1000）
			distributed_init(ROLE_SLAVE, g_cfg.master_ip, g_cfg.sync_port, g_cfg.port);
			printf("Starting as SLAVE on port %d, connecting to master %s:%d\n",
				g_cfg.port, g_cfg.master_ip, g_cfg.sync_port);
		} else {
			// 主节点同步端口 = 服务端口 + 1000（或配置文件提供）
			int sync_port = g_cfg.sync_port;
			if (sync_port == 0) sync_port = g_cfg.port + 1000;
			distributed_init(ROLE_MASTER, NULL, 0, sync_port);
			printf("Starting as MASTER on port %d, sync port %d\n", g_cfg.port, sync_port);
			printf("AOF enabled: %s (%d), fsync mode: %d\n",
				g_cfg.aof_filename, g_cfg.enable_aof, g_cfg.aof_fsync_mode);
		}
	}else{
		// eBPF 模式：不启动 TCP 分布式模块，仅保留 role 用于“slave 只读”判断
    printf("Distributed sync disabled (sync_mode = eBPF). "
           "Slave role is still enforced as read-only.\n");
	}
	#endif

    int mode = g_cfg.network_mode;

	if (mode == NETWORK_NTYCO) {
		printf("Starting in NTYCO mode on port %d\n", g_cfg.port);
        ntyco_start(g_cfg.port, kvs_filter_protocol);
    } else if (mode == NETWORK_PROACTOR) {
		printf("Starting in PROACTOR mode on port %d\n", g_cfg.port);
        proactor_start(g_cfg.port, kvs_filter_protocol);
    } else if(mode == NETWORK_REACTOR){
		printf("Starting in REACTOR mode on port %d\n", g_cfg.port);
        reactor_start(g_cfg.port, kvs_filter_protocol);
    }else {
		printf("network_mode=%d, no network listener started (offline mode)\n", mode);
	}
       
    // 保存数据并退出
    printf("Shutdown requested, saving data...\n");

	#if ENABLE_AOF
	aof_fsync(1);
	#endif

#if ENABLE_RDB
	kvs_rdb_save(g_cfg.rdb_filename);
#endif
	printf("Data saved. Exiting.\n");
    dest_kvengine();

    return 0;    
    
}
#endif


