#include "kvs_command_executor.h"
#include "kvs_globals.h"
#include "distributed.h"

#include <time.h>

static pthread_t g_ttl_scan_thread;
static int g_ttl_scan_running = 0;
static int g_ttl_scan_stop   = 0;

static void *ttl_scan_thread_main(void *arg) {
	(void)arg;

	while (!__atomic_load_n(&g_ttl_scan_stop, __ATOMIC_ACQUIRE)) {
		struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
		nanosleep(&ts, NULL);

		if (__atomic_load_n(&g_ttl_scan_stop, __ATOMIC_ACQUIRE)) {
			break;
		}
		if (global_dist_config.role == ROLE_SLAVE) {
			continue;
		}
		(void)kvs_command_executor_submit_ttl_scan(32);
	}

	return NULL;
}

int kvs_ttl_background_start(void) {
	if (__atomic_load_n(&g_ttl_scan_running, __ATOMIC_ACQUIRE)) {
		return 0;
	}
	__atomic_store_n(&g_ttl_scan_stop, 0, __ATOMIC_RELEASE);
	if (pthread_create(&g_ttl_scan_thread, NULL, ttl_scan_thread_main, NULL) != 0) {
		return -1;
	}
	__atomic_store_n(&g_ttl_scan_running, 1, __ATOMIC_RELEASE);
	return 0;
}

void kvs_ttl_background_stop(void) {
	if (!__atomic_load_n(&g_ttl_scan_running, __ATOMIC_ACQUIRE)) {
		return;
	}
	__atomic_store_n(&g_ttl_scan_stop, 1, __ATOMIC_RELEASE);
	pthread_join(g_ttl_scan_thread, NULL);
	__atomic_store_n(&g_ttl_scan_running, 0, __ATOMIC_RELEASE);
}
