#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"

#include <math.h>
#include <stdint.h>
#include <time.h>

static pthread_mutex_t g_embedding_provider_encode_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct kvs_vector_embedding_thread_context_s {
	EmbeddingHandle *runtime_handle;
	kvs_vector_embedding_provider_type_t provider_type;
	size_t dimension;
	int initialized;
	int fallback_to_global;
} kvs_vector_embedding_thread_context_t;

static __thread kvs_vector_embedding_thread_context_t *g_embedding_thread_ctx;
static kvs_vector_embedding_runtime_stats_t g_embedding_runtime_stats;

typedef struct kvs_embedding_cache_key_s {
	uint64_t text_hash;
	uint32_t model_id;
	uint16_t dim;
} kvs_embedding_cache_key_t;

typedef struct kvs_embedding_cache_entry_s {
	kvs_embedding_cache_key_t key;
	float *embedding;
	uint16_t dim;
	uint64_t create_time_ms;
	uint64_t last_access_ms;
	uint64_t hit_count;
	uint64_t source_total_us;
	struct kvs_embedding_cache_entry_s *bucket_next;
	struct kvs_embedding_cache_entry_s *lru_prev;
	struct kvs_embedding_cache_entry_s *lru_next;
} kvs_embedding_cache_entry_t;

typedef struct kvs_embedding_cache_s {
	int enabled;
	size_t capacity;
	uint64_t ttl_ms;
	size_t count;
	size_t bucket_count;
	size_t memory_bytes;
	kvs_embedding_cache_entry_t **buckets;
	kvs_embedding_cache_entry_t *lru_head;
	kvs_embedding_cache_entry_t *lru_tail;
	pthread_mutex_t mutex;
	int mutex_initialized;
} kvs_embedding_cache_t;

static kvs_embedding_cache_t g_embedding_cache;

static uint64_t embedding_stats_add(uint64_t *field, uint64_t value) {
	return __atomic_add_fetch(field, value, __ATOMIC_RELAXED);
}

static uint64_t embedding_stats_sub(uint64_t *field, uint64_t value) {
	return __atomic_sub_fetch(field, value, __ATOMIC_RELAXED);
}

static uint64_t embedding_stats_load(const uint64_t *field) {
	return __atomic_load_n(field, __ATOMIC_RELAXED);
}

static void embedding_stats_store(uint64_t *field, uint64_t value) {
	__atomic_store_n(field, value, __ATOMIC_RELAXED);
}

static void embedding_stats_update_max(uint64_t *field, uint64_t value) {
	uint64_t old_value = __atomic_load_n(field, __ATOMIC_RELAXED);

	while (value > old_value) {
		if (__atomic_compare_exchange_n(field,
		                                &old_value,
		                                value,
		                                0,
		                                __ATOMIC_RELAXED,
		                                __ATOMIC_RELAXED)) {
			break;
		}
	}
}

static uint64_t embedding_now_us(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static uint64_t embedding_now_ms(void) {
	return embedding_now_us() / 1000ull;
}

static uint64_t embedding_hash_bytes64(const char *data, size_t len) {
	const unsigned char *p = (const unsigned char *)data;
	uint64_t hash = 1469598103934665603ull;
	size_t i;

	for (i = 0; i < len; ++i) {
		hash ^= (uint64_t)p[i];
		hash *= 1099511628211ull;
	}

	return hash;
}

static uint32_t embedding_hash_bytes32(const char *data, size_t len) {
	const unsigned char *p = (const unsigned char *)data;
	uint32_t hash = 2166136261u;
	size_t i;

	for (i = 0; i < len; ++i) {
		hash ^= (uint32_t)p[i];
		hash *= 16777619u;
	}

	return hash;
}

static size_t embedding_next_pow2_size(size_t value) {
	size_t n = 1;

	while (n < value && n <= ((size_t)-1 / 2)) {
		n <<= 1;
	}

	return n;
}

static size_t embedding_cache_entry_memory_bytes(const kvs_embedding_cache_entry_t *entry) {
	size_t total;

	if (!entry) {
		return 0;
	}

	total = sizeof(*entry);
	if (entry->embedding && entry->dim > 0) {
		total += (size_t)entry->dim * sizeof(float);
	}

	return total;
}

static int embedding_text_normalize_for_cache(
	const char *text,
	size_t text_len,
	char **out_text,
	size_t *out_len
) {
	char *normalized;
	size_t i;
	size_t out_pos = 0;
	int last_was_space = 1;

	if (!text || text_len == 0 || !out_text || !out_len) {
		return 0;
	}

	*out_text = NULL;
	*out_len = 0;

	normalized = (char *)malloc(text_len + 1);
	if (!normalized) {
		return 0;
	}

	for (i = 0; i < text_len; ++i) {
		unsigned char ch = (unsigned char)text[i];

		if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
		    ch == '\f' || ch == '\v') {
			if (!last_was_space && out_pos > 0) {
				normalized[out_pos++] = ' ';
			}
			last_was_space = 1;
			continue;
		}

		if (ch >= 'A' && ch <= 'Z') {
			ch = (unsigned char)(ch - 'A' + 'a');
		}

		normalized[out_pos++] = (char)ch;
		last_was_space = 0;
	}

	if (out_pos > 0 && normalized[out_pos - 1] == ' ') {
		out_pos--;
	}

	normalized[out_pos] = '\0';
	*out_text = normalized;
	*out_len = out_pos;
	return 1;
}

static int embedding_cache_key_build(
	const kvs_vector_embedding_provider_t *provider,
	const char *text,
	size_t text_len,
	int dim,
	kvs_embedding_cache_key_t *out_key
) {
	char *normalized = NULL;
	size_t normalized_len = 0;

	if (!provider || !text || text_len == 0 || dim <= 0 || !out_key) {
		return 0;
	}

	if (!embedding_text_normalize_for_cache(
		text,
		text_len,
		&normalized,
		&normalized_len
	)) {
		return 0;
	}

	out_key->text_hash = embedding_hash_bytes64(normalized, normalized_len);
	out_key->model_id = provider->model_id;
	out_key->dim = (uint16_t)dim;
	free(normalized);
	return 1;
}

static int embedding_cache_key_equal(
	const kvs_embedding_cache_key_t *lhs,
	const kvs_embedding_cache_key_t *rhs
) {
	return lhs && rhs &&
		lhs->text_hash == rhs->text_hash &&
		lhs->model_id == rhs->model_id &&
		lhs->dim == rhs->dim;
}

