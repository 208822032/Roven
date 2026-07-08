#define KVS_VECTOR_INTERNAL
#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"
#include "vector/kvs_vector_hnsw_v2_snapshot.h"
#include "vector/kvs_vector_index_internal.h"
#include "vector/kvs_vector_index_snapshot.h"
#include "vector/kvs_vector_manager.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern kvs_config_t g_cfg;

#define BENCH_RECORDS_PATH "vector_index.records"
#define BENCH_RECORDS_TMP_PATH "vector_index.records.tmp"
#define BENCH_RECORDS_MAGIC "KVLITE_HNSW_S_RECORDS"
#define BENCH_RECORDS_MAGIC_SIZE 32
#define BENCH_RECORDS_VERSION 1u
#define BENCH_DEFAULT_COUNT 100000u
#define BENCH_QUICK_COUNT 10000u
#define BENCH_DIM 512u
#define BENCH_TOPK 10u
#define BENCH_DEFAULT_QUERIES 50u
#define BENCH_QUICK_QUERIES 10u
#define BENCH_BASE_RECORD_ID 1000000ULL
#define BENCH_VECTOR_SEED 0xabc00000ULL
#define BENCH_BASELINE_REBUILD_US 3011810795ULL

typedef struct bench_records_header_s {
	char magic[BENCH_RECORDS_MAGIC_SIZE];
	uint32_t version;
	uint32_t dim;
	uint32_t metric;
	uint32_t m;
	uint32_t m0;
	uint32_t ef_construction;
	uint32_t ef_search;
	uint32_t topk;
	uint32_t reserved;
	uint64_t count;
	uint64_t base_record_id;
	uint64_t vector_seed;
} bench_records_header_t;

typedef struct bench_load_timing_s {
	uint64_t records_restore_us;
	uint64_t snapshot_load_us;
	uint64_t snapshot_core_load_us;
	uint64_t snapshot_validate_us;
	uint64_t snapshot_deserialize_us;
	uint64_t bind_us;
	int post_load_search_ok;
	int loaded_from_snapshot;
	uint64_t fallback_rebuild_count;
} bench_load_timing_t;

