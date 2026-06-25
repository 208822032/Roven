#include "vector/kvs_vector_id_slot_map.h"
#include "kvs_vector.h"
#include "kvs_memory.h"

#include <string.h>

#define KVS_VECTOR_ID_SLOT_MIN_CAPACITY 16
#define KVS_VECTOR_ID_SLOT_MAX_LOAD_NUM 7
#define KVS_VECTOR_ID_SLOT_MAX_LOAD_DEN 10

//对齐
static size_t kvs_vector_id_slot_next_pow2(size_t value) {
	size_t out = 1;

	while (out < value) {
		out <<= 1;
	}

	return out;
}

//计算hash
static uint64_t kvs_vector_id_slot_hash_u64(uint64_t x) {
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return x;
}

static int kvs_vector_id_slot_map_allocate(
	kvs_vector_id_slot_map_t *map,
	size_t capacity
) {
	kvs_vector_id_slot_entry_t *entries;

	if (!map) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (capacity < KVS_VECTOR_ID_SLOT_MIN_CAPACITY) {
		capacity = KVS_VECTOR_ID_SLOT_MIN_CAPACITY;
	}

	capacity = kvs_vector_id_slot_next_pow2(capacity);

	entries = (kvs_vector_id_slot_entry_t *)kvs_malloc(
		capacity * sizeof(kvs_vector_id_slot_entry_t)
	);
	if (!entries) {
		return KVS_VECTOR_ERR;
	}

	memset(entries, 0, capacity * sizeof(kvs_vector_id_slot_entry_t));

    map->entries = entries;
	map->capacity = capacity;
	map->size = 0;
	map->tombstones = 0;

	return KVS_VECTOR_OK;
}


int kvs_vector_id_slot_map_init(
	kvs_vector_id_slot_map_t *map,
	size_t capacity
){
    if (!map) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(map, 0, sizeof(*map));
	return kvs_vector_id_slot_map_allocate(map, capacity);
}

void kvs_vector_id_slot_map_destroy(
	kvs_vector_id_slot_map_t *map
){
    if (!map) {
		return;
	}

	kvs_free(map->entries);
	memset(map, 0, sizeof(*map));
}

void kvs_vector_id_slot_map_reset(
	kvs_vector_id_slot_map_t *map
){
    if (!map || !map->entries) {
		return;
	}

	memset(map->entries, 0, map->capacity * sizeof(map->entries[0]));
	map->size = 0;
	map->tombstones = 0;
}

static int kvs_vector_id_slot_map_find_slot(
	const kvs_vector_id_slot_map_t *map,
	uint64_t record_id,
	size_t *out_slot,
	size_t *out_tombstone,
	int *out_found
) {
	size_t mask;
	size_t pos;
	size_t first_tombstone = (size_t)-1;
	size_t i;

	if (!map || !map->entries || map->capacity == 0 ||
	    !out_slot || !out_tombstone || !out_found) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	mask = map->capacity - 1;
	pos = (size_t)kvs_vector_id_slot_hash_u64(record_id) & mask;

	for (i = 0; i < map->capacity; i++) {
		kvs_vector_id_slot_entry_t *entry = &map->entries[pos];

		if (entry->state == KVS_VECTOR_ID_SLOT_EMPTY) {
			*out_found = 0;
			*out_slot = (first_tombstone != (size_t)-1) ? first_tombstone : pos;
			*out_tombstone = first_tombstone;
			return KVS_VECTOR_OK;
		}

		if (entry->state == KVS_VECTOR_ID_SLOT_TOMBSTONE) {
			if (first_tombstone == (size_t)-1) {
				first_tombstone = pos;
			}
		} else if (entry->record_id == record_id) {
			*out_found = 1;
			*out_slot = pos;
			*out_tombstone = first_tombstone;
			return KVS_VECTOR_OK;
		}

		pos = (pos + 1) & mask;
	}

	if (first_tombstone != (size_t)-1) {
		*out_found = 0;
		*out_slot = first_tombstone;
		*out_tombstone = first_tombstone;
		return KVS_VECTOR_OK;
	}

	return KVS_VECTOR_ERR;
}

static int kvs_vector_id_slot_map_rehash(
	kvs_vector_id_slot_map_t *map,
	size_t new_capacity
) {
	kvs_vector_id_slot_map_t new_map;
	size_t i;
	int ret;

	if (!map || !map->entries) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_id_slot_map_init(&new_map, new_capacity);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	for (i = 0; i < map->capacity; i++) {
		kvs_vector_id_slot_entry_t *entry = &map->entries[i];

		if (entry->state != KVS_VECTOR_ID_SLOT_USED) {
			continue;
		}

		ret = kvs_vector_id_slot_map_put(
			&new_map,
			entry->record_id,
			entry->slot
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_vector_id_slot_map_destroy(&new_map);
			return ret;
		}
	}

	kvs_free(map->entries);
	*map = new_map;
	return KVS_VECTOR_OK;
}

static int kvs_vector_id_slot_map_maybe_grow(
	kvs_vector_id_slot_map_t *map
) {
	size_t used;

	if (!map || !map->entries) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	used = map->size + map->tombstones;

	if ((used + 1) * KVS_VECTOR_ID_SLOT_MAX_LOAD_DEN <
	    map->capacity * KVS_VECTOR_ID_SLOT_MAX_LOAD_NUM) {
		return KVS_VECTOR_OK;
	}

	return kvs_vector_id_slot_map_rehash(map, map->capacity * 2);
}

int kvs_vector_id_slot_map_put(
	kvs_vector_id_slot_map_t *map,
	uint64_t record_id,
	uint32_t slot
){
    size_t pos;
	size_t tombstone;
	int found;
	int ret;

	if (!map || !map->entries) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_id_slot_map_maybe_grow(map);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	ret = kvs_vector_id_slot_map_find_slot(
		map,
		record_id,
		&pos,
		&tombstone,
		&found
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (found) {
		map->entries[pos].slot = slot;
		return KVS_VECTOR_OK;
	}

	if (map->entries[pos].state == KVS_VECTOR_ID_SLOT_TOMBSTONE) {
		map->tombstones--;
	}

	map->entries[pos].record_id = record_id;
	map->entries[pos].slot = slot;
	map->entries[pos].state = KVS_VECTOR_ID_SLOT_USED;
	map->size++;

	return KVS_VECTOR_OK;
}

int kvs_vector_id_slot_map_get(
	const kvs_vector_id_slot_map_t *map,
	uint64_t record_id,
	uint32_t *out_slot
){
    size_t pos;
	size_t tombstone;
	int found;
	int ret;

	if (!map || !map->entries || !out_slot) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_id_slot_map_find_slot(
		map,
		record_id,
		&pos,
		&tombstone,
		&found
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!found) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	*out_slot = map->entries[pos].slot;
	return KVS_VECTOR_OK;
}

int kvs_vector_id_slot_map_del(
	kvs_vector_id_slot_map_t *map,
	uint64_t record_id
){
    size_t pos;
	size_t tombstone;
	int found;
	int ret;

	if (!map || !map->entries) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	ret = kvs_vector_id_slot_map_find_slot(
		map,
		record_id,
		&pos,
		&tombstone,
		&found
	);
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	if (!found) {
		return KVS_VECTOR_ERR_NOT_FOUND;
	}

	map->entries[pos].state = KVS_VECTOR_ID_SLOT_TOMBSTONE;
	map->size--;
	map->tombstones++;

	return KVS_VECTOR_OK;
}