static void embedding_cache_lru_link_head(
	kvs_embedding_cache_t *cache,
	kvs_embedding_cache_entry_t *entry
) {
	if (!cache || !entry) {
		return;
	}

	entry->lru_prev = NULL;
	entry->lru_next = cache->lru_head;
	if (cache->lru_head) {
		cache->lru_head->lru_prev = entry;
	}
	cache->lru_head = entry;
	if (!cache->lru_tail) {
		cache->lru_tail = entry;
	}
}

static void embedding_cache_lru_unlink(
	kvs_embedding_cache_t *cache,
	kvs_embedding_cache_entry_t *entry
) {
	if (!cache || !entry) {
		return;
	}

	if (entry->lru_prev) {
		entry->lru_prev->lru_next = entry->lru_next;
	} else {
		cache->lru_head = entry->lru_next;
	}

	if (entry->lru_next) {
		entry->lru_next->lru_prev = entry->lru_prev;
	} else {
		cache->lru_tail = entry->lru_prev;
	}

	entry->lru_prev = NULL;
	entry->lru_next = NULL;
}

static void embedding_cache_lru_touch(
	kvs_embedding_cache_t *cache,
	kvs_embedding_cache_entry_t *entry
) {
	if (!cache || !entry || cache->lru_head == entry) {
		return;
	}

	embedding_cache_lru_unlink(cache, entry);
	embedding_cache_lru_link_head(cache, entry);
}

static void embedding_cache_entry_destroy(kvs_embedding_cache_entry_t *entry) {
	if (!entry) {
		return;
	}

	if (entry->embedding) {
		kvs_free(entry->embedding);
	}
	free(entry);
}

static void embedding_cache_remove_entry(
	kvs_embedding_cache_t *cache,
	kvs_embedding_cache_entry_t *entry,
	size_t bucket_index,
	int record_evict
) {
	kvs_embedding_cache_entry_t **pp;
	size_t bytes;

	if (!cache || !entry || !cache->buckets || bucket_index >= cache->bucket_count) {
		return;
	}

	pp = &cache->buckets[bucket_index];
	while (*pp) {
		if (*pp == entry) {
			*pp = entry->bucket_next;
			break;
		}
		pp = &(*pp)->bucket_next;
	}

	embedding_cache_lru_unlink(cache, entry);
	if (cache->count > 0) {
		cache->count--;
	}

	bytes = embedding_cache_entry_memory_bytes(entry);
	if (cache->memory_bytes >= bytes) {
		cache->memory_bytes -= bytes;
	} else {
		cache->memory_bytes = 0;
	}
	embedding_stats_store(
		&g_embedding_runtime_stats.embedding_cache_memory_bytes,
		cache->memory_bytes
	);
	if (record_evict) {
		embedding_stats_add(&g_embedding_runtime_stats.embedding_cache_evict_count, 1);
	}
	embedding_cache_entry_destroy(entry);
}

static void embedding_cache_destroy_locked(kvs_embedding_cache_t *cache) {
	size_t i;

	if (!cache) {
		return;
	}

	if (cache->buckets) {
		for (i = 0; i < cache->bucket_count; ++i) {
			kvs_embedding_cache_entry_t *entry = cache->buckets[i];
			while (entry) {
				kvs_embedding_cache_entry_t *next = entry->bucket_next;
				embedding_cache_entry_destroy(entry);
				entry = next;
			}
		}
		free(cache->buckets);
	}

	cache->enabled = 0;
	cache->capacity = 0;
	cache->ttl_ms = 0;
	cache->count = 0;
	cache->bucket_count = 0;
	cache->memory_bytes = 0;
	cache->buckets = NULL;
	cache->lru_head = NULL;
	cache->lru_tail = NULL;
	embedding_stats_store(&g_embedding_runtime_stats.embedding_cache_memory_bytes, 0);
}

static void embedding_cache_destroy(kvs_embedding_cache_t *cache) {
	if (!cache) {
		return;
	}

	if (cache->mutex_initialized) {
		pthread_mutex_lock(&cache->mutex);
		embedding_cache_destroy_locked(cache);
		pthread_mutex_unlock(&cache->mutex);
	} else {
		embedding_cache_destroy_locked(cache);
	}
}

static int embedding_cache_init(
	kvs_embedding_cache_t *cache,
	size_t capacity,
	uint64_t ttl_ms
) {
	size_t bucket_count;

	if (!cache || capacity == 0 || ttl_ms == 0) {
		return 0;
	}

	if (!cache->mutex_initialized) {
		if (pthread_mutex_init(&cache->mutex, NULL) != 0) {
			return 0;
		}
		cache->mutex_initialized = 1;
	}

	pthread_mutex_lock(&cache->mutex);
	embedding_cache_destroy_locked(cache);

	bucket_count = embedding_next_pow2_size(capacity * 2);
	if (bucket_count < 16) {
		bucket_count = 16;
	}

	cache->buckets = (kvs_embedding_cache_entry_t **)calloc(
		bucket_count,
		sizeof(*cache->buckets)
	);
	if (!cache->buckets) {
		pthread_mutex_unlock(&cache->mutex);
		return 0;
	}

	cache->enabled = 1;
	cache->capacity = capacity;
	cache->ttl_ms = ttl_ms;
	cache->bucket_count = bucket_count;
	embedding_stats_store(&g_embedding_runtime_stats.embedding_cache_enabled, 1);
	pthread_mutex_unlock(&cache->mutex);
	return 1;
}

static int embedding_cache_should_use(
	const kvs_vector_embedding_provider_t *provider
) {
	return provider &&
		provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C &&
		g_cfg.embedding_cache_enabled &&
		g_cfg.embedding_cache_capacity > 0 &&
		g_cfg.embedding_cache_ttl_ms > 0 &&
		strcasecmp(g_cfg.embedding_cache_policy, "lru") == 0 &&
		g_embedding_cache.enabled;
}

