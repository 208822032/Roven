#ifndef MASTER_SLAVE_PROTO_H
#define MASTER_SLAVE_PROTO_H

#include <stddef.h>
#include <stdint.h>

// 操作类型（可根据你的存储结构扩展）   
typedef enum {
    MS_OP_ARRAY_SET    = 1,
    MS_OP_ARRAY_DEL    = 2,
    MS_OP_ARRAY_MOD    = 3,
    MS_OP_RBTREE_RSET   = 4,
    MS_OP_RBTREE_RDEL   = 5,
    MS_OP_RBTREE_RMOD   = 6,
    MS_OP_HASH_HSET    = 7,
    MS_OP_HASH_HDEL    = 8,
    MS_OP_HASH_HMOD    = 9,
    MS_OP_SKIPTABLE_SSET = 10,
    MS_OP_SKIPTABLE_SDEL = 11,
    MS_OP_SKIPTABLE_SMOD = 12,
    MS_OP_PAPERSET     = 13,
    MS_OP_PAPERDEL     = 14,
    MS_OP_PAPERMOD     = 15,
} ms_op_t;

// 供分布式模块按需计算最小缓冲区大小
#define MS_HEADER_LEN  (1 + 1 + 1 + 4 + 4 + 1)

/**
 * 编码一条主从复制消息到 buffer
 *
 * buf      : 输出缓冲区
 * buf_size : 缓冲区大小
 * op       : 操作类型
 * key      : key 字节串（可以包含 '\0'，由 key_len 决定长度）
 * key_len  : key 长度
 * val      : value 字节串（可以为 NULL，表示无 value）
 * val_len  : value 长度（val 为空时必须为 0）
 *
 * 返回值：>0 为写入的总字节数；<=0 表示失败（buffer 不够等）
 */
int ms_encode_message(
    uint8_t       *buf,
    size_t         buf_size,
    ms_op_t        op,
    const char    *key,
    size_t         key_len,
    const char    *val,
    size_t         val_len
);

/**
 * 从流缓冲区解析一条完整消息（适合 TCP 流解析）
 *
 * in_buf/in_len : 当前已接收到的字节
 * out_op        : 输出操作类型
 * out_key       : 输出 key 指针（指向 in_buf 内部，无拷贝）
 * out_key_len   : 输出 key 长度
 * out_val       : 输出 value 指针（指向 in_buf 内部，无拷贝）
 * out_val_len   : 输出 value 长度
 *
 * 返回值：
 *   >0 : 消费的字节数（这条完整消息的长度），调用方应从 buffer 中移除这些字节
 *   =0 : 数据不够一条完整消息，需要继续接收
 *   <0 : 解析错误，调用方可丢弃 buffer 或重新对齐
 */
int ms_decode_message(
    const uint8_t *in_buf,
    size_t         in_len,
    ms_op_t       *out_op,
    const uint8_t **out_key,
    size_t        *out_key_len,
    const uint8_t **out_val,
    size_t        *out_val_len
);

// 将一条写操作编码成 ms 二进制流。
// 返回写入长度 >0 成功，<=0 失败。
int ms_encode_command(
    uint8_t *buf, size_t buf_cap,
    ms_op_t op,
    const char *key, size_t key_len,
    const char *val, size_t val_len
);


// 尝试从 buffer 中解出一条完整命令
// 返回：>0 表示消费的字节数；0 表示数据不够；<0 表示协议错误。
int ms_decode_command(
    const uint8_t *buf, size_t len,
    ms_op_t *op,
    const uint8_t **key, size_t *key_len,
    const uint8_t **val, size_t *val_len
);


#endif // MASTER_SLAVE_PROTO_H