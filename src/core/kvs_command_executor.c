#define KVS_VECTOR_INTERNAL
#include "kvs_command_executor.h"

#include "kvs_async_net_adapter.h"
#include "distributed.h"
#include "kvs_config.h"
#include "kvs_embedding_worker_pool.h"
#include "kvs_globals.h"
#include "kvs_log.h"
#include "kvs_memory.h"
#include "kvs_owner_lock.h"
#include "kvs_aof.h"
#include "kvs_distributed.h"
#include "kvs_vector.h"
#include "kvstore.h"

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
	kvs_replicated_command_task_t *repl_head;
	kvs_replicated_command_task_t *repl_tail;
	kvs_ttl_expire_task_t *ttl_head;
	kvs_ttl_expire_task_t *ttl_tail;
	kvs_ttl_scan_task_t *ttl_scan_head;
	kvs_ttl_scan_task_t *ttl_scan_tail;
	struct kvs_full_sync_snapshot_task_s *snapshot_head;
	struct kvs_full_sync_snapshot_task_s *snapshot_tail;
	struct kvs_rdb_snapshot_task_s *rdb_snapshot_head;
	struct kvs_rdb_snapshot_task_s *rdb_snapshot_tail;
	struct kvs_aof_rewrite_snapshot_task_s *aof_snapshot_head;
	struct kvs_aof_rewrite_snapshot_task_s *aof_snapshot_tail;
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

typedef struct kvs_full_sync_snapshot_task_s {
	struct kvs_full_sync_snapshot_task_s *queue_next;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int done;
	int status;
	kvs_full_sync_snapshot_t *snapshot;
} kvs_full_sync_snapshot_task_t;

typedef struct kvs_full_sync_snapshot_builder_s {
	kvs_full_sync_snapshot_t *snapshot;
	size_t next_index;
	int failed;
} kvs_full_sync_snapshot_builder_t;

typedef struct kvs_rdb_snapshot_task_s {
	struct kvs_rdb_snapshot_task_s *queue_next;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int done;
	int status;
	kvs_rdb_snapshot_t *snapshot;
} kvs_rdb_snapshot_task_t;

typedef struct kvs_rdb_snapshot_builder_s {
	kvs_rdb_snapshot_t *snapshot;
	size_t array_next_index;
	size_t rbtree_next_index;
	size_t hash_next_index;
	size_t skiptable_next_index;
	size_t vector_next_index;
	int failed;
} kvs_rdb_snapshot_builder_t;

typedef struct kvs_aof_rewrite_snapshot_task_s {
	struct kvs_aof_rewrite_snapshot_task_s *queue_next;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int done;
	int status;
	kvs_aof_rewrite_snapshot_t *snapshot;
} kvs_aof_rewrite_snapshot_task_t;

typedef struct kvs_aof_rewrite_snapshot_builder_s {
	kvs_aof_rewrite_snapshot_t *snapshot;
	size_t array_next_index;
	size_t rbtree_next_index;
	size_t hash_next_index;
	size_t skiptable_next_index;
	size_t vector_next_index;
	int failed;
} kvs_aof_rewrite_snapshot_builder_t;

static uint64_t executor_stats_add(uint64_t *field, uint64_t value) {
	return __atomic_add_fetch(field, value, __ATOMIC_RELAXED);
}

