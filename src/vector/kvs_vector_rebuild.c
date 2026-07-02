#include "vector/kvs_vector_rebuild.h"

#include "kvs_memory.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int kvs_vector_rebuild_is_active(kvs_vector_rebuild_state_t state) {
	return state == KVS_VECTOR_REBUILD_BUILDING ||
	       state == KVS_VECTOR_REBUILD_REPLAYING_DELTA ||
	       state == KVS_VECTOR_REBUILD_READY_TO_SWAP ||
	       state == KVS_VECTOR_REBUILD_SWAPPING;
}

static void kvs_vector_rebuild_clear_error(kvs_vector_rebuild_context_t *ctx) {
	ctx->error_code = 0;
	ctx->error_message[0] = '\0';
}

static void kvs_vector_rebuild_set_error(
	kvs_vector_rebuild_context_t *ctx,
	int error_code,
	const char *error_message
) {
	ctx->error_code = error_code;
	if (error_message && error_message[0]) {
		snprintf(ctx->error_message, sizeof(ctx->error_message), "%s", error_message);
	} else {
		ctx->error_message[0] = '\0';
	}
}

static void kvs_vector_rebuild_destroy_shadow(kvs_vector_rebuild_context_t *ctx) {
	if (!ctx) {
		return;
	}

	if (ctx->shadow_index && ctx->shadow_destroy_fn) {
		ctx->shadow_destroy_fn(ctx->shadow_index);
	}
	ctx->shadow_index = NULL;
	ctx->shadow_destroy_fn = NULL;
}

static void kvs_vector_rebuild_destroy_active(kvs_vector_rebuild_context_t *ctx) {
	if (!ctx) {
		return;
	}

	if (ctx->active_index && ctx->active_destroy_fn) {
		ctx->active_destroy_fn(ctx->active_index);
	}
	ctx->active_index = NULL;
	ctx->active_destroy_fn = NULL;
}

static void kvs_vector_rebuild_destroy_retired(kvs_vector_rebuild_context_t *ctx) {
	if (!ctx) {
		return;
	}

	if (ctx->retired_index && ctx->retired_destroy_fn) {
		ctx->retired_destroy_fn(ctx->retired_index);
	}
	ctx->retired_index = NULL;
	ctx->retired_destroy_fn = NULL;
}

static int kvs_vector_rebuild_reserve_delta(
	kvs_vector_rebuild_context_t *ctx,
	size_t min_capacity
) {
	kvs_vector_rebuild_delta_entry_t *new_entries;
	size_t new_capacity;
	size_t bytes;

	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (ctx->delta_capacity >= min_capacity) {
		return KVS_VECTOR_OK;
	}

	new_capacity = ctx->delta_capacity ? ctx->delta_capacity * 2 : 16;
	while (new_capacity < min_capacity) {
		if (new_capacity > SIZE_MAX / 2) {
			return KVS_VECTOR_ERR;
		}
		new_capacity *= 2;
	}
	if (new_capacity > SIZE_MAX / sizeof(*new_entries)) {
		return KVS_VECTOR_ERR;
	}

	bytes = new_capacity * sizeof(*new_entries);
	new_entries = (kvs_vector_rebuild_delta_entry_t *)kvs_malloc(bytes);
	if (!new_entries) {
		return KVS_VECTOR_ERR;
	}
	memset(new_entries, 0, bytes);

	if (ctx->delta_log && ctx->delta_count > 0) {
		memcpy(
			new_entries,
			ctx->delta_log,
			ctx->delta_count * sizeof(*new_entries)
		);
	}
	if (ctx->delta_log) {
		kvs_free(ctx->delta_log);
	}

	ctx->delta_log = new_entries;
	ctx->delta_capacity = new_capacity;
	return KVS_VECTOR_OK;
}

