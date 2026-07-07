#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"

#include <stdio.h>
#include <string.h>

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
	if (index_type) {
		snprintf(g_cfg.vector_index_type, sizeof(g_cfg.vector_index_type), "%s", index_type);
	}
}

static int expect_default_core_hnsw_v2(void) {
	kvs_vector_info_t info;
	int failed = 0;

	reset_config(NULL);
	failed |= expect_int(
		strcmp(g_cfg.vector_index_type, "core_hnsw_v2") == 0,
		"default config string is core_hnsw_v2"
	);
	failed |= expect_int(
		g_cfg.vector_hnsw_v2_experimental_enabled == 0,
		"legacy v2 flag defaults to 0"
	);
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "default init");
	memset(&info, 0, sizeof(info));
	failed |= expect_int(kvs_vector_info(&info) == KVS_VECTOR_OK, "default info");
	failed |= expect_int(
		info.index_type == KVS_VECTOR_INDEX_CORE_HNSW_V2,
		"default selects core_hnsw_v2"
	);
	failed |= expect_int(
		info.core_hnsw_v2_experimental_enabled == 0,
		"legacy flag visible but not gating"
	);
	kvs_vector_destroy();
	return failed;
}

static int expect_flat_maps_core_flat(void) {
	kvs_vector_info_t info;
	int failed = 0;

	reset_config("flat");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "flat init");
	memset(&info, 0, sizeof(info));
	failed |= expect_int(kvs_vector_info(&info) == KVS_VECTOR_OK, "flat info");
	failed |= expect_int(
		info.index_type == KVS_VECTOR_INDEX_CORE_FLAT,
		"flat selects core_flat baseline"
	);
	kvs_vector_destroy();
	return failed;
}

static int expect_legacy_core_hnsw_fallback(void) {
	kvs_vector_info_t info;
	int failed = 0;

	reset_config("core_hnsw");
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, "removed core_hnsw fallback init");
	memset(&info, 0, sizeof(info));
	failed |= expect_int(kvs_vector_info(&info) == KVS_VECTOR_OK, "removed core_hnsw fallback info");
	failed |= expect_int(
		info.index_type == KVS_VECTOR_INDEX_CORE_FLAT,
		"explicit core_hnsw falls back to core_flat"
	);
	kvs_vector_destroy();
	return failed;
}

static int expect_removed_external_backend_fallback(const char *name) {
	kvs_vector_info_t info;
	int failed = 0;
	char label[128];

	reset_config(name);
	snprintf(label, sizeof(label), "%s fallback init", name);
	failed |= expect_int(kvs_vector_init() == KVS_VECTOR_OK, label);
	memset(&info, 0, sizeof(info));
	snprintf(label, sizeof(label), "%s fallback info", name);
	failed |= expect_int(kvs_vector_info(&info) == KVS_VECTOR_OK, label);
	snprintf(label, sizeof(label), "%s falls back to core_flat", name);
	failed |= expect_int(
		info.index_type == KVS_VECTOR_INDEX_CORE_FLAT,
		label
	);
	kvs_vector_destroy();
	return failed;
}

int main(void) {
	int failed = 0;

	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	mem_init();

	failed |= expect_default_core_hnsw_v2();
	failed |= expect_flat_maps_core_flat();
	failed |= expect_legacy_core_hnsw_fallback();
	failed |= expect_removed_external_backend_fallback("hnsw");
	failed |= expect_removed_external_backend_fallback("hnswlib");
	failed |= expect_removed_external_backend_fallback("hnswlib_hnsw");
	failed |= expect_removed_external_backend_fallback("faiss");
	failed |= expect_removed_external_backend_fallback("faiss_flat");
	failed |= expect_removed_external_backend_fallback("faiss_hnsw");

	mem_destroy();

	if (failed) {
		fprintf(stderr, "vector_core_hnsw_v2_default_backend_test FAILED\n");
		return 1;
	}

	printf("vector_core_hnsw_v2_default_backend_test PASS\n");
	return 0;
}
