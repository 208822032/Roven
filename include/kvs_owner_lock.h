#ifndef KVS_OWNER_LOCK_H
#define KVS_OWNER_LOCK_H

#include <pthread.h>

#ifndef KVS_COMMAND_OWNER_ASSERTIONS
#define KVS_COMMAND_OWNER_ASSERTIONS 0
#endif

typedef enum kvs_command_owner_phase_e {
	KVS_COMMAND_OWNER_PHASE_UNREGISTERED = 0,
	KVS_COMMAND_OWNER_PHASE_STARTUP = 1,
	KVS_COMMAND_OWNER_PHASE_RUNTIME = 2,
	KVS_COMMAND_OWNER_PHASE_SHUTDOWN_DRAINED = 3,
} kvs_command_owner_phase_t;

typedef struct kvs_owner_lock_stats_s {
	unsigned long long total_calls;
	unsigned long long rdlock_calls;
	unsigned long long wrlock_calls;
	unsigned long long unlock_calls;
	unsigned long long executor_thread_hits;
	unsigned long long non_executor_thread_calls;
	unsigned long long strict_assertion_failures;
	unsigned long long weak_noop_calls;
	unsigned long long weak_fallback_calls;
} kvs_owner_lock_stats_t;

void kvs_owner_lock_set_executor_thread(void);
int kvs_owner_lock_is_executor_thread(void);

void kvs_command_owner_register_startup_thread(void);
void kvs_command_owner_register_shutdown_thread_after_drain(void);
int kvs_command_owner_is_current_thread(void);
kvs_command_owner_phase_t kvs_command_owner_phase(void);
int kvs_command_owner_mutation_allowed(void);
void kvs_command_owner_assert_mutation(const char *site);

void kvs_command_owner_register_legacy_async_rebuild_thread(void);
void kvs_command_owner_unregister_legacy_async_rebuild_thread(void);
int kvs_command_owner_is_legacy_async_rebuild_thread(void);
void kvs_command_owner_assert_legacy_async_rebuild(const char *site);

int kvs_owner_lock_weak_mode_enabled(void);
void kvs_owner_lock_set_weak_mode(int enabled);
void kvs_owner_lock_get_stats(kvs_owner_lock_stats_t *stats);

int kvs_owner_rwlock_rdlock(pthread_rwlock_t *lock, const char *site);
int kvs_owner_rwlock_wrlock(pthread_rwlock_t *lock, const char *site);
int kvs_owner_rwlock_unlock(pthread_rwlock_t *lock, const char *site);

#if KVS_COMMAND_OWNER_ASSERTIONS
#define KVS_COMMAND_OWNER_ASSERT_MUTATION(site) \
	kvs_command_owner_assert_mutation(site)
#define KVS_COMMAND_OWNER_ASSERT_LEGACY_ASYNC_REBUILD(site) \
	kvs_command_owner_assert_legacy_async_rebuild(site)
#else
#define KVS_COMMAND_OWNER_ASSERT_MUTATION(site) ((void)0)
#define KVS_COMMAND_OWNER_ASSERT_LEGACY_ASYNC_REBUILD(site) ((void)0)
#endif

#endif /* KVS_OWNER_LOCK_H */
