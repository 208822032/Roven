
#include "resp_protocol.h"
#include "kvstore.h"

//模仿redis RESP协议
/*
 1. 简单字符串：以"+"开头，以"\r\n"结尾。例如："+OK\r\n"
 2. 错误：以"-"开头，以"\r\n"结尾。例如："-Error message\r\n"
 3. 整数：以":"开头，以"\r\n"结尾。例如：":1000\r\n"
 4. 批量字符串：以"$"开头，后面跟字符串长度，然后是"\r\n"，然后是字符串，
    最后是"\r\n"。如果字符串为空，则为"$-1\r\n"。例如："$5\r\nhello\r\n"
 5. 数组：以"*"开头，后面跟数组元素个数，然后是"\r\n"，然后每个元素按照自己的类型编码。
    例如：数组["SET", "key", "value"]编码为："*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n"
*/

//初始化解析器
void resp_parser_init(resp_parser_t *parser){
    // parser->state = RESP_PARSE_INIT;
    // parser->array_len = 0;
    // parser->bulk_len = 0;
    // parser->arg_count = 0;
    // parser->arg_index = 0;
    // parser->tokens = NULL;

    memset(parser, 0, sizeof(resp_parser_t));
    parser->state = RESP_PARSE_INIT;
}

// 重置解析器状态但不释放内存
void resp_parser_reset(resp_parser_t *parser) {
    // 保留缓冲区相关状态，重置解析状态
    parser->state = RESP_PARSE_INIT;
    parser->array_len = 0;
    parser->bulk_len = 0;
    parser->arg_index = 0;
    
    // 但保留tokens指针，避免重复分配
    if (parser->tokens) {
        for (int i = 0; i < parser->array_len_allocated; i++) {
            if (parser->tokens[i]) {
                kvs_free(parser->tokens[i]);
                parser->tokens[i] = NULL;
            }
        }
    }
}


//解析RESP协议
int resp_parser(resp_parser_t *parser, const char *buffer, size_t length) {
    const char *ptr = buffer;
    const char *end = buffer + length;
    size_t parsed = 0;

    // 如果解析器已经完成解析，重置状态
    if (parser->state == RESP_PARSE_COMPLETE) {
        resp_parser_reset(parser);
    }

    while (ptr < end) {
        switch (parser->state) {
            case RESP_PARSE_INIT:
                // 查找数组起始符'*'
                while (ptr < end && *ptr != '*') {
                    ptr++;
                    parsed++;
                }
                
                if (ptr >= end) {
                    return parsed; // 需要更多数据
                }
                
                parser->state = RESP_PARSE_ARRAY_LEN;
                parser->array_len = 0;
                ptr++; // 跳过'*'
                parsed++;
                break;

            case RESP_PARSE_ARRAY_LEN:
                // 读取数组长度数字
                while (ptr < end && isdigit(*ptr)) {
                    parser->array_len = parser->array_len * 10 + (*ptr - '0');
                    ptr++;
                    parsed++;
                }
                
                if (ptr >= end) {
                    return parsed; // 需要更多数据
                }
                
                // 检查CRLF
                if (*ptr == '\r') {
                    if (ptr + 1 >= end) {
                        return parsed; // 需要更多数据
                    }
                    
                    if (*(ptr + 1) == '\n') {
                        ptr += 2;
                        parsed += 2;
                        
                        // 分配tokens数组
                        if (parser->array_len > 0) {
                            // 如果之前没有分配或者大小不够，重新分配
                            if (!parser->tokens || parser->array_len_allocated < parser->array_len) {
                                if (parser->tokens) {
                                    kvs_free(parser->tokens);
                                }
                                parser->tokens = (char **)kvs_malloc(parser->array_len * sizeof(char *));
                                if (!parser->tokens) {
                                    return -1;
                                }
                                memset(parser->tokens, 0, parser->array_len * sizeof(char *));
                                parser->array_len_allocated = parser->array_len;
                            }
                        }
                        
                        parser->arg_index = 0;
                        parser->state = RESP_PARSE_BULK_LEN;
                    } else {
                        return -1; // 格式错误
                    }
                } else {
                    return -1; // 格式错误
                }
                break;

            case RESP_PARSE_BULK_LEN:
                // 查找批量字符串起始符'$'
                while (ptr < end && *ptr != '$') {
                    ptr++;
                    parsed++;
                }
                
                if (ptr >= end) {
                    return parsed; // 需要更多数据
                }
                
                ptr++; // 跳过'$'
                parsed++;
                parser->bulk_len = 0;
                parser->state = RESP_PARSE_BULK_LEN_NUM;
                break;
                
            case RESP_PARSE_BULK_LEN_NUM:
                // 读取批量字符串长度数字
                while (ptr < end && isdigit(*ptr)) {
                    parser->bulk_len = parser->bulk_len * 10 + (*ptr - '0');
                    ptr++;
                    parsed++;
                }
                
                if (ptr >= end) {
                    return parsed; // 需要更多数据
                }
                
                // 检查CRLF
                if (*ptr == '\r') {
                    if (ptr + 1 >= end) {
                        return parsed; // 需要更多数据
                    }
                    
                    if (*(ptr + 1) == '\n') {
                        ptr += 2;
                        parsed += 2;
                        
                        // 特殊处理：NULL批量字符串
                        if (parser->bulk_len == -1) {
                            if (parser->arg_index < parser->array_len) {
                                parser->tokens[parser->arg_index] = NULL;
                                parser->arg_index++;
                            }
                            parser->state = (parser->arg_index < parser->array_len) ? 
                                RESP_PARSE_BULK_LEN : RESP_PARSE_COMPLETE;
                        } else {
                            parser->state = RESP_PARSE_BULK_DATA;
                        }
                    } else {
                        return -1; // 格式错误
                    }
                } else {
                    return -1; // 格式错误
                }
                break;

            case RESP_PARSE_BULK_DATA:
                // 检查是否有足够的数据
                if ((end - ptr) < (parser->bulk_len + 2)) {
                    return parsed; // 需要更多数据
                }
                
                // 分配空间并复制数据
                if (parser->arg_index < parser->array_len) {
                    parser->tokens[parser->arg_index] = kvs_malloc(parser->bulk_len + 1);
                    if (!parser->tokens[parser->arg_index]) {
                        return -1;
                    }
                    memcpy(parser->tokens[parser->arg_index], ptr, parser->bulk_len);
                    parser->tokens[parser->arg_index][parser->bulk_len] = '\0';
                    parser->arg_index++;
                }
                
                ptr += parser->bulk_len;
                parsed += parser->bulk_len;
                
                // 检查结束符
                if (*ptr == '\r' && *(ptr + 1) == '\n') {
                    ptr += 2;
                    parsed += 2;
                    
                    // 如果还有更多参数，继续解析
                    if (parser->arg_index < parser->array_len) {
                        parser->state = RESP_PARSE_BULK_LEN;
                    } else {
                        // 解析完成
                        parser->state = RESP_PARSE_COMPLETE;
                        return parsed;
                    }
                } else {
                    return -1; // 格式错误
                }
                break;
                
            case RESP_PARSE_COMPLETE:
                // 已经完成解析，应该重置状态
                return parsed;
                
            default:
                return -1; // 未知状态
        }
    }
    
    return parsed; // 返回已解析的字节数
}