static int kvs_command_executor_is_current_thread(void) {
	return g_executor.running && pthread_equal(g_executor.thread, pthread_self());
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

static void kvs_command_executor_destroy_ttl_task(kvs_ttl_expire_task_t *task) {
	if (!task) {
		return;
	}
	if (task->key) {
		kvs_free(task->key);
		task->key = NULL;
	}
	kvs_free(task);
}

static void kvs_command_executor_destroy_ttl_scan_task(kvs_ttl_scan_task_t *task) {
	if (!task) {
		return;
	}
	kvs_free(task);
}

static void kvs_command_executor_destroy_repl_task(kvs_replicated_command_task_t *task) {
	if (!task) {
		return;
	}
	if (task->key) {
		kvs_free(task->key);
		task->key = NULL;
	}
	if (task->value) {
		kvs_free(task->value);
		task->value = NULL;
	}
	kvs_free(task);
}

void kvs_full_sync_snapshot_destroy(kvs_full_sync_snapshot_t *snapshot) {
	size_t i;

	if (!snapshot) {
		return;
	}
	if (snapshot->entries) {
		for (i = 0; i < snapshot->entry_count; ++i) {
			if (snapshot->entries[i].key) {
				kvs_free(snapshot->entries[i].key);
				snapshot->entries[i].key = NULL;
			}
			if (snapshot->entries[i].value) {
				kvs_free(snapshot->entries[i].value);
				snapshot->entries[i].value = NULL;
			}
		}
		kvs_free(snapshot->entries);
		snapshot->entries = NULL;
	}
	snapshot->entry_count = 0;
	kvs_free(snapshot);
}

void kvs_rdb_snapshot_destroy(kvs_rdb_snapshot_t *snapshot) {
	size_t i;
	kvs_rdb_snapshot_kv_entry_t *kv_entries[] = {
		snapshot ? snapshot->array_entries : NULL,
		snapshot ? snapshot->rbtree_entries : NULL,
		snapshot ? snapshot->hash_entries : NULL,
		snapshot ? snapshot->skiptable_entries : NULL
	};
	size_t kv_counts[] = {
		snapshot ? snapshot->array_count : 0,
		snapshot ? snapshot->rbtree_count : 0,
		snapshot ? snapshot->hash_count : 0,
		snapshot ? snapshot->skiptable_count : 0
	};

	if (!snapshot) {
		return;
	}
	for (i = 0; i < sizeof(kv_entries) / sizeof(kv_entries[0]); ++i) {
		size_t j;
		for (j = 0; j < kv_counts[i]; ++j) {
			if (kv_entries[i][j].key) {
				kvs_free(kv_entries[i][j].key);
				kv_entries[i][j].key = NULL;
			}
			if (kv_entries[i][j].value) {
				kvs_free(kv_entries[i][j].value);
				kv_entries[i][j].value = NULL;
			}
		}
		if (kv_entries[i]) {
			kvs_free(kv_entries[i]);
		}
	}
	if (snapshot->vector_section.records) {
		for (i = 0; i < snapshot->vector_section.record_count; ++i) {
			kvs_rdb_snapshot_vector_record_t *record =
				&snapshot->vector_section.records[i];
			if (record->question) {
				kvs_free(record->question);
			}
			if (record->answer) {
				kvs_free(record->answer);
			}
			if (record->scope) {
				kvs_free(record->scope);
			}
			if (record->normalized_question) {
				kvs_free(record->normalized_question);
			}
			if (record->embedding) {
				kvs_free(record->embedding);
			}
		}
		kvs_free(snapshot->vector_section.records);
		snapshot->vector_section.records = NULL;
	}
	kvs_free(snapshot);
}

void kvs_aof_rewrite_snapshot_destroy(kvs_aof_rewrite_snapshot_t *snapshot) {
	size_t i;
	kvs_rdb_snapshot_kv_entry_t *kv_entries[] = {
		snapshot ? snapshot->array_entries : NULL,
		snapshot ? snapshot->rbtree_entries : NULL,
		snapshot ? snapshot->hash_entries : NULL,
		snapshot ? snapshot->skiptable_entries : NULL
	};
	size_t kv_counts[] = {
		snapshot ? snapshot->array_count : 0,
		snapshot ? snapshot->rbtree_count : 0,
		snapshot ? snapshot->hash_count : 0,
		snapshot ? snapshot->skiptable_count : 0
	};

	if (!snapshot) {
		return;
	}
	for (i = 0; i < sizeof(kv_entries) / sizeof(kv_entries[0]); ++i) {
		size_t j;
		for (j = 0; j < kv_counts[i]; ++j) {
			if (kv_entries[i][j].key) {
				kvs_free(kv_entries[i][j].key);
			}
			if (kv_entries[i][j].value) {
				kvs_free(kv_entries[i][j].value);
			}
		}
		if (kv_entries[i]) {
			kvs_free(kv_entries[i]);
		}
	}
	if (snapshot->vector_entries) {
		for (i = 0; i < snapshot->vector_count; ++i) {
			if (snapshot->vector_entries[i].question) {
				kvs_free(snapshot->vector_entries[i].question);
			}
			if (snapshot->vector_entries[i].answer) {
				kvs_free(snapshot->vector_entries[i].answer);
			}
			if (snapshot->vector_entries[i].scope) {
				kvs_free(snapshot->vector_entries[i].scope);
			}
		}
		kvs_free(snapshot->vector_entries);
	}
	kvs_free(snapshot);
}

static void kvs_command_executor_snapshot_task_complete(
	kvs_full_sync_snapshot_task_t *task,
	int status,
	kvs_full_sync_snapshot_t *snapshot
) {
	if (!task) {
		if (snapshot) {
			kvs_full_sync_snapshot_destroy(snapshot);
		}
		return;
	}

	pthread_mutex_lock(&task->mu);
	task->status = status;
	task->snapshot = snapshot;
	task->done = 1;
	pthread_cond_signal(&task->cv);
	pthread_mutex_unlock(&task->mu);
}

static void kvs_command_executor_rdb_snapshot_task_complete(
	kvs_rdb_snapshot_task_t *task,
	int status,
	kvs_rdb_snapshot_t *snapshot
) {
	if (!task) {
		if (snapshot) {
			kvs_rdb_snapshot_destroy(snapshot);
		}
		return;
	}

	pthread_mutex_lock(&task->mu);
	task->status = status;
	task->snapshot = snapshot;
	task->done = 1;
	pthread_cond_signal(&task->cv);
	pthread_mutex_unlock(&task->mu);
}

static void kvs_command_executor_aof_snapshot_task_complete(
	kvs_aof_rewrite_snapshot_task_t *task,
	int status,
	kvs_aof_rewrite_snapshot_t *snapshot
) {
	if (!task) {
		if (snapshot) {
			kvs_aof_rewrite_snapshot_destroy(snapshot);
		}
		return;
	}

	pthread_mutex_lock(&task->mu);
	task->status = status;
	task->snapshot = snapshot;
	task->done = 1;
	pthread_cond_signal(&task->cv);
	pthread_mutex_unlock(&task->mu);
}

static void kvs_command_executor_log_embedding_worker_stats(void) {
	kvs_embedding_worker_pool_stats_t stats;
	kvs_command_executor_stats_t exec_stats;
	kvs_owner_lock_stats_t owner_stats;
	kvs_vector_vset_commit_stats_t vector_stats;
	kvs_vector_info_t vector_info;
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
	uint64_t vset_backend_add_avg_us = 0;
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
	uint64_t vsearch_response_build_avg_us = 0;
	uint64_t vsearch_response_push_avg_us = 0;
	uint64_t vsearch_non_index_avg_us = 0;

	memset(&stats, 0, sizeof(stats));
	memset(&exec_stats, 0, sizeof(exec_stats));
	memset(&owner_stats, 0, sizeof(owner_stats));
	memset(&vector_stats, 0, sizeof(vector_stats));
	memset(&vector_info, 0, sizeof(vector_info));
	kvs_embedding_worker_pool_get_stats(&stats);
	kvs_command_executor_get_stats(&exec_stats);
	kvs_owner_lock_get_stats(&owner_stats);
	kvs_vector_vset_commit_stats_get(&vector_stats);
	kvs_vector_info(&vector_info);
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
	if (vector_stats.vset_backend_add_count > 0) {
		vset_backend_add_avg_us =
			vector_stats.vset_backend_add_total_us / vector_stats.vset_backend_add_count;
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
	if (vector_stats.vsearch_response_build_count > 0) {
		vsearch_response_build_avg_us =
			vector_stats.vsearch_response_build_total_us /
			vector_stats.vsearch_response_build_count;
	}
	if (vector_stats.vsearch_response_push_count > 0) {
		vsearch_response_push_avg_us =
			vector_stats.vsearch_response_push_total_us /
			vector_stats.vsearch_response_push_count;
	}
	if (vector_stats.vsearch_response_build_count > 0 &&
	    vector_stats.vsearch_search_count > 0) {
		vsearch_non_index_avg_us = vsearch_response_build_avg_us +
			vsearch_response_push_avg_us;
	}
	KVS_LOG_INFO(
		"core",
		"owner rwlock stats: weak_mode=%llu total_calls=%llu rdlock_calls=%llu wrlock_calls=%llu unlock_calls=%llu executor_thread_hits=%llu non_executor_thread_calls=%llu strict_assertion_failures=%llu weak_noop_calls=%llu weak_fallback_calls=%llu",
		(unsigned long long)kvs_owner_lock_weak_mode_enabled(),
		(unsigned long long)owner_stats.total_calls,
		(unsigned long long)owner_stats.rdlock_calls,
		(unsigned long long)owner_stats.wrlock_calls,
		(unsigned long long)owner_stats.unlock_calls,
		(unsigned long long)owner_stats.executor_thread_hits,
		(unsigned long long)owner_stats.non_executor_thread_calls,
		(unsigned long long)owner_stats.strict_assertion_failures,
		(unsigned long long)owner_stats.weak_noop_calls,
		(unsigned long long)owner_stats.weak_fallback_calls
	);
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
		"ttl executor stats: ttl_expire_task_submitted=%llu ttl_expire_task_executed=%llu ttl_expire_task_failed=%llu ttl_expire_task_dropped=%llu ttl_expire_queue_depth=%llu ttl_expire_queue_max_depth=%llu ttl_expire_deleted_keys=%llu ttl_expire_skipped_keys=%llu ttl_expire_submit_failed=%llu ttl_expire_wait_count=%llu ttl_expire_wait_total_us=%llu ttl_expire_wait_max_us=%llu",
		(unsigned long long)exec_stats.ttl_expire_task_submitted,
		(unsigned long long)exec_stats.ttl_expire_task_executed,
		(unsigned long long)exec_stats.ttl_expire_task_failed,
		(unsigned long long)exec_stats.ttl_expire_task_dropped,
		(unsigned long long)exec_stats.ttl_expire_queue_depth,
		(unsigned long long)exec_stats.ttl_expire_queue_max_depth,
		(unsigned long long)exec_stats.ttl_expire_deleted_keys,
		(unsigned long long)exec_stats.ttl_expire_skipped_keys,
		(unsigned long long)exec_stats.ttl_expire_submit_failed,
		(unsigned long long)exec_stats.ttl_expire_wait_count,
		(unsigned long long)exec_stats.ttl_expire_wait_total_us,
		(unsigned long long)exec_stats.ttl_expire_wait_max_us
	);
	KVS_LOG_INFO(
		"core",
		"ttl scanner executor stats: ttl_scan_task_submitted=%llu ttl_scan_task_executed=%llu ttl_scan_task_failed=%llu ttl_scan_task_dropped=%llu ttl_scan_submit_failed=%llu ttl_scan_queue_depth=%llu ttl_scan_queue_max_depth=%llu ttl_scan_wait_count=%llu ttl_scan_wait_total_us=%llu ttl_scan_wait_max_us=%llu",
		(unsigned long long)exec_stats.ttl_scan_task_submitted,
		(unsigned long long)exec_stats.ttl_scan_task_executed,
		(unsigned long long)exec_stats.ttl_scan_task_failed,
		(unsigned long long)exec_stats.ttl_scan_task_dropped,
		(unsigned long long)exec_stats.ttl_scan_submit_failed,
		(unsigned long long)exec_stats.ttl_scan_queue_depth,
		(unsigned long long)exec_stats.ttl_scan_queue_max_depth,
		(unsigned long long)exec_stats.ttl_scan_wait_count,
		(unsigned long long)exec_stats.ttl_scan_wait_total_us,
		(unsigned long long)exec_stats.ttl_scan_wait_max_us
	);
	KVS_LOG_INFO(
		"core",
		"replicated executor stats: repl_task_submitted=%llu repl_task_executed=%llu repl_task_failed=%llu repl_task_dropped=%llu repl_task_submit_failed=%llu repl_queue_depth=%llu repl_queue_max_depth=%llu repl_wait_count=%llu repl_wait_total_us=%llu repl_wait_max_us=%llu repl_set_count=%llu repl_del_count=%llu repl_mod_count=%llu repl_ttl_count=%llu",
		(unsigned long long)exec_stats.repl_task_submitted,
		(unsigned long long)exec_stats.repl_task_executed,
		(unsigned long long)exec_stats.repl_task_failed,
		(unsigned long long)exec_stats.repl_task_dropped,
		(unsigned long long)exec_stats.repl_task_submit_failed,
		(unsigned long long)exec_stats.repl_queue_depth,
		(unsigned long long)exec_stats.repl_queue_max_depth,
		(unsigned long long)exec_stats.repl_wait_count,
		(unsigned long long)exec_stats.repl_wait_total_us,
		(unsigned long long)exec_stats.repl_wait_max_us,
		(unsigned long long)exec_stats.repl_set_count,
		(unsigned long long)exec_stats.repl_del_count,
		(unsigned long long)exec_stats.repl_mod_count,
		(unsigned long long)exec_stats.repl_ttl_count
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
		"vset index stats: vset_flat_index_update_count=%llu vset_flat_index_update_avg_us=%llu vset_flat_index_update_total_us=%llu vset_flat_index_update_max_us=%llu vset_backend_add_count=%llu vset_backend_add_avg_us=%llu vset_backend_add_total_us=%llu vset_backend_add_max_us=%llu vset_id_map_update_count=%llu vset_id_map_update_avg_us=%llu vset_id_map_update_total_us=%llu vset_id_map_update_max_us=%llu vset_scope_answer_question_update_count=%llu vset_scope_answer_question_update_avg_us=%llu vset_scope_answer_question_update_total_us=%llu vset_scope_answer_question_update_max_us=%llu",
		(unsigned long long)vector_stats.vset_flat_index_update_count,
		(unsigned long long)vset_flat_index_update_avg_us,
		(unsigned long long)vector_stats.vset_flat_index_update_total_us,
		(unsigned long long)vector_stats.vset_flat_index_update_max_us,
		(unsigned long long)vector_stats.vset_backend_add_count,
		(unsigned long long)vset_backend_add_avg_us,
		(unsigned long long)vector_stats.vset_backend_add_total_us,
		(unsigned long long)vector_stats.vset_backend_add_max_us,
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
	KVS_LOG_INFO(
		"core",
		"vsearch breakdown stats: vsearch_search_count=%llu vsearch_search_avg_us=%llu vsearch_search_total_us=%llu vsearch_search_max_us=%llu vsearch_non_index_avg_us=%llu vsearch_response_build_count=%llu vsearch_response_build_avg_us=%llu vsearch_response_build_total_us=%llu vsearch_response_build_max_us=%llu vsearch_response_push_count=%llu vsearch_response_push_avg_us=%llu vsearch_response_push_total_us=%llu vsearch_response_push_max_us=%llu",
		(unsigned long long)vector_stats.vsearch_search_count,
		(unsigned long long)vsearch_search_avg_us,
		(unsigned long long)vector_stats.vsearch_search_total_us,
		(unsigned long long)vector_stats.vsearch_search_max_us,
		(unsigned long long)vsearch_non_index_avg_us,
		(unsigned long long)vector_stats.vsearch_response_build_count,
		(unsigned long long)vsearch_response_build_avg_us,
		(unsigned long long)vector_stats.vsearch_response_build_total_us,
		(unsigned long long)vector_stats.vsearch_response_build_max_us,
		(unsigned long long)vector_stats.vsearch_response_push_count,
		(unsigned long long)vsearch_response_push_avg_us,
		(unsigned long long)vector_stats.vsearch_response_push_total_us,
		(unsigned long long)vector_stats.vsearch_response_push_max_us
	);
	KVS_LOG_INFO(
		"core",
		"vector backend stats: backend_name=%s size=%zu capacity=%zu max_elements=%zu M=%zu efConstruction=%zu efSearch=%zu metric=%s normalize=%s faiss_ntotal=%zu hnswlib_ntotal=%zu",
		vector_info.index_type == KVS_VECTOR_INDEX_FAISS_HNSW ? "faiss_hnsw" :
			(vector_info.index_type == KVS_VECTOR_INDEX_FAISS_FLAT ? "faiss_flat" :
			 (vector_info.index_type == KVS_VECTOR_INDEX_HNSWLIB_HNSW ? "hnswlib_hnsw" :
			  (vector_info.index_type == KVS_VECTOR_INDEX_TYPE_SQ8_FLAT ? "sq8_flat" :
			   (vector_info.index_type == KVS_VECTOR_INDEX_CORE_FLAT ? "core_flat" :
			    (vector_info.index_type == KVS_VECTOR_INDEX_TYPE_HNSW ? "hnsw" : "flat"))))),
		vector_info.index_size,
		vector_info.hnswlib_capacity,
		vector_info.hnswlib_capacity,
		vector_info.hnswlib_m ? vector_info.hnswlib_m : vector_info.ann_m,
		vector_info.hnswlib_ef_construction ? vector_info.hnswlib_ef_construction : vector_info.ann_ef_construction,
		vector_info.hnswlib_ef_search ? vector_info.hnswlib_ef_search : vector_info.ann_ef_search,
		vector_info.metric_type == KVS_VECTOR_METRIC_L2 ? "l2" :
			(vector_info.metric_type == KVS_VECTOR_METRIC_COSINE ? "cosine" : "ip"),
		"hash_provider_normalized",
		vector_info.faiss_ntotal,
		vector_info.hnswlib_ntotal
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

static kvs_replicated_command_task_t *kvs_command_executor_pop_repl_locked(void) {
	kvs_replicated_command_task_t *task = g_executor.repl_head;

	if (!task) {
		return NULL;
	}
	g_executor.repl_head = task->queue_next;
	if (!g_executor.repl_head) {
		g_executor.repl_tail = NULL;
	}
	task->queue_next = NULL;
	executor_stats_sub(&g_executor.stats.repl_queue_depth, 1);
	return task;
}

static kvs_ttl_expire_task_t *kvs_command_executor_pop_ttl_locked(void) {
	kvs_ttl_expire_task_t *task = g_executor.ttl_head;

	if (!task) {
		return NULL;
	}
	g_executor.ttl_head = task->queue_next;
	if (!g_executor.ttl_head) {
		g_executor.ttl_tail = NULL;
	}
	task->queue_next = NULL;
	executor_stats_sub(&g_executor.stats.ttl_expire_queue_depth, 1);
	return task;
}

static kvs_ttl_scan_task_t *kvs_command_executor_pop_ttl_scan_locked(void) {
	kvs_ttl_scan_task_t *task = g_executor.ttl_scan_head;

	if (!task) {
		return NULL;
	}
	g_executor.ttl_scan_head = task->queue_next;
	if (!g_executor.ttl_scan_head) {
		g_executor.ttl_scan_tail = NULL;
	}
	task->queue_next = NULL;
	executor_stats_sub(&g_executor.stats.ttl_scan_queue_depth, 1);
	return task;
}

static kvs_full_sync_snapshot_task_t *kvs_command_executor_pop_snapshot_locked(void) {
	kvs_full_sync_snapshot_task_t *task = g_executor.snapshot_head;

	if (!task) {
		return NULL;
	}
	g_executor.snapshot_head = task->queue_next;
	if (!g_executor.snapshot_head) {
		g_executor.snapshot_tail = NULL;
	}
	task->queue_next = NULL;
	return task;
}

static kvs_rdb_snapshot_task_t *kvs_command_executor_pop_rdb_snapshot_locked(void) {
	kvs_rdb_snapshot_task_t *task = g_executor.rdb_snapshot_head;

	if (!task) {
		return NULL;
	}
	g_executor.rdb_snapshot_head = task->queue_next;
	if (!g_executor.rdb_snapshot_head) {
		g_executor.rdb_snapshot_tail = NULL;
	}
	task->queue_next = NULL;
	return task;
}

static kvs_aof_rewrite_snapshot_task_t *kvs_command_executor_pop_aof_snapshot_locked(void) {
	kvs_aof_rewrite_snapshot_task_t *task = g_executor.aof_snapshot_head;

	if (!task) {
		return NULL;
	}
	g_executor.aof_snapshot_head = task->queue_next;
	if (!g_executor.aof_snapshot_head) {
		g_executor.aof_snapshot_tail = NULL;
	}
	task->queue_next = NULL;
	return task;
}

static int kvs_command_executor_repl_is_set(ms_op_t op) {
	return op == MS_OP_ARRAY_SET ||
	       op == MS_OP_RBTREE_RSET ||
	       op == MS_OP_HASH_HSET ||
	       op == MS_OP_SKIPTABLE_SSET ||
	       op == MS_OP_PAPERSET;
}

static int kvs_command_executor_repl_is_del(ms_op_t op) {
	return op == MS_OP_ARRAY_DEL ||
	       op == MS_OP_RBTREE_RDEL ||
	       op == MS_OP_HASH_HDEL ||
	       op == MS_OP_SKIPTABLE_SDEL ||
	       op == MS_OP_PAPERDEL;
}

static int kvs_command_executor_repl_is_mod(ms_op_t op) {
	return op == MS_OP_ARRAY_MOD ||
	       op == MS_OP_RBTREE_RMOD ||
	       op == MS_OP_HASH_HMOD ||
	       op == MS_OP_SKIPTABLE_SMOD ||
	       op == MS_OP_PAPERMOD;
}

static int kvs_command_executor_repl_is_ttl(ms_op_t op) {
	return op == MS_OP_TTL_PEXPIREAT ||
	       op == MS_OP_TTL_PERSIST;
}

static void kvs_command_executor_repl_record_op(ms_op_t op) {
	if (kvs_command_executor_repl_is_set(op)) {
		executor_stats_add(&g_executor.stats.repl_set_count, 1);
	} else if (kvs_command_executor_repl_is_del(op)) {
		executor_stats_add(&g_executor.stats.repl_del_count, 1);
	} else if (kvs_command_executor_repl_is_mod(op)) {
		executor_stats_add(&g_executor.stats.repl_mod_count, 1);
	} else if (kvs_command_executor_repl_is_ttl(op)) {
		executor_stats_add(&g_executor.stats.repl_ttl_count, 1);
	}
}

static int kvs_full_sync_snapshot_builder_copy_bytes(
	char **out,
	const char *src,
	size_t len
) {
	char *buf;

	if (!out) {
		return -1;
	}
	*out = NULL;
	if (!src && len > 0) {
		return -1;
	}
	buf = (char *)kvs_malloc(len + 1);
	if (!buf) {
		return -1;
	}
	if (len > 0) {
		memcpy(buf, src, len);
	}
	buf[len] = '\0';
	*out = buf;
	return 0;
}

static int kvs_full_sync_snapshot_builder_append(
	kvs_full_sync_snapshot_builder_t *builder,
	kvs_full_sync_snapshot_backend_t backend,
	const char *key,
	size_t key_len,
	const char *value,
	size_t value_len,
	int64_t expire_at_ms
) {
	kvs_full_sync_snapshot_entry_t *entry;

	if (!builder || !builder->snapshot || !builder->snapshot->entries) {
		return -1;
	}
	if (builder->next_index >= builder->snapshot->entry_count) {
		return -1;
	}

	entry = &builder->snapshot->entries[builder->next_index];
	memset(entry, 0, sizeof(*entry));
	entry->backend = backend;
	entry->key_len = key_len;
	entry->value_len = value_len;
	entry->expire_at_ms = expire_at_ms;

	if (kvs_full_sync_snapshot_builder_copy_bytes(&entry->key, key, key_len) != 0) {
		return -1;
	}
	if (kvs_full_sync_snapshot_builder_copy_bytes(&entry->value, value, value_len) != 0) {
		return -1;
	}
	builder->next_index++;
	return 0;
}

typedef struct kvs_full_sync_snapshot_count_ctx_s {
	size_t count;
	int64_t now_ms;
} kvs_full_sync_snapshot_count_ctx_t;

typedef struct kvs_full_sync_snapshot_copy_ctx_s {
	kvs_full_sync_snapshot_builder_t *builder;
	int64_t now_ms;
} kvs_full_sync_snapshot_copy_ctx_t;

#if ENABLE_RBTREE
static void kvs_full_sync_snapshot_count_rbtree_node(rbtree_node *node, void *userdata) {
	kvs_full_sync_snapshot_count_ctx_t *ctx =
		(kvs_full_sync_snapshot_count_ctx_t *)userdata;

	if (!ctx || !node || !node->key || !node->value) {
		return;
	}
	if (!kv_is_expired(node->expire_at_ms, ctx->now_ms)) {
		ctx->count++;
	}
}

static void kvs_full_sync_snapshot_copy_rbtree_node(rbtree_node *node, void *userdata) {
	kvs_full_sync_snapshot_copy_ctx_t *ctx =
		(kvs_full_sync_snapshot_copy_ctx_t *)userdata;

	if (!ctx || !ctx->builder || ctx->builder->failed || !node || !node->key || !node->value) {
		return;
	}
	if (kv_is_expired(node->expire_at_ms, ctx->now_ms)) {
		return;
	}
	if (kvs_full_sync_snapshot_builder_append(
		    ctx->builder,
		    KVS_FULL_SYNC_SNAPSHOT_BACKEND_RBTREE,
		    node->key,
		    node->key_len,
		    (const char *)node->value,
		    node->value_len,
		    node->expire_at_ms) != 0) {
		ctx->builder->failed = 1;
	}
}
#endif

static size_t kvs_command_executor_count_full_sync_entries(int64_t now_ms) {
	size_t count = 0;
	int i;

#if ENABLE_ARRAY
	kvs_array_t *array = kvs_get_global_array();
	for (i = 0; i < array->total; ++i) {
		if (array->table[i].key &&
		    array->table[i].value &&
		    !kv_is_expired(array->table[i].expire_at_ms, now_ms)) {
			count++;
		}
	}
#endif

#if ENABLE_RBTREE
	{
		kvs_full_sync_snapshot_count_ctx_t ctx;

		memset(&ctx, 0, sizeof(ctx));
		ctx.now_ms = now_ms;
		rbtree_traversal_with_callback(
			(rbtree *)kvs_get_global_rbtree(),
			kvs_get_global_rbtree()->root,
			kvs_full_sync_snapshot_count_rbtree_node,
			&ctx
		);
		count += ctx.count;
	}
#endif

#if ENABLE_HASH
	kvs_hash_t *hash = kvs_get_global_hash();
	for (i = 0; i < hash->max_slots; ++i) {
		hashnode_t *slot = &hash->nodes[i];

		if (slot->used == 1 &&
		    slot->key &&
		    slot->value &&
		    !kv_is_expired(slot->expire_at_ms, now_ms)) {
			count++;
		}
	}
#endif

#if ENABLE_SKIPTABLE
	{
		kvs_skiptable_t *skiptable = kvs_get_global_skiptable();
		Node *current = skiptable->header ? skiptable->header->forward[0] : NULL;

		while (current) {
			if (current->key &&
			    current->value &&
			    !kv_is_expired(current->expire_at_ms, now_ms)) {
				count++;
			}
			current = current->forward[0];
		}
	}
#endif

	return count;
}

static int kvs_command_executor_fill_full_sync_snapshot(
	kvs_full_sync_snapshot_builder_t *builder,
	int64_t now_ms
) {
	int i;

	if (!builder || !builder->snapshot) {
		return -1;
	}

#if ENABLE_ARRAY
	kvs_array_t *array = kvs_get_global_array();
	for (i = 0; i < array->total; ++i) {
		kvs_array_item_t *item = &array->table[i];

		if (!item->key || !item->value || kv_is_expired(item->expire_at_ms, now_ms)) {
			continue;
		}
		if (kvs_full_sync_snapshot_builder_append(
		        builder,
		        KVS_FULL_SYNC_SNAPSHOT_BACKEND_ARRAY,
		        item->key,
		        item->key_len,
		        item->value,
		        item->value_len,
		        item->expire_at_ms) != 0) {
			return -1;
		}
	}
#endif

#if ENABLE_RBTREE
	{
		kvs_full_sync_snapshot_copy_ctx_t ctx;

		memset(&ctx, 0, sizeof(ctx));
		ctx.builder = builder;
		ctx.now_ms = now_ms;
		rbtree_traversal_with_callback(
			(rbtree *)kvs_get_global_rbtree(),
			kvs_get_global_rbtree()->root,
			kvs_full_sync_snapshot_copy_rbtree_node,
			&ctx
		);
		if (builder->failed) {
			return -1;
		}
	}
#endif

#if ENABLE_HASH
	kvs_hash_t *hash = kvs_get_global_hash();
	for (i = 0; i < hash->max_slots; ++i) {
		hashnode_t *slot = &hash->nodes[i];

		if (slot->used != 1 || !slot->key || !slot->value ||
		    kv_is_expired(slot->expire_at_ms, now_ms)) {
			continue;
		}
		if (kvs_full_sync_snapshot_builder_append(
		        builder,
		        KVS_FULL_SYNC_SNAPSHOT_BACKEND_HASH,
		        slot->key,
		        slot->key_len,
		        slot->value,
		        slot->value_len,
		        slot->expire_at_ms) != 0) {
			return -1;
		}
	}
#endif

#if ENABLE_SKIPTABLE
	{
		kvs_skiptable_t *skiptable = kvs_get_global_skiptable();
		Node *current = skiptable->header ? skiptable->header->forward[0] : NULL;

		while (current) {
			if (current->key &&
			    current->value &&
			    !kv_is_expired(current->expire_at_ms, now_ms)) {
				if (kvs_full_sync_snapshot_builder_append(
				        builder,
				        KVS_FULL_SYNC_SNAPSHOT_BACKEND_SKIPTABLE,
				        current->key,
				        current->key_len,
				        current->value,
				        current->value_len,
				        current->expire_at_ms) != 0) {
					return -1;
				}
			}
			current = current->forward[0];
		}
	}
#endif

	return builder->next_index == builder->snapshot->entry_count ? 0 : -1;
}

static kvs_full_sync_snapshot_t *kvs_command_executor_create_full_sync_snapshot(void) {
	kvs_full_sync_snapshot_t *snapshot = NULL;
	kvs_full_sync_snapshot_builder_t builder;
	size_t entry_count;
	int64_t now_ms;

	memset(&builder, 0, sizeof(builder));
	now_ms = kv_now_ms();
	entry_count = kvs_command_executor_count_full_sync_entries(now_ms);

	snapshot = (kvs_full_sync_snapshot_t *)kvs_malloc(sizeof(*snapshot));
	if (!snapshot) {
		return NULL;
	}
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->captured_at_ms = now_ms;
	snapshot->entry_count = entry_count;
	if (entry_count > 0) {
		snapshot->entries = (kvs_full_sync_snapshot_entry_t *)kvs_malloc(
			entry_count * sizeof(*snapshot->entries)
		);
		if (!snapshot->entries) {
			kvs_full_sync_snapshot_destroy(snapshot);
			return NULL;
		}
		memset(snapshot->entries, 0, entry_count * sizeof(*snapshot->entries));
	}

	builder.snapshot = snapshot;
	if (kvs_command_executor_fill_full_sync_snapshot(&builder, now_ms) != 0) {
		kvs_full_sync_snapshot_destroy(snapshot);
		return NULL;
	}

	return snapshot;
}

typedef struct kvs_rdb_snapshot_count_s {
	size_t array_count;
	size_t rbtree_count;
	size_t hash_count;
	size_t skiptable_count;
	size_t vector_count;
} kvs_rdb_snapshot_count_t;

typedef struct kvs_rdb_snapshot_rbtree_count_ctx_s {
	size_t count;
	int64_t now_ms;
} kvs_rdb_snapshot_rbtree_count_ctx_t;

#if ENABLE_RBTREE
static void kvs_rdb_snapshot_count_rbtree_node(rbtree_node *node, void *userdata) {
	kvs_rdb_snapshot_rbtree_count_ctx_t *ctx =
		(kvs_rdb_snapshot_rbtree_count_ctx_t *)userdata;

	if (!ctx || !node || !node->key || !node->value) {
		return;
	}
	if (!kv_is_expired(node->expire_at_ms, ctx->now_ms)) {
		ctx->count++;
	}
}
#endif

static int kvs_rdb_snapshot_builder_copy_embedding(
	float **out,
	const float *src,
	int dim
) {
	size_t bytes;
	float *buf;

	if (!out) {
		return -1;
	}
	*out = NULL;
	if (!src || dim <= 0) {
		return dim == 0 ? 0 : -1;
	}
	bytes = (size_t)dim * sizeof(float);
	buf = (float *)kvs_malloc(bytes);
	if (!buf) {
		return -1;
	}
	memcpy(buf, src, bytes);
	*out = buf;
	return 0;
}

static int kvs_rdb_snapshot_builder_append_kv(
	kvs_rdb_snapshot_kv_entry_t *entries,
	size_t capacity,
	size_t *next_index,
	const char *key,
	size_t key_len,
	const char *value,
	size_t value_len,
	int64_t expire_at_ms
) {
	kvs_rdb_snapshot_kv_entry_t *entry;

	if (!entries || !next_index || *next_index >= capacity) {
		return -1;
	}
	entry = &entries[*next_index];
	memset(entry, 0, sizeof(*entry));
	entry->key_len = key_len;
	entry->value_len = value_len;
	entry->expire_at_ms = expire_at_ms;
	if (kvs_full_sync_snapshot_builder_copy_bytes(&entry->key, key, key_len) != 0) {
		return -1;
	}
	if (kvs_full_sync_snapshot_builder_copy_bytes(&entry->value, value, value_len) != 0) {
		return -1;
	}
	(*next_index)++;
	return 0;
}

static int kvs_rdb_snapshot_builder_append_vector(
	kvs_rdb_snapshot_builder_t *builder,
	const kvs_vector_record_t *src,
	size_t record_index
) {
	kvs_rdb_snapshot_vector_record_t *dst;

	if (!builder || !builder->snapshot || !src ||
	    builder->vector_next_index >= builder->snapshot->vector_section.record_count) {
		return -1;
	}
	dst = &builder->snapshot->vector_section.records[builder->vector_next_index];
	memset(dst, 0, sizeof(*dst));
	dst->record_index = record_index;
	dst->id = src->id;
	dst->question_len = src->question_len;
	dst->answer_len = src->answer_len;
	dst->scope_len = src->scope_len;
	dst->normalized_question_len = src->normalized_question_len;
	dst->embedding_dim = src->embedding_dim;
	dst->state = src->state;
	dst->deleted = src->deleted;
	dst->created_at_ms = src->created_at_ms;
	dst->updated_at_ms = src->updated_at_ms;
	dst->expire_at_ms = src->expire_at_ms;
	dst->generation = src->generation;
	dst->embedding_model_version = src->embedding_model_version;

	if (kvs_full_sync_snapshot_builder_copy_bytes(
		    &dst->question, src->question, src->question_len) != 0 ||
	    kvs_full_sync_snapshot_builder_copy_bytes(
		    &dst->answer, src->answer, src->answer_len) != 0 ||
	    kvs_full_sync_snapshot_builder_copy_bytes(
		    &dst->scope, src->scope, src->scope_len) != 0 ||
	    kvs_full_sync_snapshot_builder_copy_bytes(
		    &dst->normalized_question,
		    src->normalized_question,
		    src->normalized_question_len) != 0 ||
	    kvs_rdb_snapshot_builder_copy_embedding(
		    &dst->embedding, src->embedding, src->embedding_dim) != 0) {
		return -1;
	}
	builder->vector_next_index++;
	return 0;
}

static void kvs_command_executor_count_rdb_snapshot(
	kvs_rdb_snapshot_count_t *counts,
	int64_t now_ms
) {
	int i;

	if (!counts) {
		return;
	}
	memset(counts, 0, sizeof(*counts));

#if ENABLE_ARRAY
	{
		kvs_array_t *array = kvs_get_global_array();
		for (i = 0; i < array->total; ++i) {
			if (array->table[i].key &&
			    array->table[i].value &&
			    !kv_is_expired(array->table[i].expire_at_ms, now_ms)) {
				counts->array_count++;
			}
		}
	}
#endif

#if ENABLE_RBTREE
	{
		kvs_rdb_snapshot_rbtree_count_ctx_t ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.now_ms = now_ms;
		rbtree_traversal_with_callback(
			(rbtree *)kvs_get_global_rbtree(),
			kvs_get_global_rbtree()->root,
			kvs_rdb_snapshot_count_rbtree_node,
			&ctx
		);
		counts->rbtree_count = ctx.count;
	}
#endif

#if ENABLE_HASH
	{
		kvs_hash_t *hash = kvs_get_global_hash();
		for (i = 0; i < hash->max_slots; ++i) {
			hashnode_t *slot = &hash->nodes[i];
			if (slot->used == 1 &&
			    slot->key &&
			    slot->value &&
			    !kv_is_expired(slot->expire_at_ms, now_ms)) {
				counts->hash_count++;
			}
		}
	}
#endif

#if ENABLE_SKIPTABLE
	{
		kvs_skiptable_t *skiptable = kvs_get_global_skiptable();
		Node *current = skiptable->header ? skiptable->header->forward[0] : NULL;
		while (current) {
			if (current->key &&
			    current->value &&
			    !kv_is_expired(current->expire_at_ms, now_ms)) {
				counts->skiptable_count++;
			}
			current = current->forward[0];
		}
	}
#endif

	{
		kvs_vector_store_t *store = &g_vector_state.store;
		for (i = 0; i < (int)store->capacity; ++i) {
			if (store->records[i].state != KVS_VECTOR_RECORD_EMPTY) {
				counts->vector_count++;
			}
		}
	}
}

#if ENABLE_RBTREE
static void kvs_rdb_snapshot_fill_rbtree_node(rbtree_node *node, void *userdata) {
	kvs_rdb_snapshot_builder_t *builder =
		(kvs_rdb_snapshot_builder_t *)userdata;

	if (!builder || builder->failed || !node || !node->key || !node->value) {
		return;
	}
	if (kv_is_expired(node->expire_at_ms, builder->snapshot->captured_at_ms)) {
		return;
	}
	if (kvs_rdb_snapshot_builder_append_kv(
		    builder->snapshot->rbtree_entries,
		    builder->snapshot->rbtree_count,
		    &builder->rbtree_next_index,
		    node->key,
		    node->key_len,
		    (const char *)node->value,
		    node->value_len,
		    node->expire_at_ms) != 0) {
		builder->failed = 1;
	}
}
#endif

static int kvs_command_executor_fill_rdb_snapshot(
	kvs_rdb_snapshot_builder_t *builder,
	int64_t now_ms
) {
	int i;

	if (!builder || !builder->snapshot) {
		return -1;
	}

#if ENABLE_ARRAY
	{
		kvs_array_t *array = kvs_get_global_array();
		for (i = 0; i < array->total; ++i) {
			kvs_array_item_t *item = &array->table[i];
			if (!item->key || !item->value || kv_is_expired(item->expire_at_ms, now_ms)) {
				continue;
			}
			if (kvs_rdb_snapshot_builder_append_kv(
			        builder->snapshot->array_entries,
			        builder->snapshot->array_count,
			        &builder->array_next_index,
			        item->key,
			        item->key_len,
			        item->value,
			        item->value_len,
			        item->expire_at_ms) != 0) {
				return -1;
			}
		}
	}
#endif

#if ENABLE_RBTREE
	rbtree_traversal_with_callback(
		(rbtree *)kvs_get_global_rbtree(),
		kvs_get_global_rbtree()->root,
		kvs_rdb_snapshot_fill_rbtree_node,
		builder
	);
	if (builder->failed) {
		return -1;
	}
#endif

#if ENABLE_HASH
	{
		kvs_hash_t *hash = kvs_get_global_hash();
		for (i = 0; i < hash->max_slots; ++i) {
			hashnode_t *slot = &hash->nodes[i];
			if (slot->used != 1 || !slot->key || !slot->value ||
			    kv_is_expired(slot->expire_at_ms, now_ms)) {
				continue;
			}
			if (kvs_rdb_snapshot_builder_append_kv(
			        builder->snapshot->hash_entries,
			        builder->snapshot->hash_count,
			        &builder->hash_next_index,
			        slot->key,
			        slot->key_len,
			        slot->value,
			        slot->value_len,
			        slot->expire_at_ms) != 0) {
				return -1;
			}
		}
	}
#endif

#if ENABLE_SKIPTABLE
	{
		kvs_skiptable_t *skiptable = kvs_get_global_skiptable();
		Node *current = skiptable->header ? skiptable->header->forward[0] : NULL;
		while (current) {
			if (current->key &&
			    current->value &&
			    !kv_is_expired(current->expire_at_ms, now_ms)) {
				if (kvs_rdb_snapshot_builder_append_kv(
				        builder->snapshot->skiptable_entries,
				        builder->snapshot->skiptable_count,
				        &builder->skiptable_next_index,
				        current->key,
				        current->key_len,
				        current->value,
				        current->value_len,
				        current->expire_at_ms) != 0) {
					return -1;
				}
			}
			current = current->forward[0];
		}
	}
#endif

	{
		kvs_vector_store_t *store = &g_vector_state.store;
		for (i = 0; i < (int)store->capacity; ++i) {
			kvs_vector_record_t *record = &store->records[i];
			if (record->state == KVS_VECTOR_RECORD_EMPTY) {
				continue;
			}
			if (kvs_rdb_snapshot_builder_append_vector(builder, record, (size_t)i) != 0) {
				return -1;
			}
		}
	}

	return builder->array_next_index == builder->snapshot->array_count &&
	       builder->rbtree_next_index == builder->snapshot->rbtree_count &&
	       builder->hash_next_index == builder->snapshot->hash_count &&
	       builder->skiptable_next_index == builder->snapshot->skiptable_count &&
	       builder->vector_next_index == builder->snapshot->vector_section.record_count ?
		       0 :
		       -1;
}

static kvs_rdb_snapshot_t *kvs_command_executor_create_rdb_snapshot(void) {
	kvs_rdb_snapshot_t *snapshot;
	kvs_rdb_snapshot_builder_t builder;
	kvs_rdb_snapshot_count_t counts;
	int64_t now_ms;

	now_ms = kv_now_ms();
	kvs_command_executor_count_rdb_snapshot(&counts, now_ms);

	snapshot = (kvs_rdb_snapshot_t *)kvs_malloc(sizeof(*snapshot));
	if (!snapshot) {
		return NULL;
	}
	memset(snapshot, 0, sizeof(*snapshot));
	memset(&builder, 0, sizeof(builder));

	snapshot->captured_at_ms = now_ms;
	snapshot->array_count = counts.array_count;
	snapshot->rbtree_count = counts.rbtree_count;
	snapshot->hash_count = counts.hash_count;
	snapshot->skiptable_count = counts.skiptable_count;
	snapshot->vector_section.initialized = g_vector_state.initialized;
	snapshot->vector_section.metric_type = g_vector_state.metric_type;
	snapshot->vector_section.index_type = g_vector_state.index_type;
	snapshot->vector_section.next_id = g_vector_state.store.next_id;
	snapshot->vector_section.record_count = counts.vector_count;
	snapshot->vector_section.active_records = g_vector_state.store.active_records;
	snapshot->vector_section.deleted_records = g_vector_state.store.deleted_records;

#define KVS_ALLOC_RDB_SNAPSHOT_ENTRIES(field, count, type)                           \
	do {                                                                         \
		if ((count) > 0) {                                                    \
			(field) = (type *)kvs_malloc((count) * sizeof(*(field)));      \
			if (!(field)) {                                                \
				kvs_rdb_snapshot_destroy(snapshot);                     \
				return NULL;                                           \
			}                                                                \
			memset((field), 0, (count) * sizeof(*(field)));                 \
		}                                                                    \
	} while (0)

	KVS_ALLOC_RDB_SNAPSHOT_ENTRIES(snapshot->array_entries, snapshot->array_count, kvs_rdb_snapshot_kv_entry_t);
	KVS_ALLOC_RDB_SNAPSHOT_ENTRIES(snapshot->rbtree_entries, snapshot->rbtree_count, kvs_rdb_snapshot_kv_entry_t);
	KVS_ALLOC_RDB_SNAPSHOT_ENTRIES(snapshot->hash_entries, snapshot->hash_count, kvs_rdb_snapshot_kv_entry_t);
	KVS_ALLOC_RDB_SNAPSHOT_ENTRIES(snapshot->skiptable_entries, snapshot->skiptable_count, kvs_rdb_snapshot_kv_entry_t);
	KVS_ALLOC_RDB_SNAPSHOT_ENTRIES(
		snapshot->vector_section.records,
		snapshot->vector_section.record_count,
		kvs_rdb_snapshot_vector_record_t
	);

#undef KVS_ALLOC_RDB_SNAPSHOT_ENTRIES

	builder.snapshot = snapshot;
	if (kvs_command_executor_fill_rdb_snapshot(&builder, now_ms) != 0) {
		kvs_rdb_snapshot_destroy(snapshot);
		return NULL;
	}

	return snapshot;
}

typedef struct kvs_aof_snapshot_count_s {
	size_t array_count;
	size_t rbtree_count;
	size_t hash_count;
	size_t skiptable_count;
	size_t vector_count;
} kvs_aof_snapshot_count_t;

typedef struct kvs_aof_snapshot_rbtree_count_ctx_s {
	size_t count;
	int64_t now_ms;
} kvs_aof_snapshot_rbtree_count_ctx_t;

#if ENABLE_RBTREE
static void kvs_aof_snapshot_count_rbtree_node(rbtree_node *node, void *userdata) {
	kvs_aof_snapshot_rbtree_count_ctx_t *ctx =
		(kvs_aof_snapshot_rbtree_count_ctx_t *)userdata;

	if (!ctx || !node || !node->key || !node->value) {
		return;
	}
	if (!kv_is_expired(node->expire_at_ms, ctx->now_ms)) {
		ctx->count++;
	}
}
#endif

static int kvs_aof_snapshot_builder_append_vector(
	kvs_aof_rewrite_snapshot_builder_t *builder,
	const kvs_vector_record_t *src
) {
	kvs_aof_rewrite_snapshot_vector_entry_t *dst;

	if (!builder || !builder->snapshot || !src ||
	    builder->vector_next_index >= builder->snapshot->vector_count) {
		return -1;
	}
	dst = &builder->snapshot->vector_entries[builder->vector_next_index];
	memset(dst, 0, sizeof(*dst));
	dst->question_len = src->question_len;
	dst->answer_len = src->answer_len;
	dst->scope_len = src->scope_len;
	if (kvs_full_sync_snapshot_builder_copy_bytes(
		    &dst->question, src->question, src->question_len) != 0 ||
	    kvs_full_sync_snapshot_builder_copy_bytes(
		    &dst->answer, src->answer, src->answer_len) != 0 ||
	    kvs_full_sync_snapshot_builder_copy_bytes(
		    &dst->scope, src->scope, src->scope_len) != 0) {
		return -1;
	}
	builder->vector_next_index++;
	return 0;
}

static void kvs_command_executor_count_aof_snapshot(
	kvs_aof_snapshot_count_t *counts,
	int64_t now_ms
) {
	int i;

	if (!counts) {
		return;
	}
	memset(counts, 0, sizeof(*counts));

#if ENABLE_ARRAY
	{
		kvs_array_t *array = kvs_get_global_array();
		for (i = 0; i < array->total; ++i) {
			if (array->table[i].key &&
			    array->table[i].value &&
			    !kv_is_expired(array->table[i].expire_at_ms, now_ms)) {
				counts->array_count++;
			}
		}
	}
#endif

#if ENABLE_RBTREE
	{
		kvs_aof_snapshot_rbtree_count_ctx_t ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.now_ms = now_ms;
		rbtree_traversal_with_callback(
			(rbtree *)kvs_get_global_rbtree(),
			kvs_get_global_rbtree()->root,
			kvs_aof_snapshot_count_rbtree_node,
			&ctx
		);
		counts->rbtree_count = ctx.count;
	}
#endif

#if ENABLE_HASH
	{
		kvs_hash_t *hash = kvs_get_global_hash();
		for (i = 0; i < hash->max_slots; ++i) {
			hashnode_t *slot = &hash->nodes[i];
			if (slot->used == 1 &&
			    slot->key &&
			    slot->value &&
			    !kv_is_expired(slot->expire_at_ms, now_ms)) {
				counts->hash_count++;
			}
		}
	}
#endif

#if ENABLE_SKIPTABLE
	{
		kvs_skiptable_t *skiptable = kvs_get_global_skiptable();
		Node *current = skiptable->header ? skiptable->header->forward[0] : NULL;
		while (current) {
			if (current->key &&
			    current->value &&
			    !kv_is_expired(current->expire_at_ms, now_ms)) {
				counts->skiptable_count++;
			}
			current = current->forward[0];
		}
	}
#endif

	{
		kvs_vector_store_t *store = &g_vector_state.store;
		for (i = 0; i < (int)store->capacity; ++i) {
			kvs_vector_record_t *record = &store->records[i];
			if (record->state == KVS_VECTOR_RECORD_ACTIVE && !record->deleted) {
				counts->vector_count++;
			}
		}
	}
}

#if ENABLE_RBTREE
static void kvs_aof_snapshot_fill_rbtree_node(rbtree_node *node, void *userdata) {
	kvs_aof_rewrite_snapshot_builder_t *builder =
		(kvs_aof_rewrite_snapshot_builder_t *)userdata;

	if (!builder || builder->failed || !node || !node->key || !node->value) {
		return;
	}
	if (kv_is_expired(node->expire_at_ms, builder->snapshot->captured_at_ms)) {
		return;
	}
	if (kvs_rdb_snapshot_builder_append_kv(
		    builder->snapshot->rbtree_entries,
		    builder->snapshot->rbtree_count,
		    &builder->rbtree_next_index,
		    node->key,
		    node->key_len,
		    (const char *)node->value,
		    node->value_len,
		    node->expire_at_ms) != 0) {
		builder->failed = 1;
	}
}
#endif

static int kvs_command_executor_fill_aof_snapshot(
	kvs_aof_rewrite_snapshot_builder_t *builder,
	int64_t now_ms
) {
	int i;

	if (!builder || !builder->snapshot) {
		return -1;
	}

#if ENABLE_ARRAY
	{
		kvs_array_t *array = kvs_get_global_array();
		for (i = 0; i < array->total; ++i) {
			kvs_array_item_t *item = &array->table[i];
			if (!item->key || !item->value || kv_is_expired(item->expire_at_ms, now_ms)) {
				continue;
			}
			if (kvs_rdb_snapshot_builder_append_kv(
			        builder->snapshot->array_entries,
			        builder->snapshot->array_count,
			        &builder->array_next_index,
			        item->key,
			        item->key_len,
			        item->value,
			        item->value_len,
			        item->expire_at_ms) != 0) {
				return -1;
			}
		}
	}
#endif

#if ENABLE_RBTREE
	rbtree_traversal_with_callback(
		(rbtree *)kvs_get_global_rbtree(),
		kvs_get_global_rbtree()->root,
		kvs_aof_snapshot_fill_rbtree_node,
		builder
	);
	if (builder->failed) {
		return -1;
	}
#endif

#if ENABLE_HASH
	{
		kvs_hash_t *hash = kvs_get_global_hash();
		for (i = 0; i < hash->max_slots; ++i) {
			hashnode_t *slot = &hash->nodes[i];
			if (slot->used != 1 || !slot->key || !slot->value ||
			    kv_is_expired(slot->expire_at_ms, now_ms)) {
				continue;
			}
			if (kvs_rdb_snapshot_builder_append_kv(
			        builder->snapshot->hash_entries,
			        builder->snapshot->hash_count,
			        &builder->hash_next_index,
			        slot->key,
			        slot->key_len,
			        slot->value,
			        slot->value_len,
			        slot->expire_at_ms) != 0) {
				return -1;
			}
		}
	}
#endif

#if ENABLE_SKIPTABLE
	{
		kvs_skiptable_t *skiptable = kvs_get_global_skiptable();
		Node *current = skiptable->header ? skiptable->header->forward[0] : NULL;
		while (current) {
			if (current->key &&
			    current->value &&
			    !kv_is_expired(current->expire_at_ms, now_ms)) {
				if (kvs_rdb_snapshot_builder_append_kv(
				        builder->snapshot->skiptable_entries,
				        builder->snapshot->skiptable_count,
				        &builder->skiptable_next_index,
				        current->key,
				        current->key_len,
				        current->value,
				        current->value_len,
				        current->expire_at_ms) != 0) {
					return -1;
				}
			}
			current = current->forward[0];
		}
	}
#endif

	{
		kvs_vector_store_t *store = &g_vector_state.store;
		for (i = 0; i < (int)store->capacity; ++i) {
			kvs_vector_record_t *record = &store->records[i];
			if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
				continue;
			}
			if (kvs_aof_snapshot_builder_append_vector(builder, record) != 0) {
				return -1;
			}
		}
	}

	return builder->array_next_index == builder->snapshot->array_count &&
	       builder->rbtree_next_index == builder->snapshot->rbtree_count &&
	       builder->hash_next_index == builder->snapshot->hash_count &&
	       builder->skiptable_next_index == builder->snapshot->skiptable_count &&
	       builder->vector_next_index == builder->snapshot->vector_count ?
		       0 :
		       -1;
}

static kvs_aof_rewrite_snapshot_t *kvs_command_executor_create_aof_snapshot(void) {
	kvs_aof_rewrite_snapshot_t *snapshot;
	kvs_aof_rewrite_snapshot_builder_t builder;
	kvs_aof_snapshot_count_t counts;
	int64_t now_ms;

	now_ms = kv_now_ms();
	kvs_command_executor_count_aof_snapshot(&counts, now_ms);

	snapshot = (kvs_aof_rewrite_snapshot_t *)kvs_malloc(sizeof(*snapshot));
	if (!snapshot) {
		return NULL;
	}
	memset(snapshot, 0, sizeof(*snapshot));
	memset(&builder, 0, sizeof(builder));

	snapshot->captured_at_ms = now_ms;
	snapshot->array_count = counts.array_count;
	snapshot->rbtree_count = counts.rbtree_count;
	snapshot->hash_count = counts.hash_count;
	snapshot->skiptable_count = counts.skiptable_count;
	snapshot->vector_count = counts.vector_count;

#define KVS_ALLOC_AOF_SNAPSHOT_ENTRIES(field, count, type)                           \
	do {                                                                         \
		if ((count) > 0) {                                                    \
			(field) = (type *)kvs_malloc((count) * sizeof(*(field)));      \
			if (!(field)) {                                                \
				kvs_aof_rewrite_snapshot_destroy(snapshot);            \
				return NULL;                                           \
			}                                                                \
			memset((field), 0, (count) * sizeof(*(field)));                 \
		}                                                                    \
	} while (0)

	KVS_ALLOC_AOF_SNAPSHOT_ENTRIES(snapshot->array_entries, snapshot->array_count, kvs_rdb_snapshot_kv_entry_t);
	KVS_ALLOC_AOF_SNAPSHOT_ENTRIES(snapshot->rbtree_entries, snapshot->rbtree_count, kvs_rdb_snapshot_kv_entry_t);
	KVS_ALLOC_AOF_SNAPSHOT_ENTRIES(snapshot->hash_entries, snapshot->hash_count, kvs_rdb_snapshot_kv_entry_t);
	KVS_ALLOC_AOF_SNAPSHOT_ENTRIES(snapshot->skiptable_entries, snapshot->skiptable_count, kvs_rdb_snapshot_kv_entry_t);
	KVS_ALLOC_AOF_SNAPSHOT_ENTRIES(snapshot->vector_entries, snapshot->vector_count, kvs_aof_rewrite_snapshot_vector_entry_t);

#undef KVS_ALLOC_AOF_SNAPSHOT_ENTRIES

	builder.snapshot = snapshot;
	if (kvs_command_executor_fill_aof_snapshot(&builder, now_ms) != 0) {
		kvs_aof_rewrite_snapshot_destroy(snapshot);
		return NULL;
	}

	return snapshot;
}

static int kvs_command_executor_apply_repl_store(kvs_replicated_command_task_t *task) {
	const char *value;
	size_t value_len;

	if (!task || !task->key || task->key_len == 0) {
		return -1;
	}
	value = task->value ? task->value : "";
	value_len = task->value ? task->value_len : 0;

	switch (task->op) {
		case MS_OP_ARRAY_SET:
#if ENABLE_ARRAY
			return kvs_array_set_len(kvs_get_global_array(), task->key, task->key_len, value, value_len);
#else
			return -1;
#endif
		case MS_OP_RBTREE_RSET:
#if ENABLE_RBTREE
			return kvs_rbtree_set_len(kvs_get_global_rbtree(), task->key, task->key_len, value, value_len);
#else
			return -1;
#endif
		case MS_OP_HASH_HSET:
#if ENABLE_HASH
			return kvs_hash_set_len(kvs_get_global_hash(), task->key, task->key_len, value, value_len);
#else
			return -1;
#endif
		case MS_OP_SKIPTABLE_SSET:
#if ENABLE_SKIPTABLE
			return kvs_skiptable_set_len(kvs_get_global_skiptable(), task->key, task->key_len, value, value_len);
#else
			return -1;
#endif
		case MS_OP_ARRAY_DEL:
#if ENABLE_ARRAY
			return kvs_array_del_len(kvs_get_global_array(), task->key, task->key_len);
#else
			return -1;
#endif
		case MS_OP_RBTREE_RDEL:
#if ENABLE_RBTREE
			return kvs_rbtree_del_len(kvs_get_global_rbtree(), task->key, task->key_len);
#else
			return -1;
#endif
		case MS_OP_HASH_HDEL:
#if ENABLE_HASH
			return kvs_hash_del_len(kvs_get_global_hash(), task->key, task->key_len);
#else
			return -1;
#endif
		case MS_OP_SKIPTABLE_SDEL:
#if ENABLE_SKIPTABLE
			return kvs_skiptable_del_len(kvs_get_global_skiptable(), task->key, task->key_len);
#else
			return -1;
#endif
		case MS_OP_ARRAY_MOD:
#if ENABLE_ARRAY
			return kvs_array_mod_len(kvs_get_global_array(), task->key, task->key_len, value, value_len);
#else
			return -1;
#endif
		case MS_OP_RBTREE_RMOD:
#if ENABLE_RBTREE
			return kvs_rbtree_mod_len(kvs_get_global_rbtree(), task->key, task->key_len, value, value_len);
#else
			return -1;
#endif
		case MS_OP_HASH_HMOD:
#if ENABLE_HASH
			return kvs_hash_mod_len(kvs_get_global_hash(), task->key, task->key_len, value, value_len);
#else
			return -1;
#endif
		case MS_OP_SKIPTABLE_SMOD:
#if ENABLE_SKIPTABLE
			return kvs_skiptable_mod_len(kvs_get_global_skiptable(), task->key, task->key_len, value, value_len);
#else
			return -1;
#endif
		case MS_OP_PAPERSET:
#if ENABLE_RBTREE
			return kvs_rbtree_set_len(kvs_get_global_rbtree(), task->key, task->key_len, value, value_len);
#else
			return -1;
#endif
		case MS_OP_PAPERDEL:
#if ENABLE_RBTREE
			return kvs_rbtree_del_len(kvs_get_global_rbtree(), task->key, task->key_len);
#else
			return -1;
#endif
		case MS_OP_PAPERMOD:
#if ENABLE_RBTREE
			return kvs_rbtree_mod_len(kvs_get_global_rbtree(), task->key, task->key_len, value, value_len);
#else
			return -1;
#endif
		case MS_OP_TTL_PEXPIREAT:
			return ttl_set_any_key_expire_at_ms(task->key, task->key_len, task->expire_at_ms) < 0 ? -1 : 0;
		case MS_OP_TTL_PERSIST:
			return ttl_set_any_key_expire_at_ms(task->key, task->key_len, KV_TTL_NO_EXPIRE) < 0 ? -1 : 0;
		default:
			return -1;
	}
}

#if ENABLE_AOF
static void kvs_command_executor_repl_append_aof(kvs_replicated_command_task_t *task) {
	const char *cmd = NULL;
	char expire_buf[32];
	char *argv[3];
	size_t arg_lens[3];
	int argc;

	if (!task || !task->key) {
		return;
	}

	switch (task->op) {
		case MS_OP_ARRAY_SET: cmd = "SET"; break;
		case MS_OP_RBTREE_RSET: cmd = "RSET"; break;
		case MS_OP_HASH_HSET: cmd = "HSET"; break;
		case MS_OP_SKIPTABLE_SSET: cmd = "SSET"; break;
		case MS_OP_ARRAY_DEL: cmd = "DEL"; break;
		case MS_OP_RBTREE_RDEL: cmd = "RDEL"; break;
		case MS_OP_HASH_HDEL: cmd = "HDEL"; break;
		case MS_OP_SKIPTABLE_SDEL: cmd = "SDEL"; break;
		case MS_OP_ARRAY_MOD: cmd = "MOD"; break;
		case MS_OP_RBTREE_RMOD: cmd = "RMOD"; break;
		case MS_OP_HASH_HMOD: cmd = "HMOD"; break;
		case MS_OP_SKIPTABLE_SMOD: cmd = "SMOD"; break;
		case MS_OP_PAPERSET: cmd = "PAPERSET"; break;
		case MS_OP_PAPERDEL: cmd = "PAPERDEL"; break;
		case MS_OP_TTL_PEXPIREAT: cmd = "PEXPIREAT"; break;
		case MS_OP_TTL_PERSIST: cmd = "PERSIST"; break;
		default: return;
	}

	argv[0] = (char *)cmd;
	arg_lens[0] = strlen(cmd);
	argv[1] = task->key;
	arg_lens[1] = task->key_len;
	if (task->op == MS_OP_ARRAY_DEL ||
	    task->op == MS_OP_RBTREE_RDEL ||
	    task->op == MS_OP_HASH_HDEL ||
	    task->op == MS_OP_SKIPTABLE_SDEL ||
	    task->op == MS_OP_TTL_PERSIST) {
		argc = 2;
	} else {
		argc = 3;
		if (task->op == MS_OP_TTL_PEXPIREAT) {
			int n = snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)task->expire_at_ms);
			if (n < 0 || n >= (int)sizeof(expire_buf)) {
				return;
			}
			argv[2] = expire_buf;
			arg_lens[2] = (size_t)n;
		} else {
			argv[2] = task->value ? task->value : "";
			arg_lens[2] = task->value ? task->value_len : 0;
		}
	}
	aof_append(argc, argv, arg_lens);
}
#endif

static void kvs_command_executor_execute_repl(kvs_replicated_command_task_t *task) {
	uint64_t now_us;
	uint64_t wait_us;
	int old_sync_context;
	int ret;

	if (!task) {
		return;
	}
	if (task->enqueue_us > 0) {
		now_us = executor_now_us();
		wait_us = now_us >= task->enqueue_us ? now_us - task->enqueue_us : 0;
		executor_record_timing(
			&g_executor.stats.repl_wait_count,
			&g_executor.stats.repl_wait_total_us,
			&g_executor.stats.repl_wait_max_us,
			wait_us
		);
	}

	if (kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "executor.replicated.write") != 0) {
		executor_stats_add(&g_executor.stats.repl_task_failed, 1);
		kvs_command_executor_destroy_repl_task(task);
		return;
	}
	old_sync_context = get_sync_context();
	set_sync_context(1);
	ret = kvs_command_executor_apply_repl_store(task);
	kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.replicated.write");

	if (ret == 0) {
#if ENABLE_AOF
		if (!aof_is_loading()) {
			kvs_command_executor_repl_append_aof(task);
		}
#endif
		executor_stats_add(&g_executor.stats.repl_task_executed, 1);
		kvs_command_executor_repl_record_op(task->op);
	} else {
		executor_stats_add(&g_executor.stats.repl_task_failed, 1);
	}
	set_sync_context(old_sync_context);
	kvs_command_executor_destroy_repl_task(task);
}

