#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"
#include "vector/kvs_vector_hnsw_v2_snapshot.h"
#include "vector/kvs_vector_index_snapshot.h"
#include "vector/kvs_vector_manager.h"

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

static int result_has_question(
	const kvs_vector_search_result_t *results,
	size_t count,
	const char *question
) {
	size_t i;
	size_t question_len = strlen(question);

	for (i = 0; i < count; i++) {
		if (results[i].question &&
		    results[i].question_len == question_len &&
		    memcmp(results[i].question, question, question_len) == 0) {
			return 1;
		}
	}
	return 0;
}

static int search_for(
	const char *query,
	const char *scope,
	const float *vector,
	const char *expected_question
) {
	kvs_vector_search_prepared_t prepared;
	kvs_vector_search_result_t results[4];
	size_t result_count = 0;
	int ret;
	int ok;

	kvs_vector_search_prepared_reset(&prepared);
	kvs_vector_search_results_reset(results, 4);
	ret = kvs_vector_search_prepare_with_embedding(
		query,
		strlen(query),
		scope,
		scope ? strlen(scope) : 0,
		vector,
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
	ok = result_has_question(results, result_count, expected_question);
	return ok ? KVS_VECTOR_OK : KVS_VECTOR_ERR_NOT_FOUND;
}

static void cleanup_snapshot_files(void) {
	(void)unlink("vector_index.manifest");
	(void)unlink("vector_index.manifest.tmp");
	(void)unlink("vector_index.snapshot");
	(void)unlink("vector_index.snapshot.tmp");
}

static int write_expected_manifest(void) {
	kvs_vector_index_snapshot_manifest_t manifest;
	char reason[128];
	int ret;

	reason[0] = '\0';
	ret = kvs_vector_index_snapshot_build_expected_manifest_after_records(
		&g_vector_state,
		"vector_index.snapshot",
		&manifest,
		reason,
		sizeof(reason)
	);
	if (ret != KVS_VECTOR_OK) {
		fprintf(stderr, "write_expected_manifest build failed: %s\n", reason);
		return ret;
	}
	manifest.created_at_ms = 1;
	return kvs_vector_index_snapshot_manifest_write_file(
		"vector_index.manifest",
		&manifest
	) == 0 ? KVS_VECTOR_OK : KVS_VECTOR_ERR;
}

static int write_current_snapshot_fixture(void) {
	int ret;

	ret = kvs_hnsw_v2_index_serialize_file(
		&g_vector_state.core_hnsw_v2_index,
		"vector_index.snapshot"
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	return write_expected_manifest();
}

static int overwrite_byte(
	const char *path,
	long offset,
	unsigned char value
) {
	FILE *fp = fopen(path, "r+b");

	if (!fp) {
		return -1;
	}
	if (fseek(fp, offset, SEEK_SET) != 0 || fwrite(&value, 1, 1, fp) != 1) {
		fclose(fp);
		return -1;
	}
	return fclose(fp);
}

static int write_text_file(const char *path, const char *text) {
	FILE *fp = fopen(path, "wb");

	if (!fp) {
		return -1;
	}
	if (fputs(text, fp) < 0) {
		fclose(fp);
		return -1;
	}
	return fclose(fp);
}

static int setup_v2_records(float *vec_a, float *vec_b, float *vec_c) {
	int failed = 0;

	one_hot(vec_a, 0);
	one_hot(vec_b, 1);
	one_hot(vec_c, 2);
	reset_config("core_hnsw_v2");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "v2 init");
	failed |= expect_int(
		commit_vector("snap-a", "answer-a", "scopeS", vec_a) == KVS_VECTOR_OK,
		"commit snap-a"
	);
	failed |= expect_int(
		commit_vector("snap-b", "answer-b", "scopeS", vec_b) == KVS_VECTOR_OK,
		"commit snap-b"
	);
	failed |= expect_int(
		commit_vector("snap-c", "answer-c", "scopeOther", vec_c) ==
			KVS_VECTOR_OK,
		"commit snap-c"
	);
	return failed ? KVS_VECTOR_ERR : KVS_VECTOR_OK;
}

static int run_valid_snapshot_load(void) {
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	kvs_vector_snapshot_status_t status;
	int failed = 0;

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records(vec_a, vec_b, vec_c) == KVS_VECTOR_OK,
	                     "valid setup");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "valid baseline rebuild");
	failed |= expect_int(write_current_snapshot_fixture() == KVS_VECTOR_OK,
	                     "valid fixture write");
	failed |= expect_int(
		kvs_hnsw_v2_index_reset(&g_vector_state.core_hnsw_v2_index) ==
			KVS_VECTOR_OK,
		"valid reset index before load"
	);
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "valid rebuild_after_load");
	memset(&status, 0, sizeof(status));
	failed |= expect_int(kvs_vector_index_snapshot_status_copy(&status) ==
	                     KVS_VECTOR_OK, "valid status copy");
	failed |= expect_int(status.snapshot_load_attempted == 1,
	                     "valid attempted");
	failed |= expect_int(status.snapshot_valid == 1, "valid manifest");
	failed |= expect_int(status.snapshot_loaded == 1, "valid loaded");
	failed |= expect_int(status.loaded_from_snapshot == 1,
	                     "valid loaded_from_snapshot");
	failed |= expect_int(status.fallback_rebuild_count == 0,
	                     "valid no fallback");
	failed |= expect_int(g_vector_state.core_hnsw_v2_index.build_stats.add_count == 0,
	                     "valid no full rebuild add_count");
	failed |= expect_int(
		search_for("snap-b-query", "scopeS", vec_b, "snap-b") ==
			KVS_VECTOR_OK,
		"valid search after snapshot"
	);
	kvs_vector_destroy();
	return failed;
}

