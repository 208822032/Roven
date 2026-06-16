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
} kvs_embedding_worker_pool_stats_t;

int kvs_embedding_worker_pool_start(int num_threads, int queue_capacity);
void kvs_embedding_worker_pool_stop(void);
int kvs_embedding_worker_pool_submit(kvs_command_task_t *task);
int kvs_embedding_worker_pool_enabled(void);
void kvs_embedding_worker_pool_get_stats(kvs_embedding_worker_pool_stats_t *out);

#endif /* KVS_EMBEDDING_WORKER_POOL_H */
