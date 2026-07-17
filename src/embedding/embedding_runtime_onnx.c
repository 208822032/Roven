#include "embedding_c_api.h"
#include "kvs_config.h"

#include <onnxruntime_c_api.h>

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define EMBEDDING_ONNX_DEFAULT_MAX_TEXT_LEN 512
#define EMBEDDING_ONNX_MAX_ERROR 512
#define EMBEDDING_ONNX_MAX_TOKENS 512
#define EMBEDDING_ONNX_WORDPIECE_MAX_CHARS 256

extern kvs_config_t g_cfg;

typedef struct EmbeddingVocabHashEntry {
	const char *token;
	int64_t id;
} EmbeddingVocabHashEntry;

typedef struct EmbeddingVocab {
	char **tokens;
	size_t count;
	size_t capacity;
	EmbeddingVocabHashEntry *hash_entries;
	size_t hash_capacity;
} EmbeddingVocab;

struct EmbeddingHandle {
	int embedding_dim;
	int model_output_dim;
	int num_threads;
	int max_text_len;
	int max_tokens;
	int normalize;
	char *model_path;
	char *vocab_path;
	char last_error[EMBEDDING_ONNX_MAX_ERROR];

	const OrtApi *ort;
	OrtEnv *env;
	int shared_env_ref_acquired;
	int owns_env;
	OrtSessionOptions *session_options;
	OrtSession *session;
	OrtMemoryInfo *memory_info;

	EmbeddingVocab vocab;
	int64_t pad_id;
	int64_t unk_id;
	int64_t cls_id;
	int64_t sep_id;

	int profile_enabled;
	pthread_mutex_t profile_mutex;
	int profile_mutex_initialized;
	uint64_t profile_count;
	uint64_t profile_success_count;
	uint64_t profile_total_us;
	uint64_t profile_tokenize_us;
	uint64_t profile_tensor_us;
	uint64_t profile_ort_us;
	uint64_t profile_output_us;
	uint64_t profile_enter_count;
	uint64_t profile_inflight;
	uint64_t profile_max_inflight;
};

typedef struct EmbeddingOnnxResourceStats {
	uint64_t shared_ort_env_enabled;
	uint64_t shared_ort_env_created;
	uint64_t shared_ort_env_destroyed;
	uint64_t shared_ort_env_refcount_current;
	uint64_t shared_ort_env_refcount_max;
	uint64_t ort_session_created;
	uint64_t ort_session_destroyed;
	uint64_t encode_count;
	uint64_t embedding_encode_total_us;
	uint64_t embedding_encode_max_us;
	uint64_t tokenizer_total_us;
	uint64_t tokenizer_max_us;
	uint64_t ort_run_total_us;
	uint64_t ort_run_max_us;
} EmbeddingOnnxResourceStats;

static pthread_mutex_t g_onnx_env_mu = PTHREAD_MUTEX_INITIALIZER;
static OrtEnv *g_shared_ort_env;
static int g_shared_ort_env_refcount;
static const OrtApi *g_shared_ort_api;
static EmbeddingOnnxResourceStats g_onnx_resource_stats;

static uint64_t embedding_resource_stats_add(uint64_t *field, uint64_t value) {
	return __atomic_add_fetch(field, value, __ATOMIC_RELAXED);
}

static uint64_t embedding_resource_stats_load(const uint64_t *field) {
	return __atomic_load_n(field, __ATOMIC_RELAXED);
}

static void embedding_resource_stats_store(uint64_t *field, uint64_t value) {
	__atomic_store_n(field, value, __ATOMIC_RELAXED);
}

