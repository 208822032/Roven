#define _POSIX_C_SOURCE 200809L

#include "kvs_config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
			return -1; \
		} \
	} while (0)

static char g_tmpdir[PATH_MAX];

static int write_named(const char *name, const char *content, char *out, size_t out_size) {
	FILE *fp;

	if (snprintf(out, out_size, "%s/%s", g_tmpdir, name) >= (int)out_size) return -1;
	fp = fopen(out, "wb");
	if (!fp) return -1;
	if (fwrite(content, 1, strlen(content), fp) != strlen(content) || fclose(fp) != 0) return -1;
	return 0;
}

static int write_config(const char *name, const char *body, char *out, size_t out_size) {
	char content[16384];
	int written = snprintf(
		content,
		sizeof(content),
		"[server]\nconfig_root=.\ndata_dir=data\n"
		"[persistence]\nrdb_enabled=false\naof_enabled=false\n"
		"[vector_snapshot]\nenabled=false\n%s",
		body ? body : ""
	);
	if (written < 0 || (size_t)written >= sizeof(content)) return -1;
	return write_named(name, content, out, out_size);
}

static int load_path(kvs_config_t *cfg, const char *path, FILE *warnings, kvs_config_error_t *error) {
	char *argv[] = {(char *)"stage3-config-test", (char *)"--config", (char *)path};
	return kvs_config_load(cfg, 3, argv, NULL, warnings, error);
}

static int expect_failure(const char *name, const char *body, const char *needle) {
	char path[PATH_MAX];
	kvs_config_t cfg;
	kvs_config_error_t error;
	FILE *warnings = tmpfile();
	int rc;

	CHECK(warnings != NULL, "tmpfile failed: %s", strerror(errno));
	CHECK(write_config(name, body, path, sizeof(path)) == 0, "write %s failed", name);
	rc = load_path(&cfg, path, warnings, &error);
	fclose(warnings);
	CHECK(rc != 0, "%s unexpectedly loaded", name);
	CHECK(strstr(error.message, needle) != NULL,
		"%s error '%s' does not contain '%s'", name, error.message, needle);
	return 0;
}

static int test_vector_hash_and_hnsw_contract(void) {
	char path[PATH_MAX];
	kvs_config_t cfg;
	kvs_config_error_t error;

	CHECK(write_config(
		"vector.conf",
		"[vector]\nenabled=true\nindex_type=core_hnsw_v2\ndimension=37\n"
		"metric=ip\nnormalize=true\nhnsw_m=48\nhnsw_m0=64\n"
		"hnsw_ef_construction=96\nhnsw_ef_search=32\ninitial_capacity=7\n"
		"default_topk=9\ndefault_threshold=0.5\nscope_overfetch_factor=3\n"
		"[embedding]\nprovider=hash\noutput_dimension=37\nmax_tokens=128\n"
		"[observability]\nvsearch_stage_stats_enabled=true\n"
		"embedding_profile_enabled=true\nhnsw_build_profile_enabled=false\n"
		"hnsw_search_profile_enabled=false\n",
		path,
		sizeof(path)) == 0,
		"write vector config failed");
	CHECK(load_path(&cfg, path, NULL, &error) == 0, "load vector config failed: %s", error.message);
	CHECK(cfg.vector_enabled && cfg.vector_dimension == 37 && cfg.embedding_dim == 37,
		"dimension contract not retained");
	CHECK(cfg.vector_hnsw_m == 48 && cfg.vector_hnsw_m0 == 64 &&
		cfg.vector_hnsw_ef_construction == 96 && cfg.vector_hnsw_ef_search == 32,
		"canonical HNSW values were modified");
	CHECK(cfg.vector_hnsw_initial_capacity == 7 && cfg.vector_default_topk == 9,
		"capacity/topK not retained");
	CHECK(cfg.embedding_model_identity != 0, "hash model identity missing");
	CHECK(cfg.observability_vsearch_stage_stats_enabled &&
		cfg.observability_embedding_profile_enabled,
		"observability switches not loaded");
	return 0;
}

