/*
 * B2-ground-truth-generation: real embedding ground truth generator.
 *
 * Consumes ONLY the B1-frozen real embedding cache:
 *   manifest.json, embeddings.f32, query_embeddings.f32, records.jsonl, queries.jsonl
 * and produces exact top-K ground truth using the project's existing core_flat
 * exact inner-product index (index-only: no manager, no g_vector_state, no RESP,
 * no ONNX).
 *
 * The written ground truth is produced by kvs_flat_index_search() (the exact
 * flat scan). An independent double-precision brute-force top-K is computed only
 * to CROSS-CHECK core_flat's output, never as the published truth.
 *
 * Subcommands:
 *   generate  - verify the B1 cache (schema + checksums + shape), build core_flat,
 *               run exact top-K over all queries, write groundtruth.jsonl +
 *               groundtruth.manifest.json into the dataset dir via temp+rename.
 *   validate  - re-check an existing ground truth (manifest schema, checksum,
 *               ranks, monotonic scores, id/row ranges) WITHOUT core_flat or ONNX.
 *
 * Must be run from the repository root (relative fixture paths and generator
 * source_sha256 depend on it), same convention as the tests/ shell scripts.
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
#include "vector/kvs_vector_flat.h"

/* g_cfg is defined inside libkvstore.a (src/core/kvs_globals.c); reference it. */
extern kvs_config_t g_cfg;

static const char *const GEN_SOURCE_REL_PATH = "scripts/benchmark/generate_real_embedding_groundtruth.c";
static const char *const GEN_VERSION = "1";
static const int DEFAULT_TOPK = 10;
static const double SCORE_TOL = 1e-4;   /* cross-check |float core_flat - double oracle| tolerance */
static const double TIE_EPS = 1e-6;     /* adjacent oracle score gap below this counts as a tie */
static const double NORM_TOL = 1e-3;    /* L2 norm tolerance for normalized vectors */

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
	return ((v & 0x000000FFu) << 24) |
	       ((v & 0x0000FF00u) << 8) |
	       ((v & 0x00FF0000u) >> 8) |
	       ((v & 0xFF000000u) >> 24);
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

/* ------------------------------------------------------------------------ */
/* JSON string escaping (write side)                                        */
/* ------------------------------------------------------------------------ */

static void json_write_escaped_string(FILE *f, const char *s) {
	const unsigned char *p;

	fputc('"', f);
	for (p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':
			fputs("\\\"", f);
			break;
		case '\\':
			fputs("\\\\", f);
			break;
		case '\n':
			fputs("\\n", f);
			break;
		case '\r':
			fputs("\\r", f);
			break;
		case '\t':
			fputs("\\t", f);
			break;
		case '\b':
			fputs("\\b", f);
			break;
		case '\f':
			fputs("\\f", f);
			break;
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

/* ------------------------------------------------------------------------ */
/* SHA-256 (OpenSSL EVP)                                                    */
/* ------------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------------ */
/* vector helpers                                                            */
/* ------------------------------------------------------------------------ */

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

/* Independent oracle: double-precision dot product (a genuinely different
 * accumulation path from core_flat's float scalar kernel). */
static double dot_double(const float *a, const float *b, int dim) {
	double s = 0.0;
	for (int i = 0; i < dim; i++) {
		s += (double)a[i] * (double)b[i];
	}
	return s;
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
			case 'n':
				out[i++] = '\n';
				break;
			case 't':
				out[i++] = '\t';
				break;
			case 'r':
				out[i++] = '\r';
				break;
			case '"':
				out[i++] = '"';
				break;
			case '\\':
				out[i++] = '\\';
				break;
			default:
				out[i++] = *v;
			}
			v++;
		} else {
			out[i++] = *v++;
		}
	}
	out[i] = '\0';
	if (*v != '"') {
		return -1;
	}
	return 0;
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

static int json_get_bool(const char *text, const char *key, int *out) {
	const char *v = json_find_key(text, key);

	if (!v) {
		return -1;
	}
	if (strncmp(v, "true", 4) == 0) {
		*out = 1;
		return 0;
	}
	if (strncmp(v, "false", 5) == 0) {
		*out = 0;
		return 0;
	}
	return -1;
}

static char *read_whole_file(const char *path, long *out_len) {
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
	if (out_len) {
		*out_len = sz;
	}
	return buf;
}

/* ------------------------------------------------------------------------ */
/* f32 cache loading (row-major little-endian float32)                      */
/* ------------------------------------------------------------------------ */

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
		"Must be run from the repository root. Consumes only the B1 cache;\n"
		"never calls ONNX. Ground truth is produced by the core_flat exact IP scan.\n",
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
/* B1 manifest verification (schema + checksums + shape)                    */
/* ------------------------------------------------------------------------ */

typedef struct {
	long long record_count;
	long long query_count;
	long long dim;
	char metric[64];
	char manifest_sha[65];
	char emb_sha[65];
	char qemb_sha[65];
	char rj_sha[65];
	char qj_sha[65];
} b1_cache_info_t;

