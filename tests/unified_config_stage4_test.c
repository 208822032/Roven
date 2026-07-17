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
	int written = snprintf(content, sizeof(content),
		"[server]\nconfig_root=.\ndata_dir=data\n"
		"[persistence]\nrdb_enabled=false\naof_enabled=false\n"
		"[vector]\nenabled=false\n"
		"[vector_snapshot]\nenabled=false\n%s", body ? body : "");
	if (written < 0 || (size_t)written >= sizeof(content)) return -1;
	return write_named(name, content, out, out_size);
}

static int load_path(kvs_config_t *cfg, const char *path, FILE *warnings,
	kvs_config_error_t *error) {
	char *argv[] = {(char *)"stage4-config-test", (char *)"--config", (char *)path};
	return kvs_config_load(cfg, 3, argv, NULL, warnings, error);
}

static int expect_failure(const char *name, const char *body, const char *needle) {
	char path[PATH_MAX];
	kvs_config_t cfg;
	kvs_config_error_t error;
	int rc;

	CHECK(write_config(name, body, path, sizeof(path)) == 0, "write %s failed", name);
	rc = load_path(&cfg, path, NULL, &error);
	CHECK(rc != 0, "%s unexpectedly loaded", name);
	CHECK(strstr(error.message, needle) != NULL,
		"%s error '%s' does not contain '%s'", name, error.message, needle);
	return 0;
}

static int test_tcp_matrix(void) {
	char path[PATH_MAX];
	kvs_config_t cfg;
	kvs_config_error_t error;

	CHECK(write_config("master.conf",
		"[server]\nrole=master\n[network]\nport=19000\n"
		"[replication]\ntransport=tcp\nsync_listen_port=20000\n"
		"full_sync_policy=required\nnot_ready_query_policy=reject\n",
		path, sizeof(path)) == 0, "write master failed");
	CHECK(load_path(&cfg, path, NULL, &error) == 0, "master failed: %s", error.message);
	CHECK(cfg.replication_transport == KVS_REPLICATION_TCP &&
		cfg.replication_sync_listen_port == 20000,
		"master TCP fields incorrect");

	CHECK(write_config("slave.conf",
		"[server]\nrole=slave\n[network]\nport=19001\n"
		"[replication]\ntransport=tcp\nmaster_host=127.0.0.1\n"
		"master_service_port=19000\nmaster_sync_port=20000\n",
		path, sizeof(path)) == 0, "write slave failed");
	CHECK(load_path(&cfg, path, NULL, &error) == 0, "slave failed: %s", error.message);
	CHECK(cfg.replication_master_sync_port == 20000 &&
		strcmp(cfg.replication_master_host, "127.0.0.1") == 0,
		"slave TCP fields incorrect");

	CHECK(expect_failure("slave-missing.conf",
		"[server]\nrole=slave\n[replication]\ntransport=tcp\n",
		"TCP slave requires") == 0, "missing slave endpoint test failed");
	CHECK(expect_failure("port-conflict.conf",
		"[server]\nrole=master\n[network]\nport=19000\n"
		"[replication]\ntransport=tcp\nsync_listen_port=19000\n",
		"conflicts") == 0, "listen conflict test failed");
	CHECK(expect_failure("not-ready.conf",
		"[replication]\nnot_ready_query_policy=allow\n",
		"invalid enum") == 0, "not-ready policy test failed");
	CHECK(expect_failure("full-sync.conf",
		"[replication]\nfull_sync_policy=optional\n",
		"invalid enum") == 0, "full-sync policy test failed");
	return 0;
}

