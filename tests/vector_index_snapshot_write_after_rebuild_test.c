#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"
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
	(void)unlink("vector_index.manifest.tmp.tmp");
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

static int verify_manifest_matches_current_records(void) {
	kvs_vector_index_snapshot_manifest_t actual;
	kvs_vector_index_snapshot_manifest_t expected;
	char reason[128];

	reason[0] = '\0';
	if (kvs_vector_index_snapshot_manifest_read_file(
		    "vector_index.manifest",
		    &actual
	    ) != 0) {
		fprintf(stderr, "manifest read failed\n");
		return KVS_VECTOR_ERR;
	}
	if (kvs_vector_index_snapshot_build_expected_manifest_after_records(
		    &g_vector_state,
		    "vector_index.snapshot",
		    &expected,
		    reason,
		    sizeof(reason)
	    ) != KVS_VECTOR_OK) {
		fprintf(stderr, "expected manifest build failed: %s\n", reason);
		return KVS_VECTOR_ERR;
	}
	if (kvs_vector_index_snapshot_manifest_validate(
		    &actual,
		    &expected,
		    reason,
		    sizeof(reason)
	    ) != 0) {
		fprintf(stderr, "manifest validate failed: %s\n", reason);
		return KVS_VECTOR_ERR;
	}
	return KVS_VECTOR_OK;
}

static int verify_written_status(
	const char *label,
	int expected_written
) {
	kvs_vector_snapshot_status_t status;
	char check_label[128];
	int failed = 0;

	memset(&status, 0, sizeof(status));
	failed |= expect_int(
		kvs_vector_index_snapshot_status_copy(&status) == KVS_VECTOR_OK,
		"status copy"
	);
	snprintf(check_label, sizeof(check_label), "%s write attempted", label);
	failed |= expect_int(status.snapshot_write_attempted == 1, check_label);
	snprintf(check_label, sizeof(check_label), "%s write result", label);
	failed |= expect_int(status.snapshot_written == expected_written, check_label);
	if (expected_written) {
		snprintf(check_label, sizeof(check_label), "%s write bytes", label);
		failed |= expect_int(status.last_snapshot_bytes > 0, check_label);
		snprintf(check_label, sizeof(check_label), "%s write checksum", label);
		failed |= expect_int(status.last_snapshot_checksum > 0, check_label);
		snprintf(check_label, sizeof(check_label), "%s write error ok", label);
		failed |= expect_int(
			strcmp(status.last_snapshot_write_error, "ok") == 0,
			check_label
		);
	}
	return failed;
}

static int setup_v2_records(float *vec_a, float *vec_b, float *vec_c) {
	int failed = 0;

	one_hot(vec_a, 0);
	one_hot(vec_b, 1);
	one_hot(vec_c, 2);
	reset_config("core_hnsw_v2");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "v2 init");
	failed |= expect_int(
		commit_vector("s4-a", "answer-a", "scopeS", vec_a) == KVS_VECTOR_OK,
		"commit s4-a"
	);
	failed |= expect_int(
		commit_vector("s4-b", "answer-b", "scopeS", vec_b) == KVS_VECTOR_OK,
		"commit s4-b"
	);
	failed |= expect_int(
		commit_vector("s4-c", "answer-c", "scopeOther", vec_c) ==
			KVS_VECTOR_OK,
		"commit s4-c"
	);
	return failed ? KVS_VECTOR_ERR : KVS_VECTOR_OK;
}

static int reset_v2_index_only(void) {
	return kvs_hnsw_v2_index_reset(&g_vector_state.core_hnsw_v2_index);
}

