#include "kvs_embedding_worker_pool.h"

#include "kvs_command_executor.h"
#include "kvs_config.h"
#include "kvs_globals.h"
#include "kvs_log.h"
#include "kvs_memory.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* ── Internal task node ─────────────────────────────────────────────────── */

typedef struct worker_task_node_s {
	kvs_command_task_t *task;
	struct worker_task_node_s *next;
} worker_task_node_t;

/* ── Worker pool state ─────────────────────────────────────────────────── */

typedef struct {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	worker_task_node_t *head;
	worker_task_node_t *tail;
	pthread_t *threads;
	int num_threads;
	int queue_capacity;
	int running;
	int stopping;
	int enabled;
	kvs_embedding_worker_pool_stats_t stats;
} worker_pool_t;

static worker_pool_t g_pool = {
	.mu = PTHREAD_MUTEX_INITIALIZER,
	.cv = PTHREAD_COND_INITIALIZER,
};

/* ── Helpers ────────────────────────────────────────────────────────────── */

static uint64_t pool_now_us(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void pool_stats_add(uint64_t *field, uint64_t value) {
	__atomic_add_fetch(field, value, __ATOMIC_RELAXED);
}

static void pool_stats_update_max(uint64_t *field, uint64_t value) {
	uint64_t old = __atomic_load_n(field, __ATOMIC_RELAXED);

	while (value > old) {
		if (__atomic_compare_exchange_n(field, &old, value, 0,
		                                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
			break;
		}
	}
}

/* ── Worker thread ──────────────────────────────────────────────────────── */

static void *worker_thread_main(void *arg) {
	(void)arg;

	for (;;) {
		worker_task_node_t *node;
		kvs_command_task_t *task;
		int submit_ret;
		uint64_t dequeue_us;

		pthread_mutex_lock(&g_pool.mu);
		while (!g_pool.head && !g_pool.stopping) {
			pthread_cond_wait(&g_pool.cv, &g_pool.mu);
		}
		if (!g_pool.head && g_pool.stopping) {
			pthread_mutex_unlock(&g_pool.mu);
			break;
		}
		node = g_pool.head;
		g_pool.head = node->next;
		if (!g_pool.head) {
			g_pool.tail = NULL;
		}
		pool_stats_add(&g_pool.stats.prepare_queue_depth,
		               (uint64_t)-1);
		pthread_mutex_unlock(&g_pool.mu);
		dequeue_us = pool_now_us();

		task = node->task;
		kvs_free(node);

		if (!task) {
			pool_stats_add(&g_pool.stats.prepare_dispatch_failed, 1);
			continue;
		}

		/* Record queue wait time */
		if (task->prepared_enqueue_us > 0 && dequeue_us >= task->prepared_enqueue_us) {
			uint64_t wait_us = dequeue_us - task->prepared_enqueue_us;

			pool_stats_add(&g_pool.stats.prepare_queue_wait_count, 1);
			pool_stats_add(&g_pool.stats.prepare_queue_wait_total_us, wait_us);
			pool_stats_update_max(&g_pool.stats.prepare_queue_wait_max_us, wait_us);
		}

		/* Execute prepare */
		{
			uint64_t prep_start = pool_now_us();
			int prep_ret;

			prep_ret = kvs_command_task_prepare(task);

			if (prep_ret == 0) {
				uint64_t prep_end = pool_now_us();
				uint64_t prep_us = prep_end >= prep_start ? prep_end - prep_start : 0;

				pool_stats_add(&g_pool.stats.prepare_count, 1);
				pool_stats_add(&g_pool.stats.prepare_total_us, prep_us);
				pool_stats_update_max(&g_pool.stats.prepare_max_us, prep_us);
				pool_stats_add(&g_pool.stats.completed_prepare_tasks, 1);

				/* Submit prepared task back to executor */
				{
					uint64_t submit_start = pool_now_us();
					submit_ret = kvs_command_executor_submit_prepared(task);
					if (submit_ret == 0) {
						uint64_t submit_end = pool_now_us();
						uint64_t submit_us = submit_end >= submit_start ?
							submit_end - submit_start : 0;

						pool_stats_add(&g_pool.stats.prepared_queue_submit_count, 1);
						pool_stats_add(&g_pool.stats.prepared_queue_submit_total_us, submit_us);
						pool_stats_update_max(&g_pool.stats.prepared_queue_submit_max_us, submit_us);
					} else {
						pool_stats_add(&g_pool.stats.prepared_queue_submit_failed, 1);
						pool_stats_add(&g_pool.stats.failed_prepare_tasks, 1);
					}
				}
			} else {
				pool_stats_add(&g_pool.stats.failed_prepare_tasks, 1);
			}
		}
	}

	return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int kvs_embedding_worker_pool_init(int num_threads, int queue_capacity) {
	(void)num_threads;
	(void)queue_capacity;
	return 0;
}

int kvs_embedding_worker_pool_start(int num_threads, int queue_capacity) {
	int i;

	if (num_threads <= 0) {
		KVS_LOG_WARN("embedding_worker_pool",
		             "invalid num_threads=%d, using 1", num_threads);
		num_threads = 1;
	}
	if (queue_capacity <= 0) {
		queue_capacity = 256;
	}

	pthread_mutex_lock(&g_pool.mu);
	if (g_pool.running) {
		pthread_mutex_unlock(&g_pool.mu);
		return 0;
	}
	g_pool.running = 1;
	g_pool.stopping = 0;
	g_pool.enabled = 1;
	g_pool.num_threads = num_threads;
	g_pool.queue_capacity = queue_capacity;
	pthread_mutex_unlock(&g_pool.mu);

	g_pool.threads = (pthread_t *)kvs_malloc(
		(size_t)num_threads * sizeof(pthread_t));
	if (!g_pool.threads) {
		KVS_LOG_ERROR("embedding_worker_pool",
		              "failed to allocate thread array");
		pthread_mutex_lock(&g_pool.mu);
		g_pool.running = 0;
		g_pool.enabled = 0;
		pthread_mutex_unlock(&g_pool.mu);
		return -1;
	}
	memset(g_pool.threads, 0, (size_t)num_threads * sizeof(pthread_t));

	for (i = 0; i < num_threads; ++i) {
		if (pthread_create(&g_pool.threads[i], NULL,
		                   worker_thread_main, NULL) != 0) {
			KVS_LOG_ERROR("embedding_worker_pool",
			              "failed to create worker thread %d", i);
			g_pool.stopping = 1;
			pthread_cond_broadcast(&g_pool.cv);
			while (--i >= 0) {
				pthread_join(g_pool.threads[i], NULL);
			}
			kvs_free(g_pool.threads);
			g_pool.threads = NULL;
			pthread_mutex_lock(&g_pool.mu);
			g_pool.running = 0;
			g_pool.enabled = 0;
			pthread_mutex_unlock(&g_pool.mu);
			return -1;
		}
	}

	pool_stats_add(&g_pool.stats.worker_threads, (uint64_t)num_threads);
	KVS_LOG_INFO("embedding_worker_pool",
	             "started with %d threads, queue_capacity=%d",
	             num_threads, queue_capacity);
	return 0;
}

void kvs_embedding_worker_pool_stop(void) {
	int i;

	pthread_mutex_lock(&g_pool.mu);
	if (!g_pool.running) {
		pthread_mutex_unlock(&g_pool.mu);
		return;
	}
	g_pool.stopping = 1;
	pthread_cond_broadcast(&g_pool.cv);
	pthread_mutex_unlock(&g_pool.mu);

	if (g_pool.threads) {
		for (i = 0; i < g_pool.num_threads; ++i) {
			pthread_join(g_pool.threads[i], NULL);
		}
		kvs_free(g_pool.threads);
		g_pool.threads = NULL;
	}

	pthread_mutex_lock(&g_pool.mu);
	g_pool.running = 0;
	g_pool.enabled = 0;
	g_pool.num_threads = 0;
	pthread_mutex_unlock(&g_pool.mu);
}

void kvs_embedding_worker_pool_destroy(void) {
	kvs_embedding_worker_pool_stop();

	pthread_mutex_lock(&g_pool.mu);
	while (g_pool.head) {
		worker_task_node_t *node = g_pool.head;
		g_pool.head = node->next;
		if (node->task) {
			kvs_command_task_destroy(node->task);
			kvs_free(node->task);
		}
		kvs_free(node);
	}
	g_pool.tail = NULL;
	pthread_mutex_unlock(&g_pool.mu);
}

int kvs_embedding_worker_pool_submit(kvs_command_task_t *task) {
	worker_task_node_t *node;
	uint64_t depth;

	if (!task) {
		pool_stats_add(&g_pool.stats.submit_failed, 1);
		return -1;
	}

	node = (worker_task_node_t *)kvs_malloc(sizeof(*node));
	if (!node) {
		pool_stats_add(&g_pool.stats.submit_failed, 1);
		return -1;
	}
	memset(node, 0, sizeof(*node));
	node->task = task;
	task->prepared_enqueue_us = pool_now_us();

	pthread_mutex_lock(&g_pool.mu);
	if (!g_pool.running) {
		pthread_mutex_unlock(&g_pool.mu);
		kvs_free(node);
		pool_stats_add(&g_pool.stats.submit_failed, 1);
		return -1;
	}
	if (g_pool.tail) {
		g_pool.tail->next = node;
	} else {
		g_pool.head = node;
	}
	g_pool.tail = node;
	depth = (uint64_t)-1;
	pool_stats_add(&g_pool.stats.prepare_queue_depth, 1);
	depth = (uint64_t)-1;
	{
		/* Count queue entries for max depth */
		worker_task_node_t *cur = g_pool.head;
		uint64_t count = 0;
		while (cur) { count++; cur = cur->next; }
		pool_stats_update_max(&g_pool.stats.prepare_queue_max_depth, count);
	}
	(void)depth;
	pool_stats_add(&g_pool.stats.submitted_prepare_tasks, 1);
	pthread_cond_signal(&g_pool.cv);
	pthread_mutex_unlock(&g_pool.mu);
	return 0;
}

int kvs_embedding_worker_pool_enabled(void) {
	return g_pool.enabled;
}

void kvs_embedding_worker_pool_get_stats(
    kvs_embedding_worker_pool_stats_t *out) {
	if (!out) {
		return;
	}
	memcpy(out, &g_pool.stats, sizeof(*out));
	out->enabled = (uint64_t)g_pool.enabled;
	out->worker_threads = (uint64_t)g_pool.num_threads;
}
