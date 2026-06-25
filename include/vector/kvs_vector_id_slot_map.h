#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * record_id <-> slot map for vector_core.
 *
 * This component belongs to vector_core infrastructure.
 *
 * It only knows:
 *   record_id -> internal slot
 *
 * It must not depend on:
 *   question / answer / scope / TTL / metadata / manager store
 */

typedef enum kvs_vector_id_slot_entry_state_e {
	KVS_VECTOR_ID_SLOT_EMPTY = 0,
	KVS_VECTOR_ID_SLOT_USED = 1,
	KVS_VECTOR_ID_SLOT_TOMBSTONE = 2,
} kvs_vector_id_slot_entry_state_t;

typedef struct kvs_vector_id_slot_entry_s {
	uint64_t record_id;
	uint32_t slot;
	uint8_t state;
} kvs_vector_id_slot_entry_t;

typedef struct kvs_vector_id_slot_map_s {
	kvs_vector_id_slot_entry_t *entries;
	size_t capacity;
	size_t size;
	size_t tombstones;
} kvs_vector_id_slot_map_t;

int kvs_vector_id_slot_map_init(
	kvs_vector_id_slot_map_t *map,
	size_t capacity
);

void kvs_vector_id_slot_map_destroy(
	kvs_vector_id_slot_map_t *map
);

void kvs_vector_id_slot_map_reset(
	kvs_vector_id_slot_map_t *map
);

int kvs_vector_id_slot_map_put(
	kvs_vector_id_slot_map_t *map,
	uint64_t record_id,
	uint32_t slot
);

int kvs_vector_id_slot_map_get(
	const kvs_vector_id_slot_map_t *map,
	uint64_t record_id,
	uint32_t *out_slot
);

int kvs_vector_id_slot_map_del(
	kvs_vector_id_slot_map_t *map,
	uint64_t record_id
);