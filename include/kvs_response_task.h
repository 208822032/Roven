#ifndef KVS_RESPONSE_TASK_H
#define KVS_RESPONSE_TASK_H

#include <stddef.h>
#include <stdint.h>

/*
 * 这里只需要保存 net_session_t 的指针，不需要知道它的完整字段。
 * 完整定义在 net_session.h 中。这样可以避免两个头文件互相包含。
 */
typedef struct net_session_s net_session_t;

/*
 * 一条已经执行完的命令响应。
 *
 * 生命周期：
 * 1. 网络层创建 response task，并把它交给 command task。
 * 2. executor 执行命令，填入 buf、status 和 len。
 * 3. executor 将它放进响应队列，并唤醒网络层。
 * 4. 原网络线程确认连接仍然有效，把 buf 中的数据写给客户端。
 * 5. 网络层调用 destroy，释放本任务拥有的缓冲区和连接引用。
 *
 * 这个结构不能只保存 fd。fd 被关闭后可能很快被系统复用，所以还要
 * 保存 session_id 和 generation，用来确认响应没有发给另一个新连接。
 */
typedef struct kvs_response_task_s {
	/*
	 * 响应队列的链表指针。
	 * 只供 executor 的响应队列使用，不是客户端响应内容的一部分。
	 */
	struct kvs_response_task_s *queue_next;

	/*
	 * RESP 响应的字节缓冲区。
	 * buf 指向实际数据；cap 是已分配容量；len 是当前有效数据长度。
	 * 网络层只能发送 [buf, buf + len)，不能按 C 字符串方式依赖 '\0'。
	 */
	char *buf;
	size_t cap;
	size_t len;

	/*
	 * 命令执行结果。
	 * finalize 后，status 大于 0 时通常等于本次响应的字节数；
	 * status 小于等于 0 表示命令没有生成可发送的正常响应。
	 */
	int status;

	/*
	 * buf 的所有权标记：
	 * 1 表示由 init_owned 申请，destroy 时必须释放；
	 * 0 表示 buf 由调用方提供，destroy 时只能重置字段，不能 free。
	 */
	int owns_buf;

	/*
	 * 是否已调用 finalize。
	 * finalize 会统一写入最终 status 和 len，避免网络层看到半完成响应。
	 */
	int finalized;

	/*
	 * 原请求所属的连接。
	 * 网络层无论发送成功还是丢弃响应，最终都要结束该连接的
	 * single_inflight 状态，并释放这次 task 持有的连接引用。
	 */
	net_session_t *session;

	/*
	 * 网络模型自己的连接对象，例如 Reactor/Proactor 中的连接扩展对象。
	 * executor 只保存和转交这个指针，绝不能直接解引用它。
	 * 真正解释和释放它的工作，由 owner_type 对应的网络层完成。
	 */
	void *session_owner;

	/*
	 * 与 session_owner 配套的释放函数。
	 * 它通常会减少连接对象的引用计数；不代表一定立即释放内存。
	 */
	void (*session_release)(void *session_owner);

	/*
	 * 下面这些字段共同标识“响应应该回到哪里”。
	 * fd 用于定位 socket；session_id 和 generation 用于防止 fd 复用；
	 * request_seq 用于标识同一连接上的第几个请求。
	 */
	int fd;
	uint64_t session_id;
	uint32_t generation;
	uint64_t request_seq;

	/*
	 * owner_type 表示响应归 Reactor、Proactor 还是 NtyCo 处理。
	 * owner_poller_id 用于把响应唤醒并投递给正确的 poller 线程。
	 */
	int owner_type;
	int owner_poller_id;
} kvs_response_task_t;

/*
 * 把任务恢复到空状态，并把 fd 设为 -1。
 * 本函数不释放 buf；若任务拥有 buf，请先调用 destroy。
 */
void kvs_response_task_reset(kvs_response_task_t *resp);

/*
 * 使用调用方提供的缓冲区。
 * 调用方必须保证 buf 在任务完成前一直有效；本任务不会释放它，
 * 也不能通过 ensure_capacity 扩大它。
 */
void kvs_response_task_init_borrowed(
	kvs_response_task_t *resp,
	char *buf,
	size_t cap
);

/*
 * 为响应申请独立缓冲区。cap 必须大于 0。
 * 成功后任务拥有该缓冲区，调用 destroy 会自动释放；失败返回 -1。
 */
int kvs_response_task_init_owned(kvs_response_task_t *resp, size_t cap);

/*
 * 确保自有缓冲区至少可容纳 need 个字节。
 * 需要扩容时会申请新内存，复制已有 [0, len) 数据，再释放旧内存。
 * 借用缓冲区没有所有权，不能扩容；失败返回 -1。
 */
int kvs_response_task_ensure_capacity(kvs_response_task_t *resp, size_t need);

/* 返回当前任务是否负责释放 buf。resp 为 NULL 时返回 0。 */
int kvs_response_task_is_owned(const kvs_response_task_t *resp);

/*
 * 把自有缓冲区的所有权交给调用方，并返回原 buf 指针。
 * 可选输出参数 len 和 cap 会得到原来的有效长度和容量。
 * 成功后任务不再持有该缓冲区；调用方必须使用 kvs_free 释放它。
 * 借用缓冲区或空任务不能 detach，返回 NULL。
 */
char *kvs_response_task_detach_buffer(
	kvs_response_task_t *resp,
	size_t *len,
	size_t *cap
);

/*
 * 销毁任务内部资源。
 * 仅释放 owns_buf 为 1 的 buf，随后执行 reset；不释放 task 结构本身。
 */
void kvs_response_task_destroy(kvs_response_task_t *resp);

/* 返回响应缓冲区指针；resp 为 NULL 时返回 NULL。 */
char *kvs_response_task_buffer(kvs_response_task_t *resp);

/* 返回已分配缓冲区容量；resp 为 NULL 时返回 0。 */
size_t kvs_response_task_capacity(const kvs_response_task_t *resp);

/*
 * 设置有效响应长度，不会检查 len 是否超过 cap。
 * 调用方应先保证缓冲区足够大，最终发送前由 finalize 统一确认状态。
 */
void kvs_response_task_set_length(kvs_response_task_t *resp, size_t len);

/* 返回当前有效响应长度；resp 为 NULL 时返回 0。 */
size_t kvs_response_task_length(const kvs_response_task_t *resp);

/* 设置命令处理状态，不会自动更新 len 或 finalized。 */
void kvs_response_task_set_status(kvs_response_task_t *resp, int status);

/* 返回命令处理状态；resp 为 NULL 时返回 0。 */
int kvs_response_task_status(const kvs_response_task_t *resp);

/*
 * 标记响应已完成，并统一设置 status 和 len。
 *
 * status 大于 0 时，把它作为响应长度；小于等于 0 时将 len 设为 0。
 * 如果成功响应超过 network.max_response_bytes，函数会改写 buf，
 * 生成一条 RESP 错误响应，避免向网络层提交超大响应。
 */
void kvs_response_task_finalize(kvs_response_task_t *resp, int status);

#endif /* KVS_RESPONSE_TASK_H */