static void kvs_command_executor_execute_ttl(kvs_ttl_expire_task_t *task) {
	uint64_t now_us;
	uint64_t wait_us;
	int ret;

	if (!task) {
		return;
	}
	if (task->enqueue_us > 0) {
		now_us = executor_now_us();
		wait_us = now_us >= task->enqueue_us ? now_us - task->enqueue_us : 0;
		executor_record_timing(
			&g_executor.stats.ttl_expire_wait_count,
			&g_executor.stats.ttl_expire_wait_total_us,
			&g_executor.stats.ttl_expire_wait_max_us,
			wait_us
		);
	}

	if (kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "executor.ttl_expire.write") != 0) {
		executor_stats_add(&g_executor.stats.ttl_expire_task_failed, 1);
		kvs_command_executor_destroy_ttl_task(task);
		return;
	}
	ret = ttl_expire_observed_key(
		task->key,
		task->key_len,
		task->observed_expire_at_ms
	);
	kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.ttl_expire.write");

	if (ret > 0) {
		executor_stats_add(&g_executor.stats.ttl_expire_deleted_keys, 1);
		executor_stats_add(&g_executor.stats.ttl_expire_task_executed, 1);
	} else if (ret == 0) {
		executor_stats_add(&g_executor.stats.ttl_expire_skipped_keys, 1);
		executor_stats_add(&g_executor.stats.ttl_expire_task_executed, 1);
	} else {
		executor_stats_add(&g_executor.stats.ttl_expire_task_failed, 1);
	}

	kvs_command_executor_destroy_ttl_task(task);
}

