#include "embedding_c_api.h"

#include <onnxruntime_c_api.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMBEDDING_ONNX_DEFAULT_MAX_TEXT_LEN 512
#define EMBEDDING_ONNX_MAX_ERROR 512
#define EMBEDDING_ONNX_MAX_TOKENS 512
#define EMBEDDING_ONNX_WORDPIECE_MAX_CHARS 256

typedef struct EmbeddingVocab {
	char **tokens;
	size_t count;
	size_t capacity;
} EmbeddingVocab;

struct EmbeddingHandle {
	int embedding_dim;
	int num_threads;
	int max_text_len;
	int max_tokens;
	int normalize;
	char *model_path;
	char *vocab_path;
	char last_error[EMBEDDING_ONNX_MAX_ERROR];

	const OrtApi *ort;
	OrtEnv *env;
	OrtSessionOptions *session_options;
	OrtSession *session;
	OrtMemoryInfo *memory_info;

	EmbeddingVocab vocab;
	int64_t pad_id;
	int64_t unk_id;
	int64_t cls_id;
	int64_t sep_id;
};

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

	for (i = 0; i < vocab->count; ++i) {
		free(vocab->tokens[i]);
	}
	free(vocab->tokens);
	vocab->tokens = NULL;
	vocab->count = 0;
	vocab->capacity = 0;
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

static int64_t embedding_vocab_lookup(
	const EmbeddingVocab *vocab,
	const char *token
) {
	size_t i;

	if (!vocab || !token) {
		return -1;
	}

	/* Linear lookup is enough for the first ONNX smoke-test path. */
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

static void embedding_release_ort_resources(EmbeddingHandle *handle) {
	if (!handle || !handle->ort) {
		return;
	}

	if (handle->session) {
		handle->ort->ReleaseSession(handle->session);
		handle->session = NULL;
	}

	if (handle->session_options) {
		handle->ort->ReleaseSessionOptions(handle->session_options);
		handle->session_options = NULL;
	}

	if (handle->memory_info) {
		handle->ort->ReleaseMemoryInfo(handle->memory_info);
		handle->memory_info = NULL;
	}

	if (handle->env) {
		handle->ort->ReleaseEnv(handle->env);
		handle->env = NULL;
	}
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

EmbeddingErrorCode embedding_create(
	const EmbeddingConfig *config,
	EmbeddingHandle **handle
) {
	EmbeddingHandle *created;
	OrtStatus *status;
	EmbeddingErrorCode inspect_result;
	EmbeddingErrorCode vocab_result;

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
	created->max_tokens = created->max_text_len > 0 ?
		created->max_text_len :
		EMBEDDING_ONNX_DEFAULT_MAX_TEXT_LEN;
	if (created->max_tokens > EMBEDDING_ONNX_MAX_TOKENS) {
		created->max_tokens = EMBEDDING_ONNX_MAX_TOKENS;
	}
	created->normalize = config->normalize ? 1 : 0;
	created->model_path = embedding_strdup_or_null(config->model_path);
	if (!created->model_path) {
		free(created);
		return EMBEDDING_ERR_OUT_OF_MEMORY;
	}
	created->vocab_path = embedding_join_dir_file(created->model_path, "vocab.txt");
	if (!created->vocab_path) {
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

	status = created->ort->CreateEnv(
		ORT_LOGGING_LEVEL_WARNING,
		"kvlite_embedding",
		&created->env
	);
	if (status) {
		embedding_set_ort_error(
			created,
			status,
			"failed to create ONNX Runtime environment",
			EMBEDDING_ERR_MODEL_LOAD_FAILED
		);
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

	status = created->ort->SetSessionGraphOptimizationLevel(
		created->session_options,
		ORT_ENABLE_BASIC
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

	inspect_result = embedding_inspect_onnx_model(created);
	if (inspect_result != EMBEDDING_OK) {
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
	free(created->vocab_path);
	free(created->model_path);
	free(created);
	return inspect_result;

fail_model_load:
	embedding_release_ort_resources(created);
	embedding_vocab_destroy(&created->vocab);
	free(created->vocab_path);
	free(created->model_path);
	free(created);
	return EMBEDDING_ERR_MODEL_LOAD_FAILED;

fail_before_ort:
	embedding_vocab_destroy(&created->vocab);
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

	if (!handle) {
		return EMBEDDING_ERR_NOT_INITIALIZED;
	}

	if (!text || text_len == 0 || !output) {
		embedding_set_error(handle, "invalid text or output buffer");
		return EMBEDDING_ERR_INVALID_ARGUMENT;
	}

	if (text_len > (size_t)handle->max_text_len) {
		embedding_set_error(handle, "text exceeds max_text_len");
		return EMBEDDING_ERR_TEXT_TOO_LONG;
	}

	if (output_dim != handle->embedding_dim) {
		embedding_set_error(handle, "output dimension mismatch");
		return EMBEDDING_ERR_DIM_MISMATCH;
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
	if (result != EMBEDDING_OK) {
		embedding_set_error(handle, "tokenization failed");
		goto cleanup;
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

	result = embedding_copy_sentence_embedding(
		handle,
		output_values[0],
		output,
		output_dim
	);
	if (result == EMBEDDING_OK) {
		snprintf(handle->last_error, sizeof(handle->last_error), "%s", "OK");
	}

cleanup:
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

void embedding_destroy(EmbeddingHandle *handle) {
	if (!handle) {
		return;
	}

	embedding_release_ort_resources(handle);
	embedding_vocab_destroy(&handle->vocab);
	free(handle->vocab_path);
	free(handle->model_path);
	free(handle);
}
