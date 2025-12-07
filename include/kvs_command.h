#pragma once
#include "kvs_types.h"

typedef struct {
	char *key;
	char *value;
	char *start_key;
	char *end_key;
	int order;
	int limit;
	int data_structure;
} command_context_t;

typedef int (*command_handler)(command_context_t *ctx, char *response);

typedef struct {
	const char *name;
	command_handler handler;
	int arity;
	int write_command;
	int data_structure;
} command_info;

// 写命令集合
extern const int write_commands[];

int get_command_index(const char *name);
const command_info *lookup_command(const char *name);
int check_arity(const command_info *cmd, int argc);
int parse_command_argument(int cmd, char **tokens, int count, command_context_t *ctx);

// 命令处理函数
int handle_set(command_context_t *ctx, char *response);
int handle_get(command_context_t *ctx, char *response);
int handle_del(command_context_t *ctx, char *response);
int handle_mod(command_context_t *ctx, char *response);
int handle_exist(command_context_t *ctx, char *response);
int handle_range(command_context_t *ctx, char *response);
int handle_sort(command_context_t *ctx, char *response);
int handle_quit(command_context_t *ctx, char *response);
int handle_fulldump(command_context_t *ctx, char *response);

extern const command_info command_table[];