#define KVS_TTL_SCAN_MAX_CANDIDATES 32

typedef struct kvs_ttl_scan_cursor_s {
	int64_t now_ms;
	int budget;
	char *keys[KVS_TTL_SCAN_MAX_CANDIDATES];
	size_t key_lens[KVS_TTL_SCAN_MAX_CANDIDATES];
	int64_t expire_at_ms[KVS_TTL_SCAN_MAX_CANDIDATES];
	int count;
} kvs_ttl_scan_cursor_t;

static void kvs_command_executor_ttl_scan_cursor_reset(kvs_ttl_scan_cursor_t *cursor) {
	if (!cursor) {
		return;
	}
	memset(cursor, 0, sizeof(*cursor));
	cursor->budget = 32;
}

static void kvs_command_executor_ttl_scan_cursor_free(kvs_ttl_scan_cursor_t *cursor) {
	int i;

	if (!cursor) {
		return;
	}
	for (i = 0; i < cursor->count; ++i) {
		if (cursor->keys[i]) {
			kvs_free(cursor->keys[i]);
			cursor->keys[i] = NULL;
		}
	}
	cursor->count = 0;
}

static int kvs_command_executor_ttl_scan_record(
	kvs_ttl_scan_cursor_t *cursor,
	const char *key,
	size_t key_len,
	int64_t expire_at_ms
) {
	char *copy;

	if (!cursor || !key || key_len == 0 ||
	    cursor->count >= KVS_TTL_SCAN_MAX_CANDIDATES) {
		return 0;
	}
	copy = (char *)kvs_malloc(key_len + 1);
	if (!copy) {
		executor_stats_add(&g_executor.stats.ttl_scan_task_failed, 1);
		return 0;
	}
	memcpy(copy, key, key_len);
	copy[key_len] = '\0';
	cursor->keys[cursor->count] = copy;
	cursor->key_lens[cursor->count] = key_len;
	cursor->expire_at_ms[cursor->count] = expire_at_ms;
	cursor->count++;
	return 1;
}

