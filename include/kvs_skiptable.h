#pragma once
#include "kvs_types.h"
#include "kvs_memory.h"

#define MAX_LEVEL 16

typedef struct Node {
	char *key;
	char *value;
	uint32_t key_len;
	uint32_t value_len;
	unsigned int key_hash;
	int level;
	int64_t expire_at_ms;
	struct Node* forward[];
} Node;

typedef struct {
	int level;
	Node* header;
	int count;
} kvs_skiptable_t;

int kvs_skiptable_create(kvs_skiptable_t *table);
void kvs_skiptable_destroy(kvs_skiptable_t *table);
int kvs_skiptable_set(kvs_skiptable_t *table, char *key, char *value);
Node* kvs_skiptable_lookup_node(kvs_skiptable_t *table, char *key);
char* kvs_skiptable_get(kvs_skiptable_t *table, char *key);
int kvs_skiptable_mod(kvs_skiptable_t *table, char *key, char *value);
int kvs_skiptable_del(kvs_skiptable_t *table, char *key);
int kvs_skiptable_exist(kvs_skiptable_t *table, char *key);

/* Binary-safe _len API: key_len and value_len are explicit */
int kvs_skiptable_set_len(kvs_skiptable_t *inst, const char *key, size_t key_len, const char *value, size_t value_len);
char* kvs_skiptable_get_len(kvs_skiptable_t *inst, const char *key, size_t key_len, size_t *value_len_out);
int kvs_skiptable_del_len(kvs_skiptable_t *inst, const char *key, size_t key_len);
int kvs_skiptable_mod_len(kvs_skiptable_t *inst, const char *key, size_t key_len, const char *value, size_t value_len);
int kvs_skiptable_exist_len(kvs_skiptable_t *inst, const char *key, size_t key_len);

int kvs_skiptable_sort(kvs_skiptable_t *table, int order, int limit, char *response, int max_len);
int kvs_skiptable_range(kvs_skiptable_t *table, char *start_key, char *end_key, int limit, char *response, int max_len);

