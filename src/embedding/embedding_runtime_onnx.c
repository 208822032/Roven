#include "embedding_c_api.h"

#include <onnxruntime_c_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMBEDDING_ONNX_DEFAULT_MAX_TEXT_LEN 512
#define EMBEDDING_ONNX_MAX_ERROR 512

struct EmbeddingHandle {
	int embedding_dim;
	int num_threads;
	int max_text_len;
	int normalize;
	char *model_path;
	char last_error[EMBEDDING_ONNX_MAX_ERROR];

	const OrtApi *ort;
	OrtEnv *env;
	OrtSessionOptions *session_options;
	OrtSession *session;
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

	if (handle->env) {
		handle->ort->ReleaseEnv(handle->env);
		handle->env = NULL;
	}
}

EmbeddingErrorCode embedding_create(
	const EmbeddingConfig *config,
	EmbeddingHandle **handle
) {
	EmbeddingHandle *created;
	OrtStatus *status;

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
	created->normalize = config->normalize ? 1 : 0;
	created->model_path = embedding_strdup_or_null(config->model_path);
	if (!created->model_path) {
		free(created);
		return EMBEDDING_ERR_OUT_OF_MEMORY;
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

	snprintf(
		created->last_error,
		sizeof(created->last_error),
		"ONNX session loaded: model=%s dimension=%d",
		created->model_path,
		created->embedding_dim
	);
	*handle = created;
	return EMBEDDING_OK;

fail_model_load:
	embedding_release_ort_resources(created);
	free(created->model_path);
	free(created);
	return EMBEDDING_ERR_MODEL_LOAD_FAILED;
}

EmbeddingErrorCode embedding_encode(
	EmbeddingHandle *handle,
	const char *text,
	size_t text_len,
	float *output,
	int output_dim
) {
	(void)text;
	(void)text_len;
	(void)output;
	(void)output_dim;

	if (!handle) {
		return EMBEDDING_ERR_NOT_INITIALIZED;
	}

	embedding_set_error(
		handle,
		"ONNX tokenizer/inference is not implemented yet"
	);
	return EMBEDDING_ERR_INFERENCE_FAILED;
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
	free(handle->model_path);
	free(handle);
}
