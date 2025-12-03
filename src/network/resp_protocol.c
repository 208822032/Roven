
#include "resp_protocol.h"
#include "kvs_api.h"
#include <ctype.h>

#define DUMP_NEARBY(ptr, end) do { \
    const char *p = ptr; \
    int cnt = 0; \
    fprintf(stderr, "  around: "); \
    while (p < end && cnt < 32) { \
        unsigned char ch = (unsigned char)*p; \
        if (ch >= 32 && ch <= 126) fputc(ch, stderr); \
        else if (ch == '\r') fputs("\\r", stderr); \
        else if (ch == '\n') fputs("\\n", stderr); \
        else fprintf(stderr, "\\x%02X", ch); \
        p++; cnt++; \
    } \
    fputc('\n', stderr); \
} while (0)

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

//读取一行 返回行长度 包含\r\n 不够返回0 错误返回-1
static int peek_read_line(const char* buf, size_t len){
    for(size_t i = 0;i + 1 < len;i++){
        if(buf[i] == '\r' && buf[i + 1] == '\n'){
            return (int)(i + 2);
        }
    }
    return 0;//不够一行
}


// 从形如 ":123\r\n" / "$5\r\n" / "*10\r\n" 中解析整数（不带前缀符号）
static int peek_parse_integer(const char* buf, size_t len, long long* value, int* line_len){

    int l = peek_read_line(buf, len);
    if(l == 0) return 0;//数据不够
    if(l < 3) return -1;

    long long v = 0;
    int sign = 1;
    size_t i = 0;

    if (buf[0] == '-' || buf[0] == '+') {
        if (buf[0] == '-') sign = -1;
        i = 1;
    }
    if (i >= (size_t)(l - 2)) return -1; // 没有数字

    for (; i < (size_t)(l - 2); ++i) {
        if (buf[i] < '0' || buf[i] > '9') return -1;
        v = v * 10 + (buf[i] - '0');
        if (v > INT_MAX && sign > 0) return -1;   // 防溢出，可按需放宽
        if (v < INT_MIN && sign < 0) return -1;
    }

    *value = v * sign;
    *line_len = l;
    return 1;

}

// 无状态探测：从 buf[offset..len) 解析一个完整 RESP 命令的总长度
int resp_peek_command(const char* buf, size_t len, size_t offset){
    if (!buf || offset >= len) return 0;

    const char *p = buf + offset;
    size_t      remain = len - offset;

    // 1. 跳过前导空白（按你上层对齐规则，通常不会有，但这里更健壮）
    size_t skip = 0;
    while (skip < remain) {
        char c = p[skip];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            skip++;
        } else {
            break;
        }
    }
    if (skip >= remain) return 0; // 全是空白，数据还不够

    p      += skip;
    remain -= skip;

    // 2. 顶层必须是数组：*<N>\r\n
    if (remain < 1) return 0;
    if (*p != '*') {
        fprintf(stderr, "resp_peek_command: first non-space char not '*'\n");
        DUMP_NEARBY(p, buf + len);
        return -1;
    }
    p++;
    remain--;

    // 解析 array_len
    long long array_len = 0;
    int line_len = 0;
    int ret = peek_parse_integer(p, remain, &array_len, &line_len);
    if (ret <= 0) return ret;  // 0: 不够, <0: 错误
    if (array_len < 0 || array_len > MAX_ARRAY_LENGTH) {
        fprintf(stderr, "resp_peek_command: array_len out of range %lld\n", array_len);
        return -1;
    }

    p      += line_len;
    remain -= line_len;

    // 3. 依次解析 array_len 个 bulk string
    for (long long i = 0; i < array_len; ++i) {
        // 3.1 读 '$'
        if (remain < 1) return 0;
        if (*p != '$') {
            fprintf(stderr, "resp_peek_command: expected '$' for bulk len\n");
            DUMP_NEARBY(p, buf + len);
            return -1;
        }
        p++;
        remain--;

        // 3.2 读 bulk_len （数字行）
        long long bulk_len = 0;
        ret = peek_parse_integer(p, remain, &bulk_len, &line_len);
        if (ret <= 0) return ret;  // 0: 不够, <0: 错误

        if (bulk_len < -1 || bulk_len > MAX_BULK_LENGTH) {
            fprintf(stderr, "resp_peek_command: bulk_len out of range %lld\n", bulk_len);
            return -1;
        }

        p      += line_len;
        remain -= line_len;

        // 3.3 如果是 NULL bulk（$-1\r\n），这个元素到此结束
        if (bulk_len == -1) {
            continue;
        }

        // 3.4 需要 bulk_len 字节数据 + "\r\n"
        size_t need = (size_t)bulk_len + 2;
        if (remain < need) return 0; // body 或 CRLF 不完整

        if (p[bulk_len] != '\r' || p[bulk_len + 1] != '\n') {
            fprintf(stderr, "resp_peek_command: bulk missing CRLF\n");
            DUMP_NEARBY(p, buf + len);
            return -1;
        }

        p      += need;
        remain -= need;
    }

    // 走到这里，说明从 buf[offset] 到当前 p 之间正好是一条完整命令
    size_t total = (size_t)(p - (buf + offset));
    return (int)total;
}

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
    parser->bulk_read = 0;
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

