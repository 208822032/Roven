#pragma once
#include "kvs_types.h"
#include "kvs_memory.h"

typedef struct kvs_array_item_s {
	char *key;
	char *value;
} kvs_array_item_t;

#define KVS_ARRAY_SIZE 2048

typedef struct kvs_array_s {
	kvs_array_item_t *table;
	int idx;
	int total;
} kvs_array_t;

int kvs_array_create(kvs_array_t *inst);
void kvs_array_destory(kvs_array_t *inst);

int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char* kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);
int kvs_array_sort(kvs_array_t *inst, int order, int limit, char *response, int max_len);
int kvs_array_range(kvs_array_t *inst, char *start_key, char *end_key, char *response, int max_len);

