
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
    parser->state = RESP_PARSE_INIT;
    parser->array_len = 0;
    parser->bulk_len = 0;
    parser->arg_count = 0;
    parser->arg_index = 0;
    parser->tokens = NULL;
}

//解析RESP协议
//返回 1解析完成 0需要更多数据 -1表示错误
int resp_parser(resp_parser_t *parser, const char *buffer, size_t length){

    const char *ptr = buffer;
    const char *end = buffer + length;

    while(ptr < end){
        switch(parser->state){
            case RESP_PARSE_INIT:
                if (*ptr == '*'){
                    parser->state = RESP_PARSE_ARRAY_LEN;
                    parser->array_len = 0;
                    ptr++;
                }else{
                    //不支持非数组格式
                    return -1;
                }
            break;

            case RESP_PARSE_ARRAY_LEN:
            if(*ptr >= '0' && *ptr <= '9'){
                parser->array_len = parser->array_len * 10 + (*ptr - '0');
                ptr++;
            }else if(*ptr == '\r'){
                if(ptr + 1 >= end){
                    return 0;//需要更多数据
                }
                if(*(ptr+1) != '\n'){
                    return -1;
                }
                parser->state = RESP_PARSE_BULK_LEN;
                parser->tokens = (char **)kvs_malloc(parser->array_len * sizeof(char *));
                if(!parser->tokens){
                    return -1;
                }
                parser->arg_index = 0;
                ptr += 2;//跳过 \r\n
            }else{
                return -1;
            }
            break;

            case RESP_PARSE_BULK_LEN:
                if(*ptr == '$'){
                    ptr++;
                    parser->bulk_len = 0;
                    while(ptr < end && *ptr >= '0' && *ptr <= '9'){
                        parser->bulk_len = parser->bulk_len * 10 + (*ptr - '0');
                        ptr++;
                    }
                    if(ptr >= end) return 0;
                    if(*ptr == '\r'){
                        if(ptr+1 >= end) return 0;//需要更多数据
                        if(*(ptr+1) != '\n') return -1;//数据错误
                        ptr += 2;
                        parser->state = RESP_PARSE_BULK_DATA;
                    }else {
                        return -1;
                    }
                }else{
                    return -1;
                }
            break;

            case RESP_PARSE_BULK_DATA:
                if((end - ptr) < (parser->bulk_len + 2)) return 0;// +2 是\r\n 需要更多数据
                //分配空间并复制数据
                parser->tokens[parser->arg_index] = kvs_malloc(parser->bulk_len + 1);
                if(!parser->tokens[parser->arg_index]) return -1;
                memcpy(parser->tokens[parser->arg_index], ptr, parser->bulk_len);
                parser->tokens[parser->arg_index][parser->bulk_len] = '\0';

                parser->arg_index++;
                ptr += parser->bulk_len;
                //检查结束符
                if(*ptr == '\r' && *(ptr+1) == '\n'){
                    ptr += 2;
                }else{
                    return -1;
                }
                parser->state = RESP_PARSE_BULK_LEN;

                //解析完所有数据
                if(parser->arg_index >= parser->array_len) return (ptr - buffer); //返回已解析的字节数
            break;
        }

    }

    return 0;//需要更多数据

}

//释放解释器分配的内存
void resp_parser_free(resp_parser_t *parser){
    if(parser->tokens){
        int i;
        for(i = 0;i < parser->arg_index; i++){
            if(parser->tokens[i]){
                kvs_free(parser->tokens[i]);
            }
        }
        kvs_free(parser->tokens);
        parser->tokens = NULL;
    }
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