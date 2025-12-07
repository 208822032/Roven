#include "netbuf.h"
#include "kvs_api.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>

//初始化缓冲区 分配 initial_capacity 字节
int netbuf_init(netbuf_t* buf, size_t initial_capacity){
    if(!buf) return -1;

    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;

    if(initial_capacity == 0){
        return 0;//允许延迟分配
    }

    if(initial_capacity > NETBUF_MAX_CAPACITY){
        fprintf(stderr, "netbuf_init: initial_capacity %zu exceeds NETBUF_MAX_CAPACITY %zu\n", initial_capacity, (size_t)NETBUF_MAX_CAPACITY);
        return -1;
    }

    buf->data = (char* )kvs_malloc(initial_capacity);
    if(!buf->data) return -1;

    buf->capacity = initial_capacity;
    return 0;
}

//确保容量至少为 min_capacity 按照2倍扩展
//若容量超过 NETBUF_MAX_CAPACITY 则返回错误
int netbuf_reserve(netbuf_t* buf, size_t min_capacity){
    if(!buf) return -1;

    if(min_capacity <= buf->capacity) return 0; //已满足

    size_t new_capacity = buf->capacity ? buf->capacity : 64 * 1024; // 64KB 起步
    while(new_capacity < min_capacity){
    //防止溢出
        if(new_capacity > (SIZE_MAX / 2)){
            new_capacity = SIZE_MAX;
            break;
        }
        new_capacity *= 2;
    }

    if(new_capacity > NETBUF_MAX_CAPACITY){
        fprintf(stderr, "netbuf_reserve: requested capacity %zu exceeds NETBUF_MAX_CAPACITY %zu\n", new_capacity, (size_t)NETBUF_MAX_CAPACITY);
        return -1;
    }

    char* new_data = (char* )kvs_realloc(buf->data, new_capacity);
    if(!new_data) return -1;

    buf->data = new_data;
    buf->capacity = new_capacity;
    return 0;
}

//从前面裁掉nbytes 用以解决已解析掉了的数据
void netbuf_consume_front(netbuf_t* buf, size_t nbytes){
    if(!buf || nbytes == 0) return;

    if(nbytes >= buf->size){
        //全部裁掉
        buf->size = 0;
        return;
    }

    size_t remaining = buf->size - nbytes;
    memmove(buf->data, buf->data + nbytes, remaining);
    buf->size = remaining;
}

//释放缓冲区
void netbuf_free(netbuf_t* buf){
    if(!buf) return;

    if(buf->data){
        kvs_free(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
    buf->capacity = 0;
}