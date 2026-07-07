#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_hnsw_v2.h"
#include "vector/kvs_vector_hnsw_v2_snapshot.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern kvs_config_t g_cfg;

static int expect_true(int cond, const char *label) {
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", label);
		return 1;
	}
	return 0;
}

static uint32_t rng_next(uint64_t *state) {
	uint64_t z;

	*state += 0x9e3779b97f4a7c15ULL;
	z = *state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return (uint32_t)(z >> 32);
}

static void fill_vector(float *vector, size_t dim, uint64_t seed) {
	uint64_t state = seed;
	float norm = 0.0f;
	size_t i;

	for (i = 0; i < dim; i++) {
		float value = (float)((int)(rng_next(&state) % 20001u) - 10000) /
			10000.0f;
		vector[i] = value;
		norm += value * value;
	}
	if (norm > 0.0f) {
		float inv = 1.0f / sqrtf(norm);
		for (i = 0; i < dim; i++) {
			vector[i] *= inv;
		}
	}
}

static void fill_update_vector(float *vector, size_t dim) {
	size_t i;

	for (i = 0; i < dim; i++) {
		vector[i] = 0.0f;
	}
	vector[0] = 1.0f;
	if (dim > 1) {
		vector[1] = 0.25f;
	}
}

static int hits_descending(const kvs_vector_hit_t *hits, size_t count) {
	size_t i;

	for (i = 1; i < count; i++) {
		if (hits[i - 1].score < hits[i].score) {
			return 0;
		}
	}
	return 1;
}

static int hit_contains(
	const kvs_vector_hit_t *hits,
	size_t count,
	uint64_t record_id
) {
	size_t i;

	for (i = 0; i < count; i++) {
		if (hits[i].record_id == record_id) {
			return 1;
		}
	}
	return 0;
}

static int compare_hits(
	const kvs_vector_hit_t *before,
	size_t before_count,
	const kvs_vector_hit_t *after,
	size_t after_count
) {
	size_t i;

	if (before_count != after_count) {
		return 0;
	}
	for (i = 0; i < before_count; i++) {
		if (before[i].record_id != after[i].record_id) {
			return 0;
		}
		if (fabsf(before[i].score - after[i].score) > 0.000001f) {
			return 0;
		}
	}
	return 1;
}

static int init_small_index(
	kvs_hnsw_v2_index_t *index,
	size_t capacity
) {
	int ret;

	memset(index, 0, sizeof(*index));
	ret = kvs_hnsw_v2_index_init(index, 32, KVS_VECTOR_METRIC_IP);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	index->M = 8;
	index->M0 = 16;
	index->ef_construction = 64;
	index->ef_search = 64;
	kvs_hnsw_v2_index_set_seed(index, 0x5a220001ULL);
	return kvs_hnsw_v2_index_reserve(index, capacity);
}

