//网络会话
#ifndef NET_SESSION_H
#define NET_SESSION_H

#include <stddef.h>

#include "resp_protocol.h"
#include "netbuf.h"

//通用会话 封装recv/send 大缓冲区 + RESP解析状态
typedef struct net_session_s {
    netbuf_t recv_buf; //接收缓冲区
    netbuf_t send_buf; //发送缓冲区
    resp_parser_t resp_parser; //RESP解析器状态

    int processing; //是否正在处理请求 避免重复调度
    int first_packet_checked; //是否已经检查过首包
} net_session_t;

//上层回调 给一条完整命令 生成响应
//返回 响应字节数 或 <= 表示错误
typedef int (*net_session_handler_fn)(
        resp_parser_t* parser,
        char* response_buf,
        size_t response_buf_cap,
        void* user_data);

//初始化会话
int net_session_init(net_session_t* session,
                     size_t recv_initial,
                     size_t send_initial);

//释放会话资源
void net_session_free(net_session_t* session);

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
                             void* user_data);

#endif 