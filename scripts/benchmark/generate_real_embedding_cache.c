/*
 * B1-fixture-cache-generation: real embedding cache generator.
 *
 * Reuses the production ONNX embedding runtime (src/embedding/embedding_runtime_onnx.c)
 * and kvs_config_init_default() (src/core/kvs_config.c) directly through the C ABI in
 * include/embedding_c_api.h. Does not link src/embedding/embedding_runtime.c (hash
 * fallback) and does not go through src/vector/kvs_vector_embedding.c's provider
 * dispatch, so hash-fallback embeddings are structurally impossible from this binary.
 *
 * Subcommands:
 *   generate  - read TSV fixtures, run real ONNX inference, write embeddings.f32 /
 *               query_embeddings.f32 / records.jsonl / queries.jsonl / manifest.json
 *               into a temp dir, validate, then atomically publish.
 *   validate  - re-check an existing dataset directory (manifest schema, checksums,
 *               shapes, finite/norm) without ever calling ONNX.
 *
 * Must be run from the repository root (same convention as the tests/ shell scripts).
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
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
#include <onnxruntime_c_api.h>

#include "embedding_c_api.h"
#include "kvs_config.h"

/* g_cfg is required by src/embedding/embedding_runtime_onnx.c (extern kvs_config_t g_cfg). */
kvs_config_t g_cfg;

static const char *const GEN_SOURCE_REL_PATH = "scripts/benchmark/generate_real_embedding_cache.c";
static const char *const GEN_VERSION = "1";

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

static int mkdir_p(const char *path) {
	char tmp[1024];
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/') {
		tmp[len - 1] = '\0';
	}
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
				return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
		return -1;
	}
	return 0;
}

static void rm_rf(const char *path) {
	struct stat st;
	DIR *d;
	struct dirent *ent;
	char child[2048];

	if (lstat(path, &st) != 0) {
		return;
	}
	if (S_ISDIR(st.st_mode)) {
		d = opendir(path);
		if (!d) {
			return;
		}
		while ((ent = readdir(d)) != NULL) {
			if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
				continue;
			}
			snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
			rm_rf(child);
		}
		closedir(d);
		rmdir(path);
	} else {
		unlink(path);
	}
}

