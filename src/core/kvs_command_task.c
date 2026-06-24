#define KVS_VECTOR_INTERNAL
#include "kvs_command_task.h"

#include "kvstore.h"
#include "distributed.h"
#include "resp_protocol.h"
#include "kvs_memory.h"
#include "sync_mode.h"
#include "kvs_config.h"
#include "kvs_globals.h"
#include "kvs_log.h"
#include "kvs_owner_lock.h"
#include "kvs_vector.h"
#include "kvs_aof.h"

#include <string.h>
#include <strings.h>
#include <time.h>

static uint64_t kvs_command_task_now_us(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

int kvs_command_task_vector_status_to_resp(int status, char *response) {
	switch (status) {
		case KVS_VECTOR_OK:
			return resp_generate_simple_string_n(response, '+', "OK", sizeof("OK") - 1);
		case KVS_VECTOR_ERR_INVALID_ARG:
			return resp_generate_simple_string_n(response, '-', "ERR invalid vector argument", sizeof("ERR invalid vector argument") - 1);
		case KVS_VECTOR_ERR_NOT_FOUND:
			return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
		case KVS_VECTOR_ERR_NOT_INITIALIZED:
			return resp_generate_simple_string_n(response, '-', "ERR vector index not initialized", sizeof("ERR vector index not initialized") - 1);
		case KVS_VECTOR_ERR_NOT_IMPLEMENTED:
			return resp_generate_simple_string_n(response, '-', "ERR vector command not implemented", sizeof("ERR vector command not implemented") - 1);
		default:
			return resp_generate_simple_string_n(response, '-', "ERR vector operation failed", sizeof("ERR vector operation failed") - 1);
	}
}

static int kvs_vector_status_to_resp(int status, char *response) {
	return kvs_command_task_vector_status_to_resp(status, response);
}

static int kvs_vector_search_prepared_to_resp(
	const command_context_t *ctx,
	const kvs_vector_search_prepared_t *prepared,
	char *response
) {
	int topk;
	size_t results_capacity;
	kvs_vector_search_result_t *results = NULL;
	size_t results_count = 0;
	size_t i;
	int status;
	int off = 0;
	uint64_t response_start_us;
	uint64_t response_end_us;

	if (!ctx || !prepared) {
		return kvs_vector_status_to_resp(KVS_VECTOR_ERR_INVALID_ARG, response);
	}

	topk = ctx->topk;
	if (topk <= 0) {
		topk = 1;
	}
	if (topk > 1024) {
		return kvs_vector_status_to_resp(KVS_VECTOR_ERR_INVALID_ARG, response);
	}

	results_capacity = (size_t)topk;
	results = (kvs_vector_search_result_t *)kvs_malloc(
		results_capacity * sizeof(kvs_vector_search_result_t)
	);
	if (!results) {
		return kvs_vector_status_to_resp(KVS_VECTOR_ERR, response);
	}

	kvs_vector_search_results_reset(results, results_capacity);
	status = kvs_vector_search_with_prepared(
		prepared,
		topk,
		ctx->has_threshold ? ctx->threshold : g_cfg.vector_score_threshold,
		results,
		results_capacity,
		&results_count
	);
	if (status != KVS_VECTOR_OK) {
		kvs_free(results);
		return kvs_vector_status_to_resp(status, response);
	}

	response_start_us = kvs_command_task_now_us();
	off += resp_generate_array_start(response + off, (int)results_count);
	for (i = 0; i < results_count; ++i) {
		char *item = NULL;
		int item_cap;
		int n;

		item_cap = snprintf(
			NULL, 0,
			"score:%.6f|scope:%.*s|question:%.*s|answer:%.*s",
			results[i].score,
			(int)results[i].scope_len,
			results[i].scope ? results[i].scope : "",
			(int)results[i].question_len,
			results[i].question ? results[i].question : "",
			(int)results[i].answer_len,
			results[i].answer ? results[i].answer : ""
		);
		if (item_cap < 0) {
			kvs_free(results);
			return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
		}

		item_cap += 1;
		item = (char *)kvs_malloc((size_t)item_cap);
		if (!item) {
			kvs_free(results);
			return resp_generate_simple_string_n(response, '-', "ERR out of memory", sizeof("ERR out of memory") - 1);
		}

		n = snprintf(
			item, (size_t)item_cap,
			"score:%.6f|scope:%.*s|question:%.*s|answer:%.*s",
			results[i].score,
			(int)results[i].scope_len,
			results[i].scope ? results[i].scope : "",
			(int)results[i].question_len,
			results[i].question ? results[i].question : "",
			(int)results[i].answer_len,
			results[i].answer ? results[i].answer : ""
		);
		if (n < 0 || n >= item_cap) {
			kvs_free(item);
			kvs_free(results);
			return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
		}

		off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
		kvs_free(item);
	}
	response_end_us = kvs_command_task_now_us();
	kvs_vector_vsearch_record_response_build(
		response_end_us >= response_start_us ? response_end_us - response_start_us : 0
	);

	kvs_free(results);
	return off;
}

static void kvs_command_task_reset(kvs_command_task_t *task) {
	if (!task) {
		return;
	}
	memset(task, 0, sizeof(*task));
	task->cmd_index = -1;
	task->fd = -1;
	kvs_vector_set_prepared_reset(&task->prepared_set);
	kvs_vector_search_prepared_reset(&task->prepared_search);
}

static void kvs_command_task_free_owned_argv(kvs_command_task_t *task) {
	int i;

	if (!task || !task->owns_argv) {
		return;
	}

	if (task->argv) {
		for (i = 0; i < task->argc; ++i) {
			if (task->argv[i]) {
				kvs_free(task->argv[i]);
			}
		}
		kvs_free(task->argv);
	}
	if (task->argv_lens) {
		kvs_free(task->argv_lens);
	}

	task->argv = NULL;
	task->argv_lens = NULL;
	task->argc = 0;
	task->owns_argv = 0;
}

void kvs_command_task_destroy(kvs_command_task_t *task) {
	if (!task) {
		return;
	}

	if (task->lock_held) {
		kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "command_task.destroy");
		task->lock_held = 0;
	}

	if (task->prepared_kind == KVS_COMMAND_TASK_PREPARED_VSET) {
		kvs_vector_set_prepared_destroy(&task->prepared_set);
	} else if (task->prepared_kind == KVS_COMMAND_TASK_PREPARED_VSEARCH) {
		kvs_vector_search_prepared_destroy(&task->prepared_search);
	}

	/*
	 * Stage-4A adds optional owned argv for future executor use. The current
	 * synchronous path still leaves owns_argv=0 and remains zero-copy.
	 */
	kvs_command_task_free_owned_argv(task);
	task->prepared = NULL;
	task->is_prepared = 0;
}

