#ifndef KVS_RESPONSE_TASK_H
#define KVS_RESPONSE_TASK_H

#include <stddef.h>
#include <stdint.h>

typedef struct net_session_s net_session_t;

typedef struct kvs_response_task_s {
	struct kvs_response_task_s *queue_next;
	char *buf;
	size_t cap;
	size_t len;
	int status;
	int owns_buf;
	int finalized;

	net_session_t *session;
	void *session_owner;
	void (*session_release)(void *session_owner);
	int fd;
	uint64_t session_id;
	uint32_t generation;
	uint64_t request_seq;
	int owner_poller_id;
} kvs_response_task_t;

void kvs_response_task_reset(kvs_response_task_t *resp);

void kvs_response_task_init_borrowed(
	kvs_response_task_t *resp,
	char *buf,
	size_t cap
);

int kvs_response_task_init_owned(kvs_response_task_t *resp, size_t cap);

int kvs_response_task_ensure_capacity(kvs_response_task_t *resp, size_t need);

int kvs_response_task_is_owned(const kvs_response_task_t *resp);

char *kvs_response_task_detach_buffer(
	kvs_response_task_t *resp,
	size_t *len,
	size_t *cap
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
