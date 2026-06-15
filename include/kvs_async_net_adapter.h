#ifndef KVS_ASYNC_NET_ADAPTER_H
#define KVS_ASYNC_NET_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "kvs_response_task.h"
#include "net_session.h"

typedef enum kvs_async_net_owner_type_e {
	KVS_ASYNC_NET_OWNER_REACTOR = 1,
	KVS_ASYNC_NET_OWNER_PROACTOR = 2,
	KVS_ASYNC_NET_OWNER_NTYCO = 3,
} kvs_async_net_owner_type_t;

typedef enum kvs_async_net_drop_reason_e {
	KVS_ASYNC_NET_DROP_SESSION_MISMATCH = 1,
	KVS_ASYNC_NET_DROP_SESSION_CLOSING = 2,
	KVS_ASYNC_NET_DROP_APPEND_FAILED = 3,
} kvs_async_net_drop_reason_t;

typedef enum kvs_async_net_dispatch_result_e {
	KVS_ASYNC_NET_DISPATCH_DROPPED = 0,
	KVS_ASYNC_NET_DISPATCH_APPENDED = 1,
	KVS_ASYNC_NET_DISPATCH_APPEND_FAILED = -1,
} kvs_async_net_dispatch_result_t;

typedef struct kvs_async_net_response_target_s {
	int fd;
	kvs_async_net_owner_type_t owner_type;
	void *owner;
	int owner_poller_id;
	net_session_t *session;
	uint64_t session_id;
	uint32_t generation;
	uint64_t request_seq;
	char *response_buf;
	size_t response_len;
	size_t response_cap;
	void (*release_session)(void *owner);
} kvs_async_net_response_target_t;

typedef struct kvs_async_net_ops_s {
	int (*validate_owner)(const kvs_async_net_response_target_t *target,
	                      kvs_async_net_drop_reason_t *reason);
	int (*append_response)(const kvs_async_net_response_target_t *target);
	void (*drop_response)(const kvs_async_net_response_target_t *target,
	                      kvs_async_net_drop_reason_t reason);
	void (*release_session)(kvs_async_net_response_target_t *target);
	int (*restore_read)(const kvs_async_net_response_target_t *target);
	int (*enable_write)(const kvs_async_net_response_target_t *target);
	int (*wake_owner)(int owner_poller_id);
} kvs_async_net_ops_t;

void kvs_async_net_response_target_from_task(
	kvs_async_net_response_target_t *target,
	kvs_response_task_t *resp
);

int kvs_async_net_dispatch_response(
	const kvs_async_net_ops_t *ops,
	kvs_async_net_response_target_t *target
);

#endif /* KVS_ASYNC_NET_ADAPTER_H */
