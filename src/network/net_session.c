#include "net_session.h"
#include "kvs_api.h"
#include "kvs_log.h"
#include "kvs_globals.h"

#include <string.h>
#include <stdio.h>

/*
 * 当前进程已成功初始化的 net_session 数量。
 * accept 路径可能并发创建连接，因此使用 GCC 原子操作增减该计数。
 */
static int g_net_session_connection_count = 0;

/* 返回单个连接允许保留的最大请求字节数；未配置时使用 netbuf 默认上限。 */
static size_t net_session_request_limit(void) {
	return g_cfg.network_max_request_bytes > 0 ?
		g_cfg.network_max_request_bytes : (size_t)NETBUF_MAX_CAPACITY;
}

/* 返回单个响应允许发送的最大字节数；未配置时使用编译期响应上限。 */
static size_t net_session_response_limit(void) {
	return g_cfg.network_max_response_bytes > 0 ?
		g_cfg.network_max_response_bytes : (size_t)KVS_MAX_RESPONSE_LENGTH;
}

/*
 * 在调用方提供的响应缓冲区写入统一的“响应过大”RESP 错误。
 * 这个函数只生成错误文本，调用方负责将返回长度写回 send_buf。
 */
static int net_session_response_too_large(char *response) {
	return resp_generate_simple_string_n(
		response,
		'-',
		"ERR response exceeds network.max_response_bytes",
		sizeof("ERR response exceeds network.max_response_bytes") - 1
	);
}

/* 返回一次读取后最多允许处理多少条 Pipeline 命令，防止单连接长期独占事件循环。 */
static int net_session_command_limit(void) {
	return g_cfg.network_max_commands_per_read > 0 ?
		g_cfg.network_max_commands_per_read : 1024;
}

/*
 * 预扫描当前 recv_buf 中已经完整的命令数量。
 *
 * 返回 0 表示命令数量未超限，且允许尾部存在半包；返回 -1 表示 RESP 格式
 * 错误或完整命令数超过 network_max_commands_per_read。这个预检查避免后续
 * 处理到一半才发现恶意 Pipeline 过长。
 */
static int net_session_validate_command_batch(const net_session_t *session) {
	/* 已确认完整命令的起始偏移。 */
	size_t offset = 0;
	/* 已确认完整命令的数量。 */
	int commands = 0;
	/* 当前连接本轮处理允许的最大命令数。 */
	int limit = net_session_command_limit();

	while (offset < session->recv_buf.size) {
		int command_len = resp_peek_command(
			session->recv_buf.data,
			session->recv_buf.size,
			offset
		);
		if (command_len < 0) {
			return -1;
		}
		if (command_len == 0) {
			return 0;
		}
		commands++;
		if (commands > limit) {
			return -1;
		}
		offset += (size_t)command_len;
	}
	return 0;
}

/*
 * 创建一个可供网络模型使用的会话。
 *
 * 顺序是：先清空旧状态并占用连接名额，再创建 recv/send 缓冲区，最后初始化
 * RESP parser 与异步生命周期字段。任一步骤失败都会归还此前已占用的资源。
 */
