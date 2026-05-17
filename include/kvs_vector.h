#pragma once

#include <stddef.h>
#include <stdint.h>

#include "msg_queue.h"
#include <pthread.h>

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
 * vector index 类型。
 *
 * 放在 public 区域，原因：
 * 1. VINFO 需要向命令层暴露当前索引类型；
 * 2. kvs_operation.c 不应该定义 KVS_VECTOR_INTERNAL；
 * 3. 外部只需要知道 flat / hnsw，不需要知道内部索引结构。
 */
typedef enum kvs_vector_index_type_e {
	KVS_VECTOR_INDEX_TYPE_FLAT = 0,
	KVS_VECTOR_INDEX_TYPE_HNSW = 1,
} kvs_vector_index_type_t;

/*
 * Vector metric type.
 *
 * Internal search score convention:
 *   larger score is always better.
 *
 * IP:
 *   score = dot(lhs, rhs)
 *
 * L2:
 *   score = 1 / (1 + squared_l2(lhs, rhs))
 *
 * COSINE:
 *   score = dot(lhs, rhs) / (||lhs|| * ||rhs||)
 */
typedef enum kvs_vector_metric_type_e {
    KVS_VECTOR_METRIC_IP = 0,
    KVS_VECTOR_METRIC_L2 = 1,
    KVS_VECTOR_METRIC_COSINE = 2,
} kvs_vector_metric_type_t;

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

	/*
	 * 0 = flat
	 * 1 = hnsw
	 */
	int index_type;
	int metric_type;

	size_t flat_physical_size;

	size_t ann_size;
	size_t ann_m;
	size_t ann_ef_construction;
	size_t ann_ef_search;
	int ann_max_level;
	int ann_ready;
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

