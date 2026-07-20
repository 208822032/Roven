#include "kvs_log.h"
#include "kvs_owner_lock.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void kvs_log_write(
	kvs_log_level_t level,
	const char *module,
	const char *file,
	int line,
	const char *fmt,
	...
) {
	(void)level;
	(void)module;
	(void)file;
	(void)line;
	(void)fmt;
}

void kvs_log_write_errno(
	kvs_log_level_t level,
	const char *module,
	const char *file,
	int line,
	const char *fmt,
	...
) {
	(void)level;
	(void)module;
	(void)file;
	(void)line;
	(void)fmt;
}

static void *non_owner_mutation(void *arg) {
	(void)arg;
	KVS_COMMAND_OWNER_ASSERT_MUTATION("test.non_owner_runtime");
	return NULL;
}

static void *legacy_rebuild_allowed(void *arg) {
	(void)arg;
	kvs_command_owner_register_legacy_async_rebuild_thread();
	KVS_COMMAND_OWNER_ASSERT_LEGACY_ASYNC_REBUILD("test.legacy_allowed");
	kvs_command_owner_unregister_legacy_async_rebuild_thread();
	return NULL;
}

static void *legacy_rebuild_mutation_denied(void *arg) {
	(void)arg;
	kvs_command_owner_register_legacy_async_rebuild_thread();
	KVS_COMMAND_OWNER_ASSERT_LEGACY_ASYNC_REBUILD("test.legacy_registered");
	KVS_COMMAND_OWNER_ASSERT_MUTATION("test.legacy_runtime_mutation");
	return NULL;
}

static int run_thread(void *(*fn)(void *)) {
	pthread_t thread;

	if (pthread_create(&thread, NULL, fn, NULL) != 0) {
		return 1;
	}
	if (pthread_join(thread, NULL) != 0) {
		return 1;
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s MODE\n", argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "startup_ok") == 0) {
		kvs_command_owner_register_startup_thread();
		KVS_COMMAND_OWNER_ASSERT_MUTATION("test.startup");
		return kvs_command_owner_phase() == KVS_COMMAND_OWNER_PHASE_STARTUP ? 0 : 1;
	}
	if (strcmp(argv[1], "runtime_ok") == 0) {
		kvs_owner_lock_set_executor_thread();
		KVS_COMMAND_OWNER_ASSERT_MUTATION("test.runtime");
		return kvs_command_owner_is_current_thread() ? 0 : 1;
	}
	if (strcmp(argv[1], "shutdown_ok") == 0) {
		kvs_owner_lock_set_executor_thread();
		kvs_command_owner_register_shutdown_thread_after_drain();
		KVS_COMMAND_OWNER_ASSERT_MUTATION("test.shutdown");
		return kvs_command_owner_phase() ==
			KVS_COMMAND_OWNER_PHASE_SHUTDOWN_DRAINED ? 0 : 1;
	}
	if (strcmp(argv[1], "non_owner_runtime") == 0) {
		kvs_owner_lock_set_executor_thread();
		return run_thread(non_owner_mutation);
	}
	if (strcmp(argv[1], "legacy_ok") == 0) {
		kvs_owner_lock_set_executor_thread();
		return run_thread(legacy_rebuild_allowed);
	}
	if (strcmp(argv[1], "legacy_mutation_denied") == 0) {
		kvs_owner_lock_set_executor_thread();
		return run_thread(legacy_rebuild_mutation_denied);
	}
	if (strcmp(argv[1], "unregistered") == 0) {
		KVS_COMMAND_OWNER_ASSERT_MUTATION("test.unregistered");
		return 0;
	}

	fprintf(stderr, "unknown mode: %s\n", argv[1]);
	return 2;
}
