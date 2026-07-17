#define KVS_VECTOR_INTERNAL
#include "vector/kvs_vector_index_snapshot.h"

#include "kvs_config.h"
#include "kvs_vector.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KVS_VECTOR_INDEX_MANIFEST_FIELD_COUNT 19u
#define KVS_VECTOR_INDEX_MANIFEST_LINE_MAX 512
#define KVS_VECTOR_INDEX_SNAPSHOT_FNV_OFFSET 1469598103934665603ull
#define KVS_VECTOR_INDEX_SNAPSHOT_FNV_PRIME 1099511628211ull

typedef enum kvs_vector_index_manifest_seen_e {
	KVS_VECTOR_INDEX_MANIFEST_SEEN_MAGIC = 1u << 0,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_MANIFEST_VERSION = 1u << 1,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_SNAPSHOT_FORMAT_VERSION = 1u << 2,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_BACKEND = 1u << 3,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_DIM = 1u << 4,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_METRIC = 1u << 5,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_HNSW_M = 1u << 6,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_HNSW_M0 = 1u << 7,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_HNSW_EF_CONSTRUCTION = 1u << 8,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_HNSW_EF_SEARCH = 1u << 9,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_ACTIVE_COUNT = 1u << 10,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_DELETED_COUNT = 1u << 11,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_RECORD_GENERATION = 1u << 12,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_RECORD_ID_CHECKSUM = 1u << 13,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_VECTOR_CHECKSUM = 1u << 14,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_SNAPSHOT_SIZE = 1u << 15,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_SNAPSHOT_CHECKSUM = 1u << 16,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_CREATED_AT_MS = 1u << 17,
	KVS_VECTOR_INDEX_MANIFEST_SEEN_MODEL_CONTENT_IDENTITY = 1u << 18,
} kvs_vector_index_manifest_seen_t;

static const uint32_t KVS_VECTOR_INDEX_MANIFEST_SEEN_ALL =
	(1u << KVS_VECTOR_INDEX_MANIFEST_FIELD_COUNT) - 1u;

static const char *kvs_vector_index_snapshot_backend_name(uint32_t backend) {
	switch ((kvs_vector_index_type_t)backend) {
	case KVS_VECTOR_INDEX_CORE_HNSW_V2:
		return "core_hnsw_v2";
	case KVS_VECTOR_INDEX_CORE_FLAT:
		return "core_flat";
	case KVS_VECTOR_INDEX_TYPE_FLAT:
		return "flat";
	case KVS_VECTOR_INDEX_CORE_HNSW:
		return "core_hnsw";
	default:
		return NULL;
	}
}

static const char *kvs_vector_index_snapshot_metric_name(uint32_t metric) {
	switch ((kvs_vector_metric_type_t)metric) {
	case KVS_VECTOR_METRIC_IP:
		return "ip";
	case KVS_VECTOR_METRIC_L2:
		return "l2";
	case KVS_VECTOR_METRIC_COSINE:
		return "cosine";
	default:
		return NULL;
	}
}

static int kvs_vector_index_snapshot_parse_u64(
	const char *value,
	uint64_t *out
) {
	char *end = NULL;
	unsigned long long parsed;

	if (!value || !out || value[0] == '\0') {
		return -1;
	}
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno != 0 || !end || *end != '\0') {
		return -1;
	}
	*out = (uint64_t)parsed;
	return 0;
}

static int kvs_vector_index_snapshot_parse_u32(
	const char *value,
	uint32_t *out
) {
	uint64_t parsed = 0;

	if (kvs_vector_index_snapshot_parse_u64(value, &parsed) != 0 ||
	    parsed > UINT32_MAX) {
		return -1;
	}
	*out = (uint32_t)parsed;
	return 0;
}

