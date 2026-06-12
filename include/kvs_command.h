#pragma once
#include "kvs_types.h"

enum {
	KVS_LOCK_MODE_WRITE = 0,
	KVS_LOCK_MODE_READ = 1,
};

#define KVS_CMD_RETRY_WITH_WRITE_LOCK (-3)

typedef enum{
	KVS_CMD_LOCK_NONE = 0,
	KVS_CMD_LOCK_READ,
	KVS_CMD_LOCK_WRITE
} kvs_cmd_lock_mode_t;

//命令上下文结构体
typedef struct {
	char *key;
	size_t key_len;
	char *value;
	size_t value_len;
	char *start_key;
	char *end_key;
	int order;
	int limit;
	int data_structure;
	int lock_mode;
	//V族命令所需字段
	char *scope;
	size_t scope_len;
	char* subcommand;
	size_t subcommand_len;
	int topk;
	float threshold;
	int has_threshold;
	char *query_text;
	size_t query_text_len;
} command_context_t;

typedef int (*command_handler)(command_context_t *ctx, char *response);


typedef struct {
	const char *name;
	command_handler handler;
	int arity;
	int write_command;
	int data_structure;
	kvs_cmd_lock_mode_t lock_mode;
} command_info;

// 写命令集合
extern const int write_commands[];

int get_command_index(const char *name);
const command_info *lookup_command(const char *name);
int check_arity(const command_info *cmd, int argc);
int parse_command_argument(int cmd, char **tokens, int count, size_t *token_lens, command_context_t *ctx);

// 命令处理函数
int handle_set(command_context_t *ctx, char *response);
int handle_get(command_context_t *ctx, char *response);
int handle_del(command_context_t *ctx, char *response);
int handle_mod(command_context_t *ctx, char *response);
int handle_exist(command_context_t *ctx, char *response);
int handle_range(command_context_t *ctx, char *response);
int handle_sort(command_context_t *ctx, char *response);
int handle_expire(command_context_t *ctx, char *response);
int handle_ttl(command_context_t *ctx, char *response);
int handle_persist(command_context_t *ctx, char *response);
int handle_pexpireat(command_context_t *ctx, char *response);
int handle_aofrewrite(command_context_t *ctx, char *response);
int handle_quit(command_context_t *ctx, char *response);
int handle_fulldump(command_context_t *ctx, char *response);

//V族命令
int handle_vset_command(command_context_t *ctx, char *response);
int handle_vget_command(command_context_t *ctx, char *response);
int handle_vsearch_command(command_context_t *ctx, char *response);
int handle_vdel_command(command_context_t *ctx, char *response);
int handle_vindex_command(command_context_t *ctx, char *response);
int handle_vinfo_command(command_context_t *ctx, char *response);

int ttl_get_any_key_expire_at_ms(const char *key, size_t key_len, int64_t *expire_at_ms);
int ttl_set_any_key_expire_at_ms(const char *key, size_t key_len, int64_t expire_at_ms);

extern const command_info command_table[];
