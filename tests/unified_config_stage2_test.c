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

static int write_config(const char *name, const char *body, char *path, size_t path_size) {
	FILE *fp;

	if (snprintf(path, path_size, "%s/%s", g_tmpdir, name) >= (int)path_size) return -1;
	fp = fopen(path, "w");
	if (!fp) return -1;
	if (fputs(body, fp) == EOF || fclose(fp) != 0) return -1;
	return 0;
}

static int load_path(const char *path, kvs_config_t *cfg, kvs_config_error_t *error) {
	char *argv[] = {(char *)"stage2-config-test", (char *)"--config", (char *)path};
	return kvs_config_load(cfg, 3, argv, NULL, NULL, error);
}

static int expect_failure(const char *name, const char *body, const char *needle) {
	char path[PATH_MAX];
	kvs_config_t cfg;
	kvs_config_error_t error;

	CHECK(write_config(name, body, path, sizeof(path)) == 0, "write %s failed", name);
	CHECK(load_path(path, &cfg, &error) != 0, "%s unexpectedly loaded", name);
	CHECK(strstr(error.message, needle) != NULL,
		"%s error '%s' missing '%s'", name, error.message, needle);
	return 0;
}

static int test_stage2_defaults_and_descriptors(void) {
	kvs_config_t cfg;
	const char *fields[] = {
		"server.test_mode", "network.listen_address", "network.max_request_bytes",
		"storage.default_engine", "storage.max_value_bytes", "ttl.scan_budget",
		"persistence.rdb_enabled", "persistence.aof_rewrite_min_bytes",
		"persistence.startup_recovery_order"
	};
	size_t i;

	kvs_config_init_default(&cfg);
	CHECK(cfg.network_worker_threads == 1, "worker_threads=%d", cfg.network_worker_threads);
	CHECK(cfg.network_max_connections == 1024, "max_connections=%d", cfg.network_max_connections);
	CHECK(cfg.max_value_size == 10485760u, "max_value_bytes=%zu", cfg.max_value_size);
	CHECK(cfg.ttl_enabled && cfg.ttl_scan_enabled, "TTL defaults disabled");
	CHECK(cfg.enable_rdb && cfg.enable_aof, "persistence defaults disabled");
	for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
		const kvs_config_field_descriptor_t *desc = kvs_config_descriptor_find(fields[i]);
		CHECK(desc && desc->status == KVS_CONFIG_STATUS_IMPLEMENTED,
			"%s is not implemented", fields[i]);
	}
	return 0;
}

static int test_effective_values_paths_and_inactive_status(void) {
	char path[PATH_MAX];
	char data_dir[PATH_MAX];
	char expected[PATH_MAX];
	char output[16384];
	kvs_config_t cfg;
	kvs_config_error_t error;
	FILE *fp;
	size_t length;

	CHECK(snprintf(data_dir, sizeof(data_dir), "%s/runtime", g_tmpdir) < (int)sizeof(data_dir),
		"data path too long");
	CHECK(mkdir(data_dir, 0700) == 0, "mkdir runtime failed: %s", strerror(errno));
	CHECK(write_config(
		"effective.conf",
		"[server]\nconfig_root=.\ndata_dir=runtime\ntest_mode=true\n"
		"[network]\nlisten_address=127.0.0.1\nport=19001\nmode=proactor\n"
		"worker_threads=1\nmax_connections=32\nlisten_backlog=64\n"
		"max_request_bytes=8192\nmax_response_bytes=4096\nmax_commands_per_read=8\n"
		"[storage]\ndefault_engine=hash\nenabled_engines=array,hash\nmemory_backend=system\n"
		"array_initial_capacity=17\nhash_initial_capacity=31\nmax_value_bytes=128\n"
		"[ttl]\nenabled=false\nscan_enabled=false\nscan_interval_ms=50\nscan_budget=7\n"
		"[persistence]\nrdb_enabled=false\nrdb_path=db.rdb\naof_enabled=false\n"
		"aof_path=db.aof\naof_fsync=always\naof_rewrite_enabled=false\n"
		"aof_rewrite_min_bytes=100\naof_rewrite_min_growth_bytes=50\n"
		"aof_rewrite_growth_percent=10\nlegacy_dump_path=legacy.dat\n"
		"startup_recovery_order=rdb_then_aof_then_index\n"
		"[replication]\nsync_listen_port=20001\n"
		"[vector]\nindex_type=core_flat\n",
		path, sizeof(path)) == 0, "write effective config failed");
	CHECK(load_path(path, &cfg, &error) == 0, "load effective config failed: %s", error.message);
	CHECK(cfg.test_mode && cfg.network_mode == NETWORK_PROACTOR, "server/network values wrong");
	CHECK(cfg.storage_enabled_engine_mask ==
		(KVS_STORAGE_ENGINE_ARRAY | KVS_STORAGE_ENGINE_HASH), "engine mask=%u",
		cfg.storage_enabled_engine_mask);
	CHECK(strcmp(cfg.storage_default_engine, "hash") == 0, "default engine=%s",
		cfg.storage_default_engine);
	CHECK(!cfg.ttl_enabled && !cfg.enable_rdb && !cfg.enable_aof, "disabled flags not applied");
	CHECK(snprintf(expected, sizeof(expected), "%s/db.rdb", data_dir) < (int)sizeof(expected),
		"expected RDB path too long");
	CHECK(strcmp(cfg.rdb_filename, expected) == 0, "RDB path=%s expected=%s", cfg.rdb_filename, expected);
	fp = tmpfile();
	CHECK(fp != NULL, "tmpfile failed");
	CHECK(kvs_config_print_effective(&cfg, fp) == 0, "effective print failed");
	rewind(fp);
	length = fread(output, 1, sizeof(output) - 1, fp);
	output[length] = '\0';
	fclose(fp);
	CHECK(strstr(output, "persistence.rdb_path=") && strstr(output, "status=inactive"),
		"disabled RDB path not marked inactive");
	CHECK(strstr(output, "ttl.scan_budget=7") && strstr(output, "status=inactive"),
		"disabled TTL scan budget not marked inactive");
	return 0;
}

