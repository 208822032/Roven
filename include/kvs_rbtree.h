#pragma once
#include "kvs_types.h"
#include "kvs_memory.h"

#define RED   1
#define BLACK 2

#define ENABLE_KEY_CHAR 1

#if ENABLE_KEY_CHAR
typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE;
#endif

typedef struct _rbtree_node {
	unsigned char color;
	struct _rbtree_node *right;
	struct _rbtree_node *left;
	struct _rbtree_node *parent;
	KEY_TYPE key;
	void *value;
	uint32_t key_len;
	uint32_t value_len;
	int64_t expire_at_ms;
} rbtree_node;

typedef struct _rbtree {
	rbtree_node *root;
	rbtree_node *nil;
} rbtree;

typedef struct _rbtree kvs_rbtree_t;

int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destory(kvs_rbtree_t *inst);
rbtree_node *kvs_rbtree_lookup_node(kvs_rbtree_t *inst, const char *key, size_t key_len);
int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value);
char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_del(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value);
int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key);

/* Binary-safe _len API: key_len and value_len are explicit */
int kvs_rbtree_set_len(kvs_rbtree_t *inst, const char *key, size_t key_len, const char *value, size_t value_len);
char* kvs_rbtree_get_len(kvs_rbtree_t *inst, const char *key, size_t key_len, size_t *value_len_out);
int kvs_rbtree_del_len(kvs_rbtree_t *inst, const char *key, size_t key_len);
int kvs_rbtree_mod_len(kvs_rbtree_t *inst, const char *key, size_t key_len, const char *value, size_t value_len);
int kvs_rbtree_exist_len(kvs_rbtree_t *inst, const char *key, size_t key_len);

int kvs_rbtree_sort(kvs_rbtree_t *inst, int order, int limit, char *response, int max_len);
int kvs_rbtree_range(kvs_rbtree_t *inst, char *start_key, char *end_key, int limit, char *response, int max_len);
void rbtree_traversal_with_callback(rbtree *T, rbtree_node *node,
									void (*callback)(rbtree_node *, void *),
									void *userdata);
void rbtree_traversal_with_callback_limit(rbtree *T, rbtree_node *node,
											void (*callback)(rbtree_node *, void *),
											void *userdata,
											int *budget);

