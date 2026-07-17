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
static char g_repo_root[PATH_MAX];

static int write_file(const char *name, const char *content, char *path, size_t path_size) {
	FILE *fp;

	if (snprintf(path, path_size, "%s/%s", g_tmpdir, name) >= (int)path_size) {
		return -1;
	}
	fp = fopen(path, "w");
	if (!fp) {
		return -1;
	}
	if (fputs(content, fp) == EOF || fclose(fp) != 0) {
		return -1;
	}
	return 0;
}

static int load_args(
	kvs_config_t *cfg,
	int argc,
	char **argv,
	FILE *warnings,
	kvs_config_error_t *error
) {
	return kvs_config_load(cfg, argc, argv, NULL, warnings, error);
}

static int load_path(
	kvs_config_t *cfg,
	const char *path,
	FILE *warnings,
	kvs_config_error_t *error
) {
	char *argv[] = {(char *)"config-test", (char *)"--config", (char *)path};
	return load_args(cfg, 3, argv, warnings, error);
}

static int expect_file_failure(const char *name, const char *content, const char *needle) {
	char path[PATH_MAX];
	kvs_config_t cfg;
	kvs_config_error_t error;
	FILE *warnings = tmpfile();
	int rc;

	CHECK(warnings != NULL, "tmpfile failed: %s", strerror(errno));
	CHECK(write_file(name, content, path, sizeof(path)) == 0, "write %s failed", name);
	rc = load_path(&cfg, path, warnings, &error);
	fclose(warnings);
	CHECK(rc != 0, "%s unexpectedly loaded", name);
	CHECK(strstr(error.message, needle) != NULL,
		"%s error '%s' does not contain '%s'", name, error.message, needle);
	return 0;
}

static int test_defaults_and_descriptors(void) {
	kvs_config_t cfg;
	const kvs_config_field_descriptor_t *desc;
	const kvs_config_source_t *source;

	kvs_config_init_default(&cfg);
	CHECK(cfg.port == 8080, "default port=%d", cfg.port);
	CHECK(cfg.vector_hnsw_m == 48, "default M=%d", cfg.vector_hnsw_m);
	CHECK(cfg.vector_hnsw_ef_construction == 512, "default efConstruction=%d",
		cfg.vector_hnsw_ef_construction);
	CHECK(cfg.vector_hnsw_ef_search == 1024, "default efSearch=%d", cfg.vector_hnsw_ef_search);
	CHECK(strcmp(cfg.vector_index_type, "core_hnsw_v2") == 0,
		"default backend=%s", cfg.vector_index_type);
	desc = kvs_config_descriptor_find("network.port");
	CHECK(desc != NULL && strcmp(desc->legacy_names, "port") == 0,
		"network.port descriptor missing");
	CHECK(desc->type == KVS_CONFIG_TYPE_INT && desc->cli_override,
		"network.port descriptor metadata invalid");
	source = kvs_config_source_get(&cfg, "network.port");
	CHECK(source != NULL && source->kind == KVS_CONFIG_SOURCE_DEFAULT,
		"default source metadata invalid");
	CHECK(kvs_config_descriptor_count() > KVS_CONFIG_FIELD_COUNT,
		"Stage 0 inactive/unsupported descriptors missing");
	return 0;
}