static int test_legacy_conversion(void) {
	char path[PATH_MAX];
	char warnings_text[8192];
	kvs_config_t cfg;
	kvs_config_error_t error;
	FILE *warnings = tmpfile();
	size_t length;

	CHECK(warnings != NULL, "tmpfile failed");
	CHECK(write_named("legacy-tcp.conf",
		"config_root=.\ndata_dir=data\nrole=slave\nport=19001\n"
		"master_ip=127.0.0.1\nmaster_port=19000\nsync_port=20000\n"
		"sync_mode=tcp\nenable_rdb=false\nenable_aof=false\nvector_enabled=false\n",
		path, sizeof(path)) == 0, "write legacy TCP failed");
	CHECK(load_path(&cfg, path, warnings, &error) == 0,
		"legacy TCP failed: %s", error.message);
	CHECK(cfg.replication_transport == KVS_REPLICATION_TCP &&
		cfg.replication_master_sync_port == 20000 &&
		cfg.replication_sync_listen_port == 0,
		"legacy sync_port was not split by role");
	CHECK(kvs_config_source_get(&cfg, "replication.master_host")->legacy_alias,
		"legacy master_ip source missing");
	rewind(warnings);
	length = fread(warnings_text, 1, sizeof(warnings_text) - 1, warnings);
	warnings_text[length] = '\0';
	fclose(warnings);
	CHECK(strstr(warnings_text, "deprecated") != NULL,
		"legacy warnings missing: %s", warnings_text);
	return 0;
}

#if KVS_ENABLE_EBPF
static int test_ebpf_modes(void) {
	char object_path[PATH_MAX];
	char path[PATH_MAX];
	char body[4096];
	kvs_config_t cfg;
	kvs_config_error_t error;

	CHECK(write_named("ebpf.o", "test-object", object_path, sizeof(object_path)) == 0,
		"write object failed");
	CHECK(snprintf(body, sizeof(body),
		"[server]\nrole=master\n[network]\nport=19000\n"
		"[replication]\ntransport=tcp\nsync_listen_port=20000\n"
		"[ebpf]\nmode=observe\nobject_path=ebpf.o\nobserve_host=127.0.0.1\n"
		"observe_port=19000\nring_buffer_bytes=8192\nmax_event_bytes=2048\n"
		"user_queue_capacity=32\nflush_interval_us=500\n") < (int)sizeof(body),
		"observe body too long");
	CHECK(write_config("observe.conf", body, path, sizeof(path)) == 0, "write observe failed");
	CHECK(load_path(&cfg, path, NULL, &error) == 0, "observe+TCP failed: %s", error.message);
	CHECK(cfg.ebpf_mode == KVS_EBPF_OBSERVE &&
		strcmp(cfg.ebpf_object_path, object_path) == 0,
		"observe config not retained");

	CHECK(expect_failure("mirror-conflict.conf",
		"[server]\nrole=master\n[network]\nport=19000\n"
		"[replication]\ntransport=tcp\nsync_listen_port=20000\n"
		"[ebpf]\nmode=experimental_mirror\nobject_path=ebpf.o\n"
		"observe_host=127.0.0.1\nobserve_port=19000\n"
		"mirror_targets=127.0.0.1:19001\n",
		"conflicts") == 0, "mirror/TCP conflict test failed");

	CHECK(write_config("mirror.conf",
		"[replication]\ntransport=disabled\n"
		"[ebpf]\nmode=experimental_mirror\nobject_path=ebpf.o\n"
		"observe_host=127.0.0.1\nobserve_port=19000\n"
		"mirror_targets=127.0.0.1:19001,127.0.0.1:19002\n",
		path, sizeof(path)) == 0, "write mirror failed");
	CHECK(load_path(&cfg, path, NULL, &error) == 0, "mirror failed: %s", error.message);
	CHECK(cfg.ebpf_mirror_target_count == 2 &&
		cfg.ebpf_mirror_endpoints[1].port == 19002,
		"mirror targets were not derived");

	CHECK(expect_failure("missing-object.conf",
		"[ebpf]\nmode=observe\nobject_path=missing.o\nobserve_host=127.0.0.1\n",
		"readable regular file") == 0, "missing object test failed");
	CHECK(expect_failure("ring.conf",
		"[ebpf]\nmode=observe\nobject_path=ebpf.o\nobserve_host=127.0.0.1\n"
		"ring_buffer_bytes=5000\n",
		"power of two") == 0, "ring size test failed");
	CHECK(expect_failure("duplicate-target.conf",
		"[ebpf]\nmode=experimental_mirror\nobject_path=ebpf.o\n"
		"observe_host=127.0.0.1\nmirror_targets=127.0.0.1:19001,127.0.0.1:19001\n",
		"duplicate") == 0, "duplicate target test failed");
	return 0;
}

