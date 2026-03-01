#pragma once
#include "kvs_types.h"
#include "kvs_memory.h"

typedef struct kvs_array_item_s {
	char *key;
	char *value;
} kvs_array_item_t;

#define KVS_ARRAY_SIZE 2048

typedef struct kvs_array_s {
	/* table: 存储槽位数组 */
	kvs_array_item_t *table;
	/* idx: 兼容保留字段（当前未使用） */
	int idx;
	/* total: 兼容保留，表示历史扫描上界（最后一个已使用槽位+1） */
	int total;
	/* len: 当前有效元素个数（key != NULL） */
	int len;
	/* capacity: 当前可用槽位总容量 */
	int capacity;
	/* min_capacity: 允许缩容的最小容量下限 */
	int min_capacity;
} kvs_array_t;

int kvs_array_create(kvs_array_t *inst);
void kvs_array_destory(kvs_array_t *inst);
int kvs_array_reserve(kvs_array_t *inst, int new_cap);

int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char* kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);
int kvs_array_sort(kvs_array_t *inst, int order, int limit, char *response, int max_len);
int kvs_array_range(kvs_array_t *inst, char *start_key, char *end_key, char *response, int max_len);

