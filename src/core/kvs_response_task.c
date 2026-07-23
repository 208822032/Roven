#include "kvs_response_task.h"

#include "kvs_config.h"
#include "kvs_memory.h"
#include "resp_protocol.h"

#include <string.h>

extern kvs_config_t g_cfg;

void kvs_response_task_reset(kvs_response_task_t *resp) {
	if (!resp) {
		return;
	}
	memset(resp, 0, sizeof(*resp));
	resp->fd = -1;
}

void kvs_response_task_init_borrowed(
	kvs_response_task_t *resp,
	char *buf,
	size_t cap
) {
	kvs_response_task_reset(resp);
	if (!resp) {
		return;
	}
	resp->buf = buf;
	resp->cap = cap;
	resp->owns_buf = 0;
}

int kvs_response_task_init_owned(kvs_response_task_t *resp, size_t cap) {
	kvs_response_task_reset(resp);
	if (!resp || cap == 0) {
		return -1;
	}
	resp->buf = (char *)kvs_malloc(cap);
	if (!resp->buf) {
		return -1;
	}
	resp->cap = cap;
	resp->owns_buf = 1;
	return 0;
}

int kvs_response_task_ensure_capacity(kvs_response_task_t *resp, size_t need) {
	char *new_buf;

	if (!resp) {
		return -1;
	}
	if (need <= resp->cap) {
		return 0;
	}
	if (!resp->owns_buf) {
		return -1;
	}

	new_buf = (char *)kvs_malloc(need);
	if (!new_buf) {
		return -1;
	}
	if (resp->buf && resp->len > 0) {
		memcpy(new_buf, resp->buf, resp->len);
	}
	if (resp->buf) {
		kvs_free(resp->buf);
	}
	resp->buf = new_buf;
	resp->cap = need;
	return 0;
}

int kvs_response_task_is_owned(const kvs_response_task_t *resp) {
	return resp ? resp->owns_buf : 0;
}

char *kvs_response_task_detach_buffer(
	kvs_response_task_t *resp,
	size_t *len,
	size_t *cap
) {
	char *buf;

	if (!resp || !resp->owns_buf) {
		return NULL;
	}
	buf = resp->buf;
	if (len) {
		*len = resp->len;
	}
	if (cap) {
		*cap = resp->cap;
	}
	resp->buf = NULL;
	resp->cap = 0;
	resp->len = 0;
	resp->owns_buf = 0;
	return buf;
}

void kvs_response_task_destroy(kvs_response_task_t *resp) {
	if (!resp) {
		return;
	}
	if (resp->owns_buf && resp->buf) {
		kvs_free(resp->buf);
	}
	kvs_response_task_reset(resp);
}

char *kvs_response_task_buffer(kvs_response_task_t *resp) {
	return resp ? resp->buf : NULL;
}

size_t kvs_response_task_capacity(const kvs_response_task_t *resp) {
	return resp ? resp->cap : 0;
}

void kvs_response_task_set_length(kvs_response_task_t *resp, size_t len) {
	if (!resp) {
		return;
	}
	resp->len = len;
}

size_t kvs_response_task_length(const kvs_response_task_t *resp) {
	return resp ? resp->len : 0;
}

void kvs_response_task_set_status(kvs_response_task_t *resp, int status) {
	if (!resp) {
		return;
	}
	resp->status = status;
}

int kvs_response_task_status(const kvs_response_task_t *resp) {
	return resp ? resp->status : 0;
}

/*
 * 收口一条响应的最终状态，准备交给网络层发送。
 *
 * status 的正数通常表示响应缓冲区中已经生成的 RESP 字节数；非正数表示
 * 命令失败或没有可发送内容。这里会把最终 status 写入 response task，并把
 * len 统一设置为实际发送长度，网络层只应使用 [buf, buf + len) 发送。
 *
 * 如果成功响应超过配置的 network.max_response_bytes，则丢弃原响应内容，
 * 改写为一条简短的 RESP 错误，避免把超大响应发送给客户端。函数只修改
 * response task 的状态和缓冲区，不负责分配、入队、socket 发送或释放内存。
 * 调用者应在响应内容生成完成后调用一次；finalized 只是状态记录，不会阻止
 * 重复调用，重复 finalize 会以最后一次传入的 status 为准。
 */
void kvs_response_task_finalize(kvs_response_task_t *resp, int status) {
	/* 当前生效的响应长度上限，优先使用统一配置，未配置时使用编译默认值。 */
	size_t configured_limit;

	if (!resp) {
		return;
	}
	/* 防止配置为 0 时失去响应大小保护。 */
	configured_limit = g_cfg.network_max_response_bytes > 0 ?
		g_cfg.network_max_response_bytes : (size_t)KVS_MAX_RESPONSE_LENGTH;
	/*
	 * status 是响应长度且超过上限时，覆盖原响应为错误 RESP。
	 * 只有 resp->buf 存在时才能改写；否则仍保留状态但不会伪造缓冲区地址。
	 */
	if (status > 0 && (size_t)status > configured_limit && resp->buf) {
		status = resp_generate_simple_string_n(
			resp->buf,
			'-',
			"ERR response exceeds network.max_response_bytes",
			sizeof("ERR response exceeds network.max_response_bytes") - 1
		);
	}
	/* 保存最终状态，并标记 response 已经完成，网络层可以开始消费。 */
	resp->status = status;
	resp->finalized = 1;
	if (status > 0) {
		/* 正数表示有效 RESP 长度，发送时只发送这部分字节。 */
		resp->len = (size_t)status;
	} else {
		/* 失败状态不发送旧的或未完成的缓冲区内容。 */
		resp->len = 0;
	}
}