static int test_validation_failures(void) {
	const char *disabled_persistence =
		"[persistence]\nrdb_enabled=false\naof_enabled=false\n"
		"[vector]\nindex_type=core_flat\n";
	char body[PATH_MAX + 2048];
	char target_dir[PATH_MAX];
	char locked_dir[PATH_MAX];

	snprintf(body, sizeof(body), "[network]\nworker_threads=2\n%s", disabled_persistence);
	CHECK(expect_failure("workers.conf", body, "out of range") == 0, "worker case failed");
	snprintf(body, sizeof(body), "[network]\nlisten_address=localhost\n%s", disabled_persistence);
	CHECK(expect_failure("address.conf", body, "valid IPv4") == 0, "address case failed");
	snprintf(body, sizeof(body),
		"[storage]\ndefault_engine=hash\nenabled_engines=array\n%s", disabled_persistence);
	CHECK(expect_failure("default-engine.conf", body, "must be present") == 0,
		"default engine case failed");
	snprintf(body, sizeof(body),
		"[storage]\nenabled_engines=array,array\n%s", disabled_persistence);
	CHECK(expect_failure("duplicate-engine.conf", body, "duplicate storage engine") == 0,
		"duplicate engine case failed");
	snprintf(body, sizeof(body),
		"[persistence]\nrdb_enabled=false\naof_enabled=false\n"
		"rdb_path=same\naof_path=same\nlegacy_dump_path=other\n"
		"[vector]\nindex_type=core_flat\n");
	CHECK(expect_failure("path-conflict.conf", body, "must be distinct") == 0,
		"path conflict case failed");
	snprintf(body, sizeof(body),
		"[persistence]\nrdb_enabled=false\naof_enabled=false\n"
		"startup_recovery_order=aof_then_rdb\n[vector]\nindex_type=core_flat\n");
	CHECK(expect_failure("recovery-order.conf", body, "invalid enum") == 0,
		"recovery order case failed");
	CHECK(snprintf(target_dir, sizeof(target_dir), "%s/rdb-target", g_tmpdir) < (int)sizeof(target_dir),
		"target dir path too long");
	CHECK(mkdir(target_dir, 0700) == 0, "mkdir target failed");
	snprintf(body, sizeof(body),
		"[persistence]\nrdb_enabled=true\nrdb_path=%s\naof_enabled=false\n"
		"[vector]\nindex_type=core_flat\n", target_dir);
	CHECK(expect_failure("directory-target.conf", body, "not a regular file") == 0,
		"directory target case failed");
	snprintf(body, sizeof(body),
		"[server]\ndata_dir=missing/runtime\n"
		"[persistence]\nrdb_enabled=true\nrdb_path=db.rdb\naof_enabled=false\n"
		"[vector]\nindex_type=core_flat\n");
	CHECK(expect_failure("missing-parent.conf", body, "parent directory") == 0,
		"missing parent case failed");
	CHECK(snprintf(locked_dir, sizeof(locked_dir), "%s/locked", g_tmpdir) < (int)sizeof(locked_dir),
		"locked dir path too long");
	CHECK(mkdir(locked_dir, 0500) == 0, "mkdir locked failed");
	snprintf(body, sizeof(body),
		"[persistence]\nrdb_enabled=true\nrdb_path=%s/db.rdb\naof_enabled=false\n"
		"[vector]\nindex_type=core_flat\n", locked_dir);
	CHECK(expect_failure("permission.conf", body, "not writable") == 0,
		"permission case failed");
	CHECK(chmod(locked_dir, 0700) == 0, "restore locked directory mode failed");
	return 0;
}

int main(void) {
	char template[] = "/tmp/kvlite-unified-config-stage2-XXXXXX";
	int failures = 0;

	CHECK(mkdtemp(template) != NULL, "mkdtemp failed: %s", strerror(errno));
	CHECK(snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", template) < (int)sizeof(g_tmpdir),
		"temporary path too long");
	failures += test_stage2_defaults_and_descriptors() != 0;
	failures += test_effective_values_paths_and_inactive_status() != 0;
	failures += test_validation_failures() != 0;
	if (failures) {
		fprintf(stderr, "unified config Stage 2 config tests failed: %d group(s)\n", failures);
		return 1;
	}
	printf("unified config Stage 2 config tests: PASS\n");
	return 0;
}