static int embedding_cache_lookup(
	const kvs_vector_embedding_provider_t *provider,
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
) {
	kvs_embedding_cache_key_t key;
	size_t bucket_index;
	kvs_embedding_cache_entry_t *entry;
	uint64_t lookup_start_us;
	uint64_t now_ms;
	size_t bytes;
	float *copy;
	int hit = 0;

	if (!out_embedding || !out_dim) {
		return 0;
	}

	*out_embedding = NULL;
	*out_dim = 0;

	if (!embedding_cache_should_use(provider)) {
		return 0;
	}

	if (!embedding_cache_key_build(
		provider,
		text,
		text_len,
		(int)provider->dimension,
		&key
	)) {
		return 0;
	}

	lookup_start_us = embedding_now_us();
	embedding_stats_add(&g_embedding_runtime_stats.embedding_cache_lookup_count, 1);
	pthread_mutex_lock(&g_embedding_cache.mutex);

	now_ms = embedding_now_ms();
	bucket_index = (size_t)(key.text_hash & (uint64_t)(g_embedding_cache.bucket_count - 1));
	entry = g_embedding_cache.buckets[bucket_index];
	while (entry) {
		if (embedding_cache_key_equal(&entry->key, &key)) {
			if (now_ms - entry->last_access_ms > g_embedding_cache.ttl_ms) {
				kvs_embedding_cache_entry_t *expired = entry;
				entry = entry->bucket_next;
				embedding_cache_remove_entry(&g_embedding_cache, expired, bucket_index, 1);
				continue;
			}

			bytes = (size_t)entry->dim * sizeof(float);
			copy = (float *)kvs_malloc(bytes);
			if (!copy) {
				break;
			}
			memcpy(copy, entry->embedding, bytes);
			entry->last_access_ms = now_ms;
			entry->hit_count++;
			embedding_cache_lru_touch(&g_embedding_cache, entry);
			*out_embedding = copy;
			*out_dim = (int)entry->dim;
			embedding_stats_add(&g_embedding_runtime_stats.embedding_cache_hit_count, 1);
			embedding_stats_add(
				&g_embedding_runtime_stats.embedding_cache_saved_onnx_us,
				entry->source_total_us
			);
			hit = 1;
			break;
		}
		entry = entry->bucket_next;
	}

	if (!hit) {
		embedding_stats_add(&g_embedding_runtime_stats.embedding_cache_miss_count, 1);
	}
	pthread_mutex_unlock(&g_embedding_cache.mutex);
	embedding_stats_add(
		&g_embedding_runtime_stats.embedding_cache_lookup_total_us,
		embedding_now_us() - lookup_start_us
	);
	return hit;
}

static void embedding_cache_insert(
	const kvs_vector_embedding_provider_t *provider,
	const char *text,
	size_t text_len,
	const float *embedding,
	int dim,
	uint64_t source_total_us
) {
	kvs_embedding_cache_key_t key;
	kvs_embedding_cache_entry_t *entry;
	size_t bucket_index;
	size_t bytes;
	uint64_t insert_start_us;
	uint64_t now_ms;

	if (!provider || !embedding || dim <= 0 || !embedding_cache_should_use(provider)) {
		return;
	}

	if (!embedding_cache_key_build(provider, text, text_len, dim, &key)) {
		return;
	}

	insert_start_us = embedding_now_us();
	pthread_mutex_lock(&g_embedding_cache.mutex);

	now_ms = embedding_now_ms();
	bucket_index = (size_t)(key.text_hash & (uint64_t)(g_embedding_cache.bucket_count - 1));
	entry = g_embedding_cache.buckets[bucket_index];
	while (entry) {
		if (embedding_cache_key_equal(&entry->key, &key)) {
			bytes = (size_t)dim * sizeof(float);
			memcpy(entry->embedding, embedding, bytes);
			entry->create_time_ms = now_ms;
			entry->last_access_ms = now_ms;
			entry->source_total_us = source_total_us;
			embedding_cache_lru_touch(&g_embedding_cache, entry);
			pthread_mutex_unlock(&g_embedding_cache.mutex);
			embedding_stats_add(&g_embedding_runtime_stats.embedding_cache_insert_count, 1);
			embedding_stats_add(
				&g_embedding_runtime_stats.embedding_cache_insert_total_us,
				embedding_now_us() - insert_start_us
			);
			return;
		}
		entry = entry->bucket_next;
	}

	while (g_embedding_cache.count >= g_embedding_cache.capacity &&
	       g_embedding_cache.lru_tail) {
		kvs_embedding_cache_entry_t *evict = g_embedding_cache.lru_tail;
		size_t evict_bucket = (size_t)(
			evict->key.text_hash & (uint64_t)(g_embedding_cache.bucket_count - 1)
		);
		embedding_cache_remove_entry(&g_embedding_cache, evict, evict_bucket, 1);
	}

	entry = (kvs_embedding_cache_entry_t *)calloc(1, sizeof(*entry));
	if (!entry) {
		pthread_mutex_unlock(&g_embedding_cache.mutex);
		return;
	}

	bytes = (size_t)dim * sizeof(float);
	entry->embedding = (float *)kvs_malloc(bytes);
	if (!entry->embedding) {
		free(entry);
		pthread_mutex_unlock(&g_embedding_cache.mutex);
		return;
	}

	memcpy(entry->embedding, embedding, bytes);
	entry->key = key;
	entry->dim = (uint16_t)dim;
	entry->create_time_ms = now_ms;
	entry->last_access_ms = now_ms;
	entry->hit_count = 0;
	entry->source_total_us = source_total_us;
	entry->bucket_next = g_embedding_cache.buckets[bucket_index];
	g_embedding_cache.buckets[bucket_index] = entry;
	embedding_cache_lru_link_head(&g_embedding_cache, entry);
	g_embedding_cache.count++;
	g_embedding_cache.memory_bytes += embedding_cache_entry_memory_bytes(entry);
	embedding_stats_store(
		&g_embedding_runtime_stats.embedding_cache_memory_bytes,
		g_embedding_cache.memory_bytes
	);
	pthread_mutex_unlock(&g_embedding_cache.mutex);

	embedding_stats_add(&g_embedding_runtime_stats.embedding_cache_insert_count, 1);
	embedding_stats_add(
		&g_embedding_runtime_stats.embedding_cache_insert_total_us,
		embedding_now_us() - insert_start_us
	);
}

static uint32_t embedding_provider_model_id(const char *provider_name) {
	char buffer[512];
	int written;

	written = snprintf(
		buffer,
		sizeof(buffer),
		"%s|%s|%d",
		provider_name ? provider_name : "unknown",
		g_cfg.embedding_model_path[0] != '\0' ? g_cfg.embedding_model_path : "",
		g_cfg.embedding_dim
	);
	if (written < 0) {
		return 0;
	}

	return embedding_hash_bytes32(
		buffer,
		(size_t)((written < (int)sizeof(buffer)) ? written : (int)(sizeof(buffer) - 1))
	);
}