int kvs_vector_set_batch_async(
	const char **questions,
	const size_t *question_lens,
	const char **answers,
	const size_t *answer_lens,
	const char **scopes,
	const size_t *scope_lens,
	size_t count,
	size_t *success_count,
	size_t *failed_count
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

/*
 * 批量 VSEARCH 入口。
 *
 * 语义：
 * 1. 一次批量生成 query embeddings；
 * 2. 每个 query embedding 仍复用现有 HNSW / flat 搜索路径；
 * 3. results 按 query 顺序连续存放：
 *      results[i * results_capacity_per_query + j]
 * 4. results_counts[i] 表示第 i 个 query 实际返回数量。
 */
int kvs_vector_search_batch(
	const char **queries,
	const size_t *query_lens,
	size_t query_count,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity_per_query,
	size_t *results_counts
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

#include "kvs_log.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define KVS_VECTOR_LOG_MODULE "vector"

#define KVS_VECTOR_INITIAL_RECORD_CAPACITY 16
#define KVS_VECTOR_INITIAL_ID_MAP_CAPACITY 32
#define KVS_VECTOR_DEFAULT_DIMENSION 64

#define KVS_VECTOR_ASYNC_QUEUE_CAPACITY 1024
#define KVS_VECTOR_ASYNC_WORKER_COUNT 1

/*
 * 内部批量 embedding 默认批大小。
 *
 * 用于 VBATCHSET / VSEARCH BATCH 内部批量 embedding 生成。
 * 批量导入、AOF/RDB 批量恢复、模型重算都可以复用。
 */
#define KVS_VECTOR_EMBEDDING_BATCH_SIZE 64

#define KVS_VECTOR_EMBEDDING_MAX_INFLIGHT 4

extern kvs_config_t g_cfg;

/********** embedding provider **********/

typedef enum kvs_vector_embedding_provider_type_e {
	KVS_VECTOR_EMBEDDING_PROVIDER_HASH = 0,
} kvs_vector_embedding_provider_type_t;

typedef struct kvs_vector_embedding_provider_s {
	kvs_vector_embedding_provider_type_t type;
	const char *name;
	int initialized;
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

/********** ANN / HNSW index **********/

#define KVS_VECTOR_HNSW_M 32
#define KVS_VECTOR_HNSW_EF_CONSTRUCTION 256
#define KVS_VECTOR_HNSW_EF_SEARCH 256

/*
 * 多层 HNSW 最大层数。
 *
 * level 从 0 开始，0 层是底层完整图；
 * level 越高，节点越稀疏，用于远距离导航。
 */
#define KVS_VECTOR_HNSW_MAX_LEVEL 16

typedef struct kvs_vector_ann_node_s {
	size_t record_index;
	uint64_t record_id;

	/*
	 * 当前节点最高层级。
	 * 该节点拥有 [0, level] 共 level + 1 层邻居表。
	 */
	int level;

	size_t **neighbors;
	size_t *neighbor_counts;
	size_t *neighbor_capacities;

	int active;
} kvs_vector_ann_node_t;

typedef struct kvs_vector_ann_index_s {
	kvs_vector_ann_node_t *nodes;

	size_t size;
	size_t capacity;
	size_t dimension;

	/*
	 * HNSW 全局入口点和当前最高层。
	 */
	size_t entry_node;
	int has_entry;
	int max_level;

	size_t m;
	size_t ef_construction;
	size_t ef_search;

	int ready;
	int needs_rebuild;
} kvs_vector_ann_index_t;

/********** FAISS-like index abstraction **********/

typedef struct kvs_vector_index_search_params_s {
    const char *scope;
    size_t scope_len;
    int topk;
    float threshold;
} kvs_vector_index_search_params_t;

typedef struct kvs_vector_index_batch_params_s {
    const char *scope;
    size_t scope_len;
    int topk;
    float threshold;
} kvs_vector_index_batch_params_t;

struct kvs_vector_index_s;

typedef struct kvs_vector_index_ops_s {
    int (*add)(
        struct kvs_vector_index_s *index,
        size_t record_index,
        const float *embedding,
        int embedding_dim
    );

    int (*search)(
        struct kvs_vector_index_s *index,
        const float *query_embedding,
        int query_dim,
        const kvs_vector_index_search_params_t *params,
        kvs_vector_search_result_t *results,
        size_t results_capacity,
        size_t *results_count
    );

    int (*search_batch)(
        struct kvs_vector_index_s *index,
        const float *query_embeddings,
        size_t query_count,
        int query_dim,
        const kvs_vector_index_batch_params_t *params,
        kvs_vector_search_result_t *results,
        size_t results_capacity_per_query,
        size_t *results_counts
    );

    int (*rebuild)(struct kvs_vector_index_s *index);

    int (*info)(
        struct kvs_vector_index_s *index,
        kvs_vector_info_t *info
    );

    void (*destroy)(struct kvs_vector_index_s *index);
} kvs_vector_index_ops_t;

typedef struct kvs_vector_index_s {
    kvs_vector_index_type_t type;
    kvs_vector_metric_type_t metric;
    size_t dimension;
    void *impl;
    const kvs_vector_index_ops_t *ops;
} kvs_vector_index_t;

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

typedef struct kvs_vector_batch_set_payload_s {
	const char **questions;
	const size_t *question_lens;

	const char **answers;
	const size_t *answer_lens;

	const char **scopes;
	const size_t *scope_lens;

	size_t count;
} kvs_vector_batch_set_payload_t;

/********** global state **********/

typedef enum kvs_vector_async_task_type_e {
	KVS_VECTOR_ASYNC_TASK_NONE = 0,
	KVS_VECTOR_ASYNC_TASK_STOP = 1,
	KVS_VECTOR_ASYNC_TASK_BATCH_SET = 2,
} kvs_vector_async_task_type_t;

typedef struct kvs_vector_async_task_s {
	kvs_vector_async_task_type_t type;

	int result;
	size_t success_count;
	size_t failed_count;

	pthread_mutex_t mu;
	pthread_cond_t cond;
	int done;

	void *payload;
} kvs_vector_async_task_t;

typedef struct kvs_vector_async_dispatcher_s {
	msg_queue_t queue;

	pthread_t *workers;
	size_t worker_count;

	pthread_mutex_t mu;
	pthread_cond_t cond;

	int initialized;
	int stopping;
} kvs_vector_async_dispatcher_t;

typedef struct kvs_vector_state_s {
	int initialized;
	kvs_vector_store_t store;
	kvs_vector_id_map_t id_map;

	kvs_vector_index_type_t index_type;
	kvs_vector_metric_type_t metric_type;

	kvs_vector_flat_index_t flat_index;
	kvs_vector_ann_index_t ann_index;

	kvs_vector_index_t current_index;

	kvs_vector_embedding_provider_t embedding_provider;

	kvs_vector_async_dispatcher_t async_dispatcher;
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

/*
 * 批量生成 embedding。
 *
 * 返回连续 float 数组：
 *
 *   embeddings[i * out_dim + j]
 *
 * 调用方使用 kvs_free 释放 out_embeddings。
 */
int kvs_vector_embedding_generate_batch(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	float **out_embeddings,
	int *out_dim
);

/*
 * Experimental API:
 * Retained for benchmarking only. Not used by default path.
 * Default path uses kvs_vector_embedding_generate_batch().
 */
int kvs_vector_embedding_generate_batch_parallel(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	size_t batch_size,
	size_t max_inflight,
	float **out_embeddings,
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

/*
 * CLEANUP_CANDIDATE:
 * Legacy text-score API. No current caller found.
 * Remove together with implementation after Index abstraction.
 */
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

float kvs_vector_l2_score(
	const float *lhs,
	const float *rhs,
	size_t dim
);

float kvs_vector_cosine_score(
	const float *lhs,
	const float *rhs,
	size_t dim
);

float kvs_vector_metric_score(
	const float *lhs,
	const float *rhs,
	size_t dim,
	kvs_vector_metric_type_t metric
);

const char *kvs_vector_metric_name(kvs_vector_metric_type_t metric);

kvs_vector_metric_type_t kvs_vector_metric_from_config(void);

int kvs_vector_index_bind_current(void);

int kvs_vector_index_add_current(
	size_t record_index,
	const float *embedding,
	int embedding_dim
);

int kvs_vector_index_search_current(
	const float *query_embedding,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

int kvs_vector_index_search_batch_current(
	const float *query_embeddings,
	size_t query_count,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity_per_query,
	size_t *results_counts
);

int kvs_vector_index_rebuild_current(void);

int kvs_vector_flat_index_search(
	kvs_vector_flat_index_t *index,
	const float *query_embedding,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_metric_type_t metric,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

int kvs_vector_flat_index_search_batch(
	kvs_vector_flat_index_t *index,
	const float *query_embeddings,
	size_t query_count,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_metric_type_t metric,
	kvs_vector_search_result_t *results,
	size_t results_capacity_per_query,
	size_t *results_counts
);

int kvs_vector_ann_index_search_batch(
	kvs_vector_ann_index_t *index,
	const float *query_embeddings,
	size_t query_count,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_metric_type_t metric,
	kvs_vector_search_result_t *results,
	size_t results_capacity_per_query,
	size_t *results_counts
);

/********** ANN / HNSW-lite index **********/

int kvs_vector_ann_index_init(
	kvs_vector_ann_index_t *index,
	size_t dimension,
	size_t m,
	size_t ef_construction,
	size_t ef_search
);

void kvs_vector_ann_index_destroy(kvs_vector_ann_index_t *index);

void kvs_vector_ann_index_reset(kvs_vector_ann_index_t *index);

int kvs_vector_ann_index_reserve(
	kvs_vector_ann_index_t *index,
	size_t new_capacity
);

int kvs_vector_ann_index_add(
	kvs_vector_ann_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
);

int kvs_vector_ann_index_search(
	kvs_vector_ann_index_t *index,
	const float *query_embedding,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_metric_type_t metric,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

int kvs_vector_rebuild_ann_index_from_store(void);

int kvs_vector_set_index_type_internal(kvs_vector_index_type_t index_type);

int kvs_vector_ann_index_set_params_internal(
	size_t m,
	size_t ef_construction,
	size_t ef_search
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

int kvs_vector_async_dispatcher_init(
	kvs_vector_async_dispatcher_t *dispatcher,
	size_t queue_capacity,
	size_t worker_count
);

void kvs_vector_async_dispatcher_destroy(
	kvs_vector_async_dispatcher_t *dispatcher
);

int kvs_vector_async_submit_and_wait(
	kvs_vector_async_dispatcher_t *dispatcher,
	kvs_vector_async_task_t *task
);

void kvs_vector_async_task_init(
	kvs_vector_async_task_t *task,
	kvs_vector_async_task_type_t type,
	void *payload
);

void kvs_vector_async_task_destroy(
	kvs_vector_async_task_t *task
);



int kvs_vector_set_batch_internal(
	const char **questions,
	const size_t *question_lens,
	const char **answers,
	const size_t *answer_lens,
	const char **scopes,
	const size_t *scope_lens,
	size_t count,
	size_t *success_count,
	size_t *failed_count
);

/*
 * 纯存储层批量写入接口。
 *
 * 与 kvs_vector_set_batch_internal() 的区别：
 * 1. 不生成 embedding；
 * 2. 直接使用调用方传入的连续 embeddings；
 * 3. 用于内部压测存储层 record / id_map / flat_index 写入性能。
 *
 * embeddings 布局：
 *
 *   embeddings[i * embedding_dim + j]
 *
 * 注意：
 * 该函数仍然会复制每条 embedding 到 record 内部，
 * 并同步追加到 flat_index。
 */
int kvs_vector_set_batch_with_embeddings_internal(
	const char **questions,
	const size_t *question_lens,
	const char **answers,
	const size_t *answer_lens,
	const char **scopes,
	const size_t *scope_lens,
	const float *embeddings,
	int embedding_dim,
	size_t count,
	size_t *success_count,
	size_t *failed_count
);

/*
 * 纯搜索层压测接口。
 *
 * 与 kvs_vector_search() 的区别：
 * 1. 不调用 embedding provider；
 * 2. 直接使用调用方传入的 query_embedding；
 * 3. 用于压测 flat scan + dot product + topK 的纯搜索性能。
 *
 * query_embedding 维度必须等于 flat_index.dimension。
 */
int kvs_vector_search_with_embedding_internal(
	const float *query_embedding,
	int query_dim,
	const char *scope,
	size_t scope_len,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

int kvs_vector_record_set_fields_with_embedding(
	kvs_vector_record_t *record,
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	const float *embedding,
	int embedding_dim
);

int kvs_vector_restore_record_with_embedding(
	size_t record_index,
	uint64_t record_id,
	int state,
	int deleted,
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	const char *normalized_question,
	size_t normalized_question_len,
	const float *embedding,
	int embedding_dim,
	int64_t created_at_ms,
	int64_t updated_at_ms
);

int kvs_vector_reset_for_load(void);

int kvs_vector_rebuild_after_load(void);

#endif /* KVS_VECTOR_INTERNAL */