static int kvs_vector_index_snapshot_parse_backend(
	const char *value,
	uint32_t *out
) {
	if (!value || !out) {
		return -1;
	}
	if (strcmp(value, "core_hnsw_v2") == 0) {
		*out = (uint32_t)KVS_VECTOR_INDEX_CORE_HNSW_V2;
		return 0;
	}
	if (strcmp(value, "core_flat") == 0) {
		*out = (uint32_t)KVS_VECTOR_INDEX_CORE_FLAT;
		return 0;
	}
	if (strcmp(value, "flat") == 0) {
		*out = (uint32_t)KVS_VECTOR_INDEX_TYPE_FLAT;
		return 0;
	}
	if (strcmp(value, "core_hnsw") == 0) {
		*out = (uint32_t)KVS_VECTOR_INDEX_CORE_HNSW;
		return 0;
	}
	return kvs_vector_index_snapshot_parse_u32(value, out);
}

static int kvs_vector_index_snapshot_parse_metric(
	const char *value,
	uint32_t *out
) {
	if (!value || !out) {
		return -1;
	}
	if (strcmp(value, "ip") == 0) {
		*out = (uint32_t)KVS_VECTOR_METRIC_IP;
		return 0;
	}
	if (strcmp(value, "l2") == 0) {
		*out = (uint32_t)KVS_VECTOR_METRIC_L2;
		return 0;
	}
	if (strcmp(value, "cosine") == 0) {
		*out = (uint32_t)KVS_VECTOR_METRIC_COSINE;
		return 0;
	}
	return kvs_vector_index_snapshot_parse_u32(value, out);
}

static void kvs_vector_index_snapshot_set_reason(
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

int kvs_vector_index_snapshot_manifest_init(
	kvs_vector_index_snapshot_manifest_t *manifest
) {
	if (!manifest) {
		return -1;
	}
	memset(manifest, 0, sizeof(*manifest));
	snprintf(
		manifest->magic,
		sizeof(manifest->magic),
		"%s",
		KVS_VECTOR_INDEX_MANIFEST_MAGIC
	);
	manifest->manifest_version = KVS_VECTOR_INDEX_MANIFEST_VERSION;
	manifest->snapshot_format_version =
		KVS_VECTOR_INDEX_SNAPSHOT_FORMAT_VERSION;
	return 0;
}

static int kvs_vector_index_snapshot_manifest_write_stream(
	FILE *fp,
	const kvs_vector_index_snapshot_manifest_t *manifest
) {
	const char *backend_name;
	const char *metric_name;

	if (!fp || !manifest) {
		return -1;
	}
	backend_name = kvs_vector_index_snapshot_backend_name(manifest->backend);
	metric_name = kvs_vector_index_snapshot_metric_name(manifest->metric);

	if (fprintf(fp, "magic=%s\n", manifest->magic) < 0 ||
	    fprintf(fp, "manifest_version=%" PRIu32 "\n",
	            manifest->manifest_version) < 0 ||
	    fprintf(fp, "snapshot_format_version=%" PRIu32 "\n",
	            manifest->snapshot_format_version) < 0) {
		return -1;
	}

	if (backend_name) {
		if (fprintf(fp, "backend=%s\n", backend_name) < 0) {
			return -1;
		}
	} else if (fprintf(fp, "backend=%" PRIu32 "\n", manifest->backend) < 0) {
		return -1;
	}

	if (fprintf(fp, "dim=%" PRIu32 "\n", manifest->dim) < 0) {
		return -1;
	}

	if (metric_name) {
		if (fprintf(fp, "metric=%s\n", metric_name) < 0) {
			return -1;
		}
	} else if (fprintf(fp, "metric=%" PRIu32 "\n", manifest->metric) < 0) {
		return -1;
	}

	if (fprintf(fp, "hnsw_m=%" PRIu32 "\n", manifest->hnsw_m) < 0 ||
	    fprintf(fp, "hnsw_m0=%" PRIu32 "\n", manifest->hnsw_m0) < 0 ||
	    fprintf(fp, "hnsw_ef_construction=%" PRIu32 "\n",
	            manifest->hnsw_ef_construction) < 0 ||
	    fprintf(fp, "hnsw_ef_search=%" PRIu32 "\n",
	            manifest->hnsw_ef_search) < 0 ||
	    fprintf(fp, "active_count=%" PRIu64 "\n",
	            manifest->active_count) < 0 ||
	    fprintf(fp, "deleted_count=%" PRIu64 "\n",
	            manifest->deleted_count) < 0 ||
	    fprintf(fp, "record_generation=%" PRIu64 "\n",
	            manifest->record_generation) < 0 ||
	    fprintf(fp, "record_id_checksum=%" PRIu64 "\n",
	            manifest->record_id_checksum) < 0 ||
	    fprintf(fp, "vector_checksum=%" PRIu64 "\n",
	            manifest->vector_checksum) < 0 ||
	    fprintf(fp, "snapshot_size=%" PRIu64 "\n",
	            manifest->snapshot_size) < 0 ||
	    fprintf(fp, "snapshot_checksum=%" PRIu64 "\n",
	            manifest->snapshot_checksum) < 0 ||
	    fprintf(fp, "created_at_ms=%" PRIu64 "\n",
	            manifest->created_at_ms) < 0 ||
	    fprintf(fp, "model_content_identity=%" PRIu64 "\n",
	            manifest->model_content_identity) < 0) {
		return -1;
	}
	return 0;
}

int kvs_vector_index_snapshot_manifest_write_file(
	const char *path,
	const kvs_vector_index_snapshot_manifest_t *manifest
) {
	char tmp_path[4096];
	FILE *fp;
	int ret = -1;

	if (!path || path[0] == '\0' || !manifest) {
		return -1;
	}
	if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >=
	    (int)sizeof(tmp_path)) {
		return -1;
	}

	fp = fopen(tmp_path, "wb");
	if (!fp) {
		return -1;
	}
	if (kvs_vector_index_snapshot_manifest_write_stream(fp, manifest) == 0 &&
	    fflush(fp) == 0 &&
	    ferror(fp) == 0) {
		ret = 0;
	}
	if (fclose(fp) != 0) {
		ret = -1;
	}
	if (ret == 0 && rename(tmp_path, path) != 0) {
		ret = -1;
	}
	if (ret != 0) {
		(void)unlink(tmp_path);
	}
	return ret;
}

