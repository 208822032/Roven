#pragma once
#include "kvs_types.h"
#include "kvs_memory.h"

#define MAX_LEVEL 16

typedef struct Node {
	char *key;
	char *value;
	int level;
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
char* kvs_skiptable_get(kvs_skiptable_t *table, char *key);
int kvs_skiptable_mod(kvs_skiptable_t *table, char *key, char *value);
int kvs_skiptable_del(kvs_skiptable_t *table, char *key);
int kvs_skiptable_exist(kvs_skiptable_t *table, char *key);
int kvs_skiptable_sort(kvs_skiptable_t *table, int order, int limit, char *response, int max_len);
int kvs_skiptable_range(kvs_skiptable_t *table, char *start_key, char *end_key, int limit, char *response, int max_len);

