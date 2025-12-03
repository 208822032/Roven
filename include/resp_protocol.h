
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
#ifndef RESP_PROTOCOL_H
#define RESP_PROTOCOL_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <limits.h>

#include "kvs_memory_internal.h"  // for kvs_mem_stats_t snapshot in MEMSTATS

// #define MAX_BULK_LENGTH (512 * 1024 * 1024) // 512MB 最大批量字符串长度
#define MAX_BULK_LENGTH  (10 * 1048576) // 10MB 最大批量字符串长度
#define MAX_ARRAY_LENGTH 1024 * 1024        // 最大数组元素数量


//RESP 类型定义
typedef enum{
    RESP_SIMPLE_STRING,
    RESP_ERROR,
    RESP_INTEGER,
    RESP_BULK_STRING,
    RESP_ARRAY
} resp_type_t;

//RESP 解析器状态
typedef enum{
    RESP_PARSE_INIT,
    RESP_PARSE_ARRAY_LEN,
    RESP_PARSE_BULK_LEN,
    RESP_PARSE_BULK_LEN_NUM,
    RESP_PARSE_BULK_DATA,
    RESP_PARSE_COMPLETE
} resp_parse_state;

//RESP 解析器
typedef struct {
    resp_parse_state state;
    int array_len;   //数组长度

    int arg_count;   //已解析的参数个数
    int arg_index;   //当前解析的参数索引
    char **tokens;   //解析出来的tokens
    int array_len_allocated;

    int bulk_len;    //当前批量字符串长度
    int bulk_read;   //已经从body中读了多少字节

} resp_parser_t;



//初始化解析器
void resp_parser_init(resp_parser_t *parser);

//解析RESP协议
//返回 1解析完成 0需要更多数据 -1表示错误
int resp_parser(resp_parser_t *parser, const char *buffer, size_t length);

//释放解释器分配的内存
void resp_parser_free(resp_parser_t *parser);
// 重置解析器状态但不释放内存
void resp_parser_reset(resp_parser_t *parser);

//生成RESP响应
int resp_generate_simple_string(char *response, const char *str);
int resp_generate_error(char *response, const char *err);
int resp_generate_integer(char *response, int num);
int resp_generate_bulk_string(char *response, const char *str);
int resp_generate_array_start(char *response, int count);
int resp_generate_array_item(char *response, const char *str);

//处理RESP协议命令
int resp_process_command(char **tokens, int count, char *response);

//使用一个无状态reader只负责在一块连续内存里 从offset中算出第一个完整命令占用了多少字节
int resp_peek_command(const char* buf, size_t len, size_t offset);

size_t resp_estimate_simple_string_len(const char *str);
size_t resp_estimate_error_len(const char *err);
size_t resp_estimate_integer_len(long long num);
size_t resp_estimate_bulk_string_len(const char *str);

#endif