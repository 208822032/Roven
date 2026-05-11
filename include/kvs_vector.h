#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * 向量模块内部状态码。
 *
 * 说明：
 * 1. 向量模块不要直接复用 RESP 字符串作为内部错误码。
 * 2. 命令层 handler 负责把这些状态码转换成 RESP 响应。
 */
typedef enum kvs_vector_status_e {
	KVS_VECTOR_OK = 0,
	KVS_VECTOR_ERR = -1,
	KVS_VECTOR_ERR_INVALID_ARG = -2,
	KVS_VECTOR_ERR_NOT_FOUND = -3,
	KVS_VECTOR_ERR_NOT_IMPLEMENTED = -4,
	KVS_VECTOR_ERR_NOT_INITIALIZED = -5,
} kvs_vector_status_t;

/*
 * VGET / VSEARCH 返回的单条检索结果。
 *
 * 注意：
 * 当前 result 字段通常引用 vector record 内部只读指针。
 * 调用方不要释放 question / answer / scope。
 */
typedef struct kvs_vector_search_result_s {
	char *question;
	size_t question_len;

	char *answer;
	size_t answer_len;

	char *scope;
	size_t scope_len;

	float score;
} kvs_vector_search_result_t;

/*
 * VINFO 返回的向量模块统计信息。
 */
typedef struct kvs_vector_info_s {
	size_t total_records;
	size_t active_records;
	size_t deleted_records;
	size_t capacity;
	size_t index_size;

	size_t dimension;
	int index_ready;
	int needs_rebuild;
} kvs_vector_info_t;

/********** 向量模块公开接口 **********/

int kvs_vector_init(void);

void kvs_vector_destroy(void);

int kvs_vector_set(
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len
);

int kvs_vector_get(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_search_result_t *result
);

int kvs_vector_search(
	const char *query,
	size_t query_len,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

int kvs_vector_del(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
);

int kvs_vector_index_rebuild(void);

int kvs_vector_info(kvs_vector_info_t *info);

void kvs_vector_search_result_reset(kvs_vector_search_result_t *result);

void kvs_vector_search_results_reset(
	kvs_vector_search_result_t *results,
	size_t count
);

/*
 * 以下内容只给 V 族内部实现文件使用。
 *
 * 使用方式：
 *
 *   #define KVS_VECTOR_INTERNAL
 *   #include "kvs_vector.h"
 *
 * 普通业务模块不要定义 KVS_VECTOR_INTERNAL。
 */
#ifdef KVS_VECTOR_INTERNAL

#include "kvs_api.h"
#include "kvs_config.h"
#include "kvs_embedding_grpc_client.h"
#include "kvs_log.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define KVS_VECTOR_LOG_MODULE "vector"

#define KVS_VECTOR_INITIAL_RECORD_CAPACITY 16
#define KVS_VECTOR_INITIAL_ID_MAP_CAPACITY 32
#define KVS_VECTOR_DEFAULT_DIMENSION 64

extern kvs_config_t g_cfg;

/********** embedding provider **********/

typedef enum kvs_vector_embedding_provider_type_e {
	KVS_VECTOR_EMBEDDING_PROVIDER_HASH = 0,
	KVS_VECTOR_EMBEDDING_PROVIDER_GRPC = 1,
} kvs_vector_embedding_provider_type_t;

typedef struct kvs_vector_embedding_provider_s {
	kvs_vector_embedding_provider_type_t type;
	const char *name;
	int initialized;

	char grpc_addr[128];
	int grpc_timeout_ms;
	int fallback_to_hash;
	int grpc_healthy;
	int grpc_dimension;
} kvs_vector_embedding_provider_t;

/********** record / store **********/

typedef enum kvs_vector_record_state_e {
	KVS_VECTOR_RECORD_EMPTY = 0,
	KVS_VECTOR_RECORD_ACTIVE = 1,
	KVS_VECTOR_RECORD_DELETED = 2,
} kvs_vector_record_state_t;

typedef struct kvs_vector_record_s {
	uint64_t id;
	uint32_t question_hash;

	char *question;
	size_t question_len;

	char *answer;
	size_t answer_len;

	char *normalized_question;
	size_t normalized_question_len;

	char *scope;
	size_t scope_len;

	float *embedding;
	int embedding_dim;

	int state;
	int deleted;

	int64_t created_at_ms;
	int64_t updated_at_ms;
} kvs_vector_record_t;

typedef struct kvs_vector_store_s {
	kvs_vector_record_t *records;
	size_t capacity;

	size_t total_records;
	size_t active_records;
	size_t deleted_records;

	size_t next_slot;

	size_t *free_indices;
	size_t free_count;
	size_t free_capacity;

	uint64_t next_id;
} kvs_vector_store_t;

/********** id map **********/

typedef enum kvs_vector_map_entry_state_e {
	KVS_VECTOR_MAP_ENTRY_EMPTY = 0,
	KVS_VECTOR_MAP_ENTRY_USED = 1,
	KVS_VECTOR_MAP_ENTRY_TOMBSTONE = 2,
} kvs_vector_map_entry_state_t;

typedef struct kvs_vector_id_entry_s {
	uint32_t hash;
	size_t record_index;
	uint8_t state;
} kvs_vector_id_entry_t;

typedef struct kvs_vector_id_map_s {
	kvs_vector_id_entry_t *entries;
	size_t capacity;
	size_t size;
	size_t tombstones;
} kvs_vector_id_map_t;

/********** flat index **********/

typedef struct kvs_vector_flat_index_s {
	float *vectors;
	size_t *record_indices;
	uint64_t *record_ids;

	size_t size;
	size_t capacity;
	size_t dimension;
	int ready;
	int needs_rebuild;
} kvs_vector_flat_index_t;

/********** topK **********/

typedef struct kvs_vector_candidate_s {
	size_t record_index;
	float score;
} kvs_vector_candidate_t;

typedef struct kvs_vector_topk_heap_s {
	kvs_vector_candidate_t *items;
	size_t size;
	size_t capacity;
} kvs_vector_topk_heap_t;

typedef struct kvs_vector_query_ctx_s {
	const char *query;
	size_t query_len;
	const char *scope;
	size_t scope_len;
	int topk;
	float threshold;
} kvs_vector_query_ctx_t;

/********** global state **********/

typedef struct kvs_vector_state_s {
	int initialized;
	kvs_vector_store_t store;
	kvs_vector_id_map_t id_map;
	kvs_vector_flat_index_t flat_index;
	kvs_vector_embedding_provider_t embedding_provider;
} kvs_vector_state_t;

extern kvs_vector_state_t g_vector_state;

/********** embedding layer **********/

int kvs_vector_embedding_provider_init(kvs_vector_embedding_provider_t *provider);

void kvs_vector_embedding_provider_destroy(kvs_vector_embedding_provider_t *provider);

const char *kvs_vector_embedding_provider_name(void);

size_t kvs_vector_embedding_provider_dimension(void);

int kvs_vector_embedding_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
);