static uint64_t bench_now_us(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static uint32_t bench_rng_next(uint64_t *state) {
	uint64_t z;

	*state += 0x9e3779b97f4a7c15ULL;
	z = *state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return (uint32_t)(z >> 32);
}

static void bench_fill_random_unit_vector(
	float *vector,
	size_t dim,
	uint64_t seed
) {
	uint64_t state = seed;
	float norm = 0.0f;
	size_t i;

	for (i = 0; i < dim; i++) {
		uint32_t raw = bench_rng_next(&state);
		float value = (float)((int)(raw % 20001u) - 10000) / 10000.0f;
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

static size_t bench_env_size_or(const char *name, size_t fallback) {
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long long parsed;

	if (!value || value[0] == '\0') {
		return fallback;
	}
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno != 0 || !end || *end != '\0' || parsed == 0ULL) {
		return fallback;
	}
	return (size_t)parsed;
}

static uint64_t bench_env_u64_or(const char *name, uint64_t fallback) {
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long long parsed;

	if (!value || value[0] == '\0') {
		return fallback;
	}
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno != 0 || !end || *end != '\0' || parsed == 0ULL) {
		return fallback;
	}
	return (uint64_t)parsed;
}

static void bench_set_text(char *dst, size_t dst_size, const char *text) {
	if (!dst || dst_size == 0) {
		return;
	}
	snprintf(dst, dst_size, "%s", text ? text : "");
}

static int bench_file_size(const char *path, uint64_t *out) {
	struct stat st;

	if (!path || !out || stat(path, &st) != 0 || st.st_size < 0) {
		return KVS_VECTOR_ERR;
	}
	*out = (uint64_t)st.st_size;
	return KVS_VECTOR_OK;
}

static void bench_reset_config(size_t dim, size_t count) {
	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	g_cfg.embedding_dim = (int)dim;
	g_cfg.vector_score_threshold = -1.0f;
	g_cfg.vector_hnsw_m = KVS_HNSW_V2_DEFAULT_M;
	g_cfg.vector_hnsw_ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	g_cfg.vector_hnsw_ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	g_cfg.vector_hnsw_initial_capacity =
		count > (size_t)INT_MAX ? INT_MAX : (int)count;
	snprintf(
		g_cfg.vector_index_type,
		sizeof(g_cfg.vector_index_type),
		"%s",
		"core_hnsw_v2"
	);
	snprintf(g_cfg.vector_metric, sizeof(g_cfg.vector_metric), "%s", "ip");
	snprintf(
		g_cfg.vector_distance_backend,
		sizeof(g_cfg.vector_distance_backend),
		"%s",
		"auto"
	);
}

static int bench_init_vector_manager(size_t dim, size_t count) {
	size_t reserve_capacity = count > 0 ? count : 1;
	int ret;

	bench_reset_config(dim, count);
	ret = kvs_vector_init();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	g_vector_state.embedding_provider.dimension = dim;

	kvs_flat_index_destroy(&g_vector_state.core_flat_index);
	kvs_hnsw_v2_index_destroy(&g_vector_state.core_hnsw_v2_index);
	ret = kvs_flat_index_init(
		&g_vector_state.core_flat_index,
		dim,
		KVS_VECTOR_METRIC_IP
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_hnsw_v2_index_init(
		&g_vector_state.core_hnsw_v2_index,
		dim,
		KVS_VECTOR_METRIC_IP
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	g_vector_state.core_hnsw_v2_index.M = KVS_HNSW_V2_DEFAULT_M;
	g_vector_state.core_hnsw_v2_index.M0 = KVS_HNSW_V2_DEFAULT_M0;
	g_vector_state.core_hnsw_v2_index.ef_construction =
		KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	g_vector_state.core_hnsw_v2_index.ef_search =
		KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	kvs_hnsw_v2_index_set_seed(
		&g_vector_state.core_hnsw_v2_index,
		0x9d000000ULL + (uint64_t)count
	);
	ret = kvs_vector_index_bind_current();
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_store_reserve(&g_vector_state.store, reserve_capacity);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_id_map_reserve(&g_vector_state.id_map, reserve_capacity * 2 + 1);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	ret = kvs_vector_record_id_map_reserve(
		&g_vector_state.store.record_id_map,
		reserve_capacity * 2 + 1
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	return kvs_hnsw_v2_index_reserve(
		&g_vector_state.core_hnsw_v2_index,
		reserve_capacity
	);
}

static void bench_make_record_text(
	uint64_t record_id,
	char *question,
	size_t question_size,
	char *answer,
	size_t answer_size
) {
	snprintf(question, question_size, "bench-record-%llu",
	         (unsigned long long)record_id);
	snprintf(answer, answer_size, "answer-%llu",
	         (unsigned long long)record_id);
}

static int bench_restore_one_record(
	size_t record_index,
	uint64_t record_id,
	const float *vector,
	size_t dim,
	uint64_t generation
) {
	char question[64];
	char answer[64];
	const char scope[] = "bench";

	bench_make_record_text(
		record_id,
		question,
		sizeof(question),
		answer,
		sizeof(answer)
	);

	return kvs_vector_restore_record_with_embedding(
		record_index,
		record_id,
		KVS_VECTOR_RECORD_ACTIVE,
		0,
		question,
		strlen(question),
		answer,
		strlen(answer),
		scope,
		strlen(scope),
		question,
		strlen(question),
		vector,
		(int)dim,
		(int64_t)generation,
		(int64_t)generation,
		0,
		generation,
		1
	);
}

static int bench_write_records_fixture(
	FILE *fp,
	size_t count,
	size_t dim
) {
	bench_records_header_t header;
	float *vector;
	size_t i;

	memset(&header, 0, sizeof(header));
	snprintf(header.magic, sizeof(header.magic), "%s", BENCH_RECORDS_MAGIC);
	header.version = BENCH_RECORDS_VERSION;
	header.dim = (uint32_t)dim;
	header.metric = (uint32_t)KVS_VECTOR_METRIC_IP;
	header.m = (uint32_t)KVS_HNSW_V2_DEFAULT_M;
	header.m0 = (uint32_t)KVS_HNSW_V2_DEFAULT_M0;
	header.ef_construction = (uint32_t)KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	header.ef_search = (uint32_t)KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	header.topk = BENCH_TOPK;
	header.count = (uint64_t)count;
	header.base_record_id = BENCH_BASE_RECORD_ID;
	header.vector_seed = BENCH_VECTOR_SEED;

	vector = (float *)malloc(dim * sizeof(*vector));
	if (!vector) {
		return KVS_VECTOR_ERR;
	}
	if (fwrite(&header, sizeof(header), 1, fp) != 1) {
		free(vector);
		return KVS_VECTOR_ERR;
	}

	for (i = 0; i < count; i++) {
		uint64_t record_id = BENCH_BASE_RECORD_ID + (uint64_t)i;
		uint64_t generation = (uint64_t)i + 1ULL;

		bench_fill_random_unit_vector(
			vector,
			dim,
			BENCH_VECTOR_SEED + (uint64_t)i
		);
		if (bench_restore_one_record(i, record_id, vector, dim, generation) !=
		    KVS_VECTOR_OK ||
		    fwrite(&record_id, sizeof(record_id), 1, fp) != 1 ||
		    fwrite(&generation, sizeof(generation), 1, fp) != 1 ||
		    fwrite(vector, dim * sizeof(*vector), 1, fp) != 1) {
			free(vector);
			return KVS_VECTOR_ERR;
		}
	}

	free(vector);
	return KVS_VECTOR_OK;
}

static int bench_read_records_header(
	FILE *fp,
	bench_records_header_t *header
) {
	if (!fp || !header || fread(header, sizeof(*header), 1, fp) != 1) {
		return KVS_VECTOR_ERR;
	}
	if (strncmp(header->magic, BENCH_RECORDS_MAGIC, sizeof(header->magic)) != 0 ||
	    header->version != BENCH_RECORDS_VERSION ||
	    header->dim == 0 ||
	    header->count == 0 ||
	    header->base_record_id != BENCH_BASE_RECORD_ID ||
	    header->vector_seed != BENCH_VECTOR_SEED ||
	    header->metric != (uint32_t)KVS_VECTOR_METRIC_IP ||
	    header->m != (uint32_t)KVS_HNSW_V2_DEFAULT_M ||
	    header->m0 != (uint32_t)KVS_HNSW_V2_DEFAULT_M0 ||
	    header->ef_construction !=
		    (uint32_t)KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION ||
	    header->ef_search != (uint32_t)KVS_HNSW_V2_DEFAULT_EF_SEARCH) {
		return KVS_VECTOR_ERR;
	}
	return KVS_VECTOR_OK;
}

static int bench_restore_records_from_fixture(
	size_t requested_count,
	bench_records_header_t *out_header
) {
	bench_records_header_t header;
	float *vector = NULL;
	FILE *fp;
	size_t i;
	int ret = KVS_VECTOR_ERR;

	fp = fopen(BENCH_RECORDS_PATH, "rb");
	if (!fp) {
		fprintf(stderr, "hnsw_s_bench_load_only_fail reason=records_fixture_missing path=%s\n",
		        BENCH_RECORDS_PATH);
		return KVS_VECTOR_ERR_NOT_FOUND;
	}
	if (bench_read_records_header(fp, &header) != KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_s_bench_load_only_fail reason=records_fixture_invalid\n");
		goto done;
	}
	if (requested_count != 0 && requested_count != (size_t)header.count) {
		fprintf(stderr,
		        "hnsw_s_bench_load_only_fail reason=count_mismatch requested=%zu fixture=%llu\n",
		        requested_count,
		        (unsigned long long)header.count);
		goto done;
	}
	if (bench_init_vector_manager((size_t)header.dim, (size_t)header.count) !=
	    KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_s_bench_load_only_fail reason=vector_init_failed\n");
		goto done;
	}

	vector = (float *)malloc((size_t)header.dim * sizeof(*vector));
	if (!vector) {
		goto done_destroy;
	}

	for (i = 0; i < (size_t)header.count; i++) {
		uint64_t record_id = 0;
		uint64_t generation = 0;

		if (fread(&record_id, sizeof(record_id), 1, fp) != 1 ||
		    fread(&generation, sizeof(generation), 1, fp) != 1 ||
		    fread(vector, (size_t)header.dim * sizeof(*vector), 1, fp) != 1 ||
		    record_id != BENCH_BASE_RECORD_ID + (uint64_t)i ||
		    generation != (uint64_t)i + 1ULL ||
		    bench_restore_one_record(
			    i,
			    record_id,
			    vector,
			    (size_t)header.dim,
			    generation
		    ) != KVS_VECTOR_OK) {
			fprintf(stderr, "hnsw_s_bench_load_only_fail reason=record_restore_failed i=%zu\n",
			        i);
			goto done_destroy;
		}
	}
	if (out_header) {
		*out_header = header;
	}
	ret = KVS_VECTOR_OK;
	goto done;

done_destroy:
	kvs_vector_destroy();
done:
	free(vector);
	fclose(fp);
	return ret;
}

static int bench_hits_descending(const kvs_vector_hit_t *hits, size_t count) {
	size_t i;

	for (i = 1; i < count; i++) {
		if (hits[i - 1].score < hits[i].score) {
			return 0;
		}
	}
	return 1;
}

static size_t bench_query_index(size_t q, size_t count, size_t queries) {
	if (queries == 0 || count == 0) {
		return 0;
	}
	return (q * count) / queries;
}

static int bench_search_sanity(
	size_t count,
	size_t queries,
	size_t *out_checked,
	size_t *out_matched
) {
	kvs_vector_hit_t hits[BENCH_TOPK];
	float query[BENCH_DIM];
	size_t checked = 0;
	size_t matched = 0;
	size_t q;

	if (queries == 0) {
		queries = 1;
	}
	if (queries > count) {
		queries = count;
	}

	for (q = 0; q < queries; q++) {
		size_t idx = bench_query_index(q, count, queries);
		uint64_t expected_id = BENCH_BASE_RECORD_ID + (uint64_t)idx;
		size_t hit_count = 0;
		size_t i;
		int found = 0;

		if (idx >= count) {
			idx = count - 1;
			expected_id = BENCH_BASE_RECORD_ID + (uint64_t)idx;
		}
		bench_fill_random_unit_vector(
			query,
			BENCH_DIM,
			BENCH_VECTOR_SEED + (uint64_t)idx
		);
		if (kvs_hnsw_v2_index_search(
			    &g_vector_state.core_hnsw_v2_index,
			    query,
			    BENCH_DIM,
			    BENCH_TOPK,
			    KVS_HNSW_V2_DEFAULT_EF_SEARCH,
			    hits,
			    &hit_count
		    ) != KVS_VECTOR_OK ||
		    hit_count == 0 ||
		    !bench_hits_descending(hits, hit_count)) {
			return KVS_VECTOR_ERR;
		}
		for (i = 0; i < hit_count; i++) {
			if (hits[i].record_id == expected_id) {
				found = 1;
				break;
			}
		}
		checked++;
		if (found) {
			matched++;
		}
	}

	if (out_checked) {
		*out_checked = checked;
	}
	if (out_matched) {
		*out_matched = matched;
	}
	return checked == matched ? KVS_VECTOR_OK : KVS_VECTOR_ERR;
}

static int bench_validate_loaded_records(char *reason, size_t reason_size) {
	kvs_hnsw_v2_index_t *hnsw = &g_vector_state.core_hnsw_v2_index;
	uint64_t active_slots = 0;
	size_t i;

	if (!hnsw->ready || hnsw->dim == 0) {
		bench_set_text(reason, reason_size, "loaded index not ready");
		return KVS_VECTOR_ERR;
	}

	for (i = 0; i < hnsw->size; i++) {
		kvs_vector_record_t *record = NULL;
		const float *vector;
		uint64_t record_id;
		size_t vector_bytes;

		if (hnsw->deleted_flags && hnsw->deleted_flags[i]) {
			continue;
		}
		active_slots++;
		record_id = hnsw->slot_to_record_id[i];
		if (kvs_vector_manager_get_record_by_id(record_id, &record) !=
		    KVS_VECTOR_OK ||
		    !record || !record->embedding ||
		    record->embedding_dim != (int)hnsw->dim) {
			bench_set_text(
				reason,
				reason_size,
				"loaded record_id not in manager records"
			);
			return KVS_VECTOR_ERR;
		}
		vector = kvs_hnsw_v2_slot_vector(hnsw, (uint32_t)i);
		vector_bytes = hnsw->dim * sizeof(float);
		if (!vector || memcmp(vector, record->embedding, vector_bytes) != 0) {
			bench_set_text(reason, reason_size, "loaded vector mismatch");
			return KVS_VECTOR_ERR;
		}
	}

	if (active_slots != (uint64_t)g_vector_state.store.active_records) {
		bench_set_text(reason, reason_size, "loaded active count mismatch");
		return KVS_VECTOR_ERR;
	}

	bench_set_text(reason, reason_size, "ok");
	return KVS_VECTOR_OK;
}

static int bench_load_snapshot_no_fallback(bench_load_timing_t *timing) {
	kvs_vector_index_snapshot_manifest_t actual;
	kvs_vector_index_snapshot_manifest_t expected;
	kvs_vector_snapshot_status_t *status = &g_vector_state.snapshot_status;
	char manifest_path[512];
	char snapshot_path[512];
	char reason[128];
	uint64_t start_us;
	uint64_t validate_start_us;
	uint64_t deserialize_start_us;
	uint64_t bind_start_us;
	int ret;

	if (!timing) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	memset(timing, 0, sizeof(*timing));
	memset(status, 0, sizeof(*status));
	bench_set_text(reason, sizeof(reason), "ok");

	if (kvs_vector_index_snapshot_default_manifest_path(
		    manifest_path,
		    sizeof(manifest_path)
	    ) != 0 ||
	    kvs_vector_index_snapshot_default_snapshot_path(
		    snapshot_path,
		    sizeof(snapshot_path)
	    ) != 0) {
		bench_set_text(status->last_snapshot_error,
		               sizeof(status->last_snapshot_error),
		               "snapshot path resolution failed");
		return KVS_VECTOR_ERR;
	}

	status->snapshot_load_attempted = 1;
	bench_set_text(status->last_snapshot_manifest_path,
	               sizeof(status->last_snapshot_manifest_path),
	               manifest_path);
	bench_set_text(status->last_snapshot_path,
	               sizeof(status->last_snapshot_path),
	               snapshot_path);

	start_us = bench_now_us();
	validate_start_us = start_us;
	ret = kvs_vector_index_snapshot_manifest_read_file(manifest_path, &actual);
	if (ret != 0) {
		bench_set_text(status->last_snapshot_error,
		               sizeof(status->last_snapshot_error),
		               "manifest missing or unreadable");
		return KVS_VECTOR_ERR_NOT_FOUND;
	}
	ret = kvs_vector_index_snapshot_build_expected_manifest_after_records(
		&g_vector_state,
		snapshot_path,
		&expected,
		reason,
		sizeof(reason)
	);
	if (ret != KVS_VECTOR_OK) {
		bench_set_text(status->last_snapshot_error,
		               sizeof(status->last_snapshot_error), reason);
		return ret;
	}
	ret = kvs_vector_index_snapshot_manifest_validate(
		&actual,
		&expected,
		reason,
		sizeof(reason)
	);
	timing->snapshot_validate_us = bench_now_us() - validate_start_us;
	if (ret != 0) {
		bench_set_text(status->last_snapshot_error,
		               sizeof(status->last_snapshot_error), reason);
		return KVS_VECTOR_ERR;
	}
	status->snapshot_valid = 1;

	deserialize_start_us = bench_now_us();
	ret = kvs_hnsw_v2_index_deserialize_file(
		&g_vector_state.core_hnsw_v2_index,
		snapshot_path
	);
	if (ret != KVS_VECTOR_OK) {
		bench_set_text(status->last_snapshot_error,
		               sizeof(status->last_snapshot_error),
		               "snapshot deserialize failed");
		return ret;
	}
	ret = kvs_hnsw_v2_index_validate_basic(
		&g_vector_state.core_hnsw_v2_index,
		reason,
		sizeof(reason)
	);
	if (ret == KVS_VECTOR_OK) {
		ret = bench_validate_loaded_records(reason, sizeof(reason));
	}
	timing->snapshot_deserialize_us = bench_now_us() - deserialize_start_us;
	if (ret != KVS_VECTOR_OK) {
		bench_set_text(status->last_snapshot_error,
		               sizeof(status->last_snapshot_error), reason);
		return ret;
	}

	bind_start_us = bench_now_us();
	ret = kvs_vector_index_bind_current();
	timing->bind_us = bench_now_us() - bind_start_us;
	if (ret != KVS_VECTOR_OK) {
		bench_set_text(status->last_snapshot_error,
		               sizeof(status->last_snapshot_error),
		               "bind loaded snapshot failed");
		return ret;
	}

	status->snapshot_loaded = 1;
	status->loaded_from_snapshot = 1;
	bench_set_text(status->last_snapshot_error,
	               sizeof(status->last_snapshot_error), "ok");
	timing->snapshot_core_load_us = bench_now_us() - start_us;
	timing->loaded_from_snapshot = status->loaded_from_snapshot;
	timing->fallback_rebuild_count = status->fallback_rebuild_count;
	return KVS_VECTOR_OK;
}

static int bench_cmp_u64(const void *lhs, const void *rhs) {
	uint64_t a = *(const uint64_t *)lhs;
	uint64_t b = *(const uint64_t *)rhs;

	return (a > b) - (a < b);
}

static uint64_t bench_percentile_u64(
	const uint64_t *values,
	size_t count,
	double pct
) {
	uint64_t *copy;
	uint64_t out;
	size_t idx;

	if (!values || count == 0) {
		return 0;
	}
	copy = (uint64_t *)malloc(count * sizeof(*copy));
	if (!copy) {
		return 0;
	}
	memcpy(copy, values, count * sizeof(*copy));
	qsort(copy, count, sizeof(*copy), bench_cmp_u64);
	idx = (size_t)((pct / 100.0) * (double)count);
	if (idx >= count) {
		idx = count - 1;
	}
	out = copy[idx];
	free(copy);
	return out;
}

static double bench_avg_u64(const uint64_t *values, size_t count) {
	uint64_t total = 0;
	size_t i;

	if (!values || count == 0) {
		return 0.0;
	}
	for (i = 0; i < count; i++) {
		total += values[i];
	}
	return (double)total / (double)count;
}

static int bench_run_create_fixture(size_t count, size_t queries) {
	uint64_t build_start_us;
	uint64_t fixture_create_build_us;
	uint64_t snapshot_size = 0;
	uint64_t manifest_size = 0;
	uint64_t records_size = 0;
	size_t sanity_checked = 0;
	size_t sanity_matched = 0;
	char reason[128];
	FILE *fp;
	int ret = KVS_VECTOR_ERR;

	printf(
		"hnsw_s_bench_start mode=create-fixture count=%zu dim=%u "
		"metric=ip_normalized M=%u M0=%u efConstruction=%u efSearch=%u "
		"topK=%u queries=%zu\n",
		count,
		BENCH_DIM,
		KVS_HNSW_V2_DEFAULT_M,
		KVS_HNSW_V2_DEFAULT_M0,
		KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION,
		KVS_HNSW_V2_DEFAULT_EF_SEARCH,
		BENCH_TOPK,
		queries
	);
	fflush(stdout);

	(void)unlink(BENCH_RECORDS_TMP_PATH);
	fp = fopen(BENCH_RECORDS_TMP_PATH, "wb");
	if (!fp) {
		fprintf(stderr, "hnsw_s_bench_create_fail reason=records_open_failed\n");
		return KVS_VECTOR_ERR;
	}

	if (bench_init_vector_manager(BENCH_DIM, count) != KVS_VECTOR_OK ||
	    bench_write_records_fixture(fp, count, BENCH_DIM) != KVS_VECTOR_OK ||
	    fflush(fp) != 0 ||
	    ferror(fp)) {
		fprintf(stderr, "hnsw_s_bench_create_fail reason=records_fixture_write_failed\n");
		fclose(fp);
		(void)unlink(BENCH_RECORDS_TMP_PATH);
		kvs_vector_destroy();
		return KVS_VECTOR_ERR;
	}
	if (fclose(fp) != 0) {
		fprintf(stderr, "hnsw_s_bench_create_fail reason=records_fixture_close_failed\n");
		(void)unlink(BENCH_RECORDS_TMP_PATH);
		kvs_vector_destroy();
		return KVS_VECTOR_ERR;
	}
	fp = NULL;
	if (rename(BENCH_RECORDS_TMP_PATH, BENCH_RECORDS_PATH) != 0) {
		fprintf(stderr, "hnsw_s_bench_create_fail reason=records_publish_failed\n");
		kvs_vector_destroy();
		return KVS_VECTOR_ERR;
	}

	if (kvs_vector_id_map_rebuild_from_store() != KVS_VECTOR_OK ||
	    kvs_vector_index_bind_current() != KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_s_bench_create_fail reason=bind_before_rebuild_failed\n");
		goto done;
	}

	build_start_us = bench_now_us();
	ret = kvs_vector_index_rebuild_current();
	fixture_create_build_us = bench_now_us() - build_start_us;
	if (ret != KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_s_bench_create_fail reason=rebuild_current_failed ret=%d\n",
		        ret);
		goto done;
	}

	reason[0] = '\0';
	ret = kvs_vector_index_snapshot_write_current(
		&g_vector_state,
		reason,
		sizeof(reason)
	);
	if (ret != KVS_VECTOR_OK) {
		fprintf(stderr, "hnsw_s_bench_create_fail reason=snapshot_write_failed detail=%s\n",
		        reason[0] ? reason : "unknown");
		goto done;
	}
	ret = bench_search_sanity(count, queries, &sanity_checked, &sanity_matched);
	if (ret != KVS_VECTOR_OK) {
		fprintf(stderr,
		        "hnsw_s_bench_create_fail reason=post_build_search_sanity_failed checked=%zu matched=%zu\n",
		        sanity_checked,
		        sanity_matched);
		goto done;
	}

	(void)bench_file_size("vector_index.snapshot", &snapshot_size);
	(void)bench_file_size("vector_index.manifest", &manifest_size);
	(void)bench_file_size(BENCH_RECORDS_PATH, &records_size);

	printf(
		"hnsw_s_bench_create_fixture count=%zu dim=%u metric=ip_normalized "
		"M=%u M0=%u efConstruction=%u efSearch=%u topK=%u queries=%zu "
		"fixture_create_build_us=%llu snapshot_file_size_bytes=%llu "
		"manifest_file_size_bytes=%llu records_fixture_size_bytes=%llu "
		"post_build_search_ok=%d topk_match_checked=%zu topk_match_count=%zu "
		"memory_bytes=%zu duplicate_edge=%llu degree_overflow=%llu\n",
		count,
		BENCH_DIM,
		KVS_HNSW_V2_DEFAULT_M,
		KVS_HNSW_V2_DEFAULT_M0,
		KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION,
		KVS_HNSW_V2_DEFAULT_EF_SEARCH,
		BENCH_TOPK,
		queries,
		(unsigned long long)fixture_create_build_us,
		(unsigned long long)snapshot_size,
		(unsigned long long)manifest_size,
		(unsigned long long)records_size,
		sanity_checked == sanity_matched ? 1 : 0,
		sanity_checked,
		sanity_matched,
		g_vector_state.core_hnsw_v2_index.memory.total_bytes,
		(unsigned long long)g_vector_state.core_hnsw_v2_index.graph_stats.duplicate_edge_count,
		(unsigned long long)g_vector_state.core_hnsw_v2_index.graph_stats.degree_overflow_count
	);

done:
	kvs_vector_destroy();
	return ret;
}

static int bench_run_load_only(
	size_t requested_count,
	size_t repeat,
	size_t queries,
	uint64_t baseline_rebuild_us
) {
	uint64_t *load_us = NULL;
	uint64_t *restore_us = NULL;
	uint64_t *validate_us = NULL;
	uint64_t *deserialize_us = NULL;
	uint64_t *bind_us = NULL;
	bench_records_header_t header;
	uint64_t snapshot_size = 0;
	uint64_t manifest_size = 0;
	size_t i;
	size_t count = requested_count;
	int failed = 0;

	if (repeat == 0) {
		repeat = 1;
	}

	load_us = (uint64_t *)calloc(repeat, sizeof(*load_us));
	restore_us = (uint64_t *)calloc(repeat, sizeof(*restore_us));
	validate_us = (uint64_t *)calloc(repeat, sizeof(*validate_us));
	deserialize_us = (uint64_t *)calloc(repeat, sizeof(*deserialize_us));
	bind_us = (uint64_t *)calloc(repeat, sizeof(*bind_us));
	if (!load_us || !restore_us || !validate_us || !deserialize_us || !bind_us) {
		failed = 1;
		goto done;
	}

	for (i = 0; i < repeat; i++) {
		bench_load_timing_t timing;
		kvs_vector_info_t info;
		size_t sanity_checked = 0;
		size_t sanity_matched = 0;
		uint64_t restore_start_us;
		uint64_t records_restore_us;

		restore_start_us = bench_now_us();
		if (bench_restore_records_from_fixture(requested_count, &header) !=
		    KVS_VECTOR_OK) {
			failed = 1;
			goto done;
		}
		records_restore_us = bench_now_us() - restore_start_us;
		count = (size_t)header.count;

		if (kvs_vector_id_map_rebuild_from_store() != KVS_VECTOR_OK ||
		    kvs_vector_index_bind_current() != KVS_VECTOR_OK ||
		    bench_load_snapshot_no_fallback(&timing) != KVS_VECTOR_OK) {
			fprintf(stderr, "hnsw_s_bench_load_only_fail reason=snapshot_load_no_fallback_failed iter=%zu\n",
			        i + 1);
			kvs_vector_destroy();
			failed = 1;
			goto done;
		}

		timing.records_restore_us = records_restore_us;
		timing.snapshot_load_us =
			timing.records_restore_us + timing.snapshot_core_load_us;
		timing.post_load_search_ok =
			bench_search_sanity(count, queries, &sanity_checked, &sanity_matched) ==
			KVS_VECTOR_OK;
		memset(&info, 0, sizeof(info));
		if (kvs_vector_info(&info) != KVS_VECTOR_OK) {
			timing.loaded_from_snapshot = 0;
		} else {
			timing.loaded_from_snapshot = info.loaded_from_snapshot;
			timing.fallback_rebuild_count = info.snapshot_fallback_rebuild_count;
		}

		load_us[i] = timing.snapshot_load_us;
		restore_us[i] = timing.records_restore_us;
		validate_us[i] = timing.snapshot_validate_us;
		deserialize_us[i] = timing.snapshot_deserialize_us;
		bind_us[i] = timing.bind_us;

		printf(
			"hnsw_s_bench_load_iter iter=%zu count=%zu dim=%u "
			"records_restore_us=%llu snapshot_load_us=%llu "
			"snapshot_core_load_us=%llu snapshot_validate_us=%llu "
			"snapshot_deserialize_us=%llu bind_us=%llu "
			"post_load_search_ok=%d topk_match_checked=%zu topk_match_count=%zu "
			"VINFO_loaded_from_snapshot=%d fallback_rebuild_count=%llu\n",
			i + 1,
			count,
			BENCH_DIM,
			(unsigned long long)timing.records_restore_us,
			(unsigned long long)timing.snapshot_load_us,
			(unsigned long long)timing.snapshot_core_load_us,
			(unsigned long long)timing.snapshot_validate_us,
			(unsigned long long)timing.snapshot_deserialize_us,
			(unsigned long long)timing.bind_us,
			timing.post_load_search_ok,
			sanity_checked,
			sanity_matched,
			timing.loaded_from_snapshot,
			(unsigned long long)timing.fallback_rebuild_count
		);

		if (!timing.post_load_search_ok ||
		    timing.loaded_from_snapshot != 1 ||
		    timing.fallback_rebuild_count != 0) {
			failed = 1;
		}
		kvs_vector_destroy();
		if (failed) {
			goto done;
		}
	}

	(void)bench_file_size("vector_index.snapshot", &snapshot_size);
	(void)bench_file_size("vector_index.manifest", &manifest_size);
	{
		double avg = bench_avg_u64(load_us, repeat);
		double speedup = avg > 0.0 ? (double)baseline_rebuild_us / avg : 0.0;
		double saved = avg > 0.0 && baseline_rebuild_us > (uint64_t)avg ?
			(double)baseline_rebuild_us - avg : 0.0;
		double saved_percent = baseline_rebuild_us > 0 ?
			(saved * 100.0) / (double)baseline_rebuild_us : 0.0;

		printf(
			"hnsw_s_bench_summary mode=load-only count=%zu dim=%u "
			"metric=ip_normalized M=%u M0=%u efConstruction=%u efSearch=%u "
			"topK=%u queries=%zu repeat=%zu records_only_rebuild_us=%llu "
			"baseline_source=reused_from_HNSW_P2 snapshot_file_size_bytes=%llu "
			"manifest_file_size_bytes=%llu records_restore_us_avg=%.2f "
			"snapshot_load_us_avg=%.2f snapshot_load_us_p50=%llu "
			"snapshot_load_us_p95=%llu snapshot_validate_us_avg=%.2f "
			"snapshot_deserialize_us_avg=%.2f bind_us_avg=%.2f "
			"speedup=%.2f cold_start_saved_time_us=%.2f "
			"cold_start_saved_time_percent=%.2f post_load_search_ok=1 "
			"VINFO_loaded_from_snapshot=1\n",
			count,
			BENCH_DIM,
			KVS_HNSW_V2_DEFAULT_M,
			KVS_HNSW_V2_DEFAULT_M0,
			KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION,
			KVS_HNSW_V2_DEFAULT_EF_SEARCH,
			BENCH_TOPK,
			queries,
			repeat,
			(unsigned long long)baseline_rebuild_us,
			(unsigned long long)snapshot_size,
			(unsigned long long)manifest_size,
			bench_avg_u64(restore_us, repeat),
			avg,
			(unsigned long long)bench_percentile_u64(load_us, repeat, 50.0),
			(unsigned long long)bench_percentile_u64(load_us, repeat, 95.0),
			bench_avg_u64(validate_us, repeat),
			bench_avg_u64(deserialize_us, repeat),
			bench_avg_u64(bind_us, repeat),
			speedup,
			saved,
			saved_percent
		);
		printf(
			"HNSW-S-bench-0 decision: production_code_changed=no "
			"benchmark_added=yes load_only_skips_fallback_rebuild=yes "
			"snapshot_benefit_proven=%s ready_for_HNSW_R=%s next_stage=%s\n",
			speedup > 1.0 ? "yes" : "no",
			speedup > 1.0 ? "yes" : "no",
			speedup > 1.0 ? "HNSW-R" : "benchmark-followup"
		);
	}

done:
	free(load_us);
	free(restore_us);
	free(validate_us);
	free(deserialize_us);
	free(bind_us);
	return failed ? KVS_VECTOR_ERR : KVS_VECTOR_OK;
}

int main(void) {
	const char *mode = getenv("HNSW_S_BENCH_MODE");
	uint64_t baseline_us = bench_env_u64_or(
		"HNSW_S_BENCH_BASELINE_REBUILD_US",
		BENCH_BASELINE_REBUILD_US
	);
	int ret = KVS_VECTOR_ERR;

	if (!mode || mode[0] == '\0') {
		mode = "quick";
	}

	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	mem_init();

	if (strcmp(mode, "create-fixture") == 0) {
		size_t count = bench_env_size_or("HNSW_S_BENCH_COUNT", BENCH_DEFAULT_COUNT);
		size_t queries = bench_env_size_or("HNSW_S_BENCH_QUERIES",
		                                    BENCH_DEFAULT_QUERIES);
		ret = bench_run_create_fixture(count, queries);
	} else if (strcmp(mode, "load-only") == 0) {
		size_t count = bench_env_size_or("HNSW_S_BENCH_COUNT", 0);
		size_t repeat = bench_env_size_or("HNSW_S_BENCH_REPEAT", 5);
		size_t queries = bench_env_size_or("HNSW_S_BENCH_QUERIES",
		                                    BENCH_DEFAULT_QUERIES);
		ret = bench_run_load_only(count, repeat, queries, baseline_us);
	} else if (strcmp(mode, "quick") == 0) {
		size_t count = bench_env_size_or("HNSW_S_BENCH_COUNT", BENCH_QUICK_COUNT);
		size_t queries = bench_env_size_or("HNSW_S_BENCH_QUERIES",
		                                    BENCH_QUICK_QUERIES);
		if (bench_run_create_fixture(count, queries) == KVS_VECTOR_OK) {
			ret = bench_run_load_only(count, 1, queries, baseline_us);
		}
	} else {
		fprintf(stderr, "hnsw_s_bench_fail reason=unknown_mode mode=%s\n", mode);
		ret = KVS_VECTOR_ERR_INVALID_ARG;
	}

	mem_destroy();
	if (ret != KVS_VECTOR_OK) {
		fprintf(stderr, "vector_index_snapshot_cold_start_bench FAILED\n");
		return 1;
	}
	printf("vector_index_snapshot_cold_start_bench PASS\n");
	return 0;
}
