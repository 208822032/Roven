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

void kvs_response_task_finalize(kvs_response_task_t *resp, int status) {
	size_t configured_limit;

	if (!resp) {
		return;
	}
	configured_limit = g_cfg.network_max_response_bytes > 0 ?
		g_cfg.network_max_response_bytes : (size_t)KVS_MAX_RESPONSE_LENGTH;
	if (status > 0 && (size_t)status > configured_limit && resp->buf) {
		status = resp_generate_simple_string_n(
			resp->buf,
			'-',
			"ERR response exceeds network.max_response_bytes",
			sizeof("ERR response exceeds network.max_response_bytes") - 1
		);
	}
	resp->status = status;
	resp->finalized = 1;
	if (status > 0) {
		resp->len = (size_t)status;
	} else {
		resp->len = 0;
	}
}
