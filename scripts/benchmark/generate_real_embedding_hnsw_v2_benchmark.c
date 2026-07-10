/*
 * B3-core-hnsw-v2-index-only-benchmark.
 *
 * Consumes the B1-frozen real embedding cache and the B2-frozen core_flat ground
 * truth, builds a core_hnsw_v2 index with the FROZEN DEFAULT params (M/M0/
 * efConstruction/efSearch), runs top-K IP search over all queries, and reports
 * recall (vs B2 ground truth), latency, QPS, build time and index stats.
 *
 * INDEX-ONLY: uses only the kvs_hnsw_v2_index_* core API (same layer as
 * core_flat). No manager, no g_vector_state, no RESP, no ONNX, no ground-truth
 * regeneration, no param sweep, no build optimization.
 *
 * Subcommands:
 *   generate  - verify B1 cache + B2 ground truth (schema + checksums), build
 *               core_hnsw_v2, search all queries, compute quality vs ground
 *               truth, write hnsw_v2_benchmark_result.json +
 *               hnsw_v2_per_query.jsonl via temp+rename (fail-fast if present).
 *   validate  - re-check an existing B3 result (schema, provenance checksums,
 *               per-query checksum, metric ranges) WITHOUT building v2 or ONNX.
 *
 * Must be run from the repository root (relative paths + generator source sha).
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "kvs_config.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "vector/kvs_vector_hnsw_v2.h"

/* g_cfg is defined inside libkvstore.a (src/core/kvs_globals.c). */
extern kvs_config_t g_cfg;

static const char *const GEN_SOURCE_REL_PATH = "scripts/benchmark/generate_real_embedding_hnsw_v2_benchmark.c";
static const char *const GEN_VERSION = "1";
static const int DEFAULT_TOPK = 10;
static const double NORM_TOL = 1e-3;

/* ------------------------------------------------------------------------ */
/* small helpers                                                            */
/* ------------------------------------------------------------------------ */

static uint64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int host_is_little_endian(void) {
	uint16_t probe = 1;
	return *(uint8_t *)&probe == 1;
}

static uint32_t swap_u32(uint32_t v) {
	return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
	       ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static int path_exists(const char *p) {
	struct stat st;
	return stat(p, &st) == 0;
}

static void iso8601_utc_now(char *out, size_t outsz) {
	time_t t = time(NULL);
	struct tm tm_utc;
	gmtime_r(&t, &tm_utc);
	strftime(out, outsz, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static void json_write_escaped_string(FILE *f, const char *s) {
	const unsigned char *p;
	fputc('"', f);
	for (p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"': fputs("\\\"", f); break;
		case '\\': fputs("\\\\", f); break;
		case '\n': fputs("\\n", f); break;
		case '\r': fputs("\\r", f); break;
		case '\t': fputs("\\t", f); break;
		case '\b': fputs("\\b", f); break;
		case '\f': fputs("\\f", f); break;
		default:
			if (*p < 0x20) {
				fprintf(f, "\\u%04x", *p);
			} else {
				fputc(*p, f);
			}
		}
	}
	fputc('"', f);
}

static int sha256_file(const char *path, char out_hex[65]) {
	FILE *f;
	EVP_MD_CTX *ctx;
	unsigned char buf[65536];
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len = 0;
	size_t n;
	int rc = -1;

	f = fopen(path, "rb");
	if (!f) {
		return -1;
	}
	ctx = EVP_MD_CTX_new();
	if (!ctx) {
		fclose(f);
		return -1;
	}
	if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
		goto done;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (EVP_DigestUpdate(ctx, buf, n) != 1) {
			goto done;
		}
	}
	if (ferror(f)) {
		goto done;
	}
	if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
		goto done;
	}
	for (unsigned int i = 0; i < digest_len; i++) {
		sprintf(out_hex + i * 2, "%02x", digest[i]);
	}
	out_hex[digest_len * 2] = '\0';
	rc = 0;

done:
	EVP_MD_CTX_free(ctx);
	fclose(f);
	return rc;
}

static int row_all_finite(const float *row, int dim) {
	for (int i = 0; i < dim; i++) {
		if (!isfinite(row[i])) {
			return 0;
		}
	}
	return 1;
}

static double row_norm(const float *row, int dim) {
	double s = 0.0;
	for (int i = 0; i < dim; i++) {
		s += (double)row[i] * (double)row[i];
	}
	return sqrt(s);
}

/* ------------------------------------------------------------------------ */
/* minimal scoped JSON reader (schema fully owned by this tool)             */
/* ------------------------------------------------------------------------ */

static const char *json_skip_ws(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
		p++;
	}
	return p;
}

static const char *json_find_key(const char *text, const char *key) {
	char pattern[256];
	const char *p;
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(text, pattern);
	if (!p) {
		return NULL;
	}
	p += strlen(pattern);
	p = json_skip_ws(p);
	if (*p != ':') {
		return NULL;
	}
	p++;
	return json_skip_ws(p);
}

static char *json_extract_object(const char *text, const char *key) {
	const char *val = json_find_key(text, key);
	const char *p;
	const char *start;
	int depth = 0;
	char *block;
	size_t len;

	if (!val || *val != '{') {
		return NULL;
	}
	start = val;
	p = val;
	do {
		if (*p == '{') {
			depth++;
		} else if (*p == '}') {
			depth--;
		} else if (*p == '\0') {
			return NULL;
		}
		p++;
	} while (depth > 0);
	len = (size_t)(p - start);
	block = malloc(len + 1);
	if (!block) {
		return NULL;
	}
	memcpy(block, start, len);
	block[len] = '\0';
	return block;
}

static int json_get_string(const char *text, const char *key, char *out, size_t outsz) {
	const char *v = json_find_key(text, key);
	size_t i = 0;
	if (!v || *v != '"') {
		return -1;
	}
	v++;
	while (*v && *v != '"' && i + 1 < outsz) {
		if (*v == '\\' && *(v + 1)) {
			v++;
			switch (*v) {
			case 'n': out[i++] = '\n'; break;
			case 't': out[i++] = '\t'; break;
			case 'r': out[i++] = '\r'; break;
			case '"': out[i++] = '"'; break;
			case '\\': out[i++] = '\\'; break;
			default: out[i++] = *v;
			}
			v++;
		} else {
			out[i++] = *v++;
		}
	}
	out[i] = '\0';
	return (*v == '"') ? 0 : -1;
}

static int json_get_int64(const char *text, const char *key, long long *out) {
	const char *v = json_find_key(text, key);
	char *end = NULL;
	long long val;
	if (!v) {
		return -1;
	}
	val = strtoll(v, &end, 10);
	if (end == v) {
		return -1;
	}
	*out = val;
	return 0;
}

