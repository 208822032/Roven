#include "kvs_vector.h"
#include "vector/kvs_vector_hnsw_v2.h"
#include "vector/kvs_vector_index_snapshot.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int expect_true(int cond, const char *label) {
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", label);
		return 1;
	}
	return 0;
}

static int expect_reason_contains(const char *reason, const char *needle) {
	return reason && needle && strstr(reason, needle) != NULL;
}

static kvs_vector_index_snapshot_manifest_t make_manifest(void) {
	kvs_vector_index_snapshot_manifest_t manifest;

	kvs_vector_index_snapshot_manifest_init(&manifest);
	manifest.backend = (uint32_t)KVS_VECTOR_INDEX_CORE_HNSW_V2;
	manifest.dim = 512;
	manifest.metric = (uint32_t)KVS_VECTOR_METRIC_IP;
	manifest.hnsw_m = KVS_HNSW_V2_DEFAULT_M;
	manifest.hnsw_m0 = KVS_HNSW_V2_DEFAULT_M0;
	manifest.hnsw_ef_construction = KVS_HNSW_V2_DEFAULT_EF_CONSTRUCTION;
	manifest.hnsw_ef_search = KVS_HNSW_V2_DEFAULT_EF_SEARCH;
	manifest.active_count = 100000;
	manifest.deleted_count = 7;
	manifest.record_generation = 12345;
	manifest.record_id_checksum = 111;
	manifest.vector_checksum = 222;
	manifest.snapshot_size = 333;
	manifest.snapshot_checksum = 444;
	manifest.created_at_ms = 555;
	return manifest;
}

static int manifests_equal(
	const kvs_vector_index_snapshot_manifest_t *lhs,
	const kvs_vector_index_snapshot_manifest_t *rhs
) {
	return strcmp(lhs->magic, rhs->magic) == 0 &&
	       lhs->manifest_version == rhs->manifest_version &&
	       lhs->snapshot_format_version == rhs->snapshot_format_version &&
	       lhs->backend == rhs->backend &&
	       lhs->dim == rhs->dim &&
	       lhs->metric == rhs->metric &&
	       lhs->hnsw_m == rhs->hnsw_m &&
	       lhs->hnsw_m0 == rhs->hnsw_m0 &&
	       lhs->hnsw_ef_construction == rhs->hnsw_ef_construction &&
	       lhs->hnsw_ef_search == rhs->hnsw_ef_search &&
	       lhs->active_count == rhs->active_count &&
	       lhs->deleted_count == rhs->deleted_count &&
	       lhs->record_generation == rhs->record_generation &&
	       lhs->record_id_checksum == rhs->record_id_checksum &&
	       lhs->vector_checksum == rhs->vector_checksum &&
	       lhs->snapshot_size == rhs->snapshot_size &&
	       lhs->snapshot_checksum == rhs->snapshot_checksum &&
	       lhs->created_at_ms == rhs->created_at_ms;
}

static int write_text(const char *path, const char *text) {
	FILE *fp = fopen(path, "wb");
	if (!fp) {
		return -1;
	}
	if (fputs(text, fp) < 0) {
		fclose(fp);
		return -1;
	}
	return fclose(fp);
}

static int test_init_default(void) {
	kvs_vector_index_snapshot_manifest_t manifest;
	int failed = 0;

	failed |= expect_true(
		kvs_vector_index_snapshot_manifest_init(&manifest) == 0,
		"init succeeds"
	);
	failed |= expect_true(
		strcmp(manifest.magic, KVS_VECTOR_INDEX_MANIFEST_MAGIC) == 0,
		"init sets magic"
	);
	failed |= expect_true(
		manifest.manifest_version == KVS_VECTOR_INDEX_MANIFEST_VERSION,
		"init sets manifest version"
	);
	failed |= expect_true(
		manifest.snapshot_format_version ==
			KVS_VECTOR_INDEX_SNAPSHOT_FORMAT_VERSION,
		"init sets snapshot format version"
	);
	return failed;
}

