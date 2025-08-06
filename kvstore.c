



#include "kvstore.h"

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


void *kvs_malloc(size_t size) {
	return malloc(size);
}

void kvs_free(void *ptr) {
	return free(ptr);
}


const char *command[] = {
	"SET", "GET", "DEL", "MOD", "EXIST","RANGE","SORT",
	"RSET", "RGET", "RDEL", "RMOD", "REXIST","RRANGE","RSORT",
	"HSET", "HGET", "HDEL", "HMOD", "HEXIST","HRANGE","HSORT",
	"SSET", "SGET", "SDEL", "SMOD", "SEXIST","SRANGE","SSORT",
	"QUIT",
};

enum {
	KVS_CMD_START = 0,
	// array
	KVS_CMD_SET = KVS_CMD_START,
	KVS_CMD_GET,
	KVS_CMD_DEL,
	KVS_CMD_MOD,
	KVS_CMD_EXIST,
	KVS_CMD_RANGE,
	KVS_CMD_SORT,
	// rbtree
	KVS_CMD_RSET,
	KVS_CMD_RGET,
	KVS_CMD_RDEL,
	KVS_CMD_RMOD,
	KVS_CMD_REXIST,
	KVS_CMD_RRANGE,
	KVS_CMD_RSORT,
	// hash
	KVS_CMD_HSET,
	KVS_CMD_HGET,
	KVS_CMD_HDEL,
	KVS_CMD_HMOD,
	KVS_CMD_HEXIST,
	KVS_CMD_HRANGE,
	KVS_CMD_HSORT,
	//skiptable
	KVS_CMD_SSET,
    KVS_CMD_SGET,
    KVS_CMD_SDEL,
    KVS_CMD_SMOD,
    KVS_CMD_SEXIST,
	KVS_CMD_SRANGE,
	KVS_CMD_SSORT,
	
	//退出命令
	KVS_CMD_QUIT,

	KVS_CMD_COUNT,
};

//保存与加载文件
//检查文件是否存在
static int file_exists(const char *filename){

	struct stat buffer;
	return (stat(filename, &buffer) == 0);

}

//保存所有数据到文件
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




// const char *response[] = {

// };


int kvs_split_token(char *msg, char *tokens[]) {

	if (msg == NULL || tokens == NULL) return -1;

	int idx = 0;
	char *token = strtok(msg, " ");
	
	while (token != NULL) {
		//printf("idx: %d, %s\n", idx, token);
		
		tokens[idx ++] = token;
		token = strtok(NULL, " ");
	}

	return idx;
}


// SET Key Value
// tokens[0] : SET
// tokens[1] : Key
// tokens[2] : Value

