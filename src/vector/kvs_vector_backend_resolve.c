#include "vector/kvs_vector_backend_resolve.h"

#include "kvs_log.h"

#include <strings.h>

#define KVS_VECTOR_LOG_MODULE "vector"

static int kvs_vector_backend_name_is(
	const char *value,
	const char *expected
) {
	return value && expected && strcasecmp(value, expected) == 0;
}

int kvs_vector_backend_is_removed_compat_name(const char *value) {
	if (!value || value[0] == '\0') {
		return 0;
	}

	return kvs_vector_backend_name_is(value, "native_hnsw") ||
		kvs_vector_backend_name_is(value, "core_hnsw") ||
		kvs_vector_backend_name_is(value, "hnsw") ||
		kvs_vector_backend_name_is(value, "hnswlib") ||
		kvs_vector_backend_name_is(value, "hnswlib_hnsw") ||
		kvs_vector_backend_name_is(value, "sq8_flat") ||
		kvs_vector_backend_name_is(value, "faiss") ||
		kvs_vector_backend_name_is(value, "faiss_hnsw") ||
		kvs_vector_backend_name_is(value, "faiss_flat");
}

kvs_vector_index_type_t kvs_vector_backend_resolve_from_any_backend(
	const char *value
) {
	if (!value || value[0] == '\0') {
		return KVS_VECTOR_INDEX_CORE_HNSW_V2;
	}

	if (kvs_vector_backend_name_is(value, "flat") ||
	    kvs_vector_backend_name_is(value, "core_flat")) {
		return KVS_VECTOR_INDEX_CORE_FLAT;
	}

	if (kvs_vector_backend_name_is(value, "core_hnsw_v2")) {
		return KVS_VECTOR_INDEX_CORE_HNSW_V2;
	}

	if (kvs_vector_backend_name_is(value, "native_hnsw")) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"native_hnsw has been removed; falling back to core_flat"
		);
		return KVS_VECTOR_INDEX_CORE_FLAT;
	}

	if (kvs_vector_backend_name_is(value, "core_hnsw")) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"core_hnsw has been removed; falling back to core_flat"
		);
		return KVS_VECTOR_INDEX_CORE_FLAT;
	}

	if (kvs_vector_backend_name_is(value, "hnsw") ||
	    kvs_vector_backend_name_is(value, "hnswlib") ||
	    kvs_vector_backend_name_is(value, "hnswlib_hnsw")) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"hnswlib external backend has been removed; falling back to core_flat"
		);
		return KVS_VECTOR_INDEX_CORE_FLAT;
	}

	if (kvs_vector_backend_name_is(value, "sq8_flat")) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"sq8_flat compatibility backend has been removed; falling back to core_flat"
		);
		return KVS_VECTOR_INDEX_CORE_FLAT;
	}

	if (kvs_vector_backend_name_is(value, "faiss") ||
	    kvs_vector_backend_name_is(value, "faiss_hnsw") ||
	    kvs_vector_backend_name_is(value, "faiss_flat")) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"faiss external backend has been removed; falling back to core_flat"
		);
		return KVS_VECTOR_INDEX_CORE_FLAT;
	}

	KVS_LOG_ERROR(
		KVS_VECTOR_LOG_MODULE,
		"unknown vector_index_type=%s; falling back to core_hnsw_v2",
		value
	);
	return KVS_VECTOR_INDEX_CORE_HNSW_V2;
}

kvs_vector_index_type_t kvs_vector_backend_resolve_from_config(
	const char *value
) {
	return kvs_vector_backend_resolve_from_any_backend(value);
}

const char *kvs_vector_backend_type_name(kvs_vector_index_type_t type) {
	switch (type) {
	case KVS_VECTOR_INDEX_TYPE_FLAT:
		return "flat_removed";
	case KVS_VECTOR_INDEX_TYPE_HNSW:
		return "hnsw_removed";
	case KVS_VECTOR_INDEX_TYPE_SQ8_FLAT:
		return "sq8_flat_removed";
	case KVS_VECTOR_INDEX_FAISS_HNSW:
		return "faiss_hnsw_removed";
	case KVS_VECTOR_INDEX_FAISS_FLAT:
		return "faiss_flat_removed";
	case KVS_VECTOR_INDEX_HNSWLIB_HNSW:
		return "hnswlib_hnsw_removed";
	case KVS_VECTOR_INDEX_CORE_FLAT:
		return "core_flat";
	case KVS_VECTOR_INDEX_CORE_HNSW:
		return "core_hnsw_removed";
	case KVS_VECTOR_INDEX_CORE_HNSW_V2:
		return "core_hnsw_v2";
	default:
		return "unknown";
	}
}