int kvs_command_task_copy_argv(kvs_command_task_t *task) {
	char **argv_copy = NULL;
	size_t *lens_copy = NULL;
	command_context_t copied_ctx;
	int i;

	if (!task || task->argc <= 0 || !task->argv || !task->argv_lens) {
		return -1;
	}
	if (task->owns_argv) {
		return 0;
	}
	if ((size_t)task->argc > ((size_t)-1) / sizeof(char *)) {
		return -1;
	}

	argv_copy = (char **)kvs_malloc((size_t)task->argc * sizeof(char *));
	if (!argv_copy) {
		return -1;
	}
	memset(argv_copy, 0, (size_t)task->argc * sizeof(char *));

	lens_copy = (size_t *)kvs_malloc((size_t)task->argc * sizeof(size_t));
	if (!lens_copy) {
		kvs_free(argv_copy);
		return -1;
	}
	memset(lens_copy, 0, (size_t)task->argc * sizeof(size_t));

	for (i = 0; i < task->argc; ++i) {
		size_t len = task->argv_lens[i];

		if (!task->argv[i] && len > 0) {
			goto fail;
		}
		if (len == (size_t)-1) {
			goto fail;
		}
		lens_copy[i] = len;
		argv_copy[i] = (char *)kvs_malloc(len + 1);
		if (!argv_copy[i]) {
			goto fail;
		}
		if (len > 0) {
			memcpy(argv_copy[i], task->argv[i], len);
		}
		argv_copy[i][len] = '\0';
	}

	if (task->cmd && task->cmd_index >= 0 && task->build_status <= 0) {
		memset(&copied_ctx, 0, sizeof(copied_ctx));
		if (parse_command_argument(task->cmd_index, argv_copy, task->argc, lens_copy, &copied_ctx) < 0) {
			goto fail;
		}
	}

	task->argv = argv_copy;
	task->argv_lens = lens_copy;
	task->owns_argv = 1;
	if (task->cmd && task->cmd_index >= 0 && task->build_status <= 0) {
		task->ctx = copied_ctx;
	}

	return 0;

fail:
	if (argv_copy) {
		for (i = 0; i < task->argc; ++i) {
			if (argv_copy[i]) {
				kvs_free(argv_copy[i]);
			}
		}
		kvs_free(argv_copy);
	}
	if (lens_copy) {
		kvs_free(lens_copy);
	}
	return -1;
}