static int test_strict_failures(void) {
	CHECK(expect_failure("dim.conf",
		"[vector]\ndimension=64\n[embedding]\noutput_dimension=32\n",
		"must equal") == 0, "dimension mismatch test failed");
	CHECK(expect_failure("m0.conf",
		"[vector]\nhnsw_m=64\nhnsw_m0=48\n",
		"invalid HNSW") == 0, "M0 test failed");
	CHECK(expect_failure("efc.conf",
		"[vector]\nhnsw_m=48\nhnsw_m0=96\nhnsw_ef_construction=95\n",
		"invalid HNSW") == 0, "efConstruction test failed");
	CHECK(expect_failure("initial.conf",
		"[vector]\ninitial_capacity=0\n",
		"out of range") == 0, "initial capacity test failed");
	CHECK(expect_failure("normalize.conf",
		"[vector]\nnormalize=false\n",
		"incompatible") == 0, "normalize test failed");
	CHECK(expect_failure("batch.conf",
		"[embedding]\nbatch_enabled=true\n",
		"unsupported") == 0, "dead batch test failed");
	CHECK(expect_failure("delta.conf",
		"[vector_rebuild]\ndelta_initial_capacity=10\ndelta_max_entries=9\n",
		"must be >=") == 0, "delta limit test failed");
	CHECK(expect_failure("cross-node.conf",
		"[vector_snapshot]\ncross_node_reuse=true\n",
		"cross_node_reuse=true") == 0, "cross-node test failed");
	return 0;
}

static int test_legacy_hnsw_and_dimension(void) {
	char path[PATH_MAX];
	char warning_text[4096];
	kvs_config_t cfg;
	kvs_config_error_t error;
	FILE *warnings = tmpfile();
	size_t length;

	CHECK(warnings != NULL, "tmpfile failed");
	CHECK(write_named(
		"legacy.conf",
		"embedding_dim=23\nvector_hnsw_m=16\n"
		"vector_hnsw_ef_construction=128\nvector_hnsw_ef_search=64\n"
		"vector_hnsw_initial_capacity=0\n"
		"[server]\nconfig_root=.\ndata_dir=data\n"
		"[persistence]\nrdb_enabled=false\naof_enabled=false\n"
		"[vector_snapshot]\nenabled=false\n",
		path,
		sizeof(path)) == 0,
		"write legacy config failed");
	CHECK(load_path(&cfg, path, warnings, &error) == 0, "legacy load failed: %s", error.message);
	CHECK(cfg.vector_dimension == 23 && cfg.embedding_dim == 23,
		"legacy embedding_dim did not derive vector.dimension");
	CHECK(cfg.vector_hnsw_m == 48 && cfg.vector_hnsw_m0 == 96 &&
		cfg.vector_hnsw_ef_construction == 512 && cfg.vector_hnsw_ef_search == 1024 &&
		cfg.vector_hnsw_initial_capacity == 16,
		"legacy HNSW normalization incorrect");
	rewind(warnings);
	length = fread(warning_text, 1, sizeof(warning_text) - 1, warnings);
	warning_text[length] = '\0';
	fclose(warnings);
	CHECK(strstr(warning_text, "deprecated") != NULL && strstr(warning_text, "normalized") != NULL,
		"legacy warnings missing: %s", warning_text);
	return 0;
}

static int test_model_content_identity_and_paths(void) {
	char model[PATH_MAX];
	char vocab[PATH_MAX];
	char path[PATH_MAX];
	char previous[PATH_MAX];
	kvs_config_t first;
	kvs_config_t second;
	kvs_config_t third;
	kvs_config_error_t error;
	uint64_t first_identity;

	CHECK(write_named("model.onnx", "model-a", model, sizeof(model)) == 0, "write model failed");
	CHECK(write_named("vocab.txt", "[PAD]\n[UNK]\n[CLS]\n[SEP]\n", vocab, sizeof(vocab)) == 0,
		"write vocab failed");
	CHECK(write_config(
		"local.conf",
		"[vector]\ndimension=8\n"
		"[embedding]\nprovider=local_c\nmodel_path=model.onnx\nvocab_path=vocab.txt\n"
		"output_dimension=8\nmodel_version=test-model\n",
		path,
		sizeof(path)) == 0,
		"write local config failed");
	CHECK(getcwd(previous, sizeof(previous)) != NULL && chdir("/") == 0, "chdir failed");
	CHECK(load_path(&first, path, NULL, &error) == 0, "local load failed: %s", error.message);
	CHECK(chdir(previous) == 0, "restore cwd failed");
	CHECK(strcmp(first.embedding_model_path, model) == 0 &&
		strcmp(first.embedding_vocab_path, vocab) == 0,
		"asset paths depend on cwd");
	first_identity = first.embedding_model_identity;
	CHECK(write_named("model.onnx", "model-b", model, sizeof(model)) == 0, "rewrite model failed");
	CHECK(load_path(&second, path, NULL, &error) == 0, "reload local failed: %s", error.message);
	CHECK(first_identity != second.embedding_model_identity,
		"model content change did not change identity");
	CHECK(write_named("vocab.txt", "[PAD]\n[UNK]\n[CLS]\n[SEP]\nextra\n", vocab,
		sizeof(vocab)) == 0, "rewrite vocab failed");
	CHECK(load_path(&third, path, NULL, &error) == 0, "vocab reload failed: %s", error.message);
	CHECK(second.embedding_model_identity != third.embedding_model_identity,
		"vocab content change did not change identity");
	return 0;
}