static int kvs_vector_index_snapshot_manifest_parse_line(
	char *line,
	kvs_vector_index_snapshot_manifest_t *manifest,
	uint32_t *seen
) {
	char *key;
	char *value;

	key = line;
	value = strchr(line, '=');
	if (!value || value == key) {
		return -1;
	}
	*value++ = '\0';
	if (value[0] == '\0') {
		return -1;
	}

	if (strcmp(key, "magic") == 0) {
		if (strlen(value) >= sizeof(manifest->magic)) {
			return -1;
		}
		memset(manifest->magic, 0, sizeof(manifest->magic));
		snprintf(manifest->magic, sizeof(manifest->magic), "%s", value);
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_MAGIC;
	} else if (strcmp(key, "manifest_version") == 0) {
		if (kvs_vector_index_snapshot_parse_u32(
			    value,
			    &manifest->manifest_version) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_MANIFEST_VERSION;
	} else if (strcmp(key, "snapshot_format_version") == 0) {
		if (kvs_vector_index_snapshot_parse_u32(
			    value,
			    &manifest->snapshot_format_version) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_SNAPSHOT_FORMAT_VERSION;
	} else if (strcmp(key, "backend") == 0) {
		if (kvs_vector_index_snapshot_parse_backend(value, &manifest->backend) !=
		    0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_BACKEND;
	} else if (strcmp(key, "dim") == 0) {
		if (kvs_vector_index_snapshot_parse_u32(value, &manifest->dim) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_DIM;
	} else if (strcmp(key, "metric") == 0) {
		if (kvs_vector_index_snapshot_parse_metric(value, &manifest->metric) !=
		    0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_METRIC;
	} else if (strcmp(key, "hnsw_m") == 0) {
		if (kvs_vector_index_snapshot_parse_u32(value, &manifest->hnsw_m) !=
		    0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_HNSW_M;
	} else if (strcmp(key, "hnsw_m0") == 0) {
		if (kvs_vector_index_snapshot_parse_u32(value, &manifest->hnsw_m0) !=
		    0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_HNSW_M0;
	} else if (strcmp(key, "hnsw_ef_construction") == 0) {
		if (kvs_vector_index_snapshot_parse_u32(
			    value,
			    &manifest->hnsw_ef_construction) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_HNSW_EF_CONSTRUCTION;
	} else if (strcmp(key, "hnsw_ef_search") == 0) {
		if (kvs_vector_index_snapshot_parse_u32(
			    value,
			    &manifest->hnsw_ef_search) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_HNSW_EF_SEARCH;
	} else if (strcmp(key, "active_count") == 0) {
		if (kvs_vector_index_snapshot_parse_u64(
			    value,
			    &manifest->active_count) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_ACTIVE_COUNT;
	} else if (strcmp(key, "deleted_count") == 0) {
		if (kvs_vector_index_snapshot_parse_u64(
			    value,
			    &manifest->deleted_count) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_DELETED_COUNT;
	} else if (strcmp(key, "record_generation") == 0) {
		if (kvs_vector_index_snapshot_parse_u64(
			    value,
			    &manifest->record_generation) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_RECORD_GENERATION;
	} else if (strcmp(key, "record_id_checksum") == 0) {
		if (kvs_vector_index_snapshot_parse_u64(
			    value,
			    &manifest->record_id_checksum) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_RECORD_ID_CHECKSUM;
	} else if (strcmp(key, "vector_checksum") == 0) {
		if (kvs_vector_index_snapshot_parse_u64(
			    value,
			    &manifest->vector_checksum) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_VECTOR_CHECKSUM;
	} else if (strcmp(key, "snapshot_size") == 0) {
		if (kvs_vector_index_snapshot_parse_u64(
			    value,
			    &manifest->snapshot_size) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_SNAPSHOT_SIZE;
	} else if (strcmp(key, "snapshot_checksum") == 0) {
		if (kvs_vector_index_snapshot_parse_u64(
			    value,
			    &manifest->snapshot_checksum) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_SNAPSHOT_CHECKSUM;
	} else if (strcmp(key, "created_at_ms") == 0) {
		if (kvs_vector_index_snapshot_parse_u64(
			    value,
			    &manifest->created_at_ms) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_CREATED_AT_MS;
	} else if (strcmp(key, "model_content_identity") == 0) {
		if (kvs_vector_index_snapshot_parse_u64(
			    value,
			    &manifest->model_content_identity) != 0) {
			return -1;
		}
		*seen |= KVS_VECTOR_INDEX_MANIFEST_SEEN_MODEL_CONTENT_IDENTITY;
	}

	return 0;
}

int kvs_vector_index_snapshot_manifest_read_file(
	const char *path,
	kvs_vector_index_snapshot_manifest_t *manifest
) {
	FILE *fp;
	char line[KVS_VECTOR_INDEX_MANIFEST_LINE_MAX];
	uint32_t seen = 0;

	if (!path || path[0] == '\0' || !manifest) {
		return -1;
	}
	fp = fopen(path, "rb");
	if (!fp) {
		return -1;
	}
	if (kvs_vector_index_snapshot_manifest_init(manifest) != 0) {
		fclose(fp);
		return -1;
	}
	while (fgets(line, sizeof(line), fp)) {
		size_t len = strlen(line);

		if (len > 0 && line[len - 1] == '\n') {
			line[--len] = '\0';
		}
		if (len > 0 && line[len - 1] == '\r') {
			line[--len] = '\0';
		}
		if (len == 0) {
			continue;
		}
		if (kvs_vector_index_snapshot_manifest_parse_line(
			    line,
			    manifest,
			    &seen) != 0) {
			fclose(fp);
			return -1;
		}
	}
	if (ferror(fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return seen == KVS_VECTOR_INDEX_MANIFEST_SEEN_ALL ? 0 : -1;
}

int kvs_vector_index_snapshot_manifest_validate(
	const kvs_vector_index_snapshot_manifest_t *actual,
	const kvs_vector_index_snapshot_manifest_t *expected,
	char *reason,
	size_t reason_size
) {
	if (!actual || !expected) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"invalid manifest argument"
		);
		return -1;
	}

	if (strcmp(actual->magic, expected->magic) != 0) {
		kvs_vector_index_snapshot_set_reason(reason, reason_size, "magic mismatch");
		return -1;
	}
	if (actual->manifest_version != expected->manifest_version) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"manifest_version mismatch"
		);
		return -1;
	}
	if (actual->snapshot_format_version != expected->snapshot_format_version) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"snapshot_format_version mismatch"
		);
		return -1;
	}
	if (actual->backend != expected->backend) {
		kvs_vector_index_snapshot_set_reason(reason, reason_size, "backend mismatch");
		return -1;
	}
	if (actual->dim != expected->dim) {
		kvs_vector_index_snapshot_set_reason(reason, reason_size, "dim mismatch");
		return -1;
	}
	if (actual->metric != expected->metric) {
		kvs_vector_index_snapshot_set_reason(reason, reason_size, "metric mismatch");
		return -1;
	}
	if (actual->model_content_identity != expected->model_content_identity) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"model_content_identity mismatch"
		);
		return -1;
	}
	if (actual->hnsw_m != expected->hnsw_m ||
	    actual->hnsw_m0 != expected->hnsw_m0 ||
	    actual->hnsw_ef_construction != expected->hnsw_ef_construction ||
	    actual->hnsw_ef_search != expected->hnsw_ef_search) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"HNSW parameter mismatch"
		);
		return -1;
	}
	if (actual->active_count != expected->active_count) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"active_count mismatch"
		);
		return -1;
	}
	if (actual->deleted_count != expected->deleted_count) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"deleted_count mismatch"
		);
		return -1;
	}
	if (actual->record_generation != expected->record_generation) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"record_generation mismatch"
		);
		return -1;
	}
	if (actual->record_id_checksum != expected->record_id_checksum) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"record_id_checksum mismatch"
		);
		return -1;
	}
	if (actual->vector_checksum != expected->vector_checksum) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"vector_checksum mismatch"
		);
		return -1;
	}
	if (actual->snapshot_size != expected->snapshot_size) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"snapshot_size mismatch"
		);
		return -1;
	}
	if (actual->snapshot_checksum != expected->snapshot_checksum) {
		kvs_vector_index_snapshot_set_reason(
			reason,
			reason_size,
			"snapshot_checksum mismatch"
		);
		return -1;
	}

	kvs_vector_index_snapshot_set_reason(reason, reason_size, "ok");
	return 0;
}