int kvs_command_task_requires_embedding_prepare(const kvs_command_task_t *task) {
	if (!task || !task->cmd || task->build_status > 0) {
		return 0;
	}
	return strcmp(task->cmd->name, "VSET") == 0 ||
	       strcmp(task->cmd->name, "VSEARCH") == 0;
}

int kvs_command_task_build(
	kvs_command_task_t *task,
	char **tokens,
	int count,
	size_t *token_lens,
	kvs_response_task_t *response_task
) {
	char *response;

	kvs_command_task_reset(task);

	task->argc = count;
	task->argv = tokens;
	task->argv_lens = token_lens;
	task->response_task = response_task;
	response = kvs_response_task_buffer(response_task);
	task->response = response;
	task->response_cap = kvs_response_task_capacity(response_task);
	task->owns_argv = 0;
	task->sync_context = get_sync_context();

	if (tokens[0] == NULL || count == 0) {
		task->status = resp_generate_simple_string_n(response, '-', "Invalid command", sizeof("Invalid command") - 1);
		task->build_status = task->status;
		return -1;
	}

	task->cmd = lookup_command(tokens[0]);
	if (task->cmd == NULL) {
		task->status = resp_generate_simple_string_n(response, '-', "Unknown command", sizeof("Unknown command") - 1);
		task->build_status = task->status;
		return -1;
	}

	task->cmd_index = get_command_index(tokens[0]);
	if (task->cmd_index == -1) {
		task->status = resp_generate_simple_string_n(response, '-', "Unknown command", sizeof("Unknown command") - 1);
		task->build_status = task->status;
		return -1;
	}

	if (!check_arity(task->cmd, count)) {
		task->status = resp_generate_simple_string_n(response, '-', "Wrong number of arguments", sizeof("Wrong number of arguments") - 1);
		task->build_status = task->status;
		return -1;
	}

	if (task->cmd->write_command &&
	    global_dist_config.role == ROLE_SLAVE &&
	    get_sync_context() == 0) {
		task->status = resp_generate_simple_string_n(response, '-', "ERROR SLAVE READ ONLY", sizeof("ERROR SLAVE READ ONLY") - 1);
		task->build_status = task->status;
		return -1;
	}

	if (parse_command_argument(task->cmd_index, tokens, count, token_lens, &task->ctx) < 0) {
		task->status = resp_generate_simple_string_n(response, '-', "Invalid arguments", sizeof("Invalid arguments") - 1);
		task->build_status = task->status;
		return -1;
	}

	switch (task->cmd->lock_mode) {
		case KVS_CMD_LOCK_READ:
			task->use_read_lock = 1;
			break;
		case KVS_CMD_LOCK_WRITE:
		default:
			task->use_write_lock = 1;
			break;
		case KVS_CMD_LOCK_NONE:
			break;
	}

	return 0;
}

int kvs_command_task_prepare(kvs_command_task_t *task) {
	int status;
	char *response = task ? kvs_response_task_buffer(task->response_task) : NULL;

	if (!task || !task->cmd) {
		return resp_generate_simple_string_n(response, '-', "Invalid command", sizeof("Invalid command") - 1);
	}
	task->response = response;
	task->prepare_completed = 1;

	if (strcmp(task->cmd->name, "VSET") == 0) {
		task->prepared_kind = KVS_COMMAND_TASK_PREPARED_VSET;
		task->prepared = &task->prepared_set;
		status = kvs_vector_set_prepare(
			task->ctx.query_text,
			task->ctx.query_text_len,
			task->ctx.value,
			task->ctx.value ? strlen(task->ctx.value) : 0,
			task->ctx.scope,
			task->ctx.scope_len,
			&task->prepared_set
		);
		if (status != KVS_VECTOR_OK) {
			task->status = kvs_vector_status_to_resp(status, task->response);
			task->prepare_status = task->status;
			return task->status;
		}
		task->is_prepared = 1;
		task->prepare_status = 0;
		return 0;
	}

	if (strcmp(task->cmd->name, "VSEARCH") == 0) {
		task->prepared_kind = KVS_COMMAND_TASK_PREPARED_VSEARCH;
		task->prepared = &task->prepared_search;
		status = kvs_vector_search_prepare(
			task->ctx.query_text,
			task->ctx.query_text_len,
			task->ctx.scope,
			task->ctx.scope_len,
			&task->prepared_search
		);
		if (status != KVS_VECTOR_OK) {
			task->status = kvs_vector_status_to_resp(status, task->response);
			task->prepare_status = task->status;
			return task->status;
		}
		task->is_prepared = 1;
		task->prepare_status = 0;
		return 0;
	}

	task->prepare_status = 0;
	return 0;
}

