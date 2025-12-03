#pragma once
#include "kvs_types.h"
#include "kvs_memory.h"

#define MAX_KEY_LEN     128
#define MAX_VALUE_LEN   512
#define MAX_TABLE_SIZE  1048576

#define ENABLE_KEY_POINTER 0

#if 0
typedef struct hashnode_s {
#if ENABLE_KEY_POINTER
	char *key;
	char *value;
#else
	char key[MAX_KEY_LEN];
	char value[MAX_VALUE_LEN];
#endif
	struct hashnode_s *next;
} hashnode_t;
#else
typedef struct hashnode_s {
	unsigned char used;   // 0: empty, 1: used, 2: tombstone
	unsigned char pad;    // 对齐填充
	unsigned short klen;
	unsigned int   vlen;
	char *key;
	char *value;
} hashnode_t;
#endif

typedef struct hashtable_s {
	hashnode_t *nodes;
	int max_slots;
	int count;
} hashtable_t;

typedef hashtable_t kvs_hash_t;

int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(kvs_hash_t *hash, char *key, char *value);
char* kvs_hash_get(kvs_hash_t *hash, char *key);
int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value);
int kvs_hash_del(kvs_hash_t *hash, char *key);
int kvs_hash_exist(kvs_hash_t *hash, char *key);
int kvs_hash_sort(kvs_hash_t *inst, int order, int limit, char *response, int max_len);
int kvs_hash_range(kvs_hash_t *inst, char *start_key, char *end_key, char *response, int max_len);