static int kvs_command_executor_ttl_expire_candidate(
	const char *key,
	size_t key_len,
	int64_t observed_expire_at_ms
) {
	int ret;

	if (!key || key_len == 0 || observed_expire_at_ms < 0) {
		executor_stats_add(&g_executor.stats.ttl_expire_task_failed, 1);
		return 0;
	}

	executor_stats_add(&g_executor.stats.ttl_expire_task_submitted, 1);
	ret = ttl_expire_observed_key(key, key_len, observed_expire_at_ms);
	if (ret > 0) {
		executor_stats_add(&g_executor.stats.ttl_expire_deleted_keys, 1);
		executor_stats_add(&g_executor.stats.ttl_expire_task_executed, 1);
		return 1;
	}
	if (ret == 0) {
		executor_stats_add(&g_executor.stats.ttl_expire_skipped_keys, 1);
		executor_stats_add(&g_executor.stats.ttl_expire_task_executed, 1);
		return 0;
	}

	executor_stats_add(&g_executor.stats.ttl_expire_task_failed, 1);
	return 0;
}

static int kvs_command_executor_ttl_scan_flush(kvs_ttl_scan_cursor_t *cursor) {
	int deleted = 0;
	int i;

	if (!cursor) {
		return 0;
	}
	for (i = 0; i < cursor->count; ++i) {
		deleted += kvs_command_executor_ttl_expire_candidate(
			cursor->keys[i],
			cursor->key_lens[i],
			cursor->expire_at_ms[i]
		);
	}
	kvs_command_executor_ttl_scan_cursor_free(cursor);
	return deleted;
}

