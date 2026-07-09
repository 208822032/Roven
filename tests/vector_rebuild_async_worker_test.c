#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_types.h"
#include "kvs_vector.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern kvs_config_t g_cfg;

#define DIM 64

static int expect_int(int cond, const char *label) {
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", label);
		return 1;
	}
	return 0;
}

static int path_exists(const char *path) {
	return access(path, F_OK) == 0;
}

static void cleanup_snapshot_files(void) {
	(void)unlink("vector_index.manifest");
	(void)unlink("vector_index.manifest.tmp");
	(void)unlink("vector_index.manifest.bak");
	(void)unlink("vector_index.snapshot");
	(void)unlink("vector_index.snapshot.tmp");
	(void)unlink("vector_index.snapshot.bak");
	(void)rmdir("vector_index.snapshot.tmp");
}

static int expect_snapshot_write_status(
	const char *label,
	int attempted,
	int written
) {
	kvs_vector_snapshot_status_t status;
	char check_label[128];
	int failed = 0;

	memset(&status, 0, sizeof(status));
	failed |= expect_int(
		kvs_vector_index_snapshot_status_copy(&status) == KVS_VECTOR_OK,
		"snapshot status copy"
	);
	snprintf(check_label, sizeof(check_label), "%s attempted", label);
	failed |= expect_int(status.snapshot_write_attempted == attempted, check_label);
	snprintf(check_label, sizeof(check_label), "%s written", label);
	failed |= expect_int(status.snapshot_written == written, check_label);
	if (attempted) {
		snprintf(check_label, sizeof(check_label), "%s started", label);
		failed |= expect_int(status.last_snapshot_write_started_ms > 0, check_label);
		snprintf(check_label, sizeof(check_label), "%s finished", label);
		failed |= expect_int(status.last_snapshot_write_finished_ms > 0, check_label);
	}
	if (written) {
		snprintf(check_label, sizeof(check_label), "%s bytes", label);
		failed |= expect_int(status.last_snapshot_bytes > 0, check_label);
		snprintf(check_label, sizeof(check_label), "%s checksum", label);
		failed |= expect_int(status.last_snapshot_checksum > 0, check_label);
		snprintf(check_label, sizeof(check_label), "%s write ok", label);
		failed |= expect_int(
			strcmp(status.last_snapshot_write_error, "ok") == 0,
			check_label
		);
	}
	return failed;
}

static int expect_vinfo_snapshot_write_status(
	const char *label,
	int attempted,
	int written
) {
	kvs_vector_info_t info;
	char check_label[128];
	int failed = 0;

	memset(&info, 0, sizeof(info));
	failed |= expect_int(kvs_vector_info(&info) == KVS_VECTOR_OK, "vinfo copy");
	snprintf(check_label, sizeof(check_label), "%s vinfo snapshot supported", label);
	failed |= expect_int(info.snapshot_supported == 1, check_label);
	snprintf(check_label, sizeof(check_label), "%s vinfo attempted", label);
	failed |= expect_int(info.snapshot_write_attempted == attempted, check_label);
	snprintf(check_label, sizeof(check_label), "%s vinfo written", label);
	failed |= expect_int(info.snapshot_written == written, check_label);
	if (written) {
		snprintf(check_label, sizeof(check_label), "%s vinfo bytes", label);
		failed |= expect_int(info.snapshot_last_bytes > 0, check_label);
		snprintf(check_label, sizeof(check_label), "%s vinfo checksum", label);
		failed |= expect_int(info.snapshot_last_checksum > 0, check_label);
	}
	return failed;
}

static void reset_config(void) {
	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	g_cfg.embedding_dim = DIM;
	g_cfg.vector_score_threshold = -1.0f;
	snprintf(
		g_cfg.vector_index_type,
		sizeof(g_cfg.vector_index_type),
		"%s",
		"core_hnsw_v2"
	);
}

static void make_vector(float *vector, int hot) {
	int i;

	for (i = 0; i < DIM; i++) {
		vector[i] = 0.0f;
	}
	if (hot >= 0 && hot < DIM) {
		vector[hot] = 1.0f;
	}
}