static void dirname_of(const char *path, char *out, size_t outsz) {
	const char *slash = strrchr(path, '/');
	size_t len;

	if (!slash) {
		snprintf(out, outsz, ".");
		return;
	}
	len = (size_t)(slash - path);
	if (len >= outsz) {
		len = outsz - 1;
	}
	memcpy(out, path, len);
	out[len] = '\0';
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
/* vector math helpers                                                      */
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

/* Mirrors kvs_vector_embedding.c's L2 normalize: double precision accumulate,
 * in place divide. The ONNX runtime itself never normalizes. */
static double l2_normalize_row(float *row, int dim) {
	double norm = row_norm(row, dim);
	if (norm > 1e-12) {
		for (int i = 0; i < dim; i++) {
			row[i] = (float)((double)row[i] / norm);
		}
	}
	return norm;
}

static double cosine(const float *a, const float *b, int dim) {
	double dot = 0.0, na = 0.0, nb = 0.0;
	for (int i = 0; i < dim; i++) {
		dot += (double)a[i] * (double)b[i];
		na += (double)a[i] * (double)a[i];
		nb += (double)b[i] * (double)b[i];
	}
	double denom = sqrt(na) * sqrt(nb);
	if (denom < 1e-12) {
		return 0.0;
	}
	return dot / denom;
}

/* ------------------------------------------------------------------------ */
/* little-endian f32 IO (explicit host-endianness check + swap)             */
/* ------------------------------------------------------------------------ */

static void write_f32_row_le(FILE *f, const float *row, int dim) {
	if (host_is_little_endian()) {
		fwrite(row, sizeof(float), (size_t)dim, f);
		return;
	}
	for (int i = 0; i < dim; i++) {
		uint32_t v, sw;
		memcpy(&v, &row[i], sizeof(v));
		sw = swap_u32(v);
		fwrite(&sw, sizeof(sw), 1, f);
	}
}

static int read_f32_row_le(FILE *f, float *row, int dim) {
	if (fread(row, sizeof(float), (size_t)dim, f) != (size_t)dim) {
		return -1;
	}
	if (!host_is_little_endian()) {
		for (int i = 0; i < dim; i++) {
			uint32_t v;
			memcpy(&v, &row[i], sizeof(v));
			v = swap_u32(v);
			memcpy(&row[i], &v, sizeof(v));
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* TSV reader                                                                */
/* ------------------------------------------------------------------------ */

typedef struct {
	char *col1; /* question / query text - the embedding source */
	char *col2; /* answer / expected_answer */
	size_t source_line; /* 1-based */
} tsv_row_t;

typedef struct {
	tsv_row_t *rows;
	size_t count;
} tsv_table_t;

static void tsv_table_free(tsv_table_t *t) {
	if (!t->rows) {
		return;
	}
	for (size_t i = 0; i < t->count; i++) {
		free(t->rows[i].col1);
		free(t->rows[i].col2);
	}
	free(t->rows);
	t->rows = NULL;
	t->count = 0;
}

static int read_tsv(const char *path, tsv_table_t *out) {
	FILE *f;
	size_t cap = 1024;
	tsv_row_t *rows;
	size_t count = 0;
	char *line = NULL;
	size_t linecap = 0;
	ssize_t len;
	size_t lineno = 0;

	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "[error] cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	rows = malloc(cap * sizeof(tsv_row_t));
	if (!rows) {
		fclose(f);
		return -1;
	}

	while ((len = getline(&line, &linecap, f)) != -1) {
		lineno++;
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}
		if (len == 0) {
			fprintf(stderr, "[error] %s:%zu empty line, expected 2 tab-separated columns\n", path, lineno);
			goto fail;
		}
		char *tab = memchr(line, '\t', (size_t)len);
		if (!tab) {
			fprintf(stderr, "[error] %s:%zu missing tab delimiter\n", path, lineno);
			goto fail;
		}
		char *tab2 = memchr(tab + 1, '\t', (size_t)(line + len - (tab + 1)));
		if (tab2) {
			fprintf(stderr, "[error] %s:%zu expected exactly 2 columns, found an extra tab\n", path, lineno);
			goto fail;
		}
		if (count == cap) {
			tsv_row_t *grown;
			cap *= 2;
			grown = realloc(rows, cap * sizeof(tsv_row_t));
			if (!grown) {
				goto fail;
			}
			rows = grown;
		}
		*tab = '\0';
		rows[count].col1 = strdup(line);
		rows[count].col2 = strdup(tab + 1);
		rows[count].source_line = lineno;
		if (!rows[count].col1 || !rows[count].col2 ||
		    rows[count].col1[0] == '\0' || rows[count].col2[0] == '\0') {
			fprintf(stderr, "[error] %s:%zu empty column value\n", path, lineno);
			free(rows[count].col1);
			free(rows[count].col2);
			goto fail;
		}
		count++;
	}
	free(line);
	fclose(f);
	if (count == 0) {
		fprintf(stderr, "[error] %s: no rows found\n", path);
		free(rows);
		return -1;
	}
	out->rows = rows;
	out->count = count;
	return 0;

fail:
	free(line);
	fclose(f);
	for (size_t i = 0; i < count; i++) {
		free(rows[i].col1);
		free(rows[i].col2);
	}
	free(rows);
	return -1;
}

/* ------------------------------------------------------------------------ */
/* CLI args                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
	const char *dataset_id;
	const char *records_tsv;
	const char *queries_tsv;
	const char *model_path;
	int dim;
	int batch_size;
	int num_threads;
	const char *out_root;
} cli_args_t;

static void cli_args_default(cli_args_t *a) {
	a->dataset_id = NULL;
	a->records_tsv = "tests/fixtures/qa_zh_1000.tsv";
	a->queries_tsv = "tests/fixtures/qa_zh_queries.tsv";
	a->model_path = "models/bge-small-zh-v1.5/model.onnx";
	a->dim = 512;
	a->batch_size = 32;
	a->num_threads = 1;
	a->out_root = "/tmp/kvlite_vector_benchmark_artifacts/real_embedding";
}

static void print_usage(const char *prog) {
	fprintf(stderr,
		"usage:\n"
		"  %s generate --dataset-id <id> [--records-tsv PATH] [--queries-tsv PATH]\n"
		"                 [--model-path PATH] [--dim N] [--batch-size N]\n"
		"                 [--num-threads N] [--out-root DIR]\n"
		"  %s validate --dataset-id <id> [--out-root DIR]\n"
		"Must be run from the repository root.\n",
		prog, prog);
}

static int parse_args(int argc, char **argv, cli_args_t *a) {
	for (int i = 2; i < argc; i++) {
		const char *key = argv[i];
		if (strcmp(key, "--dataset-id") == 0 && i + 1 < argc) {
			a->dataset_id = argv[++i];
		} else if (strcmp(key, "--records-tsv") == 0 && i + 1 < argc) {
			a->records_tsv = argv[++i];
		} else if (strcmp(key, "--queries-tsv") == 0 && i + 1 < argc) {
			a->queries_tsv = argv[++i];
		} else if (strcmp(key, "--model-path") == 0 && i + 1 < argc) {
			a->model_path = argv[++i];
		} else if (strcmp(key, "--dim") == 0 && i + 1 < argc) {
			a->dim = atoi(argv[++i]);
		} else if (strcmp(key, "--batch-size") == 0 && i + 1 < argc) {
			a->batch_size = atoi(argv[++i]);
		} else if (strcmp(key, "--num-threads") == 0 && i + 1 < argc) {
			a->num_threads = atoi(argv[++i]);
		} else if (strcmp(key, "--out-root") == 0 && i + 1 < argc) {
			a->out_root = argv[++i];
		} else {
			fprintf(stderr, "[error] unrecognized argument: %s\n", key);
			return -1;
		}
	}
	if (!a->dataset_id || a->dataset_id[0] == '\0') {
		fprintf(stderr, "[error] --dataset-id is required\n");
		return -1;
	}
	if (a->dim <= 0 || a->batch_size <= 0 || a->num_threads <= 0) {
		fprintf(stderr, "[error] --dim/--batch-size/--num-threads must be positive\n");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* sanity gate                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
	double determinism_max_abs_diff;
	double determinism_cosine;
	double distinct_cosine;
} sanity_result_t;

static int run_sanity_gate(EmbeddingHandle *h, tsv_table_t *records, int dim, sanity_result_t *out) {
	float *v0 = NULL, *v0b = NULL, *v1 = NULL, *v2 = NULL;
	int rc = -1;

	if (records->count < 3) {
		fprintf(stderr, "[error] sanity gate requires >=3 record rows, found %zu\n", records->count);
		return -1;
	}

	v0 = malloc(sizeof(float) * (size_t)dim);
	v0b = malloc(sizeof(float) * (size_t)dim);
	v1 = malloc(sizeof(float) * (size_t)dim);
	v2 = malloc(sizeof(float) * (size_t)dim);
	if (!v0 || !v0b || !v1 || !v2) {
		fprintf(stderr, "[error] oom in sanity gate\n");
		goto done;
	}

	const char *t0 = records->rows[0].col1;
	const char *t1 = records->rows[1].col1;
	const char *t2 = records->rows[2].col1;

	if (embedding_encode(h, t0, strlen(t0), v0, dim) != EMBEDDING_OK) {
		fprintf(stderr, "[error] sanity encode t0 failed: %s\n", embedding_last_error(h));
		goto done;
	}
	if (embedding_encode(h, t0, strlen(t0), v0b, dim) != EMBEDDING_OK) {
		fprintf(stderr, "[error] sanity encode t0(repeat) failed: %s\n", embedding_last_error(h));
		goto done;
	}
	if (embedding_encode(h, t1, strlen(t1), v1, dim) != EMBEDDING_OK) {
		fprintf(stderr, "[error] sanity encode t1 failed: %s\n", embedding_last_error(h));
		goto done;
	}
	if (embedding_encode(h, t2, strlen(t2), v2, dim) != EMBEDDING_OK) {
		fprintf(stderr, "[error] sanity encode t2 failed: %s\n", embedding_last_error(h));
		goto done;
	}

	if (!row_all_finite(v0, dim) || !row_all_finite(v0b, dim) ||
	    !row_all_finite(v1, dim) || !row_all_finite(v2, dim)) {
		fprintf(stderr, "[error] sanity gate: non-finite embedding value detected\n");
		goto done;
	}

	double max_abs_diff = 0.0;
	for (int i = 0; i < dim; i++) {
		double d = fabs((double)v0[i] - (double)v0b[i]);
		if (d > max_abs_diff) {
			max_abs_diff = d;
		}
	}
	double det_cos = cosine(v0, v0b, dim);
	if (max_abs_diff > 1e-6 || det_cos < 0.999999) {
		fprintf(stderr, "[error] determinism sanity failed: max_abs_diff=%.9f cosine=%.9f\n", max_abs_diff, det_cos);
		goto done;
	}

	l2_normalize_row(v0, dim);
	l2_normalize_row(v1, dim);
	if (fabs(row_norm(v0, dim) - 1.0) > 1e-3 || fabs(row_norm(v1, dim) - 1.0) > 1e-3) {
		fprintf(stderr, "[error] sanity gate: normalized norm not close to 1\n");
		goto done;
	}

	double distinct_cos = cosine(v0, v1, dim);
	if (distinct_cos > 0.999999) {
		fprintf(stderr, "[error] distinctness sanity failed: cosine=%.9f between distinct texts too high\n", distinct_cos);
		goto done;
	}

	fprintf(stdout,
		"[sanity] samples_checked=3 determinism_max_abs_diff=%.9f determinism_cosine=%.9f distinct_cosine=%.9f\n",
		max_abs_diff, det_cos, distinct_cos);

	out->determinism_max_abs_diff = max_abs_diff;
	out->determinism_cosine = det_cos;
	out->distinct_cosine = distinct_cos;
	rc = 0;

done:
	free(v0);
	free(v0b);
	free(v1);
	free(v2);
	return rc;
}

/* ------------------------------------------------------------------------ */
/* batch embed + write                                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
	uint64_t embed_total_us;
	uint64_t write_us;
} embed_stats_t;

static int embed_and_write(
	EmbeddingHandle *h,
	tsv_table_t *table,
	int dim,
	int batch_size,
	FILE *f32_out,
	FILE *jsonl_out,
	int is_records,
	embed_stats_t *stats
) {
	size_t n = table->count;
	const char **texts = malloc(sizeof(char *) * (size_t)batch_size);
	size_t *lens = malloc(sizeof(size_t) * (size_t)batch_size);
	float *outbuf = malloc(sizeof(float) * (size_t)batch_size * (size_t)dim);
	int rc = -1;

	if (!texts || !lens || !outbuf) {
		fprintf(stderr, "[error] oom in embed_and_write\n");
		goto done;
	}

	for (size_t start = 0; start < n; start += (size_t)batch_size) {
		size_t bs = (n - start) < (size_t)batch_size ? (n - start) : (size_t)batch_size;

		for (size_t i = 0; i < bs; i++) {
			texts[i] = table->rows[start + i].col1;
			lens[i] = strlen(texts[i]);
		}

		uint64_t t0 = now_us();
		EmbeddingErrorCode ec = embedding_encode_batch(h, texts, lens, bs, outbuf, dim);
		uint64_t t1 = now_us();
		if (ec != EMBEDDING_OK) {
			fprintf(stderr, "[error] batch encode failed at row %zu: %s\n", start, embedding_last_error(h));
			goto done;
		}

		for (size_t i = 0; i < bs; i++) {
			float *row = outbuf + i * (size_t)dim;
			if (!row_all_finite(row, dim)) {
				fprintf(stderr, "[error] non-finite embedding at row %zu\n", start + i);
				goto done;
			}
			l2_normalize_row(row, dim);
			double nrm = row_norm(row, dim);
			if (fabs(nrm - 1.0) > 1e-3) {
				fprintf(stderr, "[error] normalized norm out of tolerance at row %zu: %.9f\n", start + i, nrm);
				goto done;
			}
		}
		stats->embed_total_us += (t1 - t0);

		uint64_t w0 = now_us();
		for (size_t i = 0; i < bs; i++) {
			write_f32_row_le(f32_out, outbuf + i * (size_t)dim, dim);
		}
		for (size_t i = 0; i < bs; i++) {
			size_t row = start + i;
			if (is_records) {
				fprintf(jsonl_out, "{\"row\":%zu,\"source_line\":%zu,\"id\":%zu,\"question\":",
					row, table->rows[row].source_line, row);
				json_write_escaped_string(jsonl_out, table->rows[row].col1);
				fprintf(jsonl_out, ",\"answer\":");
				json_write_escaped_string(jsonl_out, table->rows[row].col2);
				fprintf(jsonl_out, ",\"text\":");
				json_write_escaped_string(jsonl_out, table->rows[row].col1);
				fprintf(jsonl_out, "}\n");
			} else {
				fprintf(jsonl_out, "{\"query_row\":%zu,\"source_line\":%zu,\"id\":%zu,\"text\":",
					row, table->rows[row].source_line, row);
				json_write_escaped_string(jsonl_out, table->rows[row].col1);
				fprintf(jsonl_out, ",\"expected_answer\":");
				json_write_escaped_string(jsonl_out, table->rows[row].col2);
				fprintf(jsonl_out, "}\n");
			}
		}
		if (ferror(f32_out) || ferror(jsonl_out)) {
			fprintf(stderr, "[error] write error during batch at row %zu\n", start);
			goto done;
		}
		uint64_t w1 = now_us();
		stats->write_us += (w1 - w0);
	}
	rc = 0;

done:
	free(texts);
	free(lens);
	free(outbuf);
	return rc;
}

/* ------------------------------------------------------------------------ */
/* shared deep checks (used by generate's self-validation and by validate)  */
/* ------------------------------------------------------------------------ */

static int check_f32_file(const char *path, size_t count, int dim, long long *out_size) {
	FILE *f;
	long sz;
	size_t expected;
	float *buf;
	int rc = -1;

	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "[error] cannot open %s\n", path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	expected = count * (size_t)dim * sizeof(float);
	if (sz < 0 || (size_t)sz != expected) {
		fprintf(stderr, "[error] %s size mismatch: got %ld expected %zu\n", path, sz, expected);
		fclose(f);
		return -1;
	}
	buf = malloc(sizeof(float) * (size_t)dim);
	if (!buf) {
		fclose(f);
		return -1;
	}
	for (size_t r = 0; r < count; r++) {
		if (read_f32_row_le(f, buf, dim) != 0) {
			fprintf(stderr, "[error] %s short read at row %zu\n", path, r);
			goto done;
		}
		if (!row_all_finite(buf, dim)) {
			fprintf(stderr, "[error] %s row %zu has a non-finite value\n", path, r);
			goto done;
		}
		double nrm = row_norm(buf, dim);
		if (fabs(nrm - 1.0) > 1e-3) {
			fprintf(stderr, "[error] %s row %zu norm=%.6f out of tolerance\n", path, r, nrm);
			goto done;
		}
	}
	rc = 0;
	if (out_size) {
		*out_size = (long long)sz;
	}

done:
	free(buf);
	fclose(f);
	return rc;
}

static int count_jsonl_lines(const char *path, size_t *out_count) {
	FILE *f;
	size_t n = 0;
	int c;

	f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	while ((c = fgetc(f)) != EOF) {
		if (c == '\n') {
			n++;
		}
	}
	fclose(f);
	*out_count = n;
	return 0;
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

/* Finds `"key"` anywhere in text and returns a pointer to the start of its
 * value (after the colon). Scope ambiguous keys by first extracting the
 * enclosing object block with json_extract_object() and searching within it. */
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

/* ------------------------------------------------------------------------ */
/* generate                                                                  */
/* ------------------------------------------------------------------------ */

static void iso8601_utc_now(char *out, size_t outsz) {
	time_t t = time(NULL);
	struct tm tm_utc;
	gmtime_r(&t, &tm_utc);
	strftime(out, outsz, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static int cmd_generate(const cli_args_t *args) {
	uint64_t total_start = now_us();
	char target_dir[1024];
	char tmp_dir[1024];
	tsv_table_t records = {0};
	tsv_table_t queries = {0};
	EmbeddingHandle *h = NULL;
	FILE *f32r = NULL, *f32q = NULL, *jr = NULL, *jq = NULL;
	int rc = 1;
	uint64_t model_load_us = 0;
	embed_stats_t rstats = {0}, qstats = {0};
	sanity_result_t sanity = {0};
	char ort_version[128] = "unknown";
	char model_dir[900];
	char model_onnx_data_path[1024];
	char vocab_path[1024];
	char records_tsv_sha[65], queries_tsv_sha[65];
	char model_onnx_sha[65], model_onnx_data_sha[65], vocab_sha[65];
	char emb_sha[65], qemb_sha[65], rj_sha[65], qj_sha[65];
	long long records_tsv_size = 0, queries_tsv_size = 0;
	long long emb_size = 0, qemb_size = 0, rj_size = 0, qj_size = 0;
	char source_sha[65];
	int have_source_sha = 0;
	char created_at[64];
	char f32r_path[1100], f32q_path[1100], jr_path[1100], jq_path[1100], mf_path[1100];
	uint64_t validation_us = 0;

	snprintf(target_dir, sizeof(target_dir), "%s/%s", args->out_root, args->dataset_id);
	if (path_exists(target_dir)) {
		fprintf(stderr, "[error] dataset already exists: %s (refusing to overwrite; remove it first to regenerate)\n", target_dir);
		return 1;
	}
	if (mkdir_p(args->out_root) != 0) {
		fprintf(stderr, "[error] failed to create out-root %s: %s\n", args->out_root, strerror(errno));
		return 1;
	}
	snprintf(tmp_dir, sizeof(tmp_dir), "%s/.tmp.%s.%d", args->out_root, args->dataset_id, (int)getpid());
	if (path_exists(tmp_dir)) {
		fprintf(stderr, "[error] stale temp dir already exists: %s\n", tmp_dir);
		return 1;
	}
	if (mkdir(tmp_dir, 0755) != 0) {
		fprintf(stderr, "[error] failed to create temp dir %s: %s\n", tmp_dir, strerror(errno));
		return 1;
	}

	if (read_tsv(args->records_tsv, &records) != 0) {
		goto cleanup;
	}
	if (read_tsv(args->queries_tsv, &queries) != 0) {
		goto cleanup;
	}
	fprintf(stdout, "[input] records=%zu queries=%zu\n", records.count, queries.count);

	kvs_config_init_default(&g_cfg);

	{
		EmbeddingConfig cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.model_path = args->model_path;
		cfg.embedding_dim = args->dim;
		cfg.num_threads = args->num_threads;
		cfg.max_text_len = 512;
		cfg.normalize = 0;

		uint64_t t0 = now_us();
		EmbeddingErrorCode ec = embedding_create(&cfg, &h);
		uint64_t t1 = now_us();
		if (ec != EMBEDDING_OK || !h) {
			fprintf(stderr, "[error] embedding_create failed: code=%d\n", (int)ec);
			goto cleanup;
		}
		model_load_us = t1 - t0;
	}

	snprintf(ort_version, sizeof(ort_version), "%s", OrtGetApiBase()->GetVersionString());
	fprintf(stdout, "[onnx] runtime=onnxruntime version=%s model=%s dim=%d\n", ort_version, args->model_path, args->dim);

	if (run_sanity_gate(h, &records, args->dim, &sanity) != 0) {
		goto cleanup;
	}

	snprintf(f32r_path, sizeof(f32r_path), "%s/embeddings.f32", tmp_dir);
	snprintf(f32q_path, sizeof(f32q_path), "%s/query_embeddings.f32", tmp_dir);
	snprintf(jr_path, sizeof(jr_path), "%s/records.jsonl", tmp_dir);
	snprintf(jq_path, sizeof(jq_path), "%s/queries.jsonl", tmp_dir);
	snprintf(mf_path, sizeof(mf_path), "%s/manifest.json", tmp_dir);

	f32r = fopen(f32r_path, "wb");
	jr = fopen(jr_path, "w");
	f32q = fopen(f32q_path, "wb");
	jq = fopen(jq_path, "w");
	if (!f32r || !jr || !f32q || !jq) {
		fprintf(stderr, "[error] failed to open output files in temp dir\n");
		goto cleanup;
	}

	if (embed_and_write(h, &records, args->dim, args->batch_size, f32r, jr, 1, &rstats) != 0) {
		goto cleanup;
	}
	if (embed_and_write(h, &queries, args->dim, args->batch_size, f32q, jq, 0, &qstats) != 0) {
		goto cleanup;
	}

	fclose(f32r);
	f32r = NULL;
	fclose(jr);
	jr = NULL;
	fclose(f32q);
	f32q = NULL;
	fclose(jq);
	jq = NULL;

	fprintf(stdout, "[embed] records_total_us=%" PRIu64 " queries_total_us=%" PRIu64 "\n",
		rstats.embed_total_us, qstats.embed_total_us);

	/* post-write self validation */
	{
		uint64_t v0 = now_us();
		int ok = 1;
		size_t lines;

		if (check_f32_file(f32r_path, records.count, args->dim, &emb_size) != 0) {
			ok = 0;
		}
		if (check_f32_file(f32q_path, queries.count, args->dim, &qemb_size) != 0) {
			ok = 0;
		}
		if (count_jsonl_lines(jr_path, &lines) != 0 || lines != records.count) {
			fprintf(stderr, "[error] records.jsonl line count mismatch\n");
			ok = 0;
		}
		if (count_jsonl_lines(jq_path, &lines) != 0 || lines != queries.count) {
			fprintf(stderr, "[error] queries.jsonl line count mismatch\n");
			ok = 0;
		}
		uint64_t v1 = now_us();
		validation_us = v1 - v0;
		if (!ok) {
			goto cleanup;
		}
	}

	/* checksums */
	if (sha256_file(args->records_tsv, records_tsv_sha) != 0) {
		fprintf(stderr, "[error] sha256 failed for %s\n", args->records_tsv);
		goto cleanup;
	}
	if (sha256_file(args->queries_tsv, queries_tsv_sha) != 0) {
		fprintf(stderr, "[error] sha256 failed for %s\n", args->queries_tsv);
		goto cleanup;
	}
	{
		struct stat st;
		stat(args->records_tsv, &st);
		records_tsv_size = (long long)st.st_size;
		stat(args->queries_tsv, &st);
		queries_tsv_size = (long long)st.st_size;
	}

	dirname_of(args->model_path, model_dir, sizeof(model_dir));
	snprintf(model_onnx_data_path, sizeof(model_onnx_data_path), "%s/model.onnx_data", model_dir);
	snprintf(vocab_path, sizeof(vocab_path), "%s/vocab.txt", model_dir);
	if (sha256_file(args->model_path, model_onnx_sha) != 0) {
		fprintf(stderr, "[error] sha256 failed for %s\n", args->model_path);
		goto cleanup;
	}
	if (sha256_file(model_onnx_data_path, model_onnx_data_sha) != 0) {
		fprintf(stderr, "[error] sha256 failed for %s\n", model_onnx_data_path);
		goto cleanup;
	}
	if (sha256_file(vocab_path, vocab_sha) != 0) {
		fprintf(stderr, "[error] sha256 failed for %s\n", vocab_path);
		goto cleanup;
	}
	if (sha256_file(f32r_path, emb_sha) != 0 || sha256_file(f32q_path, qemb_sha) != 0 ||
	    sha256_file(jr_path, rj_sha) != 0 || sha256_file(jq_path, qj_sha) != 0) {
		fprintf(stderr, "[error] sha256 failed for one or more output artifacts\n");
		goto cleanup;
	}
	{
		struct stat st;
		if (stat(jr_path, &st) == 0) {
			rj_size = (long long)st.st_size;
		}
		if (stat(jq_path, &st) == 0) {
			qj_size = (long long)st.st_size;
		}
	}
	have_source_sha = (sha256_file(GEN_SOURCE_REL_PATH, source_sha) == 0);
	if (!have_source_sha) {
		fprintf(stderr, "[warn] could not read generator source at %s for source_sha256 (run from repo root to capture it)\n", GEN_SOURCE_REL_PATH);
	}

	iso8601_utc_now(created_at, sizeof(created_at));

	{
		FILE *mf = fopen(mf_path, "w");
		if (!mf) {
			fprintf(stderr, "[error] failed to open manifest for write: %s\n", mf_path);
			goto cleanup;
		}
		uint64_t total_us = now_us() - total_start;
		uint64_t record_avg_us = records.count ? rstats.embed_total_us / records.count : 0;
		uint64_t query_avg_us = queries.count ? qstats.embed_total_us / queries.count : 0;
		uint64_t artifact_write_us = rstats.write_us + qstats.write_us;

		fprintf(mf, "{\n");
		fprintf(mf, "  \"schema_version\": 1,\n");
		fprintf(mf, "  \"dataset_id\": ");
		json_write_escaped_string(mf, args->dataset_id);
		fprintf(mf, ",\n  \"created_at_utc\": ");
		json_write_escaped_string(mf, created_at);
		fprintf(mf, ",\n  \"generator\": {\"name\": \"generate_real_embedding_cache\", \"version\": ");
		json_write_escaped_string(mf, GEN_VERSION);
		fprintf(mf, ", \"source_sha256\": ");
		if (have_source_sha) {
			json_write_escaped_string(mf, source_sha);
		} else {
			fprintf(mf, "null");
		}
		fprintf(mf, "},\n");

		fprintf(mf, "  \"model\": {\"id\": \"bge-small-zh-v1.5\", \"path\": ");
		json_write_escaped_string(mf, args->model_path);
		fprintf(mf, ", \"dim\": %d, \"runtime\": \"onnxruntime\", \"onnxruntime_version\": ", args->dim);
		json_write_escaped_string(mf, ort_version);
		fprintf(mf, ", \"normalization\": \"l2\", \"metric\": \"ip_normalized\", \"checksums\": {");
		fprintf(mf, "\"model_onnx_sha256\": ");
		json_write_escaped_string(mf, model_onnx_sha);
		fprintf(mf, ", \"model_onnx_data_sha256\": ");
		json_write_escaped_string(mf, model_onnx_data_sha);
		fprintf(mf, ", \"vocab_txt_sha256\": ");
		json_write_escaped_string(mf, vocab_sha);
		fprintf(mf, "}},\n");

		fprintf(mf, "  \"inputs\": {\n");
		fprintf(mf, "    \"records_tsv\": {\"path\": ");
		json_write_escaped_string(mf, args->records_tsv);
		fprintf(mf, ", \"sha256\": ");
		json_write_escaped_string(mf, records_tsv_sha);
		fprintf(mf, ", \"size_bytes\": %lld, \"rows\": %zu},\n", records_tsv_size, records.count);
		fprintf(mf, "    \"queries_tsv\": {\"path\": ");
		json_write_escaped_string(mf, args->queries_tsv);
		fprintf(mf, ", \"sha256\": ");
		json_write_escaped_string(mf, queries_tsv_sha);
		fprintf(mf, ", \"size_bytes\": %lld, \"rows\": %zu}\n", queries_tsv_size, queries.count);
		fprintf(mf, "  },\n");

		fprintf(mf, "  \"outputs\": {\n");
		fprintf(mf, "    \"embeddings_f32\": {\"path\": \"embeddings.f32\", \"sha256\": ");
		json_write_escaped_string(mf, emb_sha);
		fprintf(mf, ", \"size_bytes\": %lld, \"shape\": [%zu, %d], \"dtype\": \"float32\", \"byte_order\": \"little\"},\n",
			emb_size, records.count, args->dim);
		fprintf(mf, "    \"query_embeddings_f32\": {\"path\": \"query_embeddings.f32\", \"sha256\": ");
		json_write_escaped_string(mf, qemb_sha);
		fprintf(mf, ", \"size_bytes\": %lld, \"shape\": [%zu, %d], \"dtype\": \"float32\", \"byte_order\": \"little\"},\n",
			qemb_size, queries.count, args->dim);
		fprintf(mf, "    \"records_jsonl\": {\"path\": \"records.jsonl\", \"sha256\": ");
		json_write_escaped_string(mf, rj_sha);
		fprintf(mf, ", \"size_bytes\": %lld, \"rows\": %zu},\n", rj_size, records.count);
		fprintf(mf, "    \"queries_jsonl\": {\"path\": \"queries.jsonl\", \"sha256\": ");
		json_write_escaped_string(mf, qj_sha);
		fprintf(mf, ", \"size_bytes\": %lld, \"rows\": %zu}\n", qj_size, queries.count);
		fprintf(mf, "  },\n");

		fprintf(mf,
			"  \"params\": {\"record_count\": %zu, \"query_count\": %zu, \"dim\": %d, \"dtype\": \"float32\", "
			"\"byte_order\": \"little\", \"normalized\": true, \"metric\": \"ip_normalized\", \"batch_size\": %d},\n",
			records.count, queries.count, args->dim, args->batch_size);

		fprintf(mf,
			"  \"sanity\": {\"zh_samples_checked\": 3, \"determinism_max_abs_diff\": %.9f, "
			"\"determinism_cosine\": %.9f, \"distinct_cosine\": %.9f, \"deterministic_check_passed\": true, "
			"\"distinct_check_passed\": true, \"embedding_runtime_confirmed\": \"onnxruntime\"},\n",
			sanity.determinism_max_abs_diff, sanity.determinism_cosine, sanity.distinct_cosine);

		fprintf(mf,
			"  \"timing_us\": {\"model_load_us\": %" PRIu64 ", \"record_embedding_total_us\": %" PRIu64
			", \"record_embedding_avg_us\": %" PRIu64 ", \"query_embedding_total_us\": %" PRIu64
			", \"query_embedding_avg_us\": %" PRIu64 ", \"artifact_write_us\": %" PRIu64
			", \"validation_us\": %" PRIu64 ", \"total_us\": %" PRIu64 "}\n",
			model_load_us, rstats.embed_total_us, record_avg_us, qstats.embed_total_us,
			query_avg_us, artifact_write_us, validation_us, total_us);

		fprintf(mf, "}\n");
		if (ferror(mf)) {
			fprintf(stderr, "[error] write error while writing manifest.json\n");
			fclose(mf);
			goto cleanup;
		}
		fclose(mf);
	}

	if (rename(tmp_dir, target_dir) != 0) {
		fprintf(stderr, "[error] failed to publish dataset (rename %s -> %s): %s\n", tmp_dir, target_dir, strerror(errno));
		goto cleanup;
	}
	fprintf(stdout, "[publish] dataset_id=%s dir=%s\n", args->dataset_id, target_dir);
	rc = 0;

cleanup:
	if (f32r) {
		fclose(f32r);
	}
	if (f32q) {
		fclose(f32q);
	}
	if (jr) {
		fclose(jr);
	}
	if (jq) {
		fclose(jq);
	}
	if (h) {
		embedding_destroy(h);
	}
	tsv_table_free(&records);
	tsv_table_free(&queries);
	if (rc != 0) {
		rm_rf(tmp_dir);
	}
	return rc;
}

/* ------------------------------------------------------------------------ */
/* validate                                                                  */
/* ------------------------------------------------------------------------ */

static int cmd_validate(const cli_args_t *args) {
	char target_dir[1024];
	char mf_path[1100];
	FILE *f;
	long sz;
	char *buf = NULL;
	int ok = 1;
	long long schema_version = 0;
	char dataset_id_val[256];
	char *params_block = NULL;
	long long record_count = 0, query_count = 0, dim = 0;
	char dtype[32] = {0}, byte_order[32] = {0}, metric[64] = {0};
	int normalized = 0;
	char *outputs_block = NULL;

	snprintf(target_dir, sizeof(target_dir), "%s/%s", args->out_root, args->dataset_id);
	if (!path_exists(target_dir)) {
		fprintf(stderr, "[fail] dataset does not exist: %s\n", target_dir);
		return 1;
	}
	snprintf(mf_path, sizeof(mf_path), "%s/manifest.json", target_dir);
	f = fopen(mf_path, "rb");
	if (!f) {
		fprintf(stderr, "[fail] cannot open manifest: %s\n", mf_path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fprintf(stderr, "[fail] manifest empty or unreadable\n");
		fclose(f);
		return 1;
	}
	buf = malloc((size_t)sz + 1);
	if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "[fail] manifest short read\n");
		free(buf);
		fclose(f);
		return 1;
	}
	buf[sz] = '\0';
	fclose(f);

	if (json_get_int64(buf, "schema_version", &schema_version) != 0 || schema_version != 1) {
		fprintf(stderr, "[fail] schema_version missing or unsupported (got %lld, expected 1)\n", schema_version);
		ok = 0;
	}
	if (json_get_string(buf, "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0 ||
	    strcmp(dataset_id_val, args->dataset_id) != 0) {
		fprintf(stderr, "[fail] manifest dataset_id mismatch or missing (got '%s')\n", dataset_id_val);
		ok = 0;
	}

	params_block = json_extract_object(buf, "params");
	if (!params_block) {
		fprintf(stderr, "[fail] manifest missing params block\n");
		ok = 0;
	} else {
		if (json_get_int64(params_block, "record_count", &record_count) != 0 || record_count <= 0) {
			fprintf(stderr, "[fail] params.record_count missing or invalid\n");
			ok = 0;
		}
		if (json_get_int64(params_block, "query_count", &query_count) != 0 || query_count <= 0) {
			fprintf(stderr, "[fail] params.query_count missing or invalid\n");
			ok = 0;
		}
		if (json_get_int64(params_block, "dim", &dim) != 0 || dim <= 0) {
			fprintf(stderr, "[fail] params.dim missing or invalid\n");
			ok = 0;
		}
		if (json_get_string(params_block, "dtype", dtype, sizeof(dtype)) != 0 || strcmp(dtype, "float32") != 0) {
			fprintf(stderr, "[fail] params.dtype expected float32, got '%s'\n", dtype);
			ok = 0;
		}
		if (json_get_string(params_block, "byte_order", byte_order, sizeof(byte_order)) != 0 || strcmp(byte_order, "little") != 0) {
			fprintf(stderr, "[fail] params.byte_order expected little, got '%s'\n", byte_order);
			ok = 0;
		}
		if (json_get_string(params_block, "metric", metric, sizeof(metric)) != 0 || strcmp(metric, "ip_normalized") != 0) {
			fprintf(stderr, "[fail] params.metric expected ip_normalized, got '%s'\n", metric);
			ok = 0;
		}
		if (json_get_bool(params_block, "normalized", &normalized) != 0 || !normalized) {
			fprintf(stderr, "[fail] params.normalized expected true\n");
			ok = 0;
		}
	}

	if (!ok) {
		free(params_block);
		free(buf);
		fprintf(stderr, "[validate] FAIL dataset_id=%s (schema check failed)\n", args->dataset_id);
		return 1;
	}
	free(params_block);

	outputs_block = json_extract_object(buf, "outputs");
	if (!outputs_block) {
		fprintf(stderr, "[fail] manifest missing outputs block\n");
		free(buf);
		return 1;
	}

	struct {
		const char *key;
		const char *filename;
		int is_f32;
		long long expected_count;
	} items[4] = {
		{"embeddings_f32", "embeddings.f32", 1, record_count},
		{"query_embeddings_f32", "query_embeddings.f32", 1, query_count},
		{"records_jsonl", "records.jsonl", 0, record_count},
		{"queries_jsonl", "queries.jsonl", 0, query_count},
	};

	for (int idx = 0; idx < 4; idx++) {
		char *block = json_extract_object(outputs_block, items[idx].key);
		char path_val[128] = {0};
		char sha_val[80] = {0};
		long long size_bytes = -1, rows = -1;
		int have_path, have_sha;
		char file_path[1200];
		struct stat st;
		char actual_sha[65];

		if (!block) {
			fprintf(stderr, "[fail] outputs.%s missing\n", items[idx].key);
			ok = 0;
			continue;
		}
		have_path = json_get_string(block, "path", path_val, sizeof(path_val)) == 0;
		have_sha = json_get_string(block, "sha256", sha_val, sizeof(sha_val)) == 0;
		if (items[idx].is_f32) {
			if (json_get_int64(block, "size_bytes", &size_bytes) != 0) {
				fprintf(stderr, "[fail] outputs.%s missing size_bytes\n", items[idx].key);
				ok = 0;
			}
		} else {
			if (json_get_int64(block, "rows", &rows) != 0) {
				fprintf(stderr, "[fail] outputs.%s missing rows\n", items[idx].key);
				ok = 0;
			}
			if (rows != items[idx].expected_count) {
				fprintf(stderr, "[fail] outputs.%s rows=%lld does not match params (%lld)\n",
					items[idx].key, rows, items[idx].expected_count);
				ok = 0;
			}
		}
		if (!have_path || !have_sha || strlen(sha_val) != 64) {
			fprintf(stderr, "[fail] outputs.%s missing/invalid path or sha256\n", items[idx].key);
			ok = 0;
		}
		free(block);
		if (!have_path) {
			continue;
		}

		snprintf(file_path, sizeof(file_path), "%s/%s", target_dir, items[idx].filename);
		if (stat(file_path, &st) != 0) {
			fprintf(stderr, "[fail] %s missing on disk\n", file_path);
			ok = 0;
			continue;
		}
		if (items[idx].is_f32 && size_bytes >= 0 && (long long)st.st_size != size_bytes) {
			fprintf(stderr, "[fail] %s size mismatch: manifest=%lld disk=%lld\n", file_path, size_bytes, (long long)st.st_size);
			ok = 0;
		}
		if (have_sha) {
			if (sha256_file(file_path, actual_sha) != 0) {
				fprintf(stderr, "[fail] sha256 computation failed for %s\n", file_path);
				ok = 0;
			} else if (strcmp(actual_sha, sha_val) != 0) {
				fprintf(stderr, "[fail] %s sha256 mismatch: manifest=%s actual=%s\n", file_path, sha_val, actual_sha);
				ok = 0;
			}
		}
	}
	free(outputs_block);

	if (!ok) {
		free(buf);
		fprintf(stderr, "[validate] FAIL dataset_id=%s (checksum/existence check failed)\n", args->dataset_id);
		return 1;
	}

	{
		char embf[1200], qembf[1200], rj[1200], qj[1200];
		size_t rj_lines = 0, qj_lines = 0;

		snprintf(embf, sizeof(embf), "%s/embeddings.f32", target_dir);
		snprintf(qembf, sizeof(qembf), "%s/query_embeddings.f32", target_dir);
		snprintf(rj, sizeof(rj), "%s/records.jsonl", target_dir);
		snprintf(qj, sizeof(qj), "%s/queries.jsonl", target_dir);

		if (check_f32_file(embf, (size_t)record_count, (int)dim, NULL) != 0) {
			ok = 0;
		}
		if (check_f32_file(qembf, (size_t)query_count, (int)dim, NULL) != 0) {
			ok = 0;
		}
		if (count_jsonl_lines(rj, &rj_lines) != 0 || rj_lines != (size_t)record_count) {
			fprintf(stderr, "[fail] records.jsonl line count mismatch: got %zu expected %lld\n", rj_lines, record_count);
			ok = 0;
		}
		if (count_jsonl_lines(qj, &qj_lines) != 0 || qj_lines != (size_t)query_count) {
			fprintf(stderr, "[fail] queries.jsonl line count mismatch: got %zu expected %lld\n", qj_lines, query_count);
			ok = 0;
		}
	}

	free(buf);
	if (ok) {
		fprintf(stdout, "[validate] PASS dataset_id=%s record_count=%lld query_count=%lld dim=%lld\n",
			args->dataset_id, record_count, query_count, dim);
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
