#define KVS_VECTOR_INTERNAL
#include "kvs_command_task.h"

#include "kvstore.h"
#include "distributed.h"
#include "resp_protocol.h"
#include "kvs_memory.h"
#include "sync_mode.h"
#include "kvs_config.h"
#include "kvs_globals.h"
#include "kvs_log.h"
#include "kvs_owner_lock.h"
#include "kvs_vector.h"
#include "kvs_aof.h"

#include <string.h>
#include <strings.h>
#include <time.h>

static uint64_t kvs_command_task_now_us(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static unsigned int kvs_command_task_engine_bit(int data_structure) {
	switch (data_structure) {
		case DATA_STRUCTURE_ARRAY: return KVS_STORAGE_ENGINE_ARRAY;
		case DATA_STRUCTURE_RBTREE: return KVS_STORAGE_ENGINE_RBTREE;
		case DATA_STRUCTURE_HASH: return KVS_STORAGE_ENGINE_HASH;
		case DATA_STRUCTURE_SKIPTABLE: return KVS_STORAGE_ENGINE_SKIPTABLE;
		default: return 0;
	}
}

static int kvs_command_task_default_engine(void) {
	if (strcasecmp(g_cfg.storage_default_engine, "rbtree") == 0) return DATA_STRUCTURE_RBTREE;
	if (strcasecmp(g_cfg.storage_default_engine, "hash") == 0) return DATA_STRUCTURE_HASH;
	if (strcasecmp(g_cfg.storage_default_engine, "skiptable") == 0) return DATA_STRUCTURE_SKIPTABLE;
	return DATA_STRUCTURE_ARRAY;
}

static int kvs_command_task_has_stored_value(int cmd_index) {
	return cmd_index == KVS_CMD_SET || cmd_index == KVS_CMD_MOD ||
	       cmd_index == KVS_CMD_RSET || cmd_index == KVS_CMD_RMOD ||
	       cmd_index == KVS_CMD_HSET || cmd_index == KVS_CMD_HMOD ||
	       cmd_index == KVS_CMD_SSET || cmd_index == KVS_CMD_SMOD ||
	       cmd_index == KVS_CMD_PAPERSET || cmd_index == KVS_CMD_PAPERMOD;
}

int kvs_command_task_vector_status_to_resp(int status, char *response) {
	switch (status) {
		case KVS_VECTOR_OK:
			return resp_generate_simple_string_n(response, '+', "OK", sizeof("OK") - 1);
		case KVS_VECTOR_ERR_INVALID_ARG:
			return resp_generate_simple_string_n(response, '-', "ERR invalid vector argument", sizeof("ERR invalid vector argument") - 1);
		case KVS_VECTOR_ERR_NOT_FOUND:
			return resp_generate_simple_string_n(response, '+', "NO EXIST", sizeof("NO EXIST") - 1);
		case KVS_VECTOR_ERR_NOT_INITIALIZED:
			return resp_generate_simple_string_n(response, '-', "ERR vector index not initialized", sizeof("ERR vector index not initialized") - 1);
		case KVS_VECTOR_ERR_NOT_IMPLEMENTED:
			return resp_generate_simple_string_n(response, '-', "ERR vector command not implemented", sizeof("ERR vector command not implemented") - 1);
		default:
			return resp_generate_simple_string_n(response, '-', "ERR vector operation failed", sizeof("ERR vector operation failed") - 1);
	}
}

static int kvs_vector_status_to_resp(int status, char *response) {
	return kvs_command_task_vector_status_to_resp(status, response);
}

static int kvs_vector_search_prepared_to_resp(
	const command_context_t *ctx,
	const kvs_vector_search_prepared_t *prepared,
	char *response
) {
	int topk;
	size_t results_capacity;
	kvs_vector_search_result_t *results = NULL;
	size_t results_count = 0;
	size_t i;
	int status;
	int off = 0;
	uint64_t response_start_us;
	uint64_t response_end_us;
	uint64_t total_start_us;

	if (!ctx || !prepared) {
		return kvs_vector_status_to_resp(KVS_VECTOR_ERR_INVALID_ARG, response);
	}

	topk = ctx->topk;
	if (topk <= 0) {
		topk = 1;
	}
	if (topk > 1024) {
		return kvs_vector_status_to_resp(KVS_VECTOR_ERR_INVALID_ARG, response);
	}

	total_start_us = kvs_command_task_now_us();
	results_capacity = (size_t)topk;
	results = (kvs_vector_search_result_t *)kvs_malloc(
		results_capacity * sizeof(kvs_vector_search_result_t)
	);
	if (!results) {
		return kvs_vector_status_to_resp(KVS_VECTOR_ERR, response);
	}

	kvs_vector_search_results_reset(results, results_capacity);
	status = kvs_vector_search_with_prepared(
		prepared,
		topk,
		ctx->has_threshold ? ctx->threshold : g_cfg.vector_score_threshold,
		results,
		results_capacity,
		&results_count
	);
	if (status != KVS_VECTOR_OK) {
		kvs_free(results);
		return kvs_vector_status_to_resp(status, response);
	}

	response_start_us = kvs_command_task_now_us();
	off += resp_generate_array_start(response + off, (int)results_count);
	for (i = 0; i < results_count; ++i) {
		char *item = NULL;
		int item_cap;
		int n;

		item_cap = snprintf(
			NULL, 0,
			"score:%.6f|scope:%.*s|question:%.*s|answer:%.*s",
			results[i].score,
			(int)results[i].scope_len,
			results[i].scope ? results[i].scope : "",
			(int)results[i].question_len,
			results[i].question ? results[i].question : "",
			(int)results[i].answer_len,
			results[i].answer ? results[i].answer : ""
		);
		if (item_cap < 0) {
			kvs_free(results);
			return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
		}

		item_cap += 1;
		item = (char *)kvs_malloc((size_t)item_cap);
		if (!item) {
			kvs_free(results);
			return resp_generate_simple_string_n(response, '-', "ERR out of memory", sizeof("ERR out of memory") - 1);
		}

		n = snprintf(
			item, (size_t)item_cap,
			"score:%.6f|scope:%.*s|question:%.*s|answer:%.*s",
			results[i].score,
			(int)results[i].scope_len,
			results[i].scope ? results[i].scope : "",
			(int)results[i].question_len,
			results[i].question ? results[i].question : "",
			(int)results[i].answer_len,
			results[i].answer ? results[i].answer : ""
		);
		if (n < 0 || n >= item_cap) {
			kvs_free(item);
			kvs_free(results);
			return resp_generate_simple_string_n(response, '-', "ERR response too large", sizeof("ERR response too large") - 1);
		}

		off += resp_generate_bulk_string_n(response + off, item, (size_t)n);
		kvs_free(item);
	}
	response_end_us = kvs_command_task_now_us();
	kvs_vector_vsearch_record_response_build(
		response_end_us >= response_start_us ? response_end_us - response_start_us : 0
	);
	kvs_vector_vsearch_record_search(
		response_end_us >= total_start_us ? response_end_us - total_start_us : 0
	);

	kvs_free(results);
	return off;
}

static void kvs_command_task_reset(kvs_command_task_t *task) {
	if (!task) {
		return;
	}
	memset(task, 0, sizeof(*task));
	task->cmd_index = -1;
	task->fd = -1;
	kvs_vector_set_prepared_reset(&task->prepared_set);
	kvs_vector_search_prepared_reset(&task->prepared_search);
}

static void kvs_command_task_free_owned_argv(kvs_command_task_t *task) {
	int i;

	if (!task || !task->owns_argv) {
		return;
	}

	if (task->argv) {
		for (i = 0; i < task->argc; ++i) {
			if (task->argv[i]) {
				kvs_free(task->argv[i]);
			}
		}
		kvs_free(task->argv);
	}
	if (task->argv_lens) {
		kvs_free(task->argv_lens);
	}

	task->argv = NULL;
	task->argv_lens = NULL;
	task->argc = 0;
	task->owns_argv = 0;
}

void kvs_command_task_destroy(kvs_command_task_t *task) {
	if (!task) {
		return;
	}

	if (task->lock_held) {
		kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "command_task.destroy");
		task->lock_held = 0;
	}

	if (task->prepared_kind == KVS_COMMAND_TASK_PREPARED_VSET) {
		kvs_vector_set_prepared_destroy(&task->prepared_set);
	} else if (task->prepared_kind == KVS_COMMAND_TASK_PREPARED_VSEARCH) {
		kvs_vector_search_prepared_destroy(&task->prepared_search);
	}

	/*
	 * Stage-4A adds optional owned argv for future executor use. The current
	 * synchronous path still leaves owns_argv=0 and remains zero-copy.
	 */
	kvs_command_task_free_owned_argv(task);
	task->prepared = NULL;
	task->is_prepared = 0;
}

