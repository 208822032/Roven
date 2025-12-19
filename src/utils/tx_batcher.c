#include "tx_batcher.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>

static inline uint64_t tx_nsec_now(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 常用 socket sender（send + MSG_NOSIGNAL） */
int tx_send_socket(void *ctx, const void *buf, size_t len){
    int fd = *(int *)ctx;
    const char* p = (const char* )buf;
    size_t left = len;

    while(left > 0){
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if(n < 0){
            if(errno == EINTR) continue;
            return -1;
        }
        if(n == 0) return -1;
        p += (size_t)n;
        left -= (size_t)n;
    }
    return 0;
}

/* 初始化（buf 由调用者提供，避免 malloc） */
void tx_batcher_init(struct tx_batcher *t,
                     void *buf, size_t cap,
                     uint64_t flush_ns,
                     tx_send_fn send_fn,
                     void *send_ctx)
{
    t->buf = (char *)buf;
    t->cap = cap;
    t->len = 0;

    t->flush_ns = flush_ns;
    t->last_flush_ns = tx_nsec_now();

    t->send_fn = send_fn;
    t->send_ctx = send_ctx;

    t->append_calls = 0;
    t->append_bytes = 0;
    t->flush_calls  = 0;
    t->flush_bytes  = 0;
    t->drops        = 0;
}                   

/* 重置状态（例如断线重连后） */
void tx_batcher_reset(struct tx_batcher *t){
    t->len = 0;
    t->last_flush_ns = tx_nsec_now();
}

/* 追加数据（严格保持顺序） */
int tx_batcher_append(struct tx_batcher *t,
                      const void *data,
                      size_t len)
{
    const char *p = (const char *)data;

    t->append_calls++;
    t->append_bytes += len;

    while (len > 0) {
        size_t space = t->cap - t->len;
        if (space == 0) {
            if (tx_batcher_flush(t) < 0)
                return -1;
            space = t->cap;
        }

        size_t take = (len < space) ? len : space;
        memcpy(t->buf + t->len, p, take);

        t->len += take;
        p += take;
        len -= take;

        if (t->len == t->cap) {
            if (tx_batcher_flush(t) < 0)
                return -1;
        }
    }
    return 0;
}                      

/* 立即 flush */
int tx_batcher_flush(struct tx_batcher *t){
    if(t->len == 0) return 0;

    if(t->send_fn(t->send_ctx, t->buf, t->len) < 0) return -1;

    t->flush_calls++;
    t->flush_bytes += t->len;
    t->len = 0;
    t->last_flush_ns = tx_nsec_now();
    return 0;
}

/* 定时 flush（通常在 poll loop 里调用） */
int tx_batcher_maybe_flush(struct tx_batcher *t){
    if (t->flush_ns == 0 || t->len == 0)
        return 0;

    uint64_t now = tx_nsec_now();
    if (now - t->last_flush_ns >= t->flush_ns)
        return tx_batcher_flush(t);

    return 0;
}

