/*
 * Benchmark-only prepared-embedding shim for the product RESP server.
 *
 * The special benchmark binary links this file with --wrap for the two public
 * prepare functions. Fixture-tagged requests use the frozen float32 cache;
 * every other request falls through to the normal embedding provider.
 */

#include "kvs_globals.h"
#include "kvs_vector.h"
#include "vector/kvs_vector_hnsw_v2.h"
#include "vector/kvs_vector_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECORD_PREFIX "__kvlite_bench_record__:"
#define QUERY_PREFIX "__kvlite_bench_query__:"

typedef struct benchmark_fixture_map_s {
	const float *records;
	const float *queries;
	size_t records_bytes;
	size_t queries_bytes;
	size_t record_count;
	size_t query_count;
	size_t dim;
	int initialized;
	int init_failed;
} benchmark_fixture_map_t;

static benchmark_fixture_map_t g_fixture;
static pthread_once_t g_fixture_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_control_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_seed_applied;
static char g_last_control[128];
static uint64_t g_prepared_vset_count;
static uint64_t g_prepared_vsearch_count;
static uint64_t g_public_vsearch_count;

int __real_kvs_vector_set_prepare(
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_set_prepared_t *prepared
);

int __real_kvs_vector_search_prepare(
	const char *query,
	size_t query_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_search_prepared_t *prepared
);

static int benchmark_map_file(
	const char *path,
	const float **out_data,
	size_t *out_bytes
) {
	struct stat st;
	void *mapped;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	if (fstat(fd, &st) != 0 || st.st_size <= 0) {
		close(fd);
		return -1;
	}
	mapped = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (mapped == MAP_FAILED) {
		return -1;
	}
	*out_data = (const float *)mapped;
	*out_bytes = (size_t)st.st_size;
	return 0;
}

static void benchmark_fixture_init_once(void) {
	const char *cache_dir = getenv("KVLITE_BENCHMARK_CACHE_DIR");
	char records_path[4096];
	char queries_path[4096];
	size_t row_bytes;

	memset(&g_fixture, 0, sizeof(g_fixture));
	if (!cache_dir || cache_dir[0] == '\0' || g_cfg.embedding_dim <= 0) {
		g_fixture.init_failed = 1;
		return;
	}
	g_fixture.dim = (size_t)g_cfg.embedding_dim;
	if (g_fixture.dim > SIZE_MAX / sizeof(float)) {
		g_fixture.init_failed = 1;
		return;
	}
	row_bytes = g_fixture.dim * sizeof(float);
	if (snprintf(records_path, sizeof(records_path), "%s/embeddings.f32", cache_dir) >=
		(int)sizeof(records_path) ||
	    snprintf(queries_path, sizeof(queries_path), "%s/query_embeddings.f32", cache_dir) >=
		(int)sizeof(queries_path) ||
	    benchmark_map_file(records_path, &g_fixture.records, &g_fixture.records_bytes) != 0 ||
	    benchmark_map_file(queries_path, &g_fixture.queries, &g_fixture.queries_bytes) != 0 ||
	    g_fixture.records_bytes % row_bytes != 0 ||
	    g_fixture.queries_bytes % row_bytes != 0) {
		g_fixture.init_failed = 1;
		return;
	}
	g_fixture.record_count = g_fixture.records_bytes / row_bytes;
	g_fixture.query_count = g_fixture.queries_bytes / row_bytes;
	g_fixture.initialized = 1;
}

static int benchmark_parse_row(
	const char *text,
	size_t text_len,
	const char *prefix,
	size_t *out_row
) {
	size_t prefix_len = strlen(prefix);
	size_t i;
	size_t value = 0;

	if (!text || text_len <= prefix_len ||
	    memcmp(text, prefix, prefix_len) != 0 || !out_row) {
		return 0;
	}
	for (i = prefix_len; i < text_len; i++) {
		unsigned char ch = (unsigned char)text[i];
		if (ch == ':' || ch == '\n') {
			if (i == prefix_len) {
				return 0;
			}
			*out_row = value;
			return 1;
		}
		if (ch < '0' || ch > '9' || value > (SIZE_MAX - 9) / 10) {
			return 0;
		}
		value = value * 10 + (size_t)(ch - '0');
	}
	return 0;
}

static int benchmark_fixture_vector(
	int query,
	size_t row,
	const float **out_vector,
	int *out_dim
) {
	const float *base;
	size_t count;

	pthread_once(&g_fixture_once, benchmark_fixture_init_once);
	if (!g_fixture.initialized || g_fixture.init_failed) {
		return -1;
	}
	base = query ? g_fixture.queries : g_fixture.records;
	count = query ? g_fixture.query_count : g_fixture.record_count;
	if (row >= count) {
		return -1;
	}
	*out_vector = base + row * g_fixture.dim;
	*out_dim = (int)g_fixture.dim;
	return 0;
}

static void benchmark_apply_seed_once(void) {
	const char *value;
	char *end = NULL;
	unsigned long long seed;

	pthread_mutex_lock(&g_control_mu);
	if (g_seed_applied || !g_vector_state.initialized ||
	    g_vector_state.index_type != KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		pthread_mutex_unlock(&g_control_mu);
		return;
	}
	value = getenv("KVLITE_BENCHMARK_HNSW_SEED");
	if (value && value[0] != '\0' && g_vector_state.core_hnsw_v2_index.size == 0) {
		errno = 0;
		seed = strtoull(value, &end, 0);
		if (errno == 0 && end && *end == '\0') {
			kvs_hnsw_v2_index_set_seed(
				&g_vector_state.core_hnsw_v2_index,
				(uint64_t)seed
			);
		}
	}
	kvs_hnsw_v2_index_set_search_profiling(
		&g_vector_state.core_hnsw_v2_index,
		0
	);
	g_seed_applied = 1;
	pthread_mutex_unlock(&g_control_mu);
}

