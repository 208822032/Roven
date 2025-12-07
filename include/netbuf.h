//网络缓冲区

#ifndef NETBUF_H
#define NETBUF_H

#include <stddef.h>


typedef struct {
    char* data;
    size_t size; //已用字节
    size_t capacity; //总容量
} netbuf_t;

//容量上限 防止被单连接打爆内存
#ifndef NETBUF_MAX_CAPACITY
#define NETBUF_MAX_CAPACITY (1024 * 1024 * 64) //10
#endif

//初始化缓冲区 分配 initial_capacity 字节
int netbuf_init(netbuf_t* buf, size_t initial_capacity);

//确保容量至少为 min_capacity 按照2倍扩展
//若容量超过 NETBUF_MAX_CAPACITY 则返回错误
int netbuf_reserve(netbuf_t* buf, size_t min_capacity);

//从前面裁掉nbytes 用以解决已解析掉了的数据
void netbuf_consume_front(netbuf_t* buf, size_t nbytes);

//释放缓冲区
void netbuf_free(netbuf_t* buf);


#endif // NETBUF_H