int kvs_filter_protocol(char **tokens, int count, char *response) {

	if (tokens[0] == NULL || count == 0 || response == NULL) return -1;

	int cmd = KVS_CMD_START;
	for (cmd = KVS_CMD_START;cmd < KVS_CMD_COUNT;cmd ++) {
		if (strcmp(tokens[0], command[cmd]) == 0) {
			break;
		} 
	}

	int length = 0;
	int ret = 0;
	char *key = tokens[1];
	char *value = tokens[2];
	char *result = {0};

	switch(cmd) {
#if ENABLE_ARRAY
	case KVS_CMD_SET:
		ret = kvs_array_set(&global_array ,key, value);
		if (ret < 0) {
			length = sprintf(response, "ERROR\r\n");
		} else if (ret == 0) {
			length = sprintf(response, "OK\r\n");
		} else {
			length = sprintf(response, "EXIST\r\n");
		} 
		
		break;
	case KVS_CMD_GET: {
		char *result = kvs_array_get(&global_array, key);
		if (result == NULL) {
			length = sprintf(response, "NO EXIST\r\n");
		} else {
			length = sprintf(response, "%s\r\n", result);
		}
		break;
	}
	case KVS_CMD_DEL:
		ret = kvs_array_del(&global_array ,key);
		if (ret < 0) {
			length = sprintf(response, "ERROR\r\n");
 		} else if (ret == 0) {
			length = sprintf(response, "OK\r\n");
		} else {
			length = sprintf(response, "NO EXIST\r\n");
		}
		break;
	case KVS_CMD_MOD:
		ret = kvs_array_mod(&global_array ,key, value);
		if (ret < 0) {
			length = sprintf(response, "ERROR\r\n");
 		} else if (ret == 0) {
			length = sprintf(response, "OK\r\n");
		} else {
			length = sprintf(response, "NO EXIST\r\n");
		}
		break;
	case KVS_CMD_EXIST:
		ret = kvs_array_exist(&global_array ,key);
		if (ret == 0) {
			length = sprintf(response, "EXIST\r\n");
		} else {
			length = sprintf(response, "NO EXIST\r\n");
		}
		break;
	case KVS_CMD_SORT:
		length = kvs_array_sort(&global_array, response, KVS_MAX_RESPONSE_SIZE);
		if(length < 0){
			length = sprintf(response, "EMPTY\r\n");
			break;
		}
		break;
	case KVS_CMD_RANGE:
		if(count < 3){
			length = sprintf(response, "ERROR: Missing range parameters\r\n");
			break;
		}
		length = kvs_array_range(&global_array, tokens[1], tokens[2], response, KVS_MAX_RESPONSE_SIZE);
		if(length <= 0){
			length = sprintf(response, "EMPTY\r\n");
		}
		break;
#endif
	// rbtree
#if ENABLE_RBTREE
	case KVS_CMD_RSET:
		ret = kvs_rbtree_set(&global_rbtree ,key, value);
		if (ret < 0) {
			length = sprintf(response, "ERROR\r\n");
		} else if (ret == 0) {
			length = sprintf(response, "OK\r\n");
		} else {
			length = sprintf(response, "EXIST\r\n");
		} 
		
		break;
	case KVS_CMD_RGET: {
		char *result = kvs_rbtree_get(&global_rbtree, key);
		if (result == NULL) {
			length = sprintf(response, "NO EXIST\r\n");
		} else {
			length = sprintf(response, "%s\r\n", result);
		}
		break;
	}
	case KVS_CMD_RDEL:
		ret = kvs_rbtree_del(&global_rbtree ,key);
		if (ret < 0) {
			length = sprintf(response, "ERROR\r\n");
 		} else if (ret == 0) {
			length = sprintf(response, "OK\r\n");
		} else {
			length = sprintf(response, "NO EXIST\r\n");
		}
		break;
	case KVS_CMD_RMOD:
		ret = kvs_rbtree_mod(&global_rbtree ,key, value);
		if (ret < 0) {
			length = sprintf(response, "ERROR\r\n");
 		} else if (ret == 0) {
			length = sprintf(response, "OK\r\n");
		} else {
			length = sprintf(response, "NO EXIST\r\n");
		}
		break;
	case KVS_CMD_REXIST:
		ret = kvs_rbtree_exist(&global_rbtree ,key);
		if (ret == 0) {
			length = sprintf(response, "EXIST\r\n");
		} else {
			length = sprintf(response, "NO EXIST\r\n");
		}
		break;
	case KVS_CMD_RSORT:
		length = kvs_rbtree_sort(&global_rbtree, response, KVS_MAX_RESPONSE_SIZE);
		if(length <= 0){
			length = sprintf(response, "EMPTY\r\n");
		}
		break;
	case KVS_CMD_RRANGE:
		if(count < 3){
			length = sprintf(response, "ERROR: Missing range parameters\r\n");
            break;
		}
		length = kvs_rbtree_range(&global_rbtree, tokens[1], tokens[2], response, KVS_MAX_RESPONSE_SIZE);
		if (length <= 0) {
			length = sprintf(response, "EMPTY\r\n");
		}
		break;
#endif
#if ENABLE_HASH
	case KVS_CMD_HSET:
		ret = kvs_hash_set(&global_hash ,key, value);
		if (ret < 0) {
			length = sprintf(response, "ERROR\r\n");
		} else if (ret == 0) {
			length = sprintf(response, "OK\r\n");
		} else {
			length = sprintf(response, "EXIST\r\n");
		} 
		
		break;
	case KVS_CMD_HGET: {
		char *result = kvs_hash_get(&global_hash, key);
		if (result == NULL) {
			length = sprintf(response, "NO EXIST\r\n");
		} else {
			length = sprintf(response, "%s\r\n", result);
		}
		break;
	}
	case KVS_CMD_HDEL:
		ret = kvs_hash_del(&global_hash ,key);
		if (ret < 0) {
			length = sprintf(response, "ERROR\r\n");
 		} else if (ret == 0) {
			length = sprintf(response, "OK\r\n");
		} else {
			length = sprintf(response, "NO EXIST\r\n");
		}
		break;
	case KVS_CMD_HMOD:
		ret = kvs_hash_mod(&global_hash ,key, value);
		if (ret < 0) {
			length = sprintf(response, "ERROR\r\n");
 		} else if (ret == 0) {
			length = sprintf(response, "OK\r\n");
		} else {
			length = sprintf(response, "NO EXIST\r\n");
		}
		break;
	case KVS_CMD_HEXIST:
		ret = kvs_hash_exist(&global_hash ,key);
		if (ret == 0) {
			length = sprintf(response, "EXIST\r\n");
		} else {
			length = sprintf(response, "NO EXIST\r\n");
		}
		break;
	case KVS_CMD_HSORT:
		length = kvs_hash_sort(&global_hash, response, KVS_MAX_RESPONSE_SIZE);
		if (length <= 0) {
			length = sprintf(response, "EMPTY\r\n");
		}
		break;
	case KVS_CMD_HRANGE:
		if (count < 3) {
			length = sprintf(response, "ERROR: Missing range parameters\r\n");
			break;
		}
		length = kvs_hash_range(&global_hash, tokens[1], tokens[2], response, KVS_MAX_RESPONSE_SIZE);
		if (length <= 0) {
			length = sprintf(response, "EMPTY\r\n");
		}
		break;
#endif
#if ENABLE_SKIPTABLE
		//hash
	case KVS_CMD_SSET:{
		ret = kvs_skiptable_set(&global_skiptable, key, value);
		if(ret < 0){
			length = sprintf(response,"ERROR\r\n");
		}else if(ret == 0){
			length = sprintf(response,"OK\r\n");
		}else{
			length = sprintf(response,"EXIST\r\n");
		}
		break;
	}
	case KVS_CMD_SGET:
		result = kvs_skiptable_get(&global_skiptable, key);
		if(result == NULL){
			length = sprintf(response,"NO EXIST\r\n");
		}else{
			length = sprintf(response,"%s\r\n", result);
		}
		break;
	case KVS_CMD_SDEL:
		ret = kvs_skiptable_del(&global_skiptable, key);
		if(ret < 0){
			length = sprintf(response,"ERROR\r\n");
		}else if(ret == 0){
			length = sprintf(response,"OK\r\n");
		}else{
			length = sprintf(response,"NO EXIST\r\n");
		}
		break;
	case KVS_CMD_SMOD:
		ret = kvs_skiptable_mod(&global_skiptable, key, value);
		if(ret < 0){
			length = sprintf(response,"ERROR\r\n");
		}else if(ret == 0){
			length = sprintf(response,"OK\r\n");
		}else{
			length = sprintf(response,"NO EXIST\r\n");
		}
		break;
	case KVS_CMD_SEXIST:
		ret = kvs_skiptable_exist(&global_skiptable, key);
		if(ret == 0){
			length = sprintf(response,"EXIST\r\n");
		}else {
			length = sprintf(response,"NO EXIST\r\n");
		}
		break;
	case KVS_CMD_SSORT:
		length = kvs_skiptable_sort(&global_skiptable, response, KVS_MAX_RESPONSE_SIZE);
		if (length <= 0) {
			length = sprintf(response, "EMPTY\r\n");
		}
		break;
	case KVS_CMD_SRANGE:
		if (count < 3) {
			length = sprintf(response, "ERROR: Missing range parameters\r\n");
			break;
		}
		length = kvs_skiptable_range(&global_skiptable, tokens[1], tokens[2], response, KVS_MAX_RESPONSE_SIZE);
		if (length <= 0) {
			length = sprintf(response, "EMPTY\r\n");
		}
		break;
#endif
	case KVS_CMD_QUIT:
            // 设置关闭标志
            g_shutdown_flag = 1;
            length = sprintf(response, "Server shutting down...\r\n");
            break;
			
	default: 
		assert(0);
	}

	return length;
}


