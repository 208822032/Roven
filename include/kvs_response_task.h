#ifndef KVS_RESPONSE_TASK_H
#define KVS_RESPONSE_TASK_H

#include <stddef.h>

typedef struct kvs_response_task_s {
	char *buf;
	size_t cap;
	size_t len;
	int status;
	int owns_buf;
	int finalized;
} kvs_response_task_t;

void kvs_response_task_reset(kvs_response_task_t *resp);

void kvs_response_task_init_borrowed(
	kvs_response_task_t *resp,
	char *buf,
	size_t cap
);

void kvs_response_task_destroy(kvs_response_task_t *resp);

char *kvs_response_task_buffer(kvs_response_task_t *resp);

size_t kvs_response_task_capacity(const kvs_response_task_t *resp);

void kvs_response_task_set_length(kvs_response_task_t *resp, size_t len);

size_t kvs_response_task_length(const kvs_response_task_t *resp);

void kvs_response_task_set_status(kvs_response_task_t *resp, int status);

int kvs_response_task_status(const kvs_response_task_t *resp);

void kvs_response_task_finalize(kvs_response_task_t *resp, int status);

#endif /* KVS_RESPONSE_TASK_H */
