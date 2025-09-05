#include "kvstore.h"


const char *command[] = {
	"SET", "GET", "DEL", "MOD", "EXIST","RANGE","SORT",
	"RSET", "RGET", "RDEL", "RMOD", "REXIST","RRANGE","RSORT",
	"HSET", "HGET", "HDEL", "HMOD", "HEXIST","HRANGE","HSORT",
	"SSET", "SGET", "SDEL", "SMOD", "SEXIST","SRANGE","SSORT",
	"QUIT",
};

#if 0
const command_info command_table[] = {
    //数组
    {"SET", (command_handler)handle_set,  3, 1, DATA_STRUCTURE_ARRAY},
    {"GET", (command_handler)handle_set,  2, 0, DATA_STRUCTURE_ARRAY},
    {"DEL", (command_handler)handle_set,  2, 1, DATA_STRUCTURE_ARRAY},
    {"MOD", (command_handler)handle_set,  3, 1, DATA_STRUCTURE_ARRAY},
    {"EXIST",  (command_handler)handle_exist,  2, 0, DATA_STRUCTURE_ARRAY},
    {"RANGE",  (command_handler)handle_range,  3, 0, DATA_STRUCTURE_ARRAY},
    {"SORT",   (command_handler)handle_sort,  -2, 0, DATA_STRUCTURE_ARRAY}, // 至少需要2个参数

    // RBTree commands
    {"RSET",   (command_handler)handle_set,    3, 1, DATA_STRUCTURE_RBTREE},
    {"RGET",   (command_handler)handle_get,    2, 0, DATA_STRUCTURE_RBTREE},
    {"RDEL",   (command_handler)handle_del,    2, 1, DATA_STRUCTURE_RBTREE},
    {"RMOD",   (command_handler)handle_mod,    3, 1, DATA_STRUCTURE_RBTREE},
    {"REXIST", (command_handler)handle_exist,  2, 0, DATA_STRUCTURE_RBTREE},
    {"RRANGE", (command_handler)handle_range,  3, 0, DATA_STRUCTURE_RBTREE},
    {"RSORT",  (command_handler)handle_sort,  -2, 0, DATA_STRUCTURE_RBTREE},

    // Hash commands
    {"HSET",   (command_handler)handle_set,    3, 1, DATA_STRUCTURE_HASH},
    {"HGET",   (command_handler)handle_get,    2, 0, DATA_STRUCTURE_HASH},
    {"HDEL",   (command_handler)handle_del,    2, 1, DATA_STRUCTURE_HASH},
    {"HMOD",   (command_handler)handle_mod,    3, 1, DATA_STRUCTURE_HASH},
    {"HEXIST", (command_handler)handle_exist,  2, 0, DATA_STRUCTURE_HASH},
    {"HRANGE", (command_handler)handle_range,  3, 0, DATA_STRUCTURE_HASH},
    {"HSORT",  (command_handler)handle_sort,  -2, 0, DATA_STRUCTURE_HASH},

    // SkipTable commands
    {"SSET",   (command_handler)handle_set,    3, 1, DATA_STRUCTURE_SKIPTABLE},
    {"SGET",   (command_handler)handle_get,    2, 0, DATA_STRUCTURE_SKIPTABLE},
    {"SDEL",   (command_handler)handle_del,    2, 1, DATA_STRUCTURE_SKIPTABLE},
    {"SMOD",   (command_handler)handle_mod,    3, 1, DATA_STRUCTURE_SKIPTABLE},
    {"SEXIST", (command_handler)handle_exist,  2, 0, DATA_STRUCTURE_SKIPTABLE},
    {"SRANGE", (command_handler)handle_range,  3, 0, DATA_STRUCTURE_SKIPTABLE},
    {"SSORT",  (command_handler)handle_sort,  -2, 0, DATA_STRUCTURE_SKIPTABLE},

    // Special commands
    {"QUIT",   (command_handler)handle_quit,   1, 0, -1},

    {NULL, NULL, 0, 0, -1} // 结束标记
};
#else
const command_info command_table[] = {
    //数组
    {"SET", handle_set,  3, 1, DATA_STRUCTURE_ARRAY},
    {"GET", handle_get,  2, 0, DATA_STRUCTURE_ARRAY},
    {"DEL", handle_del,  2, 1, DATA_STRUCTURE_ARRAY},
    {"MOD", handle_mod,  3, 1, DATA_STRUCTURE_ARRAY},
    {"EXIST",  handle_exist,  2, 0, DATA_STRUCTURE_ARRAY},
    {"RANGE",  handle_range,  3, 0, DATA_STRUCTURE_ARRAY},
    {"SORT",   handle_sort,  -1, 0, DATA_STRUCTURE_ARRAY}, // 至少需要2个参数

    // RBTree commands
    {"RSET",   handle_set,    3, 1, DATA_STRUCTURE_RBTREE},
    {"RGET",   handle_get,    2, 0, DATA_STRUCTURE_RBTREE},
    {"RDEL",   handle_del,    2, 1, DATA_STRUCTURE_RBTREE},
    {"RMOD",   handle_mod,    3, 1, DATA_STRUCTURE_RBTREE},
    {"REXIST", handle_exist,  2, 0, DATA_STRUCTURE_RBTREE},
    {"RRANGE", handle_range,  3, 0, DATA_STRUCTURE_RBTREE},
    {"RSORT",  handle_sort,  -1, 0, DATA_STRUCTURE_RBTREE},

    // Hash commands
    {"HSET",   handle_set,    3, 1, DATA_STRUCTURE_HASH},
    {"HGET",   handle_get,    2, 0, DATA_STRUCTURE_HASH},
    {"HDEL",   handle_del,    2, 1, DATA_STRUCTURE_HASH},
    {"HMOD",   handle_mod,    3, 1, DATA_STRUCTURE_HASH},
    {"HEXIST", handle_exist,  2, 0, DATA_STRUCTURE_HASH},
    {"HRANGE", handle_range,  3, 0, DATA_STRUCTURE_HASH},
    {"HSORT",  handle_sort,  -1, 0, DATA_STRUCTURE_HASH},

    // SkipTable commands
    {"SSET",   handle_set,    3, 1, DATA_STRUCTURE_SKIPTABLE},
    {"SGET",   handle_get,    2, 0, DATA_STRUCTURE_SKIPTABLE},
    {"SDEL",   handle_del,    2, 1, DATA_STRUCTURE_SKIPTABLE},
    {"SMOD",   handle_mod,    3, 1, DATA_STRUCTURE_SKIPTABLE},
    {"SEXIST", handle_exist,  2, 0, DATA_STRUCTURE_SKIPTABLE},
    {"SRANGE", handle_range,  3, 0, DATA_STRUCTURE_SKIPTABLE},
    {"SSORT",  handle_sort,  -1, 0, DATA_STRUCTURE_SKIPTABLE},

    // Special commands
    {"QUIT",   handle_quit,   1, 0, -1},

    {NULL, NULL, 0, 0, -1} // 结束标记
};
#endif