int resp_parser(resp_parser_t *parser, const char *buffer, size_t length) {
    const char *ptr = buffer;
    const char *end = buffer + length;
    size_t parsed = 0;//只在返回前统一赋值

    // 如果上一次已经 COMPLETE，这里来的是新命令，先 reset
    if (parser->state == RESP_PARSE_COMPLETE) {
        resp_parser_reset(parser);
    }

    while (ptr < end) {
        switch (parser->state) {

        case RESP_PARSE_INIT: {
            if (end - ptr < 1) return 0;      // 等 '*'
            if (*ptr != '*') {
                fprintf(stderr, "INIT error: first char not '*'\n");
                DUMP_NEARBY(ptr, end);
                return -1;
            }
            ptr++; 
            // parsed++;
            parser->array_len = 0;
            parser->state = RESP_PARSE_ARRAY_LEN;
            break;
        }

        case RESP_PARSE_ARRAY_LEN: {
            long long tmp_len = 0;

            if (ptr >= end) return 0;//一个字节都没有

            const char* start = ptr;

            if (!isdigit((unsigned char)*ptr)) {
                fprintf(stderr, "ARRAY_LEN error: not digit\n");
                DUMP_NEARBY(ptr, end);
                return -1;
            }

            while (ptr < end && isdigit((unsigned char)*ptr)) {
                if (tmp_len > (INT_MAX - (*ptr - '0')) / 10) {
                    fprintf(stderr, "ARRAY_LEN error: overflow\n");
                    return -1;
                }
                tmp_len = tmp_len * 10 + (*ptr - '0');
                ptr++;
            }

            if (ptr >= end){
                //当前数字仅读到一半 已经消费了ptr - start 字节
                return (int)(ptr - buffer);
            }
            if (*ptr != '\r') return -1;

            if (ptr + 1 >= end){
                //只读到 '\r' 还没读到 '\n'
                return (int)(ptr - buffer);
            }

            if (*(ptr + 1) != '\n') return -1;

            ptr += 2;

            if (tmp_len < 0 || tmp_len > MAX_ARRAY_LENGTH) {
                fprintf(stderr, "ARRAY_LEN error: out of range %lld\n", tmp_len);
                return -1;
            }
            parser->array_len = (int)tmp_len;

            // parsed += local_parsed;

            // 分配 tokens
            if (parser->array_len > 0) {
                if (!parser->tokens || parser->array_len_allocated < parser->array_len) {
                    if (parser->tokens) kvs_free(parser->tokens);
                    parser->tokens = kvs_malloc(parser->array_len * sizeof(char *));
                    if (!parser->tokens) return -1;
                    memset(parser->tokens, 0, parser->array_len * sizeof(char *));
                    parser->array_len_allocated = parser->array_len;
                }
            }

            parser->arg_index = 0;
            parser->state = (parser->array_len > 0)
                 ? RESP_PARSE_BULK_LEN 
                 : RESP_PARSE_COMPLETE;
            if (parser->state == RESP_PARSE_COMPLETE){
                parsed = (size_t)(ptr - buffer);
                return (int)parsed;
            }
            break;
        }

        case RESP_PARSE_BULK_LEN: {
            if (end - ptr < 1) return 0;
            if (*ptr != '$') {
                fprintf(stderr, "BULK_LEN error: expected '$'\n");
                DUMP_NEARBY(ptr, end);
                return -1;
            }
            ptr++; 
            // parsed++;
            parser->state = RESP_PARSE_BULK_LEN_NUM;
            break;
        }

        case RESP_PARSE_BULK_LEN_NUM: {
            long long tmp_len = 0;
            int neg = 0;
            // size_t local_parsed = 0;

            if (ptr >= end) return 0;

            const char *debug_start = ptr;

            // 可选负号
            if (*ptr == '-') {
                neg = 1;
                ptr++;
                if (ptr >= end) {
                    //只读到了- 消费一个字节
                    return (int)(ptr - buffer);
                }
                if (!isdigit((unsigned char)*ptr)) {
                    fprintf(stderr, "BULK_LEN_NUM error: '-' not followed by digit\n");
                    DUMP_NEARBY(ptr - 1, end);
                    return -1;
                }
                // local_parsed++;   // '-'
            }

            if (!isdigit((unsigned char)*ptr)) {
                fprintf(stderr, "BULK_LEN_NUM error: not digit at start\n");
                DUMP_NEARBY(ptr, end);
                return -1;
            }

            // 读所有数字
            while (ptr < end && isdigit((unsigned char)*ptr)) {
                if (tmp_len > (INT_MAX - (*ptr - '0')) / 10) {
                    fprintf(stderr, "BULK_LEN_NUM error: overflow\n");
                    DUMP_NEARBY(debug_start, end);
                    return -1;
                }
                tmp_len = tmp_len * 10 + (*ptr - '0');
                ptr++; 
                // local_parsed++;
            }

            if (ptr >= end){
                //数字读到一本 等待更多数据
                return (int)(ptr - buffer);
            }
            if (*ptr != '\r') {
                fprintf(stderr, "BULK_LEN_NUM error: missing CR\n");
                DUMP_NEARBY(ptr, end);
                return -1;
            }
            if (ptr + 1 >= end){
                //读到了 '\r\ 还没有'\n'
                return (int)(ptr - buffer);
            }
            if (*(ptr + 1) != '\n') {
                fprintf(stderr, "BULK_LEN_NUM error: missing LF\n");
                DUMP_NEARBY(ptr, end);
                return -1;
            }

            ptr += 2;
            // local_parsed += 2;

            if (neg) tmp_len = -tmp_len;
            if (tmp_len < -1 || tmp_len > MAX_BULK_LENGTH) {
                fprintf(stderr, "BULK_LEN_NUM error: out of range %lld\n", tmp_len);
                DUMP_NEARBY(debug_start, end);
                return -1;
            }

            parser->bulk_len = (int)tmp_len;
            // parsed += local_parsed;

            if (parser->bulk_len == -1) {
                // NULL bulk
                if (parser->arg_index < parser->array_len) {
                    parser->tokens[parser->arg_index] = NULL;
                    parser->arg_index++;
                }
                parser->state = (parser->arg_index < parser->array_len)
                                ? RESP_PARSE_BULK_LEN
                                : RESP_PARSE_COMPLETE;
                if (parser->state == RESP_PARSE_COMPLETE){
                    parsed = (size_t)(ptr - buffer);
                    return (int)parsed;
                }
            } else {
                parser->state = RESP_PARSE_BULK_DATA;
            }
            break;
        }

        case RESP_PARSE_BULK_DATA: {
            if (parser->bulk_len < 0) {
                fprintf(stderr, "BULK_DATA error: bulk_len < 0, state=%d\n", parser->state);
                DUMP_NEARBY(ptr, end);
                return -1;
            }

            // size_t need  = (size_t)parser->bulk_len;   
            //剩余还需读取的body字节
            size_t need_body  = (size_t)parser->bulk_len  - (size_t)parser->bulk_read;   
            size_t avail = (size_t)(end - ptr);

            //第一次进入该bulk_data时 为整个body分配空间
            if(parser->arg_index < parser->array_len &&
               parser->bulk_len >= 0 &&
               parser->bulk_read == 0){

                char* p = (char* )kvs_malloc((size_t)parser->bulk_len + 1);
                if(!p){
                    fprintf(stderr, "BULK_DATA error: kvs_malloc failed, bulk_len=%d\n",
                            parser->bulk_len);
                    return -1;
                }
                parser->tokens[parser->arg_index] = p;
               }
            
            //先补齐body
            if(need_body > 0){
                if(avail == 0){
                    //当前buffer没有新字节 等待更多数据
                    return (int)(ptr - buffer);
                }

                size_t can_read = (avail < need_body) ? avail : need_body;

                if(parser->arg_index < parser->array_len){
                    char* p = parser->tokens[parser->arg_index];
                    memcpy(p + parser->bulk_read, ptr, can_read);
                }

                parser->bulk_read += (int)can_read;
                ptr               += can_read;
                avail             -= can_read;


                //body还未读满 不能读CRLF 直接返回
                if((size_t)parser->bulk_read < (size_t)parser->bulk_len){
                    return (int)(ptr - buffer);
                }
            }

            //走到这里 body已经全部读完 bulk_read == bulk_len

            //还需要检查 CRLF是否到齐
            if(avail < 2){
                //CRLF还未全部到达 等待更多数据
                return (int)(ptr - buffer);
            }

            if (ptr[0] != '\r' || ptr[1] != '\n') {
                fprintf(stderr, "BULK_DATA error: missing CRLF after body, state=%d\n",
                        parser->state);
                DUMP_NEARBY(ptr, end);
                return -1;
            }

            /* 吃掉 CRLF */
            ptr   += 2;
            avail -= 2;

            /* 补 '\0'，完成一个参数 */
            if (parser->arg_index < parser->array_len) {
                char *p = parser->tokens[parser->arg_index];
                p[parser->bulk_len] = '\0';
                parser->arg_index++;
            }

            /* 为下一个 bulk 做准备 */
            parser->bulk_read = 0;

            if (parser->arg_index < parser->array_len) {
                parser->state = RESP_PARSE_BULK_LEN;
            } else {
                parser->state = RESP_PARSE_COMPLETE;
                parsed = (size_t)(ptr - buffer);
                return (int)parsed;
            }

            break;

        }

        case RESP_PARSE_COMPLETE: {
            parsed = (size_t)(ptr - buffer);
            return (int)parsed;
        }
            
        default:
            return -1;
        }
    }

    parsed = (size_t)(ptr - buffer);
    return (int)parsed;
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
//简单字符串 +<msg>\r\n
int resp_generate_simple_string(char *response, const char *str){
    int mlen = (int)strlen(str);
    int total_len = mlen + 3; // + \r\n

    if(!response) return total_len;

    response[0] = '+';
    memcpy(response + 1, str, (size_t)mlen);
    memcpy(response + 1 + mlen, "\r\n", 2);
    return total_len;
}
//错误信息
int resp_generate_error(char *response, const char *err){
    if(!err) err = "";
    int mlen = (int)strlen(err);
    int total = mlen + 3; // - + \r\n

    if(!response) return total;

    response[0] = '-';
    memcpy(response + 1, err, (size_t)mlen);
    memcpy(response + 1 + mlen, "\r\n", 2);
    return total;
}
//整数类型
int resp_generate_integer(char *response, int num){
    char buf[64];
    int n = snprintf(buf, sizeof(buf), ":%d\r\n", num);
    if (n <= 0) return -1;
    if (!response) return n;
    memcpy(response, buf, (size_t)n);
    return n;
}

// 估算简单字符串/错误/整数/批量字符串的 RESP 长度（不实际写入）
size_t resp_estimate_simple_string_len(const char *str) {
    if (!str) str = "";
    return 1 + strlen(str) + 2; // "+" 或 "-" 或 ":" + 内容 + "\r\n"
}

size_t resp_estimate_error_len(const char *err) {
    if (!err) err = "";
    return 1 + strlen(err) + 2;
}

size_t resp_estimate_integer_len(long long num) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), ":%lld\r\n", num);
    return (n > 0) ? (size_t)n : 0;
}