int net_session_init(net_session_t* session,
                     size_t recv_initial,
                     size_t send_initial){

    if(!session) return -1;

    /* 清除调用方可能留下的旧指针和状态；调用方应先保证旧资源已释放。 */
    memset(session, 0, sizeof(net_session_t));
	{
		/* 连接上限来自统一配置；缺省时最多允许 1024 个会话。 */
		int limit = g_cfg.network_max_connections > 0 ? g_cfg.network_max_connections : 1024;
		/* 原子加一后得到本次创建后的连接数。 */
		int count = __atomic_add_fetch(&g_net_session_connection_count, 1, __ATOMIC_ACQ_REL);
		/* 超出上限时立即撤销本次加一，不创建半初始化会话。 */
		if (count > limit) {
			__atomic_sub_fetch(&g_net_session_connection_count, 1, __ATOMIC_ACQ_REL);
			return -1;
		}
		session->connection_counted = 1;
	}

	/* recv_buf 用于保留半包，最大容量受 network_max_request_bytes 限制。 */
    if(netbuf_init_limit(&session->recv_buf, recv_initial, net_session_request_limit()) < 0){
		__atomic_sub_fetch(&g_net_session_connection_count, 1, __ATOMIC_ACQ_REL);
		session->connection_counted = 0;
        return -1;
    }

	/* send_buf 保存未完成的响应写；它可按 netbuf 默认上限增长。 */
    if(netbuf_init_limit(&session->send_buf, send_initial, NETBUF_MAX_CAPACITY) < 0){
        netbuf_free(&session->recv_buf);
		__atomic_sub_fetch(&g_net_session_connection_count, 1, __ATOMIC_ACQ_REL);
		session->connection_counted = 0;
        return -1;
    }

    /* parser 保存 RESP 解析过程和最终 tokens/token_lens。 */
    resp_parser_init(&session->resp_parser);

    /* 以下字段由网络 owner 在连接生命周期内维护。 */
    session->processing = 0;
    session->first_packet_checked = 0;
    session->session_id = 0;
    session->generation = 0;
    session->closing = 0;
    __atomic_store_n(&session->refcount, 1, __ATOMIC_RELAXED);
    session->owner_poller_id = -1;
    session->next_request_seq = 0;
    session->single_inflight = 0;
    return 0;
}

/*
 * 销毁会话内部资源。
 * 调用者必须先通过 closing/refcount 确认没有异步 command task 或 response
 * task 仍会访问该会话；本函数本身不等待其他线程，也不释放外层连接对象。
 */
void net_session_free(net_session_t* session){
    if(!session) return;

    /* 依次释放收包、发包和协议解析器分配的内存。 */
    netbuf_free(&session->recv_buf);
    netbuf_free(&session->send_buf);

    resp_parser_free(&session->resp_parser);

	/* 只有 init 成功计入过的会话才能归还连接名额，避免重复 free 使计数变负。 */
	if (session->connection_counted) {
		__atomic_sub_fetch(&g_net_session_connection_count, 1, __ATOMIC_ACQ_REL);
		session->connection_counted = 0;
	}

    memset(session, 0, sizeof(net_session_t));
}

/*
 * 为新建或复用的连接写入异步回包身份。
 * refcount 重新从网络 owner 的 1 开始；请求序号和在途标记也从空状态开始。
 */
void net_session_set_identity(net_session_t *session,
                              uint64_t session_id,
                              uint32_t generation,
                              int owner_poller_id) {
    if (!session) {
        return;
    }
    session->session_id = session_id;
    session->generation = generation;
    session->owner_poller_id = owner_poller_id;
    session->closing = 0;
    __atomic_store_n(&session->refcount, 1, __ATOMIC_RELAXED);
    session->next_request_seq = 0;
    session->single_inflight = 0;
}

/* 标记关闭只改变会话状态；真正关闭 fd 和释放外层连接对象由网络模型负责。 */
void net_session_mark_closing(net_session_t *session) {
    if (!session) {
        return;
    }
    session->closing = 1;
}

/* NULL 会话无法接收响应，因此按“已关闭”处理。 */
int net_session_is_closing(const net_session_t *session) {
    return session ? session->closing : 1;
}

/*
 * 为异步 task 或 response 增加一个连接引用。
 * 这里使用 relaxed，因为引用计数只负责对象存活数量，不用它传递数据内容。
 */
void net_session_ref_inc(net_session_t *session) {
    if (!session) {
        return;
    }
    __atomic_add_fetch(&session->refcount, 1, __ATOMIC_RELAXED);
}

/*
 * 通过 CAS 循环减少引用，避免并发释放把 refcount 减到负数。
 * 成功时返回新的计数；已经为 0 时保持 0 并返回 0。
 */