/* Verifies one output artifact's sha256 against the manifest block and returns
 * the recorded sha in out_sha. */
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
		fprintf(stderr, "[error] manifest outputs.%s.sha256 missing/invalid\n", key);
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

static int verify_b1_cache(const char *dataset_dir, const char *dataset_id, b1_cache_info_t *out) {
	char mf_path[1200];
	char *buf;
	char *params = NULL;
	char *outputs = NULL;
	char dataset_id_val[256];
	char dtype[32] = {0}, byte_order[32] = {0};
	int normalized = 0;
	long long schema_version = 0;
	int rc = -1;

	snprintf(mf_path, sizeof(mf_path), "%s/manifest.json", dataset_dir);
	buf = read_whole_file(mf_path, NULL);
	if (!buf) {
		fprintf(stderr, "[error] cannot read B1 manifest: %s\n", mf_path);
		return -1;
	}
	if (sha256_file(mf_path, out->manifest_sha) != 0) {
		fprintf(stderr, "[error] cannot checksum B1 manifest\n");
		free(buf);
		return -1;
	}

	if (json_get_int64(buf, "schema_version", &schema_version) != 0 || schema_version != 1) {
		fprintf(stderr, "[error] B1 manifest schema_version != 1 (got %lld)\n", schema_version);
		goto done;
	}
	if (json_get_string(buf, "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0 ||
	    strcmp(dataset_id_val, dataset_id) != 0) {
		fprintf(stderr, "[error] B1 manifest dataset_id mismatch (got '%s', want '%s')\n",
			dataset_id_val, dataset_id);
		goto done;
	}

	params = json_extract_object(buf, "params");
	if (!params) {
		fprintf(stderr, "[error] B1 manifest missing params\n");
		goto done;
	}
	if (json_get_int64(params, "record_count", &out->record_count) != 0 || out->record_count <= 0 ||
	    json_get_int64(params, "query_count", &out->query_count) != 0 || out->query_count <= 0 ||
	    json_get_int64(params, "dim", &out->dim) != 0 || out->dim <= 0) {
		fprintf(stderr, "[error] B1 manifest params counts/dim invalid\n");
		goto done;
	}
	if (json_get_string(params, "dtype", dtype, sizeof(dtype)) != 0 || strcmp(dtype, "float32") != 0) {
		fprintf(stderr, "[error] B1 manifest params.dtype != float32 (got '%s')\n", dtype);
		goto done;
	}
	if (json_get_string(params, "byte_order", byte_order, sizeof(byte_order)) != 0 ||
	    strcmp(byte_order, "little") != 0) {
		fprintf(stderr, "[error] B1 manifest params.byte_order != little (got '%s')\n", byte_order);
		goto done;
	}
	if (json_get_string(params, "metric", out->metric, sizeof(out->metric)) != 0 ||
	    strcmp(out->metric, "ip_normalized") != 0) {
		fprintf(stderr, "[error] B1 manifest params.metric != ip_normalized (got '%s')\n", out->metric);
		goto done;
	}
	if (json_get_bool(params, "normalized", &normalized) != 0 || !normalized) {
		fprintf(stderr, "[error] B1 manifest params.normalized != true\n");
		goto done;
	}

	outputs = json_extract_object(buf, "outputs");
	if (!outputs) {
		fprintf(stderr, "[error] B1 manifest missing outputs\n");
		goto done;
	}
	if (verify_output_sha(outputs, "embeddings_f32", dataset_dir, "embeddings.f32", out->emb_sha) != 0 ||
	    verify_output_sha(outputs, "query_embeddings_f32", dataset_dir, "query_embeddings.f32", out->qemb_sha) != 0 ||
	    verify_output_sha(outputs, "records_jsonl", dataset_dir, "records.jsonl", out->rj_sha) != 0 ||
	    verify_output_sha(outputs, "queries_jsonl", dataset_dir, "queries.jsonl", out->qj_sha) != 0) {
		goto done;
	}

	rc = 0;

done:
	free(params);
	free(outputs);
	free(buf);
	return rc;
}

/* Verifies that line n of a B1 jsonl artifact carries key == n (0-based), so the
 * embedding row index used as core_flat record_id maps 1:1 back to the artifact.
 * This turns the "record_id == records.jsonl id" / "query_row == queries.jsonl
 * query_row" assumption into a checked invariant. */
static int verify_row_id_mapping(const char *dataset_dir, const char *filename,
				 const char *key, long long expected_count) {
	char path[1200];
	FILE *f;
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	long long lineno = 0;
	int rc = -1;

	snprintf(path, sizeof(path), "%s/%s", dataset_dir, filename);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "[error] cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	while ((len = getline(&line, &cap, f)) != -1) {
		long long v = -1;
		if (json_get_int64(line, key, &v) != 0) {
			fprintf(stderr, "[error] %s line %lld missing key '%s'\n", filename, lineno + 1, key);
			goto done;
		}
		if (v != lineno) {
			fprintf(stderr, "[error] %s line %lld: %s=%lld != row %lld (id/row mapping broken)\n",
				filename, lineno + 1, key, v, lineno);
			goto done;
		}
		lineno++;
	}
	if (lineno != expected_count) {
		fprintf(stderr, "[error] %s has %lld rows, expected %lld\n", filename, lineno, expected_count);
		goto done;
	}
	rc = 0;

done:
	free(line);
	fclose(f);
	return rc;
}

