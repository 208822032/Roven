#include "kvs_command_executor.h"

#include "kvs_async_net_adapter.h"
#include "kvs_config.h"
#include "kvs_embedding_worker_pool.h"
#include "kvs_globals.h"
#include "kvs_log.h"
#include "kvs_memory.h"
#include "kvs_vector.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

typedef struct kvs_command_executor_s {
	pthread_t thread;
	pthread_mutex_t cmd_mu;
	pthread_cond_t cmd_cv;
	kvs_command_task_t *cmd_head;
	kvs_command_task_t *cmd_tail;
	kvs_command_task_t *prepared_head;
	kvs_command_task_t *prepared_tail;
	pthread_mutex_t resp_mu;
	kvs_response_task_t *resp_head;
	kvs_response_task_t *resp_tail;
	int running;
	int stopping;
	int event_fd;
	kvs_command_executor_stats_t stats;
} kvs_command_executor_t;

static kvs_command_executor_t g_executor = {
	.cmd_mu = PTHREAD_MUTEX_INITIALIZER,
	.cmd_cv = PTHREAD_COND_INITIALIZER,
	.resp_mu = PTHREAD_MUTEX_INITIALIZER,
	.event_fd = -1,
};

static uint64_t executor_stats_add(uint64_t *field, uint64_t value) {
	return __atomic_add_fetch(field, value, __ATOMIC_RELAXED);
}

static void executor_stats_sub(uint64_t *field, uint64_t value) {
	__atomic_sub_fetch(field, value, __ATOMIC_RELAXED);
}

static void executor_stats_update_max(uint64_t *field, uint64_t value) {
	uint64_t old_value = __atomic_load_n(field, __ATOMIC_RELAXED);

	while (value > old_value) {
		if (__atomic_compare_exchange_n(field,
		                                &old_value,
		                                value,
		                                0,
		                                __ATOMIC_RELAXED,
		                                __ATOMIC_RELAXED)) {
			break;
		}
	}
}

static uint64_t executor_stats_load(const uint64_t *field) {
	return __atomic_load_n(field, __ATOMIC_RELAXED);
}