/*
 * msg: request message
 * length: length of request message
 * response: need to send
 * @return : length of response
 */

int kvs_protocol(char *msg, int length, char *response) {  //
	
// SET Key Value
// GET Key
// DEL Key
	if (msg == NULL || length <= 0 || response == NULL) return -1;

	//printf("recv %d : %s\n", length, msg);

	char *tokens[KVS_MAX_TOKENS] = {0};

	int count = kvs_split_token(msg, tokens);
	if (count == -1) return -1;

	//memcpy(response, msg, length);

	return kvs_filter_protocol(tokens, count, response);
}


int init_kvengine(void) {

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
}

// 信号处理函数
void handle_signal(int sig) {
    printf("\nReceived signal %d, saving data before shutdown...\n", sig);
    kvs_save_all("kvstore.dat");
    exit(0);
}

int main(int argc, char *argv[]) {

	if (argc != 2) return -1;

	int port = atoi(argv[1]);

	// 注册信号处理函数，使Ctrl+C也能保存数据
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

	init_kvengine();
	
	// 加载持久化数据
    kvs_load_all("kvstore.dat");
	
#if (NETWORK_SELECT == NETWORK_REACTOR)
	reactor_start(port, kvs_protocol);  //
#elif (NETWORK_SELECT == NETWORK_PROACTOR)
	ntyco_start(port, kvs_protocol);
#elif (NETWORK_SELECT == NETWORK_NTYCO)
	proactor_start(port, kvs_protocol);
#endif

	// 主循环，检查关闭标志
    while (!g_shutdown_flag) {
        // 根据不同网络模型实现等待
        #if (NETWORK_SELECT == NETWORK_REACTOR)
            // 简单实现：等待1秒
            sleep(1);
        #elif (NETWORK_SELECT == NETWORK_PROACTOR)
            sleep(1);
        #elif (NETWORK_SELECT == NETWORK_NTYCO)
            sleep(1);
        #endif
        
        // 检查关闭标志
        if (g_shutdown_flag) {
            printf("Shutdown requested, saving data...\n");
            kvs_save_all("kvstore.dat");
            break;
        }
    }

}