#if ENABLE_RBTREE
typedef struct kvs_ttl_scan_rbtree_ctx_s {
	kvs_ttl_scan_cursor_t *cursor;
	int *budget;
} kvs_ttl_scan_rbtree_ctx_t;

static void kvs_command_executor_ttl_scan_rbtree_node(rbtree_node *node, void *userdata) {
	kvs_ttl_scan_rbtree_ctx_t *ctx = (kvs_ttl_scan_rbtree_ctx_t *)userdata;

	if (!ctx || !ctx->cursor || !node || !node->key) {
		return;
	}
	if (ctx->cursor->count >= KVS_TTL_SCAN_MAX_CANDIDATES) {
		if (ctx->budget) {
			*ctx->budget = 0;
		}
		return;
	}
	if (ctx->budget && *ctx->budget > 0) {
		(*ctx->budget)--;
	}
	if (!kv_is_expired(node->expire_at_ms, ctx->cursor->now_ms)) {
		return;
	}
	(void)kvs_command_executor_ttl_scan_record(
		ctx->cursor,
		node->key,
		node->key_len,
		node->expire_at_ms
	);
}
#endif

static int kvs_command_executor_run_ttl_scan(int budget) {
	kvs_ttl_scan_cursor_t cursor;
	int remaining;
	int i;

	kvs_command_executor_ttl_scan_cursor_reset(&cursor);
	if (budget <= 0) {
		budget = 32;
	}
	remaining = budget;
	cursor.budget = budget;
	cursor.now_ms = kv_now_ms();

#if ENABLE_ARRAY
	{
		kvs_array_t *array = kvs_get_global_array();
		if (array && array->table && cursor.count < KVS_TTL_SCAN_MAX_CANDIDATES) {
			for (i = 0; i < array->len && remaining > 0 &&
			           cursor.count < KVS_TTL_SCAN_MAX_CANDIDATES; ++i) {
				kvs_array_item_t *item = &array->table[i];
				if (!item->key) {
					continue;
				}
				remaining--;
				if (!kv_is_expired(item->expire_at_ms, cursor.now_ms)) {
					continue;
				}
				(void)kvs_command_executor_ttl_scan_record(
					&cursor,
					item->key,
					item->key_len,
					item->expire_at_ms
				);
			}
		}
	}
#endif

#if ENABLE_HASH
	{
		kvs_hash_t *hash = kvs_get_global_hash();
		if (hash && hash->nodes && cursor.count < KVS_TTL_SCAN_MAX_CANDIDATES) {
			for (i = 0; i < hash->max_slots && remaining > 0 &&
			           cursor.count < KVS_TTL_SCAN_MAX_CANDIDATES; ++i) {
				hashnode_t *slot = &hash->nodes[i];
				if (slot->used != 1 || !slot->key) {
					continue;
				}
				remaining--;
				if (!kv_is_expired(slot->expire_at_ms, cursor.now_ms)) {
					continue;
				}
				(void)kvs_command_executor_ttl_scan_record(
					&cursor,
					slot->key,
					slot->key_len,
					slot->expire_at_ms
				);
			}
		}
	}
#endif

#if ENABLE_RBTREE
	if (remaining > 0 && cursor.count < KVS_TTL_SCAN_MAX_CANDIDATES) {
		kvs_rbtree_t *tree = kvs_get_global_rbtree();
		if (tree && tree->root && tree->nil) {
			kvs_ttl_scan_rbtree_ctx_t ctx;
			memset(&ctx, 0, sizeof(ctx));
			ctx.cursor = &cursor;
			ctx.budget = &remaining;
			rbtree_traversal_with_callback_limit(
				(rbtree *)tree,
				tree->root,
				kvs_command_executor_ttl_scan_rbtree_node,
				&ctx,
				&remaining
			);
		}
	}
#endif

#if ENABLE_SKIPTABLE
	if (remaining > 0 && cursor.count < KVS_TTL_SCAN_MAX_CANDIDATES) {
		kvs_skiptable_t *skiptable = kvs_get_global_skiptable();
		Node *current = skiptable && skiptable->header ?
			skiptable->header->forward[0] :
			NULL;
		while (current && remaining > 0 &&
		       cursor.count < KVS_TTL_SCAN_MAX_CANDIDATES) {
			Node *next = current->forward[0];
			remaining--;
			if (current->key && kv_is_expired(current->expire_at_ms, cursor.now_ms)) {
				(void)kvs_command_executor_ttl_scan_record(
					&cursor,
					current->key,
					current->key_len,
					current->expire_at_ms
				);
			}
			current = next;
		}
	}
#endif

	return kvs_command_executor_ttl_scan_flush(&cursor);
}

static void kvs_command_executor_execute_ttl_scan(kvs_ttl_scan_task_t *task) {
	uint64_t now_us;
	uint64_t wait_us;

	if (!task) {
		return;
	}
	if (task->enqueue_us > 0) {
		now_us = executor_now_us();
		wait_us = now_us >= task->enqueue_us ? now_us - task->enqueue_us : 0;
		executor_record_timing(
			&g_executor.stats.ttl_scan_wait_count,
			&g_executor.stats.ttl_scan_wait_total_us,
			&g_executor.stats.ttl_scan_wait_max_us,
			wait_us
		);
	}

	if (kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "executor.ttl_scan.write") != 0) {
		executor_stats_add(&g_executor.stats.ttl_scan_task_failed, 1);
		kvs_command_executor_destroy_ttl_scan_task(task);
		return;
	}
	(void)kvs_command_executor_run_ttl_scan(task->budget);
	kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.ttl_scan.write");

	executor_stats_add(&g_executor.stats.ttl_scan_task_executed, 1);
	kvs_command_executor_destroy_ttl_scan_task(task);
}

