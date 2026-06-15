#include "kvs_async_net_adapter.h"

#include "kvs_config.h"

#include <string.h>

void kvs_async_net_response_target_from_task(
	kvs_async_net_response_target_t *target,
	kvs_response_task_t *resp
) {
	if (!target) {
		return;
	}
	memset(target, 0, sizeof(*target));
	target->fd = -1;
	target->owner_type = KVS_ASYNC_NET_OWNER_REACTOR;
	if (!resp) {
		return;
	}

	target->fd = resp->fd;
	target->owner_type = resp->owner_type;
	target->owner = resp->session_owner;
	target->owner_poller_id = resp->owner_poller_id;
	target->session = resp->session;
	target->session_id = resp->session_id;
	target->generation = resp->generation;
	target->request_seq = resp->request_seq;
	target->response_buf = kvs_response_task_buffer(resp);
	target->response_len = kvs_response_task_length(resp);
	target->response_cap = kvs_response_task_capacity(resp);
	target->release_session = resp->session_release;
}

static void kvs_async_net_finish_target(
	const kvs_async_net_ops_t *ops,
	kvs_async_net_response_target_t *target
) {
	if (!target) {
		return;
	}
	if (target->session) {
		net_session_end_inflight(target->session);
	}
	if (ops && ops->release_session) {
		ops->release_session(target);
		return;
	}
	if (target->release_session && target->owner) {
		void (*release_fn)(void *) = target->release_session;
		void *owner = target->owner;
		target->release_session = NULL;
		target->owner = NULL;
		target->session = NULL;
		release_fn(owner);
	}
}

static void kvs_async_net_clear_inflight(kvs_async_net_response_target_t *target) {
	if (target && target->session) {
		net_session_end_inflight(target->session);
	}
}

static void kvs_async_net_release_target(
	const kvs_async_net_ops_t *ops,
	kvs_async_net_response_target_t *target
) {
	if (!target) {
		return;
	}
	if (ops && ops->release_session) {
		ops->release_session(target);
		return;
	}
	if (target->release_session && target->owner) {
		void (*release_fn)(void *) = target->release_session;
		void *owner = target->owner;
		target->release_session = NULL;
		target->owner = NULL;
		target->session = NULL;
		release_fn(owner);
	}
}

static void kvs_async_net_drop(
	const kvs_async_net_ops_t *ops,
	kvs_async_net_response_target_t *target,
	kvs_async_net_drop_reason_t reason
) {
	if (ops && ops->drop_response) {
		ops->drop_response(target, reason);
	}
	kvs_async_net_finish_target(ops, target);
}

int kvs_async_net_dispatch_response(
	const kvs_async_net_ops_t *ops,
	kvs_async_net_response_target_t *target
) {
	kvs_async_net_drop_reason_t reason = KVS_ASYNC_NET_DROP_SESSION_MISMATCH;
	int valid;

	if (!target) {
		return KVS_ASYNC_NET_DISPATCH_APPEND_FAILED;
	}
	if (!ops || !ops->validate_owner || !ops->append_response) {
		/*
		 * The reactor caller always passes g_reactor_async_net_ops. Missing
		 * mandatory ops means a broken adapter registration, not an expected
		 * runtime condition. Still release the response target's session ref
		 * when possible so the programming error does not leak ownership.
		 */
		kvs_async_net_finish_target(ops, target);
		return KVS_ASYNC_NET_DISPATCH_APPEND_FAILED;
	}

	valid = ops->validate_owner(target, &reason);
	if (!valid) {
		kvs_async_net_drop(ops, target, reason);
		return KVS_ASYNC_NET_DISPATCH_DROPPED;
	}

	if (!target->response_buf ||
	    target->response_len == 0 ||
	    target->response_len > KVS_MAX_RESPONSE_LENGTH ||
	    target->response_len > target->response_cap) {
		kvs_async_net_drop(ops, target, KVS_ASYNC_NET_DROP_APPEND_FAILED);
		return KVS_ASYNC_NET_DISPATCH_APPEND_FAILED;
	}

	if (ops->append_response(target) < 0) {
		kvs_async_net_drop(ops, target, KVS_ASYNC_NET_DROP_APPEND_FAILED);
		return KVS_ASYNC_NET_DISPATCH_APPEND_FAILED;
	}

	kvs_async_net_clear_inflight(target);
	if (ops->enable_write) {
		ops->enable_write(target);
	}
	if (ops->restore_read) {
		ops->restore_read(target);
	}
	kvs_async_net_release_target(ops, target);
	return KVS_ASYNC_NET_DISPATCH_APPENDED;
}