/* ------------------------------------------------------------------------ */
/* independent brute-force oracle top-K                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
	uint64_t record_id;
	double score;
} oracle_item_t;

static int oracle_cmp(const void *a, const void *b) {
	const oracle_item_t *x = a;
	const oracle_item_t *y = b;
	if (x->score > y->score) {
		return -1;
	}
	if (x->score < y->score) {
		return 1;
	}
	/* tie-break: record_id ascending == core_flat membership (lowest slot wins) */
	if (x->record_id < y->record_id) {
		return -1;
	}
	if (x->record_id > y->record_id) {
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* generate                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
	uint64_t queries_checked;
	uint64_t set_mismatch;
	uint64_t score_mismatch;
	uint64_t strict_order_checked;
	uint64_t strict_order_mismatch;
	double max_score_abs_diff;
} crosscheck_stats_t;

static int cmd_generate(const cli_args_t *args) {
	uint64_t total_start = now_us();
	char dataset_dir[1024];
	char gt_path[1200], gtm_path[1200];
	char gt_tmp[1200] = {0}, gtm_tmp[1200] = {0};
	b1_cache_info_t info;
	float *base = NULL, *query = NULL;
	kvs_flat_index_t flat;
	int flat_ready = 0;
	FILE *gt = NULL;
	int rc = 1;
	int topk;
	long long count, qcount, dim;
	uint64_t cache_load_us = 0, index_build_us = 0, search_total_us = 0, crosscheck_us = 0;
	crosscheck_stats_t cc = {0};
	char gt_sha[65];
	long long gt_size = 0;
	char source_sha[65];
	int have_source_sha = 0;
	char created_at[64];
	oracle_item_t *oracle = NULL;
	kvs_vector_hit_t *hits = NULL;

	memset(&info, 0, sizeof(info));
	memset(&flat, 0, sizeof(flat));

	snprintf(dataset_dir, sizeof(dataset_dir), "%s/%s", args->out_root, args->dataset_id);
	if (!path_exists(dataset_dir)) {
		fprintf(stderr, "[error] dataset directory does not exist: %s\n", dataset_dir);
		return 1;
	}
	snprintf(gt_path, sizeof(gt_path), "%s/groundtruth.jsonl", dataset_dir);
	snprintf(gtm_path, sizeof(gtm_path), "%s/groundtruth.manifest.json", dataset_dir);
	if (path_exists(gt_path) || path_exists(gtm_path)) {
		fprintf(stderr, "[error] ground truth already exists in %s (refusing to overwrite; remove groundtruth.jsonl and groundtruth.manifest.json to regenerate)\n", dataset_dir);
		return 1;
	}

	/* 1. verify B1 cache (schema + checksums + shape) -> rejects corrupt input */
	if (verify_b1_cache(dataset_dir, args->dataset_id, &info) != 0) {
		return 1;
	}
	count = info.record_count;
	qcount = info.query_count;
	dim = info.dim;
	topk = args->topk;
	if ((long long)topk > count) {
		fprintf(stderr, "[error] topk (%d) exceeds record_count (%lld)\n", topk, count);
		return 1;
	}
	/* records.jsonl id / queries.jsonl query_row must equal their row index so
	 * the core_flat record_id (= embedding row) is back-joinable to the text. */
	if (verify_row_id_mapping(dataset_dir, "records.jsonl", "id", count) != 0 ||
	    verify_row_id_mapping(dataset_dir, "queries.jsonl", "query_row", qcount) != 0) {
		return 1;
	}
	fprintf(stdout, "[b1-cache] record_count=%lld query_count=%lld dim=%lld metric=%s (checksums + row/id mapping verified)\n",
		count, qcount, dim, info.metric);

	/* 2. load caches */
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
	/* finite + norm sanity on loaded cache */
	for (long long i = 0; i < count; i++) {
		if (!row_all_finite(base + i * dim, (int)dim) ||
		    fabs(row_norm(base + i * dim, (int)dim) - 1.0) > NORM_TOL) {
			fprintf(stderr, "[error] record row %lld not finite/normalized\n", i);
			goto cleanup;
		}
	}
	for (long long i = 0; i < qcount; i++) {
		if (!row_all_finite(query + i * dim, (int)dim) ||
		    fabs(row_norm(query + i * dim, (int)dim) - 1.0) > NORM_TOL) {
			fprintf(stderr, "[error] query row %lld not finite/normalized\n", i);
			goto cleanup;
		}
	}

	/* 3. runtime backend init (required by libkvstore core index allocator) */
	memset(&g_cfg, 0, sizeof(g_cfg));
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.vector_distance_backend, sizeof(g_cfg.vector_distance_backend), "%s", "auto");
	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);

	/* 4. build core_flat exact IP index; record_id = row = records.jsonl id */
	{
		uint64_t t0 = now_us();
		if (kvs_flat_index_init(&flat, (size_t)dim, KVS_VECTOR_METRIC_IP) != KVS_VECTOR_OK) {
			fprintf(stderr, "[error] kvs_flat_index_init failed\n");
			goto cleanup;
		}
		flat_ready = 1;
		if (kvs_flat_index_reserve(&flat, (size_t)count) != KVS_VECTOR_OK) {
			fprintf(stderr, "[error] kvs_flat_index_reserve failed\n");
			goto cleanup;
		}
		for (long long i = 0; i < count; i++) {
			if (kvs_flat_index_add(&flat, (uint64_t)i, base + i * dim, (size_t)dim) != KVS_VECTOR_OK) {
				fprintf(stderr, "[error] kvs_flat_index_add failed at row %lld\n", i);
				goto cleanup;
			}
		}
		index_build_us = now_us() - t0;
	}

	hits = malloc(sizeof(kvs_vector_hit_t) * (size_t)topk);
	oracle = malloc(sizeof(oracle_item_t) * (size_t)count);
	if (!hits || !oracle) {
		fprintf(stderr, "[error] oom for search buffers\n");
		goto cleanup;
	}

	/* 5. open temp ground truth, search all queries via core_flat exact scan */
	snprintf(gt_tmp, sizeof(gt_tmp), "%s/.tmp.groundtruth.%d.jsonl", dataset_dir, (int)getpid());
	snprintf(gtm_tmp, sizeof(gtm_tmp), "%s/.tmp.groundtruth.manifest.%d.json", dataset_dir, (int)getpid());
	gt = fopen(gt_tmp, "w");
	if (!gt) {
		fprintf(stderr, "[error] cannot open temp ground truth file %s\n", gt_tmp);
		goto cleanup;
	}

	for (long long q = 0; q < qcount; q++) {
		size_t out_count = 0;
		const float *qv = query + q * dim;
		uint64_t t0 = now_us();
		int src = kvs_flat_index_search(&flat, qv, (size_t)dim, (size_t)topk, hits, &out_count);
		search_total_us += (now_us() - t0);
		if (src != KVS_VECTOR_OK) {
			fprintf(stderr, "[error] core_flat search failed at query %lld (rc=%d)\n", q, src);
			goto cleanup;
		}
		if (out_count != (size_t)topk) {
			fprintf(stderr, "[error] query %lld returned %zu hits, expected %d\n", q, out_count, topk);
			goto cleanup;
		}
		/* per-query structural checks: monotonic score, finite, unique/in-range ids */
		for (int r = 0; r < topk; r++) {
			if (!isfinite(hits[r].score)) {
				fprintf(stderr, "[error] query %lld rank %d non-finite score\n", q, r);
				goto cleanup;
			}
			if (hits[r].record_id >= (uint64_t)count) {
				fprintf(stderr, "[error] query %lld rank %d record_id %" PRIu64 " out of range\n",
					q, r, hits[r].record_id);
				goto cleanup;
			}
			if (r > 0 && hits[r].score > hits[r - 1].score) {
				fprintf(stderr, "[error] query %lld scores not monotonic at rank %d\n", q, r);
				goto cleanup;
			}
			for (int s = 0; s < r; s++) {
				if (hits[s].record_id == hits[r].record_id) {
					fprintf(stderr, "[error] query %lld duplicate record_id at ranks %d,%d\n", q, s, r);
					goto cleanup;
				}
			}
		}

		/* 6. independent double-precision brute-force cross-check (all queries) */
		{
			uint64_t c0 = now_us();
			for (long long i = 0; i < count; i++) {
				oracle[i].record_id = (uint64_t)i;
				oracle[i].score = dot_double(qv, base + i * dim, (int)dim);
			}
			qsort(oracle, (size_t)count, sizeof(oracle_item_t), oracle_cmp);
			crosscheck_us += (now_us() - c0);

			cc.queries_checked++;
			/* set match: multiset of top-K record_ids must be identical */
			int set_ok = 1;
			for (int r = 0; r < topk; r++) {
				int found = 0;
				for (int s = 0; s < topk; s++) {
					if (hits[s].record_id == oracle[r].record_id) {
						found = 1;
						break;
					}
				}
				if (!found) {
					set_ok = 0;
					break;
				}
			}
			if (!set_ok) {
				cc.set_mismatch++;
				fprintf(stderr, "[error] query %lld: core_flat top-K set != brute-force top-K set\n", q);
				goto cleanup;
			}
			/* position-wise score match (i-th largest score is unambiguous) */
			int score_ok = 1;
			for (int r = 0; r < topk; r++) {
				double d = fabs((double)hits[r].score - oracle[r].score);
				if (d > cc.max_score_abs_diff) {
					cc.max_score_abs_diff = d;
				}
				if (d > SCORE_TOL) {
					score_ok = 0;
				}
			}
			if (!score_ok) {
				cc.score_mismatch++;
				fprintf(stderr, "[error] query %lld: score cross-check exceeded tolerance %g\n", q, SCORE_TOL);
				goto cleanup;
			}
			/* strict order match only where the top-K has no near-ties */
			int distinct = 1;
			for (int r = 1; r < topk; r++) {
				if (fabs(oracle[r].score - oracle[r - 1].score) <= TIE_EPS) {
					distinct = 0;
					break;
				}
			}
			if (distinct) {
				cc.strict_order_checked++;
				for (int r = 0; r < topk; r++) {
					if (hits[r].record_id != oracle[r].record_id) {
						cc.strict_order_mismatch++;
						fprintf(stderr, "[error] query %lld: strict order mismatch at rank %d (core_flat=%" PRIu64 " oracle=%" PRIu64 ")\n",
							q, r, hits[r].record_id, oracle[r].record_id);
						goto cleanup;
					}
				}
			}
		}

		/* 7. write ground truth line (core_flat output verbatim) */
		{
			char query_id[32];
			snprintf(query_id, sizeof(query_id), "q%06lld", q);
			fprintf(gt, "{\"query_id\":");
			json_write_escaped_string(gt, query_id);
			fprintf(gt, ",\"query_row\":%lld,\"topk\":%d,\"metric\":\"ip_normalized\",\"neighbors\":[", q, topk);
			for (int r = 0; r < topk; r++) {
				/* %.9g round-trips an IEEE-754 float exactly and stays JSON-valid
				 * (scores are asserted finite above, so never nan/inf). */
				fprintf(gt, "%s{\"rank\":%d,\"record_id\":%" PRIu64 ",\"score\":%.9g}",
					r == 0 ? "" : ",", r + 1, hits[r].record_id, (double)hits[r].score);
			}
			fprintf(gt, "]}\n");
		}
		if (ferror(gt)) {
			fprintf(stderr, "[error] write error on ground truth at query %lld\n", q);
			goto cleanup;
		}
	}

	if (fflush(gt) != 0 || ferror(gt)) {
		fprintf(stderr, "[error] flush error on ground truth\n");
		goto cleanup;
	}
	fclose(gt);
	gt = NULL;

	fprintf(stdout, "[search] queries=%lld topk=%d search_total_us=%" PRIu64 " search_avg_us=%" PRIu64 "\n",
		qcount, topk, search_total_us, qcount ? search_total_us / (uint64_t)qcount : 0);
	fprintf(stdout, "[crosscheck] queries_checked=%" PRIu64 " strict_order_checked=%" PRIu64
		" max_score_abs_diff=%.3g (set/score/order all consistent)\n",
		cc.queries_checked, cc.strict_order_checked, cc.max_score_abs_diff);

	/* checksums + sizes for the published ground truth */
	if (sha256_file(gt_tmp, gt_sha) != 0) {
		fprintf(stderr, "[error] cannot checksum ground truth\n");
		goto cleanup;
	}
	{
		struct stat st;
		if (stat(gt_tmp, &st) != 0) {
			fprintf(stderr, "[error] cannot stat ground truth temp file: %s\n", strerror(errno));
			goto cleanup;
		}
		gt_size = (long long)st.st_size;
	}
	have_source_sha = (sha256_file(GEN_SOURCE_REL_PATH, source_sha) == 0);
	if (!have_source_sha) {
		fprintf(stderr, "[warn] could not read generator source at %s (run from repo root to capture source_sha256)\n", GEN_SOURCE_REL_PATH);
	}
	iso8601_utc_now(created_at, sizeof(created_at));

	/* 8. write ground truth manifest (temp) */
	{
		FILE *mf = fopen(gtm_tmp, "w");
		uint64_t total_us;
		if (!mf) {
			fprintf(stderr, "[error] cannot open temp ground truth manifest\n");
			goto cleanup;
		}
		total_us = now_us() - total_start;
		fprintf(mf, "{\n");
		fprintf(mf, "  \"schema_version\": 1,\n");
		fprintf(mf, "  \"dataset_id\": ");
		json_write_escaped_string(mf, args->dataset_id);
		fprintf(mf, ",\n  \"created_at_utc\": ");
		json_write_escaped_string(mf, created_at);
		fprintf(mf, ",\n  \"generator\": {\"name\": \"generate_real_embedding_groundtruth\", \"version\": ");
		json_write_escaped_string(mf, GEN_VERSION);
		fprintf(mf, ", \"source_sha256\": ");
		if (have_source_sha) {
			json_write_escaped_string(mf, source_sha);
		} else {
			fprintf(mf, "null");
		}
		fprintf(mf, "},\n");
		fprintf(mf, "  \"source\": {\"manifest_path\": \"manifest.json\", \"manifest_sha256\": ");
		json_write_escaped_string(mf, info.manifest_sha);
		fprintf(mf, ", \"embeddings_sha256\": ");
		json_write_escaped_string(mf, info.emb_sha);
		fprintf(mf, ", \"query_embeddings_sha256\": ");
		json_write_escaped_string(mf, info.qemb_sha);
		fprintf(mf, ", \"records_jsonl_sha256\": ");
		json_write_escaped_string(mf, info.rj_sha);
		fprintf(mf, ", \"queries_jsonl_sha256\": ");
		json_write_escaped_string(mf, info.qj_sha);
		fprintf(mf, "},\n");
		fprintf(mf,
			"  \"params\": {\"record_count\": %lld, \"query_count\": %lld, \"dim\": %lld, \"topk\": %d, "
			"\"metric\": \"ip_normalized\", \"ground_truth_source\": \"core_flat_exact_ip\", "
			"\"tie_break\": \"score_desc__membership_slot_asc_eq_record_id_asc\"},\n",
			count, qcount, dim, topk);
		fprintf(mf,
			"  \"outputs\": {\"groundtruth_jsonl\": {\"path\": \"groundtruth.jsonl\", \"sha256\": ");
		json_write_escaped_string(mf, gt_sha);
		fprintf(mf, ", \"size_bytes\": %lld, \"rows\": %lld}},\n", gt_size, qcount);
		fprintf(mf,
			"  \"crosscheck\": {\"method\": \"independent_double_precision_bruteforce\", "
			"\"queries_checked\": %" PRIu64 ", \"score_tolerance\": %g, \"tie_eps\": %g, "
			"\"set_match_all\": true, \"strict_order_checked\": %" PRIu64 ", \"strict_order_match_all\": true, "
			"\"max_score_abs_diff\": %.9g},\n",
			cc.queries_checked, SCORE_TOL, TIE_EPS, cc.strict_order_checked, cc.max_score_abs_diff);
		fprintf(mf,
			"  \"timing_us\": {\"cache_load_us\": %" PRIu64 ", \"index_build_us\": %" PRIu64
			", \"search_total_us\": %" PRIu64 ", \"search_avg_us\": %" PRIu64
			", \"crosscheck_us\": %" PRIu64 ", \"total_us\": %" PRIu64 "}\n",
			cache_load_us, index_build_us, search_total_us,
			qcount ? search_total_us / (uint64_t)qcount : 0, crosscheck_us, total_us);
		fprintf(mf, "}\n");
		/* mirror the gt discipline: a write error can first surface at the final
		 * flush/close, so always close and fold every failure into one check. */
		{
			int mf_err = (fflush(mf) != 0 || ferror(mf));
			if (fclose(mf) != 0) {
				mf_err = 1;
			}
			if (mf_err) {
				fprintf(stderr, "[error] write error on ground truth manifest\n");
				goto cleanup;
			}
		}
	}

	/* 9. atomic publish: rename data first, manifest last (validate keys on both) */
	if (rename(gt_tmp, gt_path) != 0) {
		fprintf(stderr, "[error] failed to publish groundtruth.jsonl: %s\n", strerror(errno));
		goto cleanup;
	}
	if (rename(gtm_tmp, gtm_path) != 0) {
		fprintf(stderr, "[error] failed to publish groundtruth.manifest.json: %s\n", strerror(errno));
		/* best-effort: remove the half-published data file so validate stays consistent */
		remove(gt_path);
		goto cleanup;
	}
	fprintf(stdout, "[publish] dataset_id=%s groundtruth=%s rows=%lld\n", args->dataset_id, gt_path, qcount);
	rc = 0;