static int build_roundtrip_fixture(kvs_hnsw_v2_index_t *index) {
	float vector[32];
	size_t i;
	int ret;

	ret = init_small_index(index, 128);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	for (i = 0; i < 100; i++) {
		fill_vector(vector, 32, 0x71000000ULL + i);
		ret = kvs_hnsw_v2_index_add(
			index,
			1000000ULL + i,
			vector,
			32
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	if (kvs_hnsw_v2_index_mark_delete(index, 1000003ULL) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_index_mark_delete(index, 1000020ULL) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_index_mark_delete(index, 1000007ULL) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	fill_update_vector(vector, 32);
	ret = kvs_hnsw_v2_index_add(index, 1000007ULL, vector, 32);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	kvs_hnsw_v2_graph_stats_refresh(index);
	kvs_hnsw_v2_memory_refresh(index);
	return KVS_VECTOR_OK;
}

static int test_empty_roundtrip(const char *path) {
	kvs_hnsw_v2_index_t source;
	kvs_hnsw_v2_index_t loaded;
	kvs_vector_hit_t hits[4];
	float query[8] = {0};
	kvs_vector_core_info_t info;
	size_t out_count = 99;
	int failed = 0;

	memset(&source, 0, sizeof(source));
	memset(&loaded, 0, sizeof(loaded));
	failed |= expect_true(
		kvs_hnsw_v2_index_init(&source, 8, KVS_VECTOR_METRIC_IP) ==
			KVS_VECTOR_OK,
		"empty init source"
	);
	failed |= expect_true(
		kvs_hnsw_v2_index_serialize_file(&source, path) == KVS_VECTOR_OK,
		"empty serialize"
	);
	failed |= expect_true(
		kvs_hnsw_v2_index_deserialize_file(&loaded, path) == KVS_VECTOR_OK,
		"empty deserialize"
	);
	failed |= expect_true(
		kvs_hnsw_v2_index_info(&loaded, &info) == KVS_VECTOR_OK,
		"empty info"
	);
	failed |= expect_true(info.size == 0, "empty size");
	failed |= expect_true(info.deleted_count == 0, "empty deleted");
	failed |= expect_true(
		kvs_hnsw_v2_index_search(&loaded, query, 8, 3, 16, hits, &out_count) ==
			KVS_VECTOR_OK,
		"empty search"
	);
	failed |= expect_true(out_count == 0, "empty search count");

	kvs_hnsw_v2_index_destroy(&source);
	kvs_hnsw_v2_index_destroy(&loaded);
	return failed;
}

static int compare_graph_stats(
	const kvs_hnsw_v2_index_t *source,
	const kvs_hnsw_v2_index_t *loaded
) {
	if (source->size != loaded->size ||
	    source->capacity != loaded->capacity ||
	    source->deleted_count != loaded->deleted_count ||
	    source->entry_point != loaded->entry_point ||
	    source->has_entry != loaded->has_entry ||
	    source->max_level != loaded->max_level) {
		return 0;
	}
	if (source->graph_stats.level0_degree.min !=
	        loaded->graph_stats.level0_degree.min ||
	    source->graph_stats.level0_degree.p50 !=
	        loaded->graph_stats.level0_degree.p50 ||
	    source->graph_stats.level0_degree.p95 !=
	        loaded->graph_stats.level0_degree.p95 ||
	    source->graph_stats.level0_degree.max !=
	        loaded->graph_stats.level0_degree.max ||
	    fabs(source->graph_stats.level0_degree.avg -
	         loaded->graph_stats.level0_degree.avg) > 0.000001) {
		return 0;
	}
	return source->graph_stats.duplicate_edge_count == 0 &&
	       loaded->graph_stats.duplicate_edge_count == 0 &&
	       source->graph_stats.degree_overflow_count == 0 &&
	       loaded->graph_stats.degree_overflow_count == 0;
}

static int test_small_roundtrip(const char *path) {
	kvs_hnsw_v2_index_t source;
	kvs_hnsw_v2_index_t loaded;
	kvs_vector_hit_t before[10];
	kvs_vector_hit_t after[10];
	float query[32];
	uint32_t slot = UINT32_MAX;
	size_t before_count = 0;
	size_t after_count = 0;
	int failed = 0;

	memset(&source, 0, sizeof(source));
	memset(&loaded, 0, sizeof(loaded));
	fill_update_vector(query, 32);
	failed |= expect_true(
		build_roundtrip_fixture(&source) == KVS_VECTOR_OK,
		"small fixture build"
	);
	failed |= expect_true(source.size == 101, "small fixture size");
	failed |= expect_true(source.deleted_count == 3, "small fixture deleted count");
	failed |= expect_true(
		kvs_hnsw_v2_index_search(&source, query, 32, 10, 64, before, &before_count) ==
			KVS_VECTOR_OK,
		"small search before"
	);
	failed |= expect_true(before_count == 10, "small before count");
	failed |= expect_true(hits_descending(before, before_count), "before descending");
	failed |= expect_true(hit_contains(before, before_count, 1000007ULL),
	                      "before contains updated id");
	failed |= expect_true(!hit_contains(before, before_count, 1000003ULL),
	                      "before skips deleted id");

	failed |= expect_true(
		kvs_hnsw_v2_index_serialize_file(&source, path) == KVS_VECTOR_OK,
		"small serialize"
	);
	failed |= expect_true(
		kvs_hnsw_v2_index_deserialize_file(&loaded, path) == KVS_VECTOR_OK,
		"small deserialize"
	);
	failed |= expect_true(
		kvs_hnsw_v2_index_search(&loaded, query, 32, 10, 64, after, &after_count) ==
			KVS_VECTOR_OK,
		"small search after"
	);
	failed |= expect_true(hits_descending(after, after_count), "after descending");
	failed |= expect_true(compare_hits(before, before_count, after, after_count),
	                      "search results match");
	failed |= expect_true(compare_graph_stats(&source, &loaded), "graph stats match");
	failed |= expect_true(
		kvs_vector_id_slot_map_get(&loaded.record_id_to_slot, 1000007ULL, &slot) ==
			KVS_VECTOR_OK,
		"updated id lookup"
	);
	failed |= expect_true(slot < loaded.size, "updated slot in range");
	failed |= expect_true(loaded.slot_to_record_id[slot] == 1000007ULL,
	                      "updated slot id");
	failed |= expect_true(loaded.deleted_flags[slot] == 0, "updated slot active");
	failed |= expect_true(
		kvs_hnsw_v2_index_mark_delete(&loaded, 1000007ULL) == KVS_VECTOR_OK,
		"updated id can be deleted after load"
	);

	kvs_hnsw_v2_index_destroy(&source);
	kvs_hnsw_v2_index_destroy(&loaded);
	return failed;
}

static int copy_file(const char *src, const char *dst) {
	FILE *in = fopen(src, "rb");
	FILE *out;
	unsigned char buf[4096];
	size_t n;
	int ret = 0;

	if (!in) {
		return -1;
	}
	out = fopen(dst, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			ret = -1;
			break;
		}
	}
	if (ferror(in)) {
		ret = -1;
	}
	if (fclose(out) != 0) {
		ret = -1;
	}
	if (fclose(in) != 0) {
		ret = -1;
	}
	return ret;
}

static int overwrite_byte(const char *path, long offset, unsigned char value) {
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

static int protected_target_still_valid(kvs_hnsw_v2_index_t *index) {
	kvs_vector_hit_t hits[4];
	float query[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	size_t out_count = 0;

	if (kvs_hnsw_v2_index_search(index, query, 4, 1, 8, hits, &out_count) !=
	    KVS_VECTOR_OK) {
		return 0;
	}
	return out_count == 1 && hits[0].record_id == 42ULL;
}

static int init_protected_target(kvs_hnsw_v2_index_t *index) {
	float vector[4] = {1.0f, 0.0f, 0.0f, 0.0f};

	memset(index, 0, sizeof(*index));
	if (kvs_hnsw_v2_index_init(index, 4, KVS_VECTOR_METRIC_IP) !=
	    KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	index->M = 4;
	index->M0 = 8;
	index->ef_construction = 16;
	index->ef_search = 16;
	if (kvs_hnsw_v2_index_reserve(index, 4) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	return kvs_hnsw_v2_index_add(index, 42ULL, vector, 4);
}

static int test_invalid_files_do_not_publish(
	const char *valid_path,
	const char *bad_magic_path,
	const char *truncated_path,
	const char *malformed_path
) {
	kvs_hnsw_v2_index_t target;
	int failed = 0;

	failed |= expect_true(init_protected_target(&target) == KVS_VECTOR_OK,
	                      "protected target init");
	failed |= expect_true(protected_target_still_valid(&target),
	                      "protected target initial search");

	failed |= expect_true(copy_file(valid_path, bad_magic_path) == 0,
	                      "copy bad magic fixture");
	failed |= expect_true(overwrite_byte(bad_magic_path, 0, 'X') == 0,
	                      "corrupt magic");
	failed |= expect_true(
		kvs_hnsw_v2_index_deserialize_file(&target, bad_magic_path) !=
			KVS_VECTOR_OK,
		"bad magic rejected"
	);
	failed |= expect_true(protected_target_still_valid(&target),
	                      "target valid after bad magic");

	failed |= expect_true(copy_file(valid_path, truncated_path) == 0,
	                      "copy truncated fixture");
	failed |= expect_true(truncate(truncated_path, 12) == 0, "truncate fixture");
	failed |= expect_true(
		kvs_hnsw_v2_index_deserialize_file(&target, truncated_path) !=
			KVS_VECTOR_OK,
		"truncated rejected"
	);
	failed |= expect_true(protected_target_still_valid(&target),
	                      "target valid after truncated");

	failed |= expect_true(copy_file(valid_path, malformed_path) == 0,
	                      "copy malformed fixture");
	failed |= expect_true(overwrite_byte(malformed_path, 32, 0x7f) == 0,
	                      "malform header");
	failed |= expect_true(
		kvs_hnsw_v2_index_deserialize_file(&target, malformed_path) !=
			KVS_VECTOR_OK,
		"malformed rejected"
	);
	failed |= expect_true(protected_target_still_valid(&target),
	                      "target valid after malformed");

	kvs_hnsw_v2_index_destroy(&target);
	return failed;
}

int main(void) {
	char empty_path[256];
	char roundtrip_path[256];
	char bad_magic_path[256];
	char truncated_path[256];
	char malformed_path[256];
	int failed = 0;

	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend, sizeof(g_cfg.vector_distance_backend), "%s", "auto");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	snprintf(empty_path, sizeof(empty_path), "tests/hnsw_v2_snapshot_empty.tmp");
	snprintf(roundtrip_path, sizeof(roundtrip_path), "tests/hnsw_v2_snapshot_roundtrip.tmp");
	snprintf(bad_magic_path, sizeof(bad_magic_path), "tests/hnsw_v2_snapshot_bad_magic.tmp");
	snprintf(truncated_path, sizeof(truncated_path), "tests/hnsw_v2_snapshot_truncated.tmp");
	snprintf(malformed_path, sizeof(malformed_path), "tests/hnsw_v2_snapshot_malformed.tmp");
	(void)unlink(empty_path);
	(void)unlink(roundtrip_path);
	(void)unlink(bad_magic_path);
	(void)unlink(truncated_path);
	(void)unlink(malformed_path);

	failed |= test_empty_roundtrip(empty_path);
	failed |= test_small_roundtrip(roundtrip_path);
	failed |= test_invalid_files_do_not_publish(
		roundtrip_path,
		bad_magic_path,
		truncated_path,
		malformed_path
	);

	(void)unlink(empty_path);
	(void)unlink(roundtrip_path);
	(void)unlink(bad_magic_path);
	(void)unlink(truncated_path);
	(void)unlink(malformed_path);

	if (failed) {
		return 1;
	}

	printf("vector_core_hnsw_v2_snapshot_roundtrip_test PASS\n");
	return 0;
}