static int kvs_command_task_lock(kvs_command_task_t *task) {
	uint64_t start_us;
	uint64_t end_us;

	task->response = kvs_response_task_buffer(task->response_task);

	if (task->use_read_lock) {
		start_us = kvs_command_task_now_us();
		if (kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "command_task.execute.read") != 0) {
			task->status = resp_generate_simple_string_n(task->response, '-', "Failed to acquire data lock", sizeof("Failed to acquire data lock") - 1);
			return -1;
		}
		end_us = kvs_command_task_now_us();
		task->lock_wait_us += end_us >= start_us ? end_us - start_us : 0;
		task->lock_acquired_us = end_us;
		task->lock_held = 1;
		task->ctx.lock_mode = KVS_LOCK_MODE_READ;
	} else if (task->use_write_lock) {
		start_us = kvs_command_task_now_us();
		if (kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "command_task.execute.write") != 0) {
			task->status = resp_generate_simple_string_n(task->response, '-', "Failed to acquire data lock", sizeof("Failed to acquire data lock") - 1);
			return -1;
		}
		end_us = kvs_command_task_now_us();
		task->lock_wait_us += end_us >= start_us ? end_us - start_us : 0;
		task->lock_acquired_us = end_us;
		task->lock_held = 1;
		task->ctx.lock_mode = KVS_LOCK_MODE_WRITE;
	}

	return 0;
}

static void kvs_command_task_unlock(kvs_command_task_t *task) {
	uint64_t end_us;

	if (!task || !task->lock_held) {
		return;
	}
	end_us = kvs_command_task_now_us();
	if (task->lock_acquired_us > 0) {
		task->lock_hold_us += end_us >= task->lock_acquired_us ?
			end_us - task->lock_acquired_us :
			0;
	}
	kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "command_task.execute.unlock");
	task->lock_held = 0;
	task->lock_acquired_us = 0;
}

static void kvs_command_task_capture_post_state(kvs_command_task_t *task, int ret) {
	if (!task->command_committed || !task->cmd->write_command) {
		return;
	}

	if (strcmp(task->cmd->name, "EXPIRE") == 0 ||
	    strcmp(task->cmd->name, "PEXPIREAT") == 0) {
		if (ttl_get_any_key_expire_at_ms(task->ctx.key, task->ctx.key_len, &task->expire_at_ms) > 0 &&
		    task->expire_at_ms >= 0) {
			task->has_expire_at = 1;
		} else {
			task->need_del_sync = (ret > 0);
		}
	} else if (strcmp(task->cmd->name, "PERSIST") == 0) {
		task->need_persist_sync = (ret > 0);
	}
}

static int kvs_command_task_execute_handler(kvs_command_task_t *task) {
	int ret;

	task->response = kvs_response_task_buffer(task->response_task);

	if (strcmp(task->cmd->name, "VSET") == 0) {
		int status = kvs_vector_set_commit_prepared(&task->prepared_set);
		task->prepared_kind = KVS_COMMAND_TASK_PREPARED_NONE;
		task->command_committed = (status == KVS_VECTOR_OK);
		ret = kvs_vector_status_to_resp(status, task->response);
	} else if (strcmp(task->cmd->name, "VSEARCH") == 0) {
		uint64_t start_us = kvs_command_task_now_us();
		uint64_t end_us;
		ret = kvs_vector_search_prepared_to_resp(
			&task->ctx,
			&task->prepared_search,
			task->response
		);
		end_us = kvs_command_task_now_us();
		task->vsearch_search_us += end_us >= start_us ? end_us - start_us : 0;
		task->command_committed = (ret >= 0);
	} else {
		ret = task->cmd->handler(&task->ctx, task->response);
		task->command_committed = (ret >= 0);
	}

	return ret;
}

