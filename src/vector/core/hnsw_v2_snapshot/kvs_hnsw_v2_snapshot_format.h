#pragma once

#include "vector/kvs_vector_hnsw_v2.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define KVS_HNSW_V2_SNAPSHOT_MAGIC "KVLITE_HNSW_V2_GRAPH"
#define KVS_HNSW_V2_SNAPSHOT_FORMAT_VERSION 1u
#define KVS_HNSW_V2_SNAPSHOT_FNV_OFFSET 1469598103934665603ull
#define KVS_HNSW_V2_SNAPSHOT_FNV_PRIME 1099511628211ull

typedef struct kvs_hnsw_v2_snapshot_header_s {
	char magic[32];
	uint32_t format_version;
	uint32_t header_size;

	uint32_t dim;
	uint32_t metric;
	uint32_t m;
	uint32_t m0;
	uint32_t ef_construction;
	uint32_t ef_search;

	uint64_t size;
	uint64_t capacity;
	uint64_t active_count;
	uint64_t deleted_count;

	int64_t entry_point;
	int32_t entry_level;
	int32_t max_level;

	uint64_t rng_state;
	uint64_t payload_size;
	uint64_t payload_checksum;
} kvs_hnsw_v2_snapshot_header_t;

uint64_t kvs_hnsw_v2_snapshot_checksum_update(
	uint64_t checksum,
	const void *data,
	size_t len
);

size_t kvs_hnsw_v2_snapshot_vector_stride(size_t dim);

int kvs_hnsw_v2_snapshot_payload_size(
	uint64_t size,
	uint32_t dim,
	uint32_t m,
	uint32_t m0,
	uint64_t *out_size
);

int kvs_hnsw_v2_snapshot_write_exact(
	FILE *fp,
	const void *data,
	size_t len
);

int kvs_hnsw_v2_snapshot_read_exact(
	FILE *fp,
	void *data,
	size_t len
);

int kvs_hnsw_v2_snapshot_file_size(
	const char *path,
	uint64_t *out_size
);

void kvs_hnsw_v2_snapshot_set_reason(
	char *reason,
	size_t reason_size,
	const char *message
);