static int kvs_vector_rebuild_append_delta(
	kvs_vector_rebuild_context_t *ctx,
	kvs_vector_rebuild_delta_op_t op,
	uint64_t record_id,
	const float *vector,
	size_t dim
) {
	kvs_vector_rebuild_delta_entry_t entry;
	float *vector_copy = NULL;
	uint64_t next_generation;
	int ret;

	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (op == KVS_VECTOR_REBUILD_DELTA_UPSERT) {
		size_t bytes;
		if (!vector || dim == 0 || dim > SIZE_MAX / sizeof(float)) {
			return KVS_VECTOR_ERR_INVALID_ARG;
		}
		bytes = dim * sizeof(float);
		vector_copy = (float *)kvs_malloc(bytes);
		if (!vector_copy) {
			return KVS_VECTOR_ERR;
		}
		memcpy(vector_copy, vector, bytes);
	} else if (op == KVS_VECTOR_REBUILD_DELTA_DELETE) {
		if (vector || dim != 0) {
			return KVS_VECTOR_ERR_INVALID_ARG;
		}
	} else {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	if (ctx->current_generation == UINT64_MAX) {
		if (vector_copy) {
			kvs_free(vector_copy);
		}
		return KVS_VECTOR_ERR;
	}
	next_generation = ctx->current_generation + 1;

	ret = kvs_vector_rebuild_reserve_delta(ctx, ctx->delta_count + 1);
	if (ret != KVS_VECTOR_OK) {
		if (vector_copy) {
			kvs_free(vector_copy);
		}
		return ret;
	}

	memset(&entry, 0, sizeof(entry));
	entry.generation = next_generation;
	entry.op = op;
	entry.record_id = record_id;
	entry.vector = vector_copy;
	entry.dim = dim;

	ctx->delta_log[ctx->delta_count] = entry;
	ctx->delta_count++;
	ctx->current_generation = next_generation;
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_context_init(kvs_vector_rebuild_context_t *ctx) {
	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->state = KVS_VECTOR_REBUILD_IDLE;
	return KVS_VECTOR_OK;
}

void kvs_vector_rebuild_context_destroy(kvs_vector_rebuild_context_t *ctx) {
	if (!ctx) {
		return;
	}

	kvs_vector_rebuild_destroy_shadow(ctx);
	kvs_vector_rebuild_destroy_retired(ctx);
	kvs_vector_rebuild_destroy_active(ctx);
	kvs_vector_rebuild_delta_clear(ctx);
	memset(ctx, 0, sizeof(*ctx));
	ctx->state = KVS_VECTOR_REBUILD_IDLE;
}

void kvs_vector_rebuild_delta_clear(kvs_vector_rebuild_context_t *ctx) {
	size_t i;

	if (!ctx) {
		return;
	}

	for (i = 0; i < ctx->delta_count; i++) {
		if (ctx->delta_log[i].vector) {
			kvs_free(ctx->delta_log[i].vector);
			ctx->delta_log[i].vector = NULL;
		}
	}

	if (ctx->delta_log) {
		kvs_free(ctx->delta_log);
		ctx->delta_log = NULL;
	}
	ctx->delta_count = 0;
	ctx->delta_capacity = 0;
}

int kvs_vector_rebuild_append_upsert_delta(
	kvs_vector_rebuild_context_t *ctx,
	uint64_t record_id,
	const float *vector,
	size_t dim
) {
	return kvs_vector_rebuild_append_delta(
		ctx,
		KVS_VECTOR_REBUILD_DELTA_UPSERT,
		record_id,
		vector,
		dim
	);
}

int kvs_vector_rebuild_append_delete_delta(
	kvs_vector_rebuild_context_t *ctx,
	uint64_t record_id
) {
	return kvs_vector_rebuild_append_delta(
		ctx,
		KVS_VECTOR_REBUILD_DELTA_DELETE,
		record_id,
		NULL,
		0
	);
}

size_t kvs_vector_rebuild_delta_count(
	const kvs_vector_rebuild_context_t *ctx
) {
	return ctx ? ctx->delta_count : 0;
}

bool kvs_vector_rebuild_is_capture_active(
	const kvs_vector_rebuild_context_t *ctx
) {
	if (!ctx) {
		return false;
	}

	return ctx->state == KVS_VECTOR_REBUILD_BUILDING ||
	       ctx->state == KVS_VECTOR_REBUILD_REPLAYING_DELTA ||
	       ctx->state == KVS_VECTOR_REBUILD_READY_TO_SWAP;
}

int kvs_vector_rebuild_begin(
	kvs_vector_rebuild_context_t *ctx,
	void *active_index,
	void *shadow_index
) {
	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (ctx->state != KVS_VECTOR_REBUILD_IDLE) {
		return KVS_VECTOR_ERR;
	}
	if (ctx->current_generation == UINT64_MAX) {
		return KVS_VECTOR_ERR;
	}

	kvs_vector_rebuild_destroy_retired(ctx);
	kvs_vector_rebuild_destroy_active(ctx);
	kvs_vector_rebuild_clear_error(ctx);
	ctx->cancel_requested = 0;
	ctx->active_index = active_index;
	ctx->active_destroy_fn = NULL;
	ctx->shadow_index = shadow_index;
	ctx->shadow_destroy_fn = NULL;
	ctx->retired_destroy_fn = NULL;
	ctx->retired_index = NULL;
	ctx->progress_total = 0;
	ctx->progress_done = 0;
	ctx->rebuild_generation = ++ctx->current_generation;
	ctx->state = KVS_VECTOR_REBUILD_BUILDING;
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_enter_replaying_delta(
	kvs_vector_rebuild_context_t *ctx
) {
	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (ctx->state != KVS_VECTOR_REBUILD_BUILDING) {
		return KVS_VECTOR_ERR;
	}
	ctx->state = KVS_VECTOR_REBUILD_REPLAYING_DELTA;
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_enter_ready_to_swap(
	kvs_vector_rebuild_context_t *ctx
) {
	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (ctx->state != KVS_VECTOR_REBUILD_REPLAYING_DELTA) {
		return KVS_VECTOR_ERR;
	}
	ctx->state = KVS_VECTOR_REBUILD_READY_TO_SWAP;
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_enter_swapping(
	kvs_vector_rebuild_context_t *ctx
) {
	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (ctx->state != KVS_VECTOR_REBUILD_READY_TO_SWAP) {
		return KVS_VECTOR_ERR;
	}
	ctx->state = KVS_VECTOR_REBUILD_SWAPPING;
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_finish_swap(
	kvs_vector_rebuild_context_t *ctx,
	void *new_active_index,
	void *retired_index
) {
	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (ctx->state != KVS_VECTOR_REBUILD_SWAPPING) {
		return KVS_VECTOR_ERR;
	}

	ctx->active_index = new_active_index;
	ctx->active_destroy_fn = NULL;
	ctx->retired_index = retired_index;
	ctx->retired_destroy_fn = NULL;
	ctx->shadow_index = NULL;
	ctx->shadow_destroy_fn = NULL;
	ctx->state = KVS_VECTOR_REBUILD_IDLE;
	ctx->cancel_requested = 0;
	ctx->progress_total = 0;
	ctx->progress_done = 0;
	kvs_vector_rebuild_clear_error(ctx);
	kvs_vector_rebuild_delta_clear(ctx);
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_promote_shadow(
	kvs_vector_rebuild_context_t *ctx
) {
	void *old_active;
	kvs_vector_rebuild_index_destroy_fn old_active_destroy;

	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (ctx->state != KVS_VECTOR_REBUILD_READY_TO_SWAP) {
		return KVS_VECTOR_ERR;
	}
	if (!ctx->shadow_index) {
		return KVS_VECTOR_ERR_NOT_INITIALIZED;
	}
	if (ctx->retired_index) {
		return KVS_VECTOR_ERR;
	}

	ctx->state = KVS_VECTOR_REBUILD_SWAPPING;

	old_active = ctx->active_index;
	old_active_destroy = ctx->active_destroy_fn;
	ctx->active_index = ctx->shadow_index;
	ctx->active_destroy_fn = ctx->shadow_destroy_fn;
	ctx->shadow_index = NULL;
	ctx->shadow_destroy_fn = NULL;
	ctx->retired_index = old_active;
	ctx->retired_destroy_fn = old_active_destroy;
	ctx->state = KVS_VECTOR_REBUILD_IDLE;
	ctx->cancel_requested = 0;
	ctx->progress_total = 0;
	ctx->progress_done = 0;
	kvs_vector_rebuild_clear_error(ctx);
	kvs_vector_rebuild_delta_clear(ctx);
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_retire_cleanup(
	kvs_vector_rebuild_context_t *ctx
) {
	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}

	kvs_vector_rebuild_destroy_retired(ctx);
	ctx->retired_index = NULL;
	ctx->retired_destroy_fn = NULL;
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_fail(
	kvs_vector_rebuild_context_t *ctx,
	int error_code,
	const char *error_message
) {
	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (!kvs_vector_rebuild_is_active(ctx->state)) {
		return KVS_VECTOR_ERR;
	}

	ctx->state = KVS_VECTOR_REBUILD_FAILED;
	ctx->cancel_requested = 0;
	kvs_vector_rebuild_destroy_shadow(ctx);
	kvs_vector_rebuild_set_error(ctx, error_code, error_message);
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_cancel(
	kvs_vector_rebuild_context_t *ctx
) {
	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (!kvs_vector_rebuild_is_active(ctx->state)) {
		return KVS_VECTOR_ERR;
	}

	ctx->state = KVS_VECTOR_REBUILD_CANCELLED;
	ctx->cancel_requested = 1;
	kvs_vector_rebuild_destroy_shadow(ctx);
	return KVS_VECTOR_OK;
}

int kvs_vector_rebuild_reset_idle(
	kvs_vector_rebuild_context_t *ctx
) {
	if (!ctx) {
		return KVS_VECTOR_ERR_INVALID_ARG;
	}
	if (kvs_vector_rebuild_is_active(ctx->state)) {
		return KVS_VECTOR_ERR;
	}

	kvs_vector_rebuild_delta_clear(ctx);
	ctx->state = KVS_VECTOR_REBUILD_IDLE;
	ctx->rebuild_generation = 0;
	kvs_vector_rebuild_destroy_shadow(ctx);
	ctx->shadow_index = NULL;
	ctx->shadow_destroy_fn = NULL;
	kvs_vector_rebuild_destroy_retired(ctx);
	ctx->retired_index = NULL;
	ctx->retired_destroy_fn = NULL;
	kvs_vector_rebuild_destroy_active(ctx);
	ctx->active_index = NULL;
	ctx->active_destroy_fn = NULL;
	ctx->progress_total = 0;
	ctx->progress_done = 0;
	ctx->cancel_requested = 0;
	kvs_vector_rebuild_clear_error(ctx);
	return KVS_VECTOR_OK;
}

const char *kvs_vector_rebuild_state_name(kvs_vector_rebuild_state_t state) {
	switch (state) {
	case KVS_VECTOR_REBUILD_IDLE:
		return "idle";
	case KVS_VECTOR_REBUILD_BUILDING:
		return "building";
	case KVS_VECTOR_REBUILD_REPLAYING_DELTA:
		return "replaying_delta";
	case KVS_VECTOR_REBUILD_READY_TO_SWAP:
		return "ready_to_swap";
	case KVS_VECTOR_REBUILD_SWAPPING:
		return "swapping";
	case KVS_VECTOR_REBUILD_FAILED:
		return "failed";
	case KVS_VECTOR_REBUILD_CANCELLED:
		return "cancelled";
	default:
		return "unknown";
	}
}