uint32_t kvs_vector_embedding_current_model_version(void) {
	return embedding_provider_model_id(
		g_vector_state.embedding_provider.name);
}

static void embedding_record_encode_time(uint64_t elapsed_us) {
	embedding_stats_add(&g_embedding_runtime_stats.encode_total_us, elapsed_us);
	embedding_stats_update_max(&g_embedding_runtime_stats.encode_max_us, elapsed_us);
}

static uint64_t embedding_encode_enter(void) {
	uint64_t inflight = embedding_stats_add(
		&g_embedding_runtime_stats.encode_current_inflight,
		1
	);
	embedding_stats_update_max(&g_embedding_runtime_stats.encode_max_inflight, inflight);
	return embedding_now_us();
}

static void embedding_encode_leave(uint64_t start_us) {
	uint64_t end_us = embedding_now_us();

	if (end_us >= start_us) {
		embedding_record_encode_time(end_us - start_us);
	}
	embedding_stats_sub(&g_embedding_runtime_stats.encode_current_inflight, 1);
}

static int kvs_vector_embedding_l2_normalize(float *embedding, int dim) {
	double norm_sq = 0.0;
	double inv_norm;
	int i;

	if (!embedding || dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	for (i = 0; i < dim; ++i) {
		double v = (double)embedding[i];
		norm_sq += v * v;
	}

	if (norm_sq <= 0.0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	inv_norm = 1.0 / sqrt(norm_sq);
	for (i = 0; i < dim; ++i) {
		embedding[i] = (float)((double)embedding[i] * inv_norm);
	}

	return KVS_VECTOR_OK;
}

static int kvs_vector_embedding_l2_normalize_batch(
	float *embeddings,
	size_t text_count,
	int dim
) {
	size_t i;
	int ret;

	if (!embeddings || text_count == 0 || dim <= 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	for (i = 0; i < text_count; ++i) {
		ret = kvs_vector_embedding_l2_normalize(
			embeddings + (i * (size_t)dim),
			dim
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	return KVS_VECTOR_OK;
}

static int kvs_vector_embedding_error_to_status(EmbeddingErrorCode code) {
	switch (code) {
		case EMBEDDING_OK:
			return KVS_VECTOR_OK;
		case EMBEDDING_ERR_INVALID_ARGUMENT:
		case EMBEDDING_ERR_DIM_MISMATCH:
		case EMBEDDING_ERR_TEXT_TOO_LONG:
			return KVS_VECTOR_ERR_INVALID_ARG;
		case EMBEDDING_ERR_NOT_INITIALIZED:
			return KVS_VECTOR_ERR_NOT_INITIALIZED;
		case EMBEDDING_ERR_OUT_OF_MEMORY:
		case EMBEDDING_ERR_MODEL_LOAD_FAILED:
		case EMBEDDING_ERR_INFERENCE_FAILED:
		default:
			return KVS_VECTOR_ERR;
	}
}

static void kvs_vector_embedding_build_config(EmbeddingConfig *config) {
	if (!config) {
		return;
	}
	memset(config, 0, sizeof(*config));
	config->model_path = g_cfg.embedding_model_path[0] != '\0' ?
		g_cfg.embedding_model_path :
		NULL;
	config->embedding_dim = g_cfg.embedding_dim;
	config->num_threads = g_cfg.embedding_num_threads;
	config->max_text_len = g_cfg.embedding_max_text_len;
	config->normalize = 0;
}

static int kvs_vector_embedding_create_runtime_handle(
	EmbeddingHandle **out_handle,
	size_t *out_dimension
) {
	EmbeddingConfig config;
	EmbeddingHandle *handle = NULL;
	EmbeddingErrorCode err;
	int dimension;

	if (!out_handle || !out_dimension) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	*out_handle = NULL;
	*out_dimension = 0;

	kvs_vector_embedding_build_config(&config);
	err = embedding_create(&config, &handle);
	if (err != EMBEDDING_OK) {
		return kvs_vector_embedding_error_to_status(err);
	}

	dimension = embedding_dimension(handle);
	if (dimension <= 0) {
		embedding_destroy(handle);
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_handle = handle;
	*out_dimension = (size_t)dimension;
	return KVS_VECTOR_OK;
}

static void kvs_vector_embedding_log_final_runtime_stats(void) {
	kvs_vector_embedding_runtime_stats_t stats;

	memset(&stats, 0, sizeof(stats));
	kvs_vector_embedding_runtime_get_stats(&stats);
	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"embedding runtime final stats: shared_ort_env_enabled=%llu shared_ort_env_created=%llu shared_ort_env_destroyed=%llu shared_ort_env_refcount_current=%llu shared_ort_env_refcount_max=%llu ort_session_created=%llu ort_session_destroyed=%llu runtime_encode_count=%llu embedding_encode_avg_us=%llu embedding_encode_max_us=%llu tokenizer_avg_us=%llu tokenizer_max_us=%llu ort_run_avg_us=%llu ort_run_max_us=%llu resource_product=%llu worker_threads_effective=%llu embedding_num_threads_effective=%llu contexts_created=%llu contexts_destroyed=%llu encode_global_mutex=%llu encode_thread_context=%llu encode_max_inflight=%llu",
		(unsigned long long)stats.shared_ort_env_enabled,
		(unsigned long long)stats.shared_ort_env_created,
		(unsigned long long)stats.shared_ort_env_destroyed,
		(unsigned long long)stats.shared_ort_env_refcount_current,
		(unsigned long long)stats.shared_ort_env_refcount_max,
		(unsigned long long)stats.ort_session_created,
		(unsigned long long)stats.ort_session_destroyed,
		(unsigned long long)stats.encode_count,
		(unsigned long long)(stats.encode_count > 0 ?
			stats.embedding_encode_total_us / stats.encode_count :
			0),
		(unsigned long long)stats.embedding_encode_max_us,
		(unsigned long long)(stats.encode_count > 0 ?
			stats.tokenizer_total_us / stats.encode_count :
			0),
		(unsigned long long)stats.tokenizer_max_us,
		(unsigned long long)(stats.encode_count > 0 ?
			stats.ort_run_total_us / stats.encode_count :
			0),
		(unsigned long long)stats.ort_run_max_us,
		(unsigned long long)stats.embedding_runtime_resource_product,
		(unsigned long long)stats.embedding_worker_threads_effective,
		(unsigned long long)stats.embedding_num_threads_effective,
		(unsigned long long)stats.per_thread_context_created,
		(unsigned long long)stats.per_thread_context_destroyed,
		(unsigned long long)stats.encode_global_mutex_count,
		(unsigned long long)stats.encode_thread_context_count,
		(unsigned long long)stats.encode_max_inflight
	);
}

void kvs_vector_embedding_runtime_get_stats(
	kvs_vector_embedding_runtime_stats_t *out
) {
	EmbeddingRuntimeResourceStats resource_stats;
	uint64_t worker_threads_effective;
	uint64_t embedding_num_threads_effective;

	if (!out) {
		return;
	}
	memset(&resource_stats, 0, sizeof(resource_stats));
	embedding_runtime_get_resource_stats(&resource_stats);
	worker_threads_effective = g_cfg.embedding_worker_pool_enabled ?
		(uint64_t)(g_cfg.embedding_worker_threads > 0 ?
			g_cfg.embedding_worker_threads :
			0) :
		0;
	embedding_num_threads_effective = (uint64_t)(g_cfg.embedding_num_threads > 0 ?
		g_cfg.embedding_num_threads :
		1);

	out->per_thread_context_enabled = embedding_stats_load(&g_embedding_runtime_stats.per_thread_context_enabled);
	out->per_thread_context_created = embedding_stats_load(&g_embedding_runtime_stats.per_thread_context_created);
	out->per_thread_context_destroyed = embedding_stats_load(&g_embedding_runtime_stats.per_thread_context_destroyed);
	out->per_thread_context_create_failed = embedding_stats_load(&g_embedding_runtime_stats.per_thread_context_create_failed);
	out->per_thread_context_fallback_count = embedding_stats_load(&g_embedding_runtime_stats.per_thread_context_fallback_count);
	out->encode_global_mutex_count = embedding_stats_load(&g_embedding_runtime_stats.encode_global_mutex_count);
	out->encode_thread_context_count = embedding_stats_load(&g_embedding_runtime_stats.encode_thread_context_count);
	out->encode_current_inflight = embedding_stats_load(&g_embedding_runtime_stats.encode_current_inflight);
	out->encode_max_inflight = embedding_stats_load(&g_embedding_runtime_stats.encode_max_inflight);
	out->encode_total_us = embedding_stats_load(&g_embedding_runtime_stats.encode_total_us);
	out->encode_max_us = embedding_stats_load(&g_embedding_runtime_stats.encode_max_us);
	out->shared_ort_env_enabled = resource_stats.shared_ort_env_enabled;
	out->shared_ort_env_created = resource_stats.shared_ort_env_created;
	out->shared_ort_env_destroyed = resource_stats.shared_ort_env_destroyed;
	out->shared_ort_env_refcount_current = resource_stats.shared_ort_env_refcount_current;
	out->shared_ort_env_refcount_max = resource_stats.shared_ort_env_refcount_max;
	out->ort_session_created = resource_stats.ort_session_created;
	out->ort_session_destroyed = resource_stats.ort_session_destroyed;
	out->encode_count = resource_stats.encode_count +
		embedding_stats_load(&g_embedding_runtime_stats.encode_count);
	out->embedding_encode_total_us = resource_stats.embedding_encode_total_us +
		embedding_stats_load(&g_embedding_runtime_stats.embedding_encode_total_us);
	out->embedding_encode_max_us = resource_stats.embedding_encode_max_us;
	{
		uint64_t hash_encode_max_us =
			embedding_stats_load(&g_embedding_runtime_stats.embedding_encode_max_us);
		if (hash_encode_max_us > out->embedding_encode_max_us) {
			out->embedding_encode_max_us = hash_encode_max_us;
		}
	}
	out->tokenizer_total_us = resource_stats.tokenizer_total_us;
	out->tokenizer_max_us = resource_stats.tokenizer_max_us;
	out->ort_run_total_us = resource_stats.ort_run_total_us;
	out->ort_run_max_us = resource_stats.ort_run_max_us;
	out->embedding_cache_enabled = embedding_stats_load(&g_embedding_runtime_stats.embedding_cache_enabled);
	out->embedding_cache_lookup_count = embedding_stats_load(&g_embedding_runtime_stats.embedding_cache_lookup_count);
	out->embedding_cache_hit_count = embedding_stats_load(&g_embedding_runtime_stats.embedding_cache_hit_count);
	out->embedding_cache_miss_count = embedding_stats_load(&g_embedding_runtime_stats.embedding_cache_miss_count);
	out->embedding_cache_lookup_total_us = embedding_stats_load(&g_embedding_runtime_stats.embedding_cache_lookup_total_us);
	out->embedding_cache_insert_count = embedding_stats_load(&g_embedding_runtime_stats.embedding_cache_insert_count);
	out->embedding_cache_insert_total_us = embedding_stats_load(&g_embedding_runtime_stats.embedding_cache_insert_total_us);
	out->embedding_cache_evict_count = embedding_stats_load(&g_embedding_runtime_stats.embedding_cache_evict_count);
	out->embedding_cache_memory_bytes = embedding_stats_load(&g_embedding_runtime_stats.embedding_cache_memory_bytes);
	out->embedding_cache_saved_onnx_us = embedding_stats_load(&g_embedding_runtime_stats.embedding_cache_saved_onnx_us);
	out->embedding_worker_threads_effective = worker_threads_effective;
	out->embedding_num_threads_effective = embedding_num_threads_effective;
	out->embedding_runtime_resource_product =
		worker_threads_effective * embedding_num_threads_effective;
}

int kvs_vector_embedding_thread_context_init(void) {
	kvs_vector_embedding_provider_t *provider = &g_vector_state.embedding_provider;
	kvs_vector_embedding_thread_context_t *ctx;
	EmbeddingHandle *handle = NULL;
	size_t dimension = 0;
	int ret;

	if (g_embedding_thread_ctx) {
		return 0;
	}
	if (!g_cfg.embedding_per_thread_runtime_enabled) {
		return 0;
	}
	embedding_stats_store(&g_embedding_runtime_stats.per_thread_context_enabled, 1);

	if (!provider->initialized ||
	    provider->type != KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C) {
		return 0;
	}

	ctx = (kvs_vector_embedding_thread_context_t *)malloc(sizeof(*ctx));
	if (!ctx) {
		embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_create_failed, 1);
		if (g_cfg.embedding_per_thread_runtime_fallback_mutex) {
			embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_fallback_count, 1);
			return 0;
		}
		return -1;
	}
	memset(ctx, 0, sizeof(*ctx));

	ret = kvs_vector_embedding_create_runtime_handle(&handle, &dimension);
	if (ret != KVS_VECTOR_OK) {
		free(ctx);
		embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_create_failed, 1);
		if (g_cfg.embedding_per_thread_runtime_fallback_mutex) {
			embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_fallback_count, 1);
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"per-thread embedding runtime init failed; falling back to global mutex ret=%d",
				ret
			);
			return 0;
		}
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"per-thread embedding runtime init failed ret=%d",
			ret
		);
		return -1;
	}

	ctx->runtime_handle = handle;
	ctx->provider_type = provider->type;
	ctx->dimension = dimension;
	ctx->initialized = 1;
	g_embedding_thread_ctx = ctx;
	embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_created, 1);
	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"per-thread embedding runtime ready dimension=%zu",
		dimension
	);
	return 0;
}