uint64_t kvs_vector_index_snapshot_checksum_file(const char *path) {
	FILE *fp;
	unsigned char buf[4096];
	uint64_t hash = KVS_VECTOR_INDEX_SNAPSHOT_FNV_OFFSET;

	if (!path || path[0] == '\0') {
		return 0;
	}
	fp = fopen(path, "rb");
	if (!fp) {
		return 0;
	}
	for (;;) {
		size_t i;
		size_t n = fread(buf, 1, sizeof(buf), fp);
		for (i = 0; i < n; ++i) {
			hash ^= (uint64_t)buf[i];
			hash *= KVS_VECTOR_INDEX_SNAPSHOT_FNV_PRIME;
		}
		if (n < sizeof(buf)) {
			if (ferror(fp)) {
				fclose(fp);
				return 0;
			}
			break;
		}
	}
	fclose(fp);
	return hash;
}

static int kvs_vector_index_snapshot_default_path(
	char *buf,
	size_t size,
	const char *path
) {
	int written;

	if (!buf || size == 0 || !path) {
		return -1;
	}
	written = snprintf(buf, size, "%s", path);
	if (written < 0 || (size_t)written >= size) {
		if (size > 0) {
			buf[0] = '\0';
		}
		return -1;
	}
	return 0;
}

int kvs_vector_index_snapshot_default_manifest_path(char *buf, size_t size) {
	return kvs_vector_index_snapshot_default_path(
		buf,
		size,
		g_cfg.vector_snapshot_manifest_path
	);
}

int kvs_vector_index_snapshot_default_snapshot_path(char *buf, size_t size) {
	return kvs_vector_index_snapshot_default_path(
		buf,
		size,
		g_cfg.vector_snapshot_index_path
	);
}
