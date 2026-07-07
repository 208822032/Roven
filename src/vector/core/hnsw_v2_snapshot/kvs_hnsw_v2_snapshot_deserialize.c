#include "vector/kvs_vector_hnsw_v2_snapshot.h"

#include "kvs_hnsw_v2_snapshot_format.h"

#include <stdio.h>
#include <string.h>

static int kvs_hnsw_v2_snapshot_header_fail(
	char *reason,
	size_t reason_size,
	const char *message
) {
	kvs_hnsw_v2_snapshot_set_reason(reason, reason_size, message);
	return KVS_VECTOR_ERR;
}

static int kvs_hnsw_v2_snapshot_validate_header(
	const kvs_hnsw_v2_snapshot_header_t *header,
	uint64_t file_size,
	char *reason,
	size_t reason_size
) {
	uint64_t expected_payload_size = 0;
	uint64_t expected_file_size = 0;

	if (!header) {
		return kvs_hnsw_v2_snapshot_header_fail(
			reason,
			reason_size,
			"null header"
		);
	}
	if (strncmp(
		    header->magic,
		    KVS_HNSW_V2_SNAPSHOT_MAGIC,
		    sizeof(header->magic)
	    ) != 0) {
		return kvs_hnsw_v2_snapshot_header_fail(
			reason,
			reason_size,
			"magic mismatch"
		);
	}
	if (header->format_version != KVS_HNSW_V2_SNAPSHOT_FORMAT_VERSION ||
	    header->header_size != sizeof(*header)) {
		return kvs_hnsw_v2_snapshot_header_fail(
			reason,
			reason_size,
			"format mismatch"
		);
	}
	if (header->dim == 0 || header->m == 0 || header->m0 == 0 ||
	    header->m0 < header->m || header->m > UINT16_MAX ||
	    header->m0 > UINT16_MAX) {
		return kvs_hnsw_v2_snapshot_header_fail(
			reason,
			reason_size,
			"invalid params"
		);
	}
	if (header->metric != (uint32_t)KVS_VECTOR_METRIC_IP &&
	    header->metric != (uint32_t)KVS_VECTOR_METRIC_L2 &&
	    header->metric != (uint32_t)KVS_VECTOR_METRIC_COSINE) {
		return kvs_hnsw_v2_snapshot_header_fail(
			reason,
			reason_size,
			"invalid metric"
		);
	}
	if (header->capacity > UINT32_MAX || header->size > header->capacity ||
	    header->deleted_count > header->size ||
	    header->active_count + header->deleted_count != header->size) {
		return kvs_hnsw_v2_snapshot_header_fail(
			reason,
			reason_size,
			"invalid size"
		);
	}
	if (header->size == 0) {
		if (header->entry_point != -1 || header->entry_level != -1 ||
		    header->max_level != 0 || header->deleted_count != 0) {
			return kvs_hnsw_v2_snapshot_header_fail(
				reason,
				reason_size,
				"invalid empty state"
			);
		}
	} else {
		if (header->entry_point < 0 ||
		    (uint64_t)header->entry_point >= header->size ||
		    header->entry_level < 0 ||
		    header->entry_level >= KVS_HNSW_V2_MAX_LEVEL ||
		    header->max_level < 0 ||
		    header->max_level >= KVS_HNSW_V2_MAX_LEVEL ||
		    header->entry_level != header->max_level) {
			return kvs_hnsw_v2_snapshot_header_fail(
				reason,
				reason_size,
				"invalid entry"
			);
		}
	}
	if (kvs_hnsw_v2_snapshot_payload_size(
		    header->size,
		    header->dim,
		    header->m,
		    header->m0,
		    &expected_payload_size
	    ) != KVS_VECTOR_OK ||
	    expected_payload_size != header->payload_size) {
		return kvs_hnsw_v2_snapshot_header_fail(
			reason,
			reason_size,
			"payload size mismatch"
		);
	}
	if (UINT64_MAX - (uint64_t)sizeof(*header) < header->payload_size) {
		return kvs_hnsw_v2_snapshot_header_fail(
			reason,
			reason_size,
			"file size overflow"
		);
	}
	expected_file_size = (uint64_t)sizeof(*header) + header->payload_size;
	if (file_size != expected_file_size) {
		return kvs_hnsw_v2_snapshot_header_fail(
			reason,
			reason_size,
			"file size mismatch"
		);
	}

	return KVS_VECTOR_OK;
}

