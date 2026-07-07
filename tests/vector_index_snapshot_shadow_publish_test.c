#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"
#include "vector/kvs_vector_rebuild_shadow.h"

#include <stdio.h>
#include <stdlib.h>
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

static void reset_config(const char *index_type) {
	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	g_cfg.embedding_dim = DIM;
	g_cfg.vector_score_threshold = -1.0f;
	snprintf(
		g_cfg.vector_index_type,
		sizeof(g_cfg.vector_index_type),
		"%s",
		index_type ? index_type : "core_hnsw_v2"
	);
}

static void one_hot(float *vector, int hot) {
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
	const char *scope,
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
		scope,
		scope ? strlen(scope) : 0,
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

static void cleanup_snapshot_files(void) {
	(void)unlink("vector_index.manifest");
	(void)unlink("vector_index.manifest.tmp");
	(void)unlink("vector_index.manifest.bak");
	(void)unlink("vector_index.snapshot");
	(void)unlink("vector_index.snapshot.tmp");
	(void)unlink("vector_index.snapshot.bak");
	(void)rmdir("vector_index.manifest.tmp");
	(void)rmdir("vector_index.snapshot.tmp");
}

static int path_exists(const char *path) {
	struct stat st;

	return stat(path, &st) == 0;
}

static uint64_t record_id_for_question(
	const char *question,
	const char *scope
) {
	kvs_vector_store_t *store = &g_vector_state.store;
	size_t question_len = strlen(question);
	size_t scope_len = scope ? strlen(scope) : 0;
	size_t i;

	for (i = 0; i < store->capacity; i++) {
		kvs_vector_record_t *record = &store->records[i];

		if (record->state != KVS_VECTOR_RECORD_ACTIVE || record->deleted) {
			continue;
		}
		if (record->question_len != question_len ||
		    memcmp(record->question, question, question_len) != 0) {
			continue;
		}
		if (record->scope_len != scope_len) {
			continue;
		}
		if (scope_len > 0 &&
		    memcmp(record->scope, scope, scope_len) != 0) {
			continue;
		}
		return record->id;
	}

	return 0;
}

static int product_search_has(
	const float *query,
	const char *scope,
	const char *expected_question
) {
	kvs_vector_search_prepared_t prepared;
	kvs_vector_search_result_t results[4];
	size_t result_count = 0;
	size_t i;
	int ret;

	kvs_vector_search_prepared_reset(&prepared);
	kvs_vector_search_results_reset(results, 4);
	ret = kvs_vector_search_prepare_with_embedding(
		"shadow-publish-query",
		strlen("shadow-publish-query"),
		scope,
		scope ? strlen(scope) : 0,
		query,
		DIM,
		&prepared
	);
	if (ret != KVS_VECTOR_OK) {
		kvs_vector_search_prepared_destroy(&prepared);
		return ret;
	}

	ret = kvs_vector_search_with_prepared(
		&prepared,
		4,
		-1.0f,
		results,
		4,
		&result_count
	);
	kvs_vector_search_prepared_destroy(&prepared);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	for (i = 0; i < result_count; i++) {
		if (results[i].question &&
		    results[i].question_len == strlen(expected_question) &&
		    memcmp(results[i].question,
		           expected_question,
		           results[i].question_len) == 0) {
			return KVS_VECTOR_OK;
		}
	}
	return KVS_VECTOR_ERR_NOT_FOUND;
}

static int rebuild_active_hit(
	const float *query,
	uint64_t expected_id,
	const char *label
) {
	kvs_vector_hit_t hits[4];
	size_t hit_count = 0;
	int ret;

	memset(hits, 0, sizeof(hits));
	ret = kvs_vector_rebuild_shadow_v2_active_search_for_test(
		query,
		DIM,
		4,
		32,
		hits,
		&hit_count
	);
	if (ret != KVS_VECTOR_OK) {
		fprintf(stderr, "FAIL: %s active search ret=%d\n", label, ret);
		return 1;
	}
	return expect_int(hit_count > 0 && hits[0].record_id == expected_id, label);
}

static int prepare_ready_shadow(
	const float *base_vec,
	const float *delta_vec,
	uint64_t *out_delta_id
) {
	int ret;

	ret = commit_vector("s6-base", "base", "scopeS", base_vec);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_rebuild_shadow_v2_init_for_test();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_rebuild_shadow_v2_build_from_active_records_for_test();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = commit_vector("s6-delta", "delta", "scopeS", delta_vec);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	if (out_delta_id) {
		*out_delta_id = record_id_for_question("s6-delta", "scopeS");
	}
	return kvs_vector_rebuild_shadow_v2_replay_deltas_for_test();
}

static int reset_v2_index_only(void) {
	return kvs_hnsw_v2_index_reset(&g_vector_state.core_hnsw_v2_index);
}

static int expect_write_status(
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
		"status copy"
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
		snprintf(check_label, sizeof(check_label), "%s error ok", label);
		failed |= expect_int(
			strcmp(status.last_snapshot_write_error, "ok") == 0,
			check_label
		);
	}
	return failed;
}

