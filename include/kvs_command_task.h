#ifndef KVS_COMMAND_TASK_H
#define KVS_COMMAND_TASK_H

#include <stdint.h>

#include "kvs_command.h"
#include "kvs_response_task.h"
#include "kvs_vector.h"

typedef struct net_session_s net_session_t;

typedef enum kvs_command_task_prepared_kind_e {
	KVS_COMMAND_TASK_PREPARED_NONE = 0,
	KVS_COMMAND_TASK_PREPARED_VSET,
	KVS_COMMAND_TASK_PREPARED_VSEARCH,
} kvs_command_task_prepared_kind_t;

typedef struct kvs_command_task_s {
	struct kvs_command_task_s *queue_next;
	const command_info *cmd;
	int cmd_index;
	int argc;
	char **argv;
	size_t *argv_lens;

	kvs_response_task_t *response_task;
	char *response;
	size_t response_cap;
	int status;
	int build_status;
	int command_committed;

	/*
	 * Stage-2 remains synchronous. owns_argv is intentionally 0 for normal
	 * commands: argv points into the current RESP parse buffer and must not
	 * escape this kvs_filter_protocol call.
	 */
	int owns_argv;
	int is_prepared;
	kvs_command_task_prepared_kind_t prepared_kind;
	void *prepared;

	command_context_t ctx;
	int use_read_lock;
	int use_write_lock;
	int lock_held;

	int64_t expire_at_ms;
	int has_expire_at;
	int need_del_sync;
	int need_persist_sync;

	net_session_t *session;
	void *session_owner;
	void (*session_release)(void *session_owner);
	int fd;
	uint64_t session_id;
	uint32_t generation;
	uint64_t request_seq;
	int owner_type;
	int owner_poller_id;
	int sync_context;
	int is_replication;

	kvs_vector_set_prepared_t prepared_set;
	kvs_vector_search_prepared_t prepared_search;
} kvs_command_task_t;

int kvs_command_task_build(kvs_command_task_t *task, char **tokens, int count, size_t *token_lens, kvs_response_task_t *response_task);
int kvs_command_task_copy_argv(kvs_command_task_t *task);
int kvs_command_task_prepare(kvs_command_task_t *task);
int kvs_command_task_execute_sync(kvs_command_task_t *task);
void kvs_command_task_post_commit(kvs_command_task_t *task, int ret);
void kvs_command_task_destroy(kvs_command_task_t *task);

#endif /* KVS_COMMAND_TASK_H */
