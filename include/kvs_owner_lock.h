#ifndef KVS_OWNER_LOCK_H
#define KVS_OWNER_LOCK_H

#include <pthread.h>

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

int kvs_owner_lock_weak_mode_enabled(void);
void kvs_owner_lock_set_weak_mode(int enabled);
void kvs_owner_lock_get_stats(kvs_owner_lock_stats_t *stats);

int kvs_owner_rwlock_rdlock(pthread_rwlock_t *lock, const char *site);
int kvs_owner_rwlock_wrlock(pthread_rwlock_t *lock, const char *site);
int kvs_owner_rwlock_unlock(pthread_rwlock_t *lock, const char *site);

#endif /* KVS_OWNER_LOCK_H */