static void embedding_resource_stats_update_max(uint64_t *field, uint64_t value) {
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

static uint64_t embedding_elapsed_us(uint64_t start, uint64_t end) {
	if (end < start) {
		return 0;
	}

	return end - start;
}

static void embedding_set_error(EmbeddingHandle *handle, const char *message) {
	if (!handle || !message) {
		return;
	}

	snprintf(handle->last_error, sizeof(handle->last_error), "%s", message);
}

static char *embedding_strdup_or_null(const char *src) {
	size_t len;
	char *copy;

	if (!src) {
		return NULL;
	}

	len = strlen(src);
	copy = (char *)malloc(len + 1);
	if (!copy) {
		return NULL;
	}

	memcpy(copy, src, len + 1);
	return copy;
}

static int embedding_ascii_tolower(int ch) {
	if (ch >= 'A' && ch <= 'Z') {
		return ch - 'A' + 'a';
	}

	return ch;
}

static int embedding_is_ascii_space(unsigned char ch) {
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
		ch == '\f' || ch == '\v';
}

static int embedding_is_ascii_alnum(unsigned char ch) {
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		(ch >= '0' && ch <= '9');
}

static size_t embedding_utf8_char_len(unsigned char ch) {
	if ((ch & 0x80u) == 0) {
		return 1;
	}
	if ((ch & 0xe0u) == 0xc0u) {
		return 2;
	}
	if ((ch & 0xf0u) == 0xe0u) {
		return 3;
	}
	if ((ch & 0xf8u) == 0xf0u) {
		return 4;
	}

	return 1;
}

static char *embedding_join_dir_file(const char *path, const char *file) {
	const char *slash;
	size_t dir_len;
	size_t file_len;
	char *joined;

	if (!path || !file) {
		return NULL;
	}

	slash = strrchr(path, '/');
	dir_len = slash ? (size_t)(slash - path) : 0;
	file_len = strlen(file);

	joined = (char *)malloc(dir_len + 1 + file_len + 1);
	if (!joined) {
		return NULL;
	}

	if (dir_len > 0) {
		memcpy(joined, path, dir_len);
		joined[dir_len] = '/';
		memcpy(joined + dir_len + 1, file, file_len + 1);
	} else {
		memcpy(joined, file, file_len + 1);
	}

	return joined;
}

static void embedding_vocab_destroy(EmbeddingVocab *vocab) {
	size_t i;

	if (!vocab) {
		return;
	}

	free(vocab->hash_entries);
	for (i = 0; i < vocab->count; ++i) {
		free(vocab->tokens[i]);
	}
	vocab->hash_entries = NULL;
	free(vocab->tokens);
	vocab->tokens = NULL;
	vocab->count = 0;
	vocab->capacity = 0;
	vocab->hash_capacity = 0;
}

static int embedding_vocab_add(EmbeddingVocab *vocab, const char *token) {
	char **grown;
	char *copy;

	if (!vocab || !token) {
		return 0;
	}

	if (vocab->count == vocab->capacity) {
		size_t next_capacity = vocab->capacity == 0 ?
			4096 :
			vocab->capacity * 2;
		grown = (char **)realloc(
			vocab->tokens,
			next_capacity * sizeof(*grown)
		);
		if (!grown) {
			return 0;
		}
		vocab->tokens = grown;
		vocab->capacity = next_capacity;
	}

	copy = embedding_strdup_or_null(token);
	if (!copy) {
		return 0;
	}

	vocab->tokens[vocab->count++] = copy;
	return 1;
}

static uint64_t embedding_vocab_hash(const char *token) {
	const unsigned char *p = (const unsigned char *)token;
	uint64_t hash = 1469598103934665603ull;

	while (*p) {
		hash ^= (uint64_t)(*p++);
		hash *= 1099511628211ull;
	}

	return hash;
}

static size_t embedding_next_power_of_two(size_t value) {
	size_t result = 1;

	while (result < value && result <= ((size_t)-1 / 2)) {
		result <<= 1;
	}

	return result;
}

static int embedding_vocab_build_hash(EmbeddingVocab *vocab) {
	size_t min_capacity;
	size_t i;

	if (!vocab) {
		return 0;
	}

	free(vocab->hash_entries);
	vocab->hash_entries = NULL;
	vocab->hash_capacity = 0;

	min_capacity = vocab->count > 0 ? vocab->count * 2 : 1;
	vocab->hash_capacity = embedding_next_power_of_two(min_capacity);
	if (vocab->hash_capacity < min_capacity) {
		return 0;
	}

	vocab->hash_entries = (EmbeddingVocabHashEntry *)calloc(
		vocab->hash_capacity,
		sizeof(*vocab->hash_entries)
	);
	if (!vocab->hash_entries) {
		vocab->hash_capacity = 0;
		return 0;
	}

	for (i = 0; i < vocab->count; ++i) {
		size_t mask = vocab->hash_capacity - 1;
		size_t pos = (size_t)embedding_vocab_hash(vocab->tokens[i]) & mask;

		while (vocab->hash_entries[pos].token) {
			pos = (pos + 1) & mask;
		}

		vocab->hash_entries[pos].token = vocab->tokens[i];
		vocab->hash_entries[pos].id = (int64_t)i;
	}

	return 1;
}

static int64_t embedding_vocab_lookup(
	const EmbeddingVocab *vocab,
	const char *token
) {
	size_t i;

	if (!vocab || !token) {
		return -1;
	}

	if (vocab->hash_entries && vocab->hash_capacity > 0) {
		size_t mask = vocab->hash_capacity - 1;
		size_t pos = (size_t)embedding_vocab_hash(token) & mask;

		while (vocab->hash_entries[pos].token) {
			if (strcmp(vocab->hash_entries[pos].token, token) == 0) {
				return vocab->hash_entries[pos].id;
			}
			pos = (pos + 1) & mask;
		}

		return -1;
	}

	for (i = 0; i < vocab->count; ++i) {
		if (strcmp(vocab->tokens[i], token) == 0) {
			return (int64_t)i;
		}
	}

	return -1;
}

static EmbeddingErrorCode embedding_load_vocab(EmbeddingHandle *handle) {
	FILE *fp;
	char line[1024];

	if (!handle || !handle->vocab_path) {
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	fp = fopen(handle->vocab_path, "r");
	if (!fp) {
		snprintf(
			handle->last_error,
			sizeof(handle->last_error),
			"vocab.txt not found: %s",
			handle->vocab_path
		);
		return EMBEDDING_ERR_MODEL_LOAD_FAILED;
	}

	while (fgets(line, sizeof(line), fp)) {
		size_t len = strlen(line);
		while (len > 0 &&
		       (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}

		if (!embedding_vocab_add(&handle->vocab, line)) {
			fclose(fp);
			embedding_set_error(handle, "failed to allocate vocab token");
			return EMBEDDING_ERR_OUT_OF_MEMORY;
		}
	}

	fclose(fp);

	if (!embedding_vocab_build_hash(&handle->vocab)) {
		embedding_set_error(handle, "failed to allocate vocab hash table");
		return EMBEDDING_ERR_OUT_OF_MEMORY;
	}

	handle->pad_id = embedding_vocab_lookup(&handle->vocab, "[PAD]");
	handle->unk_id = embedding_vocab_lookup(&handle->vocab, "[UNK]");
	handle->cls_id = embedding_vocab_lookup(&handle->vocab, "[CLS]");
	handle->sep_id = embedding_vocab_lookup(&handle->vocab, "[SEP]");

	if (handle->pad_id < 0 || handle->unk_id < 0 ||
	    handle->cls_id < 0 || handle->sep_id < 0) {
		snprintf(
			handle->last_error,
			sizeof(handle->last_error),
			"special token missing in vocab: [PAD]=%lld [UNK]=%lld [CLS]=%lld [SEP]=%lld",
			(long long)handle->pad_id,
			(long long)handle->unk_id,
			(long long)handle->cls_id,
			(long long)handle->sep_id
		);
		return EMBEDDING_ERR_MODEL_LOAD_FAILED;
	}

	fprintf(
		stderr,
		"[kvlite][embedding][onnx] vocab loaded path=%s size=%zu pad=%lld unk=%lld cls=%lld sep=%lld\n",
		handle->vocab_path,
		handle->vocab.count,
		(long long)handle->pad_id,
		(long long)handle->unk_id,
		(long long)handle->cls_id,
		(long long)handle->sep_id
	);

	return EMBEDDING_OK;
}

static EmbeddingErrorCode embedding_set_ort_error(
	EmbeddingHandle *handle,
	OrtStatus *status,
	const char *context,
	EmbeddingErrorCode code
) {
	const char *message = "unknown ONNX Runtime error";

	if (!handle) {
		return code;
	}

	if (handle->ort && status) {
		message = handle->ort->GetErrorMessage(status);
	}

	snprintf(
		handle->last_error,
		sizeof(handle->last_error),
		"%s: %s",
		context ? context : "ONNX Runtime error",
		message ? message : "unknown ONNX Runtime error"
	);

	if (handle->ort && status) {
		handle->ort->ReleaseStatus(status);
	}

	return code;
}

static EmbeddingErrorCode embedding_acquire_shared_ort_env(
	EmbeddingHandle *handle
) {
	OrtStatus *status;

	if (!handle || !handle->ort) {
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	if (!g_cfg.embedding_shared_ort_env_enabled) {
		embedding_resource_stats_store(
			&g_onnx_resource_stats.shared_ort_env_enabled,
			0
		);
		status = handle->ort->CreateEnv(
			ORT_LOGGING_LEVEL_WARNING,
			"kvlite_embedding",
			&handle->env
		);
		if (status) {
			return embedding_set_ort_error(
				handle,
				status,
				"failed to create ONNX Runtime environment",
				EMBEDDING_ERR_MODEL_LOAD_FAILED
			);
		}
		handle->owns_env = 1;
		return EMBEDDING_OK;
	}

	embedding_resource_stats_store(
		&g_onnx_resource_stats.shared_ort_env_enabled,
		1
	);

	pthread_mutex_lock(&g_onnx_env_mu);
	if (!g_shared_ort_env) {
		status = handle->ort->CreateEnv(
			ORT_LOGGING_LEVEL_WARNING,
			"kvlite_embedding",
			&g_shared_ort_env
		);
		if (status) {
			pthread_mutex_unlock(&g_onnx_env_mu);
			return embedding_set_ort_error(
				handle,
				status,
				"failed to create shared ONNX Runtime environment",
				EMBEDDING_ERR_MODEL_LOAD_FAILED
			);
		}
		g_shared_ort_api = handle->ort;
		embedding_resource_stats_add(
			&g_onnx_resource_stats.shared_ort_env_created,
			1
		);
	}

	g_shared_ort_env_refcount++;
	handle->env = g_shared_ort_env;
	handle->shared_env_ref_acquired = 1;
	embedding_resource_stats_store(
		&g_onnx_resource_stats.shared_ort_env_refcount_current,
		(uint64_t)g_shared_ort_env_refcount
	);
	embedding_resource_stats_update_max(
		&g_onnx_resource_stats.shared_ort_env_refcount_max,
		(uint64_t)g_shared_ort_env_refcount
	);
	pthread_mutex_unlock(&g_onnx_env_mu);
	return EMBEDDING_OK;
}

static void embedding_release_shared_ort_env_ref(EmbeddingHandle *handle) {
	const OrtApi *ort_api;
	OrtEnv *env_to_release = NULL;

	if (!handle) {
		return;
	}
	if (handle->owns_env && handle->env && handle->ort) {
		handle->ort->ReleaseEnv(handle->env);
		handle->env = NULL;
		handle->owns_env = 0;
		return;
	}
	if (!handle->shared_env_ref_acquired) {
		return;
	}

	pthread_mutex_lock(&g_onnx_env_mu);
	if (g_shared_ort_env_refcount > 0) {
		g_shared_ort_env_refcount--;
	}
	if (g_shared_ort_env_refcount == 0 && g_shared_ort_env) {
		env_to_release = g_shared_ort_env;
		ort_api = g_shared_ort_api ? g_shared_ort_api : handle->ort;
		g_shared_ort_env = NULL;
		g_shared_ort_api = NULL;
	} else {
		ort_api = NULL;
	}
	embedding_resource_stats_store(
		&g_onnx_resource_stats.shared_ort_env_refcount_current,
		(uint64_t)g_shared_ort_env_refcount
	);
	pthread_mutex_unlock(&g_onnx_env_mu);

	handle->shared_env_ref_acquired = 0;
	handle->env = NULL;
	if (env_to_release && ort_api) {
		ort_api->ReleaseEnv(env_to_release);
		embedding_resource_stats_add(
			&g_onnx_resource_stats.shared_ort_env_destroyed,
			1
		);
	}
}

static int embedding_parse_graph_opt_level(
	const char *value,
	GraphOptimizationLevel *level
) {
	if (!value || !level) {
		return 0;
	}

	if (strcasecmp(value, "basic") == 0) {
		*level = ORT_ENABLE_BASIC;
		return 1;
	}
	if (strcasecmp(value, "extended") == 0) {
		*level = ORT_ENABLE_EXTENDED;
		return 1;
	}
	if (strcasecmp(value, "all") == 0) {
		*level = ORT_ENABLE_ALL;
		return 1;
	}

	return 0;
}

static int embedding_parse_execution_mode(
	const char *value,
	ExecutionMode *mode
) {
	if (!value || !mode) {
		return 0;
	}

	if (strcasecmp(value, "sequential") == 0) {
		*mode = ORT_SEQUENTIAL;
		return 1;
	}
	if (strcasecmp(value, "parallel") == 0) {
		*mode = ORT_PARALLEL;
		return 1;
	}

	return 0;
}

static void embedding_profile_record(
	EmbeddingHandle *handle,
	int success,
	uint64_t total_us,
	uint64_t tokenize_us,
	uint64_t tensor_us,
	uint64_t ort_us,
	uint64_t output_us
) {
	uint64_t count;

	if (!handle || !handle->profile_enabled ||
	    !handle->profile_mutex_initialized) {
		return;
	}

	pthread_mutex_lock(&handle->profile_mutex);
	handle->profile_count++;
	if (success) {
		handle->profile_success_count++;
	}
	handle->profile_total_us += total_us;
	handle->profile_tokenize_us += tokenize_us;
	handle->profile_tensor_us += tensor_us;
	handle->profile_ort_us += ort_us;
	handle->profile_output_us += output_us;
	count = handle->profile_count;

	if (count % 100 == 0) {
		fprintf(
			stderr,
			"[kvlite][embedding][profile] count=%llu success_count=%llu avg_total_us=%llu avg_tokenize_us=%llu avg_tensor_us=%llu avg_ort_us=%llu avg_output_us=%llu enter_count=%llu max_inflight=%llu current_inflight=%llu\n",
			(unsigned long long)handle->profile_count,
			(unsigned long long)handle->profile_success_count,
			(unsigned long long)(handle->profile_total_us / handle->profile_count),
			(unsigned long long)(handle->profile_tokenize_us / handle->profile_count),
			(unsigned long long)(handle->profile_tensor_us / handle->profile_count),
			(unsigned long long)(handle->profile_ort_us / handle->profile_count),
			(unsigned long long)(handle->profile_output_us / handle->profile_count),
			(unsigned long long)handle->profile_enter_count,
			(unsigned long long)handle->profile_max_inflight,
			(unsigned long long)handle->profile_inflight
		);
	}
	pthread_mutex_unlock(&handle->profile_mutex);
}

static void embedding_profile_enter(EmbeddingHandle *handle) {
	if (!handle || !handle->profile_enabled ||
	    !handle->profile_mutex_initialized) {
		return;
	}

	pthread_mutex_lock(&handle->profile_mutex);
	handle->profile_enter_count++;
	handle->profile_inflight++;
	if (handle->profile_inflight > handle->profile_max_inflight) {
		handle->profile_max_inflight = handle->profile_inflight;
	}
	pthread_mutex_unlock(&handle->profile_mutex);
}

static void embedding_profile_leave(EmbeddingHandle *handle) {
	if (!handle || !handle->profile_enabled ||
	    !handle->profile_mutex_initialized) {
		return;
	}

	pthread_mutex_lock(&handle->profile_mutex);
	if (handle->profile_inflight > 0) {
		handle->profile_inflight--;
	}
	pthread_mutex_unlock(&handle->profile_mutex);
}

static void embedding_release_ort_resources(EmbeddingHandle *handle) {
	if (!handle || !handle->ort) {
		return;
	}

	if (handle->session) {
		handle->ort->ReleaseSession(handle->session);
		handle->session = NULL;
		embedding_resource_stats_add(
			&g_onnx_resource_stats.ort_session_destroyed,
			1
		);
	}

	if (handle->session_options) {
		handle->ort->ReleaseSessionOptions(handle->session_options);
		handle->session_options = NULL;
	}

	if (handle->memory_info) {
		handle->ort->ReleaseMemoryInfo(handle->memory_info);
		handle->memory_info = NULL;
	}

	embedding_release_shared_ort_env_ref(handle);
}

static const char *embedding_onnx_element_type_name(
	ONNXTensorElementDataType type
) {
	switch (type) {
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
		return "float32";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
		return "uint8";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
		return "int8";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:
		return "uint16";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:
		return "int16";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
		return "int32";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
		return "int64";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING:
		return "string";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
		return "bool";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
		return "float16";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
		return "double";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:
		return "uint32";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:
		return "uint64";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX64:
		return "complex64";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX128:
		return "complex128";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16:
		return "bfloat16";
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED:
	default:
		return "undefined";
	}
}

static void embedding_print_shape(const int64_t *dims, size_t rank) {
	size_t i;

	fprintf(stderr, "[");
	for (i = 0; i < rank; ++i) {
		if (i > 0) {
			fprintf(stderr, ", ");
		}

		if (dims[i] < 0) {
			fprintf(stderr, "-1");
		} else {
			fprintf(stderr, "%lld", (long long)dims[i]);
		}
	}
	fprintf(stderr, "]");
}

static EmbeddingErrorCode embedding_inspect_onnx_value(
	EmbeddingHandle *handle,
	OrtAllocator *allocator,
	size_t index,
	int is_input
) {
	OrtStatus *status;
	char *name = NULL;
	OrtTypeInfo *type_info = NULL;
	const OrtTensorTypeAndShapeInfo *tensor_info = NULL;
	ONNXTensorElementDataType element_type =
		ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
	size_t rank = 0;
	int64_t *dims = NULL;
	EmbeddingErrorCode result = EMBEDDING_OK;
	const char *kind = is_input ? "input" : "output";

	if (is_input) {
		status = handle->ort->SessionGetInputName(
			handle->session,
			index,
			allocator,
			&name
		);
	} else {
		status = handle->ort->SessionGetOutputName(
			handle->session,
			index,
			allocator,
			&name
		);
	}
	if (status) {
		return embedding_set_ort_error(
			handle,
			status,
			is_input ?
				"failed to get ONNX input name" :
				"failed to get ONNX output name",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
	}

	if (is_input) {
		status = handle->ort->SessionGetInputTypeInfo(
			handle->session,
			index,
			&type_info
		);
	} else {
		status = handle->ort->SessionGetOutputTypeInfo(
			handle->session,
			index,
			&type_info
		);
	}
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			is_input ?
				"failed to get ONNX input type info" :
				"failed to get ONNX output type info",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
		goto cleanup;
	}

	status = handle->ort->CastTypeInfoToTensorInfo(type_info, &tensor_info);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			is_input ?
				"failed to read ONNX input tensor type info" :
				"failed to read ONNX output tensor type info",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
		goto cleanup;
	}

	status = handle->ort->GetTensorElementType(tensor_info, &element_type);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			is_input ?
				"failed to read ONNX input element type" :
				"failed to read ONNX output element type",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
		goto cleanup;
	}

	status = handle->ort->GetDimensionsCount(tensor_info, &rank);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			is_input ?
				"failed to read ONNX input rank" :
				"failed to read ONNX output rank",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
		goto cleanup;
	}

	if (rank > 0) {
		dims = (int64_t *)calloc(rank, sizeof(*dims));
		if (!dims) {
			embedding_set_error(
				handle,
				"failed to allocate ONNX tensor shape buffer"
			);
			result = EMBEDDING_ERR_OUT_OF_MEMORY;
			goto cleanup;
		}

		status = handle->ort->GetDimensions(tensor_info, dims, rank);
		if (status) {
			result = embedding_set_ort_error(
				handle,
				status,
				is_input ?
					"failed to read ONNX input shape" :
					"failed to read ONNX output shape",
				EMBEDDING_ERR_MODEL_LOAD_FAILED
			);
			goto cleanup;
		}
	}
	if (!is_input && rank > 0 && dims[rank - 1] > 0 &&
	    dims[rank - 1] <= INT_MAX) {
		int detected = (int)dims[rank - 1];
		if (handle->model_output_dim == 0) {
			handle->model_output_dim = detected;
		} else if (handle->model_output_dim != detected) {
			embedding_set_error(handle, "ONNX outputs have inconsistent embedding dimensions");
			result = EMBEDDING_ERR_DIM_MISMATCH;
			goto cleanup;
		}
	}

	fprintf(
		stderr,
		"[kvlite][embedding][onnx] %s[%zu] name=%s type=%s(%d) shape=",
		kind,
		index,
		name ? name : "",
		embedding_onnx_element_type_name(element_type),
		(int)element_type
	);
	embedding_print_shape(dims, rank);
	fprintf(stderr, "\n");

cleanup:
	free(dims);
	if (type_info) {
		handle->ort->ReleaseTypeInfo(type_info);
	}
	if (name) {
		allocator->Free(allocator, name);
	}
	return result;
}

static EmbeddingErrorCode embedding_inspect_onnx_model(
	EmbeddingHandle *handle
) {
	OrtStatus *status;
	OrtAllocator *allocator = NULL;
	size_t input_count = 0;
	size_t output_count = 0;
	size_t i;
	EmbeddingErrorCode result;

	status = handle->ort->GetAllocatorWithDefaultOptions(&allocator);
	if (status) {
		return embedding_set_ort_error(
			handle,
			status,
			"failed to get ONNX Runtime default allocator",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
	}

	status = handle->ort->SessionGetInputCount(
		handle->session,
		&input_count
	);
	if (status) {
		return embedding_set_ort_error(
			handle,
			status,
			"failed to get ONNX input count",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
	}

	fprintf(
		stderr,
		"[kvlite][embedding][onnx] model=%s inputs=%zu\n",
		handle->model_path,
		input_count
	);
	for (i = 0; i < input_count; ++i) {
		result = embedding_inspect_onnx_value(handle, allocator, i, 1);
		if (result != EMBEDDING_OK) {
			return result;
		}
	}

	status = handle->ort->SessionGetOutputCount(
		handle->session,
		&output_count
	);
	if (status) {
		return embedding_set_ort_error(
			handle,
			status,
			"failed to get ONNX output count",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
	}

	fprintf(
		stderr,
		"[kvlite][embedding][onnx] model=%s outputs=%zu\n",
		handle->model_path,
		output_count
	);
	for (i = 0; i < output_count; ++i) {
		result = embedding_inspect_onnx_value(handle, allocator, i, 0);
		if (result != EMBEDDING_OK) {
			return result;
		}
	}

	return EMBEDDING_OK;
}

static int embedding_token_push(
	int64_t *ids,
	int *count,
	int max_content_tokens,
	int64_t id
) {
	if (*count >= max_content_tokens) {
		return 0;
	}

	ids[(*count)++] = id;
	return 1;
}

static int embedding_tokenize_wordpiece(
	EmbeddingHandle *handle,
	const char *word,
	size_t word_len,
	int64_t *ids,
	int *count,
	int max_content_tokens
) {
	size_t start = 0;
	int matched_any = 0;

	if (word_len == 0) {
		return 1;
	}

	while (start < word_len) {
		size_t end = word_len;
		int64_t matched_id = -1;
		size_t matched_end = 0;
		char token[EMBEDDING_ONNX_WORDPIECE_MAX_CHARS + 3];

		while (end > start) {
			size_t piece_len = end - start;
			size_t prefix_len = start == 0 ? 0 : 2;

			if (piece_len + prefix_len >= sizeof(token)) {
				end--;
				continue;
			}

			if (prefix_len > 0) {
				token[0] = '#';
				token[1] = '#';
			}
			memcpy(token + prefix_len, word + start, piece_len);
			token[prefix_len + piece_len] = '\0';

			matched_id = embedding_vocab_lookup(&handle->vocab, token);
			if (matched_id >= 0) {
				matched_end = end;
				break;
			}
			end--;
		}

		if (matched_id < 0) {
			return embedding_token_push(
				ids,
				count,
				max_content_tokens,
				handle->unk_id
			);
		}

		if (!embedding_token_push(ids, count, max_content_tokens, matched_id)) {
			return 0;
		}

		matched_any = 1;
		start = matched_end;
	}

	return matched_any;
}

static int embedding_tokenize_char(
	EmbeddingHandle *handle,
	const char *text,
	size_t char_len,
	int64_t *ids,
	int *count,
	int max_content_tokens
) {
	char token[8];
	int64_t id;

	if (char_len == 0 || char_len >= sizeof(token)) {
		id = handle->unk_id;
	} else {
		memcpy(token, text, char_len);
		token[char_len] = '\0';
		id = embedding_vocab_lookup(&handle->vocab, token);
		if (id < 0) {
			id = handle->unk_id;
		}
	}

	return embedding_token_push(ids, count, max_content_tokens, id);
}

static EmbeddingErrorCode bert_tokenize(
	EmbeddingHandle *handle,
	const char *text,
	size_t text_len,
	int64_t *input_ids,
	int64_t *attention_mask,
	int64_t *token_type_ids,
	int max_tokens,
	int *out_seq_len
) {
	int64_t *content_ids;
	int content_count = 0;
	int max_content_tokens = max_tokens - 2;
	size_t i = 0;
	int pos;

	if (!handle || !text || text_len == 0 || !input_ids ||
	    !attention_mask || !token_type_ids || !out_seq_len ||
	    max_tokens < 2) {
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	content_ids = (int64_t *)calloc((size_t)max_content_tokens, sizeof(*content_ids));
	if (!content_ids) {
		embedding_set_error(handle, "failed to allocate tokenizer buffer");
		return EMBEDDING_ERR_OUT_OF_MEMORY;
	}

	while (i < text_len && content_count < max_content_tokens) {
		unsigned char ch = (unsigned char)text[i];

		if (embedding_is_ascii_space(ch)) {
			i++;
			continue;
		}

		if (embedding_is_ascii_alnum(ch)) {
			char word[EMBEDDING_ONNX_WORDPIECE_MAX_CHARS];
			size_t word_len = 0;

			while (i < text_len &&
			       embedding_is_ascii_alnum((unsigned char)text[i]) &&
			       word_len + 1 < sizeof(word)) {
				word[word_len++] =
					(char)embedding_ascii_tolower((unsigned char)text[i]);
				i++;
			}

			while (i < text_len &&
			       embedding_is_ascii_alnum((unsigned char)text[i])) {
				i++;
			}

			if (!embedding_tokenize_wordpiece(
				handle,
				word,
				word_len,
				content_ids,
				&content_count,
				max_content_tokens
			)) {
				break;
			}
			continue;
		}

		if ((ch & 0x80u) != 0) {
			size_t char_len = embedding_utf8_char_len(ch);
			if (i + char_len > text_len) {
				char_len = 1;
			}

			if (!embedding_tokenize_char(
				handle,
				text + i,
				char_len,
				content_ids,
				&content_count,
				max_content_tokens
			)) {
				break;
			}
			i += char_len;
			continue;
		}

		if (!embedding_tokenize_char(
			handle,
			text + i,
			1,
			content_ids,
			&content_count,
			max_content_tokens
		)) {
			break;
		}
		i++;
	}

	pos = 0;
	input_ids[pos++] = handle->cls_id;
	for (i = 0; i < (size_t)content_count && pos < max_tokens - 1; ++i) {
		input_ids[pos++] = content_ids[i];
	}
	input_ids[pos++] = handle->sep_id;
	*out_seq_len = pos;

	for (i = 0; i < (size_t)max_tokens; ++i) {
		if ((int)i < *out_seq_len) {
			attention_mask[i] = 1;
		} else {
			input_ids[i] = handle->pad_id;
			attention_mask[i] = 0;
		}
		token_type_ids[i] = 0;
	}

	free(content_ids);
	return EMBEDDING_OK;
}

static EmbeddingErrorCode embedding_create_int64_tensor(
	EmbeddingHandle *handle,
	int64_t *data,
	int seq_len,
	OrtValue **value
) {
	OrtStatus *status;
	int64_t shape[2];

	shape[0] = 1;
	shape[1] = seq_len;

	status = handle->ort->CreateTensorWithDataAsOrtValue(
		handle->memory_info,
		data,
		(size_t)seq_len * sizeof(*data),
		shape,
		2,
		ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
		value
	);
	if (status) {
		return embedding_set_ort_error(
			handle,
			status,
			"OrtCreateTensor failed for int64 input",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
	}

	return EMBEDDING_OK;
}

static EmbeddingErrorCode embedding_create_int64_batch_tensor(
	EmbeddingHandle *handle,
	int64_t *data,
	size_t batch_size,
	int seq_len,
	OrtValue **value
) {
	OrtStatus *status;
	int64_t shape[2];

	if (batch_size == 0 || seq_len <= 0) {
		embedding_set_error(handle, "invalid batch tensor shape");
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	shape[0] = (int64_t)batch_size;
	shape[1] = seq_len;

	status = handle->ort->CreateTensorWithDataAsOrtValue(
		handle->memory_info,
		data,
		batch_size * (size_t)seq_len * sizeof(*data),
		shape,
		2,
		ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
		value
	);
	if (status) {
		return embedding_set_ort_error(
			handle,
			status,
			"OrtCreateTensor failed for int64 batch input",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
	}

	return EMBEDDING_OK;
}

static EmbeddingErrorCode embedding_copy_sentence_embedding(
	EmbeddingHandle *handle,
	OrtValue *value,
	float *output,
	int output_dim
) {
	OrtStatus *status;
	OrtTensorTypeAndShapeInfo *shape_info = NULL;
	ONNXTensorElementDataType element_type =
		ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
	size_t rank = 0;
	int64_t dims[2] = {0, 0};
	void *data = NULL;
	EmbeddingErrorCode result = EMBEDDING_OK;

	status = handle->ort->GetTensorTypeAndShape(value, &shape_info);
	if (status) {
		return embedding_set_ort_error(
			handle,
			status,
			"failed to get sentence_embedding tensor shape",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
	}

	status = handle->ort->GetTensorElementType(shape_info, &element_type);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			"failed to get sentence_embedding tensor type",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
		goto cleanup;
	}
	if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
		snprintf(
			handle->last_error,
			sizeof(handle->last_error),
			"output tensor type mismatch: expected float32 got %s(%d)",
			embedding_onnx_element_type_name(element_type),
			(int)element_type
		);
		result = EMBEDDING_ERR_DIM_MISMATCH;
		goto cleanup;
	}

	status = handle->ort->GetDimensionsCount(shape_info, &rank);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			"failed to get sentence_embedding rank",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
		goto cleanup;
	}
	if (rank != 2) {
		snprintf(
			handle->last_error,
			sizeof(handle->last_error),
			"output rank mismatch: expected 2 got %zu",
			rank
		);
		result = EMBEDDING_ERR_DIM_MISMATCH;
		goto cleanup;
	}

	status = handle->ort->GetDimensions(shape_info, dims, 2);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			"failed to get sentence_embedding dimensions",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
		goto cleanup;
	}
	if (dims[0] != 1 || dims[1] != output_dim ||
	    dims[1] != handle->embedding_dim) {
		snprintf(
			handle->last_error,
			sizeof(handle->last_error),
			"output dim mismatch: shape=[%lld,%lld] expected=[1,%d]",
			(long long)dims[0],
			(long long)dims[1],
			handle->embedding_dim
		);
		result = EMBEDDING_ERR_DIM_MISMATCH;
		goto cleanup;
	}

	status = handle->ort->GetTensorMutableData(value, &data);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			"failed to get sentence_embedding tensor data",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
		goto cleanup;
	}

	memcpy(output, data, (size_t)output_dim * sizeof(*output));

cleanup:
	if (shape_info) {
		handle->ort->ReleaseTensorTypeAndShapeInfo(shape_info);
	}
	return result;
}

static EmbeddingErrorCode embedding_copy_sentence_embedding_batch(
	EmbeddingHandle *handle,
	OrtValue *value,
	float *outputs,
	size_t batch_size,
	int output_dim
) {
	OrtStatus *status;
	OrtTensorTypeAndShapeInfo *shape_info = NULL;
	ONNXTensorElementDataType element_type =
		ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
	size_t rank = 0;
	int64_t dims[2] = {0, 0};
	void *data = NULL;
	EmbeddingErrorCode result = EMBEDDING_OK;

	status = handle->ort->GetTensorTypeAndShape(value, &shape_info);
	if (status) {
		return embedding_set_ort_error(
			handle,
			status,
			"failed to get batch sentence_embedding tensor shape",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
	}

	status = handle->ort->GetTensorElementType(shape_info, &element_type);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			"failed to get batch sentence_embedding tensor type",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
		goto cleanup;
	}
	if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
		snprintf(
			handle->last_error,
			sizeof(handle->last_error),
			"batch output tensor type mismatch: expected float32 got %s(%d)",
			embedding_onnx_element_type_name(element_type),
			(int)element_type
		);
		result = EMBEDDING_ERR_DIM_MISMATCH;
		goto cleanup;
	}

	status = handle->ort->GetDimensionsCount(shape_info, &rank);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			"failed to get batch sentence_embedding rank",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
		goto cleanup;
	}
	if (rank != 2) {
		snprintf(
			handle->last_error,
			sizeof(handle->last_error),
			"batch output rank mismatch: expected 2 got %zu",
			rank
		);
		result = EMBEDDING_ERR_DIM_MISMATCH;
		goto cleanup;
	}

	status = handle->ort->GetDimensions(shape_info, dims, 2);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			"failed to get batch sentence_embedding dimensions",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
		goto cleanup;
	}
	if (dims[0] != (int64_t)batch_size || dims[1] != output_dim ||
	    dims[1] != handle->embedding_dim) {
		snprintf(
			handle->last_error,
			sizeof(handle->last_error),
			"batch output dim mismatch: shape=[%lld,%lld] expected=[%zu,%d]",
			(long long)dims[0],
			(long long)dims[1],
			batch_size,
			handle->embedding_dim
		);
		result = EMBEDDING_ERR_DIM_MISMATCH;
		goto cleanup;
	}

	status = handle->ort->GetTensorMutableData(value, &data);
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			"failed to get batch sentence_embedding tensor data",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
		goto cleanup;
	}

	memcpy(outputs, data, batch_size * (size_t)output_dim * sizeof(*outputs));

cleanup:
	if (shape_info) {
		handle->ort->ReleaseTensorTypeAndShapeInfo(shape_info);
	}
	return result;
}