int kvs_command_task_execute_sync(kvs_command_task_t *task) {
	int ret;
	int old_sync_context;

	if (kvs_command_task_lock(task) < 0) {
		return task->status;
	}

	old_sync_context = get_sync_context();
	set_sync_context(task->sync_context);
	ret = kvs_command_task_execute_handler(task);
	if (task->use_read_lock && ret == KVS_CMD_RETRY_WITH_WRITE_LOCK) {
		kvs_command_task_unlock(task);
		task->use_read_lock = 0;
		task->use_write_lock = 1;

		task->lock_acquired_us = 0;
		uint64_t start_us = kvs_command_task_now_us();
		uint64_t end_us;
			if (kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "command_task.lazy_expire.retry") != 0) {
				task->status = resp_generate_simple_string_n(task->response, '-', "Failed to acquire data lock", sizeof("Failed to acquire data lock") - 1);
				set_sync_context(old_sync_context);
				return task->status;
		}
		end_us = kvs_command_task_now_us();
		task->lock_wait_us += end_us >= start_us ? end_us - start_us : 0;
		task->lock_acquired_us = end_us;
		task->lock_held = 1;
		task->ctx.lock_mode = KVS_LOCK_MODE_WRITE;
		ret = task->cmd->handler(&task->ctx, task->response);
		task->command_committed = (ret >= 0);
	}
	set_sync_context(old_sync_context);

	kvs_command_task_capture_post_state(task, ret);

	if (task->lock_held) {
		kvs_command_task_unlock(task);
	}

	task->status = ret;
	return ret;
}

void kvs_command_task_post_commit(kvs_command_task_t *task, int ret) {
	if (!task->command_committed || !task->cmd->write_command) {
		return;
	}

#if ENABLE_AOF
	if (!aof_is_loading()) {
		uint64_t start_us = 0;
		uint64_t end_us = 0;
		if (strcmp(task->cmd->name, "VSET") == 0) {
			start_us = kvs_command_task_now_us();
		}
		if (strcmp(task->cmd->name, "EXPIRE") == 0) {
			if (task->has_expire_at) {
				char expire_buf[32];
				snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)task->expire_at_ms);
				char *aof_tokens[] = {"PEXPIREAT", task->ctx.key, expire_buf};
				aof_append(3, aof_tokens, NULL);
			} else if (task->need_del_sync) {
				char *aof_tokens[] = {"DEL", task->ctx.key};
				aof_append(2, aof_tokens, NULL);
			}
		} else {
			aof_append(task->argc, task->argv, task->argv_lens);
		}
		if (strcmp(task->cmd->name, "VSET") == 0) {
			end_us = kvs_command_task_now_us();
			task->vset_aof_append_us += end_us >= start_us ? end_us - start_us : 0;
		}
	}
#endif

	if (task->sync_context == 2) {
		return;
	}

	if (global_dist_config.role == ROLE_MASTER && g_cfg.sync_mode == KVS_SYNC_TCP) {
		uint64_t start_us = 0;
		uint64_t end_us = 0;
		if (strcmp(task->cmd->name, "VSET") == 0) {
			start_us = kvs_command_task_now_us();
		}
		if (strcmp(task->cmd->name, "EXPIRE") == 0 ||
		    strcmp(task->cmd->name, "PEXPIREAT") == 0) {
			if (ret > 0) {
				if (task->has_expire_at) {
					char expire_buf[32];
					snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)task->expire_at_ms);
					master_sync_command("PEXPIREAT", task->ctx.key, task->ctx.key_len, expire_buf, strlen(expire_buf));
				} else {
					master_sync_command("DEL", task->ctx.key, task->ctx.key_len, NULL, 0);
				}
			}
		} else if (strcmp(task->cmd->name, "PERSIST") == 0) {
			if (task->need_persist_sync) {
				master_sync_command("PERSIST", task->ctx.key, task->ctx.key_len, NULL, 0);
			}
		} else {
			master_sync_command(
				task->argv[0],
				task->argv[1],
				task->argv_lens[1],
				(task->argc > 2) ? task->argv[2] : NULL,
				(task->argc > 2) ? task->argv_lens[2] : 0
			);
		}
		if (strcmp(task->cmd->name, "VSET") == 0) {
			end_us = kvs_command_task_now_us();
			task->vset_replication_us += end_us >= start_us ? end_us - start_us : 0;
		}
	}
}

void kvs_command_task_get_vset_post_commit_timings(
	const kvs_command_task_t *task,
	uint64_t *aof_append_us,
	uint64_t *replication_us,
	uint64_t *dirty_mark_us
) {
	if (aof_append_us) {
		*aof_append_us = task ? task->vset_aof_append_us : 0;
	}
	if (replication_us) {
		*replication_us = task ? task->vset_replication_us : 0;
	}
	if (dirty_mark_us) {
		*dirty_mark_us = task ? task->vset_dirty_mark_us : 0;
	}
}

uint64_t kvs_command_task_get_vsearch_search_us(const kvs_command_task_t *task) {
	return task ? task->vsearch_search_us : 0;
}