static int run_fallback_rebuild_writes_snapshot(void) {
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	kvs_vector_snapshot_status_t status;
	int failed = 0;

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records(vec_a, vec_b, vec_c) == KVS_VECTOR_OK,
	                     "fallback setup");
	failed |= expect_int(reset_v2_index_only() == KVS_VECTOR_OK,
	                     "fallback reset index");
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "fallback rebuild_after_load");
	failed |= verify_written_status("fallback", 1);
	failed |= expect_int(path_exists("vector_index.manifest"),
	                     "fallback manifest exists");
	failed |= expect_int(path_exists("vector_index.snapshot"),
	                     "fallback snapshot exists");
	failed |= expect_int(verify_manifest_matches_current_records() == KVS_VECTOR_OK,
	                     "fallback manifest matches");
	memset(&status, 0, sizeof(status));
	failed |= expect_int(kvs_vector_index_snapshot_status_copy(&status) ==
	                     KVS_VECTOR_OK, "fallback status copy");
	failed |= expect_int(status.snapshot_load_attempted == 1,
	                     "fallback load attempted");
	failed |= expect_int(status.snapshot_loaded == 0,
	                     "fallback not loaded");
	failed |= expect_int(status.fallback_rebuild_count == 1,
	                     "fallback count");
	failed |= expect_int(search_for("s4-b-query", "scopeS", vec_b, "s4-b") ==
	                     KVS_VECTOR_OK, "fallback search");

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_written_snapshot_loads_without_rewrite(void) {
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	kvs_vector_snapshot_status_t status;
	int failed = 0;

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records(vec_a, vec_b, vec_c) == KVS_VECTOR_OK,
	                     "load setup");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "load explicit rebuild writes");
	failed |= verify_written_status("load baseline", 1);
	(void)write_text_file("vector_index.snapshot.tmp", "leftover tmp\n");
	(void)write_text_file("vector_index.manifest.tmp", "leftover tmp\n");
	failed |= expect_int(reset_v2_index_only() == KVS_VECTOR_OK,
	                     "load reset index");
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "load rebuild_after_load");
	memset(&status, 0, sizeof(status));
	failed |= expect_int(kvs_vector_index_snapshot_status_copy(&status) ==
	                     KVS_VECTOR_OK, "load status copy");
	failed |= expect_int(status.snapshot_loaded == 1, "load snapshot loaded");
	failed |= expect_int(status.loaded_from_snapshot == 1,
	                     "load loaded_from_snapshot");
	failed |= expect_int(status.snapshot_write_attempted == 0,
	                     "load success does not rewrite");
	failed |= expect_int(
		g_vector_state.core_hnsw_v2_index.build_stats.add_count == 0,
		"load no full rebuild"
	);
	failed |= expect_int(search_for("s4-b-query", "scopeS", vec_b, "s4-b") ==
	                     KVS_VECTOR_OK, "load search");

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_missing_manifest_rewrites_snapshot(void) {
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	int failed = 0;

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records(vec_a, vec_b, vec_c) == KVS_VECTOR_OK,
	                     "missing setup");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "missing baseline rebuild");
	(void)unlink("vector_index.manifest");
	failed |= expect_int(reset_v2_index_only() == KVS_VECTOR_OK,
	                     "missing reset index");
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "missing fallback rebuild");
	failed |= verify_written_status("missing", 1);
	failed |= expect_int(verify_manifest_matches_current_records() == KVS_VECTOR_OK,
	                     "missing rewritten valid");

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_corrupted_snapshot_rewrites_valid_snapshot(void) {
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	kvs_vector_snapshot_status_t status;
	int failed = 0;

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records(vec_a, vec_b, vec_c) == KVS_VECTOR_OK,
	                     "corrupt setup");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "corrupt baseline rebuild");
	failed |= expect_int(overwrite_byte("vector_index.snapshot", 8, 0xffu) == 0,
	                     "corrupt snapshot byte");
	failed |= expect_int(reset_v2_index_only() == KVS_VECTOR_OK,
	                     "corrupt reset index");
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "corrupt fallback rebuild");
	failed |= verify_written_status("corrupt", 1);
	failed |= expect_int(verify_manifest_matches_current_records() == KVS_VECTOR_OK,
	                     "corrupt rewritten valid");
	failed |= expect_int(reset_v2_index_only() == KVS_VECTOR_OK,
	                     "corrupt second reset");
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "corrupt second load");
	memset(&status, 0, sizeof(status));
	failed |= expect_int(kvs_vector_index_snapshot_status_copy(&status) ==
	                     KVS_VECTOR_OK, "corrupt status copy");
	failed |= expect_int(status.snapshot_loaded == 1,
	                     "corrupt rewritten snapshot loads");

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_vindex_rebuild_writes_snapshot(void) {
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	int failed = 0;

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records(vec_a, vec_b, vec_c) == KVS_VECTOR_OK,
	                     "vindex setup");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "vindex rebuild");
	failed |= verify_written_status("vindex", 1);
	failed |= expect_int(verify_manifest_matches_current_records() == KVS_VECTOR_OK,
	                     "vindex manifest valid");

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_non_v2_backend_does_not_write(void) {
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
		commit_vector("flat-s4-a", "answer-a", "scopeS", vec_a) == KVS_VECTOR_OK,
		"flat commit a"
	);
	failed |= expect_int(
		commit_vector("flat-s4-b", "answer-b", "scopeS", vec_b) == KVS_VECTOR_OK,
		"flat commit b"
	);
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "flat rebuild");
	memset(&status, 0, sizeof(status));
	failed |= expect_int(kvs_vector_index_snapshot_status_copy(&status) ==
	                     KVS_VECTOR_OK, "flat status copy");
	failed |= expect_int(status.snapshot_write_attempted == 0,
	                     "flat write not attempted");
	failed |= expect_int(!path_exists("vector_index.manifest"),
	                     "flat no manifest");
	failed |= expect_int(!path_exists("vector_index.snapshot"),
	                     "flat no snapshot");
	failed |= expect_int(search_for("flat-s4-b-query", "scopeS", vec_b,
	                                "flat-s4-b") == KVS_VECTOR_OK,
	                     "flat search");

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_write_failure_does_not_block_rebuild_or_search(void) {
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	int failed = 0;

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records(vec_a, vec_b, vec_c) == KVS_VECTOR_OK,
	                     "failure setup");
	failed |= expect_int(mkdir("vector_index.snapshot.tmp", 0500) == 0,
	                     "failure create blocking tmp dir");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "failure rebuild still succeeds");
	failed |= verify_written_status("failure", 0);
	failed |= expect_int(search_for("s4-b-query", "scopeS", vec_b, "s4-b") ==
	                     KVS_VECTOR_OK, "failure active search");
	(void)rmdir("vector_index.snapshot.tmp");

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_write_failure_preserves_existing_snapshot(void) {
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	kvs_vector_snapshot_status_t status;
	int failed = 0;

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records(vec_a, vec_b, vec_c) == KVS_VECTOR_OK,
	                     "preserve setup");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "preserve baseline rebuild");
	failed |= expect_int(verify_manifest_matches_current_records() == KVS_VECTOR_OK,
	                     "preserve baseline valid");
	failed |= expect_int(mkdir("vector_index.manifest.tmp", 0500) == 0,
	                     "preserve create blocking manifest tmp dir");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "preserve rebuild still succeeds");
	failed |= verify_written_status("preserve failure", 0);
	failed |= expect_int(search_for("s4-b-query", "scopeS", vec_b, "s4-b") ==
	                     KVS_VECTOR_OK, "preserve active search");
	(void)rmdir("vector_index.manifest.tmp");
	failed |= expect_int(reset_v2_index_only() == KVS_VECTOR_OK,
	                     "preserve reset index");
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "preserve load old snapshot");
	memset(&status, 0, sizeof(status));
	failed |= expect_int(kvs_vector_index_snapshot_status_copy(&status) ==
	                     KVS_VECTOR_OK, "preserve status copy");
	failed |= expect_int(status.snapshot_loaded == 1,
	                     "preserve old snapshot loads");
	failed |= expect_int(status.snapshot_write_attempted == 0,
	                     "preserve load did not rewrite");

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

int main(void) {
	char cwd[1024];
	char tmp_template[] = "/tmp/kvlite_hnsw_s4_XXXXXX";
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

	failed |= run_fallback_rebuild_writes_snapshot();
	failed |= run_written_snapshot_loads_without_rewrite();
	failed |= run_missing_manifest_rewrites_snapshot();
	failed |= run_corrupted_snapshot_rewrites_valid_snapshot();
	failed |= run_vindex_rebuild_writes_snapshot();
	failed |= run_non_v2_backend_does_not_write();
	failed |= run_write_failure_does_not_block_rebuild_or_search();
	failed |= run_write_failure_preserves_existing_snapshot();

	cleanup_snapshot_files();
	mem_destroy();

	if (chdir(cwd) != 0) {
		fprintf(stderr, "FAIL: restore cwd\n");
		return 1;
	}
	(void)rmdir(tmp_dir);

	if (failed) {
		fprintf(stderr, "vector_index_snapshot_write_after_rebuild_test FAILED\n");
		return 1;
	}

	printf("vector_index_snapshot_write_after_rebuild_test PASS\n");
	return 0;
}
