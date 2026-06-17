#ifndef KVS_COMMAND_EXECUTOR_H
#define KVS_COMMAND_EXECUTOR_H

#include <stdint.h>

#include "kvs_command_task.h"
#include "kvs_response_task.h"
#include "kvs_vector.h"
#include "master_slave_proto.h"

typedef struct kvs_ttl_expire_task_s {
	struct kvs_ttl_expire_task_s *queue_next;
	char *key;
	size_t key_len;
	int64_t observed_expire_at_ms;
	uint64_t enqueue_us;
} kvs_ttl_expire_task_t;

typedef struct kvs_replicated_command_task_s {
	struct kvs_replicated_command_task_s *queue_next;
	ms_op_t op;
	char *key;
	size_t key_len;
	char *value;
	size_t value_len;
	int64_t expire_at_ms;
	uint64_t enqueue_us;
} kvs_replicated_command_task_t;

typedef enum kvs_full_sync_snapshot_backend_e {
	KVS_FULL_SYNC_SNAPSHOT_BACKEND_ARRAY = 1,
	KVS_FULL_SYNC_SNAPSHOT_BACKEND_RBTREE = 2,
	KVS_FULL_SYNC_SNAPSHOT_BACKEND_HASH = 3,
	KVS_FULL_SYNC_SNAPSHOT_BACKEND_SKIPTABLE = 4,
} kvs_full_sync_snapshot_backend_t;

typedef struct kvs_full_sync_snapshot_entry_s {
	kvs_full_sync_snapshot_backend_t backend;
	char *key;
	size_t key_len;
	char *value;
	size_t value_len;
	int64_t expire_at_ms;
} kvs_full_sync_snapshot_entry_t;

typedef struct kvs_full_sync_snapshot_s {
	kvs_full_sync_snapshot_entry_t *entries;
	size_t entry_count;
	int64_t captured_at_ms;
} kvs_full_sync_snapshot_t;

typedef struct kvs_rdb_snapshot_kv_entry_s {
	char *key;
	size_t key_len;
	char *value;
	size_t value_len;
	int64_t expire_at_ms;
} kvs_rdb_snapshot_kv_entry_t;

typedef struct kvs_rdb_snapshot_vector_record_s {
	size_t record_index;
	uint64_t id;
	char *question;
	size_t question_len;
	char *answer;
	size_t answer_len;
	char *scope;
	size_t scope_len;
	char *normalized_question;
	size_t normalized_question_len;
	float *embedding;
	int embedding_dim;
	int state;
	int deleted;
	int64_t created_at_ms;
	int64_t updated_at_ms;
} kvs_rdb_snapshot_vector_record_t;

typedef struct kvs_rdb_snapshot_vector_section_s {
	int initialized;
	kvs_vector_metric_type_t metric_type;
	kvs_vector_index_type_t index_type;
	uint64_t next_id;
	size_t record_count;
	size_t active_records;
	size_t deleted_records;
	kvs_rdb_snapshot_vector_record_t *records;
} kvs_rdb_snapshot_vector_section_t;

typedef struct kvs_rdb_snapshot_s {
	int64_t captured_at_ms;
	kvs_rdb_snapshot_kv_entry_t *array_entries;
	size_t array_count;
	kvs_rdb_snapshot_kv_entry_t *rbtree_entries;
	size_t rbtree_count;
	kvs_rdb_snapshot_kv_entry_t *hash_entries;
	size_t hash_count;
	kvs_rdb_snapshot_kv_entry_t *skiptable_entries;
	size_t skiptable_count;
	kvs_rdb_snapshot_vector_section_t vector_section;
} kvs_rdb_snapshot_t;

typedef struct kvs_aof_rewrite_snapshot_vector_entry_s {
	char *question;
	size_t question_len;
	char *answer;
	size_t answer_len;
	char *scope;
	size_t scope_len;
} kvs_aof_rewrite_snapshot_vector_entry_t;

typedef struct kvs_aof_rewrite_snapshot_s {
	int64_t captured_at_ms;
	kvs_rdb_snapshot_kv_entry_t *array_entries;
	size_t array_count;
	kvs_rdb_snapshot_kv_entry_t *rbtree_entries;
	size_t rbtree_count;
	kvs_rdb_snapshot_kv_entry_t *hash_entries;
	size_t hash_count;
	kvs_rdb_snapshot_kv_entry_t *skiptable_entries;
	size_t skiptable_count;
	kvs_aof_rewrite_snapshot_vector_entry_t *vector_entries;
	size_t vector_count;
} kvs_aof_rewrite_snapshot_t;

