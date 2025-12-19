#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * 抽象发送函数：
 *   ctx  : 用户上下文（socket fd / FILE* / 自定义结构）
 *   buf  : 连续内存
 *   len  : 字节数
 * return : 0 成功，<0 失败
 */
typedef int (*tx_send_fn)(void* ctx, const void* buf, size_t len);

/*
 * 通用批处理发送器
 *
 * 语义：
 *   - append() 只保证顺序，不解析内容
 *   - 内部 buffer 满或定时触发 flush
 *   - flush 一次 send 一大块
 */
struct tx_batcher {
    char     *buf;            // 发送缓冲区
    size_t    cap;            // 缓冲区容量
    size_t    len;            // 当前已使用

    uint64_t  flush_ns;       // 定时 flush 周期（ns，0 表示禁用）
    uint64_t  last_flush_ns;

    tx_send_fn send_fn;       // 实际发送函数
    void      *send_ctx;      // 发送上下文

    /* 统计信息（可选） */
    uint64_t  append_calls;
    uint64_t  append_bytes;
    uint64_t  flush_calls;
    uint64_t  flush_bytes;
    uint64_t  drops;
};

/* 初始化（buf 由调用者提供，避免 malloc） */
void tx_batcher_init(struct tx_batcher *t,
                     void *buf, size_t cap,
                     uint64_t flush_ns,
                     tx_send_fn send_fn,
                     void *send_ctx);

/* 重置状态（例如断线重连后） */
void tx_batcher_reset(struct tx_batcher *t);

/* 追加数据（严格保持顺序） */
int tx_batcher_append(struct tx_batcher *t,
                      const void *data,
                      size_t len);

/* 立即 flush */
int tx_batcher_flush(struct tx_batcher *t);

/* 定时 flush（通常在 poll loop 里调用） */
int tx_batcher_maybe_flush(struct tx_batcher *t);

/* 常用 socket sender（send + MSG_NOSIGNAL） */
int tx_send_socket(void *ctx, const void *buf, size_t len);

