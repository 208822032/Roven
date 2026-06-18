#pragma once

// 服务器网络启动接口与消息回调类型
typedef int (*msg_handler)(char **tokens, int count, size_t *token_lens, char *response);

int reactor_start(unsigned short port, msg_handler handler);
int proactor_start(unsigned short port, msg_handler handler);
int ntyco_start(unsigned short port, msg_handler handler);
void ntyco_request_shutdown(void);