static uint64_t executor_now_us(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void executor_record_timing(
	uint64_t *count,
	uint64_t *total_us,
	uint64_t *max_us,
	uint64_t elapsed_us
) {
	executor_stats_add(count, 1);
	executor_stats_add(total_us, elapsed_us);
	executor_stats_update_max(max_us, elapsed_us);
}

static void kvs_command_executor_destroy_response(kvs_response_task_t *resp) {
	if (!resp) {
		return;
	}
	if (resp->session_release && resp->session_owner) {
		void (*release_fn)(void *) = resp->session_release;
		void *owner = resp->session_owner;
		resp->session_release = NULL;
		resp->session_owner = NULL;
		resp->session = NULL;
		release_fn(owner);
	}
	kvs_response_task_destroy(resp);
	kvs_free(resp);
}

static void kvs_command_executor_destroy_task(kvs_command_task_t *task) {
	if (!task) {
		return;
	}
	if (task->response_task) {
		kvs_command_executor_destroy_response(task->response_task);
		task->response_task = NULL;
	}
	if (task->session_release && task->session_owner) {
		void (*release_fn)(void *) = task->session_release;
		void *owner = task->session_owner;
		task->session_release = NULL;
		task->session_owner = NULL;
		task->session = NULL;
		release_fn(owner);
	}
	kvs_command_task_destroy(task);
	kvs_free(task);
}

static void kvs_command_executor_log_embedding_worker_stats(void) {
	kvs_embedding_worker_pool_stats_t stats;
	kvs_command_executor_stats_t exec_stats;
	kvs_vector_vset_commit_stats_t vector_stats;
	uint64_t prepare_avg_us = 0;
	uint64_t prepare_wait_avg_us = 0;
	uint64_t prepared_submit_avg_us = 0;
	uint64_t encode_avg_us = 0;
	uint64_t runtime_encode_avg_us = 0;
	uint64_t tokenizer_avg_us = 0;
	uint64_t ort_run_avg_us = 0;
	uint64_t executor_prepared_wait_avg_us = 0;
	uint64_t prepared_task_execute_avg_us = 0;
	uint64_t execute_sync_avg_us = 0;
	uint64_t commit_latency_avg_us = 0;
	uint64_t commit_or_search_avg_us = 0;
	uint64_t finalize_response_avg_us = 0;
	uint64_t batch_avg_size = 0;
	uint64_t batch_wait_avg_us = 0;
	uint64_t batch_encode_avg_us = 0;
	uint64_t batch_vset_avg_size = 0;
	uint64_t batch_vset_wait_avg_us = 0;
	uint64_t batch_vset_encode_avg_us = 0;
	uint64_t batch_vsearch_avg_size = 0;
	uint64_t batch_vsearch_wait_avg_us = 0;
	uint64_t batch_vsearch_encode_avg_us = 0;
	uint64_t vset_prepared_wait_avg_us = 0;
	uint64_t vset_execute_sync_avg_us = 0;
	uint64_t vset_post_commit_avg_us = 0;
	uint64_t vset_finalize_response_avg_us = 0;
	uint64_t vset_push_response_avg_us = 0;
	uint64_t vsearch_execute_sync_avg_us = 0;
	uint64_t vset_commit_avg_us = 0;
	uint64_t vset_record_lookup_avg_us = 0;
	uint64_t vset_record_insert_or_update_avg_us = 0;
	uint64_t vset_vector_copy_avg_us = 0;
	uint64_t vset_flat_index_update_avg_us = 0;
	uint64_t vset_hnsw_add_avg_us = 0;
	uint64_t vset_id_map_update_avg_us = 0;
	uint64_t vset_scope_answer_question_update_avg_us = 0;
	uint64_t vset_lock_wait_avg_us = 0;
	uint64_t vset_lock_hold_avg_us = 0;
	uint64_t vset_aof_append_avg_us = 0;
	uint64_t vset_replication_avg_us = 0;
	uint64_t vset_dirty_mark_avg_us = 0;
	uint64_t vset_response_build_avg_us = 0;
	uint64_t vset_response_push_avg_us = 0;
	uint64_t vsearch_search_avg_us = 0;

	memset(&stats, 0, sizeof(stats));
	memset(&exec_stats, 0, sizeof(exec_stats));
	memset(&vector_stats, 0, sizeof(vector_stats));
	kvs_embedding_worker_pool_get_stats(&stats);
	kvs_command_executor_get_stats(&exec_stats);
	kvs_vector_vset_commit_stats_get(&vector_stats);
	if (stats.prepare_count > 0) {
		prepare_avg_us = stats.prepare_total_us / stats.prepare_count;
		prepare_wait_avg_us = stats.prepare_queue_wait_total_us / stats.prepare_count;
	}
	if (stats.prepared_queue_submit_count > 0) {
		prepared_submit_avg_us =
			stats.prepared_queue_submit_total_us / stats.prepared_queue_submit_count;
	}
	if (stats.encode_thread_context_count + stats.encode_global_mutex_count > 0) {
		encode_avg_us = stats.encode_total_us /
			(stats.encode_thread_context_count + stats.encode_global_mutex_count);
	}
	if (stats.runtime_encode_count > 0) {
		runtime_encode_avg_us = stats.embedding_encode_total_us /
			stats.runtime_encode_count;
		tokenizer_avg_us = stats.tokenizer_total_us / stats.runtime_encode_count;
		ort_run_avg_us = stats.ort_run_total_us / stats.runtime_encode_count;
	}
	if (exec_stats.prepared_wait_count > 0) {
		executor_prepared_wait_avg_us = exec_stats.prepared_wait_total_us /
			exec_stats.prepared_wait_count;
	}
	if (exec_stats.prepared_task_execute_count > 0) {
		prepared_task_execute_avg_us =
			exec_stats.prepared_task_execute_total_us /
			exec_stats.prepared_task_execute_count;
	}
	if (exec_stats.execute_sync_count > 0) {
		execute_sync_avg_us =
			exec_stats.execute_sync_total_us / exec_stats.execute_sync_count;
	}
	if (exec_stats.commit_latency_count > 0) {
		commit_latency_avg_us =
			exec_stats.commit_latency_total_us / exec_stats.commit_latency_count;
	}
	if (exec_stats.commit_or_search_count > 0) {
		commit_or_search_avg_us =
			exec_stats.commit_or_search_total_us /
			exec_stats.commit_or_search_count;
	}
	if (exec_stats.finalize_response_count > 0) {
		finalize_response_avg_us =
			exec_stats.finalize_response_total_us /
			exec_stats.finalize_response_count;
	}
	if (stats.embedding_batch_count > 0) {
		batch_avg_size =
			stats.embedding_batch_items / stats.embedding_batch_count;
		batch_wait_avg_us =
			stats.embedding_batch_wait_total_us / stats.embedding_batch_count;
		batch_encode_avg_us =
			stats.embedding_batch_encode_total_us / stats.embedding_batch_count;
	}
	if (stats.embedding_batch_vset_count > 0) {
		batch_vset_avg_size =
			stats.embedding_batch_vset_items / stats.embedding_batch_vset_count;
		batch_vset_wait_avg_us =
			stats.embedding_batch_vset_wait_total_us /
			stats.embedding_batch_vset_count;
		batch_vset_encode_avg_us =
			stats.embedding_batch_vset_encode_total_us /
			stats.embedding_batch_vset_count;
	}
	if (stats.embedding_batch_vsearch_count > 0) {
		batch_vsearch_avg_size =
			stats.embedding_batch_vsearch_items /
			stats.embedding_batch_vsearch_count;
		batch_vsearch_wait_avg_us =
			stats.embedding_batch_vsearch_wait_total_us /
			stats.embedding_batch_vsearch_count;
		batch_vsearch_encode_avg_us =
			stats.embedding_batch_vsearch_encode_total_us /
			stats.embedding_batch_vsearch_count;
	}
	if (exec_stats.vset_prepared_tasks > 0) {
		vset_prepared_wait_avg_us =
			exec_stats.vset_prepared_wait_total_us / exec_stats.vset_prepared_tasks;
	}
	if (exec_stats.vset_execute_sync_count > 0) {
		vset_execute_sync_avg_us =
			exec_stats.vset_execute_sync_total_us / exec_stats.vset_execute_sync_count;
	}
	if (exec_stats.vset_post_commit_count > 0) {
		vset_post_commit_avg_us =
			exec_stats.vset_post_commit_total_us / exec_stats.vset_post_commit_count;
	}
	if (exec_stats.vset_finalize_response_count > 0) {
		vset_finalize_response_avg_us =
			exec_stats.vset_finalize_response_total_us / exec_stats.vset_finalize_response_count;
	}
	if (exec_stats.vset_push_response_count > 0) {
		vset_push_response_avg_us =
			exec_stats.vset_push_response_total_us / exec_stats.vset_push_response_count;
	}
	if (exec_stats.vsearch_execute_sync_count > 0) {
		vsearch_execute_sync_avg_us =
			exec_stats.vsearch_execute_sync_total_us / exec_stats.vsearch_execute_sync_count;
	}
	if (vector_stats.vset_commit_count > 0) {
		vset_commit_avg_us =
			vector_stats.vset_commit_total_us / vector_stats.vset_commit_count;
	}
	if (vector_stats.vset_record_lookup_count > 0) {
		vset_record_lookup_avg_us =
			vector_stats.vset_record_lookup_total_us / vector_stats.vset_record_lookup_count;
	}
	if (vector_stats.vset_record_insert_or_update_count > 0) {
		vset_record_insert_or_update_avg_us =
			vector_stats.vset_record_insert_or_update_total_us /
			vector_stats.vset_record_insert_or_update_count;
	}
	if (vector_stats.vset_vector_copy_count > 0) {
		vset_vector_copy_avg_us =
			vector_stats.vset_vector_copy_total_us / vector_stats.vset_vector_copy_count;
	}
	if (vector_stats.vset_flat_index_update_count > 0) {
		vset_flat_index_update_avg_us =
			vector_stats.vset_flat_index_update_total_us /
			vector_stats.vset_flat_index_update_count;
	}
	if (vector_stats.vset_hnsw_add_count > 0) {
		vset_hnsw_add_avg_us =
			vector_stats.vset_hnsw_add_total_us / vector_stats.vset_hnsw_add_count;
	}
	if (vector_stats.vset_id_map_update_count > 0) {
		vset_id_map_update_avg_us =
			vector_stats.vset_id_map_update_total_us /
			vector_stats.vset_id_map_update_count;
	}
	if (vector_stats.vset_scope_answer_question_update_count > 0) {
		vset_scope_answer_question_update_avg_us =
			vector_stats.vset_scope_answer_question_update_total_us /
			vector_stats.vset_scope_answer_question_update_count;
	}
	if (vector_stats.vset_lock_wait_count > 0) {
		vset_lock_wait_avg_us =
			vector_stats.vset_lock_wait_total_us / vector_stats.vset_lock_wait_count;
	}
	if (vector_stats.vset_lock_hold_count > 0) {
		vset_lock_hold_avg_us =
			vector_stats.vset_lock_hold_total_us / vector_stats.vset_lock_hold_count;
	}
	if (vector_stats.vset_aof_append_count > 0) {
		vset_aof_append_avg_us =
			vector_stats.vset_aof_append_total_us / vector_stats.vset_aof_append_count;
	}
	if (vector_stats.vset_replication_count > 0) {
		vset_replication_avg_us =
			vector_stats.vset_replication_total_us / vector_stats.vset_replication_count;
	}
	if (vector_stats.vset_dirty_mark_count > 0) {
		vset_dirty_mark_avg_us =
			vector_stats.vset_dirty_mark_total_us / vector_stats.vset_dirty_mark_count;
	}
	if (vector_stats.vset_response_build_count > 0) {
		vset_response_build_avg_us =
			vector_stats.vset_response_build_total_us /
			vector_stats.vset_response_build_count;
	}
	if (vector_stats.vset_response_push_count > 0) {
		vset_response_push_avg_us =
			vector_stats.vset_response_push_total_us /
			vector_stats.vset_response_push_count;
	}
	if (vector_stats.vsearch_search_count > 0) {
		vsearch_search_avg_us =
			vector_stats.vsearch_search_total_us / vector_stats.vsearch_search_count;
	}

	KVS_LOG_INFO(
		"core",
		"embedding worker pool stats: enabled=%llu submitted=%llu completed=%llu failed=%llu queue_depth=%llu queue_max=%llu worker_threads=%llu submit_failed=%llu dispatch_failed=%llu prepare_count=%llu prepare_avg_us=%llu prepare_max_us=%llu prepare_wait_count=%llu prepare_queue_wait_avg_us=%llu prepare_queue_wait_max_us=%llu prepared_queue_submitted=%llu prepared_queue_failed=%llu prepared_queue_submit_avg_us=%llu prepared_queue_submit_max_us=%llu per_thread_enabled=%llu contexts_created=%llu contexts_destroyed=%llu context_create_failed=%llu context_fallback=%llu encode_global_mutex=%llu encode_thread_context=%llu encode_current_inflight=%llu encode_max_inflight=%llu encode_avg_us=%llu encode_max_us=%llu runtime_encode_count=%llu embedding_encode_avg_us=%llu embedding_encode_max_us=%llu tokenizer_avg_us=%llu tokenizer_max_us=%llu ort_run_avg_us=%llu ort_run_max_us=%llu shared_ort_env_enabled=%llu shared_ort_env_created=%llu shared_ort_env_destroyed=%llu shared_ort_env_refcount_current=%llu shared_ort_env_refcount_max=%llu ort_session_created=%llu ort_session_destroyed=%llu resource_product=%llu worker_threads_effective=%llu embedding_num_threads_effective=%llu",
		(unsigned long long)stats.enabled,
		(unsigned long long)stats.submitted_prepare_tasks,
		(unsigned long long)stats.completed_prepare_tasks,
		(unsigned long long)stats.failed_prepare_tasks,
		(unsigned long long)stats.prepare_queue_depth,
		(unsigned long long)stats.prepare_queue_max_depth,
		(unsigned long long)stats.worker_threads,
		(unsigned long long)stats.submit_failed,
		(unsigned long long)stats.prepare_dispatch_failed,
		(unsigned long long)stats.prepare_count,
		(unsigned long long)prepare_avg_us,
		(unsigned long long)stats.prepare_max_us,
		(unsigned long long)stats.prepare_queue_wait_count,
		(unsigned long long)prepare_wait_avg_us,
		(unsigned long long)stats.prepare_queue_wait_max_us,
		(unsigned long long)stats.prepared_queue_submit_count,
		(unsigned long long)stats.prepared_queue_submit_failed,
		(unsigned long long)prepared_submit_avg_us,
		(unsigned long long)stats.prepared_queue_submit_max_us,
		(unsigned long long)stats.per_thread_context_enabled,
		(unsigned long long)stats.per_thread_context_created,
		(unsigned long long)stats.per_thread_context_destroyed,
		(unsigned long long)stats.per_thread_context_create_failed,
		(unsigned long long)stats.per_thread_context_fallback_count,
		(unsigned long long)stats.encode_global_mutex_count,
		(unsigned long long)stats.encode_thread_context_count,
		(unsigned long long)stats.encode_current_inflight,
		(unsigned long long)stats.encode_max_inflight,
		(unsigned long long)encode_avg_us,
		(unsigned long long)stats.encode_max_us,
		(unsigned long long)stats.runtime_encode_count,
		(unsigned long long)runtime_encode_avg_us,
		(unsigned long long)stats.embedding_encode_max_us,
		(unsigned long long)tokenizer_avg_us,
		(unsigned long long)stats.tokenizer_max_us,
		(unsigned long long)ort_run_avg_us,
		(unsigned long long)stats.ort_run_max_us,
		(unsigned long long)stats.shared_ort_env_enabled,
		(unsigned long long)stats.shared_ort_env_created,
		(unsigned long long)stats.shared_ort_env_destroyed,
		(unsigned long long)stats.shared_ort_env_refcount_current,
		(unsigned long long)stats.shared_ort_env_refcount_max,
		(unsigned long long)stats.ort_session_created,
		(unsigned long long)stats.ort_session_destroyed,
		(unsigned long long)stats.embedding_runtime_resource_product,
		(unsigned long long)stats.embedding_worker_threads_effective,
		(unsigned long long)stats.embedding_num_threads_effective
	);
	KVS_LOG_INFO(
		"core",
		"embedding executor bottleneck stats: normal_task_executed=%llu prepare_task_submitted=%llu prepared_task_executed=%llu prepared_task_failed=%llu prepared_queue_depth=%llu prepared_queue_max=%llu prepared_wait_count=%llu prepared_wait_avg_us=%llu prepared_wait_max_us=%llu prepared_task_execute_count=%llu prepared_task_execute_avg_us=%llu prepared_task_execute_max_us=%llu execute_sync_count=%llu execute_sync_avg_us=%llu execute_sync_max_us=%llu commit_latency_count=%llu commit_latency_avg_us=%llu commit_latency_total_us=%llu commit_latency_max_us=%llu commit_or_search_count=%llu commit_or_search_avg_us=%llu commit_or_search_max_us=%llu finalize_response_count=%llu finalize_response_avg_us=%llu finalize_response_max_us=%llu",
		(unsigned long long)exec_stats.normal_task_executed,
		(unsigned long long)exec_stats.prepare_task_submitted,
		(unsigned long long)exec_stats.prepared_task_executed,
		(unsigned long long)exec_stats.prepared_task_failed,
		(unsigned long long)exec_stats.prepared_queue_depth,
		(unsigned long long)exec_stats.prepared_queue_max_depth,
		(unsigned long long)exec_stats.prepared_wait_count,
		(unsigned long long)executor_prepared_wait_avg_us,
		(unsigned long long)exec_stats.prepared_wait_max_us,
		(unsigned long long)exec_stats.prepared_task_execute_count,
		(unsigned long long)prepared_task_execute_avg_us,
		(unsigned long long)exec_stats.prepared_task_execute_max_us,
		(unsigned long long)exec_stats.execute_sync_count,
		(unsigned long long)execute_sync_avg_us,
		(unsigned long long)exec_stats.execute_sync_max_us,
		(unsigned long long)exec_stats.commit_latency_count,
		(unsigned long long)commit_latency_avg_us,
		(unsigned long long)exec_stats.commit_latency_total_us,
		(unsigned long long)exec_stats.commit_latency_max_us,
		(unsigned long long)exec_stats.commit_or_search_count,
		(unsigned long long)commit_or_search_avg_us,
		(unsigned long long)exec_stats.commit_or_search_max_us,
		(unsigned long long)exec_stats.finalize_response_count,
		(unsigned long long)finalize_response_avg_us,
		(unsigned long long)exec_stats.finalize_response_max_us
	);
	KVS_LOG_INFO(
		"core",
		"embedding batch stats: batch_enabled=%llu batch_count=%llu batch_items=%llu batch_avg_size=%llu batch_max_observed_size=%llu batch_wait_avg_us=%llu batch_wait_max_us=%llu batch_encode_avg_us=%llu batch_encode_max_us=%llu batch_fallback_single_count=%llu batch_failed=%llu batch_provider_unsupported=%llu",
		(unsigned long long)stats.embedding_batch_enabled,
		(unsigned long long)stats.embedding_batch_count,
		(unsigned long long)stats.embedding_batch_items,
		(unsigned long long)batch_avg_size,
		(unsigned long long)stats.embedding_batch_max_observed_size,
		(unsigned long long)batch_wait_avg_us,
		(unsigned long long)stats.embedding_batch_wait_max_us,
		(unsigned long long)batch_encode_avg_us,
		(unsigned long long)stats.embedding_batch_encode_max_us,
		(unsigned long long)stats.embedding_batch_fallback_single_count,
		(unsigned long long)stats.embedding_batch_failed,
		(unsigned long long)stats.embedding_batch_provider_unsupported
	);
	KVS_LOG_INFO(
		"core",
		"embedding command batch stats: embedding_batch_vset_enabled=%llu embedding_batch_vsearch_enabled=%llu embedding_batch_vset_count=%llu embedding_batch_vset_items=%llu embedding_batch_vset_avg_size=%llu embedding_batch_vset_max_observed_size=%llu embedding_batch_vset_wait_avg_us=%llu embedding_batch_vset_wait_max_us=%llu embedding_batch_vset_encode_avg_us=%llu embedding_batch_vset_encode_max_us=%llu embedding_batch_vset_fallback_single_count=%llu embedding_batch_vsearch_count=%llu embedding_batch_vsearch_items=%llu embedding_batch_vsearch_avg_size=%llu embedding_batch_vsearch_max_observed_size=%llu embedding_batch_vsearch_wait_avg_us=%llu embedding_batch_vsearch_wait_max_us=%llu embedding_batch_vsearch_encode_avg_us=%llu embedding_batch_vsearch_encode_max_us=%llu embedding_batch_vsearch_fallback_single_count=%llu",
		(unsigned long long)stats.embedding_batch_vset_enabled,
		(unsigned long long)stats.embedding_batch_vsearch_enabled,
		(unsigned long long)stats.embedding_batch_vset_count,
		(unsigned long long)stats.embedding_batch_vset_items,
		(unsigned long long)batch_vset_avg_size,
		(unsigned long long)stats.embedding_batch_vset_max_observed_size,
		(unsigned long long)batch_vset_wait_avg_us,
		(unsigned long long)stats.embedding_batch_vset_wait_max_us,
		(unsigned long long)batch_vset_encode_avg_us,
		(unsigned long long)stats.embedding_batch_vset_encode_max_us,
		(unsigned long long)stats.embedding_batch_vset_fallback_single_count,
		(unsigned long long)stats.embedding_batch_vsearch_count,
		(unsigned long long)stats.embedding_batch_vsearch_items,
		(unsigned long long)batch_vsearch_avg_size,
		(unsigned long long)stats.embedding_batch_vsearch_max_observed_size,
		(unsigned long long)batch_vsearch_wait_avg_us,
		(unsigned long long)stats.embedding_batch_vsearch_wait_max_us,
		(unsigned long long)batch_vsearch_encode_avg_us,
		(unsigned long long)stats.embedding_batch_vsearch_encode_max_us,
		(unsigned long long)stats.embedding_batch_vsearch_fallback_single_count
	);
	KVS_LOG_INFO(
		"core",
		"vset executor breakdown stats: vset_prepared_tasks=%llu vset_prepared_wait_avg_us=%llu vset_prepared_wait_total_us=%llu vset_prepared_wait_max_us=%llu vset_execute_sync_count=%llu vset_execute_sync_avg_us=%llu vset_execute_sync_total_us=%llu vset_execute_sync_max_us=%llu vset_post_commit_count=%llu vset_post_commit_avg_us=%llu vset_post_commit_total_us=%llu vset_post_commit_max_us=%llu vset_finalize_response_count=%llu vset_finalize_response_avg_us=%llu vset_finalize_response_total_us=%llu vset_finalize_response_max_us=%llu vset_push_response_count=%llu vset_push_response_avg_us=%llu vset_push_response_total_us=%llu vset_push_response_max_us=%llu vsearch_execute_sync_count=%llu vsearch_execute_sync_avg_us=%llu vsearch_execute_sync_total_us=%llu vsearch_execute_sync_max_us=%llu",
		(unsigned long long)exec_stats.vset_prepared_tasks,
		(unsigned long long)vset_prepared_wait_avg_us,
		(unsigned long long)exec_stats.vset_prepared_wait_total_us,
		(unsigned long long)exec_stats.vset_prepared_wait_max_us,
		(unsigned long long)exec_stats.vset_execute_sync_count,
		(unsigned long long)vset_execute_sync_avg_us,
		(unsigned long long)exec_stats.vset_execute_sync_total_us,
		(unsigned long long)exec_stats.vset_execute_sync_max_us,
		(unsigned long long)exec_stats.vset_post_commit_count,
		(unsigned long long)vset_post_commit_avg_us,
		(unsigned long long)exec_stats.vset_post_commit_total_us,
		(unsigned long long)exec_stats.vset_post_commit_max_us,
		(unsigned long long)exec_stats.vset_finalize_response_count,
		(unsigned long long)vset_finalize_response_avg_us,
		(unsigned long long)exec_stats.vset_finalize_response_total_us,
		(unsigned long long)exec_stats.vset_finalize_response_max_us,
		(unsigned long long)exec_stats.vset_push_response_count,
		(unsigned long long)vset_push_response_avg_us,
		(unsigned long long)exec_stats.vset_push_response_total_us,
		(unsigned long long)exec_stats.vset_push_response_max_us,
		(unsigned long long)exec_stats.vsearch_execute_sync_count,
		(unsigned long long)vsearch_execute_sync_avg_us,
		(unsigned long long)exec_stats.vsearch_execute_sync_total_us,
		(unsigned long long)exec_stats.vsearch_execute_sync_max_us
	);
	KVS_LOG_INFO(
		"core",
		"vset commit breakdown stats: vset_commit_count=%llu vset_commit_avg_us=%llu vset_commit_total_us=%llu vset_commit_max_us=%llu vset_record_lookup_count=%llu vset_record_lookup_avg_us=%llu vset_record_lookup_total_us=%llu vset_record_lookup_max_us=%llu vset_record_insert_or_update_count=%llu vset_record_insert_or_update_avg_us=%llu vset_record_insert_or_update_total_us=%llu vset_record_insert_or_update_max_us=%llu vset_vector_copy_count=%llu vset_vector_copy_avg_us=%llu vset_vector_copy_total_us=%llu vset_vector_copy_max_us=%llu",
		(unsigned long long)vector_stats.vset_commit_count,
		(unsigned long long)vset_commit_avg_us,
		(unsigned long long)vector_stats.vset_commit_total_us,
		(unsigned long long)vector_stats.vset_commit_max_us,
		(unsigned long long)vector_stats.vset_record_lookup_count,
		(unsigned long long)vset_record_lookup_avg_us,
		(unsigned long long)vector_stats.vset_record_lookup_total_us,
		(unsigned long long)vector_stats.vset_record_lookup_max_us,
		(unsigned long long)vector_stats.vset_record_insert_or_update_count,
		(unsigned long long)vset_record_insert_or_update_avg_us,
		(unsigned long long)vector_stats.vset_record_insert_or_update_total_us,
		(unsigned long long)vector_stats.vset_record_insert_or_update_max_us,
		(unsigned long long)vector_stats.vset_vector_copy_count,
		(unsigned long long)vset_vector_copy_avg_us,
		(unsigned long long)vector_stats.vset_vector_copy_total_us,
		(unsigned long long)vector_stats.vset_vector_copy_max_us
	);
	KVS_LOG_INFO(
		"core",
		"vset index breakdown stats: vset_flat_index_update_count=%llu vset_flat_index_update_avg_us=%llu vset_flat_index_update_total_us=%llu vset_flat_index_update_max_us=%llu vset_hnsw_add_count=%llu vset_hnsw_add_avg_us=%llu vset_hnsw_add_total_us=%llu vset_hnsw_add_max_us=%llu vset_id_map_update_count=%llu vset_id_map_update_avg_us=%llu vset_id_map_update_total_us=%llu vset_id_map_update_max_us=%llu vset_scope_answer_question_update_count=%llu vset_scope_answer_question_update_avg_us=%llu vset_scope_answer_question_update_total_us=%llu vset_scope_answer_question_update_max_us=%llu",
		(unsigned long long)vector_stats.vset_flat_index_update_count,
		(unsigned long long)vset_flat_index_update_avg_us,
		(unsigned long long)vector_stats.vset_flat_index_update_total_us,
		(unsigned long long)vector_stats.vset_flat_index_update_max_us,
		(unsigned long long)vector_stats.vset_hnsw_add_count,
		(unsigned long long)vset_hnsw_add_avg_us,
		(unsigned long long)vector_stats.vset_hnsw_add_total_us,
		(unsigned long long)vector_stats.vset_hnsw_add_max_us,
		(unsigned long long)vector_stats.vset_id_map_update_count,
		(unsigned long long)vset_id_map_update_avg_us,
		(unsigned long long)vector_stats.vset_id_map_update_total_us,
		(unsigned long long)vector_stats.vset_id_map_update_max_us,
		(unsigned long long)vector_stats.vset_scope_answer_question_update_count,
		(unsigned long long)vset_scope_answer_question_update_avg_us,
		(unsigned long long)vector_stats.vset_scope_answer_question_update_total_us,
		(unsigned long long)vector_stats.vset_scope_answer_question_update_max_us
	);
	KVS_LOG_INFO(
		"core",
		"vset side-effect breakdown stats: vset_lock_wait_count=%llu vset_lock_wait_avg_us=%llu vset_lock_wait_total_us=%llu vset_lock_wait_max_us=%llu vset_lock_hold_count=%llu vset_lock_hold_avg_us=%llu vset_lock_hold_total_us=%llu vset_lock_hold_max_us=%llu vset_aof_append_count=%llu vset_aof_append_avg_us=%llu vset_aof_append_total_us=%llu vset_aof_append_max_us=%llu vset_replication_count=%llu vset_replication_avg_us=%llu vset_replication_total_us=%llu vset_replication_max_us=%llu vset_dirty_mark_count=%llu vset_dirty_mark_avg_us=%llu vset_dirty_mark_total_us=%llu vset_dirty_mark_max_us=%llu vset_response_build_count=%llu vset_response_build_avg_us=%llu vset_response_build_total_us=%llu vset_response_build_max_us=%llu vset_response_push_count=%llu vset_response_push_avg_us=%llu vset_response_push_total_us=%llu vset_response_push_max_us=%llu vsearch_search_count=%llu vsearch_search_avg_us=%llu vsearch_search_total_us=%llu vsearch_search_max_us=%llu",
		(unsigned long long)vector_stats.vset_lock_wait_count,
		(unsigned long long)vset_lock_wait_avg_us,
		(unsigned long long)vector_stats.vset_lock_wait_total_us,
		(unsigned long long)vector_stats.vset_lock_wait_max_us,
		(unsigned long long)vector_stats.vset_lock_hold_count,
		(unsigned long long)vset_lock_hold_avg_us,
		(unsigned long long)vector_stats.vset_lock_hold_total_us,
		(unsigned long long)vector_stats.vset_lock_hold_max_us,
		(unsigned long long)vector_stats.vset_aof_append_count,
		(unsigned long long)vset_aof_append_avg_us,
		(unsigned long long)vector_stats.vset_aof_append_total_us,
		(unsigned long long)vector_stats.vset_aof_append_max_us,
		(unsigned long long)vector_stats.vset_replication_count,
		(unsigned long long)vset_replication_avg_us,
		(unsigned long long)vector_stats.vset_replication_total_us,
		(unsigned long long)vector_stats.vset_replication_max_us,
		(unsigned long long)vector_stats.vset_dirty_mark_count,
		(unsigned long long)vset_dirty_mark_avg_us,
		(unsigned long long)vector_stats.vset_dirty_mark_total_us,
		(unsigned long long)vector_stats.vset_dirty_mark_max_us,
		(unsigned long long)vector_stats.vset_response_build_count,
		(unsigned long long)vset_response_build_avg_us,
		(unsigned long long)vector_stats.vset_response_build_total_us,
		(unsigned long long)vector_stats.vset_response_build_max_us,
		(unsigned long long)vector_stats.vset_response_push_count,
		(unsigned long long)vset_response_push_avg_us,
		(unsigned long long)vector_stats.vset_response_push_total_us,
		(unsigned long long)vector_stats.vset_response_push_max_us,
		(unsigned long long)vector_stats.vsearch_search_count,
		(unsigned long long)vsearch_search_avg_us,
		(unsigned long long)vector_stats.vsearch_search_total_us,
		(unsigned long long)vector_stats.vsearch_search_max_us
	);
}

static void kvs_command_executor_push_response(kvs_response_task_t *resp) {
	uint64_t one = 1;
	uint64_t depth;
	int write_failed = 0;

	if (!resp) {
		return;
	}
	resp->queue_next = NULL;

	pthread_mutex_lock(&g_executor.resp_mu);
	if (g_executor.resp_tail) {
		g_executor.resp_tail->queue_next = resp;
	} else {
		g_executor.resp_head = resp;
	}
	g_executor.resp_tail = resp;
	depth = executor_stats_add(&g_executor.stats.response_queue_depth, 1);
	executor_stats_update_max(&g_executor.stats.response_queue_max_depth, depth);
	executor_stats_add(&g_executor.stats.pushed_responses, 1);
	pthread_mutex_unlock(&g_executor.resp_mu);

	executor_stats_add(&g_executor.stats.eventfd_wakeup_count, 1);
	if (g_executor.event_fd >= 0) {
		for (;;) {
			ssize_t n = write(g_executor.event_fd, &one, sizeof(one));
			if (n == (ssize_t)sizeof(one)) {
				break;
			}
			if (n < 0 && errno == EINTR) {
				continue;
			}
			write_failed = 1;
			break;
		}
	} else {
		write_failed = 1;
	}
	if (write_failed) {
		executor_stats_add(&g_executor.stats.eventfd_write_fail_count, 1);
	}
}

static kvs_command_task_t *kvs_command_executor_pop_command_locked(void) {
	kvs_command_task_t *task = g_executor.cmd_head;

	if (!task) {
		return NULL;
	}
	g_executor.cmd_head = task->queue_next;
	if (!g_executor.cmd_head) {
		g_executor.cmd_tail = NULL;
	}
	task->queue_next = NULL;
	executor_stats_sub(&g_executor.stats.command_queue_depth, 1);
	return task;
}

static kvs_command_task_t *kvs_command_executor_pop_prepared_locked(void) {
	kvs_command_task_t *task = g_executor.prepared_head;

	if (!task) {
		return NULL;
	}
	g_executor.prepared_head = task->queue_next;
	if (!g_executor.prepared_head) {
		g_executor.prepared_tail = NULL;
	}
	task->queue_next = NULL;
	executor_stats_sub(&g_executor.stats.prepared_queue_depth, 1);
	return task;
}

static int kvs_command_executor_should_async_prepare(kvs_command_task_t *task) {
	return kvs_embedding_worker_pool_enabled() &&
	       kvs_command_task_requires_embedding_prepare(task) &&
	       !task->prepare_dispatched &&
	       !task->prepare_completed;
}

static void kvs_command_executor_execute(kvs_command_task_t *task) {
	kvs_response_task_t *resp;
	int ret;
	int prepared_task;
	int is_vset_prepared;
	int is_vsearch_prepared;
	uint64_t total_start_us = 0;
	uint64_t start_us = 0;
	uint64_t end_us = 0;

	if (!task) {
		return;
	}
	prepared_task = task->prepare_completed;
	is_vset_prepared = prepared_task &&
		task->prepared_kind == KVS_COMMAND_TASK_PREPARED_VSET;
	is_vsearch_prepared = prepared_task &&
		task->prepared_kind == KVS_COMMAND_TASK_PREPARED_VSEARCH;
	if (prepared_task) {
		total_start_us = executor_now_us();
	}
	resp = task->response_task;
	if (!resp) {
		kvs_command_executor_destroy_task(task);
		return;
	}

	resp->session = task->session;
	resp->session_owner = task->session_owner;
	resp->session_release = task->session_release;
	resp->fd = task->fd;
	resp->session_id = task->session_id;
	resp->generation = task->generation;
	resp->request_seq = task->request_seq;
	resp->owner_type = task->owner_type;
	resp->owner_poller_id = task->owner_poller_id;

	task->session = NULL;
	task->session_owner = NULL;
	task->session_release = NULL;

	if (task->build_status > 0) {
		ret = task->build_status;
	} else if (task->prepare_completed) {
		ret = task->prepare_status;
		if (ret == 0) {
			start_us = executor_now_us();
			ret = kvs_command_task_execute_sync(task);
			end_us = executor_now_us();
			executor_record_timing(
				&g_executor.stats.commit_latency_count,
				&g_executor.stats.commit_latency_total_us,
				&g_executor.stats.commit_latency_max_us,
				end_us >= start_us ? end_us - start_us : 0
			);
			executor_record_timing(
				&g_executor.stats.execute_sync_count,
				&g_executor.stats.execute_sync_total_us,
				&g_executor.stats.execute_sync_max_us,
				end_us >= start_us ? end_us - start_us : 0
			);
			if (is_vset_prepared) {
				executor_record_timing(
					&g_executor.stats.vset_execute_sync_count,
					&g_executor.stats.vset_execute_sync_total_us,
					&g_executor.stats.vset_execute_sync_max_us,
					end_us >= start_us ? end_us - start_us : 0
				);
				kvs_vector_vset_record_lock_wait(task->lock_wait_us);
				kvs_vector_vset_record_lock_hold(task->lock_hold_us);
			} else if (is_vsearch_prepared) {
				executor_record_timing(
					&g_executor.stats.vsearch_execute_sync_count,
					&g_executor.stats.vsearch_execute_sync_total_us,
					&g_executor.stats.vsearch_execute_sync_max_us,
					end_us >= start_us ? end_us - start_us : 0
				);
				kvs_vector_vsearch_record_search(
					kvs_command_task_get_vsearch_search_us(task)
				);
			}
			start_us = executor_now_us();
			kvs_command_task_post_commit(task, ret);
			end_us = executor_now_us();
			executor_record_timing(
				&g_executor.stats.commit_or_search_count,
				&g_executor.stats.commit_or_search_total_us,
				&g_executor.stats.commit_or_search_max_us,
				end_us >= start_us ? end_us - start_us : 0
			);
			if (is_vset_prepared) {
				uint64_t aof_us = 0;
				uint64_t repl_us = 0;
				uint64_t dirty_us = 0;
				executor_record_timing(
					&g_executor.stats.vset_post_commit_count,
					&g_executor.stats.vset_post_commit_total_us,
					&g_executor.stats.vset_post_commit_max_us,
					end_us >= start_us ? end_us - start_us : 0
				);
				kvs_command_task_get_vset_post_commit_timings(
					task,
					&aof_us,
					&repl_us,
					&dirty_us
				);
				kvs_vector_vset_record_aof_append(aof_us);
				kvs_vector_vset_record_replication(repl_us);
				if (dirty_us > 0) {
					kvs_vector_vset_record_dirty_mark(dirty_us);
				}
			}
		}
		if (ret == 0 || task->command_committed) {
			executor_stats_add(&g_executor.stats.prepared_task_executed, 1);
		} else {
			executor_stats_add(&g_executor.stats.prepared_task_failed, 1);
		}
	} else {
		executor_stats_add(&g_executor.stats.normal_task_executed, 1);
		ret = kvs_command_task_prepare(task);
		if (ret == 0) {
			ret = kvs_command_task_execute_sync(task);
			kvs_command_task_post_commit(task, ret);
		}
	}

	start_us = prepared_task ? executor_now_us() : 0;
	kvs_response_task_finalize(resp, ret);
	if (prepared_task) {
		end_us = executor_now_us();
		executor_record_timing(
			&g_executor.stats.finalize_response_count,
			&g_executor.stats.finalize_response_total_us,
			&g_executor.stats.finalize_response_max_us,
			end_us >= start_us ? end_us - start_us : 0
		);
		if (is_vset_prepared) {
			executor_record_timing(
				&g_executor.stats.vset_finalize_response_count,
				&g_executor.stats.vset_finalize_response_total_us,
				&g_executor.stats.vset_finalize_response_max_us,
				end_us >= start_us ? end_us - start_us : 0
			);
			kvs_vector_vset_record_response_build(
				end_us >= start_us ? end_us - start_us : 0
			);
		}
	}
	task->response_task = NULL;
	kvs_command_task_destroy(task);
	kvs_free(task);
	executor_stats_add(&g_executor.stats.completed_tasks, 1);
	start_us = is_vset_prepared ? executor_now_us() : 0;
	kvs_command_executor_push_response(resp);
	if (is_vset_prepared) {
		end_us = executor_now_us();
		executor_record_timing(
			&g_executor.stats.vset_push_response_count,
			&g_executor.stats.vset_push_response_total_us,
			&g_executor.stats.vset_push_response_max_us,
			end_us >= start_us ? end_us - start_us : 0
		);
		kvs_vector_vset_record_response_push(
			end_us >= start_us ? end_us - start_us : 0
		);
	}
	if (prepared_task) {
		end_us = executor_now_us();
		executor_record_timing(
			&g_executor.stats.prepared_task_execute_count,
			&g_executor.stats.prepared_task_execute_total_us,
			&g_executor.stats.prepared_task_execute_max_us,
			end_us >= total_start_us ? end_us - total_start_us : 0
		);
	}
}

static int kvs_command_executor_dispatch_or_execute(kvs_command_task_t *task) {
	if (!task) {
		return 0;
	}
	if (kvs_command_executor_should_async_prepare(task)) {
		task->prepare_dispatched = 1;
		if (kvs_embedding_worker_pool_submit(task) == 0) {
			executor_stats_add(&g_executor.stats.embedding_prepare_dispatched, 1);
			executor_stats_add(&g_executor.stats.prepare_task_submitted, 1);
			return 1;
		}
		task->prepare_dispatched = 0;
		executor_stats_add(&g_executor.stats.embedding_prepare_submit_failed, 1);
		executor_stats_add(&g_executor.stats.embedding_prepare_inline, 1);
	}
	kvs_command_executor_execute(task);
	return 1;
}

static void *kvs_command_executor_thread_main(void *arg) {
	(void)arg;

	for (;;) {
		kvs_command_task_t *task;

		pthread_mutex_lock(&g_executor.cmd_mu);
		while (!g_executor.prepared_head && !g_executor.cmd_head && !g_executor.stopping) {
			pthread_cond_wait(&g_executor.cmd_cv, &g_executor.cmd_mu);
		}
		if (!g_executor.prepared_head && !g_executor.cmd_head && g_executor.stopping) {
			pthread_mutex_unlock(&g_executor.cmd_mu);
			break;
		}
		task = kvs_command_executor_pop_prepared_locked();
		if (!task) {
			task = kvs_command_executor_pop_command_locked();
		} else if (task->prepared_enqueue_us > 0) {
			uint64_t now_us = executor_now_us();
			uint64_t wait_us = now_us >= task->prepared_enqueue_us ?
				now_us - task->prepared_enqueue_us :
				0;
			executor_record_timing(
				&g_executor.stats.prepared_wait_count,
				&g_executor.stats.prepared_wait_total_us,
				&g_executor.stats.prepared_wait_max_us,
				wait_us
			);
			if (task->prepared_kind == KVS_COMMAND_TASK_PREPARED_VSET) {
				executor_stats_add(&g_executor.stats.vset_prepared_tasks, 1);
				executor_stats_add(
					&g_executor.stats.vset_prepared_wait_total_us,
					wait_us
				);
				executor_stats_update_max(
					&g_executor.stats.vset_prepared_wait_max_us,
					wait_us
				);
			}
		}
		pthread_mutex_unlock(&g_executor.cmd_mu);

		kvs_command_executor_dispatch_or_execute(task);
	}

	return NULL;
}

int kvs_command_executor_start(void) {
	pthread_mutex_lock(&g_executor.cmd_mu);
	if (g_executor.running) {
		pthread_mutex_unlock(&g_executor.cmd_mu);
		return 0;
	}
	g_executor.stopping = 0;
	pthread_mutex_unlock(&g_executor.cmd_mu);

	if (g_cfg.embedding_worker_pool_enabled) {
		if (kvs_embedding_worker_pool_start(
			    g_cfg.embedding_worker_threads,
			    g_cfg.embedding_worker_queue_capacity) != 0) {
			return -1;
		}
	}

	g_executor.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (g_executor.event_fd < 0) {
		kvs_embedding_worker_pool_stop();
		return -1;
	}

	if (pthread_create(&g_executor.thread, NULL, kvs_command_executor_thread_main, NULL) != 0) {
		close(g_executor.event_fd);
		g_executor.event_fd = -1;
		kvs_embedding_worker_pool_stop();
		return -1;
	}

	pthread_mutex_lock(&g_executor.cmd_mu);
	g_executor.running = 1;
	pthread_mutex_unlock(&g_executor.cmd_mu);
	return 0;
}

void kvs_command_executor_stop(void) {
	kvs_command_task_t *task;
	kvs_command_task_t *prepared;
	kvs_response_task_t *resp;
	int should_join;

	kvs_embedding_worker_pool_stop();
	if (g_cfg.embedding_worker_pool_enabled) {
		kvs_command_executor_log_embedding_worker_stats();
	}

	pthread_mutex_lock(&g_executor.cmd_mu);
	should_join = g_executor.running;
	g_executor.stopping = 1;
	pthread_cond_broadcast(&g_executor.cmd_cv);
	pthread_mutex_unlock(&g_executor.cmd_mu);

	if (should_join) {
		pthread_join(g_executor.thread, NULL);
	}

	pthread_mutex_lock(&g_executor.cmd_mu);
	g_executor.running = 0;
	task = g_executor.cmd_head;
	g_executor.cmd_head = NULL;
	g_executor.cmd_tail = NULL;
	prepared = g_executor.prepared_head;
	g_executor.prepared_head = NULL;
	g_executor.prepared_tail = NULL;
	pthread_mutex_unlock(&g_executor.cmd_mu);
	while (task) {
		kvs_command_task_t *next = task->queue_next;
		task->queue_next = NULL;
		executor_stats_sub(&g_executor.stats.command_queue_depth, 1);
		kvs_command_executor_destroy_task(task);
		task = next;
	}
	while (prepared) {
		kvs_command_task_t *next = prepared->queue_next;
		prepared->queue_next = NULL;
		executor_stats_sub(&g_executor.stats.prepared_queue_depth, 1);
		kvs_command_executor_destroy_task(prepared);
		prepared = next;
	}

	pthread_mutex_lock(&g_executor.resp_mu);
	resp = g_executor.resp_head;
	g_executor.resp_head = NULL;
	g_executor.resp_tail = NULL;
	pthread_mutex_unlock(&g_executor.resp_mu);
	while (resp) {
		kvs_response_task_t *next = resp->queue_next;
		resp->queue_next = NULL;
		executor_stats_sub(&g_executor.stats.response_queue_depth, 1);
		kvs_command_executor_destroy_response(resp);
		resp = next;
	}

	if (g_executor.event_fd >= 0) {
		close(g_executor.event_fd);
		g_executor.event_fd = -1;
	}
}

int kvs_command_executor_submit_prepared(kvs_command_task_t *task) {
	uint64_t depth;

	if (!task) {
		return -1;
	}
	task->queue_next = NULL;

	pthread_mutex_lock(&g_executor.cmd_mu);
	if (!g_executor.running) {
		pthread_mutex_unlock(&g_executor.cmd_mu);
		executor_stats_add(&g_executor.stats.submit_failed, 1);
		return -1;
	}
	task->prepared_enqueue_us = executor_now_us();
	if (g_executor.prepared_tail) {
		g_executor.prepared_tail->queue_next = task;
	} else {
		g_executor.prepared_head = task;
	}
	g_executor.prepared_tail = task;
	depth = executor_stats_add(&g_executor.stats.prepared_queue_depth, 1);
	executor_stats_update_max(&g_executor.stats.prepared_queue_max_depth, depth);
	executor_stats_add(&g_executor.stats.prepared_tasks, 1);
	pthread_cond_signal(&g_executor.cmd_cv);
	pthread_mutex_unlock(&g_executor.cmd_mu);
	return 0;
}

int kvs_command_executor_submit(kvs_command_task_t *task) {
	uint64_t depth;

	if (!task) {
		return -1;
	}
	task->queue_next = NULL;

	pthread_mutex_lock(&g_executor.cmd_mu);
	if (!g_executor.running || g_executor.stopping) {
		pthread_mutex_unlock(&g_executor.cmd_mu);
		executor_stats_add(&g_executor.stats.submit_failed, 1);
		return -1;
	}
	if (g_executor.cmd_tail) {
		g_executor.cmd_tail->queue_next = task;
	} else {
		g_executor.cmd_head = task;
	}
	g_executor.cmd_tail = task;
	depth = executor_stats_add(&g_executor.stats.command_queue_depth, 1);
	executor_stats_update_max(&g_executor.stats.command_queue_max_depth, depth);
	executor_stats_add(&g_executor.stats.submitted_tasks, 1);
	pthread_cond_signal(&g_executor.cmd_cv);
	pthread_mutex_unlock(&g_executor.cmd_mu);
	return 0;
}

int kvs_command_executor_poll_response(kvs_response_task_t **out) {
	kvs_response_task_t *resp;

	if (!out) {
		return -1;
	}
	*out = NULL;

	pthread_mutex_lock(&g_executor.resp_mu);
	resp = g_executor.resp_head;
	if (!resp) {
		pthread_mutex_unlock(&g_executor.resp_mu);
		return 0;
	}
	g_executor.resp_head = resp->queue_next;
	if (!g_executor.resp_head) {
		g_executor.resp_tail = NULL;
	}
	resp->queue_next = NULL;
	executor_stats_sub(&g_executor.stats.response_queue_depth, 1);
	executor_stats_add(&g_executor.stats.polled_responses, 1);
	pthread_mutex_unlock(&g_executor.resp_mu);

	*out = resp;
	return 1;
}

int kvs_command_executor_poll_response_for(
	int owner_type,
	int owner_poller_id,
	kvs_response_task_t **out
) {
	kvs_response_task_t *prev = NULL;
	kvs_response_task_t *resp;

	if (!out) {
		return -1;
	}
	*out = NULL;

	pthread_mutex_lock(&g_executor.resp_mu);
	resp = g_executor.resp_head;
	while (resp) {
		if (resp->owner_type == owner_type &&
		    resp->owner_poller_id == owner_poller_id) {
			if (prev) {
				prev->queue_next = resp->queue_next;
			} else {
				g_executor.resp_head = resp->queue_next;
			}
			if (g_executor.resp_tail == resp) {
				g_executor.resp_tail = prev;
			}
			resp->queue_next = NULL;
			executor_stats_sub(&g_executor.stats.response_queue_depth, 1);
			executor_stats_add(&g_executor.stats.polled_responses, 1);
			pthread_mutex_unlock(&g_executor.resp_mu);
			*out = resp;
			return 1;
		}
		prev = resp;
		resp = resp->queue_next;
	}
	pthread_mutex_unlock(&g_executor.resp_mu);
	return 0;
}

int kvs_command_executor_poll_response_for_owner(
	int owner_type,
	int owner_poller_id,
	kvs_response_task_t **out
) {
	return kvs_command_executor_poll_response_for(owner_type, owner_poller_id, out);
}

int kvs_command_executor_response_event_fd(void) {
	return g_executor.event_fd;
}

void kvs_command_executor_get_stats(kvs_command_executor_stats_t *out) {
	if (!out) {
		return;
	}
	out->submitted_tasks = __atomic_load_n(&g_executor.stats.submitted_tasks, __ATOMIC_RELAXED);
	out->completed_tasks = __atomic_load_n(&g_executor.stats.completed_tasks, __ATOMIC_RELAXED);
	out->submit_failed = __atomic_load_n(&g_executor.stats.submit_failed, __ATOMIC_RELAXED);
	out->command_queue_depth = __atomic_load_n(&g_executor.stats.command_queue_depth, __ATOMIC_RELAXED);
	out->command_queue_max_depth = __atomic_load_n(&g_executor.stats.command_queue_max_depth, __ATOMIC_RELAXED);
	out->response_queue_depth = __atomic_load_n(&g_executor.stats.response_queue_depth, __ATOMIC_RELAXED);
	out->response_queue_max_depth = __atomic_load_n(&g_executor.stats.response_queue_max_depth, __ATOMIC_RELAXED);
	out->pushed_responses = __atomic_load_n(&g_executor.stats.pushed_responses, __ATOMIC_RELAXED);
	out->polled_responses = __atomic_load_n(&g_executor.stats.polled_responses, __ATOMIC_RELAXED);
	out->eventfd_wakeup_count = __atomic_load_n(&g_executor.stats.eventfd_wakeup_count, __ATOMIC_RELAXED);
	out->eventfd_write_fail_count = __atomic_load_n(&g_executor.stats.eventfd_write_fail_count, __ATOMIC_RELAXED);
	out->dropped_responses = __atomic_load_n(&g_executor.stats.dropped_responses, __ATOMIC_RELAXED);
	out->session_mismatch_drop = __atomic_load_n(&g_executor.stats.session_mismatch_drop, __ATOMIC_RELAXED);
	out->session_closing_drop = __atomic_load_n(&g_executor.stats.session_closing_drop, __ATOMIC_RELAXED);
	out->response_append_failed = __atomic_load_n(&g_executor.stats.response_append_failed, __ATOMIC_RELAXED);
	out->prepared_queue_depth = __atomic_load_n(&g_executor.stats.prepared_queue_depth, __ATOMIC_RELAXED);
	out->prepared_queue_max_depth = __atomic_load_n(&g_executor.stats.prepared_queue_max_depth, __ATOMIC_RELAXED);
	out->prepared_tasks = __atomic_load_n(&g_executor.stats.prepared_tasks, __ATOMIC_RELAXED);
	out->embedding_prepare_dispatched = __atomic_load_n(&g_executor.stats.embedding_prepare_dispatched, __ATOMIC_RELAXED);
	out->embedding_prepare_inline = __atomic_load_n(&g_executor.stats.embedding_prepare_inline, __ATOMIC_RELAXED);
	out->embedding_prepare_submit_failed = __atomic_load_n(&g_executor.stats.embedding_prepare_submit_failed, __ATOMIC_RELAXED);
	out->normal_task_executed = executor_stats_load(&g_executor.stats.normal_task_executed);
	out->prepare_task_submitted = executor_stats_load(&g_executor.stats.prepare_task_submitted);
	out->prepared_task_executed = executor_stats_load(&g_executor.stats.prepared_task_executed);
	out->prepared_task_failed = executor_stats_load(&g_executor.stats.prepared_task_failed);
	out->prepared_wait_count = executor_stats_load(&g_executor.stats.prepared_wait_count);
	out->prepared_wait_total_us = executor_stats_load(&g_executor.stats.prepared_wait_total_us);
	out->prepared_wait_max_us = executor_stats_load(&g_executor.stats.prepared_wait_max_us);
	out->prepared_task_execute_count = executor_stats_load(&g_executor.stats.prepared_task_execute_count);
	out->prepared_task_execute_total_us = executor_stats_load(&g_executor.stats.prepared_task_execute_total_us);
	out->prepared_task_execute_max_us = executor_stats_load(&g_executor.stats.prepared_task_execute_max_us);
	out->execute_sync_count = executor_stats_load(&g_executor.stats.execute_sync_count);
	out->execute_sync_total_us = executor_stats_load(&g_executor.stats.execute_sync_total_us);
	out->execute_sync_max_us = executor_stats_load(&g_executor.stats.execute_sync_max_us);
	out->commit_latency_count = executor_stats_load(&g_executor.stats.commit_latency_count);
	out->commit_latency_total_us = executor_stats_load(&g_executor.stats.commit_latency_total_us);
	out->commit_latency_max_us = executor_stats_load(&g_executor.stats.commit_latency_max_us);
	out->commit_or_search_count = executor_stats_load(&g_executor.stats.commit_or_search_count);
	out->commit_or_search_total_us = executor_stats_load(&g_executor.stats.commit_or_search_total_us);
	out->commit_or_search_max_us = executor_stats_load(&g_executor.stats.commit_or_search_max_us);
	out->finalize_response_count = executor_stats_load(&g_executor.stats.finalize_response_count);
	out->finalize_response_total_us = executor_stats_load(&g_executor.stats.finalize_response_total_us);
	out->finalize_response_max_us = executor_stats_load(&g_executor.stats.finalize_response_max_us);
	out->vset_prepared_tasks = executor_stats_load(&g_executor.stats.vset_prepared_tasks);
	out->vset_prepared_wait_total_us = executor_stats_load(&g_executor.stats.vset_prepared_wait_total_us);
	out->vset_prepared_wait_max_us = executor_stats_load(&g_executor.stats.vset_prepared_wait_max_us);
	out->vset_execute_sync_count = executor_stats_load(&g_executor.stats.vset_execute_sync_count);
	out->vset_execute_sync_total_us = executor_stats_load(&g_executor.stats.vset_execute_sync_total_us);
	out->vset_execute_sync_max_us = executor_stats_load(&g_executor.stats.vset_execute_sync_max_us);
	out->vset_post_commit_count = executor_stats_load(&g_executor.stats.vset_post_commit_count);
	out->vset_post_commit_total_us = executor_stats_load(&g_executor.stats.vset_post_commit_total_us);
	out->vset_post_commit_max_us = executor_stats_load(&g_executor.stats.vset_post_commit_max_us);
	out->vset_finalize_response_count = executor_stats_load(&g_executor.stats.vset_finalize_response_count);
	out->vset_finalize_response_total_us = executor_stats_load(&g_executor.stats.vset_finalize_response_total_us);
	out->vset_finalize_response_max_us = executor_stats_load(&g_executor.stats.vset_finalize_response_max_us);
	out->vset_push_response_count = executor_stats_load(&g_executor.stats.vset_push_response_count);
	out->vset_push_response_total_us = executor_stats_load(&g_executor.stats.vset_push_response_total_us);
	out->vset_push_response_max_us = executor_stats_load(&g_executor.stats.vset_push_response_max_us);
	out->vsearch_execute_sync_count = executor_stats_load(&g_executor.stats.vsearch_execute_sync_count);
	out->vsearch_execute_sync_total_us = executor_stats_load(&g_executor.stats.vsearch_execute_sync_total_us);
	out->vsearch_execute_sync_max_us = executor_stats_load(&g_executor.stats.vsearch_execute_sync_max_us);
}

void kvs_command_executor_record_response_drop(int reason) {
	executor_stats_add(&g_executor.stats.dropped_responses, 1);
	switch (reason) {
		case KVS_COMMAND_EXECUTOR_DROP_SESSION_CLOSING:
			executor_stats_add(&g_executor.stats.session_closing_drop, 1);
			break;
		case KVS_COMMAND_EXECUTOR_DROP_APPEND_FAILED:
			executor_stats_add(&g_executor.stats.response_append_failed, 1);
			break;
		case KVS_COMMAND_EXECUTOR_DROP_SESSION_MISMATCH:
		default:
			executor_stats_add(&g_executor.stats.session_mismatch_drop, 1);
			break;
	}
}