static int test_snapshot_paths_and_inactive_state(void) {
	char path[PATH_MAX];
	char expected_manifest[PATH_MAX];
	char output[32768];
	kvs_config_t cfg;
	kvs_config_error_t error;
	FILE *printed;
	size_t length;

	CHECK(write_named(
		"snapshot.conf",
		"[server]\nconfig_root=.\ndata_dir=data\n"
		"[persistence]\nrdb_enabled=false\naof_enabled=false\n"
		"[vector]\nenabled=true\n"
		"[vector_snapshot]\nenabled=true\nmanifest_path=idx/manifest\nindex_path=idx/snapshot\n"
		"load_on_start=false\ninvalid_action=fail\nwrite_after_rebuild=off\ncross_node_reuse=false\n",
		path,
		sizeof(path)) == 0,
		"write snapshot config failed");
	{
		char idx[PATH_MAX];
		CHECK(snprintf(idx, sizeof(idx), "%s/data/idx", g_tmpdir) < (int)sizeof(idx), "idx path too long");
		CHECK(mkdir(idx, 0700) == 0, "mkdir idx failed: %s", strerror(errno));
	}
	CHECK(load_path(&cfg, path, NULL, &error) == 0, "snapshot load failed: %s", error.message);
	CHECK(snprintf(expected_manifest, sizeof(expected_manifest), "%s/data/idx/manifest", g_tmpdir) <
		(int)sizeof(expected_manifest), "manifest path too long");
	CHECK(strcmp(cfg.vector_snapshot_manifest_path, expected_manifest) == 0,
		"snapshot path not relative to data_dir: %s", cfg.vector_snapshot_manifest_path);

	CHECK(write_config(
		"disabled.conf",
		"[vector]\nenabled=false\n[embedding]\nprovider=local_c\nmodel_path=missing.onnx\n",
		path,
		sizeof(path)) == 0,
		"write disabled config failed");
	CHECK(load_path(&cfg, path, NULL, &error) == 0, "disabled vector config failed: %s", error.message);
	printed = tmpfile();
	CHECK(printed != NULL && kvs_config_print_effective(&cfg, printed) == 0, "effective print failed");
	rewind(printed);
	length = fread(output, 1, sizeof(output) - 1, printed);
	output[length] = '\0';
	fclose(printed);
	CHECK(strstr(output, "embedding.provider=local_c") != NULL &&
		strstr(output, "status=inactive") != NULL,
		"disabled vector fields are not marked inactive");
	return 0;
}

int main(void) {
	char template_path[] = "/tmp/kvlite-unified-config-stage3-XXXXXX";
	char data_dir[PATH_MAX];
	int failures = 0;

	if (!mkdtemp(template_path)) {
		fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
		return 1;
	}
	snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", template_path);
	if (snprintf(data_dir, sizeof(data_dir), "%s/data", g_tmpdir) >= (int)sizeof(data_dir) ||
	    mkdir(data_dir, 0700) != 0) {
		fprintf(stderr, "mkdir data failed: %s\n", strerror(errno));
		return 1;
	}

	failures += test_vector_hash_and_hnsw_contract() != 0;
	failures += test_strict_failures() != 0;
	failures += test_legacy_hnsw_and_dimension() != 0;
	failures += test_model_content_identity_and_paths() != 0;
	failures += test_snapshot_paths_and_inactive_state() != 0;

	if (failures != 0) {
		fprintf(stderr, "unified config Stage 3 tests failed: %d group(s)\n", failures);
		return 1;
	}
	printf("unified config Stage 3 tests: PASS\n");
	return 0;
}