static int run_v2_fallback_case(
	const char *label,
	void (*prepare_files)(void),
	int expect_manifest_valid
) {
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	kvs_vector_snapshot_status_t status;
	int failed = 0;
	char check_label[128];

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records(vec_a, vec_b, vec_c) == KVS_VECTOR_OK,
	                     label);
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "fallback baseline rebuild");
	if (prepare_files) {
		prepare_files();
	}
	failed |= expect_int(
		kvs_hnsw_v2_index_reset(&g_vector_state.core_hnsw_v2_index) ==
			KVS_VECTOR_OK,
		"fallback reset index before load"
	);
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "fallback rebuild_after_load");
	memset(&status, 0, sizeof(status));
	failed |= expect_int(kvs_vector_index_snapshot_status_copy(&status) ==
	                     KVS_VECTOR_OK, "fallback status copy");
	snprintf(check_label, sizeof(check_label), "%s attempted", label);
	failed |= expect_int(status.snapshot_load_attempted == 1, check_label);
	snprintf(check_label, sizeof(check_label), "%s manifest valid flag", label);
	failed |= expect_int(status.snapshot_valid == expect_manifest_valid, check_label);
	snprintf(check_label, sizeof(check_label), "%s not loaded", label);
	failed |= expect_int(status.snapshot_loaded == 0, check_label);
	snprintf(check_label, sizeof(check_label), "%s fallback count", label);
	failed |= expect_int(status.fallback_rebuild_count == 1, check_label);
	snprintf(check_label, sizeof(check_label), "%s full rebuild add_count", label);
	failed |= expect_int(
		g_vector_state.core_hnsw_v2_index.build_stats.add_count ==
			g_vector_state.store.active_records,
		check_label
	);
	snprintf(check_label, sizeof(check_label), "%s search", label);
	failed |= expect_int(search_for("snap-b-query", "scopeS", vec_b, "snap-b") ==
	                     KVS_VECTOR_OK, check_label);
	kvs_vector_destroy();
	return failed;
}

static void prepare_param_mismatch(void) {
	kvs_vector_index_snapshot_manifest_t manifest;

	if (write_current_snapshot_fixture() != KVS_VECTOR_OK) {
		return;
	}
	if (kvs_vector_index_snapshot_manifest_read_file(
		    "vector_index.manifest",
		    &manifest
	    ) != 0) {
		return;
	}
	manifest.hnsw_m++;
	(void)kvs_vector_index_snapshot_manifest_write_file(
		"vector_index.manifest",
		&manifest
	);
}