int net_session_ref_dec(net_session_t *session) {
	/* CAS 期望值；失败时 compare_exchange 会把最新值写回这里。 */
    int old_count;

    if (!session) {
        return 0;
    }

    old_count = __atomic_load_n(&session->refcount, __ATOMIC_ACQUIRE);
    while (old_count > 0) {
		/* 本次希望写入的剩余引用数。 */
        int new_count = old_count - 1;
        if (__atomic_compare_exchange_n(&session->refcount,
                                        &old_count,
                                        new_count,
                                        0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return new_count;
        }
    }
    return 0;
}

/* 原子读取引用数，供关闭路径判断是否还能安全释放外层对象。 */
int net_session_ref_count(const net_session_t *session) {
    if (!session) {
        return 0;
    }
    return __atomic_load_n(&session->refcount, __ATOMIC_ACQUIRE);
}

/*
 * 为当前网络 owner 接收到的下一条命令编号。
 * 该函数不使用原子加法，前提是同一 session 只由所属网络线程调用。
 */
uint64_t net_session_next_request_seq(net_session_t *session) {
    if (!session) {
        return 0;
    }
    return ++session->next_request_seq;
}

/*
 * 同一连接一次只允许一条命令等待 executor 响应。
 * 这样后续 Pipeline 命令必须等待前一条响应处理完成，响应顺序不会被打乱。
 */
int net_session_try_begin_inflight(net_session_t *session) {
    if (!session || session->closing || session->single_inflight) {
        return 0;
    }
    session->single_inflight = 1;
    return 1;
}

/* 响应已写入 send_buf 或被丢弃后，允许网络 owner 继续提交下一条命令。 */
void net_session_end_inflight(net_session_t *session) {
    if (!session) {
        return;
    }
    session->single_inflight = 0;
}

/*
 * 同步处理 recv_buf 中尽可能多的完整 RESP 命令。
 *
 * 此路径直接调用 handler，并把每条响应追加到 send_buf。它适合同步回调：
 * 不会把当前 parser 的 tokens 留给其他线程。recv_buf 尾部若只有半包，
 * 函数会正常返回并保留半包，等待下一次 recv 再继续解析。
 */
int net_session_process_resp(int fd,
                             net_session_t* session,
                             net_session_handler_fn handler,
                             void* user_data){
    
    /* session 或 handler 不存在时无法继续处理。 */
    if(!session || !handler) return -1;
	/* 单条请求缓冲超过配置上限，直接拒绝，避免恶意客户端耗尽内存。 */
	if (session->recv_buf.size > net_session_request_limit()) return -1;
	/* 预检查完整 Pipeline 数量和 RESP 帧边界。 */
	if (net_session_validate_command_batch(session) != 0) return -1;

	/* 防止同一网络 owner 在一次处理尚未结束时重入。 */
    if(session->processing) return 0; //正在处理 避免重复调度
    session->processing = 1;

    /* 本会话唯一的 RESP parser；每条完整命令结束后会 reset。 */
    resp_parser_t* parser = &session->resp_parser;

    /* 上一次调用若恰好停在 COMPLETE，先回到可解析下一条命令的初始状态。 */
    if(parser->state == RESP_PARSE_COMPLETE){
        resp_parser_reset(parser);
    }

    /*
     * 第一次收到数据时允许跳过前导空白，但第一个有效字节必须是 RESP 数组
     * 前缀 '*'。这是一层快速协议对齐检查，不替代后面的完整 RESP 校验。
     */
    if(!session->first_packet_checked 
        && session->recv_buf.size > 0 
        && parser->state == RESP_PARSE_INIT){
        
        /* 扫描前导空白的下标。 */
        size_t i = 0;
        while(i < session->recv_buf.size &&
                (session->recv_buf.data[i] == ' ' || 
                 session->recv_buf.data[i] == '\r' || 
                 session->recv_buf.data[i] == '\n' || 
                 session->recv_buf.data[i] == '\t')){
                i++;
            }
        
        if(i >= session->recv_buf.size){
            /* 当前缓冲区全是空白，尚不能判定协议错误，保留数据等待后续字节。 */
            session->processing = 0;
            return 0;
        }

        if(session->recv_buf.data[i] != '*'){
            /* 首个有效字节不是 '*'，不是本服务接受的 RESP 命令格式。 */
            KVS_LOG_ERROR("net", "fd=%d protocol error: first non-whitespace byte is not '*'", fd);
            session->processing = 0;
            return -1;
        }

        /* 前导空白已经不属于命令内容，可以安全从 receive buffer 删除。 */
        if(i > 0){
            netbuf_consume_front(&session->recv_buf, i);
        }

        session->first_packet_checked = 1;
    }

    /* 本轮已完整处理的 recv_buf 字节数，结束时统一从缓冲区前端删除。 */
    size_t parsed_total = 0;
    /* 本轮已提交给同步 handler 的完整命令数。 */
    int commands_count = 0;

    while(parsed_total < session->recv_buf.size
          && commands_count < net_session_command_limit()){

        /*
         * peek 只计算当前位置一条 RESP 命令是否完整及其字节长度，
         * 不修改 parser。这样半包不会污染 parser 的中间状态。
         */
        int cmd_len = resp_peek_command(session->recv_buf.data,
                                        session->recv_buf.size,
                                        parsed_total);
        
        if(cmd_len < 0){
            KVS_LOG_ERROR("net", "fd=%d incomplete command or protocol error detected by resp_peek_command", fd);
            session->processing = 0;
            return -1;
        }else if(cmd_len == 0){
            /* 尾部是半包：保留它，下一次 recv 后从同一位置继续探测。 */
            break;
        }

        /* 已知帧完整后，才交给 parser 生成 tokens 和每个参数的真实长度。 */
        if(parser->state == RESP_PARSE_INIT &&
           parser->state != RESP_PARSE_COMPLETE){
            resp_parser_reset(parser);
        }

        int parsed = resp_parser(
            parser,
            session->recv_buf.data + parsed_total,
            cmd_len
        );

        if(parsed < 0){
            KVS_LOG_ERROR("net", "fd=%d protocol error detected by resp_parser", fd);
            session->processing = 0;
            return -1;
        }

        if(parsed != cmd_len || parser->state != RESP_PARSE_COMPLETE){
            /* 完整帧却未被完整消费，说明协议内容或 parser 状态不一致。 */
            KVS_LOG_ERROR("net", "fd=%d protocol error: resp_parser did not consume expected bytes or did not complete", fd);
            session->processing = 0;
            return -1;
        }

        /*
         * 一条完整命令解析成功。handler 的响应直接写在 send_buf 尾部，
         * 因此先保留 KVS_MAX_RESPONSE_LENGTH 的可写空间。
         */
        const size_t response_cap = KVS_MAX_RESPONSE_LENGTH;
        if (session->send_buf.capacity - session->send_buf.size < response_cap) {
            size_t needed = session->send_buf.size + response_cap;
            if (netbuf_reserve(&session->send_buf, needed) < 0) {
                KVS_LOG_ERROR("net", "fd=%d failed to reserve send buffer for response", fd);
                session->processing = 0;
                return -1;
            }
        }

        /* response_buf 指向当前 send_buf 的空闲起点。 */
        char *response_buf = session->send_buf.data + session->send_buf.size;

        /* handler 返回实际写入的 RESP 字节数。 */
        int resp_len = handler(
            parser,
            response_buf,
            response_cap,
            user_data
        );
        if(resp_len <= 0 || (size_t)resp_len > response_cap){
            KVS_LOG_ERROR("net", "fd=%d handler returned invalid response length: %d", fd, resp_len);
            session->processing = 0;
            return -1;
        }
		/* 即使 handler 成功，也不能突破配置的单响应大小限制。 */
		if ((size_t)resp_len > net_session_response_limit()) {
			resp_len = net_session_response_too_large(response_buf);
		}

        session->send_buf.size += resp_len;
        commands_count++;

        /* 移动到下一条 Pipeline 命令，并重置 parser 复用其内部缓冲。 */
        parsed_total += cmd_len;
        resp_parser_reset(parser);
    }

    /* 删除已处理完整帧，只留下未处理的半包或本轮限额后的命令。 */
    if(parsed_total > 0){
        netbuf_consume_front(&session->recv_buf, parsed_total);
    }

    session->processing = 0;
    return commands_count;
                                
}

/*
 * 异步网络模型使用的“只解析一条”入口。
 *
 * 它只让 parser 暴露一条完整命令的 tokens/token_lens，不调用 handler，也不
 * 消费 recv_buf。调用方需要先把参数复制到 command task，成功提交后自行消费
 * *cmd_len 字节。这样 parser 参数不会在 executor 异步执行时失效。
 */
int net_session_parse_one_resp(int fd,
                               net_session_t *session,
                               size_t *cmd_len) {
    /* 当前会话的 RESP parser。 */
    resp_parser_t *parser;
    /* peek 得到的完整命令长度，以及 parser 实际消费的长度。 */
    int parsed;

    /* cmd_len 是输出参数；没有它就无法让调用方从 recv_buf 消费正确字节数。 */
    if (!session || !cmd_len) {
        return -1;
    }
    *cmd_len = 0;
	/* 请求缓冲或 Pipeline 预检查失败时，交给网络层关闭连接。 */
	if (session->recv_buf.size > net_session_request_limit()) {
		return -1;
	}
	if (net_session_validate_command_batch(session) != 0) {
		return -1;
	}

    /* 当前 session 已在处理另一条命令时，不重复进入 parser。 */
    if (session->processing) {
        return 0;
    }

    parser = &session->resp_parser;
    if (parser->state == RESP_PARSE_COMPLETE) {
        resp_parser_reset(parser);
    }

    if (!session->first_packet_checked
        && session->recv_buf.size > 0
        && parser->state == RESP_PARSE_INIT) {
        /* 扫描前导空白的下标。 */
        size_t i = 0;
        while (i < session->recv_buf.size &&
               (session->recv_buf.data[i] == ' ' ||
                session->recv_buf.data[i] == '\r' ||
                session->recv_buf.data[i] == '\n' ||
                session->recv_buf.data[i] == '\t')) {
            i++;
        }

        if (i >= session->recv_buf.size) {
            return 0;
        }

        if (session->recv_buf.data[i] != '*') {
            KVS_LOG_ERROR("net", "fd=%d protocol error: first non-whitespace byte is not '*'", fd);
            return -1;
        }

        if (i > 0) {
            netbuf_consume_front(&session->recv_buf, i);
        }

        session->first_packet_checked = 1;
    }

    /* 仅从 receive buffer 开头探测一条完整命令。 */
    parsed = resp_peek_command(session->recv_buf.data,
                               session->recv_buf.size,
                               0);
    if (parsed < 0) {
        KVS_LOG_ERROR("net", "fd=%d incomplete command or protocol error detected by resp_peek_command", fd);
        return -1;
    }
    if (parsed == 0) {
        return 0;
    }

    /*
     * 完整帧已确认，重置 parser 后再解析，保证 tokens 只对应本条命令。
     * processing 在 parser 调用期间为 1，避免重入；之后立即恢复为 0，
     * 因为异步“在途”限制由 single_inflight 而非该字段负责。
     */
    resp_parser_reset(parser);
    session->processing = 1;
    if (resp_parser(parser, session->recv_buf.data, (size_t)parsed) != parsed ||
        parser->state != RESP_PARSE_COMPLETE) {
        KVS_LOG_ERROR("net", "fd=%d protocol error: resp_parser did not consume expected bytes or did not complete", fd);
        session->processing = 0;
        return -1;
    }
    session->processing = 0;

    /* 将完整帧长度交给网络提交路径；此函数本身不移动 recv_buf。 */
    *cmd_len = (size_t)parsed;
    return 1;
}
