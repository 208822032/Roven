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
 * 3. 旧 enum 值需要保持编号兼容，但不会再进入已删除实现路径。
 */
typedef enum kvs_vector_index_type_e {
	/* Removed compatibility enum values; requests are mapped to core_flat. */
	KVS_VECTOR_INDEX_TYPE_FLAT = 0,
	KVS_VECTOR_INDEX_TYPE_HNSW = 1,
	KVS_VECTOR_INDEX_TYPE_SQ8_FLAT = 3,
	/* Removed external backend compatibility enum values. */
	KVS_VECTOR_INDEX_FAISS_HNSW = 4,
	KVS_VECTOR_INDEX_FAISS_FLAT = 5,
	KVS_VECTOR_INDEX_HNSWLIB_HNSW = 6,
	KVS_VECTOR_INDEX_CORE_FLAT = 7,
	/* Compatibility enum only; legacy core_hnsw source was removed. */
	KVS_VECTOR_INDEX_CORE_HNSW = 8,
	KVS_VECTOR_INDEX_CORE_HNSW_V2 = 9,
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
	 * 0/1/3 are retained compatibility values for removed legacy backends.
	 * Current product values are KVS_VECTOR_INDEX_CORE_FLAT and
	 * KVS_VECTOR_INDEX_CORE_HNSW_V2.
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

	/* Removed SQ8 Flat compatibility VINFO fields. */
	size_t sq8_size;
	size_t sq8_dimension;
	size_t sq8_code_bytes;
	size_t sq8_scale_bytes;
	size_t sq8_memory_bytes;
	int sq8_ready;
	int sq8_needs_rebuild;

	/* core_flat fields */
	size_t core_flat_deleted_count;

	/* Removed core_hnsw compatibility VINFO fields. */
	size_t core_hnsw_size;
	size_t core_hnsw_capacity;
	size_t core_hnsw_m;
	size_t core_hnsw_m0;
	size_t core_hnsw_ef_construction;
	size_t core_hnsw_ef_search;
	int core_hnsw_max_level;
	size_t core_hnsw_deleted_count;

	/* core_hnsw_v2 fields */
	/* Historical compatibility / telemetry only; no longer gates v2 selection. */
	int core_hnsw_v2_experimental_enabled;
	size_t core_hnsw_v2_size;
	size_t core_hnsw_v2_capacity;
	size_t core_hnsw_v2_m;
	size_t core_hnsw_v2_m0;
	size_t core_hnsw_v2_ef_construction;
	size_t core_hnsw_v2_ef_search;
	int core_hnsw_v2_max_level;
	size_t core_hnsw_v2_deleted_count;
	size_t core_hnsw_v2_active_count;
	size_t core_hnsw_v2_memory_bytes;
	int core_hnsw_v2_ready;

	/* manager-owned rebuild status */
	const char *rebuild_state;
	size_t rebuild_delta_count;
	int rebuild_error_code;
	const char *rebuild_error_message;
	int rebuild_shadow_present;
	int rebuild_retired_present;

	/* manager-owned optional HNSW snapshot cache status */
	int snapshot_supported;
	int snapshot_load_attempted;
	int snapshot_loaded;
	int snapshot_valid;
	int loaded_from_snapshot;
	uint64_t snapshot_fallback_rebuild_count;
	const char *snapshot_last_error;
	int snapshot_write_attempted;
	int snapshot_written;
	const char *snapshot_last_write_error;
	uint64_t snapshot_last_write_started_ms;
	uint64_t snapshot_last_write_finished_ms;
	uint64_t snapshot_last_bytes;
	uint64_t snapshot_last_checksum;

	/* Stage 5C: unified backend observability fields */
	const char *backend_name;
	size_t backend_size;
	size_t backend_capacity;
	size_t backend_dim;
	const char *backend_metric;
	size_t backend_memory_bytes;
	const char *backend_memory_bytes_source;
	size_t backend_deleted_count;
	int backend_needs_rebuild;
	const char *backend_resize_count;
	const char *backend_reserve_count;
	const char *backend_resize_count_source;
	const char *backend_reserve_count_source;

		/* runtime stats */
		int last_rebuild_status;
		int64_t last_rebuild_ms;
		uint64_t rebuild_total_us;
		uint64_t rebuild_manager_scan_us;
		uint64_t rebuild_core_reset_us;
		uint64_t rebuild_core_reserve_us;
		uint64_t rebuild_core_add_total_us;
		uint64_t rebuild_core_add_avg_us;
		uint64_t rebuild_core_add_max_us;
		uint64_t rebuild_core_add_count;
		uint64_t hnsw_add_random_level_us;
		uint64_t hnsw_add_greedy_search_us;
		uint64_t hnsw_add_layer_search_us;
		uint64_t hnsw_add_neighbor_select_us;
		uint64_t hnsw_add_link_us;
		uint64_t hnsw_add_vector_copy_us;
		/* Stage 7A: event counters for memory access pattern audit */
		uint64_t add_distance_compute_count;
		uint64_t add_neighbor_visit_count;
		uint64_t add_visited_skip_count;
		uint64_t add_active_check_count;
		uint64_t add_top_heap_push_count;
		uint64_t add_top_heap_replace_count;
		uint64_t add_candidate_push_count;
		uint64_t add_level0_neighbor_scan_count;
		uint64_t add_upper_neighbor_scan_count;
		/* Stage 7F: layer_search attribution counters */
		uint64_t layer_search_call_count;
		uint64_t layer_search_level0_call_count;
		uint64_t layer_search_upper_call_count;
		uint64_t layer_search_level0_us;
		uint64_t layer_search_upper_us;
		uint64_t layer_search_distance_compute_count;
		uint64_t layer_search_visited_skip_count;
		uint64_t layer_search_neighbor_visit_count;
		uint64_t layer_search_level0_neighbor_scan_count;
		uint64_t layer_search_upper_neighbor_scan_count;
		uint64_t layer_search_candidate_heap_push_count;
		uint64_t layer_search_candidate_heap_pop_count;
		uint64_t layer_search_candidate_heap_replace_count;
		uint64_t layer_search_top_heap_push_count;
		uint64_t layer_search_top_heap_replace_count;
		uint64_t layer_search_lower_bound_update_count;
		uint64_t layer_search_deleted_active_check_count;
		/* Stage 7H: level0 neighbor access / visited_tags layout audit */
		uint64_t layer_search_unique_visit_count;
		uint64_t layer_search_repeated_visit_count;
		uint64_t layer_search_visited_tag_access_count;
		uint64_t layer_search_slot_state_touch_count;
		uint64_t layer_search_level0_unique_visit_count;
		uint64_t layer_search_level0_repeated_visit_count;
		uint64_t layer_search_level0_visited_tag_access_count;
		uint64_t layer_search_level0_slot_state_touch_count;
		uint64_t layer_search_level0_neighbor_row_access_count;
		uint64_t layer_search_level0_neighbor_row_sequential_count;
		uint64_t layer_search_level0_neighbor_row_random_count;
		uint64_t layer_search_level0_neighbor_slot_sequential_count;
		uint64_t layer_search_level0_neighbor_slot_random_count;
		/* Stage 7L: post-layout remaining layer_search hotspot audit */
		uint64_t layer_search_level0_candidate_row_expansion_count;
		uint64_t layer_search_level0_distance_compute_count;
		uint64_t layer_search_upper_distance_compute_count;
		uint64_t layer_search_level0_deleted_active_check_count;
		uint64_t layer_search_upper_deleted_active_check_count;
		uint64_t layer_search_candidate_heap_skip_count;
		uint64_t layer_search_candidate_heap_full_skip_count;
		uint64_t layer_search_top_heap_skip_count;
		uint64_t layer_search_level0_candidate_heap_push_count;
		uint64_t layer_search_level0_candidate_heap_pop_count;
		uint64_t layer_search_level0_candidate_heap_skip_count;
		uint64_t layer_search_level0_top_heap_push_count;
		uint64_t layer_search_level0_top_heap_replace_count;
		uint64_t layer_search_level0_top_heap_skip_count;
		uint64_t layer_search_level0_lower_bound_update_count;
		uint64_t core_hnsw_level0_count_min;
		uint64_t core_hnsw_level0_count_avg;
		uint64_t core_hnsw_level0_count_p50;
		uint64_t core_hnsw_level0_count_p95;
		uint64_t core_hnsw_level0_count_max;
		size_t core_hnsw_memory_vectors_bytes;
		size_t core_hnsw_memory_level0_neighbors_bytes;
		size_t core_hnsw_memory_level0_scores_bytes;
		size_t core_hnsw_memory_upper_neighbors_bytes;
		size_t core_hnsw_memory_visited_tags_bytes;
		size_t core_hnsw_memory_slot_state_bytes;
		size_t core_hnsw_memory_context_bytes;
		/* Stage 7D: neighbor_select attribution counters */
		uint64_t neighbor_select_call_count;
		uint64_t neighbor_select_level0_call_count;
		uint64_t neighbor_select_upper_call_count;
		uint64_t neighbor_select_candidate_total;
		uint64_t neighbor_select_candidate_max;
		uint64_t neighbor_select_selected_total;
		uint64_t neighbor_select_selected_max;
		uint64_t neighbor_select_reject_count;
		uint64_t neighbor_select_heuristic_prune_count;
		uint64_t neighbor_select_distance_recompute_count;
		uint64_t neighbor_select_reused_candidate_score_count;
		uint64_t neighbor_select_duplicate_scan_count;
		uint64_t neighbor_select_worst_scan_count;
		uint64_t neighbor_select_worst_cache_hit_count;
		uint64_t neighbor_select_level0_candidate_total;
		uint64_t neighbor_select_upper_candidate_total;
		uint64_t neighbor_select_level0_selected_total;
		uint64_t neighbor_select_upper_selected_total;
		uint64_t neighbor_select_level0_reject_count;
		uint64_t neighbor_select_upper_reject_count;
		uint64_t neighbor_select_level0_heuristic_prune_count;
		uint64_t neighbor_select_upper_heuristic_prune_count;
		uint64_t neighbor_select_level0_distance_recompute_count;
		uint64_t neighbor_select_upper_distance_recompute_count;
		uint64_t neighbor_select_level0_reused_candidate_score_count;
		uint64_t neighbor_select_upper_reused_candidate_score_count;
		uint64_t neighbor_select_level0_duplicate_scan_count;
		uint64_t neighbor_select_upper_duplicate_scan_count;
		uint64_t neighbor_select_level0_worst_scan_count;
		uint64_t neighbor_select_upper_worst_scan_count;
		uint64_t neighbor_select_level0_worst_cache_hit_count;
		uint64_t neighbor_select_upper_worst_cache_hit_count;
		/* Stage 9A1: bidirectional link acceptance audit counters */
		uint64_t bidirectional_selected_neighbor_count;
		uint64_t bidirectional_forward_edge_attempt_count;
		uint64_t bidirectional_forward_edge_added_count;
		uint64_t bidirectional_reverse_edge_attempt_count;
		uint64_t bidirectional_reverse_edge_accepted_count;
		uint64_t bidirectional_reverse_edge_rejected_count;
		uint64_t bidirectional_forward_edge_skipped_due_to_reverse_reject_count;
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

	/* Removed FAISS backend compatibility VINFO fields; always disabled/0. */
	int faiss_backend_enabled;
	size_t faiss_ntotal;
	uint64_t faiss_add_count;
	uint64_t faiss_search_count;

	/* Removed hnswlib backend compatibility VINFO fields; always disabled/0. */
	int hnswlib_backend_enabled;
	size_t hnswlib_ntotal;
	size_t hnswlib_capacity;
	size_t hnswlib_m;
	size_t hnswlib_ef_construction;
	size_t hnswlib_ef_search;
	uint64_t hnswlib_add_count;
	uint64_t hnswlib_search_count;

	/* Stage 5B: VSEARCH internal profile stats */
	uint64_t vsearch_total_count;
	uint64_t vsearch_total_us;
	uint64_t vsearch_total_max_us;
	uint64_t vsearch_backend_search_count;
	uint64_t vsearch_backend_search_total_us;
	uint64_t vsearch_backend_search_max_us;
	uint64_t vsearch_manager_resolve_count;
	uint64_t vsearch_manager_resolve_total_us;
	uint64_t vsearch_manager_resolve_max_us;
	uint64_t vsearch_response_assembly_count;
	uint64_t vsearch_response_assembly_total_us;
	uint64_t vsearch_response_assembly_max_us;
	uint64_t vsearch_raw_hit_count;
	uint64_t vsearch_resolved_hit_count;
	uint64_t vsearch_filtered_hit_count;
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

void kvs_vector_vsearch_record_search(uint64_t elapsed_us);
void kvs_vector_vsearch_record_backend_search(uint64_t elapsed_us);
void kvs_vector_vsearch_record_manager_resolve(
	uint64_t elapsed_us,
	size_t raw_hit_count,
	size_t resolved_hit_count
);
void kvs_vector_vsearch_record_response_build(uint64_t elapsed_us);

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