static int test_new_format_cli_sources_and_sync_derivation(void) {
	char path[PATH_MAX];
	char *argv[5];
	kvs_config_t cfg;
	kvs_config_error_t error;
	const kvs_config_source_t *source;
	char formatted[PATH_MAX + 64];
	FILE *printed;
	char output[8192];
	size_t length;

	CHECK(write_file(
		"new.conf",
		"[server]\n"
		"role = master\n"
		"[network]\n"
		"port = 19000\n"
		"mode = reactor\n"
		"[replication]\n"
		"transport = tcp\n"
		"[vector]\n"
		"index_type = core_flat\n",
		path,
		sizeof(path)) == 0,
		"write new config failed");
	argv[0] = (char *)"config-test";
	argv[1] = (char *)"--config";
	argv[2] = path;
	argv[3] = (char *)"--network.port";
	argv[4] = (char *)"20000";
	CHECK(load_args(&cfg, 5, argv, NULL, &error) == 0, "new config load failed: %s", error.message);
	CHECK(cfg.port == 20000, "CLI did not override file port");
	CHECK(cfg.replication_sync_listen_port == 21000,
		"sync listen port derived before final CLI merge: %d",
		cfg.replication_sync_listen_port);
	source = kvs_config_source_get(&cfg, "network.port");
	CHECK(source != NULL && source->kind == KVS_CONFIG_SOURCE_CLI && !source->legacy_alias,
		"CLI source metadata invalid");
	source = kvs_config_source_get(&cfg, "replication.sync_listen_port");
	CHECK(source != NULL && source->kind == KVS_CONFIG_SOURCE_DERIVED,
		"derived sync source metadata invalid");
	CHECK(kvs_config_source_format(source, formatted, sizeof(formatted)) == 0 &&
		strstr(formatted, "network.port+1000") != NULL,
		"derived source formatting invalid: %s", formatted);
	printed = tmpfile();
	CHECK(printed != NULL, "tmpfile failed");
	CHECK(kvs_config_print_effective(&cfg, printed) == 0, "effective config print failed");
	rewind(printed);
	length = fread(output, 1, sizeof(output) - 1, printed);
	output[length] = '\0';
	fclose(printed);
	CHECK(strstr(output, "network.port=20000 source=cli status=implemented") != NULL,
		"effective output missing value/source");
	return 0;
}

static int test_legacy_format_warning_and_normalization(void) {
	char path[PATH_MAX];
	kvs_config_t cfg;
	kvs_config_error_t error;
	const kvs_config_source_t *source;
	FILE *warnings;
	char output[8192];
	size_t length;

	CHECK(write_file(
		"legacy.conf",
		"port = 18100\n"
		"sync_mode = tcp\n"
		"vector_index_type = flat\n"
		"vector_hnsw_m = 32\n"
		"vector_hnsw_ef_construction = 256\n"
		"vector_hnsw_ef_search = 128\n",
		path,
		sizeof(path)) == 0,
		"write legacy config failed");
	warnings = tmpfile();
	CHECK(warnings != NULL, "tmpfile failed");
	CHECK(load_path(&cfg, path, warnings, &error) == 0,
		"legacy config load failed: %s", error.message);
	CHECK(cfg.port == 18100 && cfg.replication_sync_listen_port == 19100,
		"legacy port/derived sync invalid");
	CHECK(strcmp(cfg.vector_index_type, "core_flat") == 0, "legacy flat was not normalized");
	CHECK(cfg.vector_hnsw_m == 48 && cfg.vector_hnsw_ef_construction == 512 &&
		cfg.vector_hnsw_ef_search == 1024, "legacy HNSW values were not normalized");
	source = kvs_config_source_get(&cfg, "network.port");
	CHECK(source != NULL && source->kind == KVS_CONFIG_SOURCE_FILE && source->legacy_alias &&
		source->line == 1, "legacy source metadata invalid");
	rewind(warnings);
	length = fread(output, 1, sizeof(output) - 1, warnings);
	output[length] = '\0';
	fclose(warnings);
	CHECK(strstr(output, "deprecated legacy alias 'port'") != NULL,
		"legacy warning missing");
	CHECK(strstr(output, "normalized to current effective value '48'") != NULL,
		"legacy normalization warning missing");
	return 0;
}

static int test_no_config(void) {
	char *argv[] = {(char *)"config-test", (char *)"--no-config"};
	kvs_config_t cfg;
	kvs_config_error_t error;

	CHECK(load_args(&cfg, 2, argv, NULL, &error) == 0, "--no-config failed: %s", error.message);
	CHECK(cfg.no_config && cfg.port == 8080 &&
		cfg.replication_transport == KVS_REPLICATION_DISABLED &&
		cfg.replication_sync_listen_port == 0,
		"--no-config did not use derived defaults");
	CHECK(cfg.config_path[0] == '\0', "--no-config unexpectedly selected a file");
	return 0;
}

