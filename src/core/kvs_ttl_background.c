#include "kvs_command_executor.h"
#include "kvs_globals.h"
#include "distributed.h"

#include <time.h>

static pthread_t g_ttl_scan_thread;
static volatile int g_ttl_scan_running = 0;
static volatile int g_ttl_scan_stop = 0;

static void *ttl_scan_thread_main(void *arg) {
	(void)arg;

	while (!g_ttl_scan_stop) {
		struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
		nanosleep(&ts, NULL);

		if (g_ttl_scan_stop) {
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
	if (g_ttl_scan_running) {
		return 0;
	}
	g_ttl_scan_stop = 0;
	if (pthread_create(&g_ttl_scan_thread, NULL, ttl_scan_thread_main, NULL) != 0) {
		return -1;
	}
	g_ttl_scan_running = 1;
	return 0;
}

void kvs_ttl_background_stop(void) {
	if (!g_ttl_scan_running) {
		return;
	}
	g_ttl_scan_stop = 1;
	pthread_join(g_ttl_scan_thread, NULL);
	g_ttl_scan_running = 0;
}
