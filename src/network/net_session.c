#include "net_session.h"
#include "kvs_api.h"

#include <string.h>
#include <stdio.h>

//一次最多处理多少条命令 防止单连接长时间占用CPU
#ifndef NET_SESSION_MAX_COMMANDS_PER_BATCH
#define NET_SESSION_MAX_COMMANDS_PER_BATCH 10000000
#endif

//初始化会话
int net_session_init(net_session_t* session,
                     size_t recv_initial,
                     size_t send_initial){

    if(!session) return -1;
    
    memset(session, 0, sizeof(net_session_t));

    if(netbuf_init(&session->recv_buf, recv_initial) < 0){
        return -1;
    }

    if(netbuf_init(&session->send_buf, send_initial) < 0){
        netbuf_free(&session->recv_buf);
        return -1;
    }

    resp_parser_init(&session->resp_parser);
    session->processing = 0;
    session->first_packet_checked = 0;
    return 0;
}

//释放会话资源
void net_session_free(net_session_t* session){
    if(!session) return;

    netbuf_free(&session->recv_buf);
    netbuf_free(&session->send_buf);

    //释放RESP解析器相关资源
    resp_parser_free(&session->resp_parser);

    memset(session, 0, sizeof(net_session_t));
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
            fprintf(stderr, "Protocol error: first non-whitespace byte is not '*'\n");
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
          && commands_count < NET_SESSION_MAX_COMMANDS_PER_BATCH){

        //用无状态peek探测 当前位置是否有完整命令
        int cmd_len = resp_peek_command(session->recv_buf.data,
                                        session->recv_buf.size,
                                        parsed_total);
        
        if(cmd_len < 0){
            printf("Incomplete command or protocol error detected by resp_peek_command\n");
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
            printf("Protocol error detected by resp_parser\n");
            session->processing = 0;
            return -1;
        }

        if(parsed != cmd_len || parser->state != RESP_PARSE_COMPLETE){
            //解析器没有消费完数据 或者没有完成解析 认为协议错误
            printf("Protocol error: resp_parser did not consume expected bytes or did not complete\n");
            session->processing = 0;
            return -1;
        }

        //一条完整命令解析成功 交给上层handler 生成响应
        char* response_buf = kvs_malloc(KVS_MAX_RESPONSE_LENGTH);
        if(!response_buf){
            printf("Memory allocation failed for response buffer\n");
            session->processing = 0;
            return -1;
        }

        int resp_len = handler(
            parser,
            response_buf,
            KVS_MAX_RESPONSE_LENGTH,
            user_data
        );
        if(resp_len <= 0 || resp_len > (int)KVS_MAX_RESPONSE_LENGTH){
            printf("Handler returned invalid response length: %d\n", resp_len);
            kvs_free(response_buf);
            session->processing = 0;
            return -1;
        }

        //将响应追加到发送缓冲区
        if(session->send_buf.size + resp_len > session->send_buf.capacity){
            size_t needed = session->send_buf.size + resp_len;
            if(netbuf_reserve(&session->send_buf, needed) < 0){
                printf("Failed to reserve space in send buffer\n");
                kvs_free(response_buf);
                session->processing = 0;
                return -1;
            }
        }

        memcpy(session->send_buf.data + session->send_buf.size,
               response_buf,
               resp_len);
        session->send_buf.size += resp_len;

        kvs_free(response_buf);
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