//获取命令索引
int get_command_index(const char *name) {
    for (int i = 0; command_table[i].name != NULL; i++) {
        if (strcmp(command_table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

//多行响应转换
int convert_multiline_to_resp_array(char *multiline_response, char *response, int max_len){
	//将多行响应转换为RESP数组
	char *response_copy = strdup(multiline_response);
	if (!response_copy) return -1;

	char temp[KVS_MAX_RESPONSE_SIZE] = {0};
	int line_count = 0;
	int pos = 0;

	char *line = strtok(response_copy, "\r\n");

	//计算行数
	while(line != NULL){
		line_count++;
		line = strtok(NULL, "\r\n");
	}

    free(response_copy);
    response_copy = strdup(multiline_response);
    if (!response_copy) return -1;

	//生成数组头部
	pos += sprintf(temp + pos, "*%d\r\n", line_count);//表示有多少行

    line = strtok(response_copy, "\r\n");
	for(int i = 0;i < line_count; i++){
		pos += sprintf(temp + pos, "$%zu\r\n%s\r\n", strlen(line), line);
		line = strtok(NULL, "\r\n");
	}

	memcpy(response, temp, pos);
    free(response_copy);
	return pos;

}

//解析命令参数到上下文
int parse_command_argument(int cmd, char **tokens, int count, command_context_t *ctx){
    memset(ctx, 0, sizeof(command_context_t));

    //设置数据结构类型
    if(cmd >= KVS_CMD_SET && cmd <= KVS_CMD_SORT ){
        ctx->data_structure = DATA_STRUCTURE_ARRAY;
    }else if(cmd >= KVS_CMD_RSET && cmd <= KVS_CMD_RSORT){
        ctx->data_structure = DATA_STRUCTURE_RBTREE;
    }else if(cmd >= KVS_CMD_HSET && cmd <= KVS_CMD_HSORT){
        ctx->data_structure = DATA_STRUCTURE_HASH;
    }else if(cmd >= KVS_CMD_SSET && cmd <= KVS_CMD_SSORT){
        ctx->data_structure = DATA_STRUCTURE_SKIPTABLE;
    }

    //解析通用参数
    if (count > 1) ctx->key = tokens[1];
    if (count > 2) ctx->value = tokens[2];

    //解析特定命令的参数
    switch(cmd){
        case KVS_CMD_SORT:
        case KVS_CMD_RSORT:
        case KVS_CMD_HSORT:
        case KVS_CMD_SSORT:
            //设置默认值
            ctx->order = 0;
            ctx->limit = -1;

            //解析参数
            if (count >= 2) {
                if (strcmp(tokens[1], "high") == 0) {
                    ctx->order = 1; // 降序
                } else if (strcmp(tokens[1], "low") == 0) {
                    ctx->order = 0; // 升序
                } else {
                    // 如果不是 high/low，尝试解析为限制数量
                    ctx->limit = atoi(tokens[1]);
                    if (ctx->limit <= 0) ctx->limit = -1;
                }
                
                // 处理第二个参数
                if (count >= 3) {
                    ctx->limit = atoi(tokens[2]);
                    if (ctx->limit <= 0) ctx->limit = -1;
                }
            }
            break;

        case KVS_CMD_RANGE:
        case KVS_CMD_RRANGE:
        case KVS_CMD_HRANGE:
        case KVS_CMD_SRANGE:
            if (count >= 3) {
                ctx->start_key = tokens[1];
                ctx->end_key = tokens[2];
            }
            break;
    }

    return 0;
}

//查找命令
const command_info *lookup_command(const char *name){
    for(int i = 0; command_table[i].name != NULL;i++){
        if(strcmp(command_table[i].name, name) == 0){
            return &command_table[i];
        }
    }

    return NULL;
}

//检查参数
int check_arity(const command_info *cmd, int argc){
    if(cmd->arity >= 0) return argc == cmd->arity;
    else{
        //至少需要 |arity|个参数
        int min_args = -cmd->arity;

        //对于某些命令 还需检查最大参数数量
        if(strcmp(cmd->name, "SORT") == 0 || strcmp(cmd->name, "RSORT") == 0 || strcmp(cmd->name, "HSORT") == 0 || strcmp(cmd->name, "SSORT") == 0){
            //SORT 命令最多接受3个参数
            return argc >= min_args && argc <= 3;
        }

        //其他命令只需要满足最小参数要求
        return argc >= min_args;
    }
}


// 声明所有命令处理函数
int handle_set(command_context_t *ctx, char *response){
    int ret = -1;

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_set(&global_array, ctx->key, ctx->value);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_set(&global_rbtree, ctx->key, ctx->value);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_set(&global_hash, ctx->key, ctx->value);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_set(&global_skiptable, ctx->key, ctx->value);
            break;
#endif
        default:
            return resp_generate_error(response, "Data structure not enabled");
    }

    if (ret < 0) {
        return resp_generate_error(response, "ERROR");
    } else if (ret == 0) {
        return resp_generate_simple_string(response, "OK");
    } else {
        return resp_generate_simple_string(response, "EXIST");
    }
    
}

int handle_get(command_context_t *ctx, char *response){
    char *result = NULL;

    switch (ctx->data_structure) {
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            result = kvs_array_get(&global_array, ctx->key);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            result = kvs_rbtree_get(&global_rbtree, ctx->key);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            result = kvs_hash_get(&global_hash, ctx->key);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            result = kvs_skiptable_get(&global_skiptable, ctx->key);
            break;
#endif
        default:
            return resp_generate_error(response, "Data structure not enabled");
    }

    if (result == NULL) {
        return resp_generate_simple_string(response, "NO EXIST");
    } else {
        return resp_generate_bulk_string(response, result);
    }
}
int handle_del(command_context_t *ctx, char *response){
    int ret = -1;

    switch (ctx->data_structure) {
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_del(&global_array, ctx->key);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_del(&global_rbtree, ctx->key);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_del(&global_hash, ctx->key);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_del(&global_skiptable, ctx->key);
            break;
#endif
        default:
            return resp_generate_error(response, "Data structure not enabled");
    }

    if (ret < 0) {
        return resp_generate_error(response, "ERROR");
    } else if (ret == 0) {
        return resp_generate_simple_string(response, "OK");
    } else {
        return resp_generate_simple_string(response, "NO EXIST");
    }
}
int handle_mod(command_context_t *ctx, char *response){
    int ret = -1;

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_mod(&global_array, ctx->key, ctx->value);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_mod(&global_rbtree, ctx->key, ctx->value);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_mod(&global_hash, ctx->key, ctx->value);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_mod(&global_skiptable, ctx->key, ctx->value);
            break;
#endif
        default:
            return resp_generate_error(response, "Data structure not enabled");
    }

    if (ret < 0) {
        return resp_generate_error(response, "ERROR");
    } else if (ret == 0) {
        return resp_generate_simple_string(response, "OK");
    } else {
        return resp_generate_simple_string(response, "NO EXIST");
    }
}
int handle_exist(command_context_t *ctx, char *response){
    int ret = -1;

    switch (ctx->data_structure) {
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_exist(&global_array, ctx->key);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_exist(&global_rbtree, ctx->key);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_exist(&global_hash, ctx->key);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_exist(&global_skiptable, ctx->key);
            break;
#endif
        default:
            return resp_generate_error(response, "Data structure not enabled");
    }

    if (ret == 0) {
        return resp_generate_simple_string(response, "EXIST");
    } else {
        return resp_generate_simple_string(response, "NO EXIST");
    }
}
int handle_range(command_context_t *ctx, char *response){
    int ret = -1;
    char temp_response[KVS_MAX_RESPONSE_SIZE] = {0};

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_range(&global_array, ctx->start_key, ctx->end_key, temp_response, sizeof(temp_response));
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_range(&global_rbtree, ctx->start_key, ctx->end_key, temp_response, sizeof(temp_response));
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_range(&global_hash, ctx->start_key, ctx->end_key, temp_response, sizeof(temp_response));
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_range(&global_skiptable, ctx->start_key, ctx->end_key, temp_response, sizeof(temp_response));
            break;
#endif
        default:
            return resp_generate_error(response, "Data structure not enabled");
    }

    if (ret <= 0) {
        return resp_generate_array_start(response, 0);
    } else {
        return convert_multiline_to_resp_array(temp_response, response, KVS_MAX_RESPONSE_SIZE);
    }
}
int handle_sort(command_context_t *ctx, char *response){
    int ret = -1;
    char temp_response[KVS_MAX_RESPONSE_SIZE] = {0};

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_sort(&global_array, ctx->order, ctx->limit, temp_response, sizeof(temp_response));
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_sort(&global_rbtree, ctx->order, ctx->limit, temp_response, sizeof(temp_response));
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_sort(&global_hash, ctx->order, ctx->limit, temp_response, sizeof(temp_response));
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_sort(&global_skiptable, ctx->order, ctx->limit, temp_response, sizeof(temp_response));
            break;
#endif       

        default:
            return resp_generate_error(response, "Data structure not enabled");
    }

    if (ret <= 0) {
        return resp_generate_array_start(response, 0);
    } else {
        return convert_multiline_to_resp_array(temp_response, response, KVS_MAX_RESPONSE_SIZE);
    }

}
int handle_quit(command_context_t *ctx, char *response){
    g_shutdown_flag = 1;
    return resp_generate_simple_string(response, "Server shutting down...");
}