static void benchmark_apply_search_control(void) {
	const char *path = getenv("KVLITE_BENCHMARK_EF_CONTROL_FILE");
	char line[128];
	char *end = NULL;
	unsigned long value;
	FILE *fp;

	if (!path || path[0] == '\0' || !g_vector_state.initialized ||
	    g_vector_state.index_type != KVS_VECTOR_INDEX_CORE_HNSW_V2) {
		return;
	}
	fp = fopen(path, "r");
	if (!fp) {
		return;
	}
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return;
	}
	fclose(fp);

	pthread_mutex_lock(&g_control_mu);
	if (strcmp(line, g_last_control) != 0) {
		errno = 0;
		value = strtoul(line, &end, 10);
		if (errno == 0 && end != line && value > 0) {
			g_vector_state.core_hnsw_v2_index.ef_search = (size_t)value;
			kvs_hnsw_v2_index_set_search_profiling(
				&g_vector_state.core_hnsw_v2_index,
				0
			);
			snprintf(g_last_control, sizeof(g_last_control), "%s", line);
		}
	}
	pthread_mutex_unlock(&g_control_mu);
}

static void benchmark_write_audit(void) {
	const char *path = getenv("KVLITE_BENCHMARK_AUDIT_FILE");
	char temp_path[4096];
	FILE *fp;

	if (!path || path[0] == '\0' ||
	    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld", path,
	             (long)getpid()) >= (int)sizeof(temp_path)) {
		return;
	}
	fp = fopen(temp_path, "w");
	if (!fp) {
		return;
	}
	fprintf(fp,
		"prepared_vset_count=%llu\n"
		"prepared_vsearch_count=%llu\n"
		"public_vsearch_count=%llu\n",
		(unsigned long long)g_prepared_vset_count,
		(unsigned long long)g_prepared_vsearch_count,
		(unsigned long long)g_public_vsearch_count);
	if (fclose(fp) == 0) {
		rename(temp_path, path);
	} else {
		unlink(temp_path);
	}
}

static void benchmark_record_path(uint64_t *counter) {
	int should_flush;

	pthread_mutex_lock(&g_control_mu);
	(*counter)++;
	should_flush = (counter == &g_prepared_vset_count) ?
		(*counter <= 100 || (*counter % 1000) == 0) :
		(*counter <= 10 || (*counter % 200) == 0);
	if (should_flush) {
		benchmark_write_audit();
	}
	pthread_mutex_unlock(&g_control_mu);
}

int __wrap_kvs_vector_set_prepare(
	const char *question,
	size_t question_len,
	const char *answer,
	size_t answer_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_set_prepared_t *prepared
) {
	const float *embedding = NULL;
	size_t row = 0;
	int dim = 0;

	if (benchmark_parse_row(question, question_len, RECORD_PREFIX, &row)) {
		benchmark_apply_seed_once();
		if (benchmark_fixture_vector(0, row, &embedding, &dim) != 0) {
			return KVS_VECTOR_ERR_INVALID_ARG;
		}
		benchmark_record_path(&g_prepared_vset_count);
		return kvs_vector_set_prepare_with_embedding(
			question, question_len, answer, answer_len,
			scope, scope_len, embedding, dim, prepared
		);
	}
	return __real_kvs_vector_set_prepare(
		question, question_len, answer, answer_len,
		scope, scope_len, prepared
	);
}

int __wrap_kvs_vector_search_prepare(
	const char *query,
	size_t query_len,
	const char *scope,
	size_t scope_len,
	kvs_vector_search_prepared_t *prepared
) {
	const float *embedding = NULL;
	size_t row = 0;
	int dim = 0;

	benchmark_apply_search_control();
	if (benchmark_parse_row(query, query_len, QUERY_PREFIX, &row)) {
		if (benchmark_fixture_vector(1, row, &embedding, &dim) != 0) {
			return KVS_VECTOR_ERR_INVALID_ARG;
		}
		benchmark_record_path(&g_prepared_vsearch_count);
		return kvs_vector_search_prepare_with_embedding(
			query, query_len, scope, scope_len,
			embedding, dim, prepared
		);
	}
	benchmark_record_path(&g_public_vsearch_count);
	return __real_kvs_vector_search_prepare(
		query, query_len, scope, scope_len, prepared
	);
}

__attribute__((destructor)) static void benchmark_fixture_destroy(void) {
	pthread_mutex_lock(&g_control_mu);
	benchmark_write_audit();
	pthread_mutex_unlock(&g_control_mu);
	if (g_fixture.records && g_fixture.records_bytes > 0) {
		munmap((void *)g_fixture.records, g_fixture.records_bytes);
	}
	if (g_fixture.queries && g_fixture.queries_bytes > 0) {
		munmap((void *)g_fixture.queries, g_fixture.queries_bytes);
	}
	memset(&g_fixture, 0, sizeof(g_fixture));
}