static int json_get_double(const char *text, const char *key, double *out) {
	const char *v = json_find_key(text, key);
	char *end = NULL;
	double val;
	if (!v) {
		return -1;
	}
	val = strtod(v, &end);
	if (end == v) {
		return -1;
	}
	*out = val;
	return 0;
}

static char *read_whole_file(const char *path) {
	FILE *f = fopen(path, "rb");
	long sz;
	char *buf;
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)sz + 1);
	if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[sz] = '\0';
	fclose(f);
	return buf;
}

static float *load_f32(const char *path, size_t count, int dim) {
	FILE *f;
	long sz;
	size_t n = count * (size_t)dim;
	float *buf;

	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "[error] cannot open %s: %s\n", path, strerror(errno));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0 || (size_t)sz != n * sizeof(float)) {
		fprintf(stderr, "[error] %s size %ld != expected %zu\n", path, sz, n * sizeof(float));
		fclose(f);
		return NULL;
	}
	buf = malloc(n * sizeof(float));
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, sizeof(float), n, f) != n) {
		fprintf(stderr, "[error] %s short read\n", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	if (!host_is_little_endian()) {
		for (size_t i = 0; i < n; i++) {
			uint32_t v;
			memcpy(&v, &buf[i], sizeof(v));
			v = swap_u32(v);
			memcpy(&buf[i], &v, sizeof(v));
		}
	}
	return buf;
}

/* ------------------------------------------------------------------------ */
/* CLI                                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
	const char *dataset_id;
	const char *out_root;
	int topk;
} cli_args_t;

static void cli_args_default(cli_args_t *a) {
	a->dataset_id = NULL;
	a->out_root = "/tmp/kvlite_vector_benchmark_artifacts/real_embedding";
	a->topk = DEFAULT_TOPK;
}

static void print_usage(const char *prog) {
	fprintf(stderr,
		"usage:\n"
		"  %s generate --dataset-id <id> [--out-root DIR] [--topk N]\n"
		"  %s validate --dataset-id <id> [--out-root DIR]\n"
		"Index-only core_hnsw_v2 benchmark. Frozen default params; never calls ONNX;\n"
		"never regenerates ground truth. Must run from the repository root.\n",
		prog, prog);
}

static int parse_args(int argc, char **argv, cli_args_t *a) {
	for (int i = 2; i < argc; i++) {
		const char *key = argv[i];
		if (strcmp(key, "--dataset-id") == 0 && i + 1 < argc) {
			a->dataset_id = argv[++i];
		} else if (strcmp(key, "--out-root") == 0 && i + 1 < argc) {
			a->out_root = argv[++i];
		} else if (strcmp(key, "--topk") == 0 && i + 1 < argc) {
			a->topk = atoi(argv[++i]);
		} else {
			fprintf(stderr, "[error] unrecognized argument: %s\n", key);
			return -1;
		}
	}
	if (!a->dataset_id || a->dataset_id[0] == '\0') {
		fprintf(stderr, "[error] --dataset-id is required\n");
		return -1;
	}
	if (a->topk <= 0) {
		fprintf(stderr, "[error] --topk must be positive\n");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* input provenance verification (B1 cache + B2 ground truth)               */
/* ------------------------------------------------------------------------ */

typedef struct {
	long long record_count;
	long long query_count;
	long long dim;
	long long topk;
	char manifest_sha[65];
	char emb_sha[65];
	char qemb_sha[65];
	char rj_sha[65];
	char qj_sha[65];
	char gt_sha[65];      /* groundtruth.jsonl */
	char gtm_sha[65];     /* groundtruth.manifest.json */
} inputs_info_t;

static int verify_output_sha(const char *outputs, const char *key, const char *dataset_dir,
			     const char *filename, char out_sha[65]) {
	char *block = json_extract_object(outputs, key);
	char sha_val[80] = {0};
	char actual[65];
	char file_path[1200];
	int rc = -1;

	if (!block) {
		fprintf(stderr, "[error] manifest outputs.%s missing\n", key);
		return -1;
	}
	if (json_get_string(block, "sha256", sha_val, sizeof(sha_val)) != 0 || strlen(sha_val) != 64) {
		fprintf(stderr, "[error] manifest outputs.%s.sha256 invalid\n", key);
		goto done;
	}
	snprintf(file_path, sizeof(file_path), "%s/%s", dataset_dir, filename);
	if (sha256_file(file_path, actual) != 0) {
		fprintf(stderr, "[error] cannot checksum %s\n", file_path);
		goto done;
	}
	if (strcmp(actual, sha_val) != 0) {
		fprintf(stderr, "[error] %s sha256 mismatch: manifest=%s actual=%s\n", file_path, sha_val, actual);
		goto done;
	}
	memcpy(out_sha, sha_val, 65);
	rc = 0;

done:
	free(block);
	return rc;
}

/* Verifies the B1 cache (manifest schema + 4 artifact checksums) and the B2
 * ground truth (manifest schema + groundtruth.jsonl checksum + provenance
 * binding to the same B1 manifest). Rejects corrupted inputs. */
static int verify_inputs(const char *dataset_dir, const char *dataset_id, int topk, inputs_info_t *out) {
	char mf_path[1200], gtm_path[1200];
	char *b1 = NULL, *gtm = NULL, *params = NULL, *outputs = NULL, *gtparams = NULL, *gtsrc = NULL, *gtout = NULL;
	char dataset_id_val[256], dtype[32] = {0}, byte_order[32] = {0}, metric[64] = {0};
	long long schema_version = 0;
	int rc = -1;

	memset(out, 0, sizeof(*out));

	/* ---- B1 manifest.json ---- */
	snprintf(mf_path, sizeof(mf_path), "%s/manifest.json", dataset_dir);
	b1 = read_whole_file(mf_path);
	if (!b1) {
		fprintf(stderr, "[error] cannot read B1 manifest: %s\n", mf_path);
		return -1;
	}
	if (sha256_file(mf_path, out->manifest_sha) != 0) {
		goto done;
	}
	if (json_get_int64(b1, "schema_version", &schema_version) != 0 || schema_version != 1) {
		fprintf(stderr, "[error] B1 manifest schema_version != 1\n");
		goto done;
	}
	if (json_get_string(b1, "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0 ||
	    strcmp(dataset_id_val, dataset_id) != 0) {
		fprintf(stderr, "[error] B1 manifest dataset_id mismatch\n");
		goto done;
	}
	params = json_extract_object(b1, "params");
	if (!params ||
	    json_get_int64(params, "record_count", &out->record_count) != 0 || out->record_count <= 0 ||
	    json_get_int64(params, "query_count", &out->query_count) != 0 || out->query_count <= 0 ||
	    json_get_int64(params, "dim", &out->dim) != 0 || out->dim <= 0) {
		fprintf(stderr, "[error] B1 manifest params invalid\n");
		goto done;
	}
	if (json_get_string(params, "dtype", dtype, sizeof(dtype)) != 0 || strcmp(dtype, "float32") != 0 ||
	    json_get_string(params, "byte_order", byte_order, sizeof(byte_order)) != 0 || strcmp(byte_order, "little") != 0 ||
	    json_get_string(params, "metric", metric, sizeof(metric)) != 0 || strcmp(metric, "ip_normalized") != 0) {
		fprintf(stderr, "[error] B1 manifest params dtype/byte_order/metric unexpected\n");
		goto done;
	}
	{
		const char *nv = json_find_key(params, "normalized");
		if (!nv || strncmp(nv, "true", 4) != 0) {
			fprintf(stderr, "[error] B1 manifest params.normalized != true\n");
			goto done;
		}
	}
	outputs = json_extract_object(b1, "outputs");
	if (!outputs ||
	    verify_output_sha(outputs, "embeddings_f32", dataset_dir, "embeddings.f32", out->emb_sha) != 0 ||
	    verify_output_sha(outputs, "query_embeddings_f32", dataset_dir, "query_embeddings.f32", out->qemb_sha) != 0 ||
	    verify_output_sha(outputs, "records_jsonl", dataset_dir, "records.jsonl", out->rj_sha) != 0 ||
	    verify_output_sha(outputs, "queries_jsonl", dataset_dir, "queries.jsonl", out->qj_sha) != 0) {
		goto done;
	}

	/* ---- B2 groundtruth.manifest.json ---- */
	snprintf(gtm_path, sizeof(gtm_path), "%s/groundtruth.manifest.json", dataset_dir);
	gtm = read_whole_file(gtm_path);
	if (!gtm) {
		fprintf(stderr, "[error] cannot read B2 groundtruth manifest: %s\n", gtm_path);
		goto done;
	}
	if (sha256_file(gtm_path, out->gtm_sha) != 0) {
		goto done;
	}
	if (json_get_int64(gtm, "schema_version", &schema_version) != 0 || schema_version != 1) {
		fprintf(stderr, "[error] B2 groundtruth manifest schema_version != 1\n");
		goto done;
	}
	if (json_get_string(gtm, "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0 ||
	    strcmp(dataset_id_val, dataset_id) != 0) {
		fprintf(stderr, "[error] B2 groundtruth manifest dataset_id mismatch\n");
		goto done;
	}
	/* B2 ground truth must be bound to THIS B1 manifest */
	gtsrc = json_extract_object(gtm, "source");
	{
		char gt_src_manifest_sha[80] = {0};
		if (!gtsrc || json_get_string(gtsrc, "manifest_sha256", gt_src_manifest_sha, sizeof(gt_src_manifest_sha)) != 0 ||
		    strcmp(gt_src_manifest_sha, out->manifest_sha) != 0) {
			fprintf(stderr, "[error] B2 ground truth is not bound to this B1 manifest (source.manifest_sha256 mismatch)\n");
			goto done;
		}
	}
	gtparams = json_extract_object(gtm, "params");
	{
		long long gt_rc = 0, gt_qc = 0, gt_dim = 0, gt_topk = 0;
		char gt_metric[64] = {0};
		if (!gtparams ||
		    json_get_int64(gtparams, "record_count", &gt_rc) != 0 || gt_rc != out->record_count ||
		    json_get_int64(gtparams, "query_count", &gt_qc) != 0 || gt_qc != out->query_count ||
		    json_get_int64(gtparams, "dim", &gt_dim) != 0 || gt_dim != out->dim ||
		    json_get_int64(gtparams, "topk", &gt_topk) != 0 || gt_topk != topk ||
		    json_get_string(gtparams, "metric", gt_metric, sizeof(gt_metric)) != 0 ||
		    strcmp(gt_metric, "ip_normalized") != 0) {
			fprintf(stderr, "[error] B2 ground truth params inconsistent with B1/topk\n");
			goto done;
		}
		out->topk = gt_topk;
	}
	gtout = json_extract_object(gtm, "outputs");
	if (!gtout || verify_output_sha(gtout, "groundtruth_jsonl", dataset_dir, "groundtruth.jsonl", out->gt_sha) != 0) {
		fprintf(stderr, "[error] B2 groundtruth.jsonl checksum verification failed\n");
		goto done;
	}
	rc = 0;

done:
	free(b1);
	free(gtm);
	free(params);
	free(outputs);
	free(gtparams);
	free(gtsrc);
	free(gtout);
	return rc;
}

/* ------------------------------------------------------------------------ */
/* ground truth loading (record_id lists per query row)                     */
/* ------------------------------------------------------------------------ */

/* gt_ids: [query_count * topk], gt_present: [query_count]. Parses B2
 * groundtruth.jsonl (neighbors in rank order). record_id == records.jsonl row. */
static int load_groundtruth(const char *dataset_dir, long long query_count, int topk,
			    long long record_count, uint64_t *gt_ids, uint8_t *gt_present) {
	char path[1200];
	FILE *f;
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	long long lineno = 0;
	int rc = -1;

	snprintf(path, sizeof(path), "%s/groundtruth.jsonl", dataset_dir);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "[error] cannot open %s\n", path);
		return -1;
	}
	memset(gt_present, 0, (size_t)query_count);

	while ((len = getline(&line, &cap, f)) != -1) {
		long long query_row = -1, line_topk = -1;
		const char *nb;
		int r = 0;
		lineno++;
		if (json_get_int64(line, "query_row", &query_row) != 0 || query_row < 0 || query_row >= query_count) {
			fprintf(stderr, "[error] groundtruth line %lld: bad query_row\n", lineno);
			goto done;
		}
		if (json_get_int64(line, "topk", &line_topk) != 0 || line_topk != topk) {
			fprintf(stderr, "[error] groundtruth line %lld: topk != %d\n", lineno, topk);
			goto done;
		}
		if (gt_present[query_row]) {
			fprintf(stderr, "[error] groundtruth duplicate query_row %lld\n", query_row);
			goto done;
		}
		nb = strstr(line, "\"neighbors\"");
		if (!nb) {
			fprintf(stderr, "[error] groundtruth line %lld: missing neighbors\n", lineno);
			goto done;
		}
		for (r = 0; r < topk; r++) {
			const char *rk = strstr(nb, "\"rank\"");
			long long rank_v = -1, rid_v = -1;
			if (!rk) {
				fprintf(stderr, "[error] groundtruth line %lld: fewer than topk neighbors\n", lineno);
				goto done;
			}
			if (json_get_int64(rk, "rank", &rank_v) != 0 || rank_v != r + 1 ||
			    json_get_int64(rk, "record_id", &rid_v) != 0 || rid_v < 0 || rid_v >= record_count) {
				fprintf(stderr, "[error] groundtruth line %lld: bad neighbor at rank %d\n", lineno, r + 1);
				goto done;
			}
			gt_ids[query_row * topk + r] = (uint64_t)rid_v;
			nb = rk + 6;
		}
		gt_present[query_row] = 1;
	}
	for (long long q = 0; q < query_count; q++) {
		if (!gt_present[q]) {
			fprintf(stderr, "[error] groundtruth missing query_row %lld\n", q);
			goto done;
		}
	}
	rc = 0;

done:
	free(line);
	fclose(f);
	return rc;
}

/* ------------------------------------------------------------------------ */
/* percentiles                                                               */
/* ------------------------------------------------------------------------ */

static int cmp_u64(const void *a, const void *b) {
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* nearest-rank percentile on a sorted-ascending array */
static uint64_t percentile(const uint64_t *sorted, size_t n, double p) {
	long long idx;
	if (n == 0) {
		return 0;
	}
	idx = (long long)ceil(p / 100.0 * (double)n) - 1;
	if (idx < 0) {
		idx = 0;
	}
	if ((size_t)idx >= n) {
		idx = (long long)n - 1;
	}
	return sorted[idx];
}

/* ------------------------------------------------------------------------ */
/* generate                                                                  */
/* ------------------------------------------------------------------------ */

static int cmd_generate(const cli_args_t *args) {
	uint64_t total_start = now_us();
	char dataset_dir[1024];
	char res_path[1200], pq_path[1200], res_tmp[1200] = {0}, pq_tmp[1200] = {0};
	inputs_info_t in;
	float *base = NULL, *query = NULL;
	uint64_t *gt_ids = NULL;
	uint8_t *gt_present = NULL;
	kvs_hnsw_v2_index_t index;
	int index_ready = 0;
	FILE *pq = NULL;
	int rc = 1;
	int topk;
	long long count, qcount, dim;
	uint64_t cache_load_us = 0, index_build_us = 0, search_total_us = 0;
	uint64_t *search_us = NULL, *sorted_us = NULL;
	kvs_vector_hit_t *hits = NULL;
	double sum_r1 = 0, sum_r10 = 0, sum_top1 = 0, sum_exact = 0, min_r10 = 1.0;
	uint64_t rng_seed;
	kvs_vector_core_info_t core_info;
	uint64_t dup_edges = 0, degree_overflow = 0;
	long long mem_bytes = 0, idx_size = 0, idx_active = 0, idx_deleted = 0, idx_capacity = 0;
	char pq_sha[65];
	long long pq_size = 0;
	char source_sha[65];
	int have_source_sha = 0;
	char created_at[64];

	memset(&index, 0, sizeof(index));
	memset(&core_info, 0, sizeof(core_info));

	snprintf(dataset_dir, sizeof(dataset_dir), "%s/%s", args->out_root, args->dataset_id);
	if (!path_exists(dataset_dir)) {
		fprintf(stderr, "[error] dataset directory does not exist: %s\n", dataset_dir);
		return 1;
	}
	snprintf(res_path, sizeof(res_path), "%s/hnsw_v2_benchmark_result.json", dataset_dir);
	snprintf(pq_path, sizeof(pq_path), "%s/hnsw_v2_per_query.jsonl", dataset_dir);
	if (path_exists(res_path) || path_exists(pq_path)) {
		fprintf(stderr, "[error] B3 result already exists in %s (refusing to overwrite; remove hnsw_v2_benchmark_result.json and hnsw_v2_per_query.jsonl to regenerate)\n", dataset_dir);
		return 1;
	}

	topk = args->topk;
	if (verify_inputs(dataset_dir, args->dataset_id, topk, &in) != 0) {
		return 1;
	}
	count = in.record_count;
	qcount = in.query_count;
	dim = in.dim;
	if ((long long)topk > count) {
		fprintf(stderr, "[error] topk (%d) exceeds record_count (%lld)\n", topk, count);
		return 1;
	}
	fprintf(stdout, "[inputs] record_count=%lld query_count=%lld dim=%lld topk=%d (B1 cache + B2 ground truth verified)\n",
		count, qcount, dim, topk);

	/* load caches */
	{
		char p[1200];
		uint64_t t0 = now_us();
		snprintf(p, sizeof(p), "%s/embeddings.f32", dataset_dir);
		base = load_f32(p, (size_t)count, (int)dim);
		snprintf(p, sizeof(p), "%s/query_embeddings.f32", dataset_dir);
		query = load_f32(p, (size_t)qcount, (int)dim);
		cache_load_us = now_us() - t0;
		if (!base || !query) {
			goto cleanup;
		}
	}
	for (long long i = 0; i < count; i++) {
		if (!row_all_finite(base + i * dim, (int)dim) ||
		    fabs(row_norm(base + i * dim, (int)dim) - 1.0) > NORM_TOL) {
			fprintf(stderr, "[error] record row %lld not finite/normalized\n", i);
			goto cleanup;
		}
	}

	gt_ids = malloc(sizeof(uint64_t) * (size_t)qcount * (size_t)topk);
	gt_present = malloc((size_t)qcount);
	if (!gt_ids || !gt_present) {
		fprintf(stderr, "[error] oom for ground truth\n");
		goto cleanup;
	}
	if (load_groundtruth(dataset_dir, qcount, topk, count, gt_ids, gt_present) != 0) {
		goto cleanup;
	}

	/* runtime backend init (same as core tests / B2) */
	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend, sizeof(g_cfg.vector_distance_backend), "%s", "auto");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	/* build core_hnsw_v2 with FROZEN DEFAULT params; record_id = row */
	rng_seed = 0x9d000000ULL + (uint64_t)count;
	{
		uint64_t t0 = now_us();
		if (kvs_hnsw_v2_index_init(&index, (size_t)dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
			fprintf(stderr, "[error] kvs_hnsw_v2_index_init failed\n");
			goto cleanup;
		}
		index_ready = 1;
		/* frozen defaults are set by init (M=48,M0=96,efC=512,efS=1024); do NOT retune. */
		kvs_hnsw_v2_index_set_seed(&index, rng_seed);
		if (kvs_hnsw_v2_index_reserve(&index, (size_t)count) != KVS_VECTOR_OK) {
			fprintf(stderr, "[error] kvs_hnsw_v2_index_reserve failed\n");
			goto cleanup;
		}
		for (long long i = 0; i < count; i++) {
			if (kvs_hnsw_v2_index_add(&index, (uint64_t)i, base + i * dim, (size_t)dim) != KVS_VECTOR_OK) {
				fprintf(stderr, "[error] kvs_hnsw_v2_index_add failed at row %lld\n", i);
				goto cleanup;
			}
		}
		index_build_us = now_us() - t0;
	}

	fprintf(stdout, "[build] M=%zu M0=%zu efConstruction=%zu efSearch=%zu seed=0x%" PRIx64 " build_us=%" PRIu64 "\n",
		index.M, index.M0, index.ef_construction, index.ef_search, rng_seed, index_build_us);

	/* index stats */
	kvs_hnsw_v2_graph_stats_refresh(&index);
	kvs_hnsw_v2_memory_refresh(&index);
	kvs_hnsw_v2_index_info(&index, &core_info);
	mem_bytes = (long long)core_info.memory_bytes;
	idx_size = (long long)core_info.size;
	idx_deleted = (long long)core_info.deleted_count;
	idx_active = idx_size - idx_deleted;
	idx_capacity = (long long)core_info.capacity;
	dup_edges = index.graph_stats.duplicate_edge_count;
	degree_overflow = index.graph_stats.degree_overflow_count;

	/* search all queries; per-query recall vs ground truth */
	hits = malloc(sizeof(kvs_vector_hit_t) * (size_t)topk);
	search_us = malloc(sizeof(uint64_t) * (size_t)qcount);
	sorted_us = malloc(sizeof(uint64_t) * (size_t)qcount);
	if (!hits || !search_us || !sorted_us) {
		fprintf(stderr, "[error] oom for search buffers\n");
		goto cleanup;
	}

	snprintf(pq_tmp, sizeof(pq_tmp), "%s/.tmp.hnsw_v2_per_query.%d.jsonl", dataset_dir, (int)getpid());
	snprintf(res_tmp, sizeof(res_tmp), "%s/.tmp.hnsw_v2_benchmark_result.%d.json", dataset_dir, (int)getpid());
	pq = fopen(pq_tmp, "w");
	if (!pq) {
		fprintf(stderr, "[error] cannot open temp per-query file\n");
		goto cleanup;
	}

	for (long long q = 0; q < qcount; q++) {
		size_t hit_count = 0;
		const float *qv = query + q * dim;
		const uint64_t *gt = gt_ids + q * topk;
		uint64_t t0, dt;
		int inter10 = 0, r1_found = 0, top1 = 0, exact = 1;

		t0 = now_us();
		int sret = kvs_hnsw_v2_index_search(&index, qv, (size_t)dim, (size_t)topk, index.ef_search, hits, &hit_count);
		dt = now_us() - t0;
		if (sret != KVS_VECTOR_OK) {
			fprintf(stderr, "[error] v2 search failed at query %lld (rc=%d)\n", q, sret);
			goto cleanup;
		}
		if (hit_count != (size_t)topk) {
			fprintf(stderr, "[error] query %lld returned %zu hits, expected %d\n", q, hit_count, topk);
			goto cleanup;
		}
		search_us[q] = dt;
		search_total_us += dt;

		/* structural checks on the ANN result */
		for (size_t r = 0; r < hit_count; r++) {
			if (!isfinite(hits[r].score)) {
				fprintf(stderr, "[error] query %lld rank %zu non-finite score\n", q, r);
				goto cleanup;
			}
			if (hits[r].record_id >= (uint64_t)count) {
				fprintf(stderr, "[error] query %lld rank %zu record_id out of range\n", q, r);
				goto cleanup;
			}
			if (r > 0 && hits[r].score > hits[r - 1].score) {
				fprintf(stderr, "[error] query %lld scores not descending at rank %zu\n", q, r);
				goto cleanup;
			}
			for (size_t s = 0; s < r; s++) {
				if (hits[s].record_id == hits[r].record_id) {
					fprintf(stderr, "[error] query %lld duplicate record_id\n", q);
					goto cleanup;
				}
			}
		}

		/* recall@10 = |gt_top10 ∩ ann_topK| / topk ; recall@1 = gt_top1 ∈ ann_topK ;
		 * top1_match = ann_rank1 == gt_rank1 ; exact = ordered ann == ordered gt */
		for (int r = 0; r < topk; r++) {
			for (size_t j = 0; j < hit_count; j++) {
				if (hits[j].record_id == gt[r]) {
					inter10++;
					break;
				}
			}
		}
		for (size_t j = 0; j < hit_count; j++) {
			if (hits[j].record_id == gt[0]) {
				r1_found = 1;
				break;
			}
		}
		top1 = (hit_count > 0 && hits[0].record_id == gt[0]) ? 1 : 0;
		if (hit_count != (size_t)topk) {
			exact = 0;
		} else {
			for (int r = 0; r < topk; r++) {
				if (hits[r].record_id != gt[r]) {
					exact = 0;
					break;
				}
			}
		}

		double r10 = (double)inter10 / (double)topk;
		sum_r10 += r10;
		sum_r1 += (double)r1_found;
		sum_top1 += (double)top1;
		sum_exact += (double)exact;
		if (r10 < min_r10) {
			min_r10 = r10;
		}

		/* deterministic per-query record (no timing) */
		{
			char query_id[32];
			snprintf(query_id, sizeof(query_id), "q%06lld", q);
			fprintf(pq, "{\"query_row\":%lld,\"query_id\":", q);
			json_write_escaped_string(pq, query_id);
			fprintf(pq, ",\"hit_count\":%zu,\"ann\":[", hit_count);
			for (size_t j = 0; j < hit_count; j++) {
				fprintf(pq, "%s%" PRIu64, j == 0 ? "" : ",", hits[j].record_id);
			}
			fprintf(pq, "],\"hits\":[");
			for (size_t j = 0; j < hit_count; j++) {
				fprintf(pq, "%s{\"rank\":%zu,\"record_id\":%" PRIu64 ",\"score\":%.9g}",
					j == 0 ? "" : ",", j + 1, hits[j].record_id, (double)hits[j].score);
			}
			fprintf(pq, "],\"recall_at_1\":%d,\"recall_at_10\":%.6f,\"top1_match\":%d,\"exact_match\":%d}\n",
				r1_found, r10, top1, exact);
		}
		if (ferror(pq)) {
			fprintf(stderr, "[error] write error on per-query file\n");
			goto cleanup;
		}
	}

	{
		int pq_err = (fflush(pq) != 0 || ferror(pq));
		if (fclose(pq) != 0) {
			pq_err = 1;
		}
		pq = NULL;
		if (pq_err) {
			fprintf(stderr, "[error] flush/close error on per-query file\n");
			goto cleanup;
		}
	}

	/* aggregate quality + latency percentiles */
	{
		double recall_at_1 = sum_r1 / (double)qcount;
		double recall_at_10 = sum_r10 / (double)qcount;
		double top1_match_rate = sum_top1 / (double)qcount;
		double exact_rate = sum_exact / (double)qcount;
		double search_avg = (double)search_total_us / (double)qcount;
		double build_avg_per_rec = (double)index_build_us / (double)count;
		double qps = search_total_us ? (double)qcount * 1000000.0 / (double)search_total_us : 0.0;
		uint64_t p50, p95, p99;

		memcpy(sorted_us, search_us, sizeof(uint64_t) * (size_t)qcount);
		qsort(sorted_us, (size_t)qcount, sizeof(uint64_t), cmp_u64);
		p50 = percentile(sorted_us, (size_t)qcount, 50.0);
		p95 = percentile(sorted_us, (size_t)qcount, 95.0);
		p99 = percentile(sorted_us, (size_t)qcount, 99.0);

		fprintf(stdout, "[quality] recall@1=%.4f recall@10=%.4f top1_match=%.4f exact=%.4f min_recall@10=%.4f\n",
			recall_at_1, recall_at_10, top1_match_rate, exact_rate, min_r10);
		fprintf(stdout, "[perf] search_avg_us=%.1f p50=%" PRIu64 " p95=%" PRIu64 " p99=%" PRIu64 " qps=%.1f build_us=%" PRIu64 "\n",
			search_avg, p50, p95, p99, qps, index_build_us);
		fprintf(stdout, "[stats] memory_bytes=%lld(estimated) size=%lld active=%lld deleted=%lld dup_edges=%" PRIu64 " degree_overflow=%" PRIu64 "\n",
			mem_bytes, idx_size, idx_active, idx_deleted, dup_edges, degree_overflow);

		if (sha256_file(pq_tmp, pq_sha) != 0) {
			fprintf(stderr, "[error] cannot checksum per-query file\n");
			goto cleanup;
		}
		{
			struct stat st;
			if (stat(pq_tmp, &st) != 0) {
				fprintf(stderr, "[error] cannot stat per-query temp file\n");
				goto cleanup;
			}
			pq_size = (long long)st.st_size;
		}
		have_source_sha = (sha256_file(GEN_SOURCE_REL_PATH, source_sha) == 0);
		if (!have_source_sha) {
			fprintf(stderr, "[warn] could not read generator source for source_sha256 (run from repo root)\n");
		}
		iso8601_utc_now(created_at, sizeof(created_at));

		/* write result manifest (temp) */
		{
			FILE *mf = fopen(res_tmp, "w");
			uint64_t total_us;
			if (!mf) {
				fprintf(stderr, "[error] cannot open temp result manifest\n");
				goto cleanup;
			}
			total_us = now_us() - total_start;
			fprintf(mf, "{\n");
			fprintf(mf, "  \"schema_version\": 1,\n");
			fprintf(mf, "  \"dataset_id\": ");
			json_write_escaped_string(mf, args->dataset_id);
			fprintf(mf, ",\n  \"backend\": \"core_hnsw_v2\",\n  \"created_at_utc\": ");
			json_write_escaped_string(mf, created_at);
			fprintf(mf, ",\n  \"generator\": {\"name\": \"generate_real_embedding_hnsw_v2_benchmark\", \"version\": ");
			json_write_escaped_string(mf, GEN_VERSION);
			fprintf(mf, ", \"source_sha256\": ");
			if (have_source_sha) {
				json_write_escaped_string(mf, source_sha);
			} else {
				fprintf(mf, "null");
			}
			fprintf(mf, "},\n");
			fprintf(mf, "  \"source\": {\"manifest_sha256\": ");
			json_write_escaped_string(mf, in.manifest_sha);
			fprintf(mf, ", \"embeddings_sha256\": ");
			json_write_escaped_string(mf, in.emb_sha);
			fprintf(mf, ", \"query_embeddings_sha256\": ");
			json_write_escaped_string(mf, in.qemb_sha);
			fprintf(mf, ", \"records_jsonl_sha256\": ");
			json_write_escaped_string(mf, in.rj_sha);
			fprintf(mf, ", \"queries_jsonl_sha256\": ");
			json_write_escaped_string(mf, in.qj_sha);
			fprintf(mf, ", \"groundtruth_jsonl_sha256\": ");
			json_write_escaped_string(mf, in.gt_sha);
			fprintf(mf, ", \"groundtruth_manifest_sha256\": ");
			json_write_escaped_string(mf, in.gtm_sha);
			fprintf(mf, "},\n");
			fprintf(mf,
				"  \"params\": {\"dim\": %lld, \"metric\": \"ip_normalized\", \"count\": %lld, \"query_count\": %lld, "
				"\"topk\": %d, \"M\": %zu, \"M0\": %zu, \"efConstruction\": %zu, \"efSearch\": %zu, "
				"\"params_source\": \"core_hnsw_v2_frozen_defaults\", \"rng_seed\": %" PRIu64 "},\n",
				dim, count, qcount, topk, index.M, index.M0, index.ef_construction, index.ef_search, rng_seed);
			fprintf(mf,
				"  \"quality\": {\"recall_at_1\": %.6f, \"recall_at_10\": %.6f, \"top1_match_rate\": %.6f, "
				"\"topk_exact_match_rate\": %.6f, \"min_per_query_recall_at_10\": %.6f, \"queries_checked\": %lld},\n",
				recall_at_1, recall_at_10, top1_match_rate, exact_rate, min_r10, qcount);
			fprintf(mf,
				"  \"performance\": {\"cache_load_us\": %" PRIu64 ", \"index_build_us\": %" PRIu64
				", \"build_avg_us_per_record\": %.3f, \"search_total_us\": %" PRIu64
				", \"search_avg_us\": %.3f, \"search_p50_us\": %" PRIu64 ", \"search_p95_us\": %" PRIu64
				", \"search_p99_us\": %" PRIu64 ", \"qps\": %.3f},\n",
				cache_load_us, index_build_us, build_avg_per_rec, search_total_us,
				search_avg, p50, p95, p99, qps);
			fprintf(mf,
				"  \"index_stats\": {\"memory_bytes\": %lld, \"memory_bytes_source\": \"estimated\", "
				"\"size\": %lld, \"active_count\": %lld, \"deleted_count\": %lld, \"capacity\": %lld, "
				"\"duplicate_edge_count\": %" PRIu64 ", \"degree_overflow_count\": %" PRIu64 ", "
				"\"graph_counters_source\": \"core_build_event_counter\"},\n",
				mem_bytes, idx_size, idx_active, idx_deleted, idx_capacity, dup_edges, degree_overflow);
			fprintf(mf,
				"  \"outputs\": {\"per_query_jsonl\": {\"path\": \"hnsw_v2_per_query.jsonl\", \"sha256\": ");
			json_write_escaped_string(mf, pq_sha);
			fprintf(mf, ", \"size_bytes\": %lld, \"rows\": %lld}},\n", pq_size, qcount);
			fprintf(mf, "  \"timing_us\": {\"total_us\": %" PRIu64 "}\n", total_us);
			fprintf(mf, "}\n");
			{
				int mf_err = (fflush(mf) != 0 || ferror(mf));
				if (fclose(mf) != 0) {
					mf_err = 1;
				}
				if (mf_err) {
					fprintf(stderr, "[error] write error on result manifest\n");
					goto cleanup;
				}
			}
		}
	}

	/* atomic publish: per-query first, result manifest last */
	if (rename(pq_tmp, pq_path) != 0) {
		fprintf(stderr, "[error] failed to publish per-query file: %s\n", strerror(errno));
		goto cleanup;
	}
	if (rename(res_tmp, res_path) != 0) {
		fprintf(stderr, "[error] failed to publish result manifest: %s\n", strerror(errno));
		remove(pq_path);
		goto cleanup;
	}
	fprintf(stdout, "[publish] dataset_id=%s result=%s\n", args->dataset_id, res_path);
	rc = 0;

cleanup:
	if (pq) {
		fclose(pq);
	}
	if (rc != 0) {
		if (pq_tmp[0]) {
			remove(pq_tmp);
		}
		if (res_tmp[0]) {
			remove(res_tmp);
		}
	}
	if (index_ready) {
		kvs_hnsw_v2_index_destroy(&index);
	}
	free(base);
	free(query);
	free(gt_ids);
	free(gt_present);
	free(hits);
	free(search_us);
	free(sorted_us);
	return rc;
}

/* ------------------------------------------------------------------------ */
/* validate (no v2 build, no ONNX)                                           */
/* ------------------------------------------------------------------------ */

static int check_file_sha(const char *dataset_dir, const char *filename, const char *expect_sha) {
	char path[1200], actual[65];
	snprintf(path, sizeof(path), "%s/%s", dataset_dir, filename);
	if (sha256_file(path, actual) != 0) {
		fprintf(stderr, "[fail] cannot checksum %s\n", filename);
		return -1;
	}
	if (strcmp(actual, expect_sha) != 0) {
		fprintf(stderr, "[fail] %s sha256 mismatch (result bound to different input)\n", filename);
		return -1;
	}
	return 0;
}

static int cmd_validate(const cli_args_t *args) {
	char dataset_dir[1024];
	char res_path[1200], pq_path[1200];
	char *buf = NULL, *src = NULL, *params = NULL, *quality = NULL, *perf = NULL, *stats = NULL, *outputs = NULL, *pqblock = NULL;
	int ok = 1;
	long long schema_version = 0;
	char dataset_id_val[256], backend[64] = {0}, metric[64] = {0}, mem_src[32] = {0};
	long long count = 0, qcount = 0, dim = 0, topk = 0;
	long long queries_checked = 0, size = 0, active = 0, deleted = 0;
	double recall_at_1 = -1, recall_at_10 = -1, top1 = -1, exact = -1, minr10 = -1, qps = -1, search_avg = -1;
	char pq_sha[80] = {0};
	long long pq_size_m = -1, pq_rows_m = -1;
	char actual[65];

	snprintf(dataset_dir, sizeof(dataset_dir), "%s/%s", args->out_root, args->dataset_id);
	snprintf(res_path, sizeof(res_path), "%s/hnsw_v2_benchmark_result.json", dataset_dir);
	snprintf(pq_path, sizeof(pq_path), "%s/hnsw_v2_per_query.jsonl", dataset_dir);
	if (!path_exists(dataset_dir)) {
		fprintf(stderr, "[fail] dataset directory missing\n");
		return 1;
	}
	buf = read_whole_file(res_path);
	if (!buf) {
		fprintf(stderr, "[fail] cannot read B3 result manifest: %s\n", res_path);
		return 1;
	}

	if (json_get_int64(buf, "schema_version", &schema_version) != 0 || schema_version != 1) {
		fprintf(stderr, "[fail] result schema_version != 1\n");
		ok = 0;
	}
	if (json_get_string(buf, "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0 ||
	    strcmp(dataset_id_val, args->dataset_id) != 0) {
		fprintf(stderr, "[fail] result dataset_id mismatch\n");
		ok = 0;
	}
	if (json_get_string(buf, "backend", backend, sizeof(backend)) != 0 || strcmp(backend, "core_hnsw_v2") != 0) {
		fprintf(stderr, "[fail] result backend != core_hnsw_v2\n");
		ok = 0;
	}

	params = json_extract_object(buf, "params");
	if (!params ||
	    json_get_int64(params, "count", &count) != 0 || count <= 0 ||
	    json_get_int64(params, "query_count", &qcount) != 0 || qcount <= 0 ||
	    json_get_int64(params, "dim", &dim) != 0 || dim <= 0 ||
	    json_get_int64(params, "topk", &topk) != 0 || topk <= 0 ||
	    json_get_string(params, "metric", metric, sizeof(metric)) != 0 || strcmp(metric, "ip_normalized") != 0) {
		fprintf(stderr, "[fail] result params invalid\n");
		ok = 0;
	}

	/* provenance: every recorded source sha must match the on-disk input */
	src = json_extract_object(buf, "source");
	if (!src) {
		fprintf(stderr, "[fail] result missing source block\n");
		ok = 0;
	} else {
		struct {
			const char *key;
			const char *file;
		} binds[7] = {
			{"manifest_sha256", "manifest.json"},
			{"embeddings_sha256", "embeddings.f32"},
			{"query_embeddings_sha256", "query_embeddings.f32"},
			{"records_jsonl_sha256", "records.jsonl"},
			{"queries_jsonl_sha256", "queries.jsonl"},
			{"groundtruth_jsonl_sha256", "groundtruth.jsonl"},
			{"groundtruth_manifest_sha256", "groundtruth.manifest.json"},
		};
		for (int i = 0; i < 6; i++) {
			char sha[80] = {0};
			if (json_get_string(src, binds[i].key, sha, sizeof(sha)) != 0 || strlen(sha) != 64) {
				fprintf(stderr, "[fail] result source.%s invalid\n", binds[i].key);
				ok = 0;
				continue;
			}
			if (check_file_sha(dataset_dir, binds[i].file, sha) != 0) {
				ok = 0;
			}
		}
	}

	quality = json_extract_object(buf, "quality");
	if (!quality ||
	    json_get_double(quality, "recall_at_1", &recall_at_1) != 0 ||
	    json_get_double(quality, "recall_at_10", &recall_at_10) != 0 ||
	    json_get_double(quality, "top1_match_rate", &top1) != 0 ||
	    json_get_double(quality, "topk_exact_match_rate", &exact) != 0 ||
	    json_get_double(quality, "min_per_query_recall_at_10", &minr10) != 0 ||
	    json_get_int64(quality, "queries_checked", &queries_checked) != 0) {
		fprintf(stderr, "[fail] result quality block invalid\n");
		ok = 0;
	} else {
		double vals[5] = {recall_at_1, recall_at_10, top1, exact, minr10};
		for (int i = 0; i < 5; i++) {
			if (!(vals[i] >= 0.0 && vals[i] <= 1.0)) {
				fprintf(stderr, "[fail] quality metric out of [0,1]: %.6f\n", vals[i]);
				ok = 0;
			}
		}
		if (queries_checked != qcount) {
			fprintf(stderr, "[fail] queries_checked (%lld) != query_count (%lld)\n", queries_checked, qcount);
			ok = 0;
		}
	}

	perf = json_extract_object(buf, "performance");
	if (!perf ||
	    json_get_double(perf, "qps", &qps) != 0 ||
	    json_get_double(perf, "search_avg_us", &search_avg) != 0) {
		fprintf(stderr, "[fail] result performance block invalid\n");
		ok = 0;
	} else if (!(qps > 0.0) || !(search_avg > 0.0)) {
		fprintf(stderr, "[fail] performance qps/search_avg_us must be positive\n");
		ok = 0;
	}

	stats = json_extract_object(buf, "index_stats");
	if (!stats ||
	    json_get_int64(stats, "size", &size) != 0 ||
	    json_get_int64(stats, "active_count", &active) != 0 ||
	    json_get_int64(stats, "deleted_count", &deleted) != 0 ||
	    json_get_string(stats, "memory_bytes_source", mem_src, sizeof(mem_src)) != 0) {
		fprintf(stderr, "[fail] result index_stats block invalid\n");
		ok = 0;
	} else {
		if (size != count || active != count || deleted != 0) {
			fprintf(stderr, "[fail] index_stats size/active/deleted inconsistent (size=%lld active=%lld deleted=%lld count=%lld)\n",
				size, active, deleted, count);
			ok = 0;
		}
		if (strcmp(mem_src, "estimated") != 0 && strcmp(mem_src, "measured") != 0 && strcmp(mem_src, "unsupported") != 0) {
			fprintf(stderr, "[fail] memory_bytes_source must be measured/estimated/unsupported (got '%s')\n", mem_src);
			ok = 0;
		}
	}

	/* per-query file provenance + row count */
	outputs = json_extract_object(buf, "outputs");
	if (outputs) {
		pqblock = json_extract_object(outputs, "per_query_jsonl");
	}
	if (!outputs || !pqblock ||
	    json_get_string(pqblock, "sha256", pq_sha, sizeof(pq_sha)) != 0 || strlen(pq_sha) != 64 ||
	    json_get_int64(pqblock, "size_bytes", &pq_size_m) != 0 ||
	    json_get_int64(pqblock, "rows", &pq_rows_m) != 0) {
		fprintf(stderr, "[fail] result outputs.per_query_jsonl invalid\n");
		ok = 0;
	} else {
		struct stat st;
		if (stat(pq_path, &st) != 0) {
			fprintf(stderr, "[fail] hnsw_v2_per_query.jsonl missing\n");
			ok = 0;
		} else {
			if ((long long)st.st_size != pq_size_m) {
				fprintf(stderr, "[fail] per-query size mismatch\n");
				ok = 0;
			}
			if (sha256_file(pq_path, actual) != 0 || strcmp(actual, pq_sha) != 0) {
				fprintf(stderr, "[fail] per-query sha256 mismatch\n");
				ok = 0;
			}
			if (pq_rows_m != qcount) {
				fprintf(stderr, "[fail] per-query rows (%lld) != query_count (%lld)\n", pq_rows_m, qcount);
				ok = 0;
			}
		}
	}

	/* per-query line count sanity */
	if (ok) {
		FILE *f = fopen(pq_path, "r");
		long long lines = 0;
		int c;
		if (!f) {
			fprintf(stderr, "[fail] cannot reopen per-query file\n");
			ok = 0;
		} else {
			while ((c = fgetc(f)) != EOF) {
				if (c == '\n') {
					lines++;
				}
			}
			fclose(f);
			if (lines != qcount) {
				fprintf(stderr, "[fail] per-query line count %lld != query_count %lld\n", lines, qcount);
				ok = 0;
			}
		}
	}

	free(pqblock);
	free(outputs);
	free(stats);
	free(perf);
	free(quality);
	free(params);
	free(src);
	free(buf);
	if (ok) {
		fprintf(stdout, "[validate] PASS dataset_id=%s backend=core_hnsw_v2 query_count=%lld topk=%lld recall_at_10=%.4f\n",
			args->dataset_id, qcount, topk, recall_at_10);
		return 0;
	}
	fprintf(stderr, "[validate] FAIL dataset_id=%s\n", args->dataset_id);
	return 1;
}

/* ------------------------------------------------------------------------ */
/* main                                                                      */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv) {
	cli_args_t args;

	if (argc < 2) {
		print_usage(argv[0]);
		return 2;
	}
	cli_args_default(&args);

	if (strcmp(argv[1], "generate") == 0) {
		if (parse_args(argc, argv, &args) != 0) {
			print_usage(argv[0]);
			return 2;
		}
		return cmd_generate(&args);
	}
	if (strcmp(argv[1], "validate") == 0) {
		if (parse_args(argc, argv, &args) != 0) {
			print_usage(argv[0]);
			return 2;
		}
		return cmd_validate(&args);
	}

	fprintf(stderr, "[error] unknown subcommand: %s\n", argv[1]);
	print_usage(argv[0]);
	return 2;
}