/*
 * 把 task 当前借用的 argv 参数复制到 task 自己管理的内存中。
 *
 * 为什么必须复制：
 * 1. task->argv 通常直接指向 net_session 的 RESP parser 缓冲区；解析下一条
 *    命令或重置 parser 时，这块内存可能被覆盖，不能让跨函数、跨线程的 task
 *    继续引用它。
 * 2. command task 提交 executor 后，网络线程可能很快返回并修改 recv_buf；如果
 *    不复制，executor 看到的 key/value 可能已经变成下一条请求，甚至变成无效地址。
 * 3. 深拷贝后，task->owns_argv=1，executor 可以独立使用和释放参数，不依赖网络
 *    线程或 parser 的生命周期，从而避免悬空指针和数据竞争。
 * 4. 同时复制每个参数的真实长度，保留参数中的 '\0' 等二进制内容；末尾额外补
 *    一个 '\0'，兼容仍按 C 字符串读取参数的命令处理代码。
 *
 * 函数采用“全部成功后再交接”的方式：中途分配或参数解析失败时，释放临时副本，
 * 原 task 仍保持原有指针和所有权状态，调用方可以按失败路径处理。
 */
int kvs_command_task_copy_argv(kvs_command_task_t *task) {
	/* 临时保存新申请的参数数组；成功前 task 仍保留原来的指针。 */
	char **argv_copy = NULL;
	/* 临时保存每个参数的真实字节长度。 */
	size_t *lens_copy = NULL;
	/* 深拷贝参数后重新生成的命令上下文。 */
	command_context_t copied_ctx;
	/* 用于遍历所有命令参数。 */
	int i;

	/* 没有完整参数信息时无法复制，调用方需要先完成 task_build。 */
	if (!task || task->argc <= 0 || !task->argv || !task->argv_lens) {
		return -1;
	}
	/* 已经拥有一份副本时直接返回，避免重复分配和重复释放。 */
	if (task->owns_argv) {
		return 0;
	}
	/* 检查 argc 乘以指针大小是否会发生整数溢出。 */
	if ((size_t)task->argc > ((size_t)-1) / sizeof(char *)) {
		return -1;
	}

	/* 先申请参数指针数组，并清零，便于失败时安全释放。 */
	argv_copy = (char **)kvs_malloc((size_t)task->argc * sizeof(char *));
	if (!argv_copy) {
		return -1;
	}
	memset(argv_copy, 0, (size_t)task->argc * sizeof(char *));

	/* 再申请长度数组；它与 argv_copy 一一对应。 */
	lens_copy = (size_t *)kvs_malloc((size_t)task->argc * sizeof(size_t));
	if (!lens_copy) {
		kvs_free(argv_copy);
		return -1;
	}
	memset(lens_copy, 0, (size_t)task->argc * sizeof(size_t));

	for (i = 0; i < task->argc; ++i) {
		/* 使用保存的长度复制二进制安全的参数，不能只依赖 strlen。 */
		size_t len = task->argv_lens[i];

		/* 非空长度参数必须有对应的数据地址。 */
		if (!task->argv[i] && len > 0) {
			goto fail;
		}
		/* -1 是无效长度，避免 len + 1 和后续 memcpy 出错。 */
		if (len == (size_t)-1) {
			goto fail;
		}
		lens_copy[i] = len;
		/* 多申请一个字节，末尾补 '\0'，兼容需要 C 字符串的旧接口。 */
		argv_copy[i] = (char *)kvs_malloc(len + 1);
		if (!argv_copy[i]) {
			goto fail;
		}
		if (len > 0) {
			memcpy(argv_copy[i], task->argv[i], len);
		}
		argv_copy[i][len] = '\0';
	}

	/* argv 已脱离 parser 缓冲区后，重新解析依赖 argv 的 ctx 指针。 */
	if (task->cmd && task->cmd_index >= 0 && task->build_status <= 0) {
		memset(&copied_ctx, 0, sizeof(copied_ctx));
		if (parse_command_argument(task->cmd_index, argv_copy, task->argc, lens_copy, &copied_ctx) < 0) {
			goto fail;
		}
	}

	/* 所有复制和校验都成功后，才一次性把所有权交给 task。 */
	task->argv = argv_copy;
	task->argv_lens = lens_copy;
	task->owns_argv = 1;
	if (task->cmd && task->cmd_index >= 0 && task->build_status <= 0) {
		task->ctx = copied_ctx;
	}

	return 0;

fail:
	/* 中途失败时释放已经申请成功的每个参数和两个数组。 */
	if (argv_copy) {
		for (i = 0; i < task->argc; ++i) {
			if (argv_copy[i]) {
				kvs_free(argv_copy[i]);
			}
		}
		kvs_free(argv_copy);
	}
	if (lens_copy) {
		kvs_free(lens_copy);
	}
	return -1;
}

