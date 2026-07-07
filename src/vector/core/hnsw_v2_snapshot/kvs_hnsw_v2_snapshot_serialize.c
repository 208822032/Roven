#include "vector/kvs_vector_hnsw_v2_snapshot.h"

#include "kvs_hnsw_v2_snapshot_format.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint64_t kvs_hnsw_v2_snapshot_checksum_payload(
	const kvs_hnsw_v2_index_t *index
) {
	uint64_t checksum = KVS_HNSW_V2_SNAPSHOT_FNV_OFFSET;
	size_t i;

	if (!index || index->size == 0) {
		return checksum;
	}

	checksum = kvs_hnsw_v2_snapshot_checksum_update(
		checksum,
		index->slot_to_record_id,
		index->size * sizeof(uint64_t)
	);
	for (i = 0; i < index->size; i++) {
		const float *vec = kvs_hnsw_v2_slot_vector(index, (uint32_t)i);
		checksum = kvs_hnsw_v2_snapshot_checksum_update(
			checksum,
			vec,
			index->dim * sizeof(float)
		);
	}
	checksum = kvs_hnsw_v2_snapshot_checksum_update(
		checksum,
		index->levels,
		index->size * sizeof(uint8_t)
	);
	checksum = kvs_hnsw_v2_snapshot_checksum_update(
		checksum,
		index->deleted_flags,
		index->size * sizeof(uint8_t)
	);
	checksum = kvs_hnsw_v2_snapshot_checksum_update(
		checksum,
		index->level0_counts,
		index->size * sizeof(uint16_t)
	);
	checksum = kvs_hnsw_v2_snapshot_checksum_update(
		checksum,
		index->level0_neighbors,
		index->size * index->M0 * sizeof(uint32_t)
	);
	checksum = kvs_hnsw_v2_snapshot_checksum_update(
		checksum,
		index->upper_counts,
		index->size * (KVS_HNSW_V2_MAX_LEVEL - 1) * sizeof(uint16_t)
	);
	checksum = kvs_hnsw_v2_snapshot_checksum_update(
		checksum,
		index->upper_neighbors,
		index->size * (KVS_HNSW_V2_MAX_LEVEL - 1) * index->M *
			sizeof(uint32_t)
	);

	return checksum;
}

static int kvs_hnsw_v2_snapshot_write_payload(
	FILE *fp,
	const kvs_hnsw_v2_index_t *index
) {
	size_t i;

	if (index->size == 0) {
		return KVS_VECTOR_OK;
	}

	if (kvs_hnsw_v2_snapshot_write_exact(
		    fp,
		    index->slot_to_record_id,
		    index->size * sizeof(uint64_t)
	    ) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	for (i = 0; i < index->size; i++) {
		const float *vec = kvs_hnsw_v2_slot_vector(index, (uint32_t)i);

		if (!vec ||
		    kvs_hnsw_v2_snapshot_write_exact(
			    fp,
			    vec,
			    index->dim * sizeof(float)
		    ) != KVS_VECTOR_OK) {
			return KVS_VECTOR_ERR;
		}
	}
	if (kvs_hnsw_v2_snapshot_write_exact(
		    fp,
		    index->levels,
		    index->size * sizeof(uint8_t)
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_write_exact(
		    fp,
		    index->deleted_flags,
		    index->size * sizeof(uint8_t)
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_write_exact(
		    fp,
		    index->level0_counts,
		    index->size * sizeof(uint16_t)
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_write_exact(
		    fp,
		    index->level0_neighbors,
		    index->size * index->M0 * sizeof(uint32_t)
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_write_exact(
		    fp,
		    index->upper_counts,
		    index->size * (KVS_HNSW_V2_MAX_LEVEL - 1) * sizeof(uint16_t)
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_write_exact(
		    fp,
		    index->upper_neighbors,
		    index->size * (KVS_HNSW_V2_MAX_LEVEL - 1) * index->M *
			    sizeof(uint32_t)
	    ) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}

	return KVS_VECTOR_OK;
}

static int kvs_hnsw_v2_snapshot_fill_header(
	const kvs_hnsw_v2_index_t *index,
	kvs_hnsw_v2_snapshot_header_t *header
) {
	uint64_t payload_size = 0;

	if (!index || !header) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (kvs_hnsw_v2_snapshot_payload_size(
		    (uint64_t)index->size,
		    (uint32_t)index->dim,
		    (uint32_t)index->M,
		    (uint32_t)index->M0,
		    &payload_size
	    ) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(header, 0, sizeof(*header));
	snprintf(header->magic, sizeof(header->magic), "%s", KVS_HNSW_V2_SNAPSHOT_MAGIC);
	header->format_version = KVS_HNSW_V2_SNAPSHOT_FORMAT_VERSION;
	header->header_size = (uint32_t)sizeof(*header);
	header->dim = (uint32_t)index->dim;
	header->metric = (uint32_t)index->metric;
	header->m = (uint32_t)index->M;
	header->m0 = (uint32_t)index->M0;
	header->ef_construction = (uint32_t)index->ef_construction;
	header->ef_search = (uint32_t)index->ef_search;
	header->size = (uint64_t)index->size;
	header->capacity = (uint64_t)index->capacity;
	header->deleted_count = (uint64_t)index->deleted_count;
	header->active_count = header->size - header->deleted_count;
	header->entry_point = index->has_entry ? (int64_t)index->entry_point : -1;
	header->entry_level = index->has_entry ?
		(int32_t)index->levels[index->entry_point] : -1;
	header->max_level = (int32_t)index->max_level;
	header->rng_state = index->rng.state;
	header->payload_size = payload_size;
	header->payload_checksum = kvs_hnsw_v2_snapshot_checksum_payload(index);
	return KVS_VECTOR_OK;
}

int kvs_hnsw_v2_index_serialize_file(
	const kvs_hnsw_v2_index_t *index,
	const char *path
) {
	kvs_hnsw_v2_snapshot_header_t header;
	char tmp_path[4096];
	char reason[128];
	FILE *fp;
	int ret = KVS_VECTOR_ERR;

	if (!index || !path || path[0] == '\0') {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (kvs_hnsw_v2_index_validate_basic(index, reason, sizeof(reason)) !=
	    KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	if (kvs_hnsw_v2_snapshot_fill_header(index, &header) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >=
	    (int)sizeof(tmp_path)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	fp = fopen(tmp_path, "wb");
	if (!fp) {
		return KVS_VECTOR_ERR;
	}

	if (kvs_hnsw_v2_snapshot_write_exact(fp, &header, sizeof(header)) ==
		    KVS_VECTOR_OK &&
	    kvs_hnsw_v2_snapshot_write_payload(fp, index) == KVS_VECTOR_OK &&
	    fflush(fp) == 0 &&
	    ferror(fp) == 0) {
		ret = KVS_VECTOR_OK;
	}
	if (fclose(fp) != 0) {
		ret = KVS_VECTOR_ERR;
	}
	if (ret == KVS_VECTOR_OK && rename(tmp_path, path) != 0) {
		ret = KVS_VECTOR_ERR;
	}
	if (ret != KVS_VECTOR_OK) {
		(void)unlink(tmp_path);
	}
	return ret;
}