static int test_legacy_ebpf_warning(void) {
	char path[PATH_MAX];
	char warning_text[8192];
	kvs_config_t cfg;
	kvs_config_error_t error;
	FILE *warnings = tmpfile();
	size_t length;

	CHECK(warnings != NULL, "tmpfile failed");
	CHECK(write_named("legacy-ebpf.conf",
		"config_root=.\ndata_dir=data\nsync_mode=ebpf\n"
		"enable_rdb=false\nenable_aof=false\nvector_enabled=false\n"
		"[ebpf]\nobject_path=ebpf.o\nobserve_host=127.0.0.1\n"
		"mirror_targets=127.0.0.1:19001\n",
		path, sizeof(path)) == 0, "write legacy eBPF failed");
	CHECK(load_path(&cfg, path, warnings, &error) == 0,
		"legacy eBPF failed: %s", error.message);
	CHECK(cfg.replication_transport == KVS_REPLICATION_DISABLED &&
		cfg.ebpf_mode == KVS_EBPF_EXPERIMENTAL_MIRROR,
		"legacy eBPF conversion incorrect");
	rewind(warnings);
	length = fread(warning_text, 1, sizeof(warning_text) - 1, warnings);
	warning_text[length] = '\0';
	fclose(warnings);
	CHECK(strstr(warning_text, "HIGH") != NULL && strstr(warning_text, "experimental_mirror") != NULL,
		"high visibility warning missing: %s", warning_text);
	return 0;
}
#else
static int test_capability_failure(void) {
	CHECK(expect_failure("capability.conf",
		"[ebpf]\nmode=observe\nobject_path=ebpf.o\nobserve_host=127.0.0.1\n",
		"KVS_ENABLE_EBPF=1") == 0, "capability test failed");
	return 0;
}
#endif

static int test_future_fields(void) {
	CHECK(expect_failure("backlog.conf", "[replication]\nbacklog_bytes=1024\n",
		"future") == 0, "backlog test failed");
	CHECK(expect_failure("ack.conf", "[replication]\nack_interval_ms=10\n",
		"future") == 0, "ack test failed");
	CHECK(expect_failure("reconnect.conf", "[replication]\nreconnect_backoff_ms=10\n",
		"future") == 0, "reconnect test failed");
	CHECK(expect_failure("offset.conf", "[replication]\noffset=1\n",
		"future") == 0, "offset test failed");
	CHECK(expect_failure("partial.conf", "[replication]\npartial_resync=true\n",
		"future") == 0, "partial resync test failed");
	CHECK(expect_failure("promotion.conf", "[replication]\npromotion_policy=manual\n",
		"future") == 0, "promotion policy test failed");
	return 0;
}

int main(void) {
	char template_path[] = "/tmp/kvlite-unified-config-stage4-XXXXXX";
	char data_dir[PATH_MAX];

	CHECK(mkdtemp(template_path) != NULL, "mkdtemp failed: %s", strerror(errno));
	snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", template_path);
	CHECK(snprintf(data_dir, sizeof(data_dir), "%s/data", g_tmpdir) < (int)sizeof(data_dir),
		"data path too long");
	CHECK(mkdir(data_dir, 0700) == 0, "mkdir data failed: %s", strerror(errno));

	CHECK(test_tcp_matrix() == 0, "TCP matrix failed");
	CHECK(test_legacy_conversion() == 0, "legacy conversion failed");
	CHECK(test_future_fields() == 0, "future field test failed");
#if KVS_ENABLE_EBPF
	CHECK(test_ebpf_modes() == 0, "eBPF mode test failed");
	CHECK(test_legacy_ebpf_warning() == 0, "legacy eBPF test failed");
#else
	CHECK(test_capability_failure() == 0, "eBPF capability test failed");
#endif

	printf("unified_config_stage4_test PASS capability=%d\n", KVS_ENABLE_EBPF);
	return 0;
}