static void kvs_command_executor_execute_full_sync_snapshot(
	kvs_full_sync_snapshot_task_t *task
) {
	kvs_full_sync_snapshot_t *snapshot = NULL;

	if (!task) {
		return;
	}

	if (kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "executor.full_sync_snapshot.read") != 0) {
		kvs_command_executor_snapshot_task_complete(task, -1, NULL);
		return;
	}
	snapshot = kvs_command_executor_create_full_sync_snapshot();
	kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.full_sync_snapshot.read");

	if (!snapshot) {
		kvs_command_executor_snapshot_task_complete(task, -1, NULL);
		return;
	}

	kvs_command_executor_snapshot_task_complete(task, 0, snapshot);
}

static void kvs_command_executor_execute_rdb_snapshot(
	kvs_rdb_snapshot_task_t *task
) {
	kvs_rdb_snapshot_t *snapshot = NULL;

	if (!task) {
		return;
	}

	if (kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "executor.rdb_snapshot.read") != 0) {
		kvs_command_executor_rdb_snapshot_task_complete(task, -1, NULL);
		return;
	}
	snapshot = kvs_command_executor_create_rdb_snapshot();
	kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.rdb_snapshot.read");

	if (!snapshot) {
		kvs_command_executor_rdb_snapshot_task_complete(task, -1, NULL);
		return;
	}

	kvs_command_executor_rdb_snapshot_task_complete(task, 0, snapshot);
}