typedef struct kvs_command_executor_stats_s {
	uint64_t submitted_tasks;
	uint64_t completed_tasks;
	uint64_t submit_failed;
	uint64_t command_queue_depth;
	uint64_t command_queue_max_depth;
	uint64_t response_queue_depth;
	uint64_t response_queue_max_depth;
	uint64_t pushed_responses;
	uint64_t polled_responses;
	uint64_t eventfd_wakeup_count;
	uint64_t eventfd_write_fail_count;
	uint64_t dropped_responses;
	uint64_t session_mismatch_drop;
	uint64_t session_closing_drop;
	uint64_t response_append_failed;
	uint64_t prepared_queue_depth;
	uint64_t prepared_queue_max_depth;
	uint64_t prepared_tasks;
	uint64_t embedding_prepare_dispatched;
	uint64_t embedding_prepare_inline;
	uint64_t embedding_prepare_submit_failed;
	uint64_t normal_task_executed;
	uint64_t prepare_task_submitted;
	uint64_t prepared_task_executed;
	uint64_t prepared_task_failed;
	uint64_t prepared_wait_count;
	uint64_t prepared_wait_total_us;
	uint64_t prepared_wait_max_us;
	uint64_t prepared_task_execute_count;
	uint64_t prepared_task_execute_total_us;
	uint64_t prepared_task_execute_max_us;
	uint64_t execute_sync_count;
	uint64_t execute_sync_total_us;
	uint64_t execute_sync_max_us;
	uint64_t commit_latency_count;
	uint64_t commit_latency_total_us;
	uint64_t commit_latency_max_us;
	uint64_t commit_or_search_count;
	uint64_t commit_or_search_total_us;
	uint64_t commit_or_search_max_us;
	uint64_t finalize_response_count;
	uint64_t finalize_response_total_us;
	uint64_t finalize_response_max_us;
	uint64_t vset_prepared_tasks;
	uint64_t vset_prepared_wait_total_us;
	uint64_t vset_prepared_wait_max_us;
	uint64_t vset_execute_sync_count;
	uint64_t vset_execute_sync_total_us;
	uint64_t vset_execute_sync_max_us;
	uint64_t vset_post_commit_count;
	uint64_t vset_post_commit_total_us;
	uint64_t vset_post_commit_max_us;
	uint64_t vset_finalize_response_count;
	uint64_t vset_finalize_response_total_us;
	uint64_t vset_finalize_response_max_us;
	uint64_t vset_push_response_count;
	uint64_t vset_push_response_total_us;
	uint64_t vset_push_response_max_us;
	uint64_t vsearch_execute_sync_count;
	uint64_t vsearch_execute_sync_total_us;
	uint64_t vsearch_execute_sync_max_us;
	uint64_t ttl_expire_task_submitted;
	uint64_t ttl_expire_task_executed;
	uint64_t ttl_expire_task_failed;
	uint64_t ttl_expire_task_dropped;
	uint64_t ttl_expire_queue_depth;
	uint64_t ttl_expire_queue_max_depth;
	uint64_t ttl_expire_deleted_keys;
	uint64_t ttl_expire_skipped_keys;
	uint64_t ttl_expire_submit_failed;
	uint64_t ttl_expire_wait_count;
	uint64_t ttl_expire_wait_total_us;
	uint64_t ttl_expire_wait_max_us;
	uint64_t repl_task_submitted;
	uint64_t repl_task_executed;
	uint64_t repl_task_failed;
	uint64_t repl_task_dropped;
	uint64_t repl_task_submit_failed;
	uint64_t repl_queue_depth;
	uint64_t repl_queue_max_depth;
	uint64_t repl_wait_count;
	uint64_t repl_wait_total_us;
	uint64_t repl_wait_max_us;
	uint64_t repl_set_count;
	uint64_t repl_del_count;
	uint64_t repl_mod_count;
	uint64_t repl_ttl_count;
} kvs_command_executor_stats_t;

typedef enum kvs_command_executor_drop_reason_e {
	KVS_COMMAND_EXECUTOR_DROP_SESSION_MISMATCH = 1,
	KVS_COMMAND_EXECUTOR_DROP_SESSION_CLOSING = 2,
	KVS_COMMAND_EXECUTOR_DROP_APPEND_FAILED = 3,
} kvs_command_executor_drop_reason_t;

int kvs_command_executor_start(void);
void kvs_command_executor_stop(void);

int kvs_command_executor_submit(kvs_command_task_t *task);
int kvs_command_executor_submit_prepared(kvs_command_task_t *task);
int kvs_command_executor_submit_ttl_expire(const char *key,
                                           size_t key_len,
                                           int64_t observed_expire_at_ms);
int kvs_command_executor_submit_replicated(ms_op_t op,
                                           const char *key,
                                           size_t key_len,
                                           const char *value,
                                           size_t value_len,
                                           int64_t expire_at_ms);
int kvs_command_executor_build_full_sync_snapshot(kvs_full_sync_snapshot_t **out);
int kvs_command_executor_build_rdb_snapshot(kvs_rdb_snapshot_t **out);
int kvs_command_executor_build_aof_rewrite_snapshot(kvs_aof_rewrite_snapshot_t **out);
int kvs_command_executor_poll_response(kvs_response_task_t **resp);
int kvs_command_executor_poll_response_for(int owner_type,
                                           int owner_poller_id,
                                           kvs_response_task_t **resp);
int kvs_command_executor_poll_response_for_owner(int owner_type,
                                                 int owner_poller_id,
                                                 kvs_response_task_t **resp);
int kvs_command_executor_response_event_fd(void);
void kvs_command_executor_get_stats(kvs_command_executor_stats_t *out);
void kvs_command_executor_record_response_drop(int reason);
void kvs_full_sync_snapshot_destroy(kvs_full_sync_snapshot_t *snapshot);
void kvs_rdb_snapshot_destroy(kvs_rdb_snapshot_t *snapshot);
void kvs_aof_rewrite_snapshot_destroy(kvs_aof_rewrite_snapshot_t *snapshot);

#endif /* KVS_COMMAND_EXECUTOR_H */