static int commit_vector(
	const char *question,
	const char *answer,
	const float *vector
) {
	kvs_vector_set_prepared_t prepared;
	int ret;

	kvs_vector_set_prepared_reset(&prepared);
	ret = kvs_vector_set_prepare_with_embedding(
		question,
		strlen(question),
		answer,
		strlen(answer),
		"scopeA",
		strlen("scopeA"),
		vector,
		DIM,
		&prepared
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_set_prepared_destroy(&prepared);
		return ret;
	}

	ret = kvs_vector_set_commit_prepared(&prepared);
	kvs_vector_set_prepared_destroy(&prepared);
	return ret;
}

static int product_search_top1(
	const float *vector,
	const char *expected_question,
	const char *label
) {
	kvs_vector_search_prepared_t prepared;
	kvs_vector_search_result_t result;
	size_t result_count = 0;
	int ret;

	kvs_vector_search_prepared_reset(&prepared);
	kvs_vector_search_result_reset(&result);
	ret = kvs_vector_search_prepare_with_embedding(
		"async-query",
		strlen("async-query"),
		"scopeA",
		strlen("scopeA"),
		vector,
		DIM,
		&prepared
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_search_prepared_destroy(&prepared);
		fprintf(stderr, "FAIL: %s prepare ret=%d\n", label, ret);
		return 1;
	}

	ret = kvs_vector_search_with_prepared(
		&prepared,
		1,
		-1.0f,
		&result,
		1,
		&result_count
	);
	kvs_vector_search_prepared_destroy(&prepared);
	if (ret != KVS_VECTOR_OK || result_count != 1) {
		fprintf(stderr, "FAIL: %s search ret=%d count=%zu\n", label, ret, result_count);
		return 1;
	}

	return expect_int(
		result.question_len == strlen(expected_question) &&
			memcmp(result.question, expected_question, result.question_len) == 0,
		label
	);
}

static int seed_records(size_t count) {
	size_t i;

	for (i = 0; i < count; i++) {
		char question[64];
		char answer[64];
		float vector[DIM];

		snprintf(question, sizeof(question), "async-q-%03zu", i);
		snprintf(answer, sizeof(answer), "async-a-%03zu", i);
		make_vector(vector, (int)(i % DIM));
		if (commit_vector(question, answer, vector) != KVS_VECTOR_OK) {
			return KVS_VECTOR_ERR;
		}
	}
	return KVS_VECTOR_OK;
}

static int run_success_and_retry(void) {
	kvs_vector_rebuild_async_status_t status;
	float vector[DIM];
	int failed = 0;

	failed |= expect_int(seed_records(24) == KVS_VECTOR_OK, "seed success records");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK, "sync baseline rebuild");
	cleanup_snapshot_files();
	memset(&g_vector_state.snapshot_status, 0, sizeof(g_vector_state.snapshot_status));

	failed |= expect_int(
		kvs_vector_index_rebuild_async_start() == KVS_VECTOR_OK,
		"async start succeeds"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild_async_wait_for_test(10000) == KVS_VECTOR_OK,
		"async rebuild finishes successfully"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild_async_status(&status) == KVS_VECTOR_OK,
		"async status after success"
	);
	failed |= expect_int(status.worker_running == 0, "worker stopped after success");
	failed |= expect_int(status.last_status == 1, "last status success");
	failed |= expect_int(
		status.rebuild_state && strcmp(status.rebuild_state, "idle") == 0,
		"rebuild state idle after success"
	);
	failed |= expect_snapshot_write_status("async publish snapshot", 1, 1);
	failed |= expect_vinfo_snapshot_write_status("async publish snapshot", 1, 1);
	failed |= expect_int(path_exists("vector_index.manifest"), "async manifest exists");
	failed |= expect_int(path_exists("vector_index.snapshot"), "async snapshot exists");

	make_vector(vector, 7);
	failed |= product_search_top1(vector, "async-q-007", "product search after async publish");

	failed |= expect_int(
		kvs_vector_index_rebuild_async_retry() == KVS_VECTOR_OK,
		"async retry from idle starts"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild_async_wait_for_test(10000) == KVS_VECTOR_OK,
		"async retry finishes successfully"
	);
	return failed;
}

