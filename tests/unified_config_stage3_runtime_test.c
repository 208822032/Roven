#define _POSIX_C_SOURCE 200809L
#define KVS_VECTOR_INTERNAL

#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_vector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern kvs_config_t g_cfg;

#define CHECK(condition, message) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "FAIL: %s\n", message); \
			return 1; \
		} \
	} while (0)

static void reset_config(int dimension) {
	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	g_cfg.vector_dimension = dimension;
	g_cfg.embedding_dim = dimension;
	g_cfg.vector_hnsw_initial_capacity = 32;
	g_cfg.vector_snapshot_enabled = 0;
	g_cfg.vector_rebuild_delta_initial_capacity = 2;
	g_cfg.vector_rebuild_delta_max_entries = 2;
}

static int test_hash_dimension_hnsw_and_profiles(void) {
	float *embedding = NULL;
	int dim = 0;
	double norm = 0.0;
	int i;

	reset_config(37);
	g_cfg.vector_hnsw_m = 48;
	g_cfg.vector_hnsw_m0 = 64;
	g_cfg.vector_hnsw_ef_construction = 96;
	g_cfg.vector_hnsw_ef_search = 32;
	g_cfg.observability_hnsw_build_profile_enabled = 0;
	g_cfg.observability_hnsw_search_profile_enabled = 0;
	CHECK(kvs_vector_init() == KVS_VECTOR_OK, "hash vector init");
	CHECK(kvs_vector_embedding_provider_dimension() == 37, "hash dimension");
	CHECK(g_vector_state.core_hnsw_v2_index.M == 48 &&
		g_vector_state.core_hnsw_v2_index.M0 == 64 &&
		g_vector_state.core_hnsw_v2_index.ef_construction == 96 &&
		g_vector_state.core_hnsw_v2_index.ef_search == 32,
		"effective HNSW parameters");
	CHECK(!g_vector_state.core_hnsw_v2_index.build_profiling_enabled &&
		!g_vector_state.core_hnsw_v2_index.search_profiling_enabled,
		"HNSW profiling disabled");
	CHECK(kvs_vector_embedding_generate("stage3", 6, &embedding, &dim) == KVS_VECTOR_OK,
		"hash embedding generate");
	CHECK(dim == 37 && embedding != NULL, "hash output dimension");
	for (i = 0; i < dim; ++i) norm += (double)embedding[i] * (double)embedding[i];
	CHECK(fabs(norm - 1.0) < 1e-5, "hash normalization");
	kvs_free(embedding);
	kvs_vector_destroy();

	reset_config(8);
	g_cfg.observability_hnsw_build_profile_enabled = 1;
	g_cfg.observability_hnsw_search_profile_enabled = 1;
	CHECK(kvs_vector_init() == KVS_VECTOR_OK, "profile-enabled vector init");
	CHECK(g_vector_state.core_hnsw_v2_index.build_profiling_enabled &&
		g_vector_state.core_hnsw_v2_index.search_profiling_enabled,
		"HNSW profiling enabled");
	kvs_vector_destroy();
	return 0;
}

static int test_onnx_capability_gate(void) {
	reset_config(8);
	snprintf(g_cfg.embedding_provider, sizeof(g_cfg.embedding_provider), "%s", "local_c");
	snprintf(g_cfg.embedding_model_path, sizeof(g_cfg.embedding_model_path), "%s", "/tmp/model.onnx");
	snprintf(g_cfg.embedding_vocab_path, sizeof(g_cfg.embedding_vocab_path), "%s", "/tmp/vocab.txt");
	CHECK(kvs_vector_init() == KVS_VECTOR_ERR_NOT_IMPLEMENTED,
		"non-ONNX binary accepted local_c");
	return 0;
}

static int test_rebuild_controls_and_delta_limit(void) {
	float vector[8] = {1.0f};

	reset_config(8);
	g_cfg.vector_rebuild_async_enabled = 0;
	CHECK(kvs_vector_init() == KVS_VECTOR_OK, "rebuild vector init");
	CHECK(kvs_vector_index_rebuild_async_start() == KVS_VECTOR_ERR_NOT_IMPLEMENTED,
		"async disabled gate");
	CHECK(kvs_vector_manager_rebuild_capture_begin_for_test() == KVS_VECTOR_OK,
		"capture begin");
	CHECK(kvs_vector_manager_capture_upsert_delta(1, vector, 8) == KVS_VECTOR_OK,
		"first delta");
	CHECK(kvs_vector_manager_capture_upsert_delta(2, vector, 8) == KVS_VECTOR_OK,
		"second delta");
	CHECK(kvs_vector_manager_capture_upsert_delta(3, vector, 8) != KVS_VECTOR_OK,
		"delta limit did not fail");
	CHECK(g_vector_state.rebuild.state == KVS_VECTOR_REBUILD_FAILED &&
		g_vector_state.rebuild.delta_count == 2,
		"delta limit failure state");
	kvs_vector_destroy();
	return 0;
}

static int test_snapshot_fail_policy(void) {
	char manifest[256];
	char snapshot[256];

	reset_config(8);
	g_cfg.vector_snapshot_enabled = 1;
	g_cfg.vector_snapshot_load_on_start = 1;
	snprintf(g_cfg.vector_snapshot_invalid_action,
		sizeof(g_cfg.vector_snapshot_invalid_action), "%s", "fail");
	snprintf(manifest, sizeof(manifest), "/tmp/kvlite-stage3-missing-%ld.manifest", (long)getpid());
	snprintf(snapshot, sizeof(snapshot), "/tmp/kvlite-stage3-missing-%ld.snapshot", (long)getpid());
	snprintf(g_cfg.vector_snapshot_manifest_path,
		sizeof(g_cfg.vector_snapshot_manifest_path), "%s", manifest);
	snprintf(g_cfg.vector_snapshot_index_path,
		sizeof(g_cfg.vector_snapshot_index_path), "%s", snapshot);
	unlink(manifest);
	unlink(snapshot);
	CHECK(kvs_vector_init() == KVS_VECTOR_OK, "snapshot vector init");
	CHECK(kvs_vector_rebuild_after_load() != KVS_VECTOR_OK,
		"invalid_action=fail rebuilt missing snapshot");
	CHECK(access(manifest, F_OK) != 0 && access(snapshot, F_OK) != 0,
		"fail policy published snapshot");
	kvs_vector_destroy();

	reset_config(8);
	snprintf(g_cfg.vector_snapshot_manifest_path,
		sizeof(g_cfg.vector_snapshot_manifest_path), "%s", manifest);
	snprintf(g_cfg.vector_snapshot_index_path,
		sizeof(g_cfg.vector_snapshot_index_path), "%s", snapshot);
	CHECK(kvs_vector_init() == KVS_VECTOR_OK, "disabled snapshot vector init");
	CHECK(kvs_vector_rebuild_after_load() == KVS_VECTOR_OK,
		"disabled snapshot records rebuild");
	CHECK(access(manifest, F_OK) != 0 && access(snapshot, F_OK) != 0,
		"snapshot disabled still wrote files");
	kvs_vector_destroy();
	return 0;
}

int main(void) {
	int failures = 0;

	mem_init();
	failures += test_hash_dimension_hnsw_and_profiles();
	failures += test_onnx_capability_gate();
	failures += test_rebuild_controls_and_delta_limit();
	failures += test_snapshot_fail_policy();
	mem_destroy();
	if (failures != 0) {
		fprintf(stderr, "unified config Stage 3 runtime tests failed: %d\n", failures);
		return 1;
	}
	printf("unified config Stage 3 runtime tests: PASS\n");
	return 0;
}
