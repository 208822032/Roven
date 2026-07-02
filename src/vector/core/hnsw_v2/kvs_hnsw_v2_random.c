#include "vector/kvs_vector_hnsw_v2.h"

#define KVS_HNSW_V2_RANDOM_GOLDEN 0x9e3779b97f4a7c15ULL

void kvs_hnsw_v2_random_init(
	kvs_hnsw_v2_random_t *rng,
	uint64_t seed
) {
	if (!rng) {
		return;
	}

	rng->state = seed;
}

static uint64_t kvs_hnsw_v2_random_next_u64(
	kvs_hnsw_v2_random_t *rng
) {
	uint64_t z;

	if (!rng) {
		return 0;
	}

	rng->state += KVS_HNSW_V2_RANDOM_GOLDEN;
	z = rng->state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

uint32_t kvs_hnsw_v2_random_next_u32(
	kvs_hnsw_v2_random_t *rng
) {
	return (uint32_t)(kvs_hnsw_v2_random_next_u64(rng) >> 32);
}

float kvs_hnsw_v2_random_next_unit(
	kvs_hnsw_v2_random_t *rng
) {
	uint32_t value;

	value = kvs_hnsw_v2_random_next_u32(rng) >> 8;
	return (float)value * (1.0f / 16777216.0f);
}

uint8_t kvs_hnsw_v2_random_level(
	kvs_hnsw_v2_random_t *rng,
	size_t M
) {
	float threshold;
	uint8_t level = 0;

	if (!rng) {
		return 0;
	}

	if (M < 2) {
		M = KVS_HNSW_V2_DEFAULT_M;
	}

	threshold = 1.0f / (float)M;
	while (level + 1 < KVS_HNSW_V2_MAX_LEVEL &&
	       kvs_hnsw_v2_random_next_unit(rng) < threshold) {
		level++;
	}

	return level;
}

void kvs_hnsw_v2_index_set_seed(
	kvs_hnsw_v2_index_t *index,
	uint64_t seed
) {
	if (!index) {
		return;
	}

	kvs_hnsw_v2_random_init(&index->rng, seed);
}

