#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include <stddef.h>
#include <pthread.h>

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
    size_t          head;     // 读索引
    size_t          tail;     // 写索引
    size_t          count;    // 当前元素个数

    int             closed;   // 1=关闭，不再接受新元素

    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} msg_queue_t;

int msg_queue_init(msg_queue_t *q, int capacity);  // capacity 会向上取 2 的幂
void msg_queue_close(msg_queue_t *q);
void msg_queue_destroy(msg_queue_t *q);

/**
 * 多生产者 push（阻塞）
 * 队列未关闭：必要时等待空间并自动扩容，0=成功
 * 队列已关闭：返回 -1
 */
int msg_queue_push(msg_queue_t *q, void *item);

/**
 * 多消费者 pop（阻塞）
 * 队列未关闭或还有数据：阻塞直到取到一个元素，0=成功（*item != NULL）
 * 队列已关闭且为空：返回 -1（可用于线程退出）
 */
int msg_queue_pop(msg_queue_t *q, void **item);

#endif // MSG_QUEUE_H