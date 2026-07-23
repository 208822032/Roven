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

/*
 * 把 executor 生成的 response 投递回对应的网络 owner。
 *
 * 处理顺序固定为：
 * 1. 检查 target 和适配器必需回调是否存在；
 * 2. 重新校验 fd、owner、session_id、generation 和连接关闭状态；
 * 3. 校验响应缓冲区和长度，防止发送空数据或越界数据；
 * 4. 调用 owner 的 append_response，把响应复制到网络发送缓冲区；
 * 5. 结束本次 single_inflight，开启写事件，必要时恢复读事件；
 * 6. 释放 response 持有的连接引用。
 *
 * 函数只负责网络层投递，不销毁 response_task 本身。调用方仍需在本函数返回后
 * 销毁 response_task；但 response 内容必须在 append_response 中完成复制后才能销毁。
 * 任意校验或追加失败都会走 drop_response + finish_target，释放连接引用并记录丢弃原因。
 *
 * 返回 KVS_ASYNC_NET_DISPATCH_APPENDED 表示已进入 send buffer；返回 DROPPED 表示
 * 因连接身份失效而丢弃；返回 APPEND_FAILED 表示参数、适配器或发送缓冲追加失败。
 */
int kvs_async_net_dispatch_response(
	const kvs_async_net_ops_t *ops,
	kvs_async_net_response_target_t *target
) {
	/* 默认按 session mismatch 记录失败原因，校验函数可根据实际情况覆盖。 */
	kvs_async_net_drop_reason_t reason = KVS_ASYNC_NET_DROP_SESSION_MISMATCH;
	/* owner 校验结果：非 0 表示 target 仍属于当前网络连接。 */
	int valid;

	if (!target) {
		/* 没有 target 时既不能投递，也没有可释放的连接引用。 */
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

	/* 先验证连接身份，防止旧 task 的响应写入 fd 复用后的新连接。 */
	valid = ops->validate_owner(target, &reason);
	if (!valid) {
		/* 连接关闭、owner 不匹配或 generation 变化都会走丢弃路径。 */
		kvs_async_net_drop(ops, target, reason);
		return KVS_ASYNC_NET_DISPATCH_DROPPED;
	}

	/* response_buf 必须有效，长度不能为 0、不能超过协议上限或实际容量。 */
	if (!target->response_buf ||
	    target->response_len == 0 ||
	    target->response_len > KVS_MAX_RESPONSE_LENGTH ||
	    target->response_len > target->response_cap) {
		kvs_async_net_drop(ops, target, KVS_ASYNC_NET_DROP_APPEND_FAILED);
		return KVS_ASYNC_NET_DISPATCH_APPEND_FAILED;
	}

	/* append_response 会把 response 内容复制到 owner 的 send buffer。 */
	if (ops->append_response(target) < 0) {
		kvs_async_net_drop(ops, target, KVS_ASYNC_NET_DROP_APPEND_FAILED);
		return KVS_ASYNC_NET_DISPATCH_APPEND_FAILED;
	}

	/* 响应已经接管完成，允许该连接提交下一条在途命令。 */
	kvs_async_net_clear_inflight(target);
	if (ops->enable_write) {
		/* send buffer 有数据，通知网络 owner 关注 EPOLLOUT/写完成事件。 */
		ops->enable_write(target);
	}
	if (ops->restore_read) {
		/* 处理响应后恢复读侧，必要时继续消费 Pipeline 中的下一条命令。 */
		ops->restore_read(target);
	}
	/* 最后释放 response 持有的 session/owner 引用。 */
	kvs_async_net_release_target(ops, target);
	return KVS_ASYNC_NET_DISPATCH_APPENDED;
}
