#ifndef KVS_COMMAND_TASK_H
#define KVS_COMMAND_TASK_H

#include <stdint.h>

#include "kvs_command.h"
#include "kvs_response_task.h"
#include "kvs_vector.h"

/* 这里只保存会话指针；完整定义在 net_session.h，避免头文件循环包含。 */
typedef struct net_session_s net_session_t;

/*
 * prepare 阶段实际准备的是哪一种向量命令。
 * 普通 KV 命令不需要 embedding，因此保持 NONE。
 */
typedef enum kvs_command_task_prepared_kind_e {
	/* 还没有准备向量数据，或该任务不是向量命令。 */
	KVS_COMMAND_TASK_PREPARED_NONE = 0,
	/* VSET 已准备好待提交的 embedding 和相关数据。 */
	KVS_COMMAND_TASK_PREPARED_VSET,
	/* VSEARCH 已准备好查询 embedding。 */
	KVS_COMMAND_TASK_PREPARED_VSEARCH,
} kvs_command_task_prepared_kind_t;

/*
 * 一条待执行或正在执行的命令。
 *
 * 常见流程：
 * 1. RESP 解析器给出 argv 和 argv_lens。
 * 2. build 查命令表、检查参数，并填充 ctx。
 * 3. 向量命令可先由 worker 做 prepare；普通命令直接进入 execute。
 * 4. execute 在正确的锁和同步上下文中调用命令 handler。
 * 5. 成功提交后，post_commit 处理 AOF 和普通 KV/TTL 复制等派生动作。
 * 6. destroy 释放本任务拥有的资源。
 *
 * 注意：task 本身不拥有 response_task，也不直接释放网络连接对象。
 * executor 把连接信息转移给 response task 后，由网络层完成发送和释放。
 */
typedef struct kvs_command_task_s {
	/* executor 命令队列的下一个节点，不属于命令参数。 */
	struct kvs_command_task_s *queue_next;

	/* 命令表中的说明项，包含 handler、参数数量、读写属性和锁模式。 */
	const command_info *cmd;
	/* 命令在内部枚举中的编号，parse_command_argument 依赖它解析参数。 */
	int cmd_index;

	/* argv 中参数数量，包括 argv[0] 的命令名。 */
	int argc;
	/*
	 * 命令参数及每个参数的真实字节长度。
	 * 初始时通常指向 RESP 解析器的内存；跨线程排队前需要 copy_argv 深拷贝。
	 */
	char **argv;
	size_t *argv_lens;

	/*
	 * 本次命令对应的响应任务。它由调用方创建，task 只借用它。
	 * response 和 response_cap 是从 response_task 缓存出的快捷访问字段，
	 * 不拥有 response 缓冲区，也不能单独释放它。
	 */
	kvs_response_task_t *response_task;
	char *response;
	size_t response_cap;

	/* 最后一次 build、prepare 或 execute 得到的状态/RESP 响应长度。 */
	int status;
	/* build 阶段失败时保存对应状态；成功时通常为 0。 */
	int build_status;
	/* 命令是否已经真正修改了数据，用于决定能否执行 post_commit。 */
	int command_committed;

	/*
	 * argv 的所有权标记：
	 * 0 表示参数仍属于 RESP 解析缓冲区，本 task 不能释放；
	 * 1 表示 copy_argv 已深拷贝参数，destroy 时必须逐个释放。
	 * 网络线程把任务交给 executor 或 embedding worker 前，应确保参数可用。
	 */
	int owns_argv;

	/* 向量 prepare 成功后置 1，表示 prepared_set 或 prepared_search 可以使用。 */
	int is_prepared;
	/* 是否已将任务发给 embedding worker，避免同一任务重复提交。 */
	int prepare_dispatched;
	/* worker 或同步 prepare 是否已经给出结果。 */
	int prepare_completed;
	/* prepare 阶段的返回状态；0 表示准备成功。 */
	int prepare_status;
	/* 记录首次请求 prepare 的单调时钟时间，单位为微秒。 */
	uint64_t prepare_enqueue_us;
	/* 记录任务进入 prepared 队列的时间，用于统计排队等待时间，单位为微秒。 */
	uint64_t prepared_enqueue_us;
	/* prepared 指向的具体类型，决定 destroy 时调用哪套清理函数。 */
	kvs_command_task_prepared_kind_t prepared_kind;
	/* 指向 prepared_set 或 prepared_search 的通用别名，便于通用队列处理。 */
	void *prepared;

	/* 获取数据锁累计等待时间，单位为微秒。 */
	uint64_t lock_wait_us;
	/* 最近一次成功获取锁的单调时钟时间，单位为微秒。 */
	uint64_t lock_acquired_us;
	/* 持有数据锁累计时间，单位为微秒。 */
	uint64_t lock_hold_us;
	/* VSEARCH 实际检索和组装结果所花时间，单位为微秒。 */
	uint64_t vsearch_search_us;

	/*
	 * 从 argv 解析出的结构化参数，例如 key、value、scope、topk 和阈值。
	 * ctx 内的字符串通常引用 argv，因此 argv 被复制后需要重新解析 ctx。
	 */
	command_context_t ctx;
	/* 命令需要读锁时置 1；由命令表的 lock_mode 决定。 */
	int use_read_lock;
	/* 命令需要写锁时置 1；写命令和惰性过期删除会使用它。 */
	int use_write_lock;
	/* 当前线程是否已经持有 g_kv_data_rwlock；destroy 会兜底解锁。 */
	int lock_held;

	/*
	 * EXPIRE/PEXPIREAT 成功后观察到的绝对过期时间，单位毫秒。
	 * post_commit 用它把相对过期结果转成可确定回放的 PEXPIREAT。
	 */
	int64_t expire_at_ms;
	/* expire_at_ms 是否有效。 */
	int has_expire_at;
	/* 过期操作发现 key 已应删除时，post_commit 应同步 DEL。 */
	int need_del_sync;
	/* PERSIST 成功时，post_commit 应同步 PERSIST。 */
	int need_persist_sync;

	/*
	 * 原客户端会话及其网络运行时对象。
	 * task 在异步路径中持有这次请求的连接引用；执行器完成后会把这些字段
	 * 转交给 response_task，再由对应网络线程校验身份、发送响应并释放引用。
	 */
	net_session_t *session;
	void *session_owner;
	void (*session_release)(void *session_owner);

	/*
	 * 回包目标的身份信息。fd 不能单独作为身份，因为 fd 可能被系统复用；
	 * session_id 与 generation 用于确认响应仍属于原连接。
	 */
	int fd;
	uint64_t session_id;
	uint32_t generation;
	/* 同一连接中的请求序号，配合 single_inflight 保持响应顺序。 */
	uint64_t request_seq;
	/* 网络模型类型及其 poller 编号，用于将响应唤醒到正确线程。 */
	int owner_type;
	int owner_poller_id;

	/*
	 * 提交任务时复制的线程本地同步上下文。
	 * execute 会临时恢复它；值 2 表示 replica 应用主节点 mutation，
	 * 可绕过 slave 对普通客户端写请求的只读限制。
	 */
	int sync_context;
	/* 该命令是否来自复制连接，主要供网络提交路径记录和区分来源。 */
	int is_replication;

	/* VSET 的 prepare 结果，包含待提交的向量和记录数据。 */
	kvs_vector_set_prepared_t prepared_set;
	/* VSEARCH 的 prepare 结果，包含待检索的查询向量。 */
	kvs_vector_search_prepared_t prepared_search;
} kvs_command_task_t;

