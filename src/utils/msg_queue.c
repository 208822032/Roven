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

// 内部扩容：在已加锁情况下调用
static int msg_queue_grow_locked(msg_queue_t *q) {
    size_t new_cap = q->capacity * 2;
    void **new_buf = (void **)MSGQ_MALLOC(sizeof(void *) * new_cap);
    if (!new_buf) {
        return -1;
    }

    // 将原有数据按顺序拷贝到新缓冲区
    // 当前队列中元素从 head 开始，共 count 个
    for (size_t i = 0; i < q->count; ++i) {
        size_t idx = (q->head + i) % q->capacity;
        new_buf[i] = q->buffer[idx];
    }

    MSGQ_FREE(q->buffer);
    q->buffer  = new_buf;
    q->capacity = new_cap;
    q->head    = 0;
    q->tail    = q->count; // [0, count) 已占用

    return 0;
}

int msg_queue_init(msg_queue_t *q, int capacity) {
    if (!q || capacity <= 0) return -1;

    memset(q, 0, sizeof(*q));

    size_t cap = next_power_of_two((size_t)capacity);
    q->buffer = (void **)MSGQ_MALLOC(sizeof(void *) * cap);
    if (!q->buffer) {
        return -1;
    }

    q->capacity = cap;
    q->head     = 0;
    q->tail     = 0;
    q->count    = 0;
    q->closed   = 0;

    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return 0;
}

void msg_queue_close(msg_queue_t *q) {
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    q->closed = 1;
    // 唤醒所有等待的生产者和消费者
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}

void msg_queue_destroy(msg_queue_t *q) {
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    if (q->buffer) {
        MSGQ_FREE(q->buffer);
        q->buffer = NULL;
    }
    q->capacity = q->head = q->tail = q->count = 0;
    pthread_mutex_unlock(&q->mutex);

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);

    memset(q, 0, sizeof(*q));
}

int msg_queue_push(msg_queue_t *q, void *item) {
    if (!q) return -1;

    pthread_mutex_lock(&q->mutex);

    // 如果已经关闭，不再接受新元素
    if (q->closed) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    // 满了就扩容（而不是阻塞等待空间）
    if (q->count == q->capacity) {
        if (msg_queue_grow_locked(q) != 0) {
            // 扩容失败则阻塞等待有空位（保底策略）
            while (!q->closed && q->count == q->capacity) {
                pthread_cond_wait(&q->not_full, &q->mutex);
            }
            if (q->closed) {
                pthread_mutex_unlock(&q->mutex);
                return -1;
            }
        }
    }

    // 写入元素
    q->buffer[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    // 唤醒一个等待的消费者
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int msg_queue_pop(msg_queue_t *q, void **item) {
    if (!q || !item) return -1;

    pthread_mutex_lock(&q->mutex);

    // 队列为空时阻塞等待
    while (q->count == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    // 如果关闭且为空，返回 -1，提示可以退出
    if (q->count == 0 && q->closed) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    // 取出元素
    *item = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    // 唤醒一个等待的生产者（如果有）
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}