static void kvs_command_executor_execute_aof_snapshot(
	kvs_aof_rewrite_snapshot_task_t *task
) {
	kvs_aof_rewrite_snapshot_t *snapshot = NULL;

	if (!task) {
		return;
	}

	if (kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "executor.aof_rewrite_snapshot.read") != 0) {
		kvs_command_executor_aof_snapshot_task_complete(task, -1, NULL);
		return;
	}
	snapshot = kvs_command_executor_create_aof_snapshot();
	kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.aof_rewrite_snapshot.read");

	if (!snapshot) {
		kvs_command_executor_aof_snapshot_task_complete(task, -1, NULL);
		return;
	}

	kvs_command_executor_aof_snapshot_task_complete(task, 0, snapshot);
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
	kvs_owner_lock_set_executor_thread();

	for (;;) {
		kvs_command_task_t *task;
		kvs_replicated_command_task_t *repl_task;
		kvs_ttl_expire_task_t *ttl_task;
		kvs_ttl_scan_task_t *ttl_scan_task;
		kvs_full_sync_snapshot_task_t *snapshot_task;
		kvs_rdb_snapshot_task_t *rdb_snapshot_task;
		kvs_aof_rewrite_snapshot_task_t *aof_snapshot_task;

		pthread_mutex_lock(&g_executor.cmd_mu);
		while (!g_executor.prepared_head &&
		       !g_executor.repl_head &&
		       !g_executor.ttl_head &&
		       !g_executor.ttl_scan_head &&
		       !g_executor.cmd_head &&
		       !g_executor.snapshot_head &&
		       !g_executor.rdb_snapshot_head &&
		       !g_executor.aof_snapshot_head &&
		       !g_executor.stopping) {
			pthread_cond_wait(&g_executor.cmd_cv, &g_executor.cmd_mu);
		}
		if (!g_executor.prepared_head &&
		    !g_executor.repl_head &&
		    !g_executor.ttl_head &&
		    !g_executor.ttl_scan_head &&
		    !g_executor.cmd_head &&
		    !g_executor.snapshot_head &&
		    !g_executor.rdb_snapshot_head &&
		    !g_executor.aof_snapshot_head &&
		    g_executor.stopping) {
			pthread_mutex_unlock(&g_executor.cmd_mu);
			break;
		}
		repl_task = NULL;
		ttl_task = NULL;
		ttl_scan_task = NULL;
		snapshot_task = NULL;
		rdb_snapshot_task = NULL;
		aof_snapshot_task = NULL;
		task = kvs_command_executor_pop_prepared_locked();
		if (!task) {
			repl_task = kvs_command_executor_pop_repl_locked();
			if (!repl_task) {
				ttl_task = kvs_command_executor_pop_ttl_locked();
				if (!ttl_task) {
					ttl_scan_task = kvs_command_executor_pop_ttl_scan_locked();
					if (!ttl_scan_task) {
						task = kvs_command_executor_pop_command_locked();
						if (!task) {
							snapshot_task = kvs_command_executor_pop_snapshot_locked();
							if (!snapshot_task) {
								rdb_snapshot_task = kvs_command_executor_pop_rdb_snapshot_locked();
								if (!rdb_snapshot_task) {
									aof_snapshot_task = kvs_command_executor_pop_aof_snapshot_locked();
								}
							}
						}
					}
				}
			}
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

		if (repl_task) {
			kvs_command_executor_execute_repl(repl_task);
		} else if (ttl_task) {
			kvs_command_executor_execute_ttl(ttl_task);
		} else if (ttl_scan_task) {
			kvs_command_executor_execute_ttl_scan(ttl_scan_task);
		} else if (snapshot_task) {
			kvs_command_executor_execute_full_sync_snapshot(snapshot_task);
		} else if (rdb_snapshot_task) {
			kvs_command_executor_execute_rdb_snapshot(rdb_snapshot_task);
		} else if (aof_snapshot_task) {
			kvs_command_executor_execute_aof_snapshot(aof_snapshot_task);
		} else {
			kvs_command_executor_dispatch_or_execute(task);
		}
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
	kvs_replicated_command_task_t *repl_task;
	kvs_ttl_expire_task_t *ttl_task;
	kvs_ttl_scan_task_t *ttl_scan_task;
	kvs_full_sync_snapshot_task_t *snapshot_task;
	kvs_rdb_snapshot_task_t *rdb_snapshot_task;
	kvs_aof_rewrite_snapshot_task_t *aof_snapshot_task;
	kvs_response_task_t *resp;
	int should_join;

	kvs_embedding_worker_pool_stop();
	kvs_command_executor_log_embedding_worker_stats();

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
	repl_task = g_executor.repl_head;
	g_executor.repl_head = NULL;
	g_executor.repl_tail = NULL;
	ttl_task = g_executor.ttl_head;
	g_executor.ttl_head = NULL;
	g_executor.ttl_tail = NULL;
	ttl_scan_task = g_executor.ttl_scan_head;
	g_executor.ttl_scan_head = NULL;
	g_executor.ttl_scan_tail = NULL;
	snapshot_task = g_executor.snapshot_head;
	g_executor.snapshot_head = NULL;
	g_executor.snapshot_tail = NULL;
	rdb_snapshot_task = g_executor.rdb_snapshot_head;
	g_executor.rdb_snapshot_head = NULL;
	g_executor.rdb_snapshot_tail = NULL;
	aof_snapshot_task = g_executor.aof_snapshot_head;
	g_executor.aof_snapshot_head = NULL;
	g_executor.aof_snapshot_tail = NULL;
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
	while (repl_task) {
		kvs_replicated_command_task_t *next = repl_task->queue_next;
		repl_task->queue_next = NULL;
		executor_stats_sub(&g_executor.stats.repl_queue_depth, 1);
		executor_stats_add(&g_executor.stats.repl_task_dropped, 1);
		kvs_command_executor_destroy_repl_task(repl_task);
		repl_task = next;
	}
	while (ttl_task) {
		kvs_ttl_expire_task_t *next = ttl_task->queue_next;
		ttl_task->queue_next = NULL;
		executor_stats_sub(&g_executor.stats.ttl_expire_queue_depth, 1);
		executor_stats_add(&g_executor.stats.ttl_expire_task_dropped, 1);
		kvs_command_executor_destroy_ttl_task(ttl_task);
		ttl_task = next;
	}
	while (ttl_scan_task) {
		kvs_ttl_scan_task_t *next = ttl_scan_task->queue_next;
		ttl_scan_task->queue_next = NULL;
		executor_stats_sub(&g_executor.stats.ttl_scan_queue_depth, 1);
		executor_stats_add(&g_executor.stats.ttl_scan_task_dropped, 1);
		kvs_command_executor_destroy_ttl_scan_task(ttl_scan_task);
		ttl_scan_task = next;
	}
	while (snapshot_task) {
		kvs_full_sync_snapshot_task_t *next = snapshot_task->queue_next;
		snapshot_task->queue_next = NULL;
		kvs_command_executor_snapshot_task_complete(snapshot_task, -1, NULL);
		snapshot_task = next;
	}
	while (rdb_snapshot_task) {
		kvs_rdb_snapshot_task_t *next = rdb_snapshot_task->queue_next;
		rdb_snapshot_task->queue_next = NULL;
		kvs_command_executor_rdb_snapshot_task_complete(rdb_snapshot_task, -1, NULL);
		rdb_snapshot_task = next;
	}
	while (aof_snapshot_task) {
		kvs_aof_rewrite_snapshot_task_t *next = aof_snapshot_task->queue_next;
		aof_snapshot_task->queue_next = NULL;
		kvs_command_executor_aof_snapshot_task_complete(aof_snapshot_task, -1, NULL);
		aof_snapshot_task = next;
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

int kvs_command_executor_submit_ttl_expire(
	const char *key,
	size_t key_len,
	int64_t observed_expire_at_ms
) {
	kvs_ttl_expire_task_t *task;
	uint64_t depth;

	if (!key || key_len == 0 || observed_expire_at_ms < 0) {
		executor_stats_add(&g_executor.stats.ttl_expire_submit_failed, 1);
		return -1;
	}

	task = (kvs_ttl_expire_task_t *)kvs_malloc(sizeof(*task));
	if (!task) {
		executor_stats_add(&g_executor.stats.ttl_expire_submit_failed, 1);
		return -1;
	}
	memset(task, 0, sizeof(*task));
	task->key = (char *)kvs_malloc(key_len + 1);
	if (!task->key) {
		kvs_free(task);
		executor_stats_add(&g_executor.stats.ttl_expire_submit_failed, 1);
		return -1;
	}
	memcpy(task->key, key, key_len);
	task->key[key_len] = '\0';
	task->key_len = key_len;
	task->observed_expire_at_ms = observed_expire_at_ms;
	task->enqueue_us = executor_now_us();

	pthread_mutex_lock(&g_executor.cmd_mu);
	if (!g_executor.running || g_executor.stopping) {
		pthread_mutex_unlock(&g_executor.cmd_mu);
		kvs_command_executor_destroy_ttl_task(task);
		executor_stats_add(&g_executor.stats.ttl_expire_submit_failed, 1);
		return -1;
	}
	if (g_executor.ttl_tail) {
		g_executor.ttl_tail->queue_next = task;
	} else {
		g_executor.ttl_head = task;
	}
	g_executor.ttl_tail = task;
	depth = executor_stats_add(&g_executor.stats.ttl_expire_queue_depth, 1);
	executor_stats_update_max(&g_executor.stats.ttl_expire_queue_max_depth, depth);
	executor_stats_add(&g_executor.stats.ttl_expire_task_submitted, 1);
	pthread_cond_signal(&g_executor.cmd_cv);
	pthread_mutex_unlock(&g_executor.cmd_mu);
	return 0;
}

int kvs_command_executor_submit_ttl_scan(int budget) {
	kvs_ttl_scan_task_t *task;
	uint64_t depth;

	if (budget <= 0) {
		budget = 32;
	}

	task = (kvs_ttl_scan_task_t *)kvs_malloc(sizeof(*task));
	if (!task) {
		executor_stats_add(&g_executor.stats.ttl_scan_submit_failed, 1);
		return -1;
	}
	memset(task, 0, sizeof(*task));
	task->budget = budget;
	task->enqueue_us = executor_now_us();

	pthread_mutex_lock(&g_executor.cmd_mu);
	if (!g_executor.running || g_executor.stopping) {
		pthread_mutex_unlock(&g_executor.cmd_mu);
		kvs_command_executor_destroy_ttl_scan_task(task);
		executor_stats_add(&g_executor.stats.ttl_scan_submit_failed, 1);
		return -1;
	}
	if (g_executor.ttl_scan_tail) {
		g_executor.ttl_scan_tail->queue_next = task;
	} else {
		g_executor.ttl_scan_head = task;
	}
	g_executor.ttl_scan_tail = task;
	depth = executor_stats_add(&g_executor.stats.ttl_scan_queue_depth, 1);
	executor_stats_update_max(&g_executor.stats.ttl_scan_queue_max_depth, depth);
	executor_stats_add(&g_executor.stats.ttl_scan_task_submitted, 1);
	pthread_cond_signal(&g_executor.cmd_cv);
	pthread_mutex_unlock(&g_executor.cmd_mu);
	return 0;
}

int kvs_command_executor_submit_replicated(
	ms_op_t op,
	const char *key,
	size_t key_len,
	const char *value,
	size_t value_len,
	int64_t expire_at_ms
) {
	kvs_replicated_command_task_t *task;
	uint64_t depth;

	if (!key || key_len == 0 || (value_len > 0 && !value)) {
		executor_stats_add(&g_executor.stats.repl_task_submit_failed, 1);
		return -1;
	}

	task = (kvs_replicated_command_task_t *)kvs_malloc(sizeof(*task));
	if (!task) {
		executor_stats_add(&g_executor.stats.repl_task_submit_failed, 1);
		return -1;
	}
	memset(task, 0, sizeof(*task));
	task->key = (char *)kvs_malloc(key_len + 1);
	if (!task->key) {
		kvs_free(task);
		executor_stats_add(&g_executor.stats.repl_task_submit_failed, 1);
		return -1;
	}
	memcpy(task->key, key, key_len);
	task->key[key_len] = '\0';
	task->key_len = key_len;

	if (value_len > 0) {
		task->value = (char *)kvs_malloc(value_len + 1);
		if (!task->value) {
			kvs_command_executor_destroy_repl_task(task);
			executor_stats_add(&g_executor.stats.repl_task_submit_failed, 1);
			return -1;
		}
		memcpy(task->value, value, value_len);
		task->value[value_len] = '\0';
		task->value_len = value_len;
	}
	task->op = op;
	task->expire_at_ms = expire_at_ms;
	task->enqueue_us = executor_now_us();

	pthread_mutex_lock(&g_executor.cmd_mu);
	if (!g_executor.running || g_executor.stopping) {
		pthread_mutex_unlock(&g_executor.cmd_mu);
		kvs_command_executor_destroy_repl_task(task);
		executor_stats_add(&g_executor.stats.repl_task_submit_failed, 1);
		return -1;
	}
	if (g_executor.repl_tail) {
		g_executor.repl_tail->queue_next = task;
	} else {
		g_executor.repl_head = task;
	}
	g_executor.repl_tail = task;
	depth = executor_stats_add(&g_executor.stats.repl_queue_depth, 1);
	executor_stats_update_max(&g_executor.stats.repl_queue_max_depth, depth);
	executor_stats_add(&g_executor.stats.repl_task_submitted, 1);
	pthread_cond_signal(&g_executor.cmd_cv);
	pthread_mutex_unlock(&g_executor.cmd_mu);
	return 0;
}

int kvs_command_executor_build_full_sync_snapshot(kvs_full_sync_snapshot_t **out) {
	kvs_full_sync_snapshot_task_t *task;
	int status;

	if (!out) {
		return -1;
	}
	*out = NULL;

	if (kvs_command_executor_is_current_thread()) {
		kvs_full_sync_snapshot_t *snapshot = NULL;

		if (kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "executor.full_sync_snapshot.read") != 0) {
			return -1;
		}
		snapshot = kvs_command_executor_create_full_sync_snapshot();
		kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.full_sync_snapshot.read");
		if (!snapshot) {
			return -1;
		}
		*out = snapshot;
		return 0;
	}

	task = (kvs_full_sync_snapshot_task_t *)kvs_malloc(sizeof(*task));
	if (!task) {
		return -1;
	}
	memset(task, 0, sizeof(*task));
	if (pthread_mutex_init(&task->mu, NULL) != 0) {
		kvs_free(task);
		return -1;
	}
	if (pthread_cond_init(&task->cv, NULL) != 0) {
		pthread_mutex_destroy(&task->mu);
		kvs_free(task);
		return -1;
	}

	pthread_mutex_lock(&g_executor.cmd_mu);
	if (!g_executor.running || g_executor.stopping) {
		pthread_mutex_unlock(&g_executor.cmd_mu);
		pthread_cond_destroy(&task->cv);
		pthread_mutex_destroy(&task->mu);
		kvs_free(task);
		return -1;
	}
	if (g_executor.snapshot_tail) {
		g_executor.snapshot_tail->queue_next = task;
	} else {
		g_executor.snapshot_head = task;
	}
	g_executor.snapshot_tail = task;
	pthread_cond_signal(&g_executor.cmd_cv);
	pthread_mutex_unlock(&g_executor.cmd_mu);

	pthread_mutex_lock(&task->mu);
	while (!task->done) {
		pthread_cond_wait(&task->cv, &task->mu);
	}
	status = task->status;
	if (status == 0) {
		*out = task->snapshot;
		task->snapshot = NULL;
	}
	pthread_mutex_unlock(&task->mu);

	if (task->snapshot) {
		kvs_full_sync_snapshot_destroy(task->snapshot);
		task->snapshot = NULL;
	}
	pthread_cond_destroy(&task->cv);
	pthread_mutex_destroy(&task->mu);
	kvs_free(task);
	return status;
}

int kvs_command_executor_build_rdb_snapshot(kvs_rdb_snapshot_t **out) {
	kvs_rdb_snapshot_task_t *task;
	int status;

	if (!out) {
		return -1;
	}
	*out = NULL;

	if (kvs_command_executor_is_current_thread()) {
		kvs_rdb_snapshot_t *snapshot = NULL;

		if (kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "executor.rdb_snapshot.read") != 0) {
			return -1;
		}
		snapshot = kvs_command_executor_create_rdb_snapshot();
		kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.rdb_snapshot.read");
		if (!snapshot) {
			return -1;
		}
		*out = snapshot;
		return 0;
	}

	task = (kvs_rdb_snapshot_task_t *)kvs_malloc(sizeof(*task));
	if (!task) {
		return -1;
	}
	memset(task, 0, sizeof(*task));
	if (pthread_mutex_init(&task->mu, NULL) != 0) {
		kvs_free(task);
		return -1;
	}
	if (pthread_cond_init(&task->cv, NULL) != 0) {
		pthread_mutex_destroy(&task->mu);
		kvs_free(task);
		return -1;
	}

	pthread_mutex_lock(&g_executor.cmd_mu);
	if (!g_executor.running || g_executor.stopping) {
		pthread_mutex_unlock(&g_executor.cmd_mu);
		pthread_cond_destroy(&task->cv);
		pthread_mutex_destroy(&task->mu);
		kvs_free(task);
		return -1;
	}
	if (g_executor.rdb_snapshot_tail) {
		g_executor.rdb_snapshot_tail->queue_next = task;
	} else {
		g_executor.rdb_snapshot_head = task;
	}
	g_executor.rdb_snapshot_tail = task;
	pthread_cond_signal(&g_executor.cmd_cv);
	pthread_mutex_unlock(&g_executor.cmd_mu);

	pthread_mutex_lock(&task->mu);
	while (!task->done) {
		pthread_cond_wait(&task->cv, &task->mu);
	}
	status = task->status;
	if (status == 0) {
		*out = task->snapshot;
		task->snapshot = NULL;
	}
	pthread_mutex_unlock(&task->mu);

	if (task->snapshot) {
		kvs_rdb_snapshot_destroy(task->snapshot);
		task->snapshot = NULL;
	}
	pthread_cond_destroy(&task->cv);
	pthread_mutex_destroy(&task->mu);
	kvs_free(task);
	return status;
}

int kvs_command_executor_build_aof_rewrite_snapshot(kvs_aof_rewrite_snapshot_t **out) {
	kvs_aof_rewrite_snapshot_task_t *task;
	int status;

	if (!out) {
		return -1;
	}
	*out = NULL;

	if (kvs_command_executor_is_current_thread()) {
		kvs_aof_rewrite_snapshot_t *snapshot = NULL;

		if (kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "executor.aof_rewrite_snapshot.read") != 0) {
			return -1;
		}
		snapshot = kvs_command_executor_create_aof_snapshot();
		kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "executor.aof_rewrite_snapshot.read");
		if (!snapshot) {
			return -1;
		}
		*out = snapshot;
		return 0;
	}

	task = (kvs_aof_rewrite_snapshot_task_t *)kvs_malloc(sizeof(*task));
	if (!task) {
		return -1;
	}
	memset(task, 0, sizeof(*task));
	if (pthread_mutex_init(&task->mu, NULL) != 0) {
		kvs_free(task);
		return -1;
	}
	if (pthread_cond_init(&task->cv, NULL) != 0) {
		pthread_mutex_destroy(&task->mu);
		kvs_free(task);
		return -1;
	}

	pthread_mutex_lock(&g_executor.cmd_mu);
	if (!g_executor.running || g_executor.stopping) {
		pthread_mutex_unlock(&g_executor.cmd_mu);
		pthread_cond_destroy(&task->cv);
		pthread_mutex_destroy(&task->mu);
		kvs_free(task);
		return -1;
	}
	if (g_executor.aof_snapshot_tail) {
		g_executor.aof_snapshot_tail->queue_next = task;
	} else {
		g_executor.aof_snapshot_head = task;
	}
	g_executor.aof_snapshot_tail = task;
	pthread_cond_signal(&g_executor.cmd_cv);
	pthread_mutex_unlock(&g_executor.cmd_mu);

	pthread_mutex_lock(&task->mu);
	while (!task->done) {
		pthread_cond_wait(&task->cv, &task->mu);
	}
	status = task->status;
	if (status == 0) {
		*out = task->snapshot;
		task->snapshot = NULL;
	}
	pthread_mutex_unlock(&task->mu);

	if (task->snapshot) {
		kvs_aof_rewrite_snapshot_destroy(task->snapshot);
	}
	pthread_cond_destroy(&task->cv);
	pthread_mutex_destroy(&task->mu);
	kvs_free(task);
	return status;
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
	out->ttl_expire_task_submitted = executor_stats_load(&g_executor.stats.ttl_expire_task_submitted);
	out->ttl_expire_task_executed = executor_stats_load(&g_executor.stats.ttl_expire_task_executed);
	out->ttl_expire_task_failed = executor_stats_load(&g_executor.stats.ttl_expire_task_failed);
	out->ttl_expire_task_dropped = executor_stats_load(&g_executor.stats.ttl_expire_task_dropped);
	out->ttl_expire_queue_depth = executor_stats_load(&g_executor.stats.ttl_expire_queue_depth);
	out->ttl_expire_queue_max_depth = executor_stats_load(&g_executor.stats.ttl_expire_queue_max_depth);
	out->ttl_expire_deleted_keys = executor_stats_load(&g_executor.stats.ttl_expire_deleted_keys);
	out->ttl_expire_skipped_keys = executor_stats_load(&g_executor.stats.ttl_expire_skipped_keys);
	out->ttl_expire_submit_failed = executor_stats_load(&g_executor.stats.ttl_expire_submit_failed);
	out->ttl_expire_wait_count = executor_stats_load(&g_executor.stats.ttl_expire_wait_count);
	out->ttl_expire_wait_total_us = executor_stats_load(&g_executor.stats.ttl_expire_wait_total_us);
	out->ttl_expire_wait_max_us = executor_stats_load(&g_executor.stats.ttl_expire_wait_max_us);
	out->ttl_scan_task_submitted = executor_stats_load(&g_executor.stats.ttl_scan_task_submitted);
	out->ttl_scan_task_executed = executor_stats_load(&g_executor.stats.ttl_scan_task_executed);
	out->ttl_scan_task_failed = executor_stats_load(&g_executor.stats.ttl_scan_task_failed);
	out->ttl_scan_task_dropped = executor_stats_load(&g_executor.stats.ttl_scan_task_dropped);
	out->ttl_scan_submit_failed = executor_stats_load(&g_executor.stats.ttl_scan_submit_failed);
	out->ttl_scan_queue_depth = executor_stats_load(&g_executor.stats.ttl_scan_queue_depth);
	out->ttl_scan_queue_max_depth = executor_stats_load(&g_executor.stats.ttl_scan_queue_max_depth);
	out->ttl_scan_wait_count = executor_stats_load(&g_executor.stats.ttl_scan_wait_count);
	out->ttl_scan_wait_total_us = executor_stats_load(&g_executor.stats.ttl_scan_wait_total_us);
	out->ttl_scan_wait_max_us = executor_stats_load(&g_executor.stats.ttl_scan_wait_max_us);
	out->repl_task_submitted = executor_stats_load(&g_executor.stats.repl_task_submitted);
	out->repl_task_executed = executor_stats_load(&g_executor.stats.repl_task_executed);
	out->repl_task_failed = executor_stats_load(&g_executor.stats.repl_task_failed);
	out->repl_task_dropped = executor_stats_load(&g_executor.stats.repl_task_dropped);
	out->repl_task_submit_failed = executor_stats_load(&g_executor.stats.repl_task_submit_failed);
	out->repl_queue_depth = executor_stats_load(&g_executor.stats.repl_queue_depth);
	out->repl_queue_max_depth = executor_stats_load(&g_executor.stats.repl_queue_max_depth);
	out->repl_wait_count = executor_stats_load(&g_executor.stats.repl_wait_count);
	out->repl_wait_total_us = executor_stats_load(&g_executor.stats.repl_wait_total_us);
	out->repl_wait_max_us = executor_stats_load(&g_executor.stats.repl_wait_max_us);
	out->repl_set_count = executor_stats_load(&g_executor.stats.repl_set_count);
	out->repl_del_count = executor_stats_load(&g_executor.stats.repl_del_count);
	out->repl_mod_count = executor_stats_load(&g_executor.stats.repl_mod_count);
	out->repl_ttl_count = executor_stats_load(&g_executor.stats.repl_ttl_count);
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
