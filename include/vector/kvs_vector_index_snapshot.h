#pragma once

#include <stddef.h>
#include <stdint.h>

#define KVS_VECTOR_INDEX_MANIFEST_MAGIC "KVLITE_HNSW_SNAPSHOT"
#define KVS_VECTOR_INDEX_MANIFEST_VERSION 1
#define KVS_VECTOR_INDEX_SNAPSHOT_FORMAT_VERSION 1

typedef struct kvs_vector_index_snapshot_manifest_s {
	char magic[32];

	uint32_t manifest_version;
	uint32_t snapshot_format_version;

	uint32_t backend;
	uint32_t dim;
	uint32_t metric;

	uint32_t hnsw_m;
	uint32_t hnsw_m0;
	uint32_t hnsw_ef_construction;
	uint32_t hnsw_ef_search;

	uint64_t active_count;
	uint64_t deleted_count;

	uint64_t record_generation;
	uint64_t record_id_checksum;
	uint64_t vector_checksum;

	uint64_t snapshot_size;
	uint64_t snapshot_checksum;

	uint64_t created_at_ms;
} kvs_vector_index_snapshot_manifest_t;

int kvs_vector_index_snapshot_manifest_init(
	kvs_vector_index_snapshot_manifest_t *manifest
);

int kvs_vector_index_snapshot_manifest_write_file(
	const char *path,
	const kvs_vector_index_snapshot_manifest_t *manifest
);

int kvs_vector_index_snapshot_manifest_read_file(
	const char *path,
	kvs_vector_index_snapshot_manifest_t *manifest
);

int kvs_vector_index_snapshot_manifest_validate(
	const kvs_vector_index_snapshot_manifest_t *actual,
	const kvs_vector_index_snapshot_manifest_t *expected,
	char *reason,
	size_t reason_size
);

uint64_t kvs_vector_index_snapshot_checksum_file(const char *path);

int kvs_vector_index_snapshot_default_manifest_path(char *buf, size_t size);

int kvs_vector_index_snapshot_default_snapshot_path(char *buf, size_t size);
