#include "kvs_owner_lock.h"

#include "kvs_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static pthread_t g_owner_executor_thread;
static int g_owner_executor_thread_set = 0;
static pthread_t g_owner_startup_thread;
static int g_owner_startup_thread_set = 0;
static pthread_t g_owner_shutdown_thread;
static int g_owner_shutdown_thread_set = 0;
static int g_command_owner_phase = KVS_COMMAND_OWNER_PHASE_UNREGISTERED;
static int g_owner_weak_mode = -1;
static __thread int g_owner_weak_noop_depth = 0;
static __thread int g_legacy_async_rebuild_thread = 0;
static pthread_mutex_t g_owner_lock_stats_mu = PTHREAD_MUTEX_INITIALIZER;
static kvs_owner_lock_stats_t g_owner_lock_stats;

#if KVS_COMMAND_OWNER_ASSERTIONS
static void kvs_command_owner_fail(const char *kind, const char *site) {
	fprintf(stderr,
		"command owner assertion failed: kind=%s site=%s phase=%d\n",
		kind ? kind : "unknown",
		site ? site : "(null)",
		__atomic_load_n(&g_command_owner_phase, __ATOMIC_ACQUIRE));
	fflush(stderr);
	abort();
}
#endif

static int kvs_owner_lock_read_weak_mode(void) {
	const char *value;

	if (g_owner_weak_mode >= 0) {
		return g_owner_weak_mode;
	}
	value = getenv("KVLITE_ROUTE_C_WEAK_RWLOCK");
	if (value) {
		g_owner_weak_mode =
			(strcmp(value, "1") == 0 ||
			 strcasecmp(value, "true") == 0 ||
			 strcasecmp(value, "yes") == 0 ||
			 strcasecmp(value, "on") == 0);
	} else {
		/* Stage 13: weak-lock is now the default.
		 * Set KVLITE_ROUTE_C_WEAK_RWLOCK=0 to roll back to real-lock mode. */
		g_owner_weak_mode = 1;
	}
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
	__atomic_store_n(&g_owner_executor_thread_set, 1, __ATOMIC_RELEASE);
	__atomic_store_n(
		&g_command_owner_phase,
		KVS_COMMAND_OWNER_PHASE_RUNTIME,
		__ATOMIC_RELEASE
	);
}

int kvs_owner_lock_is_executor_thread(void) {
	return __atomic_load_n(&g_owner_executor_thread_set, __ATOMIC_ACQUIRE) &&
	       pthread_equal(g_owner_executor_thread, pthread_self());
}

void kvs_command_owner_register_startup_thread(void) {
	g_owner_startup_thread = pthread_self();
	__atomic_store_n(&g_owner_startup_thread_set, 1, __ATOMIC_RELEASE);
	__atomic_store_n(
		&g_command_owner_phase,
		KVS_COMMAND_OWNER_PHASE_STARTUP,
		__ATOMIC_RELEASE
	);
}

void kvs_command_owner_register_shutdown_thread_after_drain(void) {
	g_owner_shutdown_thread = pthread_self();
	__atomic_store_n(&g_owner_shutdown_thread_set, 1, __ATOMIC_RELEASE);
	__atomic_store_n(
		&g_command_owner_phase,
		KVS_COMMAND_OWNER_PHASE_SHUTDOWN_DRAINED,
		__ATOMIC_RELEASE
	);
}

int kvs_command_owner_is_current_thread(void) {
	return kvs_owner_lock_is_executor_thread();
}

kvs_command_owner_phase_t kvs_command_owner_phase(void) {
	return (kvs_command_owner_phase_t)__atomic_load_n(
		&g_command_owner_phase,
		__ATOMIC_ACQUIRE
	);
}

int kvs_command_owner_mutation_allowed(void) {
	kvs_command_owner_phase_t phase = kvs_command_owner_phase();

	if (phase == KVS_COMMAND_OWNER_PHASE_RUNTIME) {
		return kvs_command_owner_is_current_thread();
	}
	if (phase == KVS_COMMAND_OWNER_PHASE_STARTUP) {
		return __atomic_load_n(&g_owner_startup_thread_set, __ATOMIC_ACQUIRE) &&
		       pthread_equal(g_owner_startup_thread, pthread_self());
	}
	if (phase == KVS_COMMAND_OWNER_PHASE_SHUTDOWN_DRAINED) {
		return __atomic_load_n(&g_owner_shutdown_thread_set, __ATOMIC_ACQUIRE) &&
		       pthread_equal(g_owner_shutdown_thread, pthread_self());
	}
	return 0;
}

void kvs_command_owner_assert_mutation(const char *site) {
#if KVS_COMMAND_OWNER_ASSERTIONS
	if (!kvs_command_owner_mutation_allowed()) {
		kvs_command_owner_fail("runtime_mutation", site);
	}
#else
	(void)site;
#endif
}

void kvs_command_owner_register_legacy_async_rebuild_thread(void) {
	g_legacy_async_rebuild_thread = 1;
}

void kvs_command_owner_unregister_legacy_async_rebuild_thread(void) {
	g_legacy_async_rebuild_thread = 0;
}

int kvs_command_owner_is_legacy_async_rebuild_thread(void) {
	return g_legacy_async_rebuild_thread;
}

void kvs_command_owner_assert_legacy_async_rebuild(const char *site) {
#if KVS_COMMAND_OWNER_ASSERTIONS
	if (kvs_command_owner_phase() != KVS_COMMAND_OWNER_PHASE_RUNTIME ||
	    !kvs_command_owner_is_legacy_async_rebuild_thread()) {
		kvs_command_owner_fail("legacy_async_rebuild", site);
	}
#else
	(void)site;
#endif
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
