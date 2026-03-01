#include "kvs_api.h"
#include "kvs_globals.h"
#include "resp_protocol.h"
#include "distributed.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>

extern aof_state_t aof_state;
extern distributed_config_t global_dist_config;

//四个数据结构
extern kvs_array_t global_array;
extern kvs_rbtree_t global_rbtree;
extern kvs_hash_t global_hash;
extern kvs_skiptable_t global_skiptable;

//rbtree回调遍历函数
void rbtree_traversal_with_callback(rbtree *T, rbtree_node *node,
                                    void (*callback)(rbtree_node *, void *),
                                    void *userdata);
const char *command[] = {
    "SET", "GET", "DEL", "MOD", "EXIST","RANGE","SORT",
    "RSET", "RGET", "RDEL", "RMOD", "REXIST","RRANGE","RSORT",
    "HSET", "HGET", "HDEL", "HMOD", "HEXIST","HRANGE","HSORT",
    "SSET", "SGET", "SDEL", "SMOD", "SEXIST","SRANGE","SSORT",
    "PAPERSET", "PAPERGET", "PAPERDEL",
    "QUIT",
    "FULLDUMP",
};

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
    {"RRANGE", handle_range, -1, 0, DATA_STRUCTURE_RBTREE},
    {"RSORT",  handle_sort,  -1, 0, DATA_STRUCTURE_RBTREE},

    // Hash commands）
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
    {"SRANGE", handle_range, -1, 0, DATA_STRUCTURE_SKIPTABLE},
    {"SSORT",  handle_sort,  -1, 0, DATA_STRUCTURE_SKIPTABLE},

    // Paper commands (use RBTree for large values)
    {"PAPERSET", handle_set,  3, 1, DATA_STRUCTURE_RBTREE},
    {"PAPERGET", handle_get,  2, 0, DATA_STRUCTURE_RBTREE},
    {"PAPERDEL", handle_del,  2, 1, DATA_STRUCTURE_RBTREE},
    {"PAPERMOD", handle_mod,  3, 1, DATA_STRUCTURE_RBTREE},

    // Special commands
    {"QUIT",   handle_quit,   1, 0, -1},
    {"FULLDUMP", handle_fulldump, 1, 0, -1},

    {NULL, NULL, 0, 0, -1} // 结束标记
};


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
    // 统一限制最大 RESP 长度
    if (max_len > (int)KVS_MAX_RESPONSE_LENGTH) {
        max_len = (int)KVS_MAX_RESPONSE_LENGTH;
    }

    //将多行响应转换为RESP数组，使用调用方提供的缓冲区并做长度检查
    char *response_copy = strdup(multiline_response);
    if (!response_copy) return -1;

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
    int n = snprintf(response + pos, max_len - pos, "*%d\r\n", line_count);
    if (n < 0 || n >= max_len - pos) {
        free(response_copy);
        return -1;
    }
    pos += n;

    line = strtok(response_copy, "\r\n");
    for(int i = 0;i < line_count; i++){
        if (!line) break;
        int need = snprintf(response + pos, max_len - pos, "$%zu\r\n%s\r\n", strlen(line), line);
        if (need < 0 || need >= max_len - pos) {
            free(response_copy);
            return -1;
        }
        pos += need;
        line = strtok(NULL, "\r\n");
    }

    free(response_copy);
    return pos;

}

