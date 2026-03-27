#pragma once
#include "kvs_types.h"
#include "kvs_memory.h"

typedef enum kvs_array_index_state_e {
	KVS_ARRAY_INDEX_EMPTY = 0,
	KVS_ARRAY_INDEX_USED = 1,
	KVS_ARRAY_INDEX_TOMBSTONE = 2,
} kvs_array_index_state_t;

typedef struct kvs_array_item_s {
	char *key;
	char *value;
	uint32_t key_len;
	uint32_t value_len;
	uint32_t key_hash;
	int64_t expire_at_ms;
} kvs_array_item_t;

typedef struct kvs_array_index_entry_s {
	char *key;
	uint32_t key_len;
	uint32_t key_hash;
	int dense_idx;
	uint8_t state;
	uint8_t padding[3];
} kvs_array_index_entry_t;

#define KVS_ARRAY_SIZE 2048

typedef struct kvs_array_s {
	/* table: 存储槽位数组 */
	kvs_array_item_t *table;
	/* side_index: key -> dense_idx 的辅助索引 */
	kvs_array_index_entry_t *side_index;
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
	/* side_capacity: side index 的槽位总容量 */
	int side_capacity;
	/* side_count: side index 当前有效槽位数 */
	int side_count;
	/* side_tombstones: side index 中的墓碑数 */
	int side_tombstones;
} kvs_array_t;

int kvs_array_create(kvs_array_t *inst);
void kvs_array_destory(kvs_array_t *inst);
int kvs_array_reserve(kvs_array_t *inst, int new_cap);

int kvs_array_lookup_index(kvs_array_t *inst, const char *key, int *out_idx);
int kvs_array_del_at(kvs_array_t *inst, int idx);

int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char* kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);
int kvs_array_sort(kvs_array_t *inst, int order, int limit, char *response, int max_len);
int kvs_array_range(kvs_array_t *inst, char *start_key, char *end_key, char *response, int max_len);