static int test_roundtrip(const char *path) {
	kvs_vector_index_snapshot_manifest_t expected = make_manifest();
	kvs_vector_index_snapshot_manifest_t actual;
	char reason[128];
	int failed = 0;

	failed |= expect_true(
		kvs_vector_index_snapshot_manifest_write_file(path, &expected) == 0,
		"manifest write succeeds"
	);
	failed |= expect_true(
		kvs_vector_index_snapshot_manifest_read_file(path, &actual) == 0,
		"manifest read succeeds"
	);
	failed |= expect_true(
		kvs_vector_index_snapshot_manifest_validate(
			&actual,
			&expected,
			reason,
			sizeof(reason)
		) == 0,
		"manifest validate succeeds"
	);
	failed |= expect_true(manifests_equal(&actual, &expected), "roundtrip preserves fields");
	return failed;
}

static int test_mismatch(
	const char *label,
	void (*mutate)(kvs_vector_index_snapshot_manifest_t *manifest),
	const char *reason_substring
) {
	kvs_vector_index_snapshot_manifest_t expected = make_manifest();
	kvs_vector_index_snapshot_manifest_t actual = expected;
	char reason[128];
	int ret;
	int failed = 0;

	mutate(&actual);
	reason[0] = '\0';
	ret = kvs_vector_index_snapshot_manifest_validate(
		&actual,
		&expected,
		reason,
		sizeof(reason)
	);
	failed |= expect_true(ret != 0, label);
	failed |= expect_true(
		expect_reason_contains(reason, reason_substring),
		reason_substring
	);
	return failed;
}

static void mutate_magic(kvs_vector_index_snapshot_manifest_t *m) {
	snprintf(m->magic, sizeof(m->magic), "%s", "BAD_MAGIC");
}

static void mutate_manifest_version(kvs_vector_index_snapshot_manifest_t *m) {
	m->manifest_version++;
}

static void mutate_snapshot_format_version(kvs_vector_index_snapshot_manifest_t *m) {
	m->snapshot_format_version++;
}

static void mutate_backend(kvs_vector_index_snapshot_manifest_t *m) {
	m->backend = (uint32_t)KVS_VECTOR_INDEX_CORE_FLAT;
}

static void mutate_dim(kvs_vector_index_snapshot_manifest_t *m) {
	m->dim++;
}

static void mutate_metric(kvs_vector_index_snapshot_manifest_t *m) {
	m->metric = (uint32_t)KVS_VECTOR_METRIC_L2;
}

static void mutate_hnsw(kvs_vector_index_snapshot_manifest_t *m) {
	m->hnsw_ef_search++;
}

static void mutate_active_count(kvs_vector_index_snapshot_manifest_t *m) {
	m->active_count++;
}

static void mutate_deleted_count(kvs_vector_index_snapshot_manifest_t *m) {
	m->deleted_count++;
}

static void mutate_record_generation(kvs_vector_index_snapshot_manifest_t *m) {
	m->record_generation++;
}

static void mutate_record_id_checksum(kvs_vector_index_snapshot_manifest_t *m) {
	m->record_id_checksum++;
}

static void mutate_vector_checksum(kvs_vector_index_snapshot_manifest_t *m) {
	m->vector_checksum++;
}

static void mutate_snapshot_size(kvs_vector_index_snapshot_manifest_t *m) {
	m->snapshot_size++;
}

static void mutate_snapshot_checksum(kvs_vector_index_snapshot_manifest_t *m) {
	m->snapshot_checksum++;
}

static int test_checksum_helper(const char *path) {
	uint64_t first;
	uint64_t second;
	int failed = 0;

	failed |= expect_true(write_text(path, "snapshot-bytes\n") == 0, "write checksum fixture");
	first = kvs_vector_index_snapshot_checksum_file(path);
	second = kvs_vector_index_snapshot_checksum_file(path);
	failed |= expect_true(first != 0, "checksum is nonzero for existing file");
	failed |= expect_true(first == second, "checksum is deterministic");
	return failed;
}

