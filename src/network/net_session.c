#include "net_session.h"
#include "kvs_api.h"
#include "kvs_log.h"
#include "kvs_globals.h"

#include <string.h>
#include <stdio.h>

static int g_net_session_connection_count = 0;

static size_t net_session_request_limit(void) {
	return g_cfg.network_max_request_bytes > 0 ?
		g_cfg.network_max_request_bytes : (size_t)NETBUF_MAX_CAPACITY;
}

static size_t net_session_response_limit(void) {
	return g_cfg.network_max_response_bytes > 0 ?
		g_cfg.network_max_response_bytes : (size_t)KVS_MAX_RESPONSE_LENGTH;
}

static int net_session_response_too_large(char *response) {
	return resp_generate_simple_string_n(
		response,
		'-',
		"ERR response exceeds network.max_response_bytes",
		sizeof("ERR response exceeds network.max_response_bytes") - 1
	);
}

static int net_session_command_limit(void) {
	return g_cfg.network_max_commands_per_read > 0 ?
		g_cfg.network_max_commands_per_read : 1024;
}

static int net_session_validate_command_batch(const net_session_t *session) {
	size_t offset = 0;
	int commands = 0;
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

//初始化会话
int net_session_init(net_session_t* session,
                     size_t recv_initial,
                     size_t send_initial){

    if(!session) return -1;

    memset(session, 0, sizeof(net_session_t));
	{
		int limit = g_cfg.network_max_connections > 0 ? g_cfg.network_max_connections : 1024;
		int count = __atomic_add_fetch(&g_net_session_connection_count, 1, __ATOMIC_ACQ_REL);
		if (count > limit) {
			__atomic_sub_fetch(&g_net_session_connection_count, 1, __ATOMIC_ACQ_REL);
			return -1;
		}
		session->connection_counted = 1;
	}

    if(netbuf_init_limit(&session->recv_buf, recv_initial, net_session_request_limit()) < 0){
		__atomic_sub_fetch(&g_net_session_connection_count, 1, __ATOMIC_ACQ_REL);
		session->connection_counted = 0;
        return -1;
    }

    if(netbuf_init_limit(&session->send_buf, send_initial, NETBUF_MAX_CAPACITY) < 0){
        netbuf_free(&session->recv_buf);
		__atomic_sub_fetch(&g_net_session_connection_count, 1, __ATOMIC_ACQ_REL);
		session->connection_counted = 0;
        return -1;
    }

    resp_parser_init(&session->resp_parser);
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

//释放会话资源
void net_session_free(net_session_t* session){
    if(!session) return;

    netbuf_free(&session->recv_buf);
    netbuf_free(&session->send_buf);

    //释放RESP解析器相关资源
    resp_parser_free(&session->resp_parser);
	if (session->connection_counted) {
		__atomic_sub_fetch(&g_net_session_connection_count, 1, __ATOMIC_ACQ_REL);
		session->connection_counted = 0;
	}

    memset(session, 0, sizeof(net_session_t));
}

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

void net_session_mark_closing(net_session_t *session) {
    if (!session) {
        return;
    }
    session->closing = 1;
}

int net_session_is_closing(const net_session_t *session) {
    return session ? session->closing : 1;
}

void net_session_ref_inc(net_session_t *session) {
    if (!session) {
        return;
    }
    __atomic_add_fetch(&session->refcount, 1, __ATOMIC_RELAXED);
}

int net_session_ref_dec(net_session_t *session) {
    int old_count;

    if (!session) {
        return 0;
    }

    old_count = __atomic_load_n(&session->refcount, __ATOMIC_ACQUIRE);
    while (old_count > 0) {
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

int net_session_ref_count(const net_session_t *session) {
    if (!session) {
        return 0;
    }
    return __atomic_load_n(&session->refcount, __ATOMIC_ACQUIRE);
}

uint64_t net_session_next_request_seq(net_session_t *session) {
    if (!session) {
        return 0;
    }
    return ++session->next_request_seq;
}

int net_session_try_begin_inflight(net_session_t *session) {
    if (!session || session->closing || session->single_inflight) {
        return 0;
    }
    session->single_inflight = 1;
    return 1;
}

void net_session_end_inflight(net_session_t *session) {
    if (!session) {
        return;
    }
    session->single_inflight = 0;
}

//处理当前recv_buf中的数据
/*
做一次协议对齐检查 首个非空白必须是 *
使用resp_peek_command + resp_parser 逐条解析命令
对每条命令调用handler 生成响应 并append到 send_buf

返回值
    >=0 处理的请求数
    <0 出错 建议上层关闭连接
*/
int net_session_process_resp(int fd,
                             net_session_t* session,
                             net_session_handler_fn handler,
                             void* user_data){
    
    if(!session || !handler) return -1;
	if (session->recv_buf.size > net_session_request_limit()) return -1;
	if (net_session_validate_command_batch(session) != 0) return -1;

    if(session->processing) return 0; //正在处理 避免重复调度
    session->processing = 1;

    resp_parser_t* parser = &session->resp_parser;

    //如果上次刚好为COMPLETE 说明上一条命令解析完成 重置状态
    if(parser->state == RESP_PARSE_COMPLETE){
        resp_parser_reset(parser);
    }

    //只在第一次做简单对齐检查 保证首个非空白为*
    if(!session->first_packet_checked 
        && session->recv_buf.size > 0 
        && parser->state == RESP_PARSE_INIT){
        
        size_t i = 0;
        while(i < session->recv_buf.size &&
                (session->recv_buf.data[i] == ' ' || 
                 session->recv_buf.data[i] == '\r' || 
                 session->recv_buf.data[i] == '\n' || 
                 session->recv_buf.data[i] == '\t')){
                i++;
            }
        
        if(i >= session->recv_buf.size){
            //全是空白 等待更多数据
            session->processing = 0;
            return 0;
        }

        if(session->recv_buf.data[i] != '*'){
            //不是合法RESP起始数组 认为协议错误
            KVS_LOG_ERROR("net", "fd=%d protocol error: first non-whitespace byte is not '*'", fd);
            session->processing = 0;
            return -1;
        }

        //丢弃之前的空白
        if(i > 0){
            netbuf_consume_front(&session->recv_buf, i);
        }

        session->first_packet_checked = 1;
    }

    size_t parsed_total = 0;//本轮在recv_buf中解析的总字节数
    int commands_count = 0;

    while(parsed_total < session->recv_buf.size
          && commands_count < net_session_command_limit()){

        //用无状态peek探测 当前位置是否有完整命令
        int cmd_len = resp_peek_command(session->recv_buf.data,
                                        session->recv_buf.size,
                                        parsed_total);
        
        if(cmd_len < 0){
            KVS_LOG_ERROR("net", "fd=%d incomplete command or protocol error detected by resp_peek_command", fd);
            session->processing = 0;
            return -1;
        }else if(cmd_len == 0){
            //不完整命令 等待更多数据
            break;
        }

        //确认有完整命令 使用有状态解析器解析
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
            //解析器没有消费完数据 或者没有完成解析 认为协议错误
            KVS_LOG_ERROR("net", "fd=%d protocol error: resp_parser did not consume expected bytes or did not complete", fd);
            session->processing = 0;
            return -1;
        }

        //一条完整命令解析成功 交给上层handler 直接写入 send_buf 尾部
        const size_t response_cap = KVS_MAX_RESPONSE_LENGTH;
        if (session->send_buf.capacity - session->send_buf.size < response_cap) {
            size_t needed = session->send_buf.size + response_cap;
            if (netbuf_reserve(&session->send_buf, needed) < 0) {
                KVS_LOG_ERROR("net", "fd=%d failed to reserve send buffer for response", fd);
                session->processing = 0;
                return -1;
            }
        }

        char *response_buf = session->send_buf.data + session->send_buf.size;

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
		if ((size_t)resp_len > net_session_response_limit()) {
			resp_len = net_session_response_too_large(response_buf);
		}

        session->send_buf.size += resp_len;
        commands_count++;

        //准备解析下一条命令
        parsed_total += cmd_len;
        resp_parser_reset(parser);
    }

    //将完全解析的数据裁剪掉 只留下未处理的半包
    if(parsed_total > 0){
        netbuf_consume_front(&session->recv_buf, parsed_total);
    }

    session->processing = 0;
    return commands_count;
                                
}

int net_session_parse_one_resp(int fd,
                               net_session_t *session,
                               size_t *cmd_len) {
    resp_parser_t *parser;
    int parsed;

    if (!session || !cmd_len) {
        return -1;
    }
    *cmd_len = 0;
	if (session->recv_buf.size > net_session_request_limit()) {
		return -1;
	}
	if (net_session_validate_command_batch(session) != 0) {
		return -1;
	}

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

    resp_parser_reset(parser);
    session->processing = 1;
    if (resp_parser(parser, session->recv_buf.data, (size_t)parsed) != parsed ||
        parser->state != RESP_PARSE_COMPLETE) {
        KVS_LOG_ERROR("net", "fd=%d protocol error: resp_parser did not consume expected bytes or did not complete", fd);
        session->processing = 0;
        return -1;
    }
    session->processing = 0;

    *cmd_len = (size_t)parsed;
    return 1;
}