cleanup:
	if (gt) {
		fclose(gt);
	}
	if (rc != 0) {
		if (gt_tmp[0]) {
			remove(gt_tmp);
		}
		if (gtm_tmp[0]) {
			remove(gtm_tmp);
		}
	}
	if (flat_ready) {
		kvs_flat_index_destroy(&flat);
	}
	free(base);
	free(query);
	free(hits);
	free(oracle);
	return rc;
}

/* ------------------------------------------------------------------------ */
/* validate (no core_flat, no ONNX)                                          */
/* ------------------------------------------------------------------------ */

static int cmd_validate(const cli_args_t *args) {
	char dataset_dir[1024];
	char gtm_path[1200], gt_path[1200], mf_path[1200];
	char *mbuf = NULL;
	char *params = NULL;
	char *outputs = NULL;
	char *gtblock = NULL;
	int ok = 1;
	long long schema_version = 0;
	char dataset_id_val[256];
	long long record_count = 0, query_count = 0, dim = 0, topk = 0;
	char metric[64] = {0};
	char gt_sha_manifest[80] = {0};
	long long gt_size_manifest = -1, gt_rows_manifest = -1;
	char actual_sha[65];
	char src_manifest_sha[80] = {0};
	FILE *gt = NULL;
	char *line = NULL;
	size_t linecap = 0;
	ssize_t len;
	long long lineno = 0;

	snprintf(dataset_dir, sizeof(dataset_dir), "%s/%s", args->out_root, args->dataset_id);
	snprintf(gtm_path, sizeof(gtm_path), "%s/groundtruth.manifest.json", dataset_dir);
	snprintf(gt_path, sizeof(gt_path), "%s/groundtruth.jsonl", dataset_dir);
	snprintf(mf_path, sizeof(mf_path), "%s/manifest.json", dataset_dir);

	if (!path_exists(dataset_dir)) {
		fprintf(stderr, "[fail] dataset directory missing: %s\n", dataset_dir);
		return 1;
	}
	mbuf = read_whole_file(gtm_path, NULL);
	if (!mbuf) {
		fprintf(stderr, "[fail] cannot read ground truth manifest: %s\n", gtm_path);
		return 1;
	}

	if (json_get_int64(mbuf, "schema_version", &schema_version) != 0 || schema_version != 1) {
		fprintf(stderr, "[fail] ground truth manifest schema_version != 1 (got %lld)\n", schema_version);
		ok = 0;
	}
	if (json_get_string(mbuf, "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0 ||
	    strcmp(dataset_id_val, args->dataset_id) != 0) {
		fprintf(stderr, "[fail] ground truth manifest dataset_id mismatch (got '%s')\n", dataset_id_val);
		ok = 0;
	}

	params = json_extract_object(mbuf, "params");
	if (!params) {
		fprintf(stderr, "[fail] ground truth manifest missing params\n");
		ok = 0;
	} else {
		if (json_get_int64(params, "record_count", &record_count) != 0 || record_count <= 0 ||
		    json_get_int64(params, "query_count", &query_count) != 0 || query_count <= 0 ||
		    json_get_int64(params, "dim", &dim) != 0 || dim <= 0 ||
		    json_get_int64(params, "topk", &topk) != 0 || topk <= 0) {
			fprintf(stderr, "[fail] ground truth manifest params invalid\n");
			ok = 0;
		}
		if (json_get_string(params, "metric", metric, sizeof(metric)) != 0 || strcmp(metric, "ip_normalized") != 0) {
			fprintf(stderr, "[fail] ground truth manifest params.metric != ip_normalized (got '%s')\n", metric);
			ok = 0;
		}
	}

	/* source manifest sha must match the current B1 manifest.json (ground truth
	 * must correspond to the cache it was built from) */
	{
		char *src = json_extract_object(mbuf, "source");
		if (!src || json_get_string(src, "manifest_sha256", src_manifest_sha, sizeof(src_manifest_sha)) != 0 ||
		    strlen(src_manifest_sha) != 64) {
			fprintf(stderr, "[fail] ground truth manifest missing source.manifest_sha256\n");
			ok = 0;
		} else if (sha256_file(mf_path, actual_sha) != 0) {
			/* provenance cannot be verified if the B1 manifest is gone/unreadable */
			fprintf(stderr, "[fail] cannot read B1 manifest.json to verify provenance: %s\n", mf_path);
			ok = 0;
		} else if (strcmp(actual_sha, src_manifest_sha) != 0) {
			fprintf(stderr, "[fail] source manifest sha256 mismatch: ground truth built from a different B1 manifest\n");
			ok = 0;
		}
		free(src);
	}

	outputs = json_extract_object(mbuf, "outputs");
	if (!outputs) {
		fprintf(stderr, "[fail] ground truth manifest missing outputs\n");
		ok = 0;
	} else {
		gtblock = json_extract_object(outputs, "groundtruth_jsonl");
		if (!gtblock ||
		    json_get_string(gtblock, "sha256", gt_sha_manifest, sizeof(gt_sha_manifest)) != 0 ||
		    strlen(gt_sha_manifest) != 64 ||
		    json_get_int64(gtblock, "size_bytes", &gt_size_manifest) != 0 ||
		    json_get_int64(gtblock, "rows", &gt_rows_manifest) != 0) {
			fprintf(stderr, "[fail] ground truth manifest outputs.groundtruth_jsonl invalid\n");
			ok = 0;
		}
	}

	if (!ok) {
		goto done;
	}

	/* checksum + size of groundtruth.jsonl */
	{
		struct stat st;
		if (stat(gt_path, &st) != 0) {
			fprintf(stderr, "[fail] groundtruth.jsonl missing: %s\n", gt_path);
			ok = 0;
			goto done;
		}
		if ((long long)st.st_size != gt_size_manifest) {
			fprintf(stderr, "[fail] groundtruth.jsonl size mismatch: manifest=%lld disk=%lld\n",
				gt_size_manifest, (long long)st.st_size);
			ok = 0;
		}
		if (sha256_file(gt_path, actual_sha) != 0 || strcmp(actual_sha, gt_sha_manifest) != 0) {
			fprintf(stderr, "[fail] groundtruth.jsonl sha256 mismatch\n");
			ok = 0;
		}
	}
	if (!ok) {
		goto done;
	}

	/* structural re-check of every ground truth line */
	gt = fopen(gt_path, "r");
	if (!gt) {
		fprintf(stderr, "[fail] cannot open groundtruth.jsonl\n");
		ok = 0;
		goto done;
	}
	while ((len = getline(&line, &linecap, gt)) != -1) {
		long long query_row = -1, line_topk = -1;
		char linemetric[64] = {0};
		const char *nb;
		double prev_score = 0.0;
		int have_prev = 0;
		int rank_expected = 1;
		uint64_t seen_ids[4096];
		int seen_n = 0;

		lineno++;
		if (json_get_int64(line, "query_row", &query_row) != 0 || query_row < 0 || query_row >= query_count) {
			fprintf(stderr, "[fail] line %lld: query_row out of range\n", lineno);
			ok = 0;
			break;
		}
		if (json_get_int64(line, "topk", &line_topk) != 0 || line_topk != topk) {
			fprintf(stderr, "[fail] line %lld: topk != %lld\n", lineno, topk);
			ok = 0;
			break;
		}
		if (json_get_string(line, "metric", linemetric, sizeof(linemetric)) != 0 ||
		    strcmp(linemetric, "ip_normalized") != 0) {
			fprintf(stderr, "[fail] line %lld: metric != ip_normalized\n", lineno);
			ok = 0;
			break;
		}
		/* walk neighbors: each "rank"/"record_id"/"score" triple in order */
		nb = strstr(line, "\"neighbors\"");
		if (!nb) {
			fprintf(stderr, "[fail] line %lld: missing neighbors\n", lineno);
			ok = 0;
			break;
		}
		for (;;) {
			const char *rk = strstr(nb, "\"rank\"");
			long long rank_v = -1, rid_v = -1;
			const char *sc;
			double score_v;
			if (!rk) {
				break;
			}
			if (json_get_int64(rk, "rank", &rank_v) != 0 || rank_v != rank_expected) {
				fprintf(stderr, "[fail] line %lld: rank not contiguous (expected %d)\n", lineno, rank_expected);
				ok = 0;
				break;
			}
			if (json_get_int64(rk, "record_id", &rid_v) != 0 || rid_v < 0 || rid_v >= record_count) {
				fprintf(stderr, "[fail] line %lld: record_id out of range\n", lineno);
				ok = 0;
				break;
			}
			sc = json_find_key(rk, "score");
			if (!sc) {
				fprintf(stderr, "[fail] line %lld: neighbor missing score\n", lineno);
				ok = 0;
				break;
			}
			score_v = strtod(sc, NULL);
			if (!isfinite(score_v)) {
				fprintf(stderr, "[fail] line %lld: non-finite score\n", lineno);
				ok = 0;
				break;
			}
			if (have_prev && score_v > prev_score + 1e-9) {
				fprintf(stderr, "[fail] line %lld: score not monotonic non-increasing at rank %lld\n", lineno, rank_v);
				ok = 0;
				break;
			}
			for (int s = 0; s < seen_n; s++) {
				if (seen_ids[s] == (uint64_t)rid_v) {
					fprintf(stderr, "[fail] line %lld: duplicate record_id %lld\n", lineno, rid_v);
					ok = 0;
					break;
				}
			}
			if (!ok) {
				break;
			}
			if (seen_n < (int)(sizeof(seen_ids) / sizeof(seen_ids[0]))) {
				seen_ids[seen_n++] = (uint64_t)rid_v;
			}
			prev_score = score_v;
			have_prev = 1;
			rank_expected++;
			nb = rk + 6;
		}
		if (!ok) {
			break;
		}
		if (rank_expected - 1 != topk) {
			fprintf(stderr, "[fail] line %lld: neighbor count %d != topk %lld\n", lineno, rank_expected - 1, topk);
			ok = 0;
			break;
		}
	}
	if (ok && lineno != query_count) {
		fprintf(stderr, "[fail] groundtruth.jsonl has %lld lines, expected query_count %lld\n", lineno, query_count);
		ok = 0;
	}

done:
	if (gt) {
		fclose(gt);
	}
	free(line);
	free(gtblock);
	free(outputs);
	free(params);
	free(mbuf);
	if (ok) {
		fprintf(stdout, "[validate] PASS dataset_id=%s query_count=%lld topk=%lld record_count=%lld dim=%lld\n",
			args->dataset_id, query_count, topk, record_count, dim);
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