static int run_snapshot_write_failure_does_not_fail_async_publish(void) {
	kvs_vector_rebuild_async_status_t status;
	float vector[DIM];
	int failed = 0;

	cleanup_snapshot_files();
	memset(&g_vector_state.snapshot_status, 0, sizeof(g_vector_state.snapshot_status));
	failed |= expect_int(
		mkdir("vector_index.snapshot.tmp", 0500) == 0,
		"create blocking snapshot tmp dir"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild_async_start() == KVS_VECTOR_OK,
		"async start with blocked snapshot tmp"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild_async_wait_for_test(10000) == KVS_VECTOR_OK,
		"async rebuild succeeds despite snapshot write failure"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild_async_status(&status) == KVS_VECTOR_OK,
		"async status after snapshot write failure"
	);
	failed |= expect_int(status.worker_running == 0, "worker stopped after write failure");
	failed |= expect_int(status.last_status == 1, "last status success after write failure");
	failed |= expect_snapshot_write_status("async snapshot failure", 1, 0);
	failed |= expect_vinfo_snapshot_write_status("async snapshot failure", 1, 0);
	failed |= expect_int(
		path_exists("vector_index.snapshot.tmp"),
		"blocking tmp dir remains after failed write"
	);

	make_vector(vector, 7);
	failed |= product_search_top1(
		vector,
		"async-q-007",
		"product search after async snapshot write failure"
	);

	(void)rmdir("vector_index.snapshot.tmp");
	cleanup_snapshot_files();
	return failed;
}

static int run_deterministic_cancel_and_retry(void) {
	kvs_vector_rebuild_async_status_t status;
	int failed = 0;

	failed |= expect_int(
		pthread_rwlock_wrlock(&g_vector_state.rebuild_worker_rwlock) == 0,
		"test acquires rebuild worker write lock"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild_async_start() == KVS_VECTOR_OK,
		"async start while worker blocked on data read lock"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild_async_cancel() == KVS_VECTOR_OK,
		"async cancel request succeeds"
	);
	(void)pthread_rwlock_unlock(&g_vector_state.rebuild_worker_rwlock);

	failed |= expect_int(
		kvs_vector_index_rebuild_async_wait_for_test(10000) != KVS_VECTOR_OK,
		"cancelled async rebuild does not report success"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild_async_status(&status) == KVS_VECTOR_OK,
		"async status after cancel"
	);
	failed |= expect_int(status.worker_running == 0, "worker stopped after cancel");
	failed |= expect_int(status.last_status == -1, "last status failure after cancel");
	failed |= expect_int(
		status.rebuild_state && strcmp(status.rebuild_state, "cancelled") == 0,
		"rebuild state cancelled"
	);

	failed |= expect_int(
		kvs_vector_index_rebuild_async_retry() == KVS_VECTOR_OK,
		"async retry after cancel starts"
	);
	failed |= expect_int(
		kvs_vector_index_rebuild_async_wait_for_test(10000) == KVS_VECTOR_OK,
		"async retry after cancel succeeds"
	);
	return failed;
}

int main(void) {
	char cwd[1024];
	char tmp_template[] = "/tmp/kvlite_hnsw_r7_async_XXXXXX";
	char *tmp_dir;
	int failed = 0;

	if (!getcwd(cwd, sizeof(cwd))) {
		fprintf(stderr, "FAIL: getcwd\n");
		return 1;
	}
	tmp_dir = mkdtemp(tmp_template);
	if (!tmp_dir || chdir(tmp_dir) != 0) {
		fprintf(stderr, "FAIL: temp dir setup\n");
		return 1;
	}

	reset_config();
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	mem_init();

	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "vector init");
	failed |= run_success_and_retry();
	failed |= run_snapshot_write_failure_does_not_fail_async_publish();
	failed |= run_deterministic_cancel_and_retry();
	kvs_vector_destroy();
	cleanup_snapshot_files();

	if (failed) {
		fprintf(stderr, "vector_rebuild_async_worker_test FAILED\n");
		return 1;
	}

	if (chdir(cwd) != 0) {
		fprintf(stderr, "FAIL: restore cwd\n");
		return 1;
	}
	(void)rmdir(tmp_dir);

	printf("vector_rebuild_async_worker_test PASS\n");
	return 0;
}
