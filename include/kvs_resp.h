#pragma once

// 协议过滤/解析到命令响应
int kvs_filter_protocol(char **tokens, int count, size_t *token_lens, char *response);