/********** index / store layer **********/

size_t kvs_vector_next_pow2(size_t value);

uint32_t kvs_vector_hash_key(const char *data, size_t len);

uint32_t kvs_vector_compose_hash(
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
);

char *kvs_vector_dup_string(
	const char *src,
	size_t src_len,
	size_t *out_len
);

int kvs_vector_text_equal(
	const char *lhs,
	size_t lhs_len,
	const char *rhs,
	size_t rhs_len
);

int kvs_vector_scope_equal(
	const char *lhs,
	size_t lhs_len,
	const char *rhs,
	size_t rhs_len
);

void kvs_vector_record_reset(kvs_vector_record_t *record);

void kvs_vector_record_destroy(kvs_vector_record_t *record);

int kvs_vector_record_set_fields(
	kvs_vector_record_t *record,
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len
);

int kvs_vector_record_update_answer(
	kvs_vector_record_t *record,
	const char *answer,
	size_t answer_len
);

int kvs_vector_record_mark_deleted(kvs_vector_record_t *record);

int kvs_vector_record_match(
	const kvs_vector_record_t *record,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
);

int kvs_vector_record_scope_match(
	const kvs_vector_record_t *record,
	const char *scope,
	size_t scope_len
);

void kvs_vector_search_result_from_record(
	kvs_vector_search_result_t *result,
	const kvs_vector_record_t *record
);

float kvs_vector_record_score(
	const kvs_vector_record_t *record,
	const char *query,
	size_t query_len
);


int kvs_vector_store_init(kvs_vector_store_t *store);

void kvs_vector_store_destroy(kvs_vector_store_t *store);

int kvs_vector_store_reserve(
	kvs_vector_store_t *store,
	size_t new_capacity
);

int kvs_vector_store_ensure_capacity(
	kvs_vector_store_t *store,
	size_t min_capacity
);

int kvs_vector_store_compact(kvs_vector_store_t *store);

int kvs_vector_store_free_list_push(
	kvs_vector_store_t *store,
	size_t record_index
);

int kvs_vector_store_free_list_pop(
	kvs_vector_store_t *store,
	size_t *out_record_index
);

void kvs_vector_store_free_list_clear(kvs_vector_store_t *store);

int kvs_vector_id_map_init(kvs_vector_id_map_t *map);

void kvs_vector_id_map_destroy(kvs_vector_id_map_t *map);

int kvs_vector_id_map_reserve(
	kvs_vector_id_map_t *map,
	size_t new_capacity
);

int kvs_vector_id_map_get(
	kvs_vector_id_map_t *map,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	size_t *out_record_index
);

int kvs_vector_id_map_put(
	kvs_vector_id_map_t *map,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len,
	size_t record_index
);

int kvs_vector_id_map_remove(
	kvs_vector_id_map_t *map,
	const char *question,
	size_t question_len,
	const char *scope,
	size_t scope_len
);

int kvs_vector_id_map_rebuild_from_store(void);

int kvs_vector_flat_index_init(
	kvs_vector_flat_index_t *index,
	size_t dimension
);

void kvs_vector_flat_index_destroy(kvs_vector_flat_index_t *index);

void kvs_vector_flat_index_reset(kvs_vector_flat_index_t *index);

void kvs_vector_flat_index_mark_stale(kvs_vector_flat_index_t *index);

int kvs_vector_flat_index_reserve(
	kvs_vector_flat_index_t *index,
	size_t new_capacity
);

int kvs_vector_flat_index_add(
	kvs_vector_flat_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
);

int kvs_vector_rebuild_flat_index_from_store(void);

float kvs_vector_dot_product(
	const float *lhs,
	const float *rhs,
	size_t dim
);

/********** topK heap **********/

int kvs_vector_topk_heap_init(
	kvs_vector_topk_heap_t *heap,
	size_t capacity
);

void kvs_vector_topk_heap_destroy(kvs_vector_topk_heap_t *heap);

void kvs_vector_topk_heap_reset(kvs_vector_topk_heap_t *heap);

int kvs_vector_topk_heap_push(
	kvs_vector_topk_heap_t *heap,
	size_t record_index,
	float score
);

int kvs_vector_topk_heap_export_desc(
	kvs_vector_topk_heap_t *heap,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count,
	const kvs_vector_store_t *store
);

#endif /* KVS_VECTOR_INTERNAL */