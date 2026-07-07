#pragma once

#include "kvs_vector.h"

/*
 * Manager-side backend name/type compatibility helper.
 *
 * This file only resolves backend config names and display names. It must not
 * access vector manager state, record stores, response assembly, RDB/AOF, or
 * vector_core internals.
 */

kvs_vector_index_type_t kvs_vector_backend_resolve_from_config(
	const char *value
);

kvs_vector_index_type_t kvs_vector_backend_resolve_from_any_backend(
	const char *value
);

const char *kvs_vector_backend_type_name(kvs_vector_index_type_t type);

int kvs_vector_backend_is_removed_compat_name(const char *value);