static int kvs_hnsw_v2_snapshot_read_payload_part(
	FILE *fp,
	void *data,
	size_t len,
	uint64_t *checksum
) {
	if (kvs_hnsw_v2_snapshot_read_exact(fp, data, len) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	if (checksum) {
		*checksum = kvs_hnsw_v2_snapshot_checksum_update(*checksum, data, len);
	}
	return KVS_VECTOR_OK;
}

static int kvs_hnsw_v2_snapshot_read_payload(
	FILE *fp,
	kvs_hnsw_v2_index_t *index,
	uint64_t *out_checksum
) {
	uint64_t checksum = KVS_HNSW_V2_SNAPSHOT_FNV_OFFSET;
	size_t i;

	if (!fp || !index || !out_checksum) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (index->size == 0) {
		*out_checksum = checksum;
		return KVS_VECTOR_OK;
	}

	if (kvs_hnsw_v2_snapshot_read_payload_part(
		    fp,
		    index->slot_to_record_id,
		    index->size * sizeof(uint64_t),
		    &checksum
	    ) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}
	for (i = 0; i < index->size; i++) {
		float *vec = index->vectors + i * index->vector_stride;

		if (kvs_hnsw_v2_snapshot_read_payload_part(
			    fp,
			    vec,
			    index->dim * sizeof(float),
			    &checksum
		    ) != KVS_VECTOR_OK) {
			return KVS_VECTOR_ERR;
		}
	}
	if (kvs_hnsw_v2_snapshot_read_payload_part(
		    fp,
		    index->levels,
		    index->size * sizeof(uint8_t),
		    &checksum
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_read_payload_part(
		    fp,
		    index->deleted_flags,
		    index->size * sizeof(uint8_t),
		    &checksum
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_read_payload_part(
		    fp,
		    index->level0_counts,
		    index->size * sizeof(uint16_t),
		    &checksum
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_read_payload_part(
		    fp,
		    index->level0_neighbors,
		    index->size * index->M0 * sizeof(uint32_t),
		    &checksum
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_read_payload_part(
		    fp,
		    index->upper_counts,
		    index->size * (KVS_HNSW_V2_MAX_LEVEL - 1) * sizeof(uint16_t),
		    &checksum
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_read_payload_part(
		    fp,
		    index->upper_neighbors,
		    index->size * (KVS_HNSW_V2_MAX_LEVEL - 1) * index->M *
			    sizeof(uint32_t),
		    &checksum
	    ) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR;
	}

	*out_checksum = checksum;
	return KVS_VECTOR_OK;
}

static int kvs_hnsw_v2_snapshot_rebuild_id_map(
	kvs_hnsw_v2_index_t *index
) {
	size_t i;
	int ret;

	if (!index) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (index->capacity == 0) {
		return KVS_VECTOR_OK;
	}

	kvs_vector_id_slot_map_reset(&index->record_id_to_slot);
	for (i = 0; i < index->size; i++) {
		ret = kvs_vector_id_slot_map_put(
			&index->record_id_to_slot,
			index->slot_to_record_id[i],
			(uint32_t)i
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}
	return KVS_VECTOR_OK;
}

static int kvs_hnsw_v2_snapshot_load_temp(
	FILE *fp,
	const kvs_hnsw_v2_snapshot_header_t *header,
	kvs_hnsw_v2_index_t *temp,
	char *reason,
	size_t reason_size
) {
	uint64_t checksum = 0;
	int ret;

	memset(temp, 0, sizeof(*temp));
	ret = kvs_hnsw_v2_index_init(
		temp,
		(size_t)header->dim,
		(int)header->metric
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}
	temp->M = (size_t)header->m;
	temp->M0 = (size_t)header->m0;
	temp->ef_construction = (size_t)header->ef_construction;
	temp->ef_search = (size_t)header->ef_search;
	temp->rng.state = header->rng_state;

	if (header->capacity > 0) {
		ret = kvs_hnsw_v2_index_reserve(temp, (size_t)header->capacity);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	temp->size = (size_t)header->size;
	temp->deleted_count = (size_t)header->deleted_count;
	temp->has_entry = header->entry_point >= 0 ? 1 : 0;
	temp->entry_point = temp->has_entry ? (uint32_t)header->entry_point : 0;
	temp->max_level = (uint8_t)header->max_level;
	temp->current_visit_tag = 1;
	temp->ready = 1;
	temp->needs_rebuild = 0;

	ret = kvs_hnsw_v2_snapshot_read_payload(fp, temp, &checksum);
	if (ret != KVS_VECTOR_OK) {
		kvs_hnsw_v2_snapshot_set_reason(reason, reason_size, "payload read failed");
		return ret;
	}
	if (checksum != header->payload_checksum) {
		kvs_hnsw_v2_snapshot_set_reason(reason, reason_size, "checksum mismatch");
		return KVS_VECTOR_ERR;
	}
	if (temp->has_entry && temp->levels[temp->entry_point] != header->entry_level) {
		kvs_hnsw_v2_snapshot_set_reason(reason, reason_size, "entry level mismatch");
		return KVS_VECTOR_ERR;
	}

	ret = kvs_hnsw_v2_snapshot_rebuild_id_map(temp);
	if (ret != KVS_VECTOR_OK) {
		kvs_hnsw_v2_snapshot_set_reason(reason, reason_size, "id map rebuild failed");
		return ret;
	}

	kvs_hnsw_v2_graph_stats_refresh(temp);
	kvs_hnsw_v2_memory_refresh(temp);

	ret = kvs_hnsw_v2_index_validate_basic(temp, reason, reason_size);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	return KVS_VECTOR_OK;
}

int kvs_hnsw_v2_index_deserialize_file(
	kvs_hnsw_v2_index_t *index,
	const char *path
) {
	kvs_hnsw_v2_snapshot_header_t header;
	kvs_hnsw_v2_index_t temp;
	char reason[128];
	uint64_t file_size = 0;
	FILE *fp;
	int ret = KVS_VECTOR_ERR;

	if (!index || !path || path[0] == '\0') {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	memset(&temp, 0, sizeof(temp));
	kvs_hnsw_v2_snapshot_set_reason(reason, sizeof(reason), "ok");

	if (kvs_hnsw_v2_snapshot_file_size(path, &file_size) != KVS_VECTOR_OK ||
	    file_size < sizeof(header)) {
		return KVS_VECTOR_ERR;
	}

	fp = fopen(path, "rb");
	if (!fp) {
		return KVS_VECTOR_ERR;
	}
	if (kvs_hnsw_v2_snapshot_read_exact(fp, &header, sizeof(header)) !=
	    KVS_VECTOR_OK) {
		fclose(fp);
		return KVS_VECTOR_ERR;
	}
	if (kvs_hnsw_v2_snapshot_validate_header(
		    &header,
		    file_size,
		    reason,
		    sizeof(reason)
	    ) != KVS_VECTOR_OK) {
		fclose(fp);
		return KVS_VECTOR_ERR;
	}

	ret = kvs_hnsw_v2_snapshot_load_temp(
		fp,
		&header,
		&temp,
		reason,
		sizeof(reason)
	);
	if (fclose(fp) != 0 && ret == KVS_VECTOR_OK) {
		ret = KVS_VECTOR_ERR;
	}
	if (ret != KVS_VECTOR_OK) {
		kvs_hnsw_v2_index_destroy(&temp);
		return ret;
	}

	kvs_hnsw_v2_index_destroy(index);
	*index = temp;
	memset(&temp, 0, sizeof(temp));
	return KVS_VECTOR_OK;
}
