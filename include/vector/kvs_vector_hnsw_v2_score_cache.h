/* Internal helper for core_hnsw_v2; stored under include/vector only to keep headers out of src/. */
#ifndef KVS_VECTOR_HNSW_V2_SCORE_CACHE_H
#define KVS_VECTOR_HNSW_V2_SCORE_CACHE_H

#include "vector/kvs_vector_hnsw_v2.h"

#include <stddef.h>
#include <stdint.h>

#define KVS_HNSW_V2_SCORE_CACHE_EMPTY_KEY UINT64_MAX

typedef struct kvs_hnsw_v2_score_cache_entry_s {
	uint64_t key;
	float score;
} kvs_hnsw_v2_score_cache_entry_t;

struct kvs_hnsw_v2_score_cache_s {
	kvs_hnsw_v2_score_cache_entry_t *entries;
	size_t capacity;
	size_t used;
};

static inline uint64_t kvs_hnsw_v2_score_cache_key(
	uint32_t lhs_slot,
	uint32_t rhs_slot
) {
	uint32_t lo = lhs_slot < rhs_slot ? lhs_slot : rhs_slot;
	uint32_t hi = lhs_slot < rhs_slot ? rhs_slot : lhs_slot;

	return ((uint64_t)lo << 32) | (uint64_t)hi;
}

static inline size_t kvs_hnsw_v2_score_cache_hash(uint64_t key)
{
	key ^= key >> 33;
	key *= 0xff51afd7ed558ccdULL;
	key ^= key >> 33;
	key *= 0xc4ceb9fe1a85ec53ULL;
	key ^= key >> 33;
	return (size_t)key;
}

static inline size_t kvs_hnsw_v2_score_cache_pos(
	const kvs_hnsw_v2_score_cache_t *cache,
	uint64_t key
) {
	size_t mask;
	size_t hash;

	hash = kvs_hnsw_v2_score_cache_hash(key);
	mask = cache->capacity - 1;
	if ((cache->capacity & mask) == 0) {
		return hash & mask;
	}
	return hash % cache->capacity;
}

static inline void kvs_hnsw_v2_score_cache_init(
	kvs_hnsw_v2_score_cache_t *cache,
	kvs_hnsw_v2_score_cache_entry_t *entries,
	size_t capacity
) {
	cache->entries = entries;
	cache->capacity = capacity;
	cache->used = 0;
}

static inline void kvs_hnsw_v2_score_cache_reset(
	kvs_hnsw_v2_score_cache_t *cache
) {
	size_t i;

	if (!cache || !cache->entries) {
		return;
	}
	for (i = 0; i < cache->capacity; i++) {
		cache->entries[i].key = KVS_HNSW_V2_SCORE_CACHE_EMPTY_KEY;
		cache->entries[i].score = 0.0f;
	}
	cache->used = 0;
}

static inline int kvs_hnsw_v2_score_cache_lookup(
	kvs_hnsw_v2_score_cache_t *cache,
	uint32_t lhs_slot,
	uint32_t rhs_slot,
	float *score,
	kvs_hnsw_v2_build_stats_t *stats
) {
	uint64_t key;
	size_t pos;
	kvs_hnsw_v2_score_cache_entry_t *entry;

	if (!cache || !cache->entries || cache->capacity == 0 || !score ||
	    lhs_slot == rhs_slot) {
		return 0;
	}
	if (stats) {
		stats->reverse_prune_pair_cache_lookup_count++;
	}
	key = kvs_hnsw_v2_score_cache_key(lhs_slot, rhs_slot);
	if (key == KVS_HNSW_V2_SCORE_CACHE_EMPTY_KEY) {
		if (stats) {
			stats->reverse_prune_pair_cache_miss_count++;
		}
		return 0;
	}

	pos = kvs_hnsw_v2_score_cache_pos(cache, key);
	entry = &cache->entries[pos];
	if (entry->key == key) {
		*score = entry->score;
		if (stats) {
			stats->reverse_prune_pair_cache_hit_count++;
		}
		return 1;
	}

	if (stats) {
		stats->reverse_prune_pair_cache_miss_count++;
		if (entry->key != KVS_HNSW_V2_SCORE_CACHE_EMPTY_KEY) {
			stats->reverse_prune_pair_cache_collision_count++;
		}
	}
	return 0;
}

static inline int kvs_hnsw_v2_score_cache_insert(
	kvs_hnsw_v2_score_cache_t *cache,
	uint32_t lhs_slot,
	uint32_t rhs_slot,
	float score,
	kvs_hnsw_v2_build_stats_t *stats
) {
	uint64_t key;
	size_t pos;
	kvs_hnsw_v2_score_cache_entry_t *entry;

	if (!cache || !cache->entries || cache->capacity == 0 ||
	    lhs_slot == rhs_slot) {
		return 0;
	}
	key = kvs_hnsw_v2_score_cache_key(lhs_slot, rhs_slot);
	if (key == KVS_HNSW_V2_SCORE_CACHE_EMPTY_KEY) {
		return 0;
	}

	pos = kvs_hnsw_v2_score_cache_pos(cache, key);
	entry = &cache->entries[pos];
	if (entry->key == KVS_HNSW_V2_SCORE_CACHE_EMPTY_KEY) {
		cache->used++;
	} else if (entry->key != key && stats) {
		stats->reverse_prune_pair_cache_collision_count++;
	}
	if (stats) {
		stats->reverse_prune_pair_cache_insert_count++;
	}
	entry->key = key;
	entry->score = score;
	return 1;
}

#endif /* KVS_VECTOR_HNSW_V2_SCORE_CACHE_H */
