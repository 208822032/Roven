#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "kvs_vector.h"

typedef enum kvs_vector_rebuild_state {
	KVS_VECTOR_REBUILD_IDLE = 0,
	KVS_VECTOR_REBUILD_BUILDING,
	KVS_VECTOR_REBUILD_REPLAYING_DELTA,
	KVS_VECTOR_REBUILD_READY_TO_SWAP,
	KVS_VECTOR_REBUILD_SWAPPING,
	KVS_VECTOR_REBUILD_FAILED,
	KVS_VECTOR_REBUILD_CANCELLED
} kvs_vector_rebuild_state_t;

typedef enum kvs_vector_rebuild_delta_op {
	KVS_VECTOR_REBUILD_DELTA_UPSERT = 1,
	KVS_VECTOR_REBUILD_DELTA_DELETE = 2
} kvs_vector_rebuild_delta_op_t;

typedef struct kvs_vector_rebuild_delta_entry_s {
	uint64_t generation;
	kvs_vector_rebuild_delta_op_t op;
	uint64_t record_id;
	float *vector;
	size_t dim;
} kvs_vector_rebuild_delta_entry_t;

typedef void (*kvs_vector_rebuild_index_destroy_fn)(void *index);

typedef struct kvs_vector_rebuild_context_s {
	kvs_vector_rebuild_state_t state;
	uint64_t current_generation;
	uint64_t rebuild_generation;

	size_t delta_count;
	size_t delta_capacity;
	kvs_vector_rebuild_delta_entry_t *delta_log;

	void *active_index;
	void *shadow_index;
	void *retired_index;
	kvs_vector_rebuild_index_destroy_fn active_destroy_fn;
	kvs_vector_rebuild_index_destroy_fn shadow_destroy_fn;
	kvs_vector_rebuild_index_destroy_fn retired_destroy_fn;

	size_t progress_total;
	size_t progress_done;

	int error_code;
	char error_message[128];
	int cancel_requested;
} kvs_vector_rebuild_context_t;

int kvs_vector_rebuild_context_init(kvs_vector_rebuild_context_t *ctx);
void kvs_vector_rebuild_context_destroy(kvs_vector_rebuild_context_t *ctx);
void kvs_vector_rebuild_delta_clear(kvs_vector_rebuild_context_t *ctx);

int kvs_vector_rebuild_append_upsert_delta(
	kvs_vector_rebuild_context_t *ctx,
	uint64_t record_id,
	const float *vector,
	size_t dim
);
int kvs_vector_rebuild_append_delete_delta(
	kvs_vector_rebuild_context_t *ctx,
	uint64_t record_id
);
size_t kvs_vector_rebuild_delta_count(
	const kvs_vector_rebuild_context_t *ctx
);
bool kvs_vector_rebuild_is_capture_active(
	const kvs_vector_rebuild_context_t *ctx
);

int kvs_vector_rebuild_begin(
	kvs_vector_rebuild_context_t *ctx,
	void *active_index,
	void *shadow_index
);
int kvs_vector_rebuild_enter_replaying_delta(
	kvs_vector_rebuild_context_t *ctx
);
int kvs_vector_rebuild_enter_ready_to_swap(
	kvs_vector_rebuild_context_t *ctx
);
int kvs_vector_rebuild_enter_swapping(
	kvs_vector_rebuild_context_t *ctx
);
int kvs_vector_rebuild_promote_shadow(
	kvs_vector_rebuild_context_t *ctx
);
int kvs_vector_rebuild_finish_swap(
	kvs_vector_rebuild_context_t *ctx,
	void *new_active_index,
	void *retired_index
);
int kvs_vector_rebuild_retire_cleanup(
	kvs_vector_rebuild_context_t *ctx
);
int kvs_vector_rebuild_fail(
	kvs_vector_rebuild_context_t *ctx,
	int error_code,
	const char *error_message
);
int kvs_vector_rebuild_cancel(
	kvs_vector_rebuild_context_t *ctx
);
int kvs_vector_rebuild_reset_idle(
	kvs_vector_rebuild_context_t *ctx
);

const char *kvs_vector_rebuild_state_name(kvs_vector_rebuild_state_t state);