EmbeddingErrorCode embedding_create(
	const EmbeddingConfig *config,
	EmbeddingHandle **handle
) {
	EmbeddingHandle *created;
	OrtStatus *status;
	EmbeddingErrorCode inspect_result;
	EmbeddingErrorCode vocab_result;
	GraphOptimizationLevel graph_opt_level;
	ExecutionMode execution_mode;

	if (!config || !handle) {
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	*handle = NULL;

	if (!config->model_path || config->model_path[0] == '\0') {
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	if (config->embedding_dim <= 0) {
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	created = (EmbeddingHandle *)calloc(1, sizeof(*created));
	if (!created) {
		return EMBEDDING_ERR_OUT_OF_MEMORY;
	}

	created->embedding_dim = config->embedding_dim;
	created->num_threads = config->num_threads > 0 ? config->num_threads : 1;
	created->max_text_len = config->max_text_len > 0 ?
		config->max_text_len :
		EMBEDDING_ONNX_DEFAULT_MAX_TEXT_LEN;
	created->max_tokens = config->max_tokens > 0 ?
		config->max_tokens :
		EMBEDDING_ONNX_DEFAULT_MAX_TEXT_LEN;
	if (created->max_tokens > EMBEDDING_ONNX_MAX_TOKENS) {
		created->max_tokens = EMBEDDING_ONNX_MAX_TOKENS;
	}
	created->normalize = config->normalize ? 1 : 0;
	if (g_cfg.observability_embedding_profile_enabled) {
		created->profile_enabled = 1;
	}
	if (created->profile_enabled &&
	    pthread_mutex_init(&created->profile_mutex, NULL) == 0) {
		created->profile_mutex_initialized = 1;
	}
	created->model_path = embedding_strdup_or_null(config->model_path);
	if (!created->model_path) {
		if (created->profile_mutex_initialized) {
			pthread_mutex_destroy(&created->profile_mutex);
		}
		free(created);
		return EMBEDDING_ERR_OUT_OF_MEMORY;
	}
	created->vocab_path = config->vocab_path && config->vocab_path[0] != '\0' ?
		embedding_strdup_or_null(config->vocab_path) :
		embedding_join_dir_file(created->model_path, "vocab.txt");
	if (!created->vocab_path) {
		if (created->profile_mutex_initialized) {
			pthread_mutex_destroy(&created->profile_mutex);
		}
		free(created->model_path);
		free(created);
		return EMBEDDING_ERR_OUT_OF_MEMORY;
	}
	vocab_result = embedding_load_vocab(created);
	if (vocab_result != EMBEDDING_OK) {
		goto fail_before_ort;
	}

	created->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
	if (!created->ort) {
		embedding_set_error(created, "failed to get ONNX Runtime C API");
		goto fail_model_load;
	}

	if (embedding_acquire_shared_ort_env(created) != EMBEDDING_OK) {
		goto fail_model_load;
	}

	status = created->ort->CreateCpuMemoryInfo(
		OrtArenaAllocator,
		OrtMemTypeDefault,
		&created->memory_info
	);
	if (status) {
		embedding_set_ort_error(
			created,
			status,
			"failed to create ONNX Runtime CPU memory info",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
		goto fail_model_load;
	}

	status = created->ort->CreateSessionOptions(&created->session_options);
	if (status) {
		embedding_set_ort_error(
			created,
			status,
			"failed to create ONNX Runtime session options",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
		goto fail_model_load;
	}

	if (created->num_threads > 0) {
		status = created->ort->SetIntraOpNumThreads(
			created->session_options,
			created->num_threads
		);
		if (status) {
			embedding_set_ort_error(
				created,
				status,
				"failed to set ONNX Runtime intra op threads",
				EMBEDDING_ERR_MODEL_LOAD_FAILED
			);
			goto fail_model_load;
		}
	}

	if (g_cfg.embedding_inter_threads > 0) {
		status = created->ort->SetInterOpNumThreads(
			created->session_options,
			g_cfg.embedding_inter_threads
		);
		if (status) {
			embedding_set_ort_error(
				created,
				status,
				"failed to set ONNX Runtime inter op threads",
				EMBEDDING_ERR_MODEL_LOAD_FAILED
			);
			goto fail_model_load;
		}
	}

	if (!embedding_parse_execution_mode(
		g_cfg.embedding_ort_execution_mode,
		&execution_mode
	)) {
		snprintf(
			created->last_error,
			sizeof(created->last_error),
			"invalid embedding_ort_execution_mode: %s",
			g_cfg.embedding_ort_execution_mode
		);
		goto fail_invalid_config;
	}

	status = created->ort->SetSessionExecutionMode(
		created->session_options,
		execution_mode
	);
	if (status) {
		embedding_set_ort_error(
			created,
			status,
			"failed to set ONNX Runtime execution mode",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
		goto fail_model_load;
	}

	if (!embedding_parse_graph_opt_level(
		g_cfg.embedding_ort_graph_opt_level,
		&graph_opt_level
	)) {
		snprintf(
			created->last_error,
			sizeof(created->last_error),
			"invalid embedding_ort_graph_opt_level: %s",
			g_cfg.embedding_ort_graph_opt_level
		);
		goto fail_invalid_config;
	}

	status = created->ort->SetSessionGraphOptimizationLevel(
		created->session_options,
		graph_opt_level
	);
	if (status) {
		embedding_set_ort_error(
			created,
			status,
			"failed to enable ONNX Runtime graph optimization",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
		goto fail_model_load;
	}

	fprintf(
		stderr,
		"[kvlite][embedding][onnx] ort config: intra_threads=%d inter_threads=%d graph_opt=%s execution_mode=%s\n",
		created->num_threads,
		g_cfg.embedding_inter_threads,
		g_cfg.embedding_ort_graph_opt_level,
		g_cfg.embedding_ort_execution_mode
	);

	status = created->ort->CreateSession(
		created->env,
		created->model_path,
		created->session_options,
		&created->session
	);
	if (status) {
		embedding_set_ort_error(
			created,
			status,
			"failed to load ONNX embedding model",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
		goto fail_model_load;
	}
	embedding_resource_stats_add(
		&g_onnx_resource_stats.ort_session_created,
		1
	);

	inspect_result = embedding_inspect_onnx_model(created);
	if (inspect_result != EMBEDDING_OK) {
		goto fail_inspect;
	}
	if (created->model_output_dim <= 0 ||
	    created->model_output_dim != created->embedding_dim) {
		snprintf(
			created->last_error,
			sizeof(created->last_error),
			"ONNX output dimension=%d does not match configured dimension=%d",
			created->model_output_dim,
			created->embedding_dim
		);
		fprintf(stderr, "[kvlite][embedding][onnx] %s\n", created->last_error);
		inspect_result = EMBEDDING_ERR_DIM_MISMATCH;
		goto fail_inspect;
	}

	snprintf(
		created->last_error,
		sizeof(created->last_error),
		"ONNX session loaded: model=%s dimension=%d",
		created->model_path,
		created->embedding_dim
	);
	*handle = created;
	return EMBEDDING_OK;

fail_inspect:
	embedding_release_ort_resources(created);
	embedding_vocab_destroy(&created->vocab);
	if (created->profile_mutex_initialized) {
		pthread_mutex_destroy(&created->profile_mutex);
	}
	free(created->vocab_path);
	free(created->model_path);
	free(created);
	return inspect_result;

fail_model_load:
	embedding_release_ort_resources(created);
	embedding_vocab_destroy(&created->vocab);
	if (created->profile_mutex_initialized) {
		pthread_mutex_destroy(&created->profile_mutex);
	}
	free(created->vocab_path);
	free(created->model_path);
	free(created);
	return EMBEDDING_ERR_MODEL_LOAD_FAILED;

fail_invalid_config:
	embedding_release_ort_resources(created);
	embedding_vocab_destroy(&created->vocab);
	if (created->profile_mutex_initialized) {
		pthread_mutex_destroy(&created->profile_mutex);
	}
	free(created->vocab_path);
	free(created->model_path);
	free(created);
	return EMBEDDING_ERR_INVALID_ARGUMENT;

fail_before_ort:
	embedding_vocab_destroy(&created->vocab);
	if (created->profile_mutex_initialized) {
		pthread_mutex_destroy(&created->profile_mutex);
	}
	free(created->vocab_path);
	free(created->model_path);
	free(created);
	return vocab_result;
}

EmbeddingErrorCode embedding_encode(
	EmbeddingHandle *handle,
	const char *text,
	size_t text_len,
	float *output,
	int output_dim
) {
	int64_t *input_ids = NULL;
	int64_t *attention_mask = NULL;
	int64_t *token_type_ids = NULL;
	OrtValue *input_values[3] = {NULL, NULL, NULL};
	OrtValue *output_values[1] = {NULL};
	const char *input_names[3] = {
		"input_ids",
		"attention_mask",
		"token_type_ids"
	};
	const char *output_names[1] = {"sentence_embedding"};
	int seq_len = 0;
	OrtStatus *status;
	EmbeddingErrorCode result = EMBEDDING_OK;
	int profile_enabled;
	uint64_t total_start_us = 0;
	uint64_t tokenize_start_us = 0;
	uint64_t tensor_start_us = 0;
	uint64_t ort_start_us = 0;
	uint64_t output_start_us = 0;
	uint64_t tokenize_us = 0;
	uint64_t tensor_us = 0;
	uint64_t ort_us = 0;
	uint64_t output_us = 0;

	if (!handle) {
		return EMBEDDING_ERR_NOT_INITIALIZED;
	}

	profile_enabled = handle->profile_enabled;
	total_start_us = embedding_now_us();
	if (profile_enabled) {
		embedding_profile_enter(handle);
	}

	if (!text || text_len == 0 || !output) {
		embedding_set_error(handle, "invalid text or output buffer");
		result = EMBEDDING_ERR_INVALID_ARGUMENT;
		goto cleanup;
	}

	if (text_len > (size_t)handle->max_text_len) {
		embedding_set_error(handle, "text exceeds max_text_len");
		result = EMBEDDING_ERR_TEXT_TOO_LONG;
		goto cleanup;
	}

	if (output_dim != handle->embedding_dim) {
		embedding_set_error(handle, "output dimension mismatch");
		result = EMBEDDING_ERR_DIM_MISMATCH;
		goto cleanup;
	}

	input_ids = (int64_t *)calloc((size_t)handle->max_tokens, sizeof(*input_ids));
	attention_mask = (int64_t *)calloc(
		(size_t)handle->max_tokens,
		sizeof(*attention_mask)
	);
	token_type_ids = (int64_t *)calloc(
		(size_t)handle->max_tokens,
		sizeof(*token_type_ids)
	);
	if (!input_ids || !attention_mask || !token_type_ids) {
		embedding_set_error(handle, "failed to allocate ONNX input buffers");
		result = EMBEDDING_ERR_OUT_OF_MEMORY;
		goto cleanup;
	}

	tokenize_start_us = embedding_now_us();
	result = bert_tokenize(
		handle,
		text,
		text_len,
		input_ids,
		attention_mask,
		token_type_ids,
		handle->max_tokens,
		&seq_len
	);
	tokenize_us = embedding_elapsed_us(tokenize_start_us, embedding_now_us());
	if (result != EMBEDDING_OK) {
		embedding_set_error(handle, "tokenization failed");
		goto cleanup;
	}

	if (profile_enabled) {
		tensor_start_us = embedding_now_us();
	}
	result = embedding_create_int64_tensor(
		handle,
		input_ids,
		seq_len,
		&input_values[0]
	);
	if (result != EMBEDDING_OK) {
		goto cleanup;
	}

	result = embedding_create_int64_tensor(
		handle,
		attention_mask,
		seq_len,
		&input_values[1]
	);
	if (result != EMBEDDING_OK) {
		goto cleanup;
	}

	result = embedding_create_int64_tensor(
		handle,
		token_type_ids,
		seq_len,
		&input_values[2]
	);
	if (result != EMBEDDING_OK) {
		goto cleanup;
	}
	if (profile_enabled) {
		tensor_us = embedding_elapsed_us(tensor_start_us, embedding_now_us());
	}

	ort_start_us = embedding_now_us();
	status = handle->ort->Run(
		handle->session,
		NULL,
		input_names,
		(const OrtValue * const *)input_values,
		3,
		output_names,
		1,
		output_values
	);
	ort_us = embedding_elapsed_us(ort_start_us, embedding_now_us());
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			"OrtRun failed for sentence_embedding",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
		goto cleanup;
	}
	if (!output_values[0]) {
		embedding_set_error(handle, "sentence_embedding output missing");
		result = EMBEDDING_ERR_INFERENCE_FAILED;
		goto cleanup;
	}

	if (profile_enabled) {
		output_start_us = embedding_now_us();
	}
	result = embedding_copy_sentence_embedding(
		handle,
		output_values[0],
		output,
		output_dim
	);
	if (profile_enabled) {
		output_us = embedding_elapsed_us(output_start_us, embedding_now_us());
	}
	if (result == EMBEDDING_OK) {
		snprintf(handle->last_error, sizeof(handle->last_error), "%s", "OK");
	}

cleanup:
	if (total_start_us > 0) {
		uint64_t total_us = embedding_elapsed_us(total_start_us, embedding_now_us());

		embedding_resource_stats_add(
			&g_onnx_resource_stats.encode_count,
			1
		);
		embedding_resource_stats_add(
			&g_onnx_resource_stats.embedding_encode_total_us,
			total_us
		);
		embedding_resource_stats_update_max(
			&g_onnx_resource_stats.embedding_encode_max_us,
			total_us
		);
		embedding_resource_stats_add(
			&g_onnx_resource_stats.tokenizer_total_us,
			tokenize_us
		);
		embedding_resource_stats_update_max(
			&g_onnx_resource_stats.tokenizer_max_us,
			tokenize_us
		);
		embedding_resource_stats_add(
			&g_onnx_resource_stats.ort_run_total_us,
			ort_us
		);
		embedding_resource_stats_update_max(
			&g_onnx_resource_stats.ort_run_max_us,
			ort_us
		);
	}
	if (profile_enabled) {
		embedding_profile_record(
			handle,
			result == EMBEDDING_OK,
			embedding_elapsed_us(total_start_us, embedding_now_us()),
			tokenize_us,
			tensor_us,
			ort_us,
			output_us
		);
		embedding_profile_leave(handle);
	}
	if (output_values[0]) {
		handle->ort->ReleaseValue(output_values[0]);
	}
	if (input_values[2]) {
		handle->ort->ReleaseValue(input_values[2]);
	}
	if (input_values[1]) {
		handle->ort->ReleaseValue(input_values[1]);
	}
	if (input_values[0]) {
		handle->ort->ReleaseValue(input_values[0]);
	}
	free(token_type_ids);
	free(attention_mask);
	free(input_ids);
	return result;
}

EmbeddingErrorCode embedding_encode_batch(
	EmbeddingHandle *handle,
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	float *outputs,
	int output_dim
) {
	int64_t *input_ids = NULL;
	int64_t *attention_mask = NULL;
	int64_t *token_type_ids = NULL;
	OrtValue *input_values[3] = {NULL, NULL, NULL};
	OrtValue *output_values[1] = {NULL};
	const char *input_names[3] = {
		"input_ids",
		"attention_mask",
		"token_type_ids"
	};
	const char *output_names[1] = {"sentence_embedding"};
	OrtStatus *status;
	EmbeddingErrorCode result = EMBEDDING_OK;
	int profile_enabled;
	int seq_len = 0;
	size_t i;
	size_t token_count;
	uint64_t total_start_us = 0;
	uint64_t tokenize_start_us = 0;
	uint64_t tensor_start_us = 0;
	uint64_t ort_start_us = 0;
	uint64_t output_start_us = 0;
	uint64_t tokenize_us = 0;
	uint64_t tensor_us = 0;
	uint64_t ort_us = 0;
	uint64_t output_us = 0;

	if (!handle) {
		return EMBEDDING_ERR_NOT_INITIALIZED;
	}

	profile_enabled = handle->profile_enabled;
	total_start_us = embedding_now_us();
	if (profile_enabled) {
		embedding_profile_enter(handle);
	}

	if (!texts || !text_lens || text_count == 0 || !outputs) {
		embedding_set_error(handle, "invalid batch input or output buffer");
		result = EMBEDDING_ERR_INVALID_ARGUMENT;
		goto cleanup;
	}

	if (output_dim != handle->embedding_dim) {
		embedding_set_error(handle, "batch output dimension mismatch");
		result = EMBEDDING_ERR_DIM_MISMATCH;
		goto cleanup;
	}

	if (text_count > ((size_t)-1 / (size_t)handle->max_tokens)) {
		embedding_set_error(handle, "batch size overflow");
		result = EMBEDDING_ERR_INVALID_ARGUMENT;
		goto cleanup;
	}
	token_count = text_count * (size_t)handle->max_tokens;

	input_ids = (int64_t *)calloc(token_count, sizeof(*input_ids));
	attention_mask = (int64_t *)calloc(token_count, sizeof(*attention_mask));
	token_type_ids = (int64_t *)calloc(token_count, sizeof(*token_type_ids));
	if (!input_ids || !attention_mask || !token_type_ids) {
		embedding_set_error(handle, "failed to allocate ONNX batch input buffers");
		result = EMBEDDING_ERR_OUT_OF_MEMORY;
		goto cleanup;
	}

	tokenize_start_us = embedding_now_us();
	for (i = 0; i < text_count; ++i) {
		int item_seq_len = 0;

		if (!texts[i] || text_lens[i] == 0) {
			embedding_set_error(handle, "invalid batch text");
			result = EMBEDDING_ERR_INVALID_ARGUMENT;
			goto cleanup;
		}
		if (text_lens[i] > (size_t)handle->max_text_len) {
			embedding_set_error(handle, "batch text exceeds max_text_len");
			result = EMBEDDING_ERR_TEXT_TOO_LONG;
			goto cleanup;
		}

		result = bert_tokenize(
			handle,
			texts[i],
			text_lens[i],
			input_ids + (i * (size_t)handle->max_tokens),
			attention_mask + (i * (size_t)handle->max_tokens),
			token_type_ids + (i * (size_t)handle->max_tokens),
			handle->max_tokens,
			&item_seq_len
		);
		if (result != EMBEDDING_OK) {
			embedding_set_error(handle, "batch tokenization failed");
			goto cleanup;
		}
		if (item_seq_len > seq_len) {
			seq_len = item_seq_len;
		}
	}
	tokenize_us = embedding_elapsed_us(tokenize_start_us, embedding_now_us());
	if (seq_len <= 0) {
		embedding_set_error(handle, "invalid batch token sequence length");
		result = EMBEDDING_ERR_INVALID_ARGUMENT;
		goto cleanup;
	}

	if (seq_len < handle->max_tokens) {
		for (i = 0; i < text_count; ++i) {
			size_t dst = i * (size_t)seq_len;
			size_t src = i * (size_t)handle->max_tokens;
			size_t bytes = (size_t)seq_len * sizeof(*input_ids);

			memmove(input_ids + dst, input_ids + src, bytes);
			memmove(attention_mask + dst, attention_mask + src, bytes);
			memmove(token_type_ids + dst, token_type_ids + src, bytes);
		}
	}

	if (profile_enabled) {
		tensor_start_us = embedding_now_us();
	}
	result = embedding_create_int64_batch_tensor(
		handle,
		input_ids,
		text_count,
		seq_len,
		&input_values[0]
	);
	if (result != EMBEDDING_OK) {
		goto cleanup;
	}
	result = embedding_create_int64_batch_tensor(
		handle,
		attention_mask,
		text_count,
		seq_len,
		&input_values[1]
	);
	if (result != EMBEDDING_OK) {
		goto cleanup;
	}
	result = embedding_create_int64_batch_tensor(
		handle,
		token_type_ids,
		text_count,
		seq_len,
		&input_values[2]
	);
	if (result != EMBEDDING_OK) {
		goto cleanup;
	}
	if (profile_enabled) {
		tensor_us = embedding_elapsed_us(tensor_start_us, embedding_now_us());
	}

	ort_start_us = embedding_now_us();
	status = handle->ort->Run(
		handle->session,
		NULL,
		input_names,
		(const OrtValue * const *)input_values,
		3,
		output_names,
		1,
		output_values
	);
	ort_us = embedding_elapsed_us(ort_start_us, embedding_now_us());
	if (status) {
		result = embedding_set_ort_error(
			handle,
			status,
			"OrtRun failed for batch sentence_embedding",
			EMBEDDING_ERR_INFERENCE_FAILED
		);
		goto cleanup;
	}
	if (!output_values[0]) {
		embedding_set_error(handle, "batch sentence_embedding output missing");
		result = EMBEDDING_ERR_INFERENCE_FAILED;
		goto cleanup;
	}

	if (profile_enabled) {
		output_start_us = embedding_now_us();
	}
	result = embedding_copy_sentence_embedding_batch(
		handle,
		output_values[0],
		outputs,
		text_count,
		output_dim
	);
	if (profile_enabled) {
		output_us = embedding_elapsed_us(output_start_us, embedding_now_us());
	}
	if (result == EMBEDDING_OK) {
		snprintf(handle->last_error, sizeof(handle->last_error), "%s", "OK");
	}

cleanup:
	if (total_start_us > 0) {
		uint64_t total_us = embedding_elapsed_us(total_start_us, embedding_now_us());

		embedding_resource_stats_add(
			&g_onnx_resource_stats.encode_count,
			text_count
		);
		embedding_resource_stats_add(
			&g_onnx_resource_stats.embedding_encode_total_us,
			total_us
		);
		embedding_resource_stats_update_max(
			&g_onnx_resource_stats.embedding_encode_max_us,
			total_us
		);
		embedding_resource_stats_add(
			&g_onnx_resource_stats.tokenizer_total_us,
			tokenize_us
		);
		embedding_resource_stats_update_max(
			&g_onnx_resource_stats.tokenizer_max_us,
			tokenize_us
		);
		embedding_resource_stats_add(
			&g_onnx_resource_stats.ort_run_total_us,
			ort_us
		);
		embedding_resource_stats_update_max(
			&g_onnx_resource_stats.ort_run_max_us,
			ort_us
		);
	}
	if (profile_enabled) {
		embedding_profile_record(
			handle,
			result == EMBEDDING_OK,
			embedding_elapsed_us(total_start_us, embedding_now_us()),
			tokenize_us,
			tensor_us,
			ort_us,
			output_us
		);
		embedding_profile_leave(handle);
	}
	if (output_values[0]) {
		handle->ort->ReleaseValue(output_values[0]);
	}
	if (input_values[2]) {
		handle->ort->ReleaseValue(input_values[2]);
	}
	if (input_values[1]) {
		handle->ort->ReleaseValue(input_values[1]);
	}
	if (input_values[0]) {
		handle->ort->ReleaseValue(input_values[0]);
	}
	free(token_type_ids);
	free(attention_mask);
	free(input_ids);
	return result;
}

int embedding_dimension(EmbeddingHandle *handle) {
	if (!handle) {
		return 0;
	}

	return handle->embedding_dim;
}

const char *embedding_last_error(EmbeddingHandle *handle) {
	if (!handle) {
		return "embedding handle is not initialized";
	}

	return handle->last_error;
}

void embedding_runtime_get_resource_stats(
	EmbeddingRuntimeResourceStats *out
) {
	if (!out) {
		return;
	}

	out->shared_ort_env_enabled = embedding_resource_stats_load(
		&g_onnx_resource_stats.shared_ort_env_enabled
	);
	out->shared_ort_env_created = embedding_resource_stats_load(
		&g_onnx_resource_stats.shared_ort_env_created
	);
	out->shared_ort_env_destroyed = embedding_resource_stats_load(
		&g_onnx_resource_stats.shared_ort_env_destroyed
	);
	out->shared_ort_env_refcount_current = embedding_resource_stats_load(
		&g_onnx_resource_stats.shared_ort_env_refcount_current
	);
	out->shared_ort_env_refcount_max = embedding_resource_stats_load(
		&g_onnx_resource_stats.shared_ort_env_refcount_max
	);
	out->ort_session_created = embedding_resource_stats_load(
		&g_onnx_resource_stats.ort_session_created
	);
	out->ort_session_destroyed = embedding_resource_stats_load(
		&g_onnx_resource_stats.ort_session_destroyed
	);
	out->encode_count = embedding_resource_stats_load(
		&g_onnx_resource_stats.encode_count
	);
	out->embedding_encode_total_us = embedding_resource_stats_load(
		&g_onnx_resource_stats.embedding_encode_total_us
	);
	out->embedding_encode_max_us = embedding_resource_stats_load(
		&g_onnx_resource_stats.embedding_encode_max_us
	);
	out->tokenizer_total_us = embedding_resource_stats_load(
		&g_onnx_resource_stats.tokenizer_total_us
	);
	out->tokenizer_max_us = embedding_resource_stats_load(
		&g_onnx_resource_stats.tokenizer_max_us
	);
	out->ort_run_total_us = embedding_resource_stats_load(
		&g_onnx_resource_stats.ort_run_total_us
	);
	out->ort_run_max_us = embedding_resource_stats_load(
		&g_onnx_resource_stats.ort_run_max_us
	);
}

int embedding_runtime_supports_onnx(void) {
	return 1;
}

void embedding_destroy(EmbeddingHandle *handle) {
	if (!handle) {
		return;
	}

	embedding_release_ort_resources(handle);
	embedding_vocab_destroy(&handle->vocab);
	if (handle->profile_mutex_initialized) {
		pthread_mutex_destroy(&handle->profile_mutex);
	}
	free(handle->vocab_path);
	free(handle->model_path);
	free(handle);
}