/*
 * 初始化 task，查找命令、校验参数、解析 ctx，并确定需要的锁。
 * tokens 和 token_lens 默认只借用，不会复制；失败时返回 -1，并将错误写入
 * task->build_status 和 response_task 的缓冲区。
 */
int kvs_command_task_build(kvs_command_task_t *task, char **tokens, int count, size_t *token_lens, kvs_response_task_t *response_task);

/*
 * 深拷贝 argv、argv_lens 和每个参数字节，并重新解析 ctx。
 * 任务将跨线程或跨 RESP 缓冲区生命周期时必须调用；成功后 owns_argv 为 1。
 */
int kvs_command_task_copy_argv(kvs_command_task_t *task);

/* 返回该命令是否为需要先生成 embedding 的 VSET 或 VSEARCH。 */
int kvs_command_task_requires_embedding_prepare(const kvs_command_task_t *task);

/*
 * 执行命令前的准备步骤。
 * 对 VSET/VSEARCH 会调用向量 provider 生成 embedding；普通 KV 命令直接成功。
 * 成功后 prepared_kind 和对应 prepared_* 结构可供 execute 使用。
 */
int kvs_command_task_prepare(kvs_command_task_t *task);

/*
 * 在当前线程同步执行命令。
 * 函数按 task 的设置获取读锁或写锁，必要时把惰性过期操作从读锁重试为写锁，
 * 调用 handler 后记录提交状态并释放锁。返回 RESP 响应长度或错误状态。
 */
int kvs_command_task_execute_sync(kvs_command_task_t *task);

/*
 * 只对已成功提交的写命令执行派生动作。
 * 当前包括 AOF 追加，以及 master 对普通 KV/TTL mutation 的 TCP 复制；
 * replica 应用命令时会根据 sync_context 避免再次向外复制。
 */
void kvs_command_task_post_commit(kvs_command_task_t *task, int ret);

/* 返回 task 最近一次 VSEARCH 的检索耗时，单位为微秒；task 为 NULL 时返回 0。 */
uint64_t kvs_command_task_get_vsearch_search_us(const kvs_command_task_t *task);

/*
 * 释放 task 自己拥有的参数、副本和向量 prepare 结果，并在异常路径兜底解锁。
 * 不释放 response_task，也不直接释放 session_owner；它们由响应/网络层处理。
 */
void kvs_command_task_destroy(kvs_command_task_t *task);

/* 把内部向量状态码转换成客户端可读的 RESP 简单字符串。 */
int kvs_command_task_vector_status_to_resp(int status, char *response);

#endif /* KVS_COMMAND_TASK_H */