static int expect_vinfo_shadow_write_status(void) {
	kvs_vector_info_t info;
	int failed = 0;

	memset(&info, 0, sizeof(info));
	failed |= expect_int(kvs_vector_info(&info) == KVS_VECTOR_OK, "vinfo copy");
	failed |= expect_int(info.snapshot_supported == 1, "vinfo snapshot supported");
	failed |= expect_int(info.snapshot_write_attempted == 1, "vinfo write attempted");
	failed |= expect_int(info.snapshot_written == 1, "vinfo written");
	failed |= expect_int(info.snapshot_last_bytes > 0, "vinfo bytes");
	failed |= expect_int(info.snapshot_last_checksum > 0, "vinfo checksum");
	return failed;
}

static int run_successful_shadow_publish_writes_loadable_snapshot(void) {
	float base_vec[DIM];
	float delta_vec[DIM];
	uint64_t delta_id = 0;
	kvs_vector_snapshot_status_t status;
	int failed = 0;

	cleanup_snapshot_files();
	one_hot(base_vec, 3);
	one_hot(delta_vec, 17);
	reset_config("core_hnsw_v2");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "success init");
	failed |= expect_int(
		prepare_ready_shadow(base_vec, delta_vec, &delta_id) == KVS_VECTOR_OK,
		"success ready"
	);
	cleanup_snapshot_files();
	memset(&g_vector_state.snapshot_status, 0, sizeof(g_vector_state.snapshot_status));

	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() == KVS_VECTOR_OK,
		"success swap"
	);
	failed |= expect_write_status("shadow publish", 1, 1);
	failed |= expect_vinfo_shadow_write_status();
	failed |= expect_int(path_exists("vector_index.manifest"), "manifest exists");
	failed |= expect_int(path_exists("vector_index.snapshot"), "snapshot exists");
	failed |= rebuild_active_hit(delta_vec, delta_id, "promoted active sees delta");
	failed |= expect_int(
		kvs_vector_rebuild_retire_cleanup_for_test() == KVS_VECTOR_OK,
		"retired cleanup"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"destroy promoted active"
	);
	failed |= expect_int(reset_v2_index_only() == KVS_VECTOR_OK, "reset product v2");
	failed |= expect_int(
		kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
		"load written snapshot"
	);
	memset(&status, 0, sizeof(status));
	failed |= expect_int(
		kvs_vector_index_snapshot_status_copy(&status) == KVS_VECTOR_OK,
		"load status copy"
	);
	failed |= expect_int(status.snapshot_loaded == 1, "snapshot loaded");
	failed |= expect_int(status.loaded_from_snapshot == 1, "loaded_from_snapshot");
	failed |= expect_int(
		product_search_has(delta_vec, "scopeS", "s6-delta") == KVS_VECTOR_OK,
		"loaded snapshot search"
	);

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_publish_failure_does_not_write_snapshot(void) {
	float base_vec[DIM];
	float delta_vec[DIM];
	int failed = 0;

	cleanup_snapshot_files();
	one_hot(base_vec, 5);
	one_hot(delta_vec, 19);
	reset_config("core_hnsw_v2");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "failure init");
	failed |= expect_int(
		prepare_ready_shadow(base_vec, delta_vec, NULL) == KVS_VECTOR_OK,
		"failure ready"
	);
	cleanup_snapshot_files();
	memset(&g_vector_state.snapshot_status, 0, sizeof(g_vector_state.snapshot_status));
	failed |= expect_int(
		kvs_vector_rebuild_force_swap_failure_for_test() == KVS_VECTOR_OK,
		"forced publish failure"
	);
	failed |= expect_write_status("publish failure", 0, 0);
	failed |= expect_int(!path_exists("vector_index.manifest"), "failure no manifest");
	failed |= expect_int(!path_exists("vector_index.snapshot"), "failure no snapshot");
	failed |= expect_int(
		product_search_has(base_vec, "scopeS", "s6-base") == KVS_VECTOR_OK,
		"failure product search"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"failure cleanup"
	);

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_snapshot_write_failure_does_not_rollback_publish(void) {
	float base_vec[DIM];
	float delta_vec[DIM];
	uint64_t delta_id = 0;
	int failed = 0;

	cleanup_snapshot_files();
	one_hot(base_vec, 7);
	one_hot(delta_vec, 23);
	reset_config("core_hnsw_v2");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "write fail init");
	failed |= expect_int(
		prepare_ready_shadow(base_vec, delta_vec, &delta_id) == KVS_VECTOR_OK,
		"write fail ready"
	);
	cleanup_snapshot_files();
	memset(&g_vector_state.snapshot_status, 0, sizeof(g_vector_state.snapshot_status));
	failed |= expect_int(
		mkdir("vector_index.snapshot.tmp", 0500) == 0,
		"create blocking tmp dir"
	);
	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() == KVS_VECTOR_OK,
		"write failure swap still succeeds"
	);
	failed |= expect_write_status("write failure", 1, 0);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_active_exists_for_test(),
		"write failure active exists"
	);
	failed |= rebuild_active_hit(delta_vec, delta_id, "write failure active search");
	failed |= expect_int(
		product_search_has(delta_vec, "scopeS", "s6-delta") == KVS_VECTOR_OK,
		"write failure product search"
	);
	(void)rmdir("vector_index.snapshot.tmp");
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"write failure cleanup"
	);

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_non_v2_backend_does_not_write_snapshot(void) {
	float base_vec[DIM];
	float delta_vec[DIM];
	int failed = 0;

	cleanup_snapshot_files();
	one_hot(base_vec, 11);
	one_hot(delta_vec, 29);
	reset_config("core_flat");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "flat init");
	failed |= expect_int(
		prepare_ready_shadow(base_vec, delta_vec, NULL) == KVS_VECTOR_OK,
		"flat ready"
	);
	cleanup_snapshot_files();
	memset(&g_vector_state.snapshot_status, 0, sizeof(g_vector_state.snapshot_status));
	failed |= expect_int(
		kvs_vector_rebuild_swap_ready_shadow_for_test() == KVS_VECTOR_OK,
		"flat swap"
	);
	failed |= expect_write_status("flat shadow", 0, 0);
	failed |= expect_int(!path_exists("vector_index.manifest"), "flat no manifest");
	failed |= expect_int(!path_exists("vector_index.snapshot"), "flat no snapshot");
	failed |= expect_int(
		product_search_has(delta_vec, "scopeS", "s6-delta") == KVS_VECTOR_OK,
		"flat product search"
	);
	failed |= expect_int(
		kvs_vector_rebuild_shadow_v2_destroy_for_test() == KVS_VECTOR_OK,
		"flat cleanup"
	);

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

int main(void) {
	char cwd[1024];
	char tmp_template[] = "/tmp/kvlite_hnsw_s6_XXXXXX";
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

	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	mem_init();

	failed |= run_successful_shadow_publish_writes_loadable_snapshot();
	failed |= run_publish_failure_does_not_write_snapshot();
	failed |= run_snapshot_write_failure_does_not_rollback_publish();
	failed |= run_non_v2_backend_does_not_write_snapshot();

	cleanup_snapshot_files();
	mem_destroy();

	if (chdir(cwd) != 0) {
		fprintf(stderr, "FAIL: restore cwd\n");
		return 1;
	}
	(void)rmdir(tmp_dir);

	if (failed) {
		fprintf(stderr, "vector_index_snapshot_shadow_publish_test FAILED\n");
		return 1;
	}

	printf("vector_index_snapshot_shadow_publish_test PASS\n");
	return 0;
}
