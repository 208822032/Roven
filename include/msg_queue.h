#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>

/**
 * 阻塞可扩容队列（多生产者、多消费者）
 *
 * - push() / pop() 内部使用 mutex + cond 进行同步
 * - 当容量不足时自动扩容（2 倍）
 * - 存储的是 void*，适合大 key/value（只存指针，不拷贝对象）
 */

typedef struct msg_queue {
    void          **buffer;   // 环形缓冲区（存放指针）
    size_t          capacity; // 当前容量

    //使用原子变量
    _Atomic size_t   head;     // 读索引
    _Atomic size_t   tail;     // 写索引

    size_t          count;    // 当前元素个数

    size_t          capacity_mask; // capacity - 1，用于快速取模
} msg_queue_t;

int msg_queue_init(msg_queue_t *q, int capacity);  // capacity 会向上取 2 的幂

//释放内部缓冲区
void msg_queue_destroy(msg_queue_t *q);

/**
 * 单生产者 push
 * 成功返回 0，失败返回 -1（队列已满）
 */
int msg_queue_push(msg_queue_t *q, void *item);

/**
 * 多消费者pop
 * 成功返回 0，在item中返回元素
 * 失败返回 -1
 */
int msg_queue_pop(msg_queue_t *q, void **item);

#endif // MSG_QUEUE_H