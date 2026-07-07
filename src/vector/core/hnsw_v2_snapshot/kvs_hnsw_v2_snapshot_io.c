#include "kvs_hnsw_v2_snapshot_format.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

uint64_t kvs_hnsw_v2_snapshot_checksum_update(
	uint64_t checksum,
	const void *data,
	size_t len
) {
	const unsigned char *bytes = (const unsigned char *)data;
	size_t i;

	if (!bytes && len != 0) {
		return checksum;
	}

	for (i = 0; i < len; i++) {
		checksum ^= (uint64_t)bytes[i];
		checksum *= KVS_HNSW_V2_SNAPSHOT_FNV_PRIME;
	}
	return checksum;
}

static size_t kvs_hnsw_v2_snapshot_align_up_size(
	size_t value,
	size_t alignment
) {
	size_t rem;

	if (alignment == 0) {
		return value;
	}

	rem = value % alignment;
	if (rem == 0) {
		return value;
	}

	return value + (alignment - rem);
}

size_t kvs_hnsw_v2_snapshot_vector_stride(size_t dim) {
	size_t floats_per_align;

	floats_per_align = KVS_HNSW_V2_VECTOR_ALIGNMENT / sizeof(float);
	if (floats_per_align == 0) {
		floats_per_align = 1;
	}

	return kvs_hnsw_v2_snapshot_align_up_size(dim, floats_per_align);
}

static int kvs_hnsw_v2_snapshot_add_u64(
	uint64_t *total,
	uint64_t value
) {
	if (!total || UINT64_MAX - *total < value) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	*total += value;
	return KVS_VECTOR_OK;
}

static int kvs_hnsw_v2_snapshot_mul_u64(
	uint64_t lhs,
	uint64_t rhs,
	uint64_t *out
) {
	if (!out) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (lhs != 0 && rhs > UINT64_MAX / lhs) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	*out = lhs * rhs;
	return KVS_VECTOR_OK;
}

int kvs_hnsw_v2_snapshot_payload_size(
	uint64_t size,
	uint32_t dim,
	uint32_t m,
	uint32_t m0,
	uint64_t *out_size
) {
	uint64_t total = 0;
	uint64_t bytes;

	if (!out_size || dim == 0 || m == 0 || m0 == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (kvs_hnsw_v2_snapshot_mul_u64(size, sizeof(uint64_t), &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_add_u64(&total, bytes) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (kvs_hnsw_v2_snapshot_mul_u64(size, (uint64_t)dim, &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_mul_u64(bytes, sizeof(float), &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_add_u64(&total, bytes) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (kvs_hnsw_v2_snapshot_mul_u64(size, sizeof(uint8_t), &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_add_u64(&total, bytes) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_mul_u64(size, sizeof(uint8_t), &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_add_u64(&total, bytes) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_mul_u64(size, sizeof(uint16_t), &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_add_u64(&total, bytes) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (kvs_hnsw_v2_snapshot_mul_u64(size, (uint64_t)m0, &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_mul_u64(bytes, sizeof(uint32_t), &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_add_u64(&total, bytes) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (kvs_hnsw_v2_snapshot_mul_u64(
		    size,
		    (uint64_t)(KVS_HNSW_V2_MAX_LEVEL - 1),
		    &bytes
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_mul_u64(bytes, sizeof(uint16_t), &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_add_u64(&total, bytes) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (kvs_hnsw_v2_snapshot_mul_u64(
		    size,
		    (uint64_t)(KVS_HNSW_V2_MAX_LEVEL - 1),
		    &bytes
	    ) != KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_mul_u64(bytes, (uint64_t)m, &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_mul_u64(bytes, sizeof(uint32_t), &bytes) !=
	    KVS_VECTOR_OK ||
	    kvs_hnsw_v2_snapshot_add_u64(&total, bytes) != KVS_VECTOR_OK) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_size = total;
	return KVS_VECTOR_OK;
}

int kvs_hnsw_v2_snapshot_write_exact(
	FILE *fp,
	const void *data,
	size_t len
) {
	if (!fp || (!data && len != 0)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (len == 0) {
		return KVS_VECTOR_OK;
	}
	if (fwrite(data, 1, len, fp) != len) {
		return KVS_VECTOR_ERR;
	}
	return KVS_VECTOR_OK;
}

int kvs_hnsw_v2_snapshot_read_exact(
	FILE *fp,
	void *data,
	size_t len
) {
	if (!fp || (!data && len != 0)) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (len == 0) {
		return KVS_VECTOR_OK;
	}
	if (fread(data, 1, len, fp) != len) {
		return KVS_VECTOR_ERR;
	}
	return KVS_VECTOR_OK;
}

int kvs_hnsw_v2_snapshot_file_size(
	const char *path,
	uint64_t *out_size
) {
	struct stat st;

	if (!path || !out_size) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (stat(path, &st) != 0 || st.st_size < 0) {
		return KVS_VECTOR_ERR;
	}
	*out_size = (uint64_t)st.st_size;
	return KVS_VECTOR_OK;
}

void kvs_hnsw_v2_snapshot_set_reason(
	char *reason,
	size_t reason_size,
	const char *message
) {
	if (!reason || reason_size == 0) {
		return;
	}
	if (!message) {
		message = "";
	}
	snprintf(reason, reason_size, "%s", message);
}