//解析命令参数到上下文
int parse_command_argument(int cmd, char **tokens, int count, command_context_t *ctx){
    memset(ctx, 0, sizeof(command_context_t));
    ctx->limit = -1;

    //设置数据结构类型
    if(cmd >= KVS_CMD_SET && cmd <= KVS_CMD_SORT ){
        ctx->data_structure = DATA_STRUCTURE_ARRAY;
    }else if(cmd >= KVS_CMD_RSET && cmd <= KVS_CMD_RSORT){
        ctx->data_structure = DATA_STRUCTURE_RBTREE;
    }else if(cmd >= KVS_CMD_HSET && cmd <= KVS_CMD_HSORT){
        ctx->data_structure = DATA_STRUCTURE_HASH;
    }else if(cmd >= KVS_CMD_SSET && cmd <= KVS_CMD_SSORT){
        ctx->data_structure = DATA_STRUCTURE_SKIPTABLE;
    }else if(cmd >= KVS_CMD_PAPERSET && cmd <= KVS_CMD_PAPERMOD){
        ctx->data_structure = DATA_STRUCTURE_RBTREE; // Paper 使用 RBTree 存储大 value
    }else if(cmd == KVS_CMD_QUIT){
        //QUIT命令不需要额外参数
        return 0;
    }else if(cmd == KVS_CMD_FULLDUMP){
        //FULLDUMP命令不需要额外参数
        return 0;
    }else{
        return -1; //未知命令 
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
        case KVS_CMD_HRANGE:
            if (count >= 3) {
                ctx->start_key = tokens[1];
                ctx->end_key = tokens[2];
            }
            break;

        case KVS_CMD_RRANGE:
        case KVS_CMD_SRANGE:
            if (count != 3 && count != 5) {
                return -1;
            }

            ctx->start_key = tokens[1];
            ctx->end_key = tokens[2];

            if (count == 5) {
                char *endptr = NULL;
                long parsed_limit;

                if (strcasecmp(tokens[3], "LIMIT") != 0) {
                    return -1;
                }

                errno = 0;
                parsed_limit = strtol(tokens[4], &endptr, 10);
                if (endptr == tokens[4] || *endptr != '\0') {
                    return -1;
                }

                if (errno == ERANGE && parsed_limit == LONG_MAX) {
                    parsed_limit = INT_MAX;
                } else if (errno != 0) {
                    return -1;
                }

                if (parsed_limit <= 0) {
                    return -1;
                }

                if (parsed_limit > INT_MAX) {
                    parsed_limit = INT_MAX;
                }

                ctx->limit = (int)parsed_limit;
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
    if(strcmp(cmd->name, "RRANGE") == 0 || strcmp(cmd->name, "SRANGE") == 0){
        return argc == 3 || argc == 5;
    }

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

    // 按配置限制单条 value 大小；0 表示不限制
    // if (g_cfg.max_value_size > 0 && ctx->value) {
    //     size_t vlen = strlen(ctx->value);
    //     if (vlen > g_cfg.max_value_size) {
    //         return resp_generate_error(response, "ERR value too large, check max_value_size");
    //     }
    // }

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_set(kvs_get_global_array(), ctx->key, ctx->value);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_set(kvs_get_global_rbtree(), ctx->key, ctx->value);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_set(kvs_get_global_hash(), ctx->key, ctx->value);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_set(kvs_get_global_skiptable(), ctx->key, ctx->value);
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
            result = kvs_array_get(kvs_get_global_array(), ctx->key);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            result = kvs_rbtree_get(kvs_get_global_rbtree(), ctx->key);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            result = kvs_hash_get(kvs_get_global_hash(), ctx->key);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            result = kvs_skiptable_get(kvs_get_global_skiptable(), ctx->key);
            break;
#endif
        default:
            return resp_generate_error(response, "Data structure not enabled");
    }

    if (result == NULL) {
        return resp_generate_simple_string(response, "NO EXIST");
    } else {
		// 防御性检查：单条 value 的 RESP 编码长度不能超过全局上限
		size_t est = resp_estimate_bulk_string_len(result);
		if (est > KVS_MAX_RESPONSE_LENGTH) {
			return resp_generate_error(response, "ERR value too large");
		}
        return resp_generate_bulk_string(response, result);
    }
}
int handle_del(command_context_t *ctx, char *response){
    int ret = -1;

    switch (ctx->data_structure) {
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_del(kvs_get_global_array(), ctx->key);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_del(kvs_get_global_rbtree(), ctx->key);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_del(kvs_get_global_hash(), ctx->key);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_del(kvs_get_global_skiptable(), ctx->key);
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

    // 按配置限制单条 value 大小；0 表示不限制
    // if (g_cfg.max_value_size > 0 && ctx->value) {
    //     size_t vlen = strlen(ctx->value);
    //     if (vlen > g_cfg.max_value_size) {
    //         return resp_generate_error(response, "ERR value too large, check max_value_size");
    //     }
    // }

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
            ret = kvs_array_mod(kvs_get_global_array(), ctx->key, ctx->value);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_mod(kvs_get_global_rbtree(), ctx->key, ctx->value);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_mod(kvs_get_global_hash(), ctx->key, ctx->value);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_mod(kvs_get_global_skiptable(), ctx->key, ctx->value);
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
            ret = kvs_array_exist(kvs_get_global_array(), ctx->key);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
            ret = kvs_rbtree_exist(kvs_get_global_rbtree(), ctx->key);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
            ret = kvs_hash_exist(kvs_get_global_hash(), ctx->key);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
            ret = kvs_skiptable_exist(kvs_get_global_skiptable(), ctx->key);
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
	char *temp_response = NULL;

    temp_response = kvs_malloc(KVS_MAX_RESPONSE_LENGTH);
    if (!temp_response) {
        return resp_generate_error(response, "ERROR");
    }
    temp_response[0] = '\0';

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
    			ret = kvs_array_range(kvs_get_global_array(), ctx->start_key, ctx->end_key, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
			ret = kvs_rbtree_range(kvs_get_global_rbtree(), ctx->start_key, ctx->end_key, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
    			ret = kvs_hash_range(kvs_get_global_hash(), ctx->start_key, ctx->end_key, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
		    	ret = kvs_skiptable_range(kvs_get_global_skiptable(), ctx->start_key, ctx->end_key, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
        default:
            kvs_free(temp_response);
            return resp_generate_error(response, "Data structure not enabled");
    }

    if (ret <= 0) {
        kvs_free(temp_response);
        return resp_generate_array_start(response, 0);
    } else {
		// 这里假定 temp_response 中的内容长度不会超过 KVS_MAX_RESPONSE_LENGTH
		int n = convert_multiline_to_resp_array(temp_response, response, KVS_MAX_RESPONSE_LENGTH);
		kvs_free(temp_response);
		return n;
    }
}
int handle_sort(command_context_t *ctx, char *response){
    int ret = -1;
	char *temp_response = NULL;

    temp_response = kvs_malloc(KVS_MAX_RESPONSE_LENGTH);
    if (!temp_response) {
        return resp_generate_error(response, "ERROR");
    }

    temp_response[0] = '\0';

    switch(ctx->data_structure){
#if ENABLE_ARRAY
        case DATA_STRUCTURE_ARRAY:
    			ret = kvs_array_sort(kvs_get_global_array(), ctx->order, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_RBTREE
        case DATA_STRUCTURE_RBTREE:
    			ret = kvs_rbtree_sort(kvs_get_global_rbtree(), ctx->order, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_HASH
        case DATA_STRUCTURE_HASH:
    			ret = kvs_hash_sort(kvs_get_global_hash(), ctx->order, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif
#if ENABLE_SKIPTABLE
        case DATA_STRUCTURE_SKIPTABLE:
    			ret = kvs_skiptable_sort(kvs_get_global_skiptable(), ctx->order, ctx->limit, temp_response, KVS_MAX_RESPONSE_LENGTH);
            break;
#endif       

        default:
        kvs_free(temp_response);
        return resp_generate_error(response, "Data structure not enabled");
    }

        if (ret <= 0) {
        kvs_free(temp_response);
        return resp_generate_array_start(response, 0);
        } else {
		int n = convert_multiline_to_resp_array(temp_response, response, KVS_MAX_RESPONSE_LENGTH);
		kvs_free(temp_response);
		return n;
        }

}
int handle_quit(command_context_t *ctx, char *response){
    g_shutdown_flag = 1;
    return resp_generate_simple_string(response, "Server shutting down...");
}

//安全地往 response[] 末尾追加数据
static int fulldump_append_raw(char* resp, 
                                int max_len, 
                                int* off,
                                const char* data, 
                                int len)
{
    if(*off + len > max_len){
        return -1; // 超出缓冲区
    }
    memcpy(resp + *off, data, len);
    *off += len;
    return 0;
}
// 追加 "*<n>\r\n"
static int fulldump_append_array_header(char *resp, 
                                        int max_len, 
                                        int *off, 
                                        int count) 
{

    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "*%d\r\n", count);
    if (n < 0) return -1;
    return fulldump_append_raw(resp, max_len, off, tmp, n);
}

// 追加 "$<len>\r\n<buf>\r\n"
static int fulldump_append_bulk(char *resp, int max_len, int *off,
                                const char *data, int len) {
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "$%d\r\n", len);
    if (n <= 0) return -1;
    if (fulldump_append_raw(resp, max_len, off, tmp, n) < 0) return -1;
    if (fulldump_append_raw(resp, max_len, off, data, len) < 0) return -1;
    if (fulldump_append_raw(resp, max_len, off, "\r\n", 2) < 0) return -1;
    return 0;
}

// 追加一条简单的 3 参数命令：CMD key value
static int fulldump_append_cmd3(char *resp, int max_len, int *off,
                                const char *cmd,
                                const char *key, const char *val) {
    int cmdlen = (int)strlen(cmd);
    int klen   = (int)strlen(key);
    int vlen   = (int)strlen(val);

    if (fulldump_append_array_header(resp, max_len, off, 3) < 0) return -1;
    if (fulldump_append_bulk(resp, max_len, off, cmd, cmdlen)   < 0) return -1;
    if (fulldump_append_bulk(resp, max_len, off, key, klen)     < 0) return -1;
    if (fulldump_append_bulk(resp, max_len, off, val, vlen)     < 0) return -1;
    return 0;
}

int handle_fulldump(command_context_t* ctx, char* response){
    (void)ctx;

    //只允许在主节点执行
    if(global_dist_config.role != ROLE_MASTER){
        return resp_generate_error(response, "ERR FULLDUMP allowed only on master node");
    }

    int off = 0;
    const int max_len = KVS_MAX_RESPONSE_LENGTH;

    //遍历生成
#if ENABLE_ARRAY
    {
        extern kvs_array_t global_array; // 已在头部声明，这里只是强调一下
        if (global_array.table && global_array.total > 0) {
            for (int i = 0; i < global_array.total; i++) {
                kvs_array_item_t *it = &global_array.table[i];
                if (!it->key || !it->value) continue;

                if (fulldump_append_cmd3(response, max_len, &off,
                                         "SET", it->key, it->value) < 0) {
                    // 缓冲区满了，直接停止，返回已填充长度
                    return off;
                }
            }
        }
    }
#endif
#if ENABLE_HASH
    {
        extern kvs_hash_t global_hash;
        if (global_hash.nodes && global_hash.max_slots > 0) {
            for (int i = 0; i < global_hash.max_slots; ++i) {
                hashnode_t *slot = &global_hash.nodes[i];
                if (slot->used != 1 || !slot->key || !slot->value) {
                    continue;
                }
                if (fulldump_append_cmd3(response, max_len, &off,
                                         "HSET",
                                         slot->key,
                                         slot->value) < 0) {
                    return off;
                }
            }
        }
    }
#endif
#if ENABLE_RBTREE
    struct rbtree_dump_ctx {
        char *resp;
        int   max_len;
        int  *off;
        int   overflow;
    } rbt_ctx = { response, max_len, &off, 0 };

    void rbtree_dump_cb(rbtree_node *node, void *ud) {
        struct rbtree_dump_ctx *c = (struct rbtree_dump_ctx *)ud;
        if (c->overflow) return;
        if (!node->key || !node->value) return;

        if (fulldump_append_cmd3(c->resp, c->max_len, c->off,
                                 "RSET",
                                 (const char *)node->key,
                                 (const char *)node->value) < 0) {
            c->overflow = 1;
        }
    }

    rbtree_traversal_with_callback((rbtree *)&global_rbtree,
                                   global_rbtree.root,
                                   rbtree_dump_cb,
                                   &rbt_ctx);
    if (rbt_ctx.overflow) {
        return off;
    }
#endif
#if ENABLE_SKIPTABLE
    {
        extern kvs_skiptable_t global_skiptable;
        if (global_skiptable.header) {
            Node *cur = global_skiptable.header->forward[0];
            while (cur) {
                if (cur->key && cur->value) {
                    if (fulldump_append_cmd3(response, max_len, &off,
                                             "SSET",
                                             cur->key,
                                             cur->value) < 0) {
                        return off;
                    }
                }
                cur = cur->forward[0];
            }
        }
    }
#endif

    if(off == 0){
        //没有数据，返回空数组
        return resp_generate_array_start(response, 0);
    }

    return off;
}