size_t resp_estimate_bulk_string_len(const char *str) {
    if (!str) {
        return 5; // "$-1\r\n"
    }
    size_t slen = strlen(str);
    char lenbuf[32];
    int n = snprintf(lenbuf, sizeof(lenbuf), "%zu", slen);
    if (n <= 0) return 0;
    return 1 + (size_t)n + 2 + slen + 2; // "$" + len + "\r\n" + body + "\r\n"
}

//批量字符串 $<length>\r\n<string>\r\n
int resp_generate_bulk_string(char *response, const char *str){
    if(!str){
        //空值
        if(!response) return 5; // "$-1\r\n"
        return sprintf(response, "$-1\r\n");
    }

    int slen = (int)strlen(str);
    char header[64];
    int hlen = snprintf(header, sizeof(header), "$%d\r\n", slen);
    int total = hlen + slen + 2; // header + body + \r\n

    if(!response) return total;

    char *p = response;
    memcpy(p, header, (size_t)hlen); p += hlen;
    memcpy(p, str,   (size_t)slen);  p += slen;
    memcpy(p, "\r\n", 2);
    return total;

}
//空数组开头 *<count>\r\n
int resp_generate_array_start(char *response, int count){
    char header[64];
    int hlen = snprintf(header, sizeof(header), "*%d\r\n", count);
    if(!response) return hlen;

    memcpy(response, header, (size_t)hlen);
    return hlen;
}
//数组元素 复用批量字符串
int resp_generate_array_item(char *response, const char *str){
    return resp_generate_bulk_string(response, str);
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