int kvs_command_task_requires_embedding_prepare(const kvs_command_task_t *task) {
	if (!task || !task->cmd || task->build_status > 0) {
		return 0;
	}
	return strcmp(task->cmd->name, "VSET") == 0 ||
	       strcmp(task->cmd->name, "VSEARCH") == 0;
}

/*
 * 根据已经解析完成的 RESP 参数，建立一个可交给 executor 的 command task。
 *
 * 本函数只做“命令建模和前置校验”：查命令表、检查参数数量、解析 ctx、检查
 * 当前节点角色和配置能力、选择读写锁模式。它不执行 handler、不加数据锁，
 * 也不执行 AOF 或复制等 post-commit 动作。
 *
 * tokens/token_lens 通常来自 resp_parser。这里先借用它们，owns_argv 保持 0；
 * 若 task 将跨线程使用，调用方必须在本函数成功后调用 copy_argv 深拷贝参数。
 * 调用者必须保证 task、tokens、token_lens 和 response_task 均为有效对象。
 *
 * 返回值：
 * - 0：命令已通过 build 阶段，可继续 prepare/execute；
 * - -1：命令无效或当前配置不允许。错误 RESP 已写入 response_task，
 *   task->status/build_status 保存该错误响应的长度，供异步回包路径使用。
 */
int kvs_command_task_build(
	kvs_command_task_t *task,
	char **tokens,
	int count,
	size_t *token_lens,
	kvs_response_task_t *response_task
) {
	/* response 是 response_task 缓冲区的快捷别名，本函数不拥有它。 */
	char *response;

	/*
	 * 先清除 task 的旧状态，设置无效默认 cmd_index，并重置嵌入的向量准备状态。
	 * 调用者应确保 task 未持有需要先 destroy 的旧资源。
	 */
	kvs_command_task_reset(task);

	/*
	 * 暂时借用 RESP parser 的参数数组。此时 argv 还没有深拷贝，不能在 parser
	 * reset 后继续使用；reactor 提交路径会紧接着调用 kvs_command_task_copy_argv。
	 */
	task->argc = count;
	task->argv = tokens;
	task->argv_lens = token_lens;
	task->response_task = response_task;
	/* 取得调用方已准备好的响应缓冲区及其容量。 */
	response = kvs_response_task_buffer(response_task);
	task->response = response;
	task->response_cap = kvs_response_task_capacity(response_task);
	task->owns_argv = 0;
	/* 保存当前线程的复制上下文，execute 时会临时恢复它。 */
	task->sync_context = get_sync_context();

	/*
	 * 空命令不能进入命令表。tokens[0] 是 RESP 命令名，因此 NULL 或 count 为 0
	 * 都会生成客户端可读的错误响应。
	 */
	if (tokens[0] == NULL || count == 0) {
		task->status = resp_generate_simple_string_n(response, '-', "Invalid command", sizeof("Invalid command") - 1);
		task->build_status = task->status;
		return -1;
	}

	/*
	 * 按命令名查 command_table，取得 handler、arity、是否写命令和锁模式。
	 * 查找不到时不能继续，因为后续没有 handler 可以执行。
	 */
	task->cmd = lookup_command(tokens[0]);
	if (task->cmd == NULL) {
		task->status = resp_generate_simple_string_n(response, '-', "Unknown command", sizeof("Unknown command") - 1);
		task->build_status = task->status;
		return -1;
	}

	/*
	 * 取得命令枚举编号。parse_command_argument 使用编号决定如何解释 argv，
	 * 因此即使已经拿到 command_info，也必须确认该编号存在。
	 */
	task->cmd_index = get_command_index(tokens[0]);
	if (task->cmd_index == -1) {
		task->status = resp_generate_simple_string_n(response, '-', "Unknown command", sizeof("Unknown command") - 1);
		task->build_status = task->status;
		return -1;
	}

	/* 用命令表声明的 arity 检查参数数量，例如 SET 必须有命令名、key、value。 */
	if (!check_arity(task->cmd, count)) {
		task->status = resp_generate_simple_string_n(response, '-', "Wrong number of arguments", sizeof("Wrong number of arguments") - 1);
		task->build_status = task->status;
		return -1;
	}

	/*
	 * slave 只拒绝普通客户端发起的写命令。sync_context 非 0 表示复制或内部
	 * 应用路径，可以继续执行，否则主节点同步过来的 mutation 无法落到 slave。
	 */
	if (task->cmd->write_command &&
	    global_dist_config.role == ROLE_SLAVE &&
	    get_sync_context() == 0) {
		task->status = resp_generate_simple_string_n(response, '-', "ERROR SLAVE READ ONLY", sizeof("ERROR SLAVE READ ONLY") - 1);
		task->build_status = task->status;
		return -1;
	}

	/*
	 * 将通用 argv 转为 command_context_t，例如填入 key、value、scope、topk、
	 * 过期时间和指定数据结构。解析失败通常表示数值范围或命令格式错误。
	 */
	if (parse_command_argument(task->cmd_index, tokens, count, token_lens, &task->ctx) < 0) {
		task->status = resp_generate_simple_string_n(response, '-', "Invalid arguments", sizeof("Invalid arguments") - 1);
		task->build_status = task->status;
		return -1;
	}

	/*
	 * 基础 KV 命令没有在参数中指定后端时，使用 storage.default_engine。
	 * 向量和其他特殊命令保留 parse_command_argument 已设置的 data_structure。
	 */
	if (task->cmd_index >= KVS_CMD_SET && task->cmd_index <= KVS_CMD_SORT) {
		task->ctx.data_structure = kvs_command_task_default_engine();
	}

	/*
	 * 命令指定或默认选择的存储后端必须出现在配置的 enabled engine mask 中。
	 * 这避免“代码已编译但运行配置禁用”的后端仍被命令访问。
	 */
	if (task->ctx.data_structure >= 0 &&
	    !(g_cfg.storage_enabled_engine_mask &
	      kvs_command_task_engine_bit(task->ctx.data_structure))) {
		task->status = resp_generate_simple_string_n(response, '-',
			"ERR storage engine disabled", sizeof("ERR storage engine disabled") - 1);
		task->build_status = task->status;
		return -1;
	}

	/* 对会保存 value 的命令检查逻辑值大小上限；0 表示不额外限制。 */
	if (kvs_command_task_has_stored_value(task->cmd_index) &&
	    g_cfg.max_value_size > 0 && task->ctx.value_len > g_cfg.max_value_size) {
		task->status = resp_generate_simple_string_n(response, '-',
			"ERR value exceeds storage.max_value_bytes",
			sizeof("ERR value exceeds storage.max_value_bytes") - 1);
		task->build_status = task->status;
		return -1;
	}

	/* TTL 功能被关闭时，EXPIRE 到 PEXPIREAT 命令不能绕过配置直接执行。 */
	if (!g_cfg.ttl_enabled && task->cmd_index >= KVS_CMD_EXPIRE &&
	    task->cmd_index <= KVS_CMD_PEXPIREAT) {
		task->status = resp_generate_simple_string_n(response, '-',
			"ERR TTL disabled", sizeof("ERR TTL disabled") - 1);
		task->build_status = task->status;
		return -1;
	}

	/* AOF rewrite 是可选能力；AOF 未开启或 rewrite 被禁用时提前拒绝。 */
	if (task->cmd_index == KVS_CMD_AOFREWRITE &&
	    (!g_cfg.enable_aof || !g_cfg.aof_rewrite_enabled)) {
		task->status = resp_generate_simple_string_n(response, '-',
			"ERR AOF rewrite disabled", sizeof("ERR AOF rewrite disabled") - 1);
		task->build_status = task->status;
		return -1;
	}

	/*
	 * 最后把 command_table 的锁策略转成 task 标记。真正获取和释放锁在
	 * kvs_command_task_execute_sync 中完成，而不是在网络线程的 build 阶段。
	 */
	switch (task->cmd->lock_mode) {
		case KVS_CMD_LOCK_READ:
			/* 只读命令可并发读取全局 KV 数据。 */
			task->use_read_lock = 1;
			break;
		case KVS_CMD_LOCK_WRITE:
		default:
			/* 写命令默认独占数据锁，保证提交过程不会与其他 mutation 并发。 */
			task->use_write_lock = 1;
			break;
		case KVS_CMD_LOCK_NONE:
			/* 无需访问受该锁保护的数据，例如部分控制类命令。 */
			break;
	}

	return 0;
}

/*
 * 为 command task 做执行前准备，不在这里修改数据库状态。
 *
 * 普通 KV 命令没有额外的准备工作，直接把 prepare_status 设为成功；VSET 和
 * VSEARCH 则分别调用向量模块准备写入数据或查询向量。准备阶段可以在 embedding
 * worker 线程执行，但准备结果最终仍要回到 executor，由 executor 在数据锁保护下
 * 完成真正的提交或搜索。
 *
 * 状态字段的含义：
 * - prepare_completed=1：本次 prepare 已经结束，成功或失败都算结束；
 * - prepared_kind：记录是 VSET 还是 VSEARCH 的准备结果；
 * - prepared：指向 task 内部的 prepared_set/prepared_search；
 * - is_prepared=1：准备对象已经成功生成，可以交给 execute_handler；
 * - prepare_status：保存准备阶段的 RESP 长度或错误状态。
 *
 * 准备失败时会把向量错误转换成 RESP 写入 response，但不会执行 handler，也不应
 * 产生数据、AOF 或复制副作用。调用者仍负责后续 finalize response 和释放 task。
 */
int kvs_command_task_prepare(kvs_command_task_t *task) {
	/* 准备阶段的返回状态，同时用于写入 task->prepare_status。 */
	int status;
	/* response 指向 response_task 的缓冲区，准备失败时直接写错误 RESP。 */
	char *response = task ? kvs_response_task_buffer(task->response_task) : NULL;

	if (!task || !task->cmd) {
		/* task 或命令描述缺失时无法准备，返回客户端可读的错误。 */
		return resp_generate_simple_string_n(response, '-', "Invalid command", sizeof("Invalid command") - 1);
	}
	/* 记录当前响应缓冲区，并标记 prepare 已经开始/完成，避免重复派发。 */
	task->response = response;
	task->prepare_completed = 1;

	if (strcmp(task->cmd->name, "VSET") == 0) {
		/* VSET prepare 只生成私有 prepared 数据，不直接写入向量记录或索引。 */
		task->prepared_kind = KVS_COMMAND_TASK_PREPARED_VSET;
		task->prepared = &task->prepared_set;
		status = kvs_vector_set_prepare(
			task->ctx.query_text,
			task->ctx.query_text_len,
			task->ctx.value,
			task->ctx.value ? strlen(task->ctx.value) : 0,
			task->ctx.scope,
			task->ctx.scope_len,
			&task->prepared_set
		);
		if (status != KVS_VECTOR_OK) {
			/* 向量准备失败时转换成 RESP；executor 后续只发送错误，不执行提交。 */
			task->status = kvs_vector_status_to_resp(status, task->response);
			task->prepare_status = task->status;
			return task->status;
		}
		/* prepared_set 已经完整，可以由 executor 在写锁下提交。 */
		task->is_prepared = 1;
		task->prepare_status = 0;
		return 0;
	}

	if (strcmp(task->cmd->name, "VSEARCH") == 0) {
		/* VSEARCH prepare 负责生成查询 embedding 和查询参数，不修改数据。 */
		task->prepared_kind = KVS_COMMAND_TASK_PREPARED_VSEARCH;
		task->prepared = &task->prepared_search;
		status = kvs_vector_search_prepare(
			task->ctx.query_text,
			task->ctx.query_text_len,
			task->ctx.scope,
			task->ctx.scope_len,
			&task->prepared_search
		);
		if (status != KVS_VECTOR_OK) {
			/* 查询向量准备失败时，保存错误响应并阻止后续搜索。 */
			task->status = kvs_vector_status_to_resp(status, task->response);
			task->prepare_status = task->status;
			return task->status;
		}
		/* prepared_search 已经可交给 executor 执行搜索和组装响应。 */
		task->is_prepared = 1;
		task->prepare_status = 0;
		return 0;
	}

	/* 非向量命令没有额外准备阶段，直接标记成功。 */
	task->prepare_status = 0;
	return 0;
}

static int kvs_command_task_lock(kvs_command_task_t *task) {
	uint64_t start_us;
	uint64_t end_us;

	task->response = kvs_response_task_buffer(task->response_task);

	if (task->use_read_lock) {
		start_us = kvs_command_task_now_us();
		if (kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "command_task.execute.read") != 0) {
			task->status = resp_generate_simple_string_n(task->response, '-', "Failed to acquire data lock", sizeof("Failed to acquire data lock") - 1);
			return -1;
		}
		end_us = kvs_command_task_now_us();
		task->lock_wait_us += end_us >= start_us ? end_us - start_us : 0;
		task->lock_acquired_us = end_us;
		task->lock_held = 1;
		task->ctx.lock_mode = KVS_LOCK_MODE_READ;
	} else if (task->use_write_lock) {
		start_us = kvs_command_task_now_us();
		if (kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "command_task.execute.write") != 0) {
			task->status = resp_generate_simple_string_n(task->response, '-', "Failed to acquire data lock", sizeof("Failed to acquire data lock") - 1);
			return -1;
		}
		end_us = kvs_command_task_now_us();
		task->lock_wait_us += end_us >= start_us ? end_us - start_us : 0;
		task->lock_acquired_us = end_us;
		task->lock_held = 1;
		task->ctx.lock_mode = KVS_LOCK_MODE_WRITE;
	}

	return 0;
}

static void kvs_command_task_unlock(kvs_command_task_t *task) {
	uint64_t end_us;

	if (!task || !task->lock_held) {
		return;
	}
	end_us = kvs_command_task_now_us();
	if (task->lock_acquired_us > 0) {
		task->lock_hold_us += end_us >= task->lock_acquired_us ?
			end_us - task->lock_acquired_us :
			0;
	}
	kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "command_task.execute.unlock");
	task->lock_held = 0;
	task->lock_acquired_us = 0;
}

/*
 * 捕获命令执行后的 TTL 状态，供 kvs_command_task_post_commit 使用。
 *
 * 这个函数不修改存储、不写 AOF、也不直接发送复制消息。它只把 handler 执行
 * 后的结果保存到 task：
 * - EXPIRE/PEXPIREAT：读取 key 当前最终的绝对过期时间；
 * - 如果 key 已在命令执行中消失，则记录需要同步 DEL；
 * - PERSIST：记录是否需要同步 PERSIST。
 *
 * 之所以要在执行后再次读取 TTL，是因为相对过期时间可能已经被换算、覆盖或
 * 导致 key 删除。post_commit 需要一个确定的最终状态，才能把操作编码成可回放
 * 的 PEXPIREAT/DEL/PERSIST。只有真正提交成功的写命令才允许进入这一步。
 */
static void kvs_command_task_capture_post_state(kvs_command_task_t *task, int ret) {
	/*
	 * 读命令、失败的写命令和未提交的命令都不能产生持久化/复制副作用。
	 * task 和 task->cmd 由 execute_sync 的前置流程保证有效。
	 */
	if (!task->command_committed || !task->cmd->write_command) {
		return;
	}

	/*
	 * EXPIRE 使用相对时间，PEXPIREAT 使用绝对时间；两者最终都统一保存为
	 * expire_at_ms，后续 post_commit 可以稳定地生成 PEXPIREAT。
	 */
	if (strcmp(task->cmd->name, "EXPIRE") == 0 ||
	    strcmp(task->cmd->name, "PEXPIREAT") == 0) {
		/* 读取命令完成后 key 仍存在时的最终绝对过期时间。 */
		if (ttl_get_any_key_expire_at_ms(task->ctx.key, task->ctx.key_len, &task->expire_at_ms) > 0 &&
		    task->expire_at_ms >= 0) {
			/* post_commit 将使用这个时间生成确定性的 PEXPIREAT。 */
			task->has_expire_at = 1;
		} else {
			/*
			 * 命令返回成功但 key 已不存在时，后续复制/AOF 需要传播 DEL，
			 * 否则其他节点可能继续保留这个 key。
			 */
			task->need_del_sync = (ret > 0);
		}
	} else if (strcmp(task->cmd->name, "PERSIST") == 0) {
		/* PERSIST 成功才需要向 AOF/主从复制传播“清除过期时间”。 */
		task->need_persist_sync = (ret > 0);
	}
}

static int kvs_command_task_execute_handler(kvs_command_task_t *task) {
	int ret;

	task->response = kvs_response_task_buffer(task->response_task);

	if (strcmp(task->cmd->name, "VSET") == 0) {
		int status = kvs_vector_set_commit_prepared(&task->prepared_set);
		task->prepared_kind = KVS_COMMAND_TASK_PREPARED_NONE;
		task->command_committed = (status == KVS_VECTOR_OK);
		ret = kvs_vector_status_to_resp(status, task->response);
	} else if (strcmp(task->cmd->name, "VSEARCH") == 0) {
		uint64_t start_us = kvs_command_task_now_us();
		uint64_t end_us;
		ret = kvs_vector_search_prepared_to_resp(
			&task->ctx,
			&task->prepared_search,
			task->response
		);
		end_us = kvs_command_task_now_us();
		task->vsearch_search_us += end_us >= start_us ? end_us - start_us : 0;
		task->command_committed = (ret >= 0);
	} else {
		ret = task->cmd->handler(&task->ctx, task->response);
		task->command_committed = (ret >= 0);
	}

	return ret;
}

/*
 * 在当前 executor 线程中同步执行一条 command task。
 *
 * 执行顺序：
 * 1. 根据 build 阶段记录的 use_read_lock/use_write_lock 获取全局数据锁；
 * 2. 临时恢复 task 保存的 sync_context，调用具体命令 handler；
 * 3. 读路径发现惰性过期时，释放读锁、切换写锁并重试一次删除；
 * 4. 恢复调用前的同步上下文，记录 TTL/PERSIST 等 post-commit 所需状态；
 * 5. 释放数据锁，保存最终执行状态并返回结果。
 *
 * 本函数只负责“执行”和状态记录，不负责最终 response 入队。调用方随后会
 * 调用 kvs_command_task_post_commit 和 kvs_response_task_finalize。
 * task 必须已经完成 build；response_task、cmd 和 ctx 由 task 提前准备好。
 *
 * 返回值来自 handler 或响应生成函数：通常大于 0 表示 RESP 字节数，
 * 小于等于 0 表示命令失败或需要特殊处理。
 */
int kvs_command_task_execute_sync(kvs_command_task_t *task) {
	/* 当前命令的最终返回值，也会写入 task->status。 */
	int ret;
	/* 保存进入本函数前的线程本地复制上下文，结束时必须恢复。 */
	int old_sync_context;

	/* 写操作必须由 command owner 执行，防止其他线程直接修改数据库状态。 */
	if (task && task->use_write_lock) {
		KVS_COMMAND_OWNER_ASSERT_MUTATION("command_task.execute.write");
	}

	/* 根据 task 的锁标记获取读锁或写锁，并记录等待时间。 */
	if (kvs_command_task_lock(task) < 0) {
		/* 加锁失败时，lock 函数已生成错误响应并写入 task->status。 */
		return task->status;
	}

	/*
	 * handler 内部可能根据 sync_context 判断是普通客户端请求、复制应用还是
	 * 其他内部路径。只在当前命令执行期间切换，不能污染 executor 后续任务。
	 */
	old_sync_context = get_sync_context();
	set_sync_context(task->sync_context);

	/* VSET/VSEARCH 和普通 KV 命令在该函数内统一进入各自的 handler。 */
	ret = kvs_command_task_execute_handler(task);

	/*
	 * 惰性过期的读操作不能在读锁下删除数据。
	 * handler 返回 RETRY_WITH_WRITE_LOCK 时，先释放读锁，再改为写锁重试，
	 * 让第二次 handler 调用真正完成过期删除。
	 */
	if (task->use_read_lock && ret == KVS_CMD_RETRY_WITH_WRITE_LOCK) {
		KVS_COMMAND_OWNER_ASSERT_MUTATION("command_task.lazy_expire.retry");
		kvs_command_task_unlock(task);
		/* 本次重试不再走读锁分支，而是使用写锁。 */
		task->use_read_lock = 0;
		task->use_write_lock = 1;

		task->lock_acquired_us = 0;
		/* 单独统计重试阶段获取写锁的等待时间。 */
		uint64_t start_us = kvs_command_task_now_us();
		uint64_t end_us;
		/* 写锁失败时不能继续删除，先恢复线程同步上下文再返回错误。 */
			if (kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "command_task.lazy_expire.retry") != 0) {
				task->status = resp_generate_simple_string_n(task->response, '-', "Failed to acquire data lock", sizeof("Failed to acquire data lock") - 1);
				set_sync_context(old_sync_context);
				return task->status;
		}
		/* 写锁获取成功，记录等待时间和当前锁模式。 */
		end_us = kvs_command_task_now_us();
		task->lock_wait_us += end_us >= start_us ? end_us - start_us : 0;
		task->lock_acquired_us = end_us;
		task->lock_held = 1;
		task->ctx.lock_mode = KVS_LOCK_MODE_WRITE;
		/* 在写锁保护下重新调用原命令 handler，完成过期删除。 */
		ret = task->cmd->handler(&task->ctx, task->response);
		task->command_committed = (ret >= 0);
	}

	/* 不论普通执行还是惰性过期重试，都要恢复进入前的线程本地上下文。 */
	set_sync_context(old_sync_context);

	/*
	 * 读取 EXPIRE/PEXPIREAT/PERSIST 的最终状态，供 post_commit 生成确定的
	 * PEXPIREAT、DEL 或 PERSIST 复制/AOF 操作。
	 */
	kvs_command_task_capture_post_state(task, ret);

	/* 正常路径和异常路径都在这里释放仍由 task 持有的数据锁。 */
	if (task->lock_held) {
		kvs_command_task_unlock(task);
	}

	/* 保存最终返回值，供 executor finalize response 和统计使用。 */
	task->status = ret;
	return ret;
}


/*
 * 处理命令已经成功修改数据之后的后置副作用。
 *
 * 本函数不执行主命令 handler，也不生成客户端响应；它只负责把已经提交的
 * 写操作追加到 AOF，并在当前节点是 TCP 主节点时编码一条复制消息。调用者
 * 必须先完成 execute_sync，并设置 command_committed；失败的写命令、读命令和
 * 尚未提交的命令都不能进入这里。
 *
 * TTL 命令需要特殊处理：EXPIRE 的相对时间在执行后会被转换成最终绝对时间，
 * 因此 AOF/复制优先使用 PEXPIREAT；如果 key 已经消失则传播 DEL。PERSIST
 * 只有真正成功清除过期时间时才传播。sync_context==2 表示复制/内部回放路径，
 * 不能再次向外广播，否则会形成复制回环。
 */
void kvs_command_task_post_commit(kvs_command_task_t *task, int ret) {
	/* 只有成功提交的写命令才允许产生持久化和复制副作用。 */
	if (!task->command_committed || !task->cmd->write_command) {
		return;
	}

#if ENABLE_AOF
	/* AOF 正在加载旧日志时不能再次追加，避免回放过程重复写入。 */
	if (!aof_is_loading()) {
		if (strcmp(task->cmd->name, "EXPIRE") == 0) {
			/* EXPIRE 统一落成最终绝对过期时间，保证重放结果确定。 */
			if (task->has_expire_at) {
				char expire_buf[32];
				snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)task->expire_at_ms);
				char *aof_tokens[] = {"PEXPIREAT", task->ctx.key, expire_buf};
				aof_append(3, aof_tokens, NULL);
			} else if (task->need_del_sync) {
				/* 命令完成后 key 已不存在，AOF 必须记录 DEL，避免重启后残留。 */
				char *aof_tokens[] = {"DEL", task->ctx.key};
				aof_append(2, aof_tokens, NULL);
			}
		} else {
			/* 其他写命令按原始 argv 和长度追加，保留二进制安全参数。 */
			aof_append(task->argc, task->argv, task->argv_lens);
		}
	}
#endif

	/* 内部复制回放不再向其他节点广播，避免主从之间形成循环传播。 */
	if (task->sync_context == 2) {
		return;
	}

	/* 只有 TCP 主节点负责向从节点发送普通 KV/TTL 复制消息。 */
	if (global_dist_config.role == ROLE_MASTER &&
	    g_cfg.replication_transport == KVS_REPLICATION_TCP) {
		if (strcmp(task->cmd->name, "EXPIRE") == 0 ||
		    strcmp(task->cmd->name, "PEXPIREAT") == 0) {
			/* TTL 复制必须使用执行后的最终状态，而不是原始相对时间。 */
			if (ret > 0) {
				if (task->has_expire_at) {
					char expire_buf[32];
					snprintf(expire_buf, sizeof(expire_buf), "%lld", (long long)task->expire_at_ms);
					master_sync_command("PEXPIREAT", task->ctx.key, task->ctx.key_len, expire_buf, strlen(expire_buf));
				} else {
					master_sync_command("DEL", task->ctx.key, task->ctx.key_len, NULL, 0);
				}
			}
		} else if (strcmp(task->cmd->name, "PERSIST") == 0) {
			/* 只有真正清除 TTL 的 PERSIST 才需要复制。 */
			if (task->need_persist_sync) {
				master_sync_command("PERSIST", task->ctx.key, task->ctx.key_len, NULL, 0);
			}
		} else {
			/* 普通写命令按 command task 的稳定副本发送给从节点。 */
			master_sync_command(
				task->argv[0],
				task->argv[1],
				task->argv_lens[1],
				(task->argc > 2) ? task->argv[2] : NULL,
				(task->argc > 2) ? task->argv_lens[2] : 0
			);
		}
	}
}

uint64_t kvs_command_task_get_vsearch_search_us(const kvs_command_task_t *task) {
	return task ? task->vsearch_search_us : 0;
}