//释放解释器分配的内存
void resp_parser_free(resp_parser_t *parser) {
    if (parser->tokens) {
        for (int i = 0; i < parser->array_len_allocated; i++) {
            if (parser->tokens[i]) {
                kvs_free(parser->tokens[i]);
            }
        }
        kvs_free(parser->tokens);
        parser->tokens = NULL;
    }
    parser->array_len_allocated = 0;
    parser->arg_index = 0;
    parser->array_len = 0;
}

//生成RESP响应
//简单字符串
int resp_generate_simple_string(char *response, const char *str){
    return sprintf(response, "+%s\r\n", str);
}
//错误信息
int resp_generate_error(char *response, const char *err){
    return sprintf(response, "-%s\r\n", err);
}
//整数类型
int resp_generate_integer(char *response, int num){
    return sprintf(response, ":%d\r\n", num);
}
int resp_generate_bulk_string(char *response, const char *str){
    if(str){
        return sprintf(response, "$%zu\r\n%s\r\n", strlen(str), str);
    }else{
        return sprintf(response, "$-1\r\n");
    }
}
int resp_generate_array_start(char *response, int count){
    return sprintf(response, "*%d\r\n", count);
}
int resp_generate_array_item(char *response, const char *str){
    return resp_generate_bulk_string(response, str);
}

//处理RESP协议命令
int resp_process_command(char **tokens, int count, char *response){
    //调用原有的命令处理函数
    return kvs_filter_protocol(tokens, count, response);
}

//简单辅助函数
int resp_parse_aof_command(const char *line, char ***tokens, int *count){
    resp_parser_t parser;
    resp_parser_init(&parser);

    int parsed = resp_parser(&parser, line, strlen(line));
    if (parsed > 0 && parser.state == RESP_PARSE_COMPLETE) {
        *tokens = parser.tokens;
        *count = parser.arg_index;
        return 0;
    }

    resp_parser_free(&parser);
    return -1;
}