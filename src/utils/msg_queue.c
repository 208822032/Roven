#include "msg_queue.h"
#include <stdlib.h>
#include <string.h>

#ifndef MSGQ_MALLOC
#   define MSGQ_MALLOC malloc
#   define MSGQ_FREE   free
#endif

// 向上取整到 2 的幂
static size_t next_power_of_two(size_t n) {
    if (n < 2) return 2;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if ULONG_MAX > 0xffffffff
    n |= n >> 32;
#endif
    n++;
    return n;
}

int msg_queue_init(msg_queue_t *q, int capacity) {
    if (!q || capacity <= 0) return -1;

    memset(q, 0, sizeof(*q));

    size_t cap = next_power_of_two((size_t)capacity);
    void** buf = (void **)MSGQ_MALLOC(sizeof(void *) * cap);
    if(!buf) return -1;

    q->buffer     = buf;
    q->capacity   = cap;
    q->capacity_mask = cap - 1;

    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);

    //初始化缓冲区为NULL
    for(size_t i = 0;i < cap;i++){
        q->buffer[i] = NULL;
    }

    return 0;
}


void msg_queue_destroy(msg_queue_t *q) {
    if (!q) return;

    if(q->buffer){
        MSGQ_FREE(q->buffer);
        q->buffer = NULL;
    }
    q->capacity = 0;
    q->capacity_mask = 0;
}


/*
 * 单生产者 push：
 * - 读取当前 head/tail
 * - 判断是否满：tail - head == capacity
 * - 直接写入 buffer[tail & mask]，然后发布新的 tail
 *
 * 因为只有一个生产者，所以不需要 CAS 保护 tail，
 * 只要在发布新 tail 前写好元素即可。
 */
int msg_queue_push(msg_queue_t *q, void *item) {
    if(!q) return -1;

    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);

    if(tail - head == q->capacity){
        //队列已满
        return -1;
    }

    size_t index = tail & q->capacity_mask;
    q->buffer[index] = item;

    //发布新的tail
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 0;
}

int msg_queue_pop(msg_queue_t *q, void **item) {
    if(!q || !item) return -1;

    for(;;){
        size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

        if(head >= tail) return -1; //队列为空

        //尝试写head
        size_t new_head = head + 1;
        if(atomic_compare_exchange_weak_explicit(
            &q->head,
            &head,  //期望值
            new_head, //目标值
            memory_order_acq_rel,
            memory_order_acquire)){
            
            //原子操作成功，取出元素
            size_t idx = head & q->capacity_mask;

            //单生产者，不需要担心并发修改
            void* val = q->buffer[idx];
            q->buffer[idx] = NULL; //清空位置 非必须

            *item = val;
            return 0;
        }
    }
}