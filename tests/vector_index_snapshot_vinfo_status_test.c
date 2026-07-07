#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"

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
	(void)unlink("vector_index.manifest.tmp.tmp");
	(void)unlink("vector_index.manifest.bak");
	(void)unlink("vector_index.snapshot");
	(void)unlink("vector_index.snapshot.tmp");
	(void)unlink("vector_index.snapshot.bak");
	(void)rmdir("vector_index.manifest.tmp");
	(void)rmdir("vector_index.snapshot.tmp");
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

static int setup_v2_records(void) {
	float vec_a[DIM];
	float vec_b[DIM];
	float vec_c[DIM];
	int failed = 0;

	one_hot(vec_a, 0);
	one_hot(vec_b, 1);
	one_hot(vec_c, 2);
	reset_config("core_hnsw_v2");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "v2 init");
	failed |= expect_int(
		commit_vector("s5-a", "answer-a", "scopeS", vec_a) == KVS_VECTOR_OK,
		"commit s5-a"
	);
	failed |= expect_int(
		commit_vector("s5-b", "answer-b", "scopeS", vec_b) == KVS_VECTOR_OK,
		"commit s5-b"
	);
	failed |= expect_int(
		commit_vector("s5-c", "answer-c", "scopeOther", vec_c) ==
			KVS_VECTOR_OK,
		"commit s5-c"
	);
	return failed ? KVS_VECTOR_ERR : KVS_VECTOR_OK;
}

static int snapshot_info(kvs_vector_info_t *info) {
	if (!info) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	memset(info, 0, sizeof(*info));
	return kvs_vector_info(info);
}

static int run_direct_fallback_and_valid_load(void) {
	kvs_vector_info_t info;
	int failed = 0;

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records() == KVS_VECTOR_OK, "fallback setup");
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "missing snapshot fallback rebuild");
	failed |= expect_int(snapshot_info(&info) == KVS_VECTOR_OK,
	                     "fallback info");
	failed |= expect_int(info.snapshot_supported == 1,
	                     "fallback snapshot supported");
	failed |= expect_int(info.snapshot_load_attempted == 1,
	                     "fallback load attempted");
	failed |= expect_int(info.snapshot_loaded == 0, "fallback not loaded");
	failed |= expect_int(info.snapshot_valid == 0, "fallback invalid");
	failed |= expect_int(info.loaded_from_snapshot == 0,
	                     "fallback loaded_from_snapshot");
	failed |= expect_int(info.snapshot_fallback_rebuild_count == 1,
	                     "fallback count");
	failed |= expect_int(info.snapshot_last_error &&
	                     info.snapshot_last_error[0] != '\0',
	                     "fallback error text");
	failed |= expect_int(info.snapshot_write_attempted == 1,
	                     "fallback write attempted");
	failed |= expect_int(info.snapshot_written == 1, "fallback written");

	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "valid snapshot load");
	failed |= expect_int(snapshot_info(&info) == KVS_VECTOR_OK,
	                     "valid info");
	failed |= expect_int(info.snapshot_loaded == 1, "valid loaded");
	failed |= expect_int(info.snapshot_valid == 1, "valid snapshot valid");
	failed |= expect_int(info.loaded_from_snapshot == 1,
	                     "valid loaded_from_snapshot");
	failed |= expect_int(info.snapshot_write_attempted == 0,
	                     "valid load no rewrite");

	failed |= expect_int(overwrite_byte("vector_index.snapshot", 0, 0x7f) == 0,
	                     "corrupt snapshot");
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "corrupt snapshot fallback rebuild");
	failed |= expect_int(snapshot_info(&info) == KVS_VECTOR_OK,
	                     "corrupt info");
	failed |= expect_int(info.snapshot_load_attempted == 1,
	                     "corrupt load attempted");
	failed |= expect_int(info.snapshot_loaded == 0, "corrupt not loaded");
	failed |= expect_int(info.snapshot_valid == 0, "corrupt invalid");
	failed |= expect_int(info.snapshot_fallback_rebuild_count == 1,
	                     "corrupt fallback count");
	failed |= expect_int(info.snapshot_last_error &&
	                     info.snapshot_last_error[0] != '\0',
	                     "corrupt error text");
	failed |= expect_int(info.snapshot_write_attempted == 1,
	                     "corrupt rewrite attempted");
	failed |= expect_int(info.snapshot_written == 1,
	                     "corrupt rewrite success");

	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_direct_write_failure(void) {
	kvs_vector_info_t info;
	int failed = 0;

	cleanup_snapshot_files();
	failed |= expect_int(setup_v2_records() == KVS_VECTOR_OK,
	                     "write failure setup");
	failed |= expect_int(mkdir("vector_index.snapshot.tmp", 0500) == 0,
	                     "create blocking snapshot tmp dir");
	failed |= expect_int(kvs_vector_index_rebuild() == KVS_VECTOR_OK,
	                     "write failure rebuild still succeeds");
	failed |= expect_int(snapshot_info(&info) == KVS_VECTOR_OK,
	                     "write failure info");
	failed |= expect_int(info.snapshot_write_attempted == 1,
	                     "write failure attempted");
	failed |= expect_int(info.snapshot_written == 0,
	                     "write failure not written");
	failed |= expect_int(info.snapshot_last_write_error &&
	                     info.snapshot_last_write_error[0] != '\0',
	                     "write failure error text");
	failed |= expect_int(info.snapshot_last_bytes == 0,
	                     "write failure bytes zero");
	failed |= expect_int(info.snapshot_last_checksum == 0,
	                     "write failure checksum zero");
	(void)rmdir("vector_index.snapshot.tmp");
	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

static int run_direct_non_v2(void) {
	kvs_vector_info_t info;
	float vec_a[DIM];
	int failed = 0;

	cleanup_snapshot_files();
	one_hot(vec_a, 0);
	reset_config("core_flat");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "flat init");
	failed |= expect_int(
		commit_vector("s5-flat", "answer-flat", "scopeS", vec_a) ==
			KVS_VECTOR_OK,
		"flat commit"
	);
	failed |= expect_int(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
	                     "flat rebuild after load");
	failed |= expect_int(snapshot_info(&info) == KVS_VECTOR_OK, "flat info");
	failed |= expect_int(info.snapshot_supported == 0,
	                     "flat snapshot unsupported");
	failed |= expect_int(info.snapshot_load_attempted == 0,
	                     "flat load not attempted");
	failed |= expect_int(info.snapshot_write_attempted == 0,
	                     "flat write not attempted");
	kvs_vector_destroy();
	cleanup_snapshot_files();
	return failed;
}

int main(void) {
	char cwd[1024];
	char tmp_template[] = "/tmp/kvlite_hnsw_s5_vinfo_XXXXXX";
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

	failed |= run_direct_fallback_and_valid_load();
	failed |= run_direct_write_failure();
	failed |= run_direct_non_v2();

	cleanup_snapshot_files();
	mem_destroy();

	if (chdir(cwd) != 0) {
		fprintf(stderr, "FAIL: restore cwd\n");
		return 1;
	}
	(void)rmdir(tmp_dir);

	if (failed) {
		fprintf(stderr, "vector_index_snapshot_vinfo_status_test FAILED\n");
		return 1;
	}

	printf("vector_index_snapshot_vinfo_status_test PASS\n");
	return 0;
}
