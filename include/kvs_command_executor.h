#ifndef KVS_COMMAND_EXECUTOR_H
#define KVS_COMMAND_EXECUTOR_H

#include <stdint.h>

#include "kvs_command_task.h"
#include "kvs_response_task.h"

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
} kvs_command_executor_stats_t;

typedef enum kvs_command_executor_drop_reason_e {
	KVS_COMMAND_EXECUTOR_DROP_SESSION_MISMATCH = 1,
	KVS_COMMAND_EXECUTOR_DROP_SESSION_CLOSING = 2,
	KVS_COMMAND_EXECUTOR_DROP_APPEND_FAILED = 3,
} kvs_command_executor_drop_reason_t;

int kvs_command_executor_start(void);
void kvs_command_executor_stop(void);

int kvs_command_executor_submit(kvs_command_task_t *task);
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

#endif /* KVS_COMMAND_EXECUTOR_H */