void kvs_vector_embedding_thread_context_destroy(void) {
	kvs_vector_embedding_thread_context_t *ctx = g_embedding_thread_ctx;

	if (!ctx) {
		return;
	}
	g_embedding_thread_ctx = NULL;
	if (ctx->runtime_handle) {
		embedding_destroy(ctx->runtime_handle);
		ctx->runtime_handle = NULL;
	}
	embedding_stats_add(&g_embedding_runtime_stats.per_thread_context_destroyed, 1);
	free(ctx);
}

int kvs_vector_embedding_thread_context_available(void) {
	return g_embedding_thread_ctx &&
	       g_embedding_thread_ctx->initialized &&
	       g_embedding_thread_ctx->runtime_handle != NULL;
}

/*
 * 初始化 embedding provider。
 *
 * 当前支持 hash provider 和本地 C ABI runtime provider。
 */
int kvs_vector_embedding_provider_init(
	kvs_vector_embedding_provider_t *provider
) {
	int use_local_c;

	if (!provider) {
		KVS_LOG_ERROR(KVS_VECTOR_LOG_MODULE, "embedding provider init failed: provider is null");
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(provider, 0, sizeof(*provider));

	use_local_c =
		strcasecmp(g_cfg.embedding_provider, "local_c") == 0 ||
		strcasecmp(g_cfg.embedding_provider, "c_api") == 0;

	if (use_local_c) {
		size_t dimension;
		int ret;

		ret = kvs_vector_embedding_create_runtime_handle(
			&provider->runtime_handle,
			&dimension
		);
		if (ret != KVS_VECTOR_OK) {
			KVS_LOG_ERROR(
				KVS_VECTOR_LOG_MODULE,
				"local_c embedding provider init failed ret=%d",
				ret
			);
			return ret;
		}

		provider->type = KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C;
		provider->name = "local_c";
		provider->dimension = dimension;
		provider->model_id = embedding_provider_model_id(provider->name);
		provider->initialized = 1;

		if (g_cfg.embedding_cache_enabled) {
			if (!embedding_cache_init(
				&g_embedding_cache,
				(size_t)g_cfg.embedding_cache_capacity,
				(uint64_t)g_cfg.embedding_cache_ttl_ms
			)) {
				KVS_LOG_ERROR(
					KVS_VECTOR_LOG_MODULE,
					"embedding cache init failed capacity=%d ttl_ms=%d policy=%s",
					g_cfg.embedding_cache_capacity,
					g_cfg.embedding_cache_ttl_ms,
					g_cfg.embedding_cache_policy
				);
				return KVS_VECTOR_ERR;
			}
		} else {
			embedding_stats_store(&g_embedding_runtime_stats.embedding_cache_enabled, 0);
		}

		KVS_LOG_INFO(
			KVS_VECTOR_LOG_MODULE,
			"local_c embedding provider ready dimension=%zu max_text_len=%d threads=%d cache_enabled=%d cache_capacity=%d cache_ttl_ms=%d",
			provider->dimension,
			g_cfg.embedding_max_text_len,
			g_cfg.embedding_num_threads,
			g_cfg.embedding_cache_enabled,
			g_cfg.embedding_cache_capacity,
			g_cfg.embedding_cache_ttl_ms
		);

		return KVS_VECTOR_OK;
	}

	provider->type = KVS_VECTOR_EMBEDDING_PROVIDER_HASH;
	provider->name = "hash";
	provider->dimension = KVS_VECTOR_DEFAULT_DIMENSION;
	provider->model_id = embedding_provider_model_id(provider->name);
	provider->initialized = 1;
	embedding_stats_store(&g_embedding_runtime_stats.embedding_cache_enabled, 0);

	KVS_LOG_INFO(
		KVS_VECTOR_LOG_MODULE,
		"hash embedding provider ready dimension=%d",
		KVS_VECTOR_DEFAULT_DIMENSION
	);

	return KVS_VECTOR_OK;
}

/*
 * 销毁 embedding provider。
 */
void kvs_vector_embedding_provider_destroy(
	kvs_vector_embedding_provider_t *provider
) {
	if (!provider) {
		return;
	}

	if (provider->initialized &&
		provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_HASH) {
		KVS_LOG_DEBUG(
			KVS_VECTOR_LOG_MODULE,
			"destroy hash embedding provider"
		);
	}

	if (provider->runtime_handle) {
		embedding_destroy(provider->runtime_handle);
	}
	embedding_cache_destroy(&g_embedding_cache);
	embedding_stats_store(&g_embedding_runtime_stats.embedding_cache_enabled, 0);
	kvs_vector_embedding_log_final_runtime_stats();

	memset(provider, 0, sizeof(*provider));
}

/*
 * 返回当前 embedding provider 名称。
 */
const char *kvs_vector_embedding_provider_name(void) {
	if (!g_vector_state.embedding_provider.initialized ||
		!g_vector_state.embedding_provider.name) {
		return "unknown";
	}

	return g_vector_state.embedding_provider.name;
}

/*
 * 获取当前 embedding provider 的有效维度。
 */
size_t kvs_vector_embedding_provider_dimension(void) {
	if (!g_vector_state.embedding_provider.initialized ||
	    g_vector_state.embedding_provider.dimension == 0) {
		return KVS_VECTOR_DEFAULT_DIMENSION;
	}

	return g_vector_state.embedding_provider.dimension;
}

/*
 * 带 seed 的字节 hash。
 */
uint32_t kvs_vector_hash_bytes_seeded(
	const char *data,
	size_t len,
	uint32_t seed
) {
	uint32_t hash = 2166136261u ^ seed;
	size_t i;

	if (!data || len == 0) {
		return hash;
	}

	for (i = 0; i < len; ++i) {
		hash ^= (unsigned char)data[i];
		hash *= 16777619u;
		hash ^= (uint32_t)i + (seed << 1);
	}

	return hash;
}

/*
 * 本地 HASH embedding provider。
 *
 * 策略：
 * 1. 固定 64 维；
 * 2. 按字符/字节 hash 分桶；
 * 3. 返回原始桶计数向量，由 embedding 统一入口执行 L2 normalize。
 */
int kvs_vector_hash_embedding_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
) {
	float *embedding;
	size_t i;
	size_t dim = kvs_vector_embedding_provider_dimension();
	uint64_t start_us;
	uint64_t end_us;
	uint64_t elapsed_us;

	if (!out_embedding || !out_dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embedding = NULL;
	*out_dim = 0;

	if (!text || text_len == 0 || dim == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	embedding = (float *)kvs_malloc(dim * sizeof(float));
	if (!embedding) {
		return KVS_VECTOR_ERR;
	}

	start_us = embedding_now_us();
	memset(embedding, 0, dim * sizeof(float));

	for (i = 0; i < text_len; ++i) {
		unsigned char ch = (unsigned char)text[i];
		uint32_t h1 = kvs_vector_hash_bytes_seeded((const char *)&text[i], 1, (uint32_t)i + 1);
		uint32_t h2 = kvs_vector_hash_bytes_seeded(text, text_len, (uint32_t)ch + 131u);
		size_t bucket1 = (size_t)(h1 % dim);
		size_t bucket2 = (size_t)(h2 % dim);

		embedding[bucket1] += 1.0f;
		embedding[bucket2] += 0.5f;
	}
	end_us = embedding_now_us();
	elapsed_us = end_us >= start_us ? end_us - start_us : 0;
	embedding_stats_add(&g_embedding_runtime_stats.encode_count, 1);
	embedding_stats_add(&g_embedding_runtime_stats.embedding_encode_total_us, elapsed_us);
	embedding_stats_update_max(&g_embedding_runtime_stats.embedding_encode_max_us, elapsed_us);

	*out_embedding = embedding;
	*out_dim = (int)dim;

	return KVS_VECTOR_OK;
}

/*
 * HASH 批量 embedding provider。
 *
 * 返回连续 float 数组：embeddings[i * out_dim + j]
 * 调用方使用 kvs_free 释放 out_embeddings。
 */
int kvs_vector_hash_embedding_generate_batch(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	float **out_embeddings,
	int *out_dim
) {
	float *batch_embeddings = NULL;
	size_t dim = kvs_vector_embedding_provider_dimension();
	size_t i;
	size_t bytes;

	if (!texts || !text_lens || text_count == 0 || !out_embeddings || !out_dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embeddings = NULL;
	*out_dim = 0;

	bytes = text_count * dim * sizeof(float);
	if (dim == 0 || bytes / sizeof(float) / dim != text_count) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	batch_embeddings = (float *)kvs_malloc(bytes);
	if (!batch_embeddings) {
		return KVS_VECTOR_ERR;
	}

	memset(batch_embeddings, 0, bytes);

	for (i = 0; i < text_count; ++i) {
		float *one_embedding = NULL;
		int one_dim = 0;
		int ret;

		ret = kvs_vector_hash_embedding_generate(
			texts[i],
			text_lens[i],
			&one_embedding,
			&one_dim
		);
		if (ret != KVS_VECTOR_OK) {
			kvs_free(batch_embeddings);
			return ret;
		}

		if (!one_embedding || one_dim != (int)dim) {
			if (one_embedding) {
				kvs_free(one_embedding);
			}
			kvs_free(batch_embeddings);
			return KVS_VECTOR_ERR;
		}

		memcpy(
			batch_embeddings + (i * dim),
			one_embedding,
			dim * sizeof(float)
		);

		kvs_free(one_embedding);
	}

	*out_embeddings = batch_embeddings;
	*out_dim = (int)dim;

	return KVS_VECTOR_OK;
}

static int kvs_vector_local_c_embedding_generate(
	kvs_vector_embedding_provider_t *provider,
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
) {
	float *embedding;
	size_t dim;
	EmbeddingErrorCode err;
	EmbeddingHandle *handle;
	int use_thread_context = 0;
	uint64_t start_us;

	if (!provider || !provider->runtime_handle || !out_embedding || !out_dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embedding = NULL;
	*out_dim = 0;

	if (!text || text_len == 0 || provider->dimension == 0) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	dim = provider->dimension;
	embedding = (float *)kvs_malloc(dim * sizeof(float));
	if (!embedding) {
		return KVS_VECTOR_ERR;
	}

	handle = provider->runtime_handle;
	if (g_embedding_thread_ctx &&
	    g_embedding_thread_ctx->initialized &&
	    g_embedding_thread_ctx->runtime_handle &&
	    g_embedding_thread_ctx->provider_type == provider->type &&
	    g_embedding_thread_ctx->dimension == dim) {
		handle = g_embedding_thread_ctx->runtime_handle;
		use_thread_context = 1;
	}

	if (use_thread_context) {
		embedding_stats_add(&g_embedding_runtime_stats.encode_thread_context_count, 1);
		start_us = embedding_encode_enter();
	} else {
		embedding_stats_add(&g_embedding_runtime_stats.encode_global_mutex_count, 1);
		pthread_mutex_lock(&g_embedding_provider_encode_mu);
		start_us = embedding_encode_enter();
	}
	err = embedding_encode(
		handle,
		text,
		text_len,
		embedding,
		(int)dim
	);
	if (err != EMBEDDING_OK) {
		KVS_LOG_ERROR(
			KVS_VECTOR_LOG_MODULE,
			"local_c embedding encode failed err=%d message=%s text_len=%zu thread_context=%d",
			(int)err,
			embedding_last_error(handle),
			text_len,
			use_thread_context
		);
		embedding_encode_leave(start_us);
		if (!use_thread_context) {
			pthread_mutex_unlock(&g_embedding_provider_encode_mu);
		}
		kvs_free(embedding);
		return kvs_vector_embedding_error_to_status(err);
	}
	embedding_encode_leave(start_us);
	if (!use_thread_context) {
		pthread_mutex_unlock(&g_embedding_provider_encode_mu);
	}

	*out_embedding = embedding;
	*out_dim = (int)dim;

	return KVS_VECTOR_OK;
}

/*
 * embedding 统一入口。
 */
int kvs_vector_embedding_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dim
) {
	kvs_vector_embedding_provider_t *provider = &g_vector_state.embedding_provider;
	float *embedding = NULL;
	int dim = 0;
	int ret;
	uint64_t start_us = 0;
	uint64_t normalize_start_us = 0;
	uint64_t normalize_elapsed_us = 0;
	uint64_t total_elapsed_us = 0;

	if (!provider->initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	if (!text || text_len == 0 || !out_embedding || !out_dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embedding = NULL;
	*out_dim = 0;

	if (provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C) {
		if (embedding_cache_lookup(
			provider,
			text,
			text_len,
			&embedding,
			&dim
		)) {
			*out_embedding = embedding;
			*out_dim = dim;
			return KVS_VECTOR_OK;
		}

		start_us = embedding_now_us();
		ret = kvs_vector_local_c_embedding_generate(
			provider,
			text,
			text_len,
			&embedding,
			&dim
		);
	} else {
		ret = kvs_vector_hash_embedding_generate(
			text,
			text_len,
			&embedding,
			&dim
		);
	}
	if (ret != KVS_VECTOR_OK) {
		return ret;
	}

	normalize_start_us = embedding_now_us();
	ret = kvs_vector_embedding_l2_normalize(embedding, dim);
	normalize_elapsed_us = embedding_now_us() - normalize_start_us;
	(void)normalize_elapsed_us;
	if (ret != KVS_VECTOR_OK) {
		kvs_free(embedding);
		return ret;
	}

	if (provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C) {
		total_elapsed_us = embedding_now_us() - start_us;
		embedding_cache_insert(
			provider,
			text,
			text_len,
			embedding,
			dim,
			total_elapsed_us
		);
	}

	*out_embedding = embedding;
	*out_dim = dim;

	return KVS_VECTOR_OK;
}

/*
 * embedding 批量统一入口。
 */
int kvs_vector_embedding_generate_batch(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	float **out_embeddings,
	int *out_dim
) {
	kvs_vector_embedding_provider_t *provider = &g_vector_state.embedding_provider;
	float *embeddings = NULL;
	int dim = 0;
	int ret;

	if (!provider->initialized) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}

	if (!texts || !text_lens || text_count == 0 || !out_embeddings || !out_dim) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	*out_embeddings = NULL;
	*out_dim = 0;

	if (provider->type == KVS_VECTOR_EMBEDDING_PROVIDER_LOCAL_C) {
		size_t provider_dim = provider->dimension;
		size_t i;
		size_t bytes;
		EmbeddingHandle *handle = provider->runtime_handle;
		int use_thread_context = 0;
		uint64_t start_us;

		if (provider_dim == 0 ||
		    text_count > ((size_t)-1 / provider_dim / sizeof(float))) {
			return KVS_VECTOR_ERR_INVALID_ARG;
		}

		bytes = text_count * provider_dim * sizeof(float);
		embeddings = (float *)kvs_malloc(bytes);
		if (!embeddings) {
			return KVS_VECTOR_ERR;
		}

		if (g_embedding_thread_ctx &&
		    g_embedding_thread_ctx->initialized &&
		    g_embedding_thread_ctx->runtime_handle &&
		    g_embedding_thread_ctx->provider_type == provider->type &&
		    g_embedding_thread_ctx->dimension == provider_dim) {
			handle = g_embedding_thread_ctx->runtime_handle;
			use_thread_context = 1;
		}

		if (use_thread_context) {
			embedding_stats_add(&g_embedding_runtime_stats.encode_thread_context_count, text_count);
			start_us = embedding_encode_enter();
		} else {
			embedding_stats_add(&g_embedding_runtime_stats.encode_global_mutex_count, text_count);
			pthread_mutex_lock(&g_embedding_provider_encode_mu);
			start_us = embedding_encode_enter();
		}
		{
			EmbeddingErrorCode err = embedding_encode_batch(
				handle,
				texts,
				text_lens,
				text_count,
				embeddings,
				(int)provider_dim
			);
			if (err != EMBEDDING_OK) {
				KVS_LOG_ERROR(
					KVS_VECTOR_LOG_MODULE,
					"local_c embedding batch encode failed err=%d message=%s count=%zu thread_context=%d",
					(int)err,
					embedding_last_error(handle),
					text_count,
					use_thread_context
				);
				embedding_encode_leave(start_us);
				if (!use_thread_context) {
					pthread_mutex_unlock(&g_embedding_provider_encode_mu);
				}
				kvs_free(embeddings);
				return kvs_vector_embedding_error_to_status(err);
			}
		}
		embedding_encode_leave(start_us);
		if (!use_thread_context) {
			pthread_mutex_unlock(&g_embedding_provider_encode_mu);
		}

		dim = (int)provider_dim;
	} else {
		ret = kvs_vector_hash_embedding_generate_batch(
			texts,
			text_lens,
			text_count,
			&embeddings,
			&dim
		);
		if (ret != KVS_VECTOR_OK) {
			return ret;
		}
	}

	ret = kvs_vector_embedding_l2_normalize_batch(embeddings, text_count, dim);
	if (ret != KVS_VECTOR_OK) {
		kvs_free(embeddings);
		return ret;
	}

	*out_embeddings = embeddings;
	*out_dim = dim;

	return KVS_VECTOR_OK;
}

/*
 * embedding 并发批量统一入口。
 *
 * hash provider 没有远程 RPC，直接复用普通 batch。
 */
int kvs_vector_embedding_generate_batch_parallel(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	size_t batch_size,
	size_t max_inflight,
	float **out_embeddings,
	int *out_dim
) {
	(void)batch_size;
	(void)max_inflight;

	return kvs_vector_embedding_generate_batch(
		texts,
		text_lens,
		text_count,
		out_embeddings,
		out_dim
	);
}
