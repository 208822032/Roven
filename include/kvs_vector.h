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
	KVS_VECTOR_INDEX_TYPE_SQ8_FLAT = 3,
	KVS_VECTOR_INDEX_FAISS_HNSW = 4,
	KVS_VECTOR_INDEX_FAISS_FLAT = 5,
	KVS_VECTOR_INDEX_HNSWLIB_HNSW = 6,
	KVS_VECTOR_INDEX_CORE_FLAT = 7,
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
	 * 3 = sq8_flat
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

	/* SQ8 Flat fields */
	size_t sq8_size;
	size_t sq8_dimension;
	size_t sq8_code_bytes;
	size_t sq8_scale_bytes;
	size_t sq8_memory_bytes;
	int sq8_ready;
	int sq8_needs_rebuild;

	/* core_flat fields */
	size_t core_flat_deleted_count;

	/* runtime stats */
	int last_rebuild_status;
	int64_t last_rebuild_ms;
	size_t vector_memory_bytes;
	size_t index_memory_bytes;
	size_t total_vector_memory_bytes;

	/* Stage 2B: worker pool + executor prepared stats */
	int embedding_worker_pool_enabled;
	int embedding_worker_threads;
	uint64_t embedding_worker_submitted;
	uint64_t embedding_worker_completed;
	uint64_t embedding_worker_failed;
	uint64_t embedding_worker_queue_depth;
	uint64_t embedding_worker_max_queue_depth;
	uint64_t executor_prepare_task_submitted;
	uint64_t executor_prepared_task_executed;
	uint64_t executor_prepared_task_failed;

	/* faiss backend stats */
	int faiss_backend_enabled;
	size_t faiss_ntotal;
	uint64_t faiss_add_count;
	uint64_t faiss_search_count;

	/* hnswlib backend stats */
	int hnswlib_backend_enabled;
	size_t hnswlib_ntotal;
	size_t hnswlib_capacity;
	size_t hnswlib_m;
	size_t hnswlib_ef_construction;
	size_t hnswlib_ef_search;
	uint64_t hnswlib_add_count;
	uint64_t hnswlib_search_count;
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

/*
 * Stage-1 prepared vector APIs.
 *
 * prepare runs without touching vector store/id_map/HNSW. The caller owns the
 * copied strings and embedding returned by *_prepare and must either pass them
 * to commit/search or destroy/free them on failure.
 */
typedef struct kvs_vector_set_prepared_s {
	char *question;
	size_t question_len;
	char *answer;
	size_t answer_len;
	char *scope;
	size_t scope_len;
	float *embedding;
	int embedding_dim;
} kvs_vector_set_prepared_t;

typedef struct kvs_vector_search_prepared_s {
	char *query;
	size_t query_len;
	char *scope;
	size_t scope_len;
	float *embedding;
	int embedding_dim;
} kvs_vector_search_prepared_t;

void kvs_vector_set_prepared_reset(kvs_vector_set_prepared_t *prepared);

void kvs_vector_set_prepared_destroy(kvs_vector_set_prepared_t *prepared);

int kvs_vector_set_prepare(
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_set_prepared_t *prepared
);

int kvs_vector_set_prepare_with_embedding(
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	const float *embedding,
	int embedding_dim,
	kvs_vector_set_prepared_t *prepared
);

int kvs_vector_set_commit_prepared(kvs_vector_set_prepared_t *prepared);

typedef struct kvs_vector_vset_commit_stats_s {
	uint64_t vset_commit_count;
	uint64_t vset_commit_total_us;
	uint64_t vset_commit_max_us;
	uint64_t vset_record_lookup_count;
	uint64_t vset_record_lookup_total_us;
	uint64_t vset_record_lookup_max_us;
	uint64_t vset_record_insert_or_update_count;
	uint64_t vset_record_insert_or_update_total_us;
	uint64_t vset_record_insert_or_update_max_us;
	uint64_t vset_vector_copy_count;
	uint64_t vset_vector_copy_total_us;
	uint64_t vset_vector_copy_max_us;
	uint64_t vset_flat_index_update_count;
	uint64_t vset_flat_index_update_total_us;
	uint64_t vset_flat_index_update_max_us;
	uint64_t vset_backend_add_count;
	uint64_t vset_backend_add_total_us;
	uint64_t vset_backend_add_max_us;
	uint64_t vset_id_map_update_count;
	uint64_t vset_id_map_update_total_us;
	uint64_t vset_id_map_update_max_us;
	uint64_t vset_scope_answer_question_update_count;
	uint64_t vset_scope_answer_question_update_total_us;
	uint64_t vset_scope_answer_question_update_max_us;
	uint64_t vset_lock_wait_count;
	uint64_t vset_lock_wait_total_us;
	uint64_t vset_lock_wait_max_us;
	uint64_t vset_lock_hold_count;
	uint64_t vset_lock_hold_total_us;
	uint64_t vset_lock_hold_max_us;
	uint64_t vset_aof_append_count;
	uint64_t vset_aof_append_total_us;
	uint64_t vset_aof_append_max_us;
	uint64_t vset_replication_count;
	uint64_t vset_replication_total_us;
	uint64_t vset_replication_max_us;
	uint64_t vset_dirty_mark_count;
	uint64_t vset_dirty_mark_total_us;
	uint64_t vset_dirty_mark_max_us;
	uint64_t vset_response_build_count;
	uint64_t vset_response_build_total_us;
	uint64_t vset_response_build_max_us;
	uint64_t vset_response_push_count;
	uint64_t vset_response_push_total_us;
	uint64_t vset_response_push_max_us;
	uint64_t vsearch_search_count;
	uint64_t vsearch_search_total_us;
	uint64_t vsearch_search_max_us;
	uint64_t vsearch_response_build_count;
	uint64_t vsearch_response_build_total_us;
	uint64_t vsearch_response_build_max_us;
	uint64_t vsearch_response_push_count;
	uint64_t vsearch_response_push_total_us;
	uint64_t vsearch_response_push_max_us;
	uint64_t flat_search_core_count;
	uint64_t flat_search_core_total_us;
	uint64_t flat_search_core_max_us;
	uint64_t flat_search_export_count;
	uint64_t flat_search_export_total_us;
	uint64_t flat_search_export_max_us;
	uint64_t hnsw_search_core_count;
	uint64_t hnsw_search_core_total_us;
	uint64_t hnsw_search_core_max_us;
	uint64_t hnsw_search_export_count;
	uint64_t hnsw_search_export_total_us;
	uint64_t hnsw_search_export_max_us;
} kvs_vector_vset_commit_stats_t;

