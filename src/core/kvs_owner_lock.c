#include "kvs_owner_lock.h"

#include "kvs_log.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static pthread_t g_owner_executor_thread;
static int g_owner_executor_thread_set = 0;
static int g_owner_weak_mode = -1;
static __thread int g_owner_weak_noop_depth = 0;
static pthread_mutex_t g_owner_lock_stats_mu = PTHREAD_MUTEX_INITIALIZER;
static kvs_owner_lock_stats_t g_owner_lock_stats;

static int kvs_owner_lock_read_weak_mode(void) {
	const char *value;

	if (g_owner_weak_mode >= 0) {
		return g_owner_weak_mode;
	}
	value = getenv("KVLITE_ROUTE_C_WEAK_RWLOCK");
	g_owner_weak_mode = value &&
		(strcmp(value, "1") == 0 ||
		 strcasecmp(value, "true") == 0 ||
		 strcasecmp(value, "yes") == 0 ||
		 strcasecmp(value, "on") == 0);
	return g_owner_weak_mode;
}

static void kvs_owner_lock_record_call(int op, int is_executor, int no_op, int fallback) {
	pthread_mutex_lock(&g_owner_lock_stats_mu);
	g_owner_lock_stats.total_calls++;
	if (op == 1) {
		g_owner_lock_stats.rdlock_calls++;
	} else if (op == 2) {
		g_owner_lock_stats.wrlock_calls++;
	} else if (op == 3) {
		g_owner_lock_stats.unlock_calls++;
	}
	if (is_executor) {
		g_owner_lock_stats.executor_thread_hits++;
	} else {
		g_owner_lock_stats.non_executor_thread_calls++;
	}
	if (no_op) {
		g_owner_lock_stats.weak_noop_calls++;
	}
	if (fallback) {
		g_owner_lock_stats.weak_fallback_calls++;
		g_owner_lock_stats.strict_assertion_failures++;
	}
	pthread_mutex_unlock(&g_owner_lock_stats_mu);
}

void kvs_owner_lock_set_executor_thread(void) {
	g_owner_executor_thread = pthread_self();
	g_owner_executor_thread_set = 1;
}

int kvs_owner_lock_is_executor_thread(void) {
	return g_owner_executor_thread_set &&
	       pthread_equal(g_owner_executor_thread, pthread_self());
}

int kvs_owner_lock_weak_mode_enabled(void) {
	return kvs_owner_lock_read_weak_mode();
}

void kvs_owner_lock_set_weak_mode(int enabled) {
	g_owner_weak_mode = enabled ? 1 : 0;
}

void kvs_owner_lock_get_stats(kvs_owner_lock_stats_t *stats) {
	if (!stats) {
		return;
	}
	pthread_mutex_lock(&g_owner_lock_stats_mu);
	*stats = g_owner_lock_stats;
	pthread_mutex_unlock(&g_owner_lock_stats_mu);
}

int kvs_owner_rwlock_rdlock(pthread_rwlock_t *lock, const char *site) {
	int is_executor = kvs_owner_lock_is_executor_thread();
	if (kvs_owner_lock_read_weak_mode()) {
		if (is_executor) {
			g_owner_weak_noop_depth++;
			kvs_owner_lock_record_call(1, is_executor, 1, 0);
			return 0;
		}
		kvs_owner_lock_record_call(1, is_executor, 0, 1);
		KVS_LOG_WARN("owner_lock",
			"weak rwlock rdlock fallback on non-executor thread, site=%s",
			site ? site : "(null)");
		return pthread_rwlock_rdlock(lock);
	}
	kvs_owner_lock_record_call(1, is_executor, 0, 0);
	return pthread_rwlock_rdlock(lock);
}

int kvs_owner_rwlock_wrlock(pthread_rwlock_t *lock, const char *site) {
	int is_executor = kvs_owner_lock_is_executor_thread();
	if (kvs_owner_lock_read_weak_mode()) {
		if (is_executor) {
			g_owner_weak_noop_depth++;
			kvs_owner_lock_record_call(2, is_executor, 1, 0);
			return 0;
		}
		kvs_owner_lock_record_call(2, is_executor, 0, 1);
		KVS_LOG_WARN("owner_lock",
			"weak rwlock wrlock fallback on non-executor thread, site=%s",
			site ? site : "(null)");
		return pthread_rwlock_wrlock(lock);
	}
	kvs_owner_lock_record_call(2, is_executor, 0, 0);
	return pthread_rwlock_wrlock(lock);
}

int kvs_owner_rwlock_unlock(pthread_rwlock_t *lock, const char *site) {
	int is_executor = kvs_owner_lock_is_executor_thread();
	if (g_owner_weak_noop_depth > 0) {
		g_owner_weak_noop_depth--;
		kvs_owner_lock_record_call(3, is_executor, 1, 0);
		return 0;
	}
	if (kvs_owner_lock_read_weak_mode()) {
		kvs_owner_lock_record_call(3, is_executor, 0, 1);
		KVS_LOG_WARN("owner_lock",
			"weak rwlock unlock fallback on non-executor thread, site=%s",
			site ? site : "(null)");
		return pthread_rwlock_unlock(lock);
	}
	kvs_owner_lock_record_call(3, is_executor, 0, 0);
	return pthread_rwlock_unlock(lock);
}