static int test_missing_and_malformed(const char *missing_path, const char *bad_path) {
	kvs_vector_index_snapshot_manifest_t manifest;
	int failed = 0;

	failed |= expect_true(
		kvs_vector_index_snapshot_manifest_read_file(missing_path, &manifest) != 0,
		"missing manifest fails"
	);
	failed |= expect_true(write_text(bad_path, "this is not key value\n") == 0,
	                      "write malformed manifest");
	failed |= expect_true(
		kvs_vector_index_snapshot_manifest_read_file(bad_path, &manifest) != 0,
		"malformed manifest fails"
	);
	failed |= expect_true(
		write_text(
			bad_path,
			"magic=KVLITE_HNSW_SNAPSHOT\n"
			"manifest_version=1\n"
		) == 0,
		"write incomplete manifest"
	);
	failed |= expect_true(
		kvs_vector_index_snapshot_manifest_read_file(bad_path, &manifest) != 0,
		"incomplete manifest fails"
	);
	return failed;
}

int main(void) {
	char manifest_path[256];
	char checksum_path[256];
	char missing_path[256];
	char malformed_path[256];
	int failed = 0;
	pid_t pid = getpid();

	snprintf(
		manifest_path,
		sizeof(manifest_path),
		"/tmp/kvlite_vector_index_manifest_%ld.manifest",
		(long)pid
	);
	snprintf(
		checksum_path,
		sizeof(checksum_path),
		"/tmp/kvlite_vector_index_manifest_%ld.snapshot",
		(long)pid
	);
	snprintf(
		missing_path,
		sizeof(missing_path),
		"/tmp/kvlite_vector_index_manifest_%ld.missing",
		(long)pid
	);
	snprintf(
		malformed_path,
		sizeof(malformed_path),
		"/tmp/kvlite_vector_index_manifest_%ld.bad",
		(long)pid
	);

	(void)unlink(manifest_path);
	(void)unlink(checksum_path);
	(void)unlink(missing_path);
	(void)unlink(malformed_path);

	failed |= test_init_default();
	failed |= test_roundtrip(manifest_path);
	failed |= test_mismatch("magic mismatch", mutate_magic, "magic mismatch");
	failed |= test_mismatch(
		"manifest version mismatch",
		mutate_manifest_version,
		"manifest_version mismatch"
	);
	failed |= test_mismatch(
		"snapshot format version mismatch",
		mutate_snapshot_format_version,
		"snapshot_format_version mismatch"
	);
	failed |= test_mismatch("backend mismatch", mutate_backend, "backend mismatch");
	failed |= test_mismatch("dim mismatch", mutate_dim, "dim mismatch");
	failed |= test_mismatch("metric mismatch", mutate_metric, "metric mismatch");
	failed |= test_mismatch("HNSW params mismatch", mutate_hnsw, "HNSW parameter mismatch");
	failed |= test_mismatch("active_count mismatch", mutate_active_count, "active_count mismatch");
	failed |= test_mismatch("deleted_count mismatch", mutate_deleted_count, "deleted_count mismatch");
	failed |= test_mismatch(
		"record_generation mismatch",
		mutate_record_generation,
		"record_generation mismatch"
	);
	failed |= test_mismatch(
		"record_id_checksum mismatch",
		mutate_record_id_checksum,
		"record_id_checksum mismatch"
	);
	failed |= test_mismatch(
		"vector_checksum mismatch",
		mutate_vector_checksum,
		"vector_checksum mismatch"
	);
	failed |= test_mismatch("snapshot_size mismatch", mutate_snapshot_size, "snapshot_size mismatch");
	failed |= test_mismatch(
		"snapshot_checksum mismatch",
		mutate_snapshot_checksum,
		"snapshot_checksum mismatch"
	);
	failed |= test_checksum_helper(checksum_path);
	failed |= test_missing_and_malformed(missing_path, malformed_path);

	(void)unlink(manifest_path);
	(void)unlink(checksum_path);
	(void)unlink(malformed_path);

	if (failed) {
		fprintf(stderr, "vector_index_snapshot_manifest_test FAILED\n");
		return 1;
	}

	printf("vector_index_snapshot_manifest_test PASS\n");
	return 0;
}