static int test_path_resolution_independent_of_cwd(void) {
	char cfg_dir[PATH_MAX];
	char root_dir[PATH_MAX];
	char path[PATH_MAX];
	char expected[PATH_MAX];
	char resolved[PATH_MAX];
	char previous[PATH_MAX];
	kvs_config_t cfg;
	kvs_config_error_t error;

	CHECK(snprintf(cfg_dir, sizeof(cfg_dir), "%s/pathcase", g_tmpdir) < (int)sizeof(cfg_dir),
		"config dir path too long");
	CHECK(snprintf(root_dir, sizeof(root_dir), "%s/assets", cfg_dir) < (int)sizeof(root_dir),
		"root dir path too long");
	CHECK(mkdir(cfg_dir, 0700) == 0 && mkdir(root_dir, 0700) == 0,
		"mkdir path fixture failed: %s", strerror(errno));
	CHECK(snprintf(path, sizeof(path), "%s/main.conf", cfg_dir) < (int)sizeof(path), "path too long");
	{
		FILE *fp = fopen(path, "w");
		CHECK(fp != NULL, "open path config failed");
		CHECK(fputs(
			"[server]\nconfig_root = assets\ndata_dir = runtime\n"
			"[persistence]\nrdb_enabled = false\naof_enabled = false\n"
			"[vector_snapshot]\nenabled = false\n",
			fp) != EOF,
			"write path config failed");
		CHECK(fclose(fp) == 0, "close path config failed");
	}
	CHECK(getcwd(previous, sizeof(previous)) != NULL, "getcwd failed");
	CHECK(chdir("/") == 0, "chdir / failed");
	CHECK(load_path(&cfg, path, NULL, &error) == 0, "path config load failed: %s", error.message);
	CHECK(chdir(previous) == 0, "restore cwd failed");
	CHECK(snprintf(expected, sizeof(expected), "%s/runtime", root_dir) < (int)sizeof(expected),
		"expected data path too long");
	CHECK(strcmp(cfg.config_root, root_dir) == 0, "config_root=%s expected=%s", cfg.config_root, root_dir);
	CHECK(strcmp(cfg.data_dir, expected) == 0, "data_dir=%s expected=%s", cfg.data_dir, expected);
	CHECK(kvs_config_resolve_path(&cfg, KVS_CONFIG_PATH_DATA, "db/main.rdb", resolved, sizeof(resolved)) == 0,
		"data path resolution failed");
	CHECK(snprintf(expected, sizeof(expected), "%s/runtime/db/main.rdb", root_dir) < (int)sizeof(expected),
		"expected runtime path too long");
	CHECK(strcmp(resolved, expected) == 0, "resolved data path=%s expected=%s", resolved, expected);
	CHECK(kvs_config_resolve_path(&cfg, KVS_CONFIG_PATH_ASSET, "models/model.onnx", resolved, sizeof(resolved)) == 0,
		"asset path resolution failed");
	CHECK(snprintf(expected, sizeof(expected), "%s/models/model.onnx", root_dir) < (int)sizeof(expected),
		"expected asset path too long");
	CHECK(strcmp(resolved, expected) == 0, "resolved asset path=%s expected=%s", resolved, expected);
	return 0;
}

static int test_file_failures(void) {
	CHECK(expect_file_failure("unknown-section.conf", "[unknown]\nport=1\n", "unknown configuration section") == 0,
		"unknown section case failed");
	CHECK(expect_file_failure("unknown-key.conf", "mystery=1\n", "unknown configuration item") == 0,
		"unknown key case failed");
	CHECK(expect_file_failure("missing-equals.conf", "port 9000\n", "missing '='") == 0,
		"missing equals case failed");
	CHECK(expect_file_failure("empty.conf", "port =\n", "is empty") == 0, "empty value case failed");
	CHECK(expect_file_failure("duplicate.conf", "port=9000\nport=9001\n", "duplicate configuration") == 0,
		"duplicate case failed");
	CHECK(expect_file_failure(
		"canonical-legacy-duplicate.conf",
		"port=9000\n[network]\nport=9001\n",
		"duplicate configuration") == 0,
		"canonical/legacy duplicate case failed");
	CHECK(expect_file_failure("invalid-bool.conf", "enable_aof=maybe\n", "invalid boolean") == 0,
		"invalid bool case failed");
	CHECK(expect_file_failure("overflow.conf", "port=999999999999999999999\n", "invalid integer") == 0,
		"overflow case failed");
	CHECK(expect_file_failure("range.conf", "port=70000\n", "out of range") == 0,
		"range case failed");
	CHECK(expect_file_failure("invalid-enum.conf", "network_mode=threads\n", "invalid enum") == 0,
		"invalid enum case failed");
	CHECK(expect_file_failure("removed-backend.conf", "vector_index_type=hnsw\n", "removed vector backend") == 0,
		"removed backend case failed");
	CHECK(expect_file_failure("unsupported.conf", "embedding_batch_enabled=no\n", "unsupported") == 0,
		"unsupported field case failed");
	CHECK(expect_file_failure(
		"queue-alias.conf",
		"embedding_prepare_queue_capacity=8\nembedding_worker_queue_capacity=8\n",
		"duplicate configuration") == 0,
		"queue alias duplicate case failed");
	CHECK(expect_file_failure("trailing-section.conf", "[network] trailing\n", "trailing content") == 0,
		"trailing section case failed");
	CHECK(expect_file_failure("trailing-number.conf", "port=9000 trailing\n", "invalid integer") == 0,
		"trailing number case failed");
	return 0;
}

static int test_cli_and_missing_file_failures(void) {
	kvs_config_t cfg;
	kvs_config_error_t error;
	char *missing[] = {(char *)"config-test", (char *)"--config"};
	char *missing_file[] = {(char *)"config-test", (char *)"--config", (char *)"/definitely/missing/kvlite.conf"};
	char *unknown[] = {(char *)"config-test", (char *)"--no-config", (char *)"--unknown", (char *)"x"};
	char *missing_value[] = {(char *)"config-test", (char *)"--no-config", (char *)"--network.port"};
	char *both[] = {(char *)"config-test", (char *)"--no-config", (char *)"--config", (char *)"x"};

	CHECK(load_args(&cfg, 2, missing, NULL, &error) != 0 && strstr(error.message, "requires a path"),
		"missing --config value did not fail");
	CHECK(load_args(&cfg, 3, missing_file, NULL, &error) != 0 && strstr(error.message, "does not exist"),
		"explicit missing file did not fail");
	CHECK(load_args(&cfg, 4, unknown, NULL, &error) != 0 && strstr(error.message, "unknown command-line"),
		"unknown CLI did not fail");
	CHECK(load_args(&cfg, 3, missing_value, NULL, &error) != 0 && strstr(error.message, "requires a value"),
		"missing CLI value did not fail");
	CHECK(load_args(&cfg, 4, both, NULL, &error) != 0 && strstr(error.message, "cannot be combined"),
		"--no-config/--config conflict did not fail");
	return 0;
}

static int test_existing_configs(void) {
	const char *relative_paths[] = {
		"config/kvstore.conf",
		"config/kvstore-embedding.conf",
		"config/master.conf",
		"config/slave.conf",
		"docs/configuration/kvstore.conf.example"
	};
	size_t i;

	for (i = 0; i < sizeof(relative_paths) / sizeof(relative_paths[0]); ++i) {
		char path[PATH_MAX];
		kvs_config_t cfg;
		kvs_config_error_t error;
		FILE *warnings = tmpfile();
		CHECK(warnings != NULL, "tmpfile failed");
		CHECK(snprintf(path, sizeof(path), "%s/%s", g_repo_root, relative_paths[i]) < (int)sizeof(path),
			"official config path too long");
		CHECK(load_path(&cfg, path, warnings, &error) == 0,
			"official config %s failed: %s", relative_paths[i], error.message);
		fclose(warnings);
	}
	return 0;
}

int main(void) {
	char template[] = "/tmp/kvlite-unified-config-stage1-XXXXXX";
	int failures = 0;

	CHECK(getcwd(g_repo_root, sizeof(g_repo_root)) != NULL, "getcwd failed");
	CHECK(mkdtemp(template) != NULL, "mkdtemp failed: %s", strerror(errno));
	CHECK(snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", template) < (int)sizeof(g_tmpdir),
		"temporary directory path too long");
	{
		char data_dir[PATH_MAX];
		CHECK(snprintf(data_dir, sizeof(data_dir), "%s/data", g_tmpdir) < (int)sizeof(data_dir),
			"temporary data directory path too long");
		CHECK(mkdir(data_dir, 0700) == 0, "mkdir temporary data directory failed: %s", strerror(errno));
	}

	failures += test_defaults_and_descriptors() != 0;
	failures += test_new_format_cli_sources_and_sync_derivation() != 0;
	failures += test_legacy_format_warning_and_normalization() != 0;
	failures += test_no_config() != 0;
	failures += test_path_resolution_independent_of_cwd() != 0;
	failures += test_file_failures() != 0;
	failures += test_cli_and_missing_file_failures() != 0;
	failures += test_existing_configs() != 0;

	if (failures) {
		fprintf(stderr, "unified config Stage 1 tests failed: %d group(s)\n", failures);
		return 1;
	}
	printf("unified config Stage 1 tests: PASS\n");
	return 0;
}
