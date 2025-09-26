#include "kvstore.h"
#include "distributed.h"
#include "resp_protocol.h"


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

mem_manager_t g_mem_manager;

extern distributed_config_t global_dist_config;

static __thread int sync_context = 0;//每个线程都有的局部变量

void set_sync_context(int context) {
	sync_context = context;
}
int get_sync_context() {
	return sync_context;
}

#if 1
//保存与加载文件
//检查文件是否存在
int file_exists(const char *filename){

	struct stat buffer;
	return (stat(filename, &buffer) == 0);

}

//保存所有数据到文件
//相同符号 产品化
void kvs_save_all(const char *filename){
	FILE *fp = fopen(filename, "wb");
	if(!fp){
		perror("Failed to open save file\n");
		return;
	}

	//保存数组数据
#if ENABLE_ARRAY
	fprintf(fp, "[ARRAY]\n");
	int i = 0;
	for(i = 0;i < global_array.total; i++){
		if(global_array.table[i].key && global_array.table[i].value){
			//fwrit sizeof int 往前写四个字节
			fprintf(fp, "%s:%s\n", global_array.table[i].key, global_array.table[i].value);
		}
	}
#endif

	//保存红黑树数据
#if ENABLE_RBTREE
	fprintf(fp, "[RBTREE]\n");
	//中序遍历保存
	void save_rbtree(rbtree_node *node, FILE *fp){
		if (node == global_rbtree.nil) return;
		save_rbtree(node->left, fp);
		fprintf(fp, "%s:%s\n", node->key, (char*)node->value);
		save_rbtree(node->right, fp);
	}
	save_rbtree(global_rbtree.root, fp);
#endif

	//哈希表
#if ENABLE_HASH
	fprintf(fp, "[HASH]\n");
	for(i = 0;i < global_hash.max_slots; i++){
		hashnode_t *node = global_hash.nodes[i];
		while(node){
			fprintf(fp, "%s:%s\n", node->key, node->value);
            node = node->next;
		}
	}
#endif

	//跳表
#if ENABLE_SKIPTABLE
	fprintf(fp, "[SKIPTABLE]\n");
	Node *current = global_skiptable.header->forward[0];
	while(current){
		fprintf(fp, "%s:%s\n", current->key, current->value);
        current = current->forward[0];
	}
#endif

	fclose(fp);
	printf("Data saved to %s\n", filename);
}

//从文件加载所有数据
void kvs_load_all(const char *filename){
	if(!file_exists(filename)){
		printf("No save file found\n");
		return;
	}

	FILE *fp = fopen(filename, "rb");
	if(!fp){
		perror("Failed to open save file\n");
		return;
	}

	char line[2024];
	char current_section[32] = {0};
	while(fgets(line, sizeof(line), fp)){
		//去除换行符
		line[strcspn(line, "\n")] = 0;

		//检查段标题
		if(line[0] == '[' && line[strlen(line) - 1] == ']'){
			strncpy(current_section, line+1, strlen(line)-2);//跳过开头的[ 减去[]
			current_section[strlen(line) - 2] = '\0';
			continue;
		}

		char *key = strtok(line, ":");
		char *value = strtok(NULL, "");
		if(!key || !value) continue;

		//根据段标题选择数据结构
		if(strcmp(current_section, "ARRAY") == 0){
			#if ENABLE_ARRAY
			kvs_array_set(&global_array, key, value);
			#endif
		}else if(strcmp(current_section, "RBTREE") == 0){
			#if ENABLE_RBTREE
			kvs_rbtree_set(&global_rbtree, key, value);
			#endif
		}else if(strcmp(current_section, "HASH") == 0){
			#if ENABLE_HASH
			kvs_hash_set(&global_hash, key, value);
			#endif
		}else if(strcmp(current_section, "SKIPTABLE") == 0){
			#if ENABLE_SKIPTABLE
			kvs_skiptable_set(&global_skiptable, key, value);
			#endif
		}
	}

	fclose(fp);
	printf("Data loaded from %s\n", filename);
}
#endif

int kvs_filter_protocol(char **tokens, int count, char *response){
	if (tokens[0] == NULL || count == 0 || response == NULL) {
        return resp_generate_error(response, "Invalid command");
    }

	//查找命令
	const command_info *cmd = lookup_command(tokens[0]);
	if(cmd == NULL) return resp_generate_error(response, "Unknown command");

	int cmd_index = get_command_index(tokens[0]);
	if(cmd_index == -1) return resp_generate_error(response, "Unknown command");

	//检查参数数量
	if(!check_arity(cmd, count)) return resp_generate_error(response, "Wrong number of arguments");
	
	//检查从节点写操作权限
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
	if(ret >= 0 && cmd->write_command && get_sync_context() != 2){
		#if ENABLE_AOF// 添加日志打印命令
		aof_append(count, tokens);
		#endif

		//主节点需要将命令分发给从节点
		if(global_dist_config.role == ROLE_MASTER){
			master_sync_command(tokens[0], tokens[1], (count > 2) ? tokens[2] : NULL);
		}
	}

	return ret;

}

