#include "kvs_command_executor.h"

#include "kvs_async_net_adapter.h"
#include "kvs_memory.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

typedef struct kvs_command_executor_s {
	pthread_t thread;
	pthread_mutex_t cmd_mu;
	pthread_cond_t cmd_cv;
	kvs_command_task_t *cmd_head;
	kvs_command_task_t *cmd_tail;
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

static void kvs_command_executor_execute(kvs_command_task_t *task) {
	kvs_response_task_t *resp;
	int ret;

	if (!task) {
		return;
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
	} else {
		ret = kvs_command_task_prepare(task);
		if (ret == 0) {
			ret = kvs_command_task_execute_sync(task);
			kvs_command_task_post_commit(task, ret);
		}
	}

	kvs_response_task_finalize(resp, ret);
	task->response_task = NULL;
	kvs_command_task_destroy(task);
	kvs_free(task);
	executor_stats_add(&g_executor.stats.completed_tasks, 1);
	kvs_command_executor_push_response(resp);
}

static void *kvs_command_executor_thread_main(void *arg) {
	(void)arg;

	for (;;) {
		kvs_command_task_t *task;

		pthread_mutex_lock(&g_executor.cmd_mu);
		while (!g_executor.cmd_head && !g_executor.stopping) {
			pthread_cond_wait(&g_executor.cmd_cv, &g_executor.cmd_mu);
		}
		if (!g_executor.cmd_head && g_executor.stopping) {
			pthread_mutex_unlock(&g_executor.cmd_mu);
			break;
		}
		task = kvs_command_executor_pop_command_locked();
		pthread_mutex_unlock(&g_executor.cmd_mu);

		kvs_command_executor_execute(task);
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

	g_executor.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (g_executor.event_fd < 0) {
		return -1;
	}

	if (pthread_create(&g_executor.thread, NULL, kvs_command_executor_thread_main, NULL) != 0) {
		close(g_executor.event_fd);
		g_executor.event_fd = -1;
		return -1;
	}

	pthread_mutex_lock(&g_executor.cmd_mu);
	g_executor.running = 1;
	pthread_mutex_unlock(&g_executor.cmd_mu);
	return 0;
}

void kvs_command_executor_stop(void) {
	kvs_command_task_t *task;
	kvs_response_task_t *resp;
	int should_join;

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
	pthread_mutex_unlock(&g_executor.cmd_mu);
	while (task) {
		kvs_command_task_t *next = task->queue_next;
		task->queue_next = NULL;
		executor_stats_sub(&g_executor.stats.command_queue_depth, 1);
		kvs_command_executor_destroy_task(task);
		task = next;
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
