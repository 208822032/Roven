#include "master_slave_proto.h"
#include <string.h>
#include <arpa/inet.h>

#define MS_MAGIC       0xAB
#define MS_VERSION     0x01
#define MS_HEADER_LEN  (1 + 1 + 1 + 4 + 4 + 1)

/*
 * 头部布局（共 12 字节）：
 *  0: magic    (1 byte)
 *  1: version  (1 byte)
 *  2: op       (1 byte)
 *  3-6: key_len (uint32, network-order)
 *  7-10: val_len (uint32, network-order)
 * 11: reserved (1 byte)
 */

int ms_encode_message(
    uint8_t       *buf,
    size_t         buf_size,
    ms_op_t        op,
    const char    *key,
    size_t         key_len,
    const char    *val,
    size_t         val_len
) {
    if (!buf || !key) return -1;

    size_t need = MS_HEADER_LEN + key_len + val_len;
    if (need > buf_size) return -1;

    buf[0] = MS_MAGIC;
    buf[1] = MS_VERSION;
    buf[2] = (uint8_t)op;

    uint32_t klen_n = htonl((uint32_t)key_len);
    uint32_t vlen_n = htonl((uint32_t)val_len);
    memcpy(buf + 3, &klen_n, 4);
    memcpy(buf + 7, &vlen_n, 4);
    buf[11] = 0; // reserved

    memcpy(buf + MS_HEADER_LEN, key, key_len);
    if (val && val_len > 0) {
        memcpy(buf + MS_HEADER_LEN + key_len, val, val_len);
    }

    return (int)need;
}

int ms_decode_message(
    const uint8_t *in_buf,
    size_t         in_len,
    ms_op_t       *out_op,
    const uint8_t **out_key,
    size_t        *out_key_len,
    const uint8_t **out_val,
    size_t        *out_val_len
){
    if (!in_buf || in_len < MS_HEADER_LEN) {
        return 0; // 数据不够头部
    }

    if (in_buf[0] != MS_MAGIC) {
        // magic 不匹配，认为是解析错误
        return -1;
    }

    uint8_t ver = in_buf[1];
    if (ver != MS_VERSION) {
        return -1;
    }

    uint8_t op = in_buf[2];

    uint32_t klen_n, vlen_n;
    memcpy(&klen_n, in_buf + 3, 4);
    memcpy(&vlen_n, in_buf + 7, 4);
    uint32_t klen = ntohl(klen_n);
    uint32_t vlen = ntohl(vlen_n);

    size_t need = MS_HEADER_LEN + (size_t)klen + (size_t)vlen;
    if (in_len < need) {
        return 0; // 还不够一条完整消息
    }

    if (out_op)      *out_op      = (ms_op_t)op;
    if (out_key)     *out_key     = in_buf + MS_HEADER_LEN;
    if (out_key_len) *out_key_len = klen;
    if (out_val)     *out_val     = in_buf + MS_HEADER_LEN + klen;
    if (out_val_len) *out_val_len = vlen;

    return (int)need;
}


// 将一条写操作编码成 ms 二进制流。
// 返回写入长度 >0 成功，<=0 失败。
int ms_encode_command(
    uint8_t *buf, size_t buf_cap,
    ms_op_t op,
    const char *key, size_t key_len,
    const char *val, size_t val_len
){
    if (!buf || !key) return -1;
    return ms_encode_message(buf, buf_cap, op, key, key_len, val, val_len);
}

// 尝试从 buffer 中解出一条完整命令
// 返回：>0 表示消费的字节数；0 表示数据不够；<0 表示协议错误。
int ms_decode_command(
    const uint8_t *buf, size_t len,
    ms_op_t *op,
    const uint8_t **key, size_t *key_len,
    const uint8_t **val, size_t *val_len
){

    return ms_decode_message(buf, len, op, key, key_len, val, val_len);
}