// 性能问题 异步处理 GET/SET性能 是否开启区别不大
// aof 转发命令 主从同步mysql


//支持pipeline
//问题1 大量数据无法读完 分包粘包问题
//问题2 考虑拓展之后的功能 key value1 value2 value3
	//了解 kv协议重新设计 key/value如何传 JSON/redis 做法

int init_kvengine(void) {

	mem_init();  // 初始化内存管理组件

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

	#if ENABLE_AOF
	aof_free();
	#endif

}
//ebpf 主从同步 探测主节点 实现同步功能
//存入论文 标题key 内容value 图片链接 /博客

//3.1 3.2

// 信号处理函数
void handle_signal(int sig) {
    printf("\nReceived signal %d, saving data before shutdown...\n", sig);
	
	aof_fsync(1);

	#if ENABLE_RDB
		kvs_rdb_save("kvstore.rdb");
	#else
		kvs_save_all("kvstore.dat");
	#endif

	
    exit(0);
}

#if 1
int main(int argc, char *argv[]) {

	//if (argc != 2) return -1;

	//默认配置
	int port = 8080;
	int is_slave = 0;
	char *master_ip = NULL;
	int master_port = 0;
	int sync_port = 0;
	int aof_fsync_mod = AOF_FSYNC_EVERYSEC;
	char *aof_filename = "kvstore.aof";

	//int port = atoi(argv[1]);

	//解析命令行参数
#if 1
	int i;
	for(i = 1;i < argc; i++){
		if(strcmp(argv[i], "--slave") == 0){
			is_slave = 1;
		}else if(strcmp(argv[i], "--master") == 0){
			if(i + 2 < argc){
				master_ip = argv[++i];
				master_port = atoi(argv[++i]);
				port = atoi(argv[++i]);
			}else{
				fprintf(stderr, "Error, --master requires IP and PORT\n");
			}
		}else if(i == 1 && !is_slave){//都没有 说明是正常启动 master
			port = atoi(argv[i]);
		}
#if 1
		else if(strcmp(argv[i], "--aof_fsync") == 0){
			if(i + 1 < argc){
				if(strcmp(argv[i+1], "no") == 0){
					aof_fsync_mod = AOF_FSYNC_NO;
				}else if(strcmp(argv[i+1], "always") == 0){
					aof_fsync_mod = AOF_FSYNC_ALWAYS;
				}else{
					aof_fsync_mod = AOF_FSYNC_EVERYSEC;
				}
				i++;
			}else{
				fprintf(stderr, "Error, --aof-fsync requires mode [no|everysec|always]\n");
				return 1;
			}
		}else if(strcmp(argv[i], "--aof_filename") == 0){
			if(i + 1 < argc){
				aof_filename = argv[++i];
			}else{
				fprintf(stderr, "Error, --aof-filename requires filename\n");
                return 1;
			}
		}
#endif
	}
#endif
	// 注册信号处理函数，使Ctrl+C也能保存数据
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

	init_kvengine();

	
	// 加载持久化数据 优先加载AOF

    // 初始化AOF，使用每秒同步模式
    if(aof_init(aof_filename, aof_fsync_mod) != 0){
		fprintf(stderr, "Failed to initialize AOF\n");
		dest_kvengine();
		return 1;
	}

	aof_load(aof_filename);

    #if ENABLE_RDB
		kvs_rdb_load("kvstore.dat");
	#else
		kvs_load_all("kvstore.dat");
	#endif


	//分布式初始化
	if(is_slave){
		if(!master_ip || master_port == 0){
			fprintf(stderr, "Error: Slave mode requires master IP and port\n");
            return 1;
		}

		//主节点的同步端口 = 主节点服务端口 + 1000
		int master_sync_port = master_port + 1000;
		distributed_init(ROLE_SLAVE, master_ip, master_sync_port, port);
		printf("Starting as SLAVE on port %d, connecting to master %s:%d\n", port, master_ip, master_sync_port);
	}else{
		//主节点同步端口 = 服务端口 + 1000
		sync_port = port + 1000;
		distributed_init(ROLE_MASTER, NULL, 0, sync_port);
		printf("Starting as MASTER on port %d, sync port %d\n", port, sync_port);
		printf("AOF enabled: %s, fsync mode: %d\n", aof_filename, aof_fsync_mod);
	}
	
#if (NETWORK_SELECT == NETWORK_REACTOR)
	reactor_start(port, kvs_filter_protocol);  
#elif (NETWORK_SELECT == NETWORK_NTYCO)
	ntyco_start(port, kvs_filter_protocol); 
#elif (NETWORK_SELECT == NETWORK_PROACTOR)
	proactor_start(port, kvs_filter_protocol); //kvs_protocol
#endif
       
    // 保存数据并退出
    printf("Shutdown requested, saving data...\n");

	aof_fsync(1);


#if ENABLE_RDB
	kvs_rdb_save("kvstore.rdb");
#else
	kvs_save_all("kvstore.dat");
#endif
	//差分存储 第一次 第二次
	//保存cmd log 直接读命令 再次执行  
		//rdb 文件快照
		//aof 差分存储
    dest_kvengine();

    return 0;    
    
}
#endif