void kvs_vector_vset_commit_stats_get(kvs_vector_vset_commit_stats_t *out);
void kvs_vector_vset_record_lock_wait(uint64_t elapsed_us);
void kvs_vector_vset_record_lock_hold(uint64_t elapsed_us);
void kvs_vector_vset_record_aof_append(uint64_t elapsed_us);
void kvs_vector_vset_record_replication(uint64_t elapsed_us);
void kvs_vector_vset_record_dirty_mark(uint64_t elapsed_us);
void kvs_vector_vset_record_response_build(uint64_t elapsed_us);
void kvs_vector_vset_record_response_push(uint64_t elapsed_us);
void kvs_vector_vsearch_record_search(uint64_t elapsed_us);
void kvs_vector_vsearch_record_response_build(uint64_t elapsed_us);
void kvs_vector_vsearch_record_response_push(uint64_t elapsed_us);
void kvs_vector_flat_record_search_core(uint64_t elapsed_us);
void kvs_vector_flat_record_search_export(uint64_t elapsed_us);
void kvs_vector_hnsw_record_search_core(uint64_t elapsed_us);
void kvs_vector_hnsw_record_search_export(uint64_t elapsed_us);

void kvs_vector_search_prepared_reset(kvs_vector_search_prepared_t *prepared);

void kvs_vector_search_prepared_destroy(kvs_vector_search_prepared_t *prepared);

int kvs_vector_search_prepare(
	const char *query,
	size_t query_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_search_prepared_t *prepared
);

int kvs_vector_search_prepare_with_embedding(
	const char *query,
	size_t query_len,
	const char *scope,
	size_t scope_len,
	const float *embedding,
	int embedding_dim,
	kvs_vector_search_prepared_t *prepared
);

int kvs_vector_search_with_prepared(
	const kvs_vector_search_prepared_t *prepared,
	int topk,
	float threshold,
	kvs_vector_search_result_t *results,
	size_t results_capacity,
	size_t *results_count
);

int kvs_vector_embedding_generate_batch(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	float **out_embeddings,
	int *out_dim
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

typedef struct kvs_vector_embedding_runtime_stats_s {
	uint64_t per_thread_context_enabled;
	uint64_t per_thread_context_created;
	uint64_t per_thread_context_destroyed;
	uint64_t per_thread_context_create_failed;
	uint64_t per_thread_context_fallback_count;
	uint64_t encode_global_mutex_count;
	uint64_t encode_thread_context_count;
	uint64_t encode_current_inflight;
	uint64_t encode_max_inflight;
	uint64_t encode_total_us;
	uint64_t encode_max_us;
	uint64_t shared_ort_env_enabled;
	uint64_t shared_ort_env_created;
	uint64_t shared_ort_env_destroyed;
	uint64_t shared_ort_env_refcount_current;
	uint64_t shared_ort_env_refcount_max;
	uint64_t ort_session_created;
	uint64_t ort_session_destroyed;
	uint64_t encode_count;
	uint64_t embedding_encode_total_us;
	uint64_t embedding_encode_max_us;
	uint64_t tokenizer_total_us;
	uint64_t tokenizer_max_us;
	uint64_t ort_run_total_us;
	uint64_t ort_run_max_us;
	uint64_t embedding_cache_enabled;
	uint64_t embedding_cache_lookup_count;
	uint64_t embedding_cache_hit_count;
	uint64_t embedding_cache_miss_count;
	uint64_t embedding_cache_lookup_total_us;
	uint64_t embedding_cache_insert_count;
	uint64_t embedding_cache_insert_total_us;
	uint64_t embedding_cache_evict_count;
	uint64_t embedding_cache_memory_bytes;
	uint64_t embedding_cache_saved_onnx_us;
	uint64_t embedding_runtime_resource_product;
	uint64_t embedding_worker_threads_effective;
	uint64_t embedding_num_threads_effective;
} kvs_vector_embedding_runtime_stats_t;

void kvs_vector_embedding_runtime_get_stats(
	kvs_vector_embedding_runtime_stats_t *out
);

int kvs_vector_embedding_thread_context_init(void);

void kvs_vector_embedding_thread_context_destroy(void);

int kvs_vector_embedding_thread_context_available(void);

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

#include "embedding_c_api.h"
#include "kvs_log.h"

#include "vector/kvs_vector_record.h"
#include "vector/kvs_vector_core.h"
#include "vector/kvs_vector_index_internal.h"
#include "vector/kvs_vector_manager.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define KVS_VECTOR_LOG_MODULE "vector"

#define KVS_VECTOR_INITIAL_RECORD_CAPACITY 16
#define KVS_VECTOR_INITIAL_ID_MAP_CAPACITY 32
#define KVS_VECTOR_DEFAULT_DIMENSION 64

extern kvs_config_t g_cfg;

#endif /* KVS_VECTOR_INTERNAL */