static void prepare_missing_manifest(void) {
	(void)unlink("vector_index.manifest");
	(void)unlink("vector_index.manifest.tmp");
}

static void prepare_checksum_mismatch(void) {
	if (write_current_snapshot_fixture() != KVS_VECTOR_OK) {
		return;
	}
	(void)overwrite_byte("vector_index.snapshot", 8, 0xffu);
}

static void prepare_corrupted_snapshot(void) {
	if (write_current_snapshot_fixture() != KVS_VECTOR_OK) {
		return;
	}
	(void)overwrite_byte("vector_index.snapshot", 0, 'X');
	(void)write_expected_manifest();
}

static void prepare_deserialize_failure(void) {
	if (write_current_snapshot_fixture() != KVS_VECTOR_OK) {
		return;
	}
	if (truncate("vector_index.snapshot", 12) != 0) {
		return;
	}
	(void)write_expected_manifest();
}

static void prepare_text_snapshot_failure(void) {
	if (write_text_file("vector_index.snapshot", "not-a-hnsw-snapshot\n") != 0) {
		return;
	}
	(void)write_expected_manifest();
}

static int run_non_v2_backend(void) {
	float vec_a[DIM];
	float vec_b[DIM];
	kvs_vector_snapshot_status_t status;
	int failed = 0;

	cleanup_snapshot_files();
	one_hot(vec_a, 0);
	one_hot(vec_b, 1);
	reset_config("core_flat");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "flat init");
	failed |= expect_int(
		commit_vector("flat-a", "answer-a", "scopeS", vec_a) == KVS_VECTOR_OK,
		"flat commit a"
	);
	failed |= expect_int(
		commit_vector("flat-b", "answer-b", "scopeS", vec_b) == KVS_VECTOR_OK,
		"flat commit b"
	);
	(void)write_text_file("vector_index.manifest", "invalid\n");
	(void)write_text_file("vector_index.snapshot", "invalid\n");
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "flat rebuild_after_load");
	memset(&status, 0, sizeof(status));
	failed |= expect_int(kvs_vector_index_snapshot_status_copy(&status) ==
	                     KVS_VECTOR_OK, "flat status copy");
	failed |= expect_int(status.snapshot_load_attempted == 0,
	                     "flat did not attempt snapshot");
	failed |= expect_int(status.snapshot_loaded == 0, "flat not loaded");
	failed |= expect_int(search_for("flat-b-query", "scopeS", vec_b, "flat-b") ==
	                     KVS_VECTOR_OK, "flat search");
	kvs_vector_destroy();
	return failed;
}

int main(void) {
	char cwd[1024];
	char tmp_template[] = "/tmp/kvlite_hnsw_s3_XXXXXX";
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

	failed |= run_valid_snapshot_load();
	failed |= run_v2_fallback_case("missing manifest",
	                               prepare_missing_manifest, 0);
	failed |= run_v2_fallback_case("manifest param mismatch",
	                               prepare_param_mismatch, 0);
	failed |= run_v2_fallback_case("snapshot checksum mismatch",
	                               prepare_checksum_mismatch, 0);
	failed |= run_v2_fallback_case("corrupted snapshot",
	                               prepare_corrupted_snapshot, 1);
	failed |= run_v2_fallback_case("deserialize failure",
	                               prepare_deserialize_failure, 1);
	failed |= run_v2_fallback_case("text snapshot failure",
	                               prepare_text_snapshot_failure, 1);
	failed |= run_non_v2_backend();

	cleanup_snapshot_files();
	mem_destroy();

	if (chdir(cwd) != 0) {
		fprintf(stderr, "FAIL: restore cwd\n");
		return 1;
	}
	(void)rmdir(tmp_dir);

	if (failed) {
		fprintf(stderr, "vector_index_snapshot_load_after_records_test FAILED\n");
		return 1;
	}

	printf("vector_index_snapshot_load_after_records_test PASS\n");
	return 0;
}
