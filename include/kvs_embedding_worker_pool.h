#ifndef KVS_EMBEDDING_WORKER_POOL_H
#define KVS_EMBEDDING_WORKER_POOL_H

#include <stdint.h>

#include "kvs_command_task.h"

typedef struct kvs_embedding_worker_pool_stats_s {
	uint64_t submitted_prepare_tasks;
	uint64_t completed_prepare_tasks;
	uint64_t failed_prepare_tasks;
	uint64_t prepare_queue_depth;
	uint64_t prepare_queue_max_depth;
	uint64_t worker_threads;
	uint64_t submit_failed;
	uint64_t prepare_dispatch_failed;
	uint64_t prepare_count;
	uint64_t prepare_total_us;
	uint64_t prepare_max_us;
	uint64_t prepare_queue_wait_total_us;
	uint64_t prepare_queue_wait_max_us;
	uint64_t prepared_queue_submit_count;
	uint64_t prepared_queue_submit_total_us;
	uint64_t prepared_queue_submit_max_us;
	uint64_t embedding_batch_enabled;
	uint64_t embedding_batch_vset_enabled;
	uint64_t embedding_batch_vsearch_enabled;
	uint64_t embedding_batch_count;
	uint64_t embedding_batch_items;
	uint64_t embedding_batch_max_observed_size;
	uint64_t embedding_batch_wait_total_us;
	uint64_t embedding_batch_wait_max_us;
	uint64_t embedding_batch_encode_total_us;
	uint64_t embedding_batch_encode_max_us;
	uint64_t embedding_batch_fallback_single_count;
	uint64_t embedding_batch_failed;
	uint64_t embedding_batch_provider_unsupported;
	uint64_t embedding_batch_vset_count;
	uint64_t embedding_batch_vset_items;
	uint64_t embedding_batch_vset_max_observed_size;
	uint64_t embedding_batch_vset_wait_total_us;
	uint64_t embedding_batch_vset_wait_max_us;
	uint64_t embedding_batch_vset_encode_total_us;
	uint64_t embedding_batch_vset_encode_max_us;
	uint64_t embedding_batch_vset_fallback_single_count;
	uint64_t embedding_batch_vsearch_count;
	uint64_t embedding_batch_vsearch_items;
	uint64_t embedding_batch_vsearch_max_observed_size;
	uint64_t embedding_batch_vsearch_wait_total_us;
	uint64_t embedding_batch_vsearch_wait_max_us;
	uint64_t embedding_batch_vsearch_encode_total_us;
	uint64_t embedding_batch_vsearch_encode_max_us;
	uint64_t embedding_batch_vsearch_fallback_single_count;
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
	uint64_t runtime_encode_count;
	uint64_t embedding_encode_total_us;
	uint64_t embedding_encode_max_us;
	uint64_t tokenizer_total_us;
	uint64_t tokenizer_max_us;
	uint64_t ort_run_total_us;
	uint64_t ort_run_max_us;
	uint64_t embedding_runtime_resource_product;
	uint64_t embedding_worker_threads_effective;
	uint64_t embedding_num_threads_effective;
} kvs_embedding_worker_pool_stats_t;

int kvs_embedding_worker_pool_start(int num_threads, int queue_capacity);
void kvs_embedding_worker_pool_stop(void);
int kvs_embedding_worker_pool_submit(kvs_command_task_t *task);
int kvs_embedding_worker_pool_enabled(void);
void kvs_embedding_worker_pool_get_stats(kvs_embedding_worker_pool_stats_t *out);

#endif /* KVS_EMBEDDING_WORKER_